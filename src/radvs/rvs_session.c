// This file is included only by rvs_engine.c after private engine types.

#include "radvs/rvs_session.h"
#include "radvs/rvs_demon.h"
#include "radvs/rvs_request.h"

typedef struct
{
  RVS_QueueNode base;
  RVS_Event     event;
} RVS_SessionEventMessage;

internal void rvs_session_event_message_copy(Arena *arena, void *dst, void *src);

internal B32
rvs_session_operation_key_resolves_locked(RVS_Session *session, RVS_SchedulerKey key)
{
  if (key.op != RVS_SchedulerOp_ReadOnly && key.op != RVS_SchedulerOp_TargetConfiguration) {
    return 1;
  }

  RVS_TargetControl *program = rvs_scheduler_target_from_id_locked(&session->scheduler, key.target);
  return program != 0 && program->state != RVS_TargetState_Removed;
}

internal RVS_Result
rvs_session_push_event(RVS_Session *session, RVS_Event *event)
{
  ProfBeginFunction();
  rvs_control_assert_unlocked();
  RVS_SessionEventMessage spec = { .event = *event };
  RVS_Result result = rvs_queue_push_copy(session->event_queue, &spec, rvs_session_event_message_copy);
  ProfEnd();
  return result;
}

internal void
rvs_session_event_message_copy(Arena *arena, void *dst_ptr, void *src_ptr)
{
  RVS_SessionEventMessage *dst = dst_ptr;
  RVS_SessionEventMessage *src = src_ptr;
  RVS_QueueNode base = dst->base;
  *dst = *src;
  dst->base = base;
  (void)arena;
}

internal void
rvs_session_close_events(RVS_Session *session)
{
  rvs_queue_close(session->event_queue);
  rvs_entity_store_close(&session->entities);
}

internal RVS_Session *
rvs_session_alloc(RVS_Engine *engine)
{
  Arena *arena = arena_alloc(.name = "Debug Engine Session");
  RVS_Session *session = push_array(arena, RVS_Session, 1);
  session->arena                   = arena;
  session->engine                  = engine;
  session->control                 = engine->control;
  session->scheduler.arena         = arena;
  session->scheduler.entities      = &session->entities;
  session->scheduler.recycle_mutex = mutex_alloc();
  session->ref_count               = 2; // engine ownership plus the returned handle
  session->event_queue             = rvs_queue_alloc(sizeof(RVS_SessionEventMessage), AlignOf(RVS_SessionEventMessage));
  rvs_engine_control_addref(session->control);
  rvs_entity_store_init(&session->entities, arena, session->control->mutex);
  return session;
}

internal void
rvs_session_release_ref(RVS_Session *session)
{
  if (ins_atomic_u32_dec_eval(&session->ref_count) == 0) {
    AssertAlways(session->engine_released);
    rvs_queue_release(session->event_queue);
    rvs_entity_store_release(&session->entities);
    mutex_release(session->scheduler.recycle_mutex);
    rvs_engine_control_release(session->control);
    arena_release(session->arena);
  }
}

internal void
rvs_session_release_engine(RVS_Session *session)
{
  // The engine holds control->mutex while releasing session ownership.
  AssertAlways(session->scheduler.stop_transaction.owner == 0);
  for EachNode(program, RVS_TargetControl, session->scheduler.target_first) {
    AssertAlways(program->state == RVS_TargetExecutionState_Idle || program->state == RVS_TargetState_Removed);
  }
  session->engine = 0;
  session->engine_released = 1;
  rvs_session_release_ref(session); // drop engine ownership
}

internal RVS_Result
rvs_session_submit(RVS_Session *session, RVS_EngineCommand command, RVS_SubmitInfo *submit_out)
{
  ProfBeginFunction();

  RVS_Result         result = RVS_Result_Error;
  RVS_SchedulerOp    op = RVS_SchedulerOp_Null;
  RVS_SchedulerKey   key = {0};

  AssertAlways(session != 0);
  AssertAlways(submit_out != 0);

  MemoryZeroStruct(submit_out);

  rvs_engine_command_scheduler_op(command, &op);
  key = (RVS_SchedulerKey){ .op = op, .identity = command.kind };
  AssertAlways(rvs_scheduler_key_is_well_formed(key));

  rvs_control_mutex_take(session->control);

  if (session->control->is_shutdown ||
      session->engine_released      ||
      session->engine == 0          ||
      ins_atomic_u32_eval(&session->engine->state) != RVS_ThreadState_Live) {
    result = RVS_Result_EngineStopped;
    goto exit_control_mutex;
  }

  if (rvs_scheduler_execution_blocks_operation_locked(&session->scheduler, op)) {
    result = RVS_Result_AlreadyPending;
    goto exit_control_mutex;
  }

  RVS_Engine *engine = session->engine;

  if ( ! rvs_session_operation_key_resolves_locked(session, key)) {
    goto exit_control_mutex;
  }
  if (command.kind == RVS_EngineCommandKind_Run || command.kind == RVS_EngineCommandKind_Interrupt) {
    RVS_ProgramID *programs = command.kind == RVS_EngineCommandKind_Run ? command.run.programs : command.interrupt.programs;
    U64 programs_count = command.kind == RVS_EngineCommandKind_Run ? command.run.programs_count : command.interrupt.programs_count;
    for EachIndex(program_idx, programs_count) {
      RVS_TargetControl *program = rvs_scheduler_target_from_id_locked(&session->scheduler, programs[program_idx]);
      if (program == 0) {
        goto exit_control_mutex;
      }
      if (program->state == RVS_TargetState_Removed) {
        result = RVS_Result_StaleState;
        goto exit_control_mutex;
      }
    }
  }
  U64 captured_program_state_epoch = 0;
  if (op == RVS_SchedulerOp_ReadOnly) {
    RVS_TargetControl *program = rvs_scheduler_target_from_id_locked(&session->scheduler, key.target);
    if (program) { captured_program_state_epoch = program->revision; }
  }
  RVS_ProgramID *targets = 0;
  U64 targets_count = 0;
  if (command.kind == RVS_EngineCommandKind_Run || command.kind == RVS_EngineCommandKind_Interrupt) {
    targets = command.kind == RVS_EngineCommandKind_Run ? command.run.programs : command.interrupt.programs;
    targets_count = command.kind == RVS_EngineCommandKind_Run ? command.run.programs_count : command.interrupt.programs_count;
  }
  RVS_SchedulerAdmission admission = {0};
  result = rvs_scheduler_admit_locked(&session->scheduler, engine->request_pool, &engine->next_request_id,
                                      key, targets, targets_count, captured_program_state_epoch, &admission);
  if (result != RVS_Result_Ok) {
    goto exit_control_mutex;
  }
  if (admission.kind != RVS_SchedulerAdmissionKind_New) {
    submit_out->request = admission.request;
    result = RVS_Result_Ok;
    goto exit_control_mutex;
  }
  RVS_ScheduledOperation *operation = admission.operation;
  if (command.kind == RVS_EngineCommandKind_Run) {
    operation->run_intent = command.run.intent;
    if (command.run.mode == RVS_RunMode_ToAddress) {
      rvs_scheduler_attach_run_to_address_locked(&session->scheduler, operation, command.run.programs[0], command.run.address);
    }
  }

  RVS_EngineMessage message = {
    .type    = RVS_EngineMessageType_DispatchRequest,
    .command = command,
  };
  message.request_id = operation->request->request_id;

  result = rvs_engine_send_message_locked(engine, &message);

  if (result == RVS_Result_Ok) {
    submit_out->request = operation->request;
    submit_out->control = rvs_request_control_alloc(session, operation, operation->is_keyed);
  } else {
    rvs_scheduler_rollback_admission_locked(&session->scheduler, &admission);
  }
  exit_control_mutex:;
  rvs_control_mutex_drop(session->control);

  exit:;
  ProfEnd();
  return result;
}

RVS_Result
rvs_session_launch(RVS_Session *session, String8 cmdl, String8 wdir, RVS_SubmitInfo *submit_out)
{
  ProfBeginFunction();
  RVS_Result result = RVS_Result_Error;

  // @API_ARG_CHECK
  if (submit_out) { MemoryZeroStruct(submit_out); }
  if (session == 0 || submit_out == 0) {
    result = RVS_Result_InvalidArgument;
    goto exit;
  }

  Temp scratch = scratch_begin(0, 0);

  RVS_EngineCommand command = {
    .kind   = RVS_EngineCommandKind_Launch,
    .launch = {
      .params = {
        .cmd_line = str8_split_by_string_chars(scratch.arena, cmdl, str8_lit(" "), 0),
        .path     = wdir,
      }
    }
  };

  result = rvs_session_submit(session, command, submit_out);

  scratch_end(scratch);
  exit:;
  ProfEnd();
  return result;
}

RVS_Result
rvs_session_run_many(RVS_Session *session, RVS_ProgramID *programs, U64 programs_count, RVS_SubmitInfo *submit_out)
{
  ProfBeginFunction();
  RVS_Result result = RVS_Result_Error;

  // @API_ARG_CHECK
  if (submit_out) { MemoryZeroStruct(submit_out); }
  if (session == 0 || programs == 0 || programs_count == 0 || submit_out == 0)  {
    result = RVS_Result_InvalidArgument;
    goto exit;
  }
  for EachIndex(program_idx, programs_count) {
    if (rvs_program_id_is_zero(programs[program_idx])) {
      result = RVS_Result_InvalidArgument;
      goto exit;
    }
    for EachIndex(previous_idx, program_idx) {
      if (rvs_program_id_match(programs[previous_idx], programs[program_idx])) {
        result = RVS_Result_InvalidArgument;
        goto exit;
      }
    }
  }

  RVS_EngineCommand command = {
    .kind = RVS_EngineCommandKind_Run,
    .run  = {
      .programs_count = programs_count,
      .programs       = programs,
      .mode           = RVS_RunMode_Normal,
      .intent         = { .kind = RVS_RunIntentKind_Execute },
    },
  };

  result = rvs_session_submit(session, command, submit_out);

  exit:;
  ProfEnd();
  return result;
}

RVS_Result
rvs_session_run(RVS_Session *session, RVS_ProgramID program_id, RVS_SubmitInfo *submit_out)
{
  if (submit_out) { MemoryZeroStruct(submit_out); }
  if (session == 0 || rvs_program_id_is_zero(program_id) || submit_out == 0) {
    return RVS_Result_InvalidArgument;
  }
  return rvs_session_run_many(session, &program_id, 1, submit_out);
}

RVS_Result
rvs_session_run_to_address(RVS_Session *session, RVS_ProgramID program_id, U64 vaddr, RVS_SubmitInfo *submit_out)
{
  if (submit_out) { MemoryZeroStruct(submit_out); }
  if (session == 0 || submit_out == 0 || rvs_program_id_is_zero(program_id) || vaddr == 0) {
    return RVS_Result_InvalidArgument;
  }
  return rvs_session_submit(session, (RVS_EngineCommand){
    .kind = RVS_EngineCommandKind_Run,
    .run = {
      .programs_count = 1,
      .programs = &program_id,
      .mode = RVS_RunMode_ToAddress,
      .address = vaddr,
      .intent = { .kind = RVS_RunIntentKind_Execute },
    },
  }, submit_out);
}

RVS_Result
rvs_session_interrupt_many(RVS_Session *session, RVS_ProgramID *programs, U64 programs_count, RVS_SubmitInfo *submit_out)
{
  if (submit_out) { MemoryZeroStruct(submit_out); }
  if (session == 0 || programs == 0 || programs_count == 0 || submit_out == 0) { return RVS_Result_InvalidArgument; }
  for EachIndex(program_idx, programs_count) {
    if (rvs_program_id_is_zero(programs[program_idx])) { return RVS_Result_InvalidArgument; }
    for EachIndex(previous_idx, program_idx) {
      if (rvs_program_id_match(programs[previous_idx], programs[program_idx])) { return RVS_Result_InvalidArgument; }
    }
  }
  return rvs_session_submit(session, (RVS_EngineCommand){
    .kind = RVS_EngineCommandKind_Interrupt,
    .interrupt = { .programs = programs, .programs_count = programs_count },
  }, submit_out);
}

RVS_Result
rvs_session_interrupt(RVS_Session *session, RVS_ProgramID program_id, RVS_SubmitInfo *submit_out)
{
  if (submit_out) { MemoryZeroStruct(submit_out); }
  if (session == 0 || rvs_program_id_is_zero(program_id) || submit_out == 0) { return RVS_Result_InvalidArgument; }
  return rvs_session_interrupt_many(session, &program_id, 1, submit_out);
}

RVS_Result
rvs_session_select_thread(RVS_Session *session, RVS_ProgramID program_id, RVS_ThreadID thread_id)
{
  if (session == 0 || rvs_program_id_is_zero(program_id) || rvs_thread_id_is_zero(thread_id)) {
    return RVS_Result_InvalidArgument;
  }
  Temp scratch = scratch_begin(0, 0);
  RVS_EnginePreparedDecision prepared = {0};
  RVS_Result result = rvs_control_reduce_scheduler_event(session, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_ThreadSelected,
    .thread_selected = { .target = program_id, .thread = thread_id },
  }, scratch.arena, 1, &prepared);
  AssertAlways(prepared.emission_first == 0 && prepared.command_kind == RVS_SchedulerCommand_Null);
  scratch_end(scratch);
  return result;
}

RVS_Result
rvs_session_selected_thread(RVS_Session *session, RVS_ProgramID *program_id_out, RVS_ThreadID *thread_id_out)
{
  if (program_id_out) { *program_id_out = rvs_program_id_zero(); }
  if (thread_id_out) { *thread_id_out = rvs_thread_id_zero(); }
  if (session == 0 || program_id_out == 0 || thread_id_out == 0) { return RVS_Result_InvalidArgument; }
  rvs_control_mutex_take(session->control);
  RVS_Result result = RVS_Result_StaleState;
  RVS_Thread *thread = rvs_entity_thread_from_id_locked(&session->entities, session->scheduler.selected_thread);
  if (thread && !thread->snapshot.is_retired &&
      rvs_program_id_match(thread->snapshot.program, session->scheduler.selected_target)) {
    *program_id_out = thread->snapshot.program;
    *thread_id_out = thread->snapshot.thread;
    result = RVS_Result_Ok;
  }
  rvs_control_mutex_drop(session->control);
  return result;
}

RVS_Result
rvs_session_continue(RVS_Session *session, RVS_ProgramID program_id, RVS_SubmitInfo *submit_out)
{
  if (submit_out) { MemoryZeroStruct(submit_out); }
  if (session == 0 || rvs_program_id_is_zero(program_id) || submit_out == 0) {
    return RVS_Result_InvalidArgument;
  }
  return rvs_session_submit(session, (RVS_EngineCommand){
    .kind = RVS_EngineCommandKind_Run,
    .run = {
      .programs_count = 1,
      .programs = &program_id,
      .mode = RVS_RunMode_Normal,
      .intent = { .kind = RVS_RunIntentKind_Continue },
    },
  }, submit_out);
}

RVS_Result
rvs_session_step(RVS_Session *session, RVS_StepKind kind, RVS_StepUnit unit, RVS_ThreadID thread_id, RVS_SubmitInfo *submit_out)
{
  if (submit_out) { MemoryZeroStruct(submit_out); }
  if (session == 0 || submit_out == 0 || rvs_thread_id_is_zero(thread_id) ||
      (kind != RVS_StepKind_Into && kind != RVS_StepKind_Over && kind != RVS_StepKind_Out) ||
      (unit != RVS_StepUnit_Statement && unit != RVS_StepUnit_Line && unit != RVS_StepUnit_Instruction)) {
    return RVS_Result_InvalidArgument;
  }
  rvs_control_mutex_take(session->control);
  RVS_Thread *thread = rvs_entity_thread_from_id_locked(&session->entities, thread_id);
  RVS_Result result = thread && !thread->snapshot.is_retired ? RVS_Result_Unsupported : RVS_Result_StaleState;
  rvs_control_mutex_drop(session->control);
  return result;
}

RVS_Result
rvs_session_wait_for_event(Arena *arena, RVS_Session *session, U64 wait_us, RVS_Event *event_out)
{
  ProfBeginFunction();

  RVS_Result result = RVS_Result_Error;
  if (session == 0) {
    goto exit;
  }
  rvs_session_addref(session);
  RVS_Queue *event_queue = session->event_queue;
  RVS_QueuePopResult pop = rvs_queue_pop_result(event_queue, wait_us);
  RVS_SessionEventMessage *message = (RVS_SessionEventMessage *)pop.node;
  if (message) {
    if (!pop.is_closed) {
      *event_out = message->event;
      result = RVS_Result_Ok;
    }
    rvs_queue_recycle(event_queue, &message->base);
  }
  if (result != RVS_Result_Ok) {
    if (pop.is_closed) {
      result = RVS_Result_EngineStopped;
    } else {
      result = RVS_Result_Timeout;
    }
  }

  rvs_session_release(session);
  exit:;
  ProfEnd();
  return result;
}

RVS_Result
rvs_session_ack_event(RVS_Session *session, U64 sequence)
{
  if (session == 0 || sequence == 0) { return RVS_Result_InvalidArgument; }
  Temp scratch = scratch_begin(0, 0);

  RVS_EnginePreparedDecision prepared = {0};
  RVS_SchedulerEvent event  = { .kind = RVS_SchedulerEvent_AcknowledgeEvent, .acknowledged = { .sequence = sequence } };
  RVS_Result         result = rvs_control_reduce_scheduler_event(session, event, scratch.arena, 1, &prepared);
  AssertAlways(prepared.emission_first == 0 && prepared.command_kind == RVS_SchedulerCommand_Null);
  scratch_end(scratch);
  return result == RVS_Result_StaleState ? RVS_Result_Ok : result;
}

RVS_Result
rvs_session_copy_programs(Arena *arena, RVS_Session *session, RVS_ProgramSnapshot **snapshots_out, U64 *snapshots_count_out)
{
  if (snapshots_out) { *snapshots_out = 0; }
  if (snapshots_count_out) { *snapshots_count_out = 0; }
  if (arena == 0 || session == 0 || snapshots_out == 0 || snapshots_count_out == 0) { return RVS_Result_InvalidArgument; }
  rvs_session_addref(session);
  RVS_Result result = rvs_entity_copy_programs(&session->entities, arena, snapshots_out, snapshots_count_out);
  rvs_session_release(session);
  return result;
}

RVS_Result
rvs_session_fetch_program(RVS_Session *session, RVS_ProgramID id, U64 wait_us, RVS_ProgramSnapshot *snapshot_out)
{
  if (snapshot_out) { MemoryZeroStruct(snapshot_out); }
  if (session == 0 || snapshot_out == 0 || rvs_program_id_is_zero(id)) { return RVS_Result_InvalidArgument; }
  rvs_session_addref(session);
  RVS_Result result = rvs_entity_fetch_program(&session->entities, id, wait_us, snapshot_out);
  rvs_session_release(session);
  return result;
}

RVS_Result
rvs_session_fetch_process(RVS_Session *session, RVS_ProcessID id, U64 wait_us, RVS_ProcessSnapshot *snapshot_out)
{
  if (snapshot_out) { MemoryZeroStruct(snapshot_out); }
  if (session == 0 || snapshot_out == 0 || rvs_process_id_is_zero(id)) { return RVS_Result_InvalidArgument; }
  rvs_session_addref(session);
  RVS_Result result = rvs_entity_fetch_process(&session->entities, id, wait_us, snapshot_out);
  rvs_session_release(session);
  return result;
}

RVS_Result
rvs_session_fetch_thread(RVS_Session *session, RVS_ThreadID id, U64 wait_us, RVS_ThreadSnapshot *snapshot_out)
{
  if (snapshot_out) { MemoryZeroStruct(snapshot_out); }
  if (session == 0 || snapshot_out == 0 || rvs_thread_id_is_zero(id)) { return RVS_Result_InvalidArgument; }
  rvs_session_addref(session);
  RVS_Result result = rvs_entity_fetch_thread(&session->entities, id, wait_us, snapshot_out);
  rvs_session_release(session);
  return result;
}

void
rvs_session_addref(RVS_Session *session)
{
  AssertAlways(session != 0);
  ins_atomic_u32_inc_eval(&session->ref_count);
}

void
rvs_session_release(RVS_Session *session)
{
  if (session) {
    rvs_session_release_ref(session);
  }
}
