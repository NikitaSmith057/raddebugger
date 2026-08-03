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
  RVS_Program *program = rvs_session_program_from_id_locked(session, program_id);
  if (program) {
    result = program->state_epoch;
  }
  ProfEnd();
  return result;
}

internal void
rvs_session_bump_program_state_epoch_locked(RVS_Session *session, RVS_ProgramID program_id)
{
  ProfBeginFunction();
  RVS_Program *program = rvs_session_program_from_id_locked(session, program_id);
  if (program) {
    program->state_epoch += 1;
    RVS_TargetLedgerEntry *entry = rvs_scheduler_target_from_id_locked(&session->scheduler, program_id);
    AssertAlways(entry != 0);
    entry->state_generation += 1;
  }
  ProfEnd();
}

internal B32
rvs_session_operation_key_resolves_locked(RVS_Session *session, RVS_OperationKey key)
{
  if (key.operation_class == RVS_OperationClass_Topology ||
      key.operation_class == RVS_OperationClass_ExecutionWorkflow ||
      key.operation_class == RVS_OperationClass_InterruptTransition ||
      key.operation_class == RVS_OperationClass_Termination ||
      key.operation_class == RVS_OperationClass_TargetConfiguration) {
    return 1;
  }

  return rvs_session_program_from_id_locked(session, key.program_id) != 0;
}

internal RVS_Program *
rvs_session_program_add_locked(RVS_Session *session, U32 pid, DMN_Handle process)
{
  RVS_Program *program = push_array(session->program_arena, RVS_Program, 1);
  program->arena       = arena_alloc(.name = "Engine Program");
  program->id          = process;
  program->pid         = pid;
  program->process     = process;
  program->state_epoch = 1;
  SLLQueuePush(session->first_program, session->last_program, program);
  rvs_scheduler_target_add_locked(&session->scheduler, program->id);
  return program;
}

internal B32
rvs_session_programs_to_processes(RVS_Session *session, RVS_ProgramID *programs, U64 programs_count, DMN_Handle *processes_out)
{
  B32 all_programs_found = programs_count != 0;
  for EachIndex(program_idx, programs_count) {
    RVS_Program *program = rvs_session_program_from_id_locked(session, programs[program_idx]);
    if (program == 0 || dmn_handle_match(program->process, dmn_handle_zero())) {
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
  if (operation->key.operation_class == RVS_OperationClass_ReadOnly) {
    reply->program_state_epoch = operation->captured_program_state_epoch;
    if (reply->program_state_epoch != rvs_session_program_state_epoch_locked(session, operation->key.program_id)) {
      reply->result = RVS_Result_StaleState;
    }
    for EachIndex(target_idx, operation->targets_count) {
      RVS_TargetSnapshot *snapshot = &operation->targets[target_idx];
      RVS_TargetLedgerEntry *entry = rvs_scheduler_target_from_id_locked(&session->scheduler, snapshot->target);
      if (entry == 0 || entry->is_termination_fenced ||
          entry->state_generation != snapshot->state_generation ||
          entry->topology_generation != snapshot->topology_generation ||
          entry->configuration_generation != snapshot->configuration_generation) {
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
    rvs_engine_control_release(session->control);
    arena_release(session->arena);
  }
}

internal void
rvs_session_release_engine(RVS_Session *session)
{
  // The engine holds control->mutex while releasing session ownership.
  rvs_scheduler_release_execution_leases_locked(&session->scheduler);
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

internal B32
rvs_operation_schedule_from_engine_command(RVS_EngineCommand command, RVS_RequestPolicy *policy_out, RVS_OperationKey *key_out)
{
  switch (command.kind) {
  case RVS_EngineCommandKind_Launch: {
    *policy_out = rvs_request_policy_from_engine_command_kind(command.kind);
    *key_out = (RVS_OperationKey){
      .operation_class = rvs_op_class_from_engine_command_kind(command.kind),
      .operation_id    = command.kind,
    };
    return 1;
  } break;
  case RVS_EngineCommandKind_Run: {
    if (command.run.programs_count == 0 || command.run.programs == 0) {
      return 0;
    }
    for EachIndex(program_idx, command.run.programs_count) {
      if (dmn_handle_match(command.run.programs[program_idx], dmn_handle_zero())) {
        return 0;
      }
      for EachIndex(previous_idx, program_idx) {
        if (dmn_handle_match(command.run.programs[previous_idx], command.run.programs[program_idx])) {
          return 0;
        }
      }
    }
    *policy_out = rvs_request_policy_from_engine_command_kind(command.kind);
    *key_out = (RVS_OperationKey){
      .operation_class = rvs_op_class_from_engine_command_kind(command.kind),
      .operation_id    = command.kind,
    };
    return 1;
  } break;
  default: break;
  }
  return 0;
}

internal RVS_Result
rvs_session_submit(RVS_Session *session, RVS_EngineCommand command, RVS_SubmitInfo *submit_out)
{
  ProfBeginFunction();

  RVS_Result         result = RVS_Result_Error;
  RVS_RequestPolicy  policy = RVS_RequestPolicy_Null;
  RVS_OperationKey   key = {0};

  AssertAlways(session != 0);
  AssertAlways(submit_out != 0);

  MemoryZeroStruct(submit_out);

  // Validate Run shape before accessing its target or constructing scheduling metadata.
  if ( ! rvs_operation_schedule_from_engine_command(command, &policy, &key)) {
    goto exit;
  }
  AssertAlways(rvs_operation_key_is_well_formed_for_policy(policy, key));

  mutex_take(session->control->mutex);

  if (session->control->is_shutdown ||
      session->engine_released      ||
      session->engine == 0          ||
      ins_atomic_u32_eval(&session->engine->state) != RVS_ThreadState_Live) {
    result = RVS_Result_EngineStopped;
    goto exit_control_mutex;
  }

  if (rvs_scheduler_execution_blocks_operation_locked(&session->scheduler, key.operation_class)) {
    result = RVS_Result_AlreadyPending;
    goto exit_control_mutex;
  }

  RVS_Engine *engine = session->engine;
  mutex_take(engine->arena_mutex);

  if ( ! rvs_session_operation_key_resolves_locked(session, key)) {
    goto exit_arena_mutex;
  }
  if (command.kind == RVS_EngineCommandKind_Run) {
    for EachIndex(program_idx, command.run.programs_count) {
      if (rvs_session_program_from_id_locked(session, command.run.programs[program_idx]) == 0) {
        goto exit_arena_mutex;
      }
    }
  }
  U64 captured_program_state_epoch = 0;
  if (key.operation_class == RVS_OperationClass_ReadOnly) {
    captured_program_state_epoch = rvs_session_program_state_epoch_locked(session, key.program_id);
  }
  RVS_ProgramID *targets = 0;
  U64 targets_count = 0;
  if (command.kind == RVS_EngineCommandKind_Run) {
    targets = command.run.programs;
    targets_count = command.run.programs_count;
  }
  RVS_SchedulerAdmission admission = {0};
  result = rvs_scheduler_admit_locked(&session->scheduler, engine->request_pool, &engine->next_request_id, policy, key, targets, targets_count, captured_program_state_epoch, &admission);
  if (result != RVS_Result_Ok) {
    goto exit_arena_mutex;
  }
  if (admission.is_terminal) {
    submit_out->request = admission.request;
    result = RVS_Result_Ok;
    goto exit_arena_mutex;
  }
  if (admission.joined) {
    submit_out->request = admission.request;
    goto exit_arena_mutex;
  }

  RVS_ScheduledOperation *operation = admission.operation;
  operation->command_kind = command.kind;

  RVS_EngineMessage message = {
    .type    = RVS_EngineMessageType_Command,
    .session = session,
    .command = command,
  };
  message.request_id = operation->request->request_id;

  result = rvs_engine_send_message_locked(engine, &message);

  if (result == RVS_Result_Ok) {
    submit_out->request = operation->request;
    submit_out->control = rvs_request_control_alloc(session, operation, admission.registered);
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
