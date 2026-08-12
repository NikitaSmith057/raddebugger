// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////

#include "rvs_plan.h"

////////////////////////////////

internal RVS_Effect*
rvs_emit_effect(RVS_Scheduler *sch, RVS_Operation *operation)
{
  RVS_Effect *effect = push_array(operation->arena, RVS_Effect, 1);
  effect->id        = ++sch->next_effect_id;
  effect->operation = operation;
  return effect;
}

internal RVS_Effect *
rvs_emit_effect_dispatch_operation(RVS_Scheduler *sch, RVS_Operation *operation)
{
  RVS_Effect *effect = rvs_emit_effect(sch, operation);
  effect->kind = RVS_EffectKind_DispatchOperation;
  return effect;
}

internal RVS_Effect *
rvs_emit_effect_complete_command(RVS_Scheduler *sch, RVS_Operation *operation, RVS_CommandReply command_reply)
{
  RVS_Effect *effect = rvs_emit_effect(sch, operation);
  effect->kind            = RVS_EffectKind_CompleteCommand;
  effect->v.command_reply = command_reply;
  return effect;
}

internal RVS_Effect *
rvs_emit_effect_backend_run(RVS_Scheduler *sch, RVS_Operation *operation, RVS_ProcessID *processes, U64 processes_count)
{
  RVS_Effect *effect = rvs_emit_effect(sch, operation);
  effect->kind = RVS_EffectKind_BackendRun;
  effect->v.backend_run.processes_count = processes_count;
  effect->v.backend_run.processes       = push_array(operation->arena, RVS_ProcessID, processes_count);
  MemoryCopy(effect->v.backend_run.processes, processes, processes_count);
  return effect;
}

////////////////////////////////

RVS_PLAN_FUNC(rvs_plan_launch_suspended)
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

  RVS_PlanResult result = { .status = RVS_PlanStatus_Null };

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
    result = (RVS_PlanResult){
      .status     = RVS_PlanStatus_Ok,
      .next_state = RVS_LaunchState_PendingSendAck,
      .next_wait  = RVS_PlanWaitKind_EffectCompletion,
      .effect     = rvs_emit_effect_dispatch_operation(sch, operation),
    };
  } break;

  //
  // 2. was backend message sent?
  //
  case RVS_LaunchState_PendingSendAck: {
    AssertAlways(message->kind == RVS_SchedulerMessageKind_BackendSendResult);

    // launch message was sent to the backend -- now wait for events
    if (message->backend_send_result.send_result == RVS_Result_Ok) {
      result = (RVS_PlanResult){
        .status     = RVS_PlanStatus_Ok,
        .next_wait  = RVS_PlanWaitKind_BackendReply,
        .next_state = RVS_LaunchState_AwaitLaunchAck,
      };
    }
    // backend didn't receive the launch message sent by the plan; complete the command with the sent error code
    else {
      RVS_CommandReply command_reply = {
        .request_id = operation->request_id,
        .result     = message->backend_send_result.send_result,
        .kind       = RVS_CommandReplyKind_LaunchAck,
      };
      result = (RVS_PlanResult){
        .status     = RVS_PlanStatus_Ok,
        .effect     = rvs_emit_effect_complete_command(sch, operation, command_reply),
        .next_state = RVS_PlanState_End,
      };
    }
  } break;

  //
  // 3. wait for the backend to reply with process PID
  //
  case RVS_LaunchState_AwaitLaunchAck: {
    AssertAlways(message->kind == RVS_SchedulerMessageKind_BackendReply);

    RVS_DemonReply *reply = &message->backend_reply;
    if (reply->kind == RVS_DemonReplyKind_LaunchStarted) {
      // 3a. backend launched the process successfully -- stash the PID
      RVS_LaunchSuspendedState *ud = plan->ud;
      ud->pid = reply->launch_pid;

      // 3b. wait for the create process
      result = (RVS_PlanResult){
        .status     = RVS_PlanStatus_Ok,
        .next_wait  = RVS_PlanWaitKind_Event,
        .next_state = RVS_LaunchState_AwaitCreateProcess,
      };
    }
  } break;

  //
  // 4. wait for the "create process" event
  //
  case RVS_LaunchState_AwaitCreateProcess: {
    AssertAlways(message->kind == RVS_SchedulerMessageKind_BackendEvent);

    RVS_LaunchSuspendedState *launch_state = plan->ud;
    RVS_Event                *event        = &message->backend_event;

    if (event->raw_event.system_process_id == launch_state->pid) {
      // 4a. create process event found -> create program entity
      if (event->raw_event.kind == DMN_EventKind_CreateProcess) {
        RVS_LaunchSuspendedState *ud = plan->ud;
        AssertAlways(event->raw_event.system_process_id == ud->pid);

        RVS_CommandReply command_reply = {
          .result                = RVS_Result_Ok,
          .kind                  = RVS_CommandReplyKind_LaunchAck,
          .launch_ack.program_id = event->program,
          .launch_ack.pid        = ud->pid,
        };

        result = (RVS_PlanResult){
          .status     = RVS_PlanStatus_Ok,
          .effect     = rvs_emit_effect_complete_command(sch, operation, command_reply),
          .next_state = RVS_PlanState_End
        };
      }
      // 4b. exit-process found before create-process -> stop debugging
      else if (event->raw_event.kind == DMN_EventKind_ExitProcess) {
        RVS_CommandReply command_reply = {
          .result                = RVS_Result_Error,
          .kind                  = RVS_CommandReplyKind_LaunchAck,
          .launch_ack.program_id = 0,
          .launch_ack.pid        = launch_state->pid,
        };
        result = (RVS_PlanResult){
          .status = RVS_PlanStatus_Ok,
          .effect = rvs_emit_effect_complete_command(sch, operation, command_reply),
        };
      }
      // 4c. no event found -> wait for more events
      else {
        result = (RVS_PlanResult){
          .status    = RVS_PlanStatus_Ok,
          //.next_wait = { RVS_PlanWaitKind_Event },
        };
      }
    }
  } break;

  //
  // 5. wait for program entity to be created and reply with the PID and ProgramID
  //
  case RVS_LaunchState_PendingProgram: {
    if (message->kind == RVS_SchedulerMessageKind_NewProgram) {
      RVS_CommandReply command_reply = {
        .result                = RVS_Result_Ok,
        .kind                  = RVS_CommandReplyKind_LaunchAck,
        .launch_ack.program_id = message->new_program.program,
        .launch_ack.pid        = ((RVS_LaunchSuspendedState *)plan->ud)->pid,
      };
      result = (RVS_PlanResult){
        .status = RVS_PlanStatus_Ok,
        .effect = rvs_emit_effect_complete_command(sch, operation, command_reply),
      };
    }
  } break;

  default: result.status = RVS_PlanStatus_NoStateMatch; break;
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
rvs_scheduler_queue_effect(RVS_Operation *operation, RVS_Effect *effect)
{
  RVS_EffectPtrNode *n = push_array(operation->arena, RVS_EffectPtrNode, 1);
  n->v = effect;
  SLLQueuePush(operation->effect_first, operation->effect_last, n);
}

internal void
rvs_scheduler_complete_operation(RVS_Scheduler *sch, RVS_Operation *operation)
{
  rvs_operation_release(sch, operation);
}

internal RVS_Result
rvs_scheduler_advance(RVS_Scheduler *sch, RVS_SchedulerMessage message)
{
  // propagate down message to the active plan
  RVS_Operation  *operation = sch->active_operation;
  RVS_Plan       *plan      = operation->active_plan;

  // is plan waiting for this message?
  B32 route_message = 1;  
  switch (plan->wait) {
  case RVS_PlanWaitKind_Null: break;
  case RVS_PlanWaitKind_Event: {
    route_message = message.kind == RVS_SchedulerMessageKind_BackendEvent;
  } break;
  case RVS_PlanWaitKind_EffectCompletion: {
  } break;
  case RVS_PlanWaitKind_BackendReply: {
    route_message = message.kind == RVS_SchedulerMessageKind_BackendReply;
  } break;
  }

  RVS_PlanResult result = {0};
  if (route_message) {
    result = plan->sig(operation->arena, sch, operation, plan, &message, plan->state);
  }

  // advance the plan to next state
  plan->state       = result.next_state;
  plan->wait        = result.next_wait;
  plan->wait_effect = plan->wait == RVS_PlanWaitKind_EffectCompletion ? result.effect : 0;

  // queue plan effect
  if (result.effect) {
    rvs_scheduler_queue_effect(operation, result.effect);
  }

  //return result;
  return RVS_Result_Ok;
}

internal RVS_Result
rvs_scheduler_apply(RVS_Scheduler *sch, RVS_EntityStore *entities, RVS_SchedulerMessage message)
{
  Temp scratch = scratch_begin(0,0);

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
      case RVS_CommandKind_Launch:       { plan_sig = rvs_plan_launch_suspended; } break;
      case RVS_CommandKind_Run:          { NotImplemented; } break;
      case RVS_CommandKind_Pause:        { NotImplemented; } break;
      case RVS_CommandKind_Step:         { NotImplemented; } break;
      case RVS_CommandKind_Exit:         { NotImplemented; } break;
      case RVS_CommandKind_SelectThread: { NotImplemented; } break;

      case RVS_CommandKind_Null: break;
      default: InvalidPath;
      }

      plan        = push_array(op->arena, RVS_Plan, 1);
      plan->state = RVS_PlanState_Begin;
      plan->sig   = plan_sig;
    }

    // set root and active plans
    op->root_plan   = plan;
    op->active_plan = op->root_plan;

    // schedule new operation
    sch->active_operation = op;

    result = rvs_scheduler_advance(sch, message);
  } break;

  case RVS_SchedulerMessageKind_BackendSendResult:
  case RVS_SchedulerMessageKind_NewProgram: {
    result = rvs_scheduler_advance(sch, message);
  } break;

  case RVS_SchedulerMessageKind_BackendReply: {
    //
    // process backend event batch message
    //
    if (message.backend_reply.kind == RVS_DemonReplyKind_EventBatch) {
      for EachNode(n, DMN_EventNode, message.backend_reply.event_batch.first) {
        Temp temp = temp_begin(scratch.arena);

        //
        // 1. normalize backend event
        //
        RVS_EventList normalized_events = {0};
        result = rvs_entity_store_apply_backend_event(temp.arena, entities, n->v, &normalized_events);

        // TODO: error handle bad normalization
        if (result != RVS_Result_Ok) {
          NotImplemented;
          goto stop_event_reduction;
        }

        //
        // 2. single backend event may normalize to multiple events, so apply
        //    the normal batch before advancing to the next backend event
        //
        for EachNode(normal_event_n, RVS_EventNode, normalized_events.first) {
          // wrap event into a message
          RVS_SchedulerMessage event_message = {
            .kind          = RVS_SchedulerMessageKind_BackendEvent,
            .backend_event = normal_event_n->v,
          };

          //
          // 3. advance scheduler with the normalized event
          //
          result = rvs_scheduler_advance(sch, event_message);

          // was scheduler advanced?
          if (result != RVS_Result_Ok) { goto stop_event_reduction; }
        }

        temp_end(temp);
      }
      stop_event_reduction:;
      break;
    }

    result = rvs_scheduler_advance(sch, message);
  } break;

  case RVS_SchedulerMessageKind_CommandCompleteResult: {
    // TODO: scheduler is not doing anything with the completion status

    RVS_Effect *effect    = message.command_complete_result.effect;
    RVS_Operation       *operation = effect->operation;
    if (operation == sch->active_operation) {
      sch->active_operation = 0;
    }
    rvs_operation_release(sch, effect->operation);
  } break;

  default: InvalidPath;
  }

  scratch_end(scratch);
  return result;
}

internal RVS_Effect *
rvs_scheduler_pump_effect(Arena *arena, RVS_Scheduler *sch)
{
  RVS_Effect    *effect    = 0;
  RVS_Operation *operation = sch->active_operation;
  RVS_Plan      *plan      = operation->active_plan;

  // pop first effect from the operation
  if (operation->effect_first) {
    RVS_EffectPtrNode *effect_ptr = operation->effect_first;
    operation->effect_first = effect_ptr->next;
    if (operation->effect_last == effect_ptr) {
      operation->effect_last = 0;
    }
    effect = effect_ptr->v;
  }

  // plan needs a new event to advance to the next state, but the backend is in idle state and event batch is empty;
  // in this case, emit a backend request to run targets that are under plan
  if (effect == 0 && plan->wait == RVS_PlanWaitKind_Event && sch->backend_state_kind == RVS_BackendState_Idle) {
    // TODO: need a global event reducer to implement this step
    NotImplemented;
    //effect = rvs_emit_effect_backend_run(sch, operation, operation->processes, operation->processes_count);
    //rvs_emit_effect_send_backend_message(sch, operation, (RVS_DemonMessage){ .base.kind = RVS_CommandKind_Run });
    rvs_scheduler_queue_effect(operation, effect);
  }

  return effect;
}

