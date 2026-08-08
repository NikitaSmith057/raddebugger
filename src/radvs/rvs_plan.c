// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////

#include "rvs_plan.h"

////////////////////////////////

internal RVS_SchedulerEffect *
rvs_emit_effect_send_backend_message(Arena *arena, RVS_DemonMessage message)
{
  RVS_SchedulerEffect *effect = push_array(arena, RVS_SchedulerEffect, 1);
  effect->kind            = RVS_SchedulerEffectKind_SendBackendMessage;
  effect->backend_message = message;
  return effect;
}

internal RVS_SchedulerEffect *
rvs_emit_effect_complete_command(Arena *arena, RVS_CommandReply command_reply)
{
  RVS_SchedulerEffect *effect = push_array(arena, RVS_SchedulerEffect, 1);
  effect->kind          = RVS_SchedulerEffectKind_CompleteCommand;
  effect->command_reply = command_reply;
  return effect;
}

internal RVS_SchedulerEffect *
rvs_emit_effect_create_program(Arena *arena, RVS_ProgramID program, DMN_Event event)
{
  RVS_SchedulerEffect *effect = push_array(arena, RVS_SchedulerEffect, 1);
  effect->kind = RVS_SchedulerEffectKind_CreateProgram;
  effect->create_program.program        = program;
  effect->create_program.process        = (RVS_ProcessID){ .handle = event.process };
  effect->create_program.parent_process = (RVS_ProcessID){ .handle = event.parent_process };
  effect->create_program.pid            = event.system_process_id;
  return effect;
}

internal RVS_SchedulerEffect *
rvs_emit_effect_complete_operation(Arena *arena)
{
  RVS_SchedulerEffect *effect = push_array(arena, RVS_SchedulerEffect, 1);
  effect->kind = RVS_SchedulerEffectKind_OperationCompleted;
  return effect;
}

////////////////////////////////

RVS_PLAN_FUNC(rvs_launch_suspended_plan)
{
  enum {
    RVS_LaunchState_LaunchAck = RVS_PlanState_UserLo,
    RVS_LaunchState_PendingSendAck,
    RVS_LaunchState_AwaitLaunchAck,
    RVS_LaunchState_AwaitCreateProcess,
    RVS_LaunchState_PendingProgram,
  };

  typedef struct {
    U32 pid;
  } RVS_LaunchSuspendedState;

  RVS_PlanResult result = { .status = RVS_PlanStatus_Unhandled };

  switch (state) {
  //
  // 1. initialize program launch
  //
  case RVS_PlanState_Begin: {
    AssertAlways(message->kind == RVS_SchedulerMessageKind_Command);
    AssertAlways(operation->command.kind == RVS_CommandKind_Launch);

    // 1a. allocate temp state for the PID
    plan->ud = push_array(arena, RVS_LaunchSuspendedState, 1);

    // 1b. request backend to launch the process
    RVS_DemonMessage message = {
      .type          = RVS_DemonMessage_Launch,
      .request_id    = operation->request_id,
      .launch.params = operation->command.launch_params,
    };
    result = (RVS_PlanResult){
      .status     = RVS_PlanStatus_Handled,
      .effect     = rvs_emit_effect_send_backend_message(arena, message),
      .next_state = RVS_LaunchState_PendingSendAck,
      .next_wait  = { RVS_PlanWaitKind_BackendReply },
    };
  } break;

  //
  // 2. was backend message sent?
  //
  case RVS_LaunchState_PendingSendAck: {
    if (message->kind == RVS_SchedulerMessageKind_BackendSendResult) {
      // launch message was sent to the backend -- now wait for events
      if (message->backend_send_result.send_result == RVS_Result_Ok) {
        result = (RVS_PlanResult){
          .status     = RVS_PlanStatus_Handled,
          .next_state = RVS_LaunchState_AwaitLaunchAck,
          .next_wait  = { RVS_PlanWaitKind_Event },
        };
      }
      // backend didn't receive the launch message sent by the plan; complete the command with the send error code
      else {
        RVS_CommandReply command_reply = {
          .request_id = operation->request_id,
          .result     = message->backend_send_result.send_result,
          .kind       = RVS_CommandReplyKind_LaunchAck,
        };
        result = (RVS_PlanResult){
          .status     = RVS_PlanStatus_Handled,
          .next_state = RVS_PlanState_End,
          .effect     = rvs_emit_effect_complete_command(arena, command_reply),
        };
      }
    }
  } break;

  //
  // 3. wait for the backend to reply with process PID
  //
  case RVS_LaunchState_AwaitLaunchAck: {
    if (message->kind == RVS_SchedulerMessageKind_BackendReply) {
      RVS_DemonReply *reply = &message->backend_reply;
      if (reply->kind == RVS_DemonReplyKind_LaunchStarted) {
        // 3a. backend launched the process successfully -- stash the PID
        RVS_LaunchSuspendedState *ud = plan->ud;
        ud->pid = reply->launch_started.pid;

        // 3b. wait for the create process
        result = (RVS_PlanResult){
          .status     = RVS_PlanStatus_Handled,
          .next_state = RVS_LaunchState_AwaitCreateProcess,
          .next_wait  = { RVS_PlanWaitKind_Event },
        };
      }
    }
  } break;

  //
  // 4. wait for the "create process" event
  //
  case RVS_LaunchState_AwaitCreateProcess: {
    if (message->kind == RVS_SchedulerMessageKind_BackendEvent) {
      RVS_LaunchSuspendedState *launch_state = plan->ud;
      DMN_Event *event = &message->backend_event;

      if (event->system_process_id == launch_state->pid) {
        // 4a. create process event found -> create program entity
        if (event->kind == DMN_EventKind_CreateProcess) {
          result = (RVS_PlanResult){
            .status     = RVS_PlanStatus_Handled,
            .effect     = rvs_emit_effect_create_program(arena, (RVS_ProgramID){ .value = operation->request_id }, *event),
            .next_state = RVS_LaunchState_PendingProgram,
          };
        }
        // 4b. exit-process found before create-process -> stop debugging
        else if (event->kind == DMN_EventKind_ExitProcess) {
          RVS_CommandReply command_reply = {
            .request_id            = operation->request_id,
            .result                = RVS_Result_Error,
            .kind                  = RVS_CommandReplyKind_LaunchAck,
            .launch_ack.program_id = 0,
            .launch_ack.pid        = launch_state->pid,
          };
          result = (RVS_PlanResult){
            .status = RVS_PlanStatus_Handled,
            .effect = rvs_emit_effect_complete_command(arena, command_reply),
          };
        }
        // 4c. no event found -> wait for more events
        else {
          result = (RVS_PlanResult){
            .status    = RVS_PlanStatus_Handled,
            .next_wait = { RVS_PlanWaitKind_Event },
          };
        }
      }
    }
  } break;

  //
  // 5. wait for program entity to be created and reply with the PID and ProgramID
  //
  case RVS_LaunchState_PendingProgram: {
    if (message->kind == RVS_SchedulerMessageKind_NewProgram) {
      RVS_CommandReply command_reply = {
        .request_id            = operation->request_id,
        .result                = RVS_Result_Ok,
        .kind                  = RVS_CommandReplyKind_LaunchAck,
        .launch_ack.program_id = message->new_program.program,
        .launch_ack.pid        = ((RVS_LaunchSuspendedState *)plan->ud)->pid,
      };
      result = (RVS_PlanResult){
        .status = RVS_PlanStatus_Handled,
        .effect = rvs_emit_effect_complete_command(arena, command_reply),
      };
    }
  } break;

  default: break;
  }

  return result;
}

////////////////////////////////

internal RVS_Scheduler *
rvs_scheduler_init(Arena *arena, RVS_RequestPool *request_pool)
{
  RVS_Scheduler *sch = push_array(arena, RVS_Scheduler, 1);
  sch->arena        = arena;
  sch->request_pool = request_pool;
  return sch;
}

internal void
rvs_scheduler_release(RVS_Scheduler *sch)
{
  for (ArenaNode *curr = sch->op_arena_first, *next = 0;
       curr != 0; curr = next) {
    next = curr->next;
    arena_release(curr->v);
  }
  for (ArenaNode *curr = sch->op_arena_free_list, *next = 0;
       curr != 0; curr = next) {
    next = curr->next;
    arena_release(curr->v);
  }
}

internal ArenaNode *
rvs_scheduler_arena_alloc(RVS_Scheduler *sch)
{
  ArenaNode *n = sch->op_arena_free_list;
  if (n) {
    SLLStackPop(sch->op_arena_free_list);
    DLLPushBack(sch->op_arena_first, sch->op_arena_last, n);
  } else {
    Arena *arena = arena_alloc(.name = "Operation Arena");
    n    = push_array(sch->arena, ArenaNode, 1);
    n->v = arena;
    DLLPushBack(sch->op_arena_first, sch->op_arena_last, n);
  }
  return n;
}

internal void
rvs_scheduler_arena_recycle(RVS_Scheduler *sch, ArenaNode *n)
{
  // TODO: release arenas above KB(512) threshold
  arena_clear(n->v);
  DLLRemove(sch->op_arena_first, sch->op_arena_last, n);
  SLLStackPush(sch->op_arena_free_list, n);
}

internal RVS_Operation *
rvs_operation_alloc(RVS_Scheduler *sch)
{
  ArenaNode *arena_node = rvs_scheduler_arena_alloc(sch);
  RVS_Operation *op = push_array(arena_node->v, RVS_Operation, 1);
  op->arena_node = arena_node;
  op->arena = arena_node->v;
  return op;
}

internal void
rvs_operation_release(RVS_Scheduler *sch, RVS_Operation *op)
{
  rvs_scheduler_arena_recycle(sch, op->arena_node);
}

internal void
rvs_scheduler_queue_effect(RVS_Scheduler       *sch,
                           RVS_Operation       *operation,
                           RVS_SchedulerEffect *effect)
{
  AssertAlways(effect && effect->kind != RVS_SchedulerEffectKind_Null);
  AssertAlways(sch->next_effect_id != max_U64);
  effect->id        = ++sch->next_effect_id;
  effect->operation = operation;
  effect->next      = 0;
  SLLQueuePush(sch->effect_first, sch->effect_last, effect);
}

internal void
rvs_scheduler_complete_operation(RVS_Scheduler *sch, RVS_Operation *operation)
{
  rvs_operation_release(sch, operation);
}

internal RVS_Result
rvs_scheduler_advance_plan(RVS_Scheduler *sch, RVS_SchedulerMessage message)
{
  // propagate down message to the active plan
  RVS_Operation  *operation = sch->active_operation;
  RVS_Plan       *plan      = operation->active_plan;
  RVS_PlanResult  result    = plan->sig(operation->arena, operation, plan, &message, plan->state);

  // advance the plan to next state
  plan->state = result.next_state;
  plan->wait  = result.next_wait;

  // queue plan effect
  if (result.effect) {
    rvs_scheduler_queue_effect(sch, operation, result.effect);
  }

  //return result;
  return RVS_Result_Ok;
}

internal RVS_Result
rvs_scheduler_apply(RVS_Scheduler *sch, RVS_SchedulerMessage message)
{
  RVS_Result result = RVS_Result_Error;

  switch (message.kind) {
  case RVS_SchedulerMessageKind_Null: {} break;

  case RVS_SchedulerMessageKind_Command: {
    // TODO: need to be more lenient on admission of operations
    if (sch->active_operation) {
      result = RVS_Result_AlreadyPending;
      break;
    }

    // init operation for the command
    RVS_Operation *op = rvs_operation_alloc(sch);
    op->request_id  = message.command.request_id;
    op->apply_epoch = sch->stop_state.epoch;
    rvs_command_copy(op->arena, &op->command, &message.command.v); // sync command lifetime with the allocated operation

    // init root plan
    RVS_Plan *plan;
    {
      // find plan that matches command
      RVS_PlanSig *plan_sig = 0;
      switch (message.command.v.kind) {
      case RVS_CommandKind_Launch:       { plan_sig = rvs_launch_suspended_plan; } break;
      case RVS_CommandKind_Run:          { NotImplemented; } break;
      case RVS_CommandKind_Pause:        { NotImplemented; } break;
      case RVS_CommandKind_Step:         { NotImplemented; } break;
      case RVS_CommandKind_Exit:         { NotImplemented; } break;
      case RVS_CommandKind_SelectThread: { NotImplemented; } break;

      case RVS_CommandKind_Null: break;
      default: InvalidPath;
      }

      plan        = push_array(op->arena, RVS_Plan, 1);
      plan->epoch = sch->stop_state.epoch;
      plan->state = RVS_PlanState_Begin;
      plan->sig   = plan_sig;
    }

    // set root and active plans
    op->root_plan   = plan;
    op->active_plan = op->root_plan;

    // schedule new operation
    sch->active_operation = op;

    result = rvs_scheduler_advance_plan(sch, message);
  } break;

  case RVS_SchedulerMessageKind_BackendSendResult:
  case RVS_SchedulerMessageKind_NewProgram: {
    result = rvs_scheduler_advance_plan(sch, message);
  } break;

  case RVS_SchedulerMessageKind_BackendReply: {
    if (message.backend_reply.kind == RVS_DemonReplyKind_EventBatch) {
      for EachNode(n, DMN_EventNode, message.backend_reply.event_batch.events.first) {
        RVS_SchedulerMessage event_message = {
          .kind          = RVS_SchedulerMessageKind_BackendEvent,
          .backend_event = n->v
        };
        result = rvs_scheduler_advance_plan(sch, event_message);
      }
    } else {
      result = rvs_scheduler_advance_plan(sch, message);
    }
  } break;

  case RVS_SchedulerMessageKind_CommandCompleteResult: {
    // TODO: scheduler is not doing anything with the completion status

    RVS_SchedulerEffect *effect    = message.command_complete_result.effect;
    RVS_Operation       *operation = effect->operation;
    if (operation == sch->active_operation) {
      sch->active_operation = 0;
    }
    rvs_operation_release(sch, effect->operation);
  } break;

  default: InvalidPath;
  }

  return result;
}

internal RVS_SchedulerEffect *
rvs_scheduler_take_next_effect(Arena *arena, RVS_Scheduler *sch)
{
  RVS_SchedulerEffect *effect = sch->effect_first;

  if (effect) { 
    // pop the first effect in queue
    sch->effect_first = effect->next;
    if (sch->effect_last == effect) {
      sch->effect_last = 0;
    }

    RVS_Operation *operation = effect->operation;
    RVS_Plan      *plan      = operation->active_plan;
    if (plan->wait.kind == RVS_PlanWaitKind_Event &&
        sch->backend_state_kind == RVS_BackendState_Idle) {
      RVS_DemonMessage pump_message = {
        .type       = RVS_DemonMessage_Pump,
        .request_id = operation->request_id,
      };
      RVS_SchedulerEffect *pump_effect = rvs_emit_effect_send_backend_message(operation->arena, pump_message);
      rvs_scheduler_queue_effect(sch, operation, pump_effect);
    }

    // when a command is completed, emit effect that will release the command owner operation
    if (effect->kind == RVS_SchedulerEffectKind_CompleteCommand && sch->active_operation == effect->operation) {
      RVS_SchedulerEffect *complete_operation = rvs_emit_effect_complete_operation(effect->operation->arena);
      rvs_scheduler_queue_effect(sch, effect->operation, complete_operation);
    }
  }

  return effect;
}

