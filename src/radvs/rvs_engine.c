#include "radvs/rvs_engine.h"
#include "radvs/rvs_async.h"
#include "radvs/rvs_demon.h"
#include "radvs/rvs_request.h"
#include "radvs/rvs_scheduler.h"

////////////////////////////////
// Types

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
  RVS_EngineMessageType_Shutdown,
} RVS_EngineMessageType;

typedef struct
{
  RVS_QueueNode          base;
  RVS_EngineMessageType  type;
  union {
    struct {
      RVS_EngineCommand command;
      RVS_MessageID     request_id;
    };
    struct {
      RVS_Demon      *source;
      RVS_DemonReply *reply;
      ArenaNode      *arena_node;
    } demon_reply;
  };
} RVS_EngineMessage;

internal B32
rvs_engine_command_scheduler_op(RVS_EngineCommand command, RVS_SchedulerOp *op_out)
{
  switch (command.kind) {
  case RVS_EngineCommandKind_Launch: {
    *op_out = RVS_SchedulerOp_Launch;
    return 1;
  } break;
  case RVS_EngineCommandKind_Run:
  case RVS_EngineCommandKind_Interrupt: {
    RVS_ProgramID *programs = command.kind == RVS_EngineCommandKind_Run ? command.run.programs : command.interrupt.programs;
    U64 programs_count = command.kind == RVS_EngineCommandKind_Run ? command.run.programs_count : command.interrupt.programs_count;
    if (programs_count == 0 || programs == 0) {
      return 0;
    }
    for EachIndex(program_idx, programs_count) {
      if (dmn_handle_match(programs[program_idx], dmn_handle_zero())) {
        return 0;
      }
      for EachIndex(previous_idx, program_idx) {
        if (dmn_handle_match(programs[previous_idx], programs[program_idx])) {
          return 0;
        }
      }
    }
    *op_out = command.kind == RVS_EngineCommandKind_Run ? RVS_SchedulerOp_Run : RVS_SchedulerOp_Interrupt;
    return 1;
  } break;
  default: break;
  }
  return 0;
}

struct RVS_EngineControl
{
  Arena *arena;
  Mutex  mutex;
  U32    ref_count;
  B32    is_shutdown;
};

struct RVS_Engine
{
  Arena          *arena;
  Mutex           arena_mutex;
  RVS_Queue      *inbox_queue;
  RVS_MessageID   next_request_id;
  RVS_ThreadState state;
  Thread          thread;

  RVS_EngineControl *control;
  RVS_RequestPool   *request_pool;
  RVS_Session       *session;

  RVS_Demon *demon;
  Arena     *demon_reply_arena;
  Mutex      demon_reply_mutex;
  ArenaNode *demon_reply_arena_active_list;
  ArenaNode *demon_reply_arena_free_list;

#if RVS_ENGINE_TESTING
  U32 test_fail_command_enqueue;
  U32 test_hold_before_dispatch;
  U32 test_is_held_before_dispatch;
  U32 test_fail_demon_message_type;
  U32 test_scheduler_effect_execution_depth;
  U32 test_scheduler_effect_execution_max_depth;
#endif
};

#include "radvs/rvs_session.h"

////////////////////////////////
// Scheduler Adapter

internal void
rvs_engine_apply_scheduler_event_locked(RVS_Engine *engine, RVS_SchedulerEvent event, RVS_SchedulerEffectList *effects_out)
{
  rvs_scheduler_apply_locked(&engine->session->scheduler, event, effects_out);
}
internal void
rvs_engine_apply_scheduler_event(RVS_Engine *engine, RVS_SchedulerEvent event, RVS_SchedulerEffectList *effects_out)
{
  mutex_take(engine->control->mutex);
  rvs_engine_apply_scheduler_event_locked(engine, event, effects_out);
  mutex_drop(engine->control->mutex);
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
  } break;
  case RVS_EngineMessageType_Shutdown: {
  } break;
  default: { InvalidPath; } break;
  }
  ProfEnd();
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
  RVS_EngineMessage *message = rvs_queue_alloc_struct(engine->inbox_queue, RVS_EngineMessage);
  rvs_engine_message_copy(engine->arena, message, spec);
  RVS_Result result = rvs_queue_push(engine->inbox_queue, &message->base);
  ProfEnd();
  return result;
}

internal RVS_Result
rvs_engine_send_message(RVS_Engine *engine, RVS_EngineMessage *spec)
{
  ProfBeginFunction();
  mutex_take(engine->arena_mutex);
  RVS_Result result = rvs_engine_send_message_locked(engine, spec);
  mutex_drop(engine->arena_mutex);
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
#include "radvs/rvs_scheduler.c"
#include "radvs/rvs_session.c"

internal B32
rvs_engine_request_mark_dispatched(RVS_Engine *engine, RVS_MessageID request_id, RVS_EngineCommand *command)
{
  ProfBeginFunction();
  B32 result = 0;
  mutex_take(engine->control->mutex);
  RVS_Session *session = engine->session;
  if ( ! engine->control->is_shutdown) {
    RVS_SchedulerOp command_op = RVS_SchedulerOp_Null;
    RVS_ScheduledOperation *operation = rvs_scheduler_find_active_operation_locked(&session->scheduler, request_id);
    if (operation && rvs_engine_command_scheduler_op(*command, &command_op) && operation->key.op == command_op) {
      RVS_SchedulerEffectList effects = {0};
      rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
        .kind = RVS_SchedulerEvent_DispatchStarted,
        .request = { .request_id = request_id },
      }, &effects);
      result = effects.first == 0;
      rvs_scheduler_effect_list_release(&effects);
    }
  }
  mutex_drop(engine->control->mutex);
  ProfEnd();
  return result;
}

internal void rvs_engine_execute_scheduler_effects(RVS_Engine *engine, RVS_SchedulerEffectList *effects);
internal RVS_Result rvs_engine_send_demon_message(RVS_Engine *engine, RVS_DemonMessage message);

typedef enum
{
  RVS_EngineSchedulerFeedbackKind_Null,
  RVS_EngineSchedulerFeedbackKind_Event,
  RVS_EngineSchedulerFeedbackKind_PublishTarget,
} RVS_EngineSchedulerFeedbackKind;

typedef struct RVS_EngineSchedulerFeedback RVS_EngineSchedulerFeedback;
struct RVS_EngineSchedulerFeedback
{
  RVS_EngineSchedulerFeedback     *next;
  RVS_EngineSchedulerFeedbackKind  kind;
  RVS_SchedulerEvent               event;
  RVS_MessageID                    request_id;
  U32                              pid;
  DMN_Handle                       process;
};

typedef struct
{
  RVS_EngineSchedulerFeedback *first;
  RVS_EngineSchedulerFeedback *last;
} RVS_EngineSchedulerFeedbackList;

internal RVS_EngineSchedulerFeedback *
rvs_engine_scheduler_feedback_push(Arena *arena, RVS_EngineSchedulerFeedbackList *list, RVS_EngineSchedulerFeedbackKind kind)
{
  RVS_EngineSchedulerFeedback *feedback = push_array(arena, RVS_EngineSchedulerFeedback, 1);
  feedback->kind = kind;
  SLLQueuePush(list->first, list->last, feedback);
  return feedback;
}

internal void
rvs_engine_release_active_requests(RVS_Engine *engine, RVS_Result pending_result)
{
  ProfBeginFunction();
  AssertAlways(pending_result == RVS_Result_EngineStopped);
  RVS_SchedulerEffectList effects = {0};
  rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){ .kind = RVS_SchedulerEvent_Shutdown }, &effects);
  rvs_engine_execute_scheduler_effects(engine, &effects);
  ProfEnd();
}

// Completion

internal void rvs_engine_execute_scheduler_effects(RVS_Engine *engine, RVS_SchedulerEffectList *effects);

internal void
rvs_engine_complete_reply(RVS_Engine *engine, RVS_EngineReply reply)
{
  ProfBeginFunction();
  RVS_SchedulerEffectList effects = {0};
  rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_BackendCompleted,
    .completed = { .reply = reply },
  }, &effects);
  rvs_engine_execute_scheduler_effects(engine, &effects);
  ProfEnd();
}

internal void
rvs_engine_submit_launch_started(RVS_Engine *engine, RVS_MessageID request_id, U32 pid)
{
  ProfBeginFunction();
  RVS_SchedulerEffectList effects = {0};
  rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_LaunchStarted,
    .launch_started = { .request_id = request_id, .pid = pid },
  }, &effects);
  rvs_engine_execute_scheduler_effects(engine, &effects);
  ProfEnd();
}

internal void
rvs_engine_execute_scheduler_effects(RVS_Engine *engine, RVS_SchedulerEffectList *effects)
{
#if RVS_ENGINE_TESTING
  engine->test_scheduler_effect_execution_depth += 1;
  engine->test_scheduler_effect_execution_max_depth = Max(engine->test_scheduler_effect_execution_max_depth,
                                                          engine->test_scheduler_effect_execution_depth);
#endif
  Temp scratch = scratch_begin(0, 0);
  RVS_EngineSchedulerFeedbackList feedbacks = {0};
  RVS_SchedulerEffectList current_effects = *effects;
  MemoryZeroStruct(effects);
  for (;;) {
    for EachNode(effect, RVS_SchedulerEffect, current_effects.first) {
      if (effect->kind == RVS_SchedulerEffect_ResumeTargetSubset) {
        RVS_Result result = RVS_Result_Ok;
        DMN_Handle *processes = 0;
        if (effect->targets_count != 0) {
          processes = push_array(scratch.arena, DMN_Handle, effect->targets_count);
          mutex_take(engine->control->mutex);
          if ( ! rvs_session_programs_to_processes(engine->session, effect->targets, effect->targets_count, processes)) {
            result = RVS_Result_Error;
          }
          mutex_drop(engine->control->mutex);
        }
        if (result == RVS_Result_Ok) {
          result = rvs_engine_send_demon_message(engine, (RVS_DemonMessage){
            .type = RVS_DemonMessage_Resume,
            .request_id = effect->operation->request->request_id,
            .resume = {
              .processes = processes,
              .processes_count = effect->targets_count,
              .execution_request_id = effect->execution_request_id,
            },
          });
        }
        if (result != RVS_Result_Ok) {
          RVS_EngineSchedulerFeedback *feedback = rvs_engine_scheduler_feedback_push(scratch.arena, &feedbacks, RVS_EngineSchedulerFeedbackKind_Event);
          feedback->event = (RVS_SchedulerEvent){
            .kind = RVS_SchedulerEvent_OperationFailed,
            .request = { .request_id = effect->operation->request->request_id },
          };
        }
      } else if (effect->kind == RVS_SchedulerEffect_PumpLaunch) {
        RVS_Result result = rvs_engine_send_demon_message(engine, (RVS_DemonMessage){
          .type       = RVS_DemonMessage_Pump,
          .request_id = effect->operation->request->request_id,
        });
        if (result != RVS_Result_Ok) {
          RVS_EngineSchedulerFeedback *feedback = rvs_engine_scheduler_feedback_push(scratch.arena, &feedbacks, RVS_EngineSchedulerFeedbackKind_Event);
          feedback->event = (RVS_SchedulerEvent){
            .kind = RVS_SchedulerEvent_LaunchPumpFailed,
            .request = { .request_id = effect->operation->request->request_id },
          };
        }
      } else if (effect->kind == RVS_SchedulerEffect_RetireTarget) {
        mutex_take(engine->control->mutex);
        rvs_session_program_retire_locked(engine->session, effect->process, effect->exit_code);
        mutex_drop(engine->control->mutex);
      } else if (effect->kind == RVS_SchedulerEffect_CompleteRequest) {
        RVS_Request *request = effect->operation->request;
        RVS_EngineReply reply = effect->reply;
        mutex_take(engine->control->mutex);
        rvs_session_prepare_reply_locked(engine->session, effect->operation, &reply);
        mutex_drop(engine->control->mutex);
        rvs_request_complete(request, reply);
      } else if (effect->kind == RVS_SchedulerEffect_PublishTarget) {
        RVS_EngineSchedulerFeedback *feedback = rvs_engine_scheduler_feedback_push(scratch.arena, &feedbacks, RVS_EngineSchedulerFeedbackKind_PublishTarget);
        feedback->request_id = effect->operation->request->request_id;
        feedback->pid = effect->pid;
        feedback->process = effect->process;
      }
    }
    rvs_scheduler_effect_list_release(&current_effects);

    // Finish the current transition's effects before reducing any feedback they produced.
    RVS_EngineSchedulerFeedback *feedback = feedbacks.first;
    if (feedback == 0) { break; }
    SLLQueuePop(feedbacks.first, feedbacks.last);
    if (feedback->kind == RVS_EngineSchedulerFeedbackKind_Event) {
      rvs_engine_apply_scheduler_event(engine, feedback->event, &current_effects);
    } else if (feedback->kind == RVS_EngineSchedulerFeedbackKind_PublishTarget) {
      mutex_take(engine->control->mutex);
      RVS_ScheduledOperation *operation = rvs_scheduler_find_active_operation_locked(&engine->session->scheduler, feedback->request_id);
      if (operation == 0 || operation->key.op != RVS_SchedulerOp_Launch ||
          operation->launch_phase != RVS_LaunchPhase_AwaitTargetRegistration ||
          operation->launch_pid != feedback->pid ||
          !dmn_handle_match(operation->launch_process, feedback->process) ||
          rvs_scheduler_target_from_id_locked(&engine->session->scheduler, feedback->process) != 0 ||
          rvs_session_program_from_id_locked(engine->session, feedback->process) != 0) {
        rvs_engine_apply_scheduler_event_locked(engine, (RVS_SchedulerEvent){
          .kind = RVS_SchedulerEvent_OperationFailed,
          .request = { .request_id = feedback->request_id },
        }, &current_effects);
      } else {
        RVS_Program *program = rvs_session_program_add_locked(engine->session, feedback->pid, feedback->process);
        rvs_engine_apply_scheduler_event_locked(engine, (RVS_SchedulerEvent){
          .kind = RVS_SchedulerEvent_TargetRegistered,
          .target = {
            .request_id = feedback->request_id,
            .target = program->id,
            .pid = program->pid,
          },
        }, &current_effects);
      }
      mutex_drop(engine->control->mutex);
    }
  }
  scratch_end(scratch);
#if RVS_ENGINE_TESTING
  engine->test_scheduler_effect_execution_depth -= 1;
#endif
}

internal void
rvs_engine_submit_scheduler_failure(RVS_Engine *engine, RVS_MessageID request_id)
{
  RVS_SchedulerEffectList effects = {0};
  rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_OperationFailed,
    .request = { .request_id = request_id },
  }, &effects);
  rvs_engine_execute_scheduler_effects(engine, &effects);
}

internal RVS_Result
rvs_engine_send_demon_message(RVS_Engine *engine, RVS_DemonMessage message)
{
#if RVS_ENGINE_TESTING
  if (ins_atomic_u32_eval_cond_assign(&engine->test_fail_demon_message_type,
                                      RVS_DemonMessage_Null,
                                      (U32)message.type) == (U32)message.type) {
    return RVS_Result_Error;
  }
#endif
  return rvs_demon_send_message(engine->demon, message);
}

////////////////////////////////
// DEMON Output

internal void
rvs_engine_recycle_demon_reply_arena(RVS_Engine *engine, ArenaNode *arena_node)
{
  ProfBeginFunction();
  mutex_take(engine->demon_reply_mutex);

  // TODO: replace with a hash map
  ArenaNode **node_ptr = &engine->demon_reply_arena_active_list;
  while (*node_ptr && *node_ptr != arena_node) {
    node_ptr = &(*node_ptr)->next;
  }
  AssertAlways(*node_ptr == arena_node);
  *node_ptr = arena_node->next;

  arena_clear(arena_node->v);
  SLLStackPush(engine->demon_reply_arena_free_list, arena_node);

  mutex_drop(engine->demon_reply_mutex);
  ProfEnd();
}

internal RVS_Result
rvs_engine_push_demon_reply(RVS_Engine *engine, RVS_Demon *source, RVS_DemonReply *reply)
{
  ProfBeginFunction();
  mutex_take(engine->demon_reply_mutex);

  ArenaNode *arena_node = engine->demon_reply_arena_free_list;
  if (arena_node) {
    SLLStackPop(engine->demon_reply_arena_free_list);
  } else {
    arena_node    = push_array(engine->demon_reply_arena, ArenaNode, 1);
    arena_node->v = arena_alloc(.name = "Engine DEMON Reply");
  }
  arena_clear(arena_node->v);

  RVS_DemonReply *reply_copy = push_array(arena_node->v, RVS_DemonReply, 1);
  rvs_demon_reply_copy(arena_node->v, reply_copy, reply);
  SLLStackPush(engine->demon_reply_arena_active_list, arena_node);

  RVS_EngineMessage message = {
    .type = RVS_EngineMessageType_DemonReply,
    .demon_reply = {
      .source     = source,
      .reply      = reply_copy,
      .arena_node = arena_node,
    },
  };
  RVS_Result result = rvs_engine_send_message(engine, &message);

  // on failure, put resources on the free lists
  if (result != RVS_Result_Ok) {
    AssertAlways(engine->demon_reply_arena_active_list == arena_node);
    SLLStackPop(engine->demon_reply_arena_active_list);
    arena_clear(arena_node->v);
    SLLStackPush(engine->demon_reply_arena_free_list, arena_node);
  }

  mutex_drop(engine->demon_reply_mutex);
  ProfEnd();
  return result;
}

internal void
rvs_engine_demon_reply_callback(RVS_Demon *demon, RVS_DemonReply *reply, void *ud)
{
  ProfBeginFunction();
  AssertAlways(rvs_engine_push_demon_reply(ud, demon, reply) == RVS_Result_Ok);
  ProfEnd();
}

////////////////////////////////
// Worker Dispatch

// Command and Output Dispatch

internal void
rvs_engine_execute_dispatch_request(RVS_Engine *engine, RVS_MessageID request_id, RVS_EngineCommand *command)
{
  ProfBeginFunction();
#if RVS_ENGINE_TESTING
  if (ins_atomic_u32_eval(&engine->test_hold_before_dispatch)) {
    ins_atomic_u32_eval_assign(&engine->test_is_held_before_dispatch, 1);
    while (ins_atomic_u32_eval(&engine->test_hold_before_dispatch)) {
      sleep_ms(1);
    }
    ins_atomic_u32_eval_assign(&engine->test_is_held_before_dispatch, 0);
  }
#endif
  if ( ! rvs_engine_request_mark_dispatched(engine, request_id, command)) {
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
      rvs_engine_submit_scheduler_failure(engine, request_id);
    }
  } break;
  case RVS_EngineCommandKind_Run: {
    RVS_Session *session = engine->session;
    Temp scratch = scratch_begin(0, 0);
    DMN_Handle *processes = push_array(scratch.arena, DMN_Handle, command->run.programs_count);
    B32 all_programs_found = rvs_session_programs_to_processes(session, command->run.programs, command->run.programs_count, processes);

    RVS_Result result = RVS_Result_Error;
    if (all_programs_found) {
      result = rvs_engine_send_demon_message(engine, (RVS_DemonMessage){
        .type       = RVS_DemonMessage_Run,
        .request_id = request_id,
        .run = {
          .processes = processes,
          .processes_count = command->run.programs_count,
        },
      });
    }
    if (result != RVS_Result_Ok) {
      rvs_engine_submit_scheduler_failure(engine, request_id);
    }
    scratch_end(scratch);
  } break;
  case RVS_EngineCommandKind_Interrupt: {
    RVS_Session *session = engine->session;
    Temp scratch = scratch_begin(0, 0);
    DMN_Handle *processes = push_array(scratch.arena, DMN_Handle, command->interrupt.programs_count);
    RVS_Result result = RVS_Result_Error;
    if (rvs_session_programs_to_processes(session, command->interrupt.programs, command->interrupt.programs_count, processes)) {
      result = rvs_demon_interrupt(engine->demon, request_id, processes, command->interrupt.programs_count);
    }
    if (result != RVS_Result_Ok) {
      rvs_engine_submit_scheduler_failure(engine, request_id);
    }
    scratch_end(scratch);
  } break;
  default: { InvalidPath; } break;
  }
  ProfEnd();
}

internal void
rvs_engine_process_demon_reply(RVS_Engine *engine, RVS_DemonReply *reply)
{
  ProfBeginFunction();
  switch (reply->kind) {
  case RVS_DemonReplyKind_LaunchStarted: {
    rvs_engine_submit_launch_started(engine, reply->request_id, reply->launch_started.pid);
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
      RVS_SchedulerEffectList effects = {0};
      rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
        .kind = result == RVS_Result_Ok ? RVS_SchedulerEvent_DispatchAccepted : RVS_SchedulerEvent_OperationFailed,
        .request = { .request_id = reply->request_id },
      }, &effects);
      if (result == RVS_Result_Ok) {
        rvs_engine_complete_reply(engine, (RVS_EngineReply){
          .request_id = reply->request_id,
          .result = result,
          .kind = RVS_EngineReplyKind_Run,
        });
      }
      rvs_engine_execute_scheduler_effects(engine, &effects);
    } else if (action == RVS_DemonAction_Resume) {
      RVS_SchedulerEffectList effects = {0};
      rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
        .kind = result == RVS_Result_Ok ? RVS_SchedulerEvent_ResumeAccepted : RVS_SchedulerEvent_OperationFailed,
        .request = { .request_id = reply->request_id },
      }, &effects);
      rvs_engine_execute_scheduler_effects(engine, &effects);
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
    RVS_SchedulerEffectList effects = {0};
    rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
      .kind = RVS_SchedulerEvent_DemonEventBatch,
      .demon_events = {
        .request_id = reply->request_id,
        .events = reply->event_batch.events,
        .dispositions = dispositions,
        .dispositions_count = raw_events_count,
      },
    }, &effects);
    rvs_engine_execute_scheduler_effects(engine, &effects);
    U64 raw_event_idx = 0;
    for EachNode(n, DMN_EventNode, reply->event_batch.events.first) {
      if (dispositions[raw_event_idx++] == RVS_SchedulerEventDisposition_Suppress) { continue; }
      RVS_Result push_result = rvs_session_push_event(engine->session, &n->v);
      AssertAlways(push_result == RVS_Result_Ok || push_result == RVS_Result_EngineStopped);
    }
    scratch_end(scratch);
  } break;

  case RVS_DemonReplyKind_ExecutionFinished: {
    RVS_SchedulerEffectList effects = {0};
    rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
      .kind = RVS_SchedulerEvent_RunFinished,
      .request = { .request_id = reply->request_id },
    }, &effects);
    rvs_engine_execute_scheduler_effects(engine, &effects);
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
      rvs_engine_execute_dispatch_request(engine, message->request_id, &message->command);
    } break;

    case RVS_EngineMessageType_DemonReply: {
      rvs_engine_process_demon_reply(engine, message->demon_reply.reply);
      rvs_engine_recycle_demon_reply_arena(engine, message->demon_reply.arena_node);
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
  static RVS_Engine engine = {0};
  if (engine_out == 0) {
    goto exit;
  }
  if (ins_atomic_u32_eval_cond_assign(&engine.state, RVS_ThreadState_Initing, RVS_ThreadState_Null) != RVS_ThreadState_Null) {
    goto exit;
  }

  engine.arena              = arena_alloc(.name = "Engine");
  engine.inbox_queue        = rvs_queue_alloc(engine.arena, sizeof(RVS_EngineMessage), AlignOf(RVS_EngineMessage));
  engine.arena_mutex        = mutex_alloc();
  engine.control            = rvs_engine_control_alloc();
  engine.request_pool       = rvs_request_pool_alloc();
  engine.demon_reply_arena = arena_alloc(.name = "Engine DEMON Reply Nodes");
  engine.demon_reply_mutex = mutex_alloc();

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
  if (session_out) { *session_out = 0; }
  if (engine == 0 || session_out == 0) {
    return result;
  }
  mutex_take(engine->control->mutex);
  if (engine->control->is_shutdown || ins_atomic_u32_eval(&engine->state) != RVS_ThreadState_Live) {
    result = RVS_Result_EngineStopped;
  } else if (engine->session) {
    result = RVS_Result_Unsupported;
  } else {
    engine->session = rvs_session_alloc(engine);
    *session_out = engine->session;
    result = RVS_Result_Ok;
  }
  mutex_drop(engine->control->mutex);
  return result;
}

void
rvs_engine_shutdown(RVS_Engine *engine)
{
  ProfBeginFunction();
  if (ins_atomic_u32_eval_cond_assign(&engine->state, RVS_ThreadState_Terminating, RVS_ThreadState_Live) != RVS_ThreadState_Live) {
    goto exit;
  }

  mutex_take(engine->control->mutex);
  engine->control->is_shutdown = 1;
#if RVS_ENGINE_TESTING
  ins_atomic_u32_eval_assign(&engine->test_hold_before_dispatch, 0);
#endif
  if (engine->session) {
    rvs_session_close_events_locked(engine->session);
  }
  mutex_drop(engine->control->mutex);

  // shutdown the DEMON thread
  AssertAlways(rvs_demon_shutdown(engine->demon) == RVS_Result_Ok);

  // shutdown the engine thread
  RVS_EngineMessage shutdown = { .type = RVS_EngineMessageType_Shutdown };
  AssertAlways(rvs_engine_send_message(engine, &shutdown) == RVS_Result_Ok);
  thread_join(engine->thread, max_U64);

  // Release engine ownership; retained requests keep their immutable terminal replies.
  if (engine->session) {
    rvs_engine_release_active_requests(engine, RVS_Result_EngineStopped);
  }
  rvs_request_pool_release_engine(engine->request_pool);

  // release arenas for DEMON outputs after both workers have drained them
  mutex_take(engine->demon_reply_mutex);
  AssertAlways(engine->demon_reply_arena_active_list == 0);
  for EachNode(n, ArenaNode, engine->demon_reply_arena_free_list) { arena_release(n->v); }
  engine->demon_reply_arena_free_list = 0;
  mutex_drop(engine->demon_reply_mutex);

  // release engine thread resources
  rvs_queue_release(engine->inbox_queue);
  if (engine->session) {
    mutex_take(engine->control->mutex);
    rvs_session_release_engine(engine->session);
    engine->session = 0;
    mutex_drop(engine->control->mutex);
  }
  rvs_engine_control_release(engine->control);
  mutex_release(engine->arena_mutex);
  mutex_release(engine->demon_reply_mutex);
  arena_release(engine->demon_reply_arena);
  arena_release(engine->arena);
  MemoryZeroStruct(engine);

  exit:;
  ProfEnd();
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
