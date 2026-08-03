// This file is included only by rvs_engine.c after private engine types.

#include "radvs/rvs_session.h"
#include "radvs/rvs_demon.h"
#include "radvs/rvs_request.h"

typedef struct
{
  RVS_QueueNode base;
  RVS_Event     event;
} RVS_SessionEventMessage;

internal RVS_Program *
rvs_session_program_from_id_locked(RVS_Session *session, RVS_ProgramID program_id)
{
  for EachNode(program, RVS_Program, session->first_program) {
    if (dmn_handle_match(program->id, program_id)) {
      return program;
    }
  }
  return 0;
}

internal U64
rvs_session_program_state_epoch_locked(RVS_Session *session, RVS_ProgramID program_id)
{
  ProfBeginFunction();
  U64 result = 0;
  RVS_TargetLedgerEntry *entry = rvs_scheduler_target_from_id_locked(&session->scheduler, program_id);
  if (entry) {
    result = entry->revision;
  }
  ProfEnd();
  return result;
}

internal B32
rvs_session_operation_key_resolves_locked(RVS_Session *session, RVS_SchedulerKey key)
{
  if (key.op != RVS_SchedulerOp_ReadOnly && key.op != RVS_SchedulerOp_TargetConfiguration) {
    return 1;
  }

  RVS_Program *program = rvs_session_program_from_id_locked(session, key.target);
  return program != 0 && program->lifecycle == RVS_ProgramLifecycle_Live;
}

internal RVS_Program *
rvs_session_program_add_locked(RVS_Session *session, U32 pid, DMN_Handle process)
{
  RVS_Program *program = push_array(session->program_arena, RVS_Program, 1);
  program->arena       = arena_alloc(.name = "Engine Program");
  program->id          = process;
  program->pid         = pid;
  program->process     = process;
  SLLQueuePush(session->first_program, session->last_program, program);
  return program;
}

internal void
rvs_session_program_retire_locked(RVS_Session *session, RVS_ProgramID program_id, U32 exit_code)
{
  RVS_Program *program = rvs_session_program_from_id_locked(session, program_id);
  if (program && program->lifecycle == RVS_ProgramLifecycle_Live) {
    program->lifecycle = RVS_ProgramLifecycle_Removed;
    program->exit_code = exit_code;
  }
}

internal B32
rvs_session_programs_to_processes(RVS_Session *session, RVS_ProgramID *programs, U64 programs_count, DMN_Handle *processes_out)
{
  B32 all_programs_found = programs_count != 0;
  for EachIndex(program_idx, programs_count) {
    RVS_Program *program = rvs_session_program_from_id_locked(session, programs[program_idx]);
    if (program == 0 || program->lifecycle != RVS_ProgramLifecycle_Live ||
        dmn_handle_match(program->process, dmn_handle_zero())) {
      all_programs_found = 0;
      break;
    }
    processes_out[program_idx] = program->process;
  }
  return all_programs_found;
}

internal void
rvs_session_prepare_reply_locked(RVS_Session *session, RVS_ScheduledOperation *operation, RVS_EngineReply *reply)
{
  if (operation->key.op == RVS_SchedulerOp_ReadOnly) {
    reply->program_state_epoch = operation->captured_program_state_epoch;
    if (reply->program_state_epoch != rvs_session_program_state_epoch_locked(session, operation->key.target)) {
      reply->result = RVS_Result_StaleState;
    }
    for EachIndex(target_idx, operation->targets_count) {
      RVS_TargetSnapshot *snapshot = &operation->targets[target_idx];
      RVS_TargetLedgerEntry *entry = rvs_scheduler_target_from_id_locked(&session->scheduler, snapshot->target);
      if (entry == 0 || entry->is_termination_fenced ||
           entry->revision != snapshot->revision) {
        reply->result = RVS_Result_StaleState;
        break;
      }
    }
  }
}

internal RVS_Result
rvs_session_push_event(RVS_Session *session, RVS_Event *event)
{
  ProfBeginFunction();
  RVS_SessionEventMessage *message = rvs_queue_alloc_struct(session->event_queue, RVS_SessionEventMessage);
  RVS_Result result = RVS_Result_EngineStopped;
  if (message) {
    rvs_demon_event_copy(session->arena, &message->event, event);
    result = rvs_queue_push(session->event_queue, &message->base);
  }
  ProfEnd();
  return result;
}

internal void
rvs_session_close_events_locked(RVS_Session *session)
{
  rvs_queue_close(session->event_queue);
}

internal RVS_Session *
rvs_session_alloc(RVS_Engine *engine)
{
  Arena *arena = arena_alloc(.name = "Session");
  RVS_Session *session = push_array(arena, RVS_Session, 1);
  session->arena = arena;
  session->engine = engine;
  session->control = engine->control;
  session->scheduler.arena = arena;
  session->scheduler.recycle_mutex = mutex_alloc();
  session->ref_count = 2; // engine ownership plus the returned handle
  session->event_queue = rvs_queue_alloc(arena, sizeof(RVS_SessionEventMessage), AlignOf(RVS_SessionEventMessage));
  session->program_arena = arena_alloc(.name = "Session Programs");
  rvs_engine_control_addref(session->control);
  return session;
}

internal void
rvs_session_release_ref(RVS_Session *session)
{
  if (ins_atomic_u32_dec_eval(&session->ref_count) == 0) {
    AssertAlways(session->engine_released);
    rvs_queue_release(session->event_queue);
    mutex_release(session->scheduler.recycle_mutex);
    rvs_engine_control_release(session->control);
    arena_release(session->arena);
  }
}

internal void
rvs_session_release_engine(RVS_Session *session)
{
  // The engine holds control->mutex while releasing session ownership.
  AssertAlways(session->scheduler.stop_transaction.owner == 0 && session->scheduler.resume_transaction.owner == 0);
  for EachNode(entry, RVS_TargetLedgerEntry, session->scheduler.target_first) {
    AssertAlways(entry->state == RVS_TargetExecutionState_Idle || entry->state == RVS_TargetState_Removed);
  }
  for EachNode(program, RVS_Program, session->first_program) {
    arena_release(program->arena);
  }
  arena_release(session->program_arena);
  session->program_arena = 0;
  session->first_program = 0;
  session->last_program = 0;
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

  // Validate Run shape before accessing its target or constructing scheduling metadata.
  if ( ! rvs_engine_command_scheduler_op(command, &op)) {
    goto exit;
  }
  key = (RVS_SchedulerKey){ .op = op, .identity = command.kind };
  AssertAlways(rvs_scheduler_key_is_well_formed(key));

  mutex_take(session->control->mutex);

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
  mutex_take(engine->arena_mutex);

  if ( ! rvs_session_operation_key_resolves_locked(session, key)) {
    goto exit_arena_mutex;
  }
  if (command.kind == RVS_EngineCommandKind_Run || command.kind == RVS_EngineCommandKind_Interrupt) {
    RVS_ProgramID *programs = command.kind == RVS_EngineCommandKind_Run ? command.run.programs : command.interrupt.programs;
    U64 programs_count = command.kind == RVS_EngineCommandKind_Run ? command.run.programs_count : command.interrupt.programs_count;
    for EachIndex(program_idx, programs_count) {
      RVS_Program *program = rvs_session_program_from_id_locked(session, programs[program_idx]);
      if (program == 0) {
        goto exit_arena_mutex;
      }
      if (program->lifecycle != RVS_ProgramLifecycle_Live) {
        result = RVS_Result_StaleState;
        goto exit_arena_mutex;
      }
    }
  }
  U64 captured_program_state_epoch = 0;
  if (op == RVS_SchedulerOp_ReadOnly) {
    captured_program_state_epoch = rvs_session_program_state_epoch_locked(session, key.target);
  }
  RVS_ProgramID *targets = 0;
  U64 targets_count = 0;
  if (command.kind == RVS_EngineCommandKind_Run || command.kind == RVS_EngineCommandKind_Interrupt) {
    targets = command.kind == RVS_EngineCommandKind_Run ? command.run.programs : command.interrupt.programs;
    targets_count = command.kind == RVS_EngineCommandKind_Run ? command.run.programs_count : command.interrupt.programs_count;
  }
  RVS_SchedulerAdmission admission = {0};
  result = rvs_scheduler_admit_locked(&session->scheduler, engine->request_pool, &engine->next_request_id,
                                      op, key, targets, targets_count, captured_program_state_epoch, &admission);
  if (result != RVS_Result_Ok) {
    goto exit_arena_mutex;
  }
  if (admission.is_terminal || admission.joined) {
    submit_out->request = admission.request;
    result = RVS_Result_Ok;
    goto exit_arena_mutex;
  }
  RVS_ScheduledOperation *operation = admission.operation;

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
  exit_arena_mutex:;
  mutex_drop(engine->arena_mutex);

  exit_control_mutex:;
  mutex_drop(session->control->mutex);

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

  RVS_EngineCommand command = {
    .kind = RVS_EngineCommandKind_Run,
    .run  = {
      .programs_count = programs_count,
      .programs       = programs,
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
  if (session == 0 || dmn_handle_match(program_id, dmn_handle_zero()) || submit_out == 0) {
    return RVS_Result_InvalidArgument;
  }
  return rvs_session_run_many(session, &program_id, 1, submit_out);
}

RVS_Result
rvs_session_interrupt_many(RVS_Session *session, RVS_ProgramID *programs, U64 programs_count, RVS_SubmitInfo *submit_out)
{
  if (submit_out) { MemoryZeroStruct(submit_out); }
  if (session == 0 || programs == 0 || programs_count == 0 || submit_out == 0) { return RVS_Result_InvalidArgument; }
  return rvs_session_submit(session, (RVS_EngineCommand){
    .kind = RVS_EngineCommandKind_Interrupt,
    .interrupt = { .programs = programs, .programs_count = programs_count },
  }, submit_out);
}

RVS_Result
rvs_session_interrupt(RVS_Session *session, RVS_ProgramID program_id, RVS_SubmitInfo *submit_out)
{
  if (submit_out) { MemoryZeroStruct(submit_out); }
  if (session == 0 || dmn_handle_match(program_id, dmn_handle_zero()) || submit_out == 0) { return RVS_Result_InvalidArgument; }
  return rvs_session_interrupt_many(session, &program_id, 1, submit_out);
}

RVS_Result
rvs_session_select_thread(RVS_Session *session, RVS_ProgramID program_id, RVS_ThreadID thread_id)
{
  if (session == 0 || dmn_handle_match(program_id, dmn_handle_zero()) || dmn_handle_match(thread_id, dmn_handle_zero())) {
    return RVS_Result_InvalidArgument;
  }
  mutex_take(session->control->mutex);
  RVS_Result result = RVS_Result_StaleState;
  RVS_TargetLedgerEntry *target = rvs_scheduler_target_from_id_locked(&session->scheduler, program_id);
  RVS_ThreadLedgerEntry *thread = rvs_scheduler_thread_from_id_locked(&session->scheduler, thread_id);
  if (target && target->state != RVS_TargetState_Removed && thread && !thread->is_removed &&
      dmn_handle_match(thread->target, program_id)) {
    session->scheduler.selected_target = program_id;
    session->scheduler.selected_thread = thread_id;
    result = RVS_Result_Ok;
  }
  mutex_drop(session->control->mutex);
  return result;
}

RVS_Result
rvs_session_selected_thread(RVS_Session *session, RVS_ProgramID *program_id_out, RVS_ThreadID *thread_id_out)
{
  if (program_id_out) { *program_id_out = dmn_handle_zero(); }
  if (thread_id_out) { *thread_id_out = dmn_handle_zero(); }
  if (session == 0 || program_id_out == 0 || thread_id_out == 0) { return RVS_Result_InvalidArgument; }
  mutex_take(session->control->mutex);
  RVS_Result result = RVS_Result_StaleState;
  RVS_ThreadLedgerEntry *thread = rvs_scheduler_thread_from_id_locked(&session->scheduler, session->scheduler.selected_thread);
  if (thread && !thread->is_removed && dmn_handle_match(thread->target, session->scheduler.selected_target)) {
    *program_id_out = thread->target;
    *thread_id_out = thread->thread;
    result = RVS_Result_Ok;
  }
  mutex_drop(session->control->mutex);
  return result;
}

RVS_Result
rvs_session_continue(RVS_Session *session, RVS_ProgramID program_id, RVS_SubmitInfo *submit_out)
{
  return rvs_session_run(session, program_id, submit_out);
}

RVS_Result
rvs_session_step(RVS_Session *session, RVS_StepKind kind, RVS_ThreadID thread_id, RVS_SubmitInfo *submit_out)
{
  if (submit_out) { MemoryZeroStruct(submit_out); }
  if (session == 0 || submit_out == 0 || dmn_handle_match(thread_id, dmn_handle_zero()) ||
      (kind != RVS_StepKind_Into && kind != RVS_StepKind_Over && kind != RVS_StepKind_Out)) {
    return RVS_Result_InvalidArgument;
  }
  mutex_take(session->control->mutex);
  RVS_ThreadLedgerEntry *thread = rvs_scheduler_thread_from_id_locked(&session->scheduler, thread_id);
  RVS_Result result = thread && !thread->is_removed ? RVS_Result_Unsupported : RVS_Result_StaleState;
  mutex_drop(session->control->mutex);
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
  mutex_take(session->control->mutex);
  B32 is_stopped = session->control->is_shutdown || session->engine_released;
  RVS_Queue *event_queue = session->event_queue;
  mutex_drop(session->control->mutex);
  if (is_stopped) {
    result = RVS_Result_EngineStopped;
    goto exit_session;
  }

  RVS_SessionEventMessage *message = rvs_queue_pop_struct(event_queue, RVS_SessionEventMessage, wait_us);
  mutex_take(session->control->mutex);
  is_stopped = session->control->is_shutdown || session->engine_released;
  mutex_drop(session->control->mutex);
  if (message) {
    if ( ! is_stopped) {
      rvs_demon_event_copy(arena, event_out, &message->event);
      result = RVS_Result_Ok;
    }
    rvs_queue_recycle(event_queue, &message->base);
  }
  if (result != RVS_Result_Ok) {
    if (is_stopped) {
      result = RVS_Result_EngineStopped;
    } else {
      result = RVS_Result_Timeout;
    }
  }

  exit_session:;
  rvs_session_release(session);
  exit:;
  ProfEnd();
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
