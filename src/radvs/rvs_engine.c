// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////

#include "radvs/rvs_engine.h"
#include "radvs/rvs_async.h"
#include "radvs/rvs_demon.h"
#include "radvs/rvs_entity.h"
#include "radvs/rvs_request.h"
#include "radvs/rvs_scheduler.h"

////////////////////////////////

typedef struct
{
  RVS_EngineCommandKind kind;
  union {
    struct {
      ProcessLaunchParams params;
    } launch;
    struct {
      U64            programs_count;
      RVS_ProgramID *programs;
      RVS_RunMode     mode;
      U64             address;
      RVS_RunIntent   intent;
    } run;
    struct {
      U64            programs_count;
      RVS_ProgramID *programs;
    } interrupt;
  };
} RVS_EngineCommand;

typedef enum
{
  RVS_EngineMessageType_Null,
  RVS_EngineMessageType_DispatchRequest,
  RVS_EngineMessageType_DemonReply,
  RVS_EngineMessageType_SchedulerEvent,
  RVS_EngineMessageType_Shutdown,
} RVS_EngineMessageType;

typedef struct RVS_EnginePreparedEmission RVS_EnginePreparedEmission;
struct RVS_EnginePreparedEmission
{
  RVS_EnginePreparedEmission *next;
  RVS_SchedulerEmissionKind   kind;
  RVS_Request                *request;
  RVS_EngineReply             reply;
  RVS_Event                   event;
};

typedef struct
{
  RVS_EnginePreparedEmission *emission_first;
  RVS_EnginePreparedEmission *emission_last;
  RVS_SchedulerCommandKind    command_kind;
  RVS_SchedulerCommandToken   command_token;
  RVS_Result                  command_prepare_result;
  DMN_Handle                 *processes;
  U64                         processes_count;
  DMN_TrapChunkList           traps;
  RVS_MessageID               execution_request_id;
  U32                         pid;
  DMN_Handle                  process;
} RVS_EnginePreparedDecision;

typedef struct
{
  RVS_QueueNode          base;
  RVS_EngineMessageType  type;
  union {
    struct {
      RVS_EngineCommand command;
      RVS_MessageID     request_id;
    };
    RVS_DemonReply      demon_reply;
    RVS_SchedulerEvent  scheduler_event;
  };
} RVS_EngineMessage;

struct RVS_EngineControl
{
  Arena *arena;
  Mutex  mutex;
  U32    ref_count;
  B32    is_shutdown;
};

struct RVS_Engine
{
  Arena             *arena;
  RVS_Queue         *inbox_queue;
  RVS_MessageID      next_request_id;
  RVS_ThreadState    state;
  Thread             thread;
  RVS_EngineControl *control;
  RVS_RequestPool   *request_pool;
  RVS_Session       *session;
  RVS_Demon         *demon;

#if RVS_ENGINE_TESTING
  U32 test_fail_command_enqueue;
  U32 test_hold_before_dispatch;
  U32 test_is_held_before_dispatch;
  U32 test_hold_after_dispatch_prepare;
  U32 test_is_held_after_dispatch_prepare;
  U32 test_fail_demon_message_type;
  U32 test_effect_sequence;
  U32 test_first_emission_sequence;
  U32 test_first_command_sequence;
#endif
};

thread_static U32 rvs_control_mutex_depth;

////////////////////////////////
// Types

internal void
rvs_engine_assert_well_formed_programs(RVS_ProgramID *programs, U64 programs_count)
{
  AssertAlways(programs != 0 && programs_count != 0);
  for EachIndex(program_idx, programs_count) {
    AssertAlways(!rvs_program_id_is_zero(programs[program_idx]));
    for EachIndex(previous_idx, program_idx) {
      AssertAlways(!rvs_program_id_match(programs[previous_idx], programs[program_idx]));
    }
  }
}

internal void
rvs_engine_command_scheduler_op(RVS_EngineCommand command, RVS_SchedulerOp *op_out)
{
  AssertAlways(op_out != 0);
  switch (command.kind) {
  case RVS_EngineCommandKind_Launch: {
    *op_out = RVS_SchedulerOp_Launch;
  } break;

  case RVS_EngineCommandKind_Run: {
    rvs_engine_assert_well_formed_programs(command.run.programs, command.run.programs_count);
    AssertAlways((command.run.mode == RVS_RunMode_Normal && command.run.address == 0) ||
                 (command.run.mode == RVS_RunMode_ToAddress && command.run.address != 0 && command.run.programs_count == 1));
    AssertAlways(command.run.intent.kind == RVS_RunIntentKind_Execute || command.run.intent.kind == RVS_RunIntentKind_Continue);
    *op_out = RVS_SchedulerOp_Run;
  } break;

  case RVS_EngineCommandKind_Interrupt: {
    rvs_engine_assert_well_formed_programs(command.interrupt.programs, command.interrupt.programs_count);
    *op_out = RVS_SchedulerOp_Interrupt;
  } break;

  default: { InvalidPath; } break;
  }
}

internal void
rvs_control_mutex_take(RVS_EngineControl *control)
{
  AssertAlways(rvs_control_mutex_depth == 0);
  mutex_take(control->mutex);
  rvs_control_mutex_depth = 1;
}

internal void
rvs_control_mutex_drop(RVS_EngineControl *control)
{
  AssertAlways(rvs_control_mutex_depth == 1);
  rvs_control_mutex_depth = 0;
  mutex_drop(control->mutex);
}

internal void
rvs_control_assert_unlocked(void)
{
  AssertAlways(rvs_control_mutex_depth == 0);
}

#include "radvs/rvs_session.h"

////////////////////////////////
// Internals

internal RVS_Result rvs_control_reduce_scheduler_event(RVS_Session *session, RVS_SchedulerEvent event, Arena *scratch,
                                                        B32 reject_if_shutdown, RVS_EnginePreparedDecision *prepared);

internal RVS_SchedulerEvent rvs_engine_execute_prepared_decision(RVS_Engine *engine, RVS_EnginePreparedDecision *prepared);

internal B32 rvs_engine_collect_operation_traps_locked(Arena *arena, RVS_Engine *engine, RVS_ScheduledOperation *operation,
                                                       RVS_ProgramID *allowed_targets, U64 allowed_targets_count,
                                                       DMN_TrapChunkList *traps_out);


internal void rvs_engine_drive_scheduler_event(RVS_Engine *engine, RVS_SchedulerEvent event);

internal RVS_Result rvs_engine_send_demon_message(RVS_Engine *engine, RVS_DemonMessage message);

////////////////////////////////
// Scheduler Adapter

internal void
rvs_engine_apply_scheduler_event_locked(RVS_Engine *engine, RVS_SchedulerEvent event, RVS_SchedulerDecision *decision_out)
{
  rvs_scheduler_apply_locked(&engine->session->scheduler, event, decision_out);
}
internal void
rvs_engine_apply_scheduler_event(RVS_Engine *engine, RVS_SchedulerEvent event, RVS_SchedulerDecision *decision_out)
{
  rvs_control_mutex_take(engine->control);
  rvs_engine_apply_scheduler_event_locked(engine, event, decision_out);
  rvs_control_mutex_drop(engine->control);
}

//////////////////////////////
// Message Transport

internal void
rvs_engine_command_copy(Arena *arena, RVS_EngineCommand *dst, RVS_EngineCommand *src)
{
  ProfBeginFunction();
  *dst = *src;

  switch (src->kind) {
  case RVS_EngineCommandKind_Launch: {
    dst->launch.params = *process_launch_params_copy(arena, &src->launch.params);
  } break;
  case RVS_EngineCommandKind_Run: {
    dst->run.programs = push_array(arena, RVS_ProgramID, src->run.programs_count);
    dst->run.programs_count = src->run.programs_count;
    MemoryCopyTyped(dst->run.programs, src->run.programs, src->run.programs_count);
  } break;
  case RVS_EngineCommandKind_Interrupt: {
    dst->interrupt.programs = push_array(arena, RVS_ProgramID, src->interrupt.programs_count);
    dst->interrupt.programs_count = src->interrupt.programs_count;
    MemoryCopyTyped(dst->interrupt.programs, src->interrupt.programs, src->interrupt.programs_count);
  } break;
  default: { InvalidPath; } break;
  }
  ProfEnd();
}

internal void
rvs_engine_message_copy(Arena *arena, RVS_EngineMessage *dst, RVS_EngineMessage *src)
{
  ProfBeginFunction();
  RVS_QueueNode base = dst->base;
  *dst = *src;
  dst->base = base;

  switch (src->type) {
  case RVS_EngineMessageType_DispatchRequest: {
    rvs_engine_command_copy(arena, &dst->command, &src->command);
  } break;
  case RVS_EngineMessageType_DemonReply: {
    rvs_demon_reply_copy(arena, &dst->demon_reply, &src->demon_reply);
  } break;
  case RVS_EngineMessageType_SchedulerEvent: {
    AssertAlways(src->scheduler_event.kind == RVS_SchedulerEvent_CommandOutcome);
  } break;
  case RVS_EngineMessageType_Shutdown: {
  } break;
  default: { InvalidPath; } break;
  }
  ProfEnd();
}

internal void
rvs_engine_message_queue_copy(Arena *arena, void *dst, void *src)
{
  rvs_engine_message_copy(arena, dst, src);
}

internal RVS_Result
rvs_engine_send_message_locked(RVS_Engine *engine, RVS_EngineMessage *spec)
{
  ProfBeginFunction();
#if RVS_ENGINE_TESTING
  if (spec->type == RVS_EngineMessageType_DispatchRequest &&
      ins_atomic_u32_eval_cond_assign(&engine->test_fail_command_enqueue, 0, 1) == 1) {
    ProfEnd();
    return RVS_Result_Error;
  }
#endif
  RVS_Result result = rvs_queue_push_copy(engine->inbox_queue, spec, rvs_engine_message_queue_copy);
  ProfEnd();
  return result;
}

internal RVS_Result
rvs_engine_send_message(RVS_Engine *engine, RVS_EngineMessage *spec)
{
  ProfBeginFunction();
  RVS_Result result = rvs_engine_send_message_locked(engine, spec);
  ProfEnd();
  return result;
}

////////////////////////////////
internal RVS_EngineControl *
rvs_engine_control_alloc(void)
{
  ProfBeginFunction();
  Arena *arena = arena_alloc(.name = "Engine Control");
  RVS_EngineControl *control = push_array(arena, RVS_EngineControl, 1);
  control->arena = arena;
  control->mutex = mutex_alloc();
  control->ref_count = 1;
  ProfEnd();
  return control;
}

internal void
rvs_engine_control_addref(RVS_EngineControl *control)
{
  ProfBeginFunction();
  ins_atomic_u32_inc_eval(&control->ref_count);
  ProfEnd();
}

internal void
rvs_engine_control_release(RVS_EngineControl *control)
{
  ProfBeginFunction();
  if (ins_atomic_u32_dec_eval(&control->ref_count) == 0) {
    mutex_release(control->mutex);
    arena_release(control->arena);
  }
  ProfEnd();
}

#include "radvs/rvs_request.c"
#include "radvs/rvs_entity.c"
#include "radvs/rvs_scheduler.c"
#include "radvs/rvs_session.c"

internal B32
rvs_engine_collect_operation_traps_locked(Arena *arena, RVS_Engine *engine, RVS_ScheduledOperation *operation,
                                           RVS_ProgramID *allowed_targets, U64 allowed_targets_count,
                                           DMN_TrapChunkList *traps_out)
{
  AssertAlways(operation != 0 && operation->key.op == RVS_SchedulerOp_Run && traps_out != 0);
  for (RVS_PlanID plan_id = operation->active_plan_id; plan_id != 0;) {
    RVS_Plan *plan = rvs_scheduler_plan_from_id_locked(&engine->session->scheduler, plan_id);
    if (plan == 0 || plan->operation_request_id != operation->request->request_id) { return 0; }
    if (plan->header.kind == RVS_PlanKind_RunToAddress) {
      B32 target_allowed = 0;
      for EachIndex(target_idx, allowed_targets_count) {
        if (rvs_program_id_match(allowed_targets[target_idx], plan->run_to_address.target)) {
          target_allowed = 1;
          break;
        }
      }
      if (target_allowed) {
        RVS_TargetControl *program = rvs_scheduler_target_from_id_locked(&engine->session->scheduler, plan->run_to_address.target);
        if (program == 0 || program->state == RVS_TargetState_Removed) { return 0; }
        RVS_ProcessSnapshot process = {0};
        if (!rvs_entity_live_process_for_program_locked(&engine->session->entities, program->id, &process)) { return 0; }
        DMN_Trap trap = {
          .process = rvs_process_id_handle(process.process),
          .vaddr = plan->run_to_address.vaddr,
          .id = plan->header.id,
        };
        dmn_trap_chunk_list_push(arena, traps_out, 8, &trap);
      }
    }
    plan_id = plan->header.parent;
  }
  return 1;
}

internal B32
rvs_engine_request_mark_dispatched_locked(RVS_Engine *engine, RVS_MessageID request_id, RVS_EngineCommand *command,
                                          RVS_EnginePreparedDecision *prepared)
{
  ProfBeginFunction();
  B32 result = 0;
  RVS_Session *session = engine->session;
  if ( ! engine->control->is_shutdown) {
    RVS_SchedulerOp         command_op = RVS_SchedulerOp_Null;
    RVS_ScheduledOperation *operation  = rvs_scheduler_operation_from_request_id_locked(&session->scheduler, request_id);
    if (operation) {
      rvs_engine_command_scheduler_op(*command, &command_op);
      if (operation->key.op == command_op) {
        RVS_SchedulerDecision decision = {0};
        rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
          .kind = RVS_SchedulerEvent_DispatchStarted,
          .request = { .request_id = request_id },
        }, &decision);
        result = decision.result == RVS_Result_Ok && decision.emissions.first == 0;
        if (result && decision.command.kind != RVS_SchedulerCommand_Null) {
          AssertAlways(command->kind == RVS_EngineCommandKind_Interrupt &&
                       decision.command.kind == RVS_SchedulerCommand_InterruptExecution);
          prepared->command_prepare_result = RVS_Result_Ok;
          prepared->command_kind = decision.command.kind;
          prepared->command_token = decision.command.token;
          prepared->execution_request_id = decision.command.interrupt_execution.execution_request_id;
        }
        rvs_scheduler_decision_release(&decision);
      }
    }
  }
  ProfEnd();
  return result;
}

internal B32
rvs_engine_request_mark_dispatched(RVS_Engine *engine, RVS_MessageID request_id, RVS_EngineCommand *command)
{
  RVS_EnginePreparedDecision prepared = {0};
  rvs_control_mutex_take(engine->control);
  B32 result = rvs_engine_request_mark_dispatched_locked(engine, request_id, command, &prepared);
  rvs_control_mutex_drop(engine->control);
  AssertAlways(!result || prepared.command_kind == RVS_SchedulerCommand_Null);
  return result;
}

internal void
rvs_engine_release_active_requests(RVS_Engine *engine, RVS_Result pending_result)
{
  ProfBeginFunction();
  AssertAlways(pending_result == RVS_Result_EngineStopped);
  rvs_engine_drive_scheduler_event(engine, (RVS_SchedulerEvent){ .kind = RVS_SchedulerEvent_Shutdown });
  ProfEnd();
}

internal void
rvs_engine_complete_reply(RVS_Engine *engine, RVS_EngineReply reply)
{
  ProfBeginFunction();
  rvs_engine_drive_scheduler_event(engine, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_BackendCompleted,
    .completed = { .reply = reply },
  });
  ProfEnd();
}

internal void
rvs_engine_prepare_scheduler_decision_locked(RVS_Engine *engine, RVS_SchedulerDecision *decision, Arena *scratch, RVS_EnginePreparedDecision *prepared)
{
  MemoryZeroStruct(prepared);

  prepared->command_prepare_result = RVS_Result_Ok;

  for EachNode(emission, RVS_SchedulerEmission, decision->emissions.first) {
    if (emission->kind == RVS_SchedulerEmission_CompleteRequest || emission->kind == RVS_SchedulerEmission_PublishEvent) {
      RVS_EnginePreparedEmission *prepared_emission = push_array(scratch, RVS_EnginePreparedEmission, 1);
      prepared_emission->kind = emission->kind;
      if (emission->kind == RVS_SchedulerEmission_CompleteRequest) {
        rvs_scheduler_prepare_reply_locked(&engine->session->scheduler, emission->operation, &emission->reply);
        prepared_emission->request = emission->operation->request;
        prepared_emission->reply = emission->reply;
        rvs_request_addref(prepared_emission->request);
      } else {
        prepared_emission->event = emission->event;
      }
      SLLQueuePush(prepared->emission_first, prepared->emission_last, prepared_emission);
    }
  }

  RVS_SchedulerCommand *command = &decision->command;
  if (command->kind != RVS_SchedulerCommand_Null) {
    prepared->command_kind = command->kind;
    prepared->command_token = command->token;
  }

  if (prepared->command_kind == RVS_SchedulerCommand_InterruptExecution) {
    prepared->execution_request_id = command->interrupt_execution.execution_request_id;
  } else if (prepared->command_kind == RVS_SchedulerCommand_ResumeTargetSubset && command->resume_target_subset.targets_count != 0) {
    prepared->processes            = push_array(scratch, DMN_Handle, command->resume_target_subset.targets_count);
    prepared->processes_count      = command->resume_target_subset.targets_count;
    prepared->execution_request_id = command->resume_target_subset.execution_request_id;
    for EachIndex(target_idx, command->resume_target_subset.targets_count) {
      RVS_ProgramID target = command->resume_target_subset.targets[target_idx];
      RVS_TargetControl *program = rvs_scheduler_target_from_id_locked(&engine->session->scheduler, target);
      if (program == 0 || program->state == RVS_TargetState_Removed) {
        prepared->command_prepare_result = RVS_Result_Error;
        break;
      }
        RVS_ProcessSnapshot process = {0};
        if (!rvs_entity_live_process_for_program_locked(&engine->session->entities, program->id, &process)) {
          prepared->command_prepare_result = RVS_Result_Error;
          break;
        }
        prepared->processes[target_idx] = rvs_process_id_handle(process.process);
    }
  } else if (prepared->command_kind == RVS_SchedulerCommand_ResumeTargetSubset) {
    prepared->execution_request_id = command->resume_target_subset.execution_request_id;
  } else if (prepared->command_kind == RVS_SchedulerCommand_PublishTarget) {
    prepared->pid = command->publish_target.pid;
    prepared->process = command->publish_target.process;
  }
  if (prepared->command_kind == RVS_SchedulerCommand_ResumeTargetSubset && prepared->command_prepare_result == RVS_Result_Ok) {
    RVS_ScheduledOperation *execution_operation = rvs_scheduler_operation_from_request_id_locked(&engine->session->scheduler,
                                                                                                 command->resume_target_subset.execution_request_id);
    if (execution_operation &&
        !rvs_engine_collect_operation_traps_locked(scratch,
                                                   engine,
                                                   execution_operation,
                                                   command->resume_target_subset.targets,
                                                   command->resume_target_subset.targets_count,
                                                   &prepared->traps)) {
      prepared->command_prepare_result = RVS_Result_Error;
    }
  }
}

internal RVS_SchedulerEvent
rvs_engine_execute_prepared_decision(RVS_Engine *engine, RVS_EnginePreparedDecision *prepared)
{
  rvs_control_assert_unlocked();
  for EachNode(emission, RVS_EnginePreparedEmission, prepared->emission_first) {
#if RVS_ENGINE_TESTING
    if (engine) {
      U32 emission_sequence = ++engine->test_effect_sequence;
      if (engine->test_first_emission_sequence == 0) { engine->test_first_emission_sequence = emission_sequence; }
    }
#endif
    if (emission->kind == RVS_SchedulerEmission_CompleteRequest) {
      rvs_request_complete(emission->request, emission->reply);
      rvs_request_release(emission->request);
    } else if (emission->kind == RVS_SchedulerEmission_PublishEvent) {
      RVS_Result result = rvs_session_push_event(engine->session, &emission->event);
      AssertAlways(result == RVS_Result_Ok || result == RVS_Result_EngineStopped);
    }
  }

  RVS_SchedulerEvent event = {0};
#if RVS_ENGINE_TESTING
  if (engine && prepared->command_kind != RVS_SchedulerCommand_Null) {
    U32 command_sequence = ++engine->test_effect_sequence;
    if (engine->test_first_command_sequence == 0) { engine->test_first_command_sequence = command_sequence; }
  }
#endif
  if (prepared->command_kind == RVS_SchedulerCommand_InterruptExecution) {
    RVS_Result result = rvs_engine_send_demon_message(engine, (RVS_DemonMessage){
      .type = RVS_DemonMessage_InterruptExecution,
      .request_id = prepared->command_token.request_id,
      .interrupt_execution = {
        .execution_request_id = prepared->execution_request_id,
        .command_id = prepared->command_token.command_id,
      },
    });
    if (result != RVS_Result_Ok) {
      event = (RVS_SchedulerEvent){
        .kind = RVS_SchedulerEvent_CommandOutcome,
        .command_outcome = {
          .command_kind = prepared->command_kind,
          .command = prepared->command_token,
          .result = result,
        },
      };
    }
  } else if (prepared->command_kind == RVS_SchedulerCommand_ResumeTargetSubset) {
    RVS_Result result = prepared->command_prepare_result;
    if (result == RVS_Result_Ok) {
      result = rvs_engine_send_demon_message(engine, (RVS_DemonMessage){
        .type = RVS_DemonMessage_Resume,
        .request_id = prepared->command_token.request_id,
        .resume = {
          .processes = prepared->processes,
          .processes_count = prepared->processes_count,
          .execution_request_id = prepared->execution_request_id,
          .command_id = prepared->command_token.command_id,
          .traps = prepared->traps,
        },
      });
    }
    if (result != RVS_Result_Ok) {
      event = (RVS_SchedulerEvent){
        .kind = RVS_SchedulerEvent_CommandOutcome,
        .command_outcome = {
          .command_kind = prepared->command_kind,
          .command = prepared->command_token,
          .result = result,
        },
      };
    }
  } else if (prepared->command_kind == RVS_SchedulerCommand_PumpLaunch) {
    RVS_Result result = rvs_engine_send_demon_message(engine, (RVS_DemonMessage){
      .type = RVS_DemonMessage_Pump,
      .request_id = prepared->command_token.request_id,
      .pump = { .command_id = prepared->command_token.command_id },
    });
    if (result != RVS_Result_Ok) {
      event = (RVS_SchedulerEvent){
        .kind = RVS_SchedulerEvent_CommandOutcome,
        .command_outcome = {
          .command_kind = prepared->command_kind,
          .command = prepared->command_token,
          .result = result,
        },
      };
    }
  } else if (prepared->command_kind == RVS_SchedulerCommand_PublishTarget) {
    event = (RVS_SchedulerEvent){
      .kind = RVS_SchedulerEvent_CommandOutcome,
      .command_outcome = {
        .command_kind = RVS_SchedulerCommand_PublishTarget,
        .command = prepared->command_token,
        .result = RVS_Result_Ok,
        .process = rvs_process_id_from_handle(prepared->process),
        .pid = prepared->pid,
      },
    };
  }
  return event;
}

internal RVS_Result
rvs_control_reduce_scheduler_event(RVS_Session *session, RVS_SchedulerEvent event, Arena *scratch,
                                   B32 reject_if_shutdown, RVS_EnginePreparedDecision *prepared)
{
  RVS_Result result = RVS_Result_EngineStopped;
  rvs_control_mutex_take(session->control);
  if (session->control->is_shutdown && event.kind == RVS_SchedulerEvent_CommandOutcome &&
      event.command_outcome.command_kind == RVS_SchedulerCommand_PublishTarget) {
    event.command_outcome.result = RVS_Result_EngineStopped;
  }
  if (reject_if_shutdown && (session->control->is_shutdown || session->engine_released || session->engine == 0)) {
    MemoryZeroStruct(prepared);
  } else {
    RVS_SchedulerDecision decision = {0};
    RVS_Engine *engine = session->engine;
    AssertAlways(engine != 0);
    rvs_engine_apply_scheduler_event_locked(engine, event, &decision);
    rvs_engine_prepare_scheduler_decision_locked(engine, &decision, scratch, prepared);
    result = decision.result;
    rvs_scheduler_decision_release(&decision);
  }
  rvs_control_mutex_drop(session->control);
  return result;
}

internal void
rvs_engine_drive_scheduler_event(RVS_Engine *engine, RVS_SchedulerEvent event)
{
  enum { immediate_transition_limit = 64 };

  for (U32 transition_idx = 0;; transition_idx += 1) {
    Temp scratch = scratch_begin(0, 0);
    RVS_EnginePreparedDecision prepared = {0};
    (void)rvs_control_reduce_scheduler_event(engine->session, event, scratch.arena, 0, &prepared);

    event = rvs_engine_execute_prepared_decision(engine, &prepared);
    scratch_end(scratch);
    if (event.kind == RVS_SchedulerEvent_Null) { break; }
    if (transition_idx + 1 >= immediate_transition_limit) {
      AssertAlways(event.kind == RVS_SchedulerEvent_CommandOutcome);
      RVS_Result enqueue_result = rvs_engine_send_message(engine, &(RVS_EngineMessage){
        .type = RVS_EngineMessageType_SchedulerEvent,
        .scheduler_event = event,
      });
      AssertAlways(enqueue_result == RVS_Result_Ok || enqueue_result == RVS_Result_EngineStopped);
      break;
    }
  }
}

internal void
rvs_engine_submit_scheduler_failure(RVS_Engine *engine, RVS_MessageID request_id, RVS_Result result)
{
  rvs_engine_drive_scheduler_event(engine, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_OperationFailed,
    .failed = { .request_id = request_id, .result = result },
  });
}

internal RVS_Result
rvs_engine_send_demon_message(RVS_Engine *engine, RVS_DemonMessage message)
{
  rvs_control_assert_unlocked();
  if (ins_atomic_u32_eval(&engine->state) != RVS_ThreadState_Live) { return RVS_Result_EngineStopped; }
#if RVS_ENGINE_TESTING
  if (ins_atomic_u32_eval_cond_assign(&engine->test_fail_demon_message_type,
                                      RVS_DemonMessage_Null,
                                      (U32)message.type) == (U32)message.type) {
    return RVS_Result_Error;
  }
#endif
  RVS_Result result = rvs_demon_send_message(engine->demon, message);
  if (result != RVS_Result_Ok && ins_atomic_u32_eval(&engine->state) != RVS_ThreadState_Live) {
    result = RVS_Result_EngineStopped;
  }
  return result;
}

internal void
rvs_engine_demon_reply_callback(RVS_Demon *demon, RVS_DemonReply *reply, void *ud)
{
  ProfBeginFunction();
  rvs_control_assert_unlocked();
  (void)demon;
  AssertAlways(rvs_engine_send_message(ud, &(RVS_EngineMessage){
    .type = RVS_EngineMessageType_DemonReply,
    .demon_reply = *reply,
  }) == RVS_Result_Ok);
  ProfEnd();
}

////////////////////////////////
// Worker Dispatch

// Command and Output Dispatch

internal void
rvs_engine_execute_root_dispatch(RVS_Engine *engine, RVS_MessageID request_id, RVS_EngineCommand *command)
{
  ProfBeginFunction();
  Temp scratch = scratch_begin(0, 0);
  DMN_Handle *processes = 0;
  U64 processes_count = 0;
  DMN_TrapChunkList traps = {0};
  B32 dispatch_started = 0;
  B32 dispatch_rejected = 0;
  B32 dispatch_ready = 0;
  RVS_EnginePreparedDecision dispatch_prepared = {0};

  rvs_control_mutex_take(engine->control);
  RVS_ScheduledOperation *queued_operation = rvs_scheduler_operation_from_request_id_locked(&engine->session->scheduler, request_id);
  if (rvs_engine_request_mark_dispatched_locked(engine, request_id, command, &dispatch_prepared)) {
    dispatch_started = 1;
    dispatch_ready = 1;
    if (command->kind == RVS_EngineCommandKind_Run) {
      RVS_ProgramID *programs = command->run.programs;
      processes_count = command->run.programs_count;
      processes = push_array(scratch.arena, DMN_Handle, processes_count);
      for EachIndex(program_idx, processes_count) {
        RVS_TargetControl *program = rvs_scheduler_target_from_id_locked(&engine->session->scheduler, programs[program_idx]);
        if (program == 0 || program->state == RVS_TargetState_Removed) {
          dispatch_ready = 0;
          break;
        }
        RVS_ProcessSnapshot process = {0};
        if (!rvs_entity_live_process_for_program_locked(&engine->session->entities, program->id, &process)) {
          dispatch_ready = 0;
          break;
        }
        processes[program_idx] = rvs_process_id_handle(process.process);
      }
      if (dispatch_ready) {
        dispatch_ready = rvs_engine_collect_operation_traps_locked(scratch.arena, engine, queued_operation,
                                                                    programs, processes_count, &traps);
      }
    }
  } else if (queued_operation) {
    dispatch_rejected = 1;
  }
  rvs_control_mutex_drop(engine->control);

#if RVS_ENGINE_TESTING
  if (ins_atomic_u32_eval(&engine->test_hold_after_dispatch_prepare)) {
    ins_atomic_u32_eval_assign(&engine->test_is_held_after_dispatch_prepare, 1);
    while (ins_atomic_u32_eval(&engine->test_hold_after_dispatch_prepare)) { sleep_ms(1); }
    ins_atomic_u32_eval_assign(&engine->test_is_held_after_dispatch_prepare, 0);
  }
#endif

  if (!dispatch_ready) {
    if (dispatch_started) { rvs_engine_submit_scheduler_failure(engine, request_id, RVS_Result_Error); }
    else if (dispatch_rejected) { rvs_engine_submit_scheduler_failure(engine, request_id, RVS_Result_StaleState); }
    scratch_end(scratch);
    ProfEnd();
    return;
  }

  switch (command->kind) {
  case RVS_EngineCommandKind_Launch: {
    RVS_DemonMessage spec = {
      .type       = RVS_DemonMessage_Launch,
      .request_id = request_id,
      .launch     = { .params = command->launch.params },
    };

    RVS_Result result = rvs_engine_send_demon_message(engine, spec);

    if (result != RVS_Result_Ok) {
      rvs_engine_submit_scheduler_failure(engine, request_id, result);
    }
  } break;
  case RVS_EngineCommandKind_Run: {
    RVS_Result result = rvs_engine_send_demon_message(engine, (RVS_DemonMessage){
      .type       = RVS_DemonMessage_Run,
      .request_id = request_id,
      .run = {
        .processes = processes,
        .processes_count = processes_count,
        .traps = traps,
      },
    });
    if (result != RVS_Result_Ok) {
      rvs_engine_submit_scheduler_failure(engine, request_id, result);
    }
  } break;
  case RVS_EngineCommandKind_Interrupt: {
    AssertAlways(dispatch_prepared.command_kind == RVS_SchedulerCommand_InterruptExecution);
    RVS_SchedulerEvent event = rvs_engine_execute_prepared_decision(engine, &dispatch_prepared);
    if (event.kind != RVS_SchedulerEvent_Null) {
      rvs_engine_drive_scheduler_event(engine, event);
    }
  } break;
  default: { InvalidPath; } break;
  }
  scratch_end(scratch);
  ProfEnd();
}

internal void
rvs_engine_process_demon_reply(RVS_Engine *engine, RVS_DemonReply *reply)
{
  ProfBeginFunction();
  switch (reply->kind) {
  case RVS_DemonReplyKind_LaunchStarted: {
    ProfBegin("DEMON Reply: LaunchStarted");

    RVS_SchedulerEvent event = {
      .kind           = RVS_SchedulerEvent_LaunchStarted,
      .launch_started = {
        .request_id = reply->request_id,
        .pid        = reply->launch_started.pid,
      },
    };

    rvs_engine_drive_scheduler_event(engine, event);

    ProfEnd();
  } break;

  case RVS_DemonReplyKind_ActionResult: {
    RVS_DemonAction action = reply->action_result.action;
    RVS_Result result = reply->action_result.result;
    if (action == RVS_DemonAction_Launch) {
      if (result != RVS_Result_Ok) {
        rvs_engine_complete_reply(engine, (RVS_EngineReply){
          .request_id = reply->request_id,
          .result = result,
          .kind = RVS_EngineReplyKind_Launch,
        });
      }
    } else if (action == RVS_DemonAction_Run) {
      rvs_engine_drive_scheduler_event(engine, (RVS_SchedulerEvent){
        .kind = result == RVS_Result_Ok ? RVS_SchedulerEvent_DispatchAccepted : RVS_SchedulerEvent_OperationFailed,
        .failed = { .request_id = reply->request_id, .result = result },
      });
      if (result == RVS_Result_Ok) {
        rvs_engine_complete_reply(engine, (RVS_EngineReply){
          .request_id = reply->request_id,
          .result = result,
          .kind = RVS_EngineReplyKind_Run,
        });
      }
    } else if (action == RVS_DemonAction_Resume) {
      rvs_engine_drive_scheduler_event(engine, (RVS_SchedulerEvent){
        .kind = RVS_SchedulerEvent_CommandOutcome,
        .command_outcome = {
          .command_kind = RVS_SchedulerCommand_ResumeTargetSubset,
          .command = { .request_id = reply->request_id, .command_id = reply->action_result.command_id },
          .result = result,
        },
      });
    } else if (action == RVS_DemonAction_Terminate) {
      // Termination has no scheduler vertical slice yet.
    } else {
      InvalidPath;
    }
  } break;

  case RVS_DemonReplyKind_EventBatch: {
    U64 raw_events_count = 0;
    for EachNode(n, DMN_EventNode, reply->event_batch.events.first) {
      raw_events_count += 1;
    }
    Temp scratch = scratch_begin(0, 0);
    RVS_SchedulerEventDisposition *dispositions = push_array(scratch.arena, RVS_SchedulerEventDisposition, raw_events_count);
    rvs_engine_drive_scheduler_event(engine, (RVS_SchedulerEvent){
      .kind = RVS_SchedulerEvent_DemonEventBatch,
      .demon_events = {
        .source = reply->event_batch.command_id == 0 ? RVS_SchedulerEventBatchSource_Execution : RVS_SchedulerEventBatchSource_PumpLaunch,
        .command = { .request_id = reply->request_id, .command_id = reply->event_batch.command_id },
        .request_id = reply->request_id,
        .events = reply->event_batch.events,
        .dispositions = dispositions,
        .dispositions_count = raw_events_count,
      },
    });
    scratch_end(scratch);
  } break;

  case RVS_DemonReplyKind_ExecutionStopped: {
    rvs_engine_drive_scheduler_event(engine, (RVS_SchedulerEvent){
      .kind = RVS_SchedulerEvent_CommandOutcome,
      .command_outcome = {
        .command_kind = RVS_SchedulerCommand_InterruptExecution,
        .command = { .request_id = reply->request_id, .command_id = reply->execution_stopped.command_id },
        .result = RVS_Result_Ok,
      },
    });
  } break;

  case RVS_DemonReplyKind_ExecutionFinished: {
    rvs_engine_drive_scheduler_event(engine, (RVS_SchedulerEvent){
      .kind = RVS_SchedulerEvent_RunFinished,
      .request = { .request_id = reply->request_id },
    });
  } break;

  default: { InvalidPath; } break;
  }
  ProfEnd();
}

internal void
rvs_engine_worker(void *user_data)
{
  ProfBeginFunction();
  RVS_Engine *engine = user_data;

  for (;;) {
    RVS_EngineMessage *message = rvs_queue_pop_struct(engine->inbox_queue, RVS_EngineMessage, max_U64);
    if (message == 0) { continue; }

    B32 should_exit = 0;
    switch (message->type) {
    case RVS_EngineMessageType_DispatchRequest: {
#if RVS_ENGINE_TESTING
      if (ins_atomic_u32_eval(&engine->test_hold_before_dispatch)) {
        ins_atomic_u32_eval_assign(&engine->test_is_held_before_dispatch, 1);
        while (ins_atomic_u32_eval(&engine->test_hold_before_dispatch)) { sleep_ms(1); }
        ins_atomic_u32_eval_assign(&engine->test_is_held_before_dispatch, 0);
      }
#endif
      rvs_engine_execute_root_dispatch(engine, message->request_id, &message->command);
    } break;

    case RVS_EngineMessageType_DemonReply: {
      rvs_engine_process_demon_reply(engine, &message->demon_reply);
    } break;

    case RVS_EngineMessageType_SchedulerEvent: {
      rvs_engine_drive_scheduler_event(engine, message->scheduler_event);
    } break;

    case RVS_EngineMessageType_Shutdown: {
      should_exit = 1;
    } break;

    default: { InvalidPath; } break;
    }

    rvs_queue_recycle(engine->inbox_queue, &message->base);

    if (should_exit) {
      break;
    }
  }

  ins_atomic_u32_eval_assign(&engine->state, RVS_ThreadState_Exited);
  ProfEnd();
}

////////////////////////////////
// API

RVS_Result
rvs_engine_init(RVS_Engine **engine_out)
{
  ProfBeginFunction();
  RVS_Result result = RVS_Result_Error;

  MemoryZeroStruct(engine_out);

  if (engine_out == 0) {
    goto exit;
  }

  static RVS_Engine engine = {0};

  if (ins_atomic_u32_eval_cond_assign(&engine.state, RVS_ThreadState_Initing, RVS_ThreadState_Null) != RVS_ThreadState_Null) {
    goto exit;
  }


  engine.arena              = arena_alloc(.name = "Engine");
  engine.inbox_queue        = rvs_queue_alloc(sizeof(RVS_EngineMessage), AlignOf(RVS_EngineMessage));
  engine.control            = rvs_engine_control_alloc();
  engine.request_pool       = rvs_request_pool_alloc();

  result = rvs_demon_init(&engine, rvs_engine_demon_reply_callback, &engine.demon);
  if (result != RVS_Result_Ok) {
    ins_atomic_u32_eval_assign(&engine.state, RVS_ThreadState_Exited);
    goto exit;
  }

  engine.thread = thread_launch(rvs_engine_worker, &engine);
  if (MemoryIsZeroStruct(&engine.thread)) {
    ins_atomic_u32_eval_assign(&engine.state, RVS_ThreadState_Exited);
    result = RVS_Result_Error;
    goto exit;
  }

  ins_atomic_u32_eval_assign(&engine.state, RVS_ThreadState_Live);
  *engine_out = &engine;
  result = RVS_Result_Ok;

  exit:;
  ProfEnd();
  return result;
}

RVS_Result
rvs_engine_create_session(RVS_Engine *engine, RVS_Session **session_out)
{
  RVS_Result result = RVS_Result_Error;

  MemoryZeroStruct(session_out);

  if (session_out) { *session_out = 0; }
  if (engine == 0 || session_out == 0) {
    return result;
  }


  rvs_control_mutex_take(engine->control);
  if (engine->control->is_shutdown || ins_atomic_u32_eval(&engine->state) != RVS_ThreadState_Live) {
    result = RVS_Result_EngineStopped;
  } else if (engine->session) {
    result = RVS_Result_Unsupported;
  } else {
    engine->session = rvs_session_alloc(engine);
    *session_out = engine->session;
    result = RVS_Result_Ok;
  }
  rvs_control_mutex_drop(engine->control);
  return result;
}

RVS_Result
rvs_engine_shutdown(RVS_Engine *engine)
{
  ProfBeginFunction();

  RVS_Result result = RVS_Result_Ok;

  if (ins_atomic_u32_eval_cond_assign(&engine->state, RVS_ThreadState_Terminating, RVS_ThreadState_Live) != RVS_ThreadState_Live) {
    result = RVS_Result_EngineStopped;
    goto exit;
  }

  rvs_control_mutex_take(engine->control);
  engine->control->is_shutdown = 1;

#if RVS_ENGINE_TESTING
  ins_atomic_u32_eval_assign(&engine->test_hold_before_dispatch, 0);
  ins_atomic_u32_eval_assign(&engine->test_hold_after_dispatch_prepare, 0);
#endif

  RVS_Session *session = engine->session;
  rvs_control_mutex_drop(engine->control);
  if (session) { rvs_session_close_events(session); }

  // shutdown the DEMON thread
  AssertAlways(rvs_demon_shutdown(engine->demon) == RVS_Result_Ok);

  // shutdown the engine thread
  RVS_EngineMessage shutdown = { .type = RVS_EngineMessageType_Shutdown };
  AssertAlways(rvs_engine_send_message(engine, &shutdown) == RVS_Result_Ok);
  thread_join(engine->thread, max_U64);
  rvs_demon_release_resources(engine->demon);

  // Release engine ownership; retained requests keep their immutable terminal replies.
  if (engine->session) {
    rvs_engine_release_active_requests(engine, RVS_Result_EngineStopped);
  }
  rvs_request_pool_release_engine(engine->request_pool);

  // release engine thread resources
  rvs_queue_release(engine->inbox_queue);
  if (engine->session) {
    rvs_control_mutex_take(engine->control);
    rvs_session_release_engine(engine->session);
    engine->session = 0;
    rvs_control_mutex_drop(engine->control);
  }
  rvs_engine_control_release(engine->control);
  arena_release(engine->arena);
  MemoryZeroStruct(engine);

  exit:;
  ProfEnd();
  return result;
}

////////////////////////////////
// Enum

internal String8
rvs_string_from_command_kind(RVS_EngineCommandKind v)
{
  switch (v) {
#define X(id, ...) case RVS_EngineCommandKind_##id: return str8_lit(Stringify(id));
  RVS_ENGINE_COMMAND_XLIST
#undef X
  default: break;
  }
  return str8_zero();
}

internal String8
rvs_help_from_command_kind(RVS_EngineCommandKind v)
{
  switch (v) {
  case RVS_EngineCommandKind_Null: break;
#define X(id, help, ...) case RVS_EngineCommandKind_##id: return str8_lit(help);
  RVS_ENGINE_COMMAND_XLIST
#undef X
  }
  return str8_zero();
}

internal RVS_EngineCommandKind
rvs_command_kind_from_string(String8 v)
{
#define X(id, ...) if (str8_matchi(str8_lit(Stringify(id)), v)) return RVS_EngineCommandKind_##id;
  RVS_ENGINE_COMMAND_XLIST
#undef X
  return RVS_EngineCommandKind_Null;
}
