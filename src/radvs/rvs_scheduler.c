// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////

#include "rvs_scheduler.h"

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
rvs_emit_effect_dispatch_operation_command(RVS_Scheduler *sch, RVS_Operation *operation)
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

internal RVS_Effect *
rvs_emit_effect_pump_backend_event(RVS_Scheduler *sch, RVS_Operation *operation)
{
  RVS_Effect *effect = rvs_emit_effect(sch, operation);
  effect->kind                      = RVS_EffectKind_SendBackendMessage;
  effect->v.backend_message.base.id = effect->id;
  effect->v.backend_message.command.kind = (RVS_CommandKind)RVS_DemonCommand_PumpEvent;
  return effect;
}

////////////////////////////////

internal void
rvs_logf(char *fmt, ...)
{
  Temp scratch = scratch_begin(0,0);
  va_list args;
  va_start(args, fmt);
  String8 result = push_str8fv(scratch.arena, fmt, args);
  va_end(args);
  fprintf(stderr, "%.*s\n", str8_varg(result));
  scratch_end(scratch);
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
      .effect     = rvs_emit_effect_dispatch_operation_command(sch, operation),
    };
  } break;

  //
  // 2. was backend message sent?
  //
  case RVS_LaunchState_PendingSendAck: {
    AssertAlways(message->kind == RVS_SchedulerMessageKind_EffectComplete);

    // launch message was sent to the backend -- now wait for events
    if (message->effect_complete.result == RVS_Result_Ok) {
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
        .result     = message->effect_complete.result,
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
    AssertAlways(message->kind == RVS_SchedulerMessageKind_BackendEvent_Normal);

    RVS_LaunchSuspendedState *launch_state = plan->ud;
    RVS_Event                *event        = &message->backend_event;

    if (event->raw_event.code == launch_state->pid) {
      // 4a. create process event found -> create program entity
      if (event->raw_event.kind == DMN_EventKind_CreateProcess) {
        RVS_LaunchSuspendedState *ud = plan->ud;
        AssertAlways(event->raw_event.code == ud->pid);

        RVS_CommandReply command_reply = {
          .request_id            = operation->request_id,
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
          .request_id            = operation->request_id,
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

  //
  // 6. TODO: cleanup program entity on failed launch
  //

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
  if (operation == 0) { return RVS_Result_Ok; }

  RVS_Plan       *plan      = operation->active_plan;

  // is plan waiting for this message?
  B32 route_message = 0;
  switch (plan->wait) {
  case RVS_PlanWaitKind_Null: {
    route_message = 1;
  } break;
  case RVS_PlanWaitKind_Event: {
    route_message = message.kind == RVS_SchedulerMessageKind_BackendEvent_Normal;
  } break;
  case RVS_PlanWaitKind_EffectCompletion: {
    AssertAlways(plan->wait_effect);
    route_message = (message.kind == RVS_SchedulerMessageKind_EffectComplete &&
                     message.effect_complete.effect == plan->wait_effect &&
                     message.effect_complete.effect->operation == operation);
  } break;
  case RVS_PlanWaitKind_BackendReply: {
    route_message = (message.kind == RVS_SchedulerMessageKind_BackendReply &&
                     message.backend_reply.id == operation->request_id);
  } break;
  default: InvalidPath;
  }

  if (route_message) {
    RVS_PlanResult result = plan->sig(operation->arena, sch, operation, plan, &message, plan->state);
    if (result.status == RVS_PlanStatus_Ok) {
      // advance the plan to next state
      plan->state       = result.next_state;
      plan->wait        = result.next_wait;
      plan->wait_effect = plan->wait == RVS_PlanWaitKind_EffectCompletion ? result.effect : 0;

      // queue plan effect
      if (result.effect) {
        rvs_scheduler_queue_effect(operation, result.effect);
      }
    }
  }

  //return result;
  return RVS_Result_Ok;
}

internal RVS_Result
rvs_scheduler_notify(RVS_Scheduler *sch, RVS_EntityStore *entities, RVS_SchedulerMessage message)
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

    // init root command plan from the dequeued message
    RVS_Plan *plan;
    {
      #define PLAN_XLIST \
      X(Launch, rvs_plan_launch_suspended)

      RVS_PlanSig *plan_sig = 0;
      switch (message.command.v.kind) {
        #define X(id, func) case RVS_CommandKind_##id: plan_sig = func; break;
        PLAN_XLIST
        #undef X
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

  case RVS_SchedulerMessageKind_EffectComplete: {
      if (sch->backend_state_kind            == RVS_BackendState_Pumping                &&
          message.kind                       == RVS_SchedulerMessageKind_EffectComplete &&
          message.effect_complete.effect->id == sch->active_effect_id                   &&
          message.effect_complete.result     != RVS_Result_Ok
          ) {
      sch->backend_state_kind       = RVS_BackendState_Idle;
      sch->active_backend_request_id = 0;
      sch->active_effect_id          = 0;
    }
    result = rvs_scheduler_advance(sch, message);
  } break;

  case RVS_SchedulerMessageKind_BackendEvent_Raw: {
    Temp temp = temp_begin(scratch.arena);

    // 1. normalize backend event
    RVS_EventList normalized_events = {0};
    result = rvs_entity_store_apply_backend_event(temp.arena, entities, message.raw_backend_event, &normalized_events);

    // TODO: error handle bad normalization
    if (result != RVS_Result_Ok) {
      NotImplemented;
    }

    // 2. single backend event may normalize to multiple events, so apply
    //    the normal batch before advancing to the next backend event
    for EachNode(event_n, RVS_EventNode, normalized_events.first) {
      // wrap event into a message
      RVS_SchedulerMessage event_message = {
        .kind          = RVS_SchedulerMessageKind_BackendEvent_Normal,
        .backend_event = event_n->v,
      };

      // 3. advance scheduler with the normalized event
      result = rvs_scheduler_advance(sch, event_message);

      // was scheduler advanced?
      if (result != RVS_Result_Ok) { break; }
    }

    temp_end(temp);
  } break;

  case RVS_SchedulerMessageKind_BackendReply: {
    if (message.backend_reply.kind == RVS_DemonReplyKind_EventBatch) {
      // the engine applies every event before completing the batch, so no next
      // pump can overtake events that were already returned by the backend
      if (sch->backend_state_kind == RVS_BackendState_Pumping &&
          message.backend_reply.id == sch->active_backend_request_id) {
        sch->backend_state_kind        = RVS_BackendState_Idle;
        sch->active_backend_request_id = 0;
        sch->active_effect_id          = 0;
      }
      result = RVS_Result_Ok;
    } else {
      result = rvs_scheduler_advance(sch, message);
    }
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
  (void)arena;

  RVS_Effect *effect = 0;

  if (sch->active_operation) {
    RVS_Operation *operation = sch->active_operation;

    // pop first effect from the operation
    if (operation->effect_first) {
      RVS_EffectPtrNode *effect_ptr = operation->effect_first;
      operation->effect_first = effect_ptr->next;
      if (operation->effect_last == effect_ptr) {
        operation->effect_last = 0;
      }
      effect = effect_ptr->v;
    }

    if (effect == 0 &&
        operation->active_plan->wait == RVS_PlanWaitKind_Event &&
        sch->backend_state_kind == RVS_BackendState_Idle) {
      // pump one frozen backend control cycle when an event-waiting plan has no
      // queued work; this receives pending events without resuming debug targets
      effect = rvs_emit_effect_pump_backend_event(sch, operation);

      sch->backend_state_kind        = RVS_BackendState_Pumping;
      sch->active_backend_request_id = effect->v.backend_message.base.id;
      sch->active_effect_id          = effect->id;
    }
  }

  return effect;
}

