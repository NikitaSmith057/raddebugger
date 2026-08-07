// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////

#include "rvs_scheduler.h"

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

////////////////////////////////

RVS_PLAN_FUNC(rvs_launch_suspended_plan)
{
  enum {
    RVS_LaunchState_LaunchAck = RVS_PlanState_UserLo,
    RVS_LaunchState_AwaitLaunchAck,
    RVS_LaunchState_AwaitCreateProcess,
    RVS_LaunchState_PendingProgram,
  };

  typedef struct {
    U32 pid;
  } RVS_LaunchSuspendedState;

  RVS_PlanResult result = { .status = RVS_PlanStatus_Unhandled };

  switch (plan->state) {
  //
  // 1. initialize program launch
  //
  case RVS_PlanState_Begin: {
    AssertAlways(input->kind == RVS_InputSourceKind_Command);
    AssertAlways(operation->command.kind == RVS_CommandKind_Launch);

    //
    // 1a. allocate temp state for the PID
    //
    plan->ud = push_array(arena, RVS_LaunchSuspendedState, 1);

    //
    // 1b. send backend request for launch + parameters
    //
    RVS_DemonMessage message = {
      .type          = RVS_DemonMessage_Launch,
      .request_id    = operation->request_id,
      .launch.params = operation->command.launch_params,
    };
    result = (RVS_PlanResult){
      .status     = RVS_PlanStatus_AwaitBackendReply,
      .next_state = RVS_LaunchState_AwaitLaunchAck,
      .effect     = rvs_emit_effect_send_backend_message(arena, message),
    };
  } break;

  //
  // 2. wait for the backend to reply with process PID
  //
  case RVS_LaunchState_AwaitLaunchAck: {
    if (input->kind == RVS_InputSourceKind_BackendSendResult &&
        input->backend_send_result.v != RVS_Result_Ok) {
      result = (RVS_PlanResult){
        .status = RVS_PlanStatus_Handled,
        .next_state = RVS_PlanState_End,
        .effect = rvs_emit_effect_complete_command(arena, (RVS_CommandReply){
          .request_id = operation->request_id,
          .result = input->backend_send_result.v,
          .kind = RVS_CommandReplyKind_LaunchAck,
        }),
      };
    }
    if (input->kind == RVS_InputSourceKind_BackendReply) {
      RVS_DemonReply *reply = &input->backend_reply.v;
      if (reply->kind == RVS_DemonReplyKind_LaunchStarted) {
        RVS_LaunchSuspendedState *ud = plan->ud;
        ud->pid = reply->launch_started.pid;

        result = (RVS_PlanResult){
          .status      = RVS_PlanStatus_AwaitEvent,
          .next_state  = RVS_LaunchState_AwaitCreateProcess,
          .effect = rvs_emit_effect_send_backend_message(arena, (RVS_DemonMessage){
            .type = RVS_DemonMessage_Pump,
            .request_id = operation->request_id,
          }),
        };
      } else if (reply->kind == RVS_DemonReplyKind_ActionResult &&
                 reply->action_result.action == RVS_DemonAction_Launch) {
        result = (RVS_PlanResult){
          .status = RVS_PlanStatus_Handled,
          .next_state = RVS_PlanState_End,
          .effect = rvs_emit_effect_complete_command(arena, (RVS_CommandReply){
            .request_id = operation->request_id,
            .result = reply->action_result.result,
            .kind = RVS_CommandReplyKind_LaunchAck,
          }),
        };
      }
    }
  } break;

  //
  // 3. wait for the "create process" event
  //
  case RVS_LaunchState_AwaitCreateProcess: {
    if (input->kind == RVS_InputSourceKind_BackendSendResult &&
        input->backend_send_result.v != RVS_Result_Ok) {
      result = (RVS_PlanResult){
        .status = RVS_PlanStatus_Handled,
        .next_state = RVS_PlanState_End,
        .effect = rvs_emit_effect_complete_command(arena, (RVS_CommandReply){
          .request_id = operation->request_id,
          .result = input->backend_send_result.v,
          .kind = RVS_CommandReplyKind_LaunchAck,
        }),
      };
    }
    if (input->kind == RVS_InputSourceKind_BackendEvent) {
      RVS_LaunchSuspendedState *launch_state = plan->ud;
      DMN_Event *event = &input->backend_event.event;

      if (event->system_process_id == launch_state->pid) {
        //
        // 3a. create process event found -> create program entity
        //
        if (event->kind == DMN_EventKind_CreateProcess) {
          result = (RVS_PlanResult){
            .status     = RVS_PlanStatus_Handled,
            .effect     = rvs_emit_effect_create_program(arena, (RVS_ProgramID){ .value = operation->request_id }, *event),
            .next_state = RVS_LaunchState_PendingProgram,
          };
        }
        //
        // 3b. exit process found -> stop debugging
        //
        else if (event->kind == DMN_EventKind_ExitProcess) {
          // if process exits before the create process event was seen
          // fail launch handshake
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
        //
        // 3c. no event found -> wait for more events
        //
        else {
          // ask scheduler to retry again with a new event
          result = (RVS_PlanResult){ .status = RVS_PlanStatus_AwaitEvent };
        }
      }
    }
  } break;

  //
  // 4. wait for program entity to be created and reply with the PID and ProgramID
  //
  case RVS_LaunchState_PendingProgram: {
    if (input->kind == RVS_InputSourceKind_NewProgram) {
      RVS_CommandReply command_reply = {
        .request_id            = operation->request_id,
        .result                = RVS_Result_Ok,
        .kind                  = RVS_CommandReplyKind_LaunchAck,
        .launch_ack.program_id = input->new_program.program,
        .launch_ack.pid        = ((RVS_LaunchSuspendedState *)plan->ud)->pid,
      };
      return (RVS_PlanResult){
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
rvs_scheduler_queue_effect(RVS_Scheduler *sch, RVS_Operation *operation, RVS_SchedulerEffect *effect)
{
  AssertAlways(effect != 0 && effect->kind != RVS_SchedulerEffectKind_Null);
  AssertAlways(sch->next_effect_id != max_U64);
  effect->id = ++sch->next_effect_id;
  AssertAlways(effect->id != 0);
  effect->operation = operation;
  effect->next = 0;
  SLLQueuePush(sch->effect_first, sch->effect_last, effect);
}

internal RVS_Result
rvs_scheduler_apply_active_plan(RVS_Scheduler *sch, RVS_SchedulerInput input)
{
  RVS_Operation *operation = sch->active_operation;
  if (operation == 0 || operation->active_plan == 0) {
    return RVS_Result_StaleState;
  }
  if (input.kind == RVS_InputSourceKind_BackendReply &&
      input.backend_reply.v.request_id != operation->request_id) {
    return RVS_Result_StaleState;
  }
  if (input.kind == RVS_InputSourceKind_BackendSendResult &&
      input.backend_send_result.request_id != operation->request_id) {
    return RVS_Result_StaleState;
  }
  if (input.kind == RVS_InputSourceKind_NewProgram &&
      input.new_program.request_id != operation->request_id) {
    return RVS_Result_StaleState;
  }

  RVS_Plan *plan = operation->active_plan;
  RVS_PlanResult result = plan->sig(operation->arena, operation, plan, &input);
  if (result.status == RVS_PlanStatus_Unhandled) {
    return RVS_Result_StaleState;
  }
  if (result.next_state != RVS_PlanState_Null) {
    plan->state = result.next_state;
  }
  if (result.effect) {
    rvs_scheduler_queue_effect(sch, operation, result.effect);
  }
  return RVS_Result_Ok;
}

internal RVS_Result
rvs_scheduler_apply(RVS_Scheduler *sch, RVS_SchedulerInput input)
{
  RVS_Result result = RVS_Result_Error;

  switch (input.kind) {
  case RVS_InputSourceKind_Null: {} break;

  case RVS_InputSourceKind_Command: {
    // TODO: need to be more lenient on admission of operations
    if (sch->active_operation) {
      result = RVS_Result_AlreadyPending;
      break;
    }

    // init operation for the command
    RVS_Operation *op = rvs_operation_alloc(sch);
    op->request_id  = input.command.request_id;
    op->apply_epoch = sch->stop_state.epoch;
    rvs_command_copy(op->arena, &op->command, &input.command.v); // sync command lifetime with the allocated operation

    // init root plan
    RVS_Plan *plan;
    {
      // find plan that matches command
      RVS_PlanSig *plan_sig = 0;
      switch (input.command.v.kind) {
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
    result = rvs_scheduler_apply_active_plan(sch, input);
  } break;

  case RVS_InputSourceKind_BackendSendResult:
  case RVS_InputSourceKind_BackendReply:
  case RVS_InputSourceKind_BackendEvent:
  case RVS_InputSourceKind_NewProgram: {
    result = rvs_scheduler_apply_active_plan(sch, input);
  } break;

  default: InvalidPath;
  }

  return result;
}

internal RVS_SchedulerEffect *
rvs_scheduler_pump(Arena *arena, RVS_Scheduler *sch)
{
  RVS_SchedulerEffect *result = push_array(arena, RVS_SchedulerEffect, 1);
  RVS_SchedulerEffect *effect = sch->effect_first;
  if (effect) {
    sch->effect_first = effect->next;
    if (sch->effect_last == effect) {
      sch->effect_last = 0;
    }

    *result = *effect;
    result->next = 0;
    if (result->kind == RVS_SchedulerEffectKind_CompleteCommand &&
        sch->active_operation == result->operation) {
      sch->active_operation = 0;
      rvs_operation_release(sch, result->operation);
    }
  }
  return result;
}

