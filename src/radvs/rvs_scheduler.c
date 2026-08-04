// This file is included only by rvs_engine.c after private engine/session types.

#include "radvs/rvs_request.h"

internal RVS_SchedulerEmission *rvs_scheduler_emit_locked(RVS_Scheduler *scheduler, RVS_SchedulerDecision *decision, RVS_SchedulerEmissionKind kind, RVS_ScheduledOperation *operation);
internal RVS_SchedulerCommand  *rvs_scheduler_set_command_locked(RVS_Scheduler *scheduler, RVS_SchedulerDecision *decision, RVS_SchedulerCommandKind kind, RVS_ScheduledOperation *operation);
internal RVS_TargetSnapshotStorage *rvs_scheduler_target_storage_acquire(RVS_Scheduler *scheduler, U64 capacity);
internal void rvs_scheduler_target_storage_release(RVS_Scheduler *scheduler, RVS_TargetSnapshotStorage *storage);
internal void rvs_scheduler_retire_operation_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation,
                                                     RVS_ScheduledOperationState terminal_state);
internal void rvs_scheduler_retire_exhausted_suspended_locked(RVS_Scheduler *scheduler);
internal RVS_SchedulerEmission *rvs_scheduler_complete_operation_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation,
                                                                         RVS_Result result, RVS_SchedulerDecision *decision);
internal RVS_Plan *rvs_scheduler_plan_alloc_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation,
                                                   RVS_PlanKind kind, RVS_PlanID parent);
internal RVS_Plan *rvs_scheduler_plan_from_id_locked(RVS_Scheduler *scheduler, RVS_PlanID plan_id);

thread_static RVS_Scheduler *rvs_scheduler_reducer_active;

internal void
rvs_scheduler_assert_reducing(RVS_Scheduler *scheduler)
{
  AssertAlways(rvs_scheduler_reducer_active == scheduler);
}

internal RVS_ExecutionOwner
rvs_execution_owner_operation(RVS_MessageID request_id)
{
  return (RVS_ExecutionOwner){ .kind = RVS_ExecutionOwnerKind_Operation, .request_id = request_id };
}

internal RVS_ExecutionOwner
rvs_execution_owner_control(RVS_MessageID request_id, U64 transaction_id)
{
  return (RVS_ExecutionOwner){
    .kind = RVS_ExecutionOwnerKind_ControlTransaction,
    .request_id = request_id,
    .transaction_id = transaction_id,
  };
}

internal B32
rvs_execution_owner_match(RVS_ExecutionOwner a, RVS_ExecutionOwner b)
{
  return a.kind == b.kind && a.request_id == b.request_id && a.transaction_id == b.transaction_id;
}

internal void
rvs_scheduler_assert_execution_owner_locked(RVS_Scheduler *scheduler)
{
  RVS_ExecutionOwner owner = scheduler->execution_owner;
  if (owner.kind == RVS_ExecutionOwnerKind_Operation) {
    RVS_ScheduledOperation *operation = rvs_scheduler_operation_from_request_id_locked(scheduler, owner.request_id);
    AssertAlways(owner.request_id != 0 && owner.transaction_id == 0);
#if RVS_ENGINE_TESTING
    if (operation) {
#else
    {
      AssertAlways(operation != 0);
#endif
      AssertAlways(operation->state == RVS_ScheduledOperationState_Queued || operation->state == RVS_ScheduledOperationState_Active);
    }
  } else if (owner.kind == RVS_ExecutionOwnerKind_ControlTransaction) {
    AssertAlways(owner.transaction_id != 0 && scheduler->stop_transaction.transaction_id == owner.transaction_id);
    AssertAlways(scheduler->stop_transaction.owner != 0 && scheduler->stop_transaction.owner->request->request_id == owner.request_id);
    RVS_StopTransaction *stop = &scheduler->stop_transaction;
    if (stop->backend_stable) {
      AssertAlways(stop->stable_stop_generation != 0 && stop->interrupt_token.request_id == 0 &&
                   stop->interrupt_token.command_id == 0 && stop->phase == RVS_ControlTransactionPhase_WaitingForResume &&
                   stop->owner->pending_command.kind == RVS_SchedulerCommand_ResumeTargetSubset);
    } else if (stop->phase == RVS_ControlTransactionPhase_WaitingForInterrupt ||
               stop->phase == RVS_ControlTransactionPhase_CollectingBatch) {
      AssertAlways(stop->stable_stop_generation == 0 && stop->interrupt_token.request_id == owner.request_id &&
                   stop->interrupt_token.command_id != 0 &&
                   stop->owner->pending_command.kind == RVS_SchedulerCommand_InterruptExecution);
    }
  } else if (owner.kind == RVS_ExecutionOwnerKind_Shutdown) {
    AssertAlways(scheduler->phase == RVS_SchedulerPhase_Exited);
  } else {
    AssertAlways(owner.request_id == 0 && owner.transaction_id == 0);
  }
}

typedef enum
{
  RVS_SchedulerScope_Session,
  RVS_SchedulerScope_Targets,
} RVS_SchedulerScope;

typedef enum
{
  RVS_SchedulerTargetShape_None,
  RVS_SchedulerTargetShape_One,
  RVS_SchedulerTargetShape_OneOrMore,
} RVS_SchedulerTargetShape;

typedef enum
{
  RVS_SchedulerTransition_Generic,
  RVS_SchedulerTransition_Run,
  RVS_SchedulerTransition_Interrupt,
  RVS_SchedulerTransition_Terminate,
} RVS_SchedulerTransition;

typedef enum
{
  RVS_AdmissionRequirement_None,
  RVS_AdmissionRequirement_StoppedUnfenced,
  RVS_AdmissionRequirement_ActiveOneLease,
  RVS_AdmissionRequirement_LiveUnfenced,
  RVS_AdmissionRequirement_ReadOnly,
} RVS_AdmissionRequirement;

typedef struct
{
  RVS_SchedulerOp         op;
  RVS_SchedulerDuplicate  duplicate;
  RVS_SchedulerScope      scope;
  RVS_SchedulerTargetShape target_shape;
  RVS_AdmissionRequirement admission_requirement;
  RVS_SchedulerTransition transition;
} RVS_SchedulerOperationRule;

typedef enum
{
  RVS_TargetTransition_Queue,
  RVS_TargetTransition_RunAccepted,
  RVS_TargetTransition_PreDispatchCancelled,
  RVS_TargetTransition_ConfirmedStop,
  RVS_TargetTransition_BarrierPause,
  RVS_TargetTransition_BarrierRollback,
  RVS_TargetTransition_BarrierResume,
  RVS_TargetTransition_TerminationFence,
  RVS_TargetTransition_ConfirmedExit,
  RVS_TargetTransition_Retired,
  RVS_TargetTransition_Shutdown,
} RVS_TargetTransitionKind;

typedef struct
{
  RVS_TargetState          source;
  RVS_TargetState          destination;
  RVS_TargetTransitionKind kind;
  B32                      advances_revision;
} RVS_TargetTransitionRule;

global RVS_SchedulerOperationRule rvs_scheduler_operation_rules[] = {
  { RVS_SchedulerOp_Launch,              RVS_SchedulerDuplicate_Reject, RVS_SchedulerScope_Session, RVS_SchedulerTargetShape_None,      RVS_AdmissionRequirement_None,             RVS_SchedulerTransition_Generic },
  { RVS_SchedulerOp_Run,                 RVS_SchedulerDuplicate_Reject, RVS_SchedulerScope_Targets, RVS_SchedulerTargetShape_OneOrMore, RVS_AdmissionRequirement_StoppedUnfenced,  RVS_SchedulerTransition_Run },
  { RVS_SchedulerOp_Interrupt,           RVS_SchedulerDuplicate_Reject, RVS_SchedulerScope_Targets, RVS_SchedulerTargetShape_OneOrMore, RVS_AdmissionRequirement_ActiveOneLease,   RVS_SchedulerTransition_Interrupt },
  { RVS_SchedulerOp_Terminate,           RVS_SchedulerDuplicate_Reject, RVS_SchedulerScope_Targets, RVS_SchedulerTargetShape_OneOrMore, RVS_AdmissionRequirement_LiveUnfenced,     RVS_SchedulerTransition_Terminate },
  { RVS_SchedulerOp_ReadOnly,            RVS_SchedulerDuplicate_Join,   RVS_SchedulerScope_Targets, RVS_SchedulerTargetShape_One,       RVS_AdmissionRequirement_ReadOnly,         RVS_SchedulerTransition_Generic },
  { RVS_SchedulerOp_TargetConfiguration, RVS_SchedulerDuplicate_Reject, RVS_SchedulerScope_Targets, RVS_SchedulerTargetShape_One,       RVS_AdmissionRequirement_StoppedUnfenced,  RVS_SchedulerTransition_Generic },
};

global RVS_TargetTransitionRule rvs_target_transition_rules[] = {
  { RVS_TargetState_Stopped,          RVS_TargetState_Queued,         RVS_TargetTransition_Queue,                 0 },
  { RVS_TargetState_Queued,           RVS_TargetState_Running,        RVS_TargetTransition_RunAccepted,           1 },
  { RVS_TargetState_Queued,           RVS_TargetState_Stopped,        RVS_TargetTransition_PreDispatchCancelled,  0 },
  { RVS_TargetState_Running,          RVS_TargetState_Stopped,        RVS_TargetTransition_ConfirmedStop,         1 },
  { RVS_TargetState_PausedForEvent,   RVS_TargetState_Stopped,        RVS_TargetTransition_ConfirmedStop,         1 },
  { RVS_TargetState_Running,          RVS_TargetState_PausedForEvent, RVS_TargetTransition_BarrierPause,          0 },
  { RVS_TargetState_PausedForEvent,   RVS_TargetState_Running,        RVS_TargetTransition_BarrierRollback,       0 },
  { RVS_TargetState_PausedForEvent,   RVS_TargetState_Running,        RVS_TargetTransition_BarrierResume,         0 },
  { RVS_TargetState_Running,          RVS_TargetState_Stopped,        RVS_TargetTransition_ConfirmedExit,         1 },
  { RVS_TargetState_PausedForEvent,   RVS_TargetState_Stopped,        RVS_TargetTransition_ConfirmedExit,         1 },
  { RVS_TargetState_Stopped,          RVS_TargetState_Removed,        RVS_TargetTransition_Retired,               1 },
  { RVS_TargetState_Queued,           RVS_TargetState_Removed,        RVS_TargetTransition_Retired,               1 },
  { RVS_TargetState_Running,          RVS_TargetState_Removed,        RVS_TargetTransition_Retired,               1 },
  { RVS_TargetState_PausedForEvent,   RVS_TargetState_Removed,        RVS_TargetTransition_Retired,               1 },
  { RVS_TargetState_StoppedAttention, RVS_TargetState_Removed,        RVS_TargetTransition_Retired,               1 },
  { RVS_TargetState_Terminating,      RVS_TargetState_Removed,        RVS_TargetTransition_Retired,               1 },
};

internal RVS_SchedulerOperationRule *
rvs_scheduler_operation_rule(RVS_SchedulerOp op)
{
  for EachIndex(rule_idx, ArrayCount(rvs_scheduler_operation_rules)) {
    if (rvs_scheduler_operation_rules[rule_idx].op == op) { return &rvs_scheduler_operation_rules[rule_idx]; }
  }
  return 0;
}

internal B32
rvs_scheduler_key_is_well_formed(RVS_SchedulerKey key)
{
  RVS_SchedulerOperationRule *rule = rvs_scheduler_operation_rule(key.op);
  if (rule == 0 || key.identity == 0) { return 0; }
  if (key.op == RVS_SchedulerOp_ReadOnly || key.op == RVS_SchedulerOp_TargetConfiguration) {
    return !MemoryIsZeroStruct(&key.target);
  }
  return MemoryIsZeroStruct(&key.target);
}

internal B32
rvs_scheduler_key_match(RVS_SchedulerKey a, RVS_SchedulerKey b)
{
  return a.op == b.op && dmn_handle_match(a.target, b.target) && a.identity == b.identity;
}

internal B32
rvs_scheduler_keys_conflict(RVS_SchedulerKey a, RVS_SchedulerKey b)
{
  RVS_SchedulerOperationRule *a_rule = rvs_scheduler_operation_rule(a.op);
  RVS_SchedulerOperationRule *b_rule = rvs_scheduler_operation_rule(b.op);
  if (a_rule == 0 || b_rule == 0) { return 0; }
  if (a_rule->scope == RVS_SchedulerScope_Session || b_rule->scope == RVS_SchedulerScope_Session) { return 1; }
  if (a.op == RVS_SchedulerOp_ReadOnly || b.op == RVS_SchedulerOp_ReadOnly) { return 0; }
  return a.op == b.op;
}

internal B32
rvs_scheduler_operation_targets_overlap(RVS_ScheduledOperation *operation, RVS_ProgramID *targets, U64 targets_count)
{
  for EachIndex(target_idx, targets_count) {
    for EachIndex(operation_target_idx, operation->targets_count) {
      if (dmn_handle_match(targets[target_idx], operation->targets[operation_target_idx].target)) { return 1; }
    }
  }
  return 0;
}

internal B32
rvs_scheduler_has_conflicting_operation_locked(RVS_Scheduler *scheduler, RVS_SchedulerKey key, RVS_ProgramID *targets, U64 targets_count)
{
  for EachNode(operation, RVS_ScheduledOperation, scheduler->operation_first) {
    if (key.op == RVS_SchedulerOp_ReadOnly || operation->key.op == RVS_SchedulerOp_ReadOnly) { continue; }
    if (key.op == RVS_SchedulerOp_Interrupt && operation->key.op == RVS_SchedulerOp_Run &&
        operation->state == RVS_ScheduledOperationState_Active) { continue; }
    if (key.op == RVS_SchedulerOp_Launch || operation->key.op == RVS_SchedulerOp_Launch) { return 1; }
    if (key.op == RVS_SchedulerOp_Terminate || operation->key.op == RVS_SchedulerOp_Terminate) { continue; }
    if (rvs_scheduler_operation_targets_overlap(operation, targets, targets_count)) { return 1; }
  }
  return 0;
}

internal void
rvs_scheduler_program_add_locked(RVS_Scheduler *scheduler, RVS_ProgramID target, U32 pid)
{
  AssertAlways(rvs_scheduler_program_from_id_locked(scheduler, target) == 0);
  RVS_Program *entry = push_array(scheduler->arena, RVS_Program, 1);
  entry->target = target;
  entry->pid = pid;
  entry->revision = 1;
  SLLQueuePush(scheduler->target_first, scheduler->target_last, entry);
}

internal RVS_Program *
rvs_scheduler_program_from_id_locked(RVS_Scheduler *scheduler, RVS_ProgramID target)
{
  for EachNode(entry, RVS_Program, scheduler->target_first) {
    if (dmn_handle_match(entry->target, target)) { return entry; }
  }
  return 0;
}

internal RVS_ThreadLedgerEntry *
rvs_scheduler_thread_from_id_locked(RVS_Scheduler *scheduler, RVS_ThreadID thread)
{
  for EachNode(entry, RVS_ThreadLedgerEntry, scheduler->thread_first) {
    if (dmn_handle_match(entry->thread, thread)) { return entry; }
  }
  return 0;
}

internal void
rvs_scheduler_thread_add_locked(RVS_Scheduler *scheduler, RVS_ProgramID target, RVS_ThreadID thread)
{
  if (dmn_handle_match(thread, dmn_handle_zero()) || rvs_scheduler_thread_from_id_locked(scheduler, thread)) { return; }
  RVS_ThreadLedgerEntry *entry = push_array(scheduler->arena, RVS_ThreadLedgerEntry, 1);
  entry->thread = thread;
  entry->target = target;
  SLLQueuePush(scheduler->thread_first, scheduler->thread_last, entry);
}

internal B32
rvs_scheduler_commit_target_locked(RVS_Program *entry, U64 expected_revision, RVS_TargetState next_state, RVS_TargetTransitionKind transition_kind)
{
  if (expected_revision != 0 && entry->revision != expected_revision) { return 0; }
  B32 state_changed = entry->state != next_state;
  B32 advances_revision = 0;
  if (transition_kind == RVS_TargetTransition_TerminationFence) {
    advances_revision = 1;
  } else {
    for EachIndex(rule_idx, ArrayCount(rvs_target_transition_rules)) {
      RVS_TargetTransitionRule *rule = &rvs_target_transition_rules[rule_idx];
      if (rule->source == entry->state && rule->destination == next_state && rule->kind == transition_kind) {
        advances_revision = rule->advances_revision;
        break;
      }
    }
    if ( ! state_changed || (transition_kind != RVS_TargetTransition_Shutdown && advances_revision == 0 &&
                             transition_kind != RVS_TargetTransition_Queue && transition_kind != RVS_TargetTransition_PreDispatchCancelled &&
                             transition_kind != RVS_TargetTransition_BarrierPause && transition_kind != RVS_TargetTransition_BarrierRollback &&
                             transition_kind != RVS_TargetTransition_BarrierResume)) {
      if (transition_kind != RVS_TargetTransition_Shutdown || !state_changed) { return 0; }
    }
  }
  if (state_changed) { entry->state = next_state; }
  if (advances_revision || (transition_kind == RVS_TargetTransition_Shutdown && state_changed)) { entry->revision += 1; }
  return 1;
}

internal U32
rvs_scheduler_stop_cause_priority(RVS_StopCause cause)
{
  switch (cause) {
  case RVS_StopCause_ProcessExit:    return 70;
  case RVS_StopCause_Exception:      return 60;
  case RVS_StopCause_Breakpoint:     return 50;
  case RVS_StopCause_UserInterrupt:  return 40;
  case RVS_StopCause_RunCompletion:  return 30;
  case RVS_StopCause_Incidental:     return 10;
  default: return 0;
  }
}

internal void
rvs_scheduler_note_primary_stop_locked(RVS_Scheduler *scheduler, RVS_StopCause cause, RVS_ProgramID target)
{
  RVS_StopTransaction *stop = &scheduler->stop_transaction;
  if (rvs_scheduler_stop_cause_priority(cause) >= rvs_scheduler_stop_cause_priority(stop->primary_cause)) {
    stop->primary_cause = cause;
    stop->primary_target = target;
  }
}

internal B32
rvs_scheduler_reserve_execution_locked(RVS_Scheduler *scheduler, RVS_TargetSnapshot *targets, U64 targets_count, RVS_MessageID request_id)
{
  if (request_id == 0 || targets == 0 || targets_count == 0) { return 0; }
  for EachNode(entry, RVS_Program, scheduler->target_first) {
    if (entry->state != RVS_TargetExecutionState_Idle && entry->state != RVS_TargetState_Removed) { return 0; }
  }
  for EachIndex(target_idx, targets_count) {
    if (rvs_scheduler_program_from_id_locked(scheduler, targets[target_idx].target) == 0) { return 0; }
  }
  for EachIndex(target_idx, targets_count) {
    RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, targets[target_idx].target);
    rvs_scheduler_commit_target_locked(entry, 0, RVS_TargetExecutionState_Queued, RVS_TargetTransition_Queue);
    entry->execution_token += 1;
    entry->execution_owner = request_id;
  }
  return 1;
}

internal B32
rvs_scheduler_mark_run_in_flight_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id)
{
  B32 found = 0;
  for EachNode(entry, RVS_Program, scheduler->target_first) {
    if (entry->execution_owner == request_id) {
      if (entry->state != RVS_TargetExecutionState_Queued) { return 0; }
      found = 1;
    }
  }
  if (found) {
    for EachNode(entry, RVS_Program, scheduler->target_first) {
      if (entry->execution_owner == request_id) {
        rvs_scheduler_commit_target_locked(entry, 0, RVS_TargetExecutionState_RunInFlight, RVS_TargetTransition_RunAccepted);
      }
    }
  }
  return found;
}

internal B32
rvs_scheduler_clear_queued_execution_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id)
{
  B32 found = 0;
  for EachNode(entry, RVS_Program, scheduler->target_first) {
    if (entry->execution_owner == request_id) {
      if (entry->state != RVS_TargetExecutionState_Queued) { return 0; }
      found = 1;
    }
  }
  if (found) {
    for EachNode(entry, RVS_Program, scheduler->target_first) {
      if (entry->execution_owner == request_id) {
        rvs_scheduler_commit_target_locked(entry, 0, RVS_TargetExecutionState_Idle, RVS_TargetTransition_PreDispatchCancelled);
        entry->execution_owner = 0;
      }
    }
  }
  return found;
}

internal B32
rvs_scheduler_finish_run_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id)
{
  B32 found = 0;
  for EachNode(entry, RVS_Program, scheduler->target_first) {
    if (entry->execution_owner == request_id) {
      if (entry->state != RVS_TargetExecutionState_RunInFlight) { return 0; }
      found = 1;
    }
  }
  if (found) {
    for EachNode(entry, RVS_Program, scheduler->target_first) {
      if (entry->execution_owner == request_id) {
        rvs_scheduler_commit_target_locked(entry, 0, RVS_TargetExecutionState_Idle, RVS_TargetTransition_ConfirmedStop);
        entry->execution_owner = 0;
      }
    }
  }
  return found;
}

internal RVS_Result
rvs_scheduler_finish_resume_transaction_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id)
{
  RVS_StopTransaction *stop = &scheduler->stop_transaction;
  RVS_ResumeTransaction *resume = &scheduler->resume_transaction;
  if (resume->owner == 0 || resume->owner->request->request_id != request_id ||
      resume->execution_owner == 0 || !stop->backend_stable || stop->stable_stop_generation == 0 ||
      resume->stable_stop_generation != stop->stable_stop_generation) { return RVS_Result_Error; }
  RVS_ScheduledOperation *operation = resume->owner;
  RVS_ExecutionOwner previous_owner = stop->previous_owner;
  for EachIndex(target_idx, operation->targets_count) {
    RVS_TargetSnapshot *snapshot = &operation->targets[target_idx];
    if (snapshot->exit_observed) { continue; }
    RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, snapshot->target);
    if (entry == 0 || entry->state != RVS_TargetExecutionState_InterruptPending ||
        entry->execution_owner != resume->execution_owner) {
      return RVS_Result_Error;
    }
    if (!snapshot->is_selected) {
      AssertAlways(rvs_scheduler_commit_target_locked(entry, 0, RVS_TargetExecutionState_RunInFlight,
                                                       RVS_TargetTransition_BarrierResume));
    }
  }
  for EachIndex(target_idx, operation->targets_count) {
    RVS_TargetSnapshot *snapshot = &operation->targets[target_idx];
    if (snapshot->is_selected && snapshot->exit_observed) {
      for EachIndex(selected_idx, operation->targets_count) {
        if (operation->targets[selected_idx].is_selected) {
          RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, operation->targets[selected_idx].target);
          if (entry && entry->state != RVS_TargetState_Removed) {
            rvs_scheduler_commit_target_locked(entry, 0, RVS_TargetExecutionState_Idle, RVS_TargetTransition_ConfirmedExit);
            entry->execution_owner = 0;
          }
        }
      }
      MemoryZeroStruct(stop);
      MemoryZeroStruct(resume);
      B32 execution_remains = 0;
      for EachNode(entry, RVS_Program, scheduler->target_first) {
        if (entry->execution_owner == previous_owner.request_id && entry->state == RVS_TargetExecutionState_RunInFlight) {
          execution_remains = 1;
          break;
        }
      }
      if (execution_remains) {
        scheduler->execution_owner = previous_owner;
        RVS_ScheduledOperation *interrupted = rvs_scheduler_operation_from_request_id_locked(scheduler, previous_owner.request_id);
        if (interrupted) {
          interrupted->state = RVS_ScheduledOperationState_Active;
          MemoryZeroStruct(&interrupted->interruption);
        }
        scheduler->phase = RVS_SchedulerPhase_Running;
      } else {
        MemoryZeroStruct(&scheduler->execution_owner);
        RVS_ScheduledOperation *interrupted = rvs_scheduler_operation_from_request_id_locked(scheduler, previous_owner.request_id);
        B32 has_live_target = 0;
        for EachIndex(target_idx, operation->targets_count) {
          RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, operation->targets[target_idx].target);
          if (entry && entry->state != RVS_TargetState_Removed) { has_live_target = 1; break; }
        }
        if (!has_live_target) {
          if (interrupted) { rvs_scheduler_retire_operation_locked(scheduler, interrupted, RVS_ScheduledOperationState_Completed); }
          scheduler->run_intent.state = RVS_RunIntentState_Completed;
        } else {
          scheduler->run_intent.state = RVS_RunIntentState_Suspended;
        }
        scheduler->phase = RVS_SchedulerPhase_Stopped;
      }
      return RVS_Result_StaleState;
    }
    if (snapshot->is_selected && !snapshot->stop_observed) { return RVS_Result_Error; }
  }
  for EachIndex(target_idx, operation->targets_count) {
    RVS_TargetSnapshot *snapshot = &operation->targets[target_idx];
    if (snapshot->is_selected && !snapshot->exit_observed) {
      RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, snapshot->target);
      AssertAlways(entry && entry->state == RVS_TargetExecutionState_InterruptPending &&
                   entry->execution_owner == resume->execution_owner);
      rvs_scheduler_commit_target_locked(entry, 0, RVS_TargetExecutionState_Idle, RVS_TargetTransition_ConfirmedStop);
      entry->execution_owner = 0;
    }
  }
  MemoryZeroStruct(stop);
  MemoryZeroStruct(resume);
  B32 execution_remains = 0;
  for EachNode(entry, RVS_Program, scheduler->target_first) {
    if (entry->execution_owner == previous_owner.request_id && entry->state == RVS_TargetExecutionState_RunInFlight) {
      execution_remains = 1;
      break;
    }
  }
  if (execution_remains) {
    scheduler->execution_owner = previous_owner;
    RVS_ScheduledOperation *interrupted = rvs_scheduler_operation_from_request_id_locked(scheduler, previous_owner.request_id);
    if (interrupted) {
      interrupted->state = RVS_ScheduledOperationState_Active;
      MemoryZeroStruct(&interrupted->interruption);
    }
    scheduler->phase = RVS_SchedulerPhase_Running;
  } else {
    MemoryZeroStruct(&scheduler->execution_owner);
    RVS_ScheduledOperation *interrupted = rvs_scheduler_operation_from_request_id_locked(scheduler, previous_owner.request_id);
    B32 has_live_target = 0;
    for EachIndex(target_idx, operation->targets_count) {
      RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, operation->targets[target_idx].target);
      if (entry && entry->state != RVS_TargetState_Removed) { has_live_target = 1; break; }
    }
    if (!has_live_target) {
      if (interrupted) { rvs_scheduler_retire_operation_locked(scheduler, interrupted, RVS_ScheduledOperationState_Completed); }
      scheduler->run_intent.state = RVS_RunIntentState_Completed;
    } else {
      scheduler->run_intent.state = RVS_RunIntentState_Suspended;
    }
    scheduler->phase = RVS_SchedulerPhase_Stopped;
  }
  return RVS_Result_Ok;
}

internal B32
rvs_scheduler_settle_stop_transaction_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id)
{
  rvs_scheduler_assert_reducing(scheduler);
  RVS_StopTransaction *stop = &scheduler->stop_transaction;
  if (stop->owner == 0 || stop->owner->request->request_id != request_id || stop->execution_owner == 0 ||
      (!stop->backend_stable && stop->phase != RVS_ControlTransactionPhase_Stable &&
       stop->phase != RVS_ControlTransactionPhase_WaitingForResume)) {
    return 0;
  }
  RVS_ExecutionOwner previous_owner = stop->previous_owner;
  B32 has_live_target = 0;
  for EachIndex(target_idx, stop->owner->targets_count) {
    RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, stop->owner->targets[target_idx].target);
    if (entry && entry->state != RVS_TargetState_Removed) { has_live_target = 1; break; }
  }
  for EachNode(entry, RVS_Program, scheduler->target_first) {
    if (entry->execution_owner == stop->execution_owner &&
        (entry->state == RVS_TargetExecutionState_RunInFlight ||
         entry->state == RVS_TargetExecutionState_InterruptPending)) {
      rvs_scheduler_commit_target_locked(entry, 0, RVS_TargetExecutionState_Idle, RVS_TargetTransition_ConfirmedStop);
      entry->execution_owner = 0;
    }
  }
  MemoryZeroStruct(stop);
  MemoryZeroStruct(&scheduler->resume_transaction);
  MemoryZeroStruct(&scheduler->execution_owner);
  scheduler->run_intent.state = has_live_target ? RVS_RunIntentState_Suspended : RVS_RunIntentState_Completed;
  scheduler->phase = RVS_SchedulerPhase_Stopped;
  if (previous_owner.kind == RVS_ExecutionOwnerKind_Operation) {
    RVS_ScheduledOperation *interrupted = rvs_scheduler_operation_from_request_id_locked(scheduler, previous_owner.request_id);
    if (interrupted) {
      interrupted->state = RVS_ScheduledOperationState_Suspended;
      interrupted->interruption.requires_reconciliation = 1;
    }
  }
  rvs_scheduler_retire_exhausted_suspended_locked(scheduler);
  return 1;
}

internal B32
rvs_scheduler_cancel_stop_transaction_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id)
{
  RVS_StopTransaction *stop = &scheduler->stop_transaction;
  if (stop->owner == 0 || stop->owner->request->request_id != request_id || stop->execution_owner == 0) { return 0; }
  RVS_ExecutionOwner previous_owner = stop->previous_owner;
  for EachNode(entry, RVS_Program, scheduler->target_first) {
    if (entry->state == RVS_TargetExecutionState_InterruptPending &&
        entry->execution_owner == stop->execution_owner) {
      rvs_scheduler_commit_target_locked(entry, 0, RVS_TargetExecutionState_RunInFlight, RVS_TargetTransition_BarrierRollback);
    }
  }
  MemoryZeroStruct(stop);
  MemoryZeroStruct(&scheduler->resume_transaction);
  if (previous_owner.kind == RVS_ExecutionOwnerKind_Operation) {
    scheduler->execution_owner = previous_owner;
    RVS_ScheduledOperation *interrupted = rvs_scheduler_operation_from_request_id_locked(scheduler, previous_owner.request_id);
    if (interrupted && interrupted->state == RVS_ScheduledOperationState_Suspended) {
      interrupted->state = RVS_ScheduledOperationState_Active;
      MemoryZeroStruct(&interrupted->interruption);
    }
  }
  scheduler->phase = RVS_SchedulerPhase_Running;
  return 1;
}

internal B32
rvs_scheduler_begin_interrupt_transaction_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation)
{
  rvs_scheduler_assert_reducing(scheduler);
  if (operation == 0 || operation->key.op != RVS_SchedulerOp_Interrupt || operation->targets_count == 0 ||
      scheduler->phase != RVS_SchedulerPhase_Running || scheduler->run_intent.state != RVS_RunIntentState_Active ||
      scheduler->stop_transaction.owner != 0 || scheduler->execution_owner.kind != RVS_ExecutionOwnerKind_Operation ||
      scheduler->run_intent.execution_owner != scheduler->execution_owner.request_id) {
    return 0;
  }
  RVS_Program *first_entry = rvs_scheduler_program_from_id_locked(scheduler, operation->targets[0].target);
  if (first_entry == 0 || first_entry->execution_owner != scheduler->execution_owner.request_id) { return 0; }
  for EachIndex(target_idx, operation->targets_count) {
    RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, operation->targets[target_idx].target);
    if (entry == 0 || entry->state != RVS_TargetExecutionState_RunInFlight ||
        entry->execution_owner != scheduler->execution_owner.request_id) {
      return 0;
    }
  }

  RVS_TargetSnapshot *selected_targets = operation->targets;
  U64 selected_targets_count = operation->targets_count;
  scheduler->next_control_transaction_id += 1;
  AssertAlways(scheduler->next_control_transaction_id != 0);
  scheduler->stop_transaction = (RVS_StopTransaction){
    .owner = operation,
    .previous_owner = scheduler->execution_owner,
    .execution_owner = first_entry->execution_owner,
    .transaction_id = scheduler->next_control_transaction_id,
    .phase = RVS_ControlTransactionPhase_PreparingInterrupt,
    .cycle_epoch = ++scheduler->run_cycle_epoch,
    .primary_cause = RVS_StopCause_UserInterrupt,
  };
  scheduler->execution_owner = rvs_execution_owner_control(operation->request->request_id,
                                                            scheduler->stop_transaction.transaction_id);
  RVS_ScheduledOperation *interrupted = rvs_scheduler_operation_from_request_id_locked(scheduler,
                                                                                        scheduler->stop_transaction.previous_owner.request_id);
#if RVS_ENGINE_TESTING
  if (interrupted) {
#else
  {
    AssertAlways(interrupted != 0);
#endif
    AssertAlways(interrupted->key.op == RVS_SchedulerOp_Run && interrupted->state == RVS_ScheduledOperationState_Active);
    interrupted->state = RVS_ScheduledOperationState_Suspended;
    interrupted->interruption = (RVS_OperationInterruption){
      .control_transaction_id = scheduler->stop_transaction.transaction_id,
      .cause = RVS_StopCause_UserInterrupt,
      .requires_reconciliation = 1,
    };
  }

  operation->targets_count = 0;
  for EachNode(entry, RVS_Program, scheduler->target_first) {
    if (entry->execution_owner == scheduler->stop_transaction.execution_owner) { operation->targets_count += 1; }
  }
  RVS_TargetSnapshotStorage *old_storage = operation->target_storage;
  operation->target_storage = rvs_scheduler_target_storage_acquire(scheduler, operation->targets_count);
  operation->targets = operation->target_storage->targets;
  U64 target_idx = 0;
  for EachNode(entry, RVS_Program, scheduler->target_first) {
    if (entry->execution_owner == scheduler->stop_transaction.execution_owner) {
      B32 is_selected = 0;
      for EachIndex(selected_idx, selected_targets_count) {
        if (dmn_handle_match(selected_targets[selected_idx].target, entry->target)) { is_selected = 1; break; }
      }
      operation->targets[target_idx++] = (RVS_TargetSnapshot){
        .target = entry->target,
        .revision = entry->revision,
        .execution_token = entry->execution_token,
        .is_selected = is_selected,
      };
    }
  }
  rvs_scheduler_target_storage_release(scheduler, old_storage);
  for EachIndex(sort_idx, operation->targets_count) {
    for (U64 previous_idx = sort_idx; previous_idx != 0 &&
         operation->targets[previous_idx].target.u64[0] < operation->targets[previous_idx - 1].target.u64[0];
         previous_idx -= 1) {
      Swap(RVS_TargetSnapshot, operation->targets[previous_idx], operation->targets[previous_idx - 1]);
    }
  }
  return 1;
}

internal void
rvs_scheduler_note_stop_transaction_locked(RVS_Scheduler *scheduler, RVS_Event *event)
{
  RVS_ScheduledOperation *operation = scheduler->stop_transaction.owner;
  if (operation == 0) { return; }
  for EachIndex(target_idx, operation->targets_count) {
    RVS_TargetSnapshot *snapshot = &operation->targets[target_idx];
    if (dmn_handle_match(snapshot->target, event->process)) {
      snapshot->stop_observed |= event->kind == DMN_EventKind_Halt;
      snapshot->exit_observed |= event->kind == DMN_EventKind_ExitProcess;
    }
  }
}

internal void
rvs_scheduler_resume_command_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation, RVS_SchedulerDecision *decision)
{
  RVS_StopTransaction *stop = &scheduler->stop_transaction;
  AssertAlways(stop->owner == operation && stop->backend_stable && stop->stable_stop_generation != 0 &&
               stop->phase == RVS_ControlTransactionPhase_Stable &&
               operation->pending_command.kind == RVS_SchedulerCommand_Null);
  RVS_ProgramID *targets = push_array(scheduler->arena, RVS_ProgramID, operation->targets_count);
  U64 targets_count = 0;
  for EachIndex(target_idx, operation->targets_count) {
    RVS_TargetSnapshot *target = &operation->targets[target_idx];
    if (!target->is_selected && !target->exit_observed) {
      RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, target->target);
      AssertAlways(target->stable_stop_generation == stop->stable_stop_generation && entry &&
                   entry->state == RVS_TargetExecutionState_InterruptPending &&
                   entry->execution_owner == stop->execution_owner);
      targets[targets_count++] = target->target;
    }
  }
  RVS_SchedulerCommand *command = rvs_scheduler_set_command_locked(scheduler, decision, RVS_SchedulerCommand_ResumeTargetSubset, operation);
  command->resume_target_subset.targets = targets;
  command->resume_target_subset.targets_count = targets_count;
  command->resume_target_subset.execution_request_id = scheduler->stop_transaction.execution_owner;
}

internal RVS_StableStopDisposition
rvs_plan_on_stable_stop_locked(RVS_Scheduler *scheduler, RVS_Plan *plan, RVS_StableStop const *stable_stop)
{
  rvs_scheduler_assert_reducing(scheduler);
  (void)stable_stop;
  if (plan->header.kind == RVS_PlanKind_Run) {
    return (RVS_StableStopDisposition){ .kind = RVS_StableStopDisposition_Continue };
  }
  if (plan->header.kind == RVS_PlanKind_RunToAddress) {
    // An explicit interrupt is unrelated to the temporary address trap; let the root RunPlan continue.
    return (RVS_StableStopDisposition){ .kind = RVS_StableStopDisposition_Unhandled };
  }
#if RVS_ENGINE_TESTING
  if (plan->header.kind == RVS_PlanKind_Test) { return plan->test_disposition; }
#endif
  return (RVS_StableStopDisposition){ .kind = RVS_StableStopDisposition_Unhandled };
}

internal RVS_StableStopDisposition
rvs_plan_dispatch_stable_stop_locked(RVS_Scheduler *scheduler, RVS_PlanID plan_id, RVS_StableStop const *stable_stop)
{
  rvs_scheduler_assert_reducing(scheduler);
  for (RVS_PlanID current_id = plan_id; current_id != 0;) {
    RVS_Plan *plan = rvs_scheduler_plan_from_id_locked(scheduler, current_id);
    AssertAlways(plan && plan->operation_request_id == stable_stop->execution_request_id);
#if RVS_ENGINE_TESTING
    scheduler->test_plan_dispatch_count += 1;
    if (scheduler->test_first_plan_visited == 0) { scheduler->test_first_plan_visited = current_id; }
    else if (scheduler->test_second_plan_visited == 0) { scheduler->test_second_plan_visited = current_id; }
#endif
    RVS_StableStopDisposition disposition = rvs_plan_on_stable_stop_locked(scheduler, plan, stable_stop);
    if (disposition.kind != RVS_StableStopDisposition_Unhandled) {
      scheduler->last_stop_policy_source = RVS_StopPolicySource_Plan;
      scheduler->decisive_plan_id = current_id;
      return disposition;
    }
    current_id = plan->header.parent;
  }
  return (RVS_StableStopDisposition){ .kind = RVS_StableStopDisposition_Unhandled };
}

internal RVS_StableStopDisposition
rvs_operation_handlers_dispatch_stable_stop_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation,
                                                    RVS_StableStop const *stable_stop)
{
  rvs_scheduler_assert_reducing(scheduler);
  (void)operation;
  (void)stable_stop;
#if RVS_ENGINE_TESTING
  scheduler->test_handler_dispatch_count += 1;
  return scheduler->test_handler_disposition;
#else
  return (RVS_StableStopDisposition){ .kind = RVS_StableStopDisposition_Unhandled };
#endif
}

internal B32
rvs_scheduler_stable_stop_dispatch_is_fresh_locked(RVS_Scheduler *scheduler, RVS_StableStop const *stable_stop)
{
  return stable_stop->stable_stop_generation != 0 &&
         scheduler->last_dispatched_stable_stop_generation != stable_stop->stable_stop_generation;
}

internal RVS_StableStopDisposition
rvs_scheduler_dispatch_stable_stop_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation,
                                          RVS_StableStop const *stable_stop, RVS_SchedulerDecision *decision)
{
  rvs_scheduler_assert_reducing(scheduler);
  RVS_StopTransaction *stop = &scheduler->stop_transaction;
  AssertAlways(stable_stop->stable_stop_generation != 0 &&
               stable_stop->stable_stop_generation == stop->stable_stop_generation && stop->backend_stable &&
               !stable_stop->amended_while_resume_pending &&
               stop->interrupt_token.request_id == 0 && stop->interrupt_token.command_id == 0 &&
               rvs_scheduler_stable_stop_dispatch_is_fresh_locked(scheduler, stable_stop) &&
               decision->command.kind == RVS_SchedulerCommand_Null);
  for EachIndex(target_idx, stable_stop->targets_count) {
    AssertAlways(stable_stop->targets[target_idx].stable_stop_generation == stable_stop->stable_stop_generation);
  }

  scheduler->last_dispatched_stable_stop_generation = stable_stop->stable_stop_generation;
  scheduler->last_stop_policy_source = RVS_StopPolicySource_Null;
  scheduler->decisive_plan_id = 0;
  scheduler->decisive_handler_id = 0;
  scheduler->last_stable_stop_used_fallback = 0;
  scheduler->last_stable_stop_command = RVS_SchedulerCommand_Null;
#if RVS_ENGINE_TESTING
  scheduler->test_plan_dispatch_count = 0;
  scheduler->test_handler_dispatch_count = 0;
  scheduler->test_first_plan_visited = 0;
  scheduler->test_second_plan_visited = 0;
#endif
  RVS_ExecutionOwner execution_owner_before = scheduler->execution_owner;
  RVS_ControlTransactionPhase phase_before = stop->phase;
  U64 next_command_id_before = scheduler->next_command_id;

  RVS_StableStopDisposition plan_disposition = rvs_plan_dispatch_stable_stop_locked(
    scheduler, operation ? operation->active_plan_id : 0, stable_stop);
  AssertAlways(rvs_execution_owner_match(scheduler->execution_owner, execution_owner_before) &&
               scheduler->stop_transaction.backend_stable && scheduler->stop_transaction.phase == phase_before &&
               scheduler->next_command_id == next_command_id_before && decision->command.kind == RVS_SchedulerCommand_Null);

  RVS_StableStopDisposition handler_disposition = { .kind = RVS_StableStopDisposition_Unhandled };
  if (plan_disposition.kind == RVS_StableStopDisposition_Unhandled) {
    handler_disposition = rvs_operation_handlers_dispatch_stable_stop_locked(scheduler, operation, stable_stop);
    AssertAlways(rvs_execution_owner_match(scheduler->execution_owner, execution_owner_before) &&
                 scheduler->stop_transaction.backend_stable && scheduler->stop_transaction.phase == phase_before &&
                 scheduler->next_command_id == next_command_id_before && decision->command.kind == RVS_SchedulerCommand_Null);
    if (handler_disposition.kind != RVS_StableStopDisposition_Unhandled) {
      scheduler->last_stop_policy_source = RVS_StopPolicySource_Handler;
#if RVS_ENGINE_TESTING
      scheduler->decisive_handler_id = scheduler->test_handler_id;
#endif
    }
  }

  scheduler->last_plan_stop_disposition = plan_disposition.kind;
  scheduler->last_handler_stop_disposition = handler_disposition.kind;
  return plan_disposition.kind != RVS_StableStopDisposition_Unhandled ? plan_disposition : handler_disposition;
}

internal void
rvs_scheduler_apply_stable_stop_continuation_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation,
                                                    RVS_StableStop const *stable_stop, RVS_SchedulerDecision *decision)
{
  rvs_scheduler_assert_reducing(scheduler);
  AssertAlways(operation == scheduler->stop_transaction.owner && scheduler->stop_transaction.backend_stable &&
               scheduler->stop_transaction.phase == RVS_ControlTransactionPhase_Stable &&
               stable_stop->stable_stop_generation == scheduler->stop_transaction.stable_stop_generation &&
               operation->pending_command.kind == RVS_SchedulerCommand_Null &&
               decision->command.kind == RVS_SchedulerCommand_Null);
  if (stable_stop->execution_lease_fully_exited) {
    AssertAlways(rvs_scheduler_settle_stop_transaction_locked(scheduler, operation->request->request_id));
    rvs_scheduler_complete_operation_locked(scheduler, operation, RVS_Result_StaleState, decision);
  } else {
    RVS_StopTransaction *stop = &scheduler->stop_transaction;
    scheduler->resume_transaction = (RVS_ResumeTransaction){
      .owner = operation,
      .execution_owner = stop->execution_owner,
      .cycle_epoch = scheduler->run_cycle_epoch,
      .stable_stop_generation = stop->stable_stop_generation,
      .resume_attempt = 1,
    };
    scheduler->phase = RVS_SchedulerPhase_PreparingResume;
    rvs_scheduler_resume_command_locked(scheduler, operation, decision);
    stop->phase = RVS_ControlTransactionPhase_WaitingForResume;
  }
  scheduler->last_stable_stop_command = decision->command.kind;
}

internal void
rvs_scheduler_apply_stable_stop_fallback_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation,
                                                RVS_StableStop const *stable_stop, RVS_SchedulerDecision *decision)
{
  rvs_scheduler_assert_reducing(scheduler);
  AssertAlways(scheduler->last_stop_policy_source == RVS_StopPolicySource_Null);
  scheduler->last_stop_policy_source = RVS_StopPolicySource_Fallback;
  scheduler->last_stable_stop_used_fallback = 1;
  rvs_scheduler_apply_stable_stop_continuation_locked(scheduler, operation, stable_stop, decision);
}

internal void
rvs_scheduler_establish_stable_stop_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation,
                                           RVS_SchedulerCommandToken interrupt_token,
                                           RVS_SchedulerDecision *decision)
{
  rvs_scheduler_assert_reducing(scheduler);
  RVS_StopTransaction *stop = &scheduler->stop_transaction;
  AssertAlways(stop->owner == operation && !stop->backend_stable && operation->pending_command.kind == RVS_SchedulerCommand_Null &&
               stop->interrupt_token.request_id == interrupt_token.request_id &&
               stop->interrupt_token.command_id == interrupt_token.command_id &&
               (stop->phase == RVS_ControlTransactionPhase_WaitingForInterrupt ||
                stop->phase == RVS_ControlTransactionPhase_CollectingBatch) &&
               (scheduler->phase == RVS_SchedulerPhase_Interrupting ||
                scheduler->phase == RVS_SchedulerPhase_CollectingStop));

  AssertAlways(scheduler->next_stable_stop_generation != max_U64);
  scheduler->next_stable_stop_generation += 1;
  AssertAlways(scheduler->next_stable_stop_generation != 0);
  stop->stable_stop_generation = scheduler->next_stable_stop_generation;
  MemoryZeroStruct(&stop->interrupt_token);

  for EachIndex(target_idx, operation->targets_count) {
    RVS_TargetSnapshot *target = &operation->targets[target_idx];
    target->stable_stop_generation = stop->stable_stop_generation;
    if (target->exit_observed) { continue; }
    RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, target->target);
    AssertAlways(entry && entry->execution_owner == stop->execution_owner &&
                 (entry->state == RVS_TargetExecutionState_RunInFlight ||
                  entry->state == RVS_TargetExecutionState_InterruptPending));
    if (entry->state == RVS_TargetExecutionState_RunInFlight) {
      AssertAlways(rvs_scheduler_commit_target_locked(entry, 0, RVS_TargetExecutionState_InterruptPending,
                                                       RVS_TargetTransition_BarrierPause));
    }
    target->stop_observed = 1;
  }

  RVS_ScheduledOperation *interrupted = 0;
  if (stop->previous_owner.kind == RVS_ExecutionOwnerKind_Operation) {
    interrupted = rvs_scheduler_operation_from_request_id_locked(scheduler, stop->previous_owner.request_id);
  }
  if (interrupted) {
    AssertAlways(interrupted->state == RVS_ScheduledOperationState_Suspended &&
                 interrupted->interruption.control_transaction_id == stop->transaction_id);
    interrupted->interruption.stable_stop_generation = stop->stable_stop_generation;
  }

  stop->backend_stable = 1;
  stop->phase = RVS_ControlTransactionPhase_Stable;
  scheduler->phase = RVS_SchedulerPhase_PublishingStop;

  RVS_TargetSnapshotStorage *stable_storage = rvs_scheduler_target_storage_acquire(scheduler, operation->targets_count);
  MemoryCopyTyped(stable_storage->targets, operation->targets, operation->targets_count);
  B32 execution_lease_fully_exited = operation->targets_count != 0;
  for EachIndex(target_idx, operation->targets_count) {
    execution_lease_fully_exited &= operation->targets[target_idx].exit_observed;
  }
  RVS_StableStop stable_stop = {
    .stable_stop_generation = stop->stable_stop_generation,
    .execution_request_id = stop->execution_owner,
    .interrupt_token = interrupt_token,
    .targets = stable_storage->targets,
    .targets_count = operation->targets_count,
    .primary_target = stop->primary_target,
    .primary_cause = stop->primary_cause,
    .execution_lease_fully_exited = execution_lease_fully_exited,
    .amended_while_resume_pending = stop->amended_while_resume_pending,
  };
  RVS_StableStopDisposition disposition = rvs_scheduler_dispatch_stable_stop_locked(scheduler, interrupted, &stable_stop, decision);
  AssertAlways(disposition.kind == RVS_StableStopDisposition_Unhandled ||
               disposition.kind == RVS_StableStopDisposition_Continue);
  if (disposition.kind == RVS_StableStopDisposition_Continue) {
    rvs_scheduler_apply_stable_stop_continuation_locked(scheduler, operation, &stable_stop, decision);
  } else {
    rvs_scheduler_apply_stable_stop_fallback_locked(scheduler, operation, &stable_stop, decision);
  }
  rvs_scheduler_target_storage_release(scheduler, stable_storage);
}

internal B32
rvs_scheduler_observe_stop_transaction_locked(RVS_Scheduler *scheduler, RVS_SchedulerEvent event, DMN_EventKind event_kind)
{
  RVS_StopTransaction *stop = &scheduler->stop_transaction;
  RVS_ScheduledOperation *operation = stop->owner;
  B32 matches_target = 0;
  if (operation) {
    for EachIndex(target_idx, operation->targets_count) {
      RVS_TargetSnapshot *target = &operation->targets[target_idx];
      if (dmn_handle_match(target->target, event.observed.target) &&
          (event.observed.execution_token == 0 || target->execution_token == event.observed.execution_token)) {
        matches_target = 1;
        break;
      }
    }
  }
  B32 collecting_stop = (scheduler->phase == RVS_SchedulerPhase_Interrupting ||
                         scheduler->phase == RVS_SchedulerPhase_CollectingStop) && !stop->backend_stable;
  B32 amending_stable_stop = event_kind == DMN_EventKind_ExitProcess && stop->backend_stable &&
                             stop->phase == RVS_ControlTransactionPhase_WaitingForResume;
  if ((!collecting_stop && !amending_stable_stop) || operation == 0 ||
      event.observed.request_id != stop->execution_owner || !matches_target) { return 0; }
  rvs_scheduler_note_stop_transaction_locked(scheduler, &(RVS_Event){ .kind = event_kind, .process = event.observed.target });
  if (amending_stable_stop) { stop->amended_while_resume_pending = 1; }
  return 1;
}

internal B32
rvs_scheduler_observe_target_locked(RVS_Scheduler *scheduler, RVS_SchedulerEvent event, DMN_EventKind event_kind)
{
  RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, event.observed.target);
  if (entry == 0 || event.observed.request_id != entry->execution_owner ||
      (event.observed.execution_token != 0 && entry->execution_token != event.observed.execution_token)) {
    return 0;
  }
  B32 observed_global = rvs_scheduler_observe_stop_transaction_locked(scheduler, event, event_kind);
  if (event_kind == DMN_EventKind_Halt) {
    return observed_global;
  }
  rvs_scheduler_commit_target_locked(entry, 0, RVS_TargetExecutionState_Idle, RVS_TargetTransition_ConfirmedExit);
  entry->execution_owner = 0;
  return 1;
}

internal void
rvs_scheduler_retire_exhausted_suspended_locked(RVS_Scheduler *scheduler)
{
  rvs_scheduler_assert_reducing(scheduler);
  RVS_ScheduledOperation *exhausted = 0;
  for EachNode(operation, RVS_ScheduledOperation, scheduler->operation_first) {
    if (operation->key.op != RVS_SchedulerOp_Run || operation->state != RVS_ScheduledOperationState_Suspended) { continue; }
    B32 has_live_target = 0;
    for EachIndex(target_idx, operation->targets_count) {
      RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, operation->targets[target_idx].target);
      if (entry && entry->state != RVS_TargetState_Removed) { has_live_target = 1; break; }
    }
    if (!has_live_target) {
      AssertAlways(exhausted == 0);
      exhausted = operation;
    }
  }
  if (exhausted) {
    AssertAlways(exhausted->request_completed && exhausted->pending_command.kind == RVS_SchedulerCommand_Null);
    rvs_scheduler_retire_operation_locked(scheduler, exhausted, RVS_ScheduledOperationState_Completed);
  }
}

internal B32
rvs_scheduler_retire_target_locked(RVS_Scheduler *scheduler, RVS_Program *entry, RVS_MessageID execution_owner, DMN_Event *demon_event)
{
  rvs_scheduler_assert_reducing(scheduler);
  if (entry == 0 || entry->state == RVS_TargetState_Removed) { return 0; }
  if (entry->execution_owner != 0) {
    rvs_scheduler_observe_stop_transaction_locked(scheduler, (RVS_SchedulerEvent){
      .observed = { .request_id = execution_owner, .target = entry->target },
    }, DMN_EventKind_ExitProcess);
  }
  rvs_scheduler_commit_target_locked(entry, 0, RVS_TargetState_Removed, RVS_TargetTransition_Retired);
  entry->is_termination_fenced = 1;
  entry->execution_owner = 0;
  entry->execution_token = 0;
  entry->exit_code = demon_event->code;
  for EachNode(thread, RVS_ThreadLedgerEntry, scheduler->thread_first) {
    if (dmn_handle_match(thread->target, entry->target)) { thread->is_removed = 1; }
  }
  if (dmn_handle_match(scheduler->selected_target, entry->target)) {
    scheduler->selected_target = dmn_handle_zero();
    scheduler->selected_thread = dmn_handle_zero();
  }
  if (scheduler->execution_owner.kind == RVS_ExecutionOwnerKind_Operation) {
    B32 execution_remains = 0;
    for EachNode(other, RVS_Program, scheduler->target_first) {
      if (other->execution_owner == scheduler->execution_owner.request_id) {
        execution_remains = 1;
        break;
      }
    }
    if (!execution_remains) {
      RVS_ScheduledOperation *operation = rvs_scheduler_operation_from_request_id_locked(scheduler,
                                                                                          scheduler->execution_owner.request_id);
      if (operation) {
        AssertAlways(operation->key.op == RVS_SchedulerOp_Run && operation->request_completed);
        rvs_scheduler_retire_operation_locked(scheduler, operation, RVS_ScheduledOperationState_Completed);
      }
      MemoryZeroStruct(&scheduler->execution_owner);
      MemoryZeroStruct(&scheduler->stop_transaction);
      MemoryZeroStruct(&scheduler->resume_transaction);
      scheduler->run_intent.state = RVS_RunIntentState_Completed;
      scheduler->phase = RVS_SchedulerPhase_Stopped;
    }
  }
  rvs_scheduler_retire_exhausted_suspended_locked(scheduler);
  return 1;
}

internal void
rvs_scheduler_release_execution_leases_locked(RVS_Scheduler *scheduler)
{
  for EachNode(entry, RVS_Program, scheduler->target_first) {
    if (entry->state != RVS_TargetExecutionState_Idle && entry->state != RVS_TargetState_Removed) {
      rvs_scheduler_commit_target_locked(entry, 0, RVS_TargetExecutionState_Idle, RVS_TargetTransition_Shutdown);
      entry->execution_owner = 0;
    }
  }
}

internal B32
rvs_scheduler_execution_blocks_operation_locked(RVS_Scheduler *scheduler, RVS_SchedulerOp op)
{
  if (op != RVS_SchedulerOp_Run && op != RVS_SchedulerOp_Launch) { return 0; }
  for EachNode(entry, RVS_Program, scheduler->target_first) {
    if (entry->state != RVS_TargetExecutionState_Idle && entry->state != RVS_TargetState_Removed) { return 1; }
  }
  return 0;
}

internal B32
rvs_scheduler_targets_are_stopped_and_unfenced_locked(RVS_Scheduler *scheduler, RVS_ProgramID *targets, U64 targets_count)
{
  if (targets == 0 || targets_count == 0) { return 0; }
  for EachIndex(target_idx, targets_count) {
    RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, targets[target_idx]);
    if (entry == 0 || entry->is_termination_fenced || entry->state != RVS_TargetExecutionState_Idle) { return 0; }
  }
  return 1;
}

internal B32
rvs_scheduler_targets_are_active_locked(RVS_Scheduler *scheduler, RVS_ProgramID *targets, U64 targets_count)
{
  if (targets == 0 || targets_count == 0) { return 0; }
  RVS_MessageID execution_request_id = 0;
  for EachIndex(target_idx, targets_count) {
    RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, targets[target_idx]);
    if (entry == 0 || entry->is_termination_fenced || entry->state != RVS_TargetExecutionState_RunInFlight) { return 0; }
    if (execution_request_id == 0) { execution_request_id = entry->execution_owner; }
    else if (entry->execution_owner != execution_request_id) { return 0; }
  }
  return 1;
}

internal B32
rvs_scheduler_targets_are_fenced_or_missing_locked(RVS_Scheduler *scheduler, RVS_ProgramID *targets, U64 targets_count)
{
  if (targets == 0 || targets_count == 0) { return 1; }
  for EachIndex(target_idx, targets_count) {
    RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, targets[target_idx]);
    if (entry == 0 || entry->is_termination_fenced) { return 1; }
  }
  return 0;
}

internal RVS_Request *
rvs_scheduler_terminal_request_alloc(RVS_RequestPool *pool, RVS_MessageID request_id, RVS_Result result)
{
  RVS_Request *request = rvs_request_pool_request_alloc(pool);
  request->ref_count = 1;
  request->request_id = request_id;
  request->reply = (RVS_EngineReply){ .request_id = request_id, .result = result };
  return request;
}

internal RVS_TargetSnapshotStorage *
rvs_scheduler_target_storage_acquire(RVS_Scheduler *scheduler, U64 capacity)
{
  mutex_take(scheduler->recycle_mutex);
  RVS_TargetSnapshotStorage **storage_ptr = &scheduler->target_storage_free_first;
  while (*storage_ptr && (*storage_ptr)->capacity < capacity) { storage_ptr = &(*storage_ptr)->next; }
  RVS_TargetSnapshotStorage *storage = *storage_ptr;
  if (storage) {
    *storage_ptr = storage->next;
  } else {
    storage = push_array(scheduler->arena, RVS_TargetSnapshotStorage, 1);
    storage->capacity = capacity;
    storage->targets = push_array(scheduler->arena, RVS_TargetSnapshot, capacity);
  }
  mutex_drop(scheduler->recycle_mutex);
  return storage;
}

internal void
rvs_scheduler_target_storage_release(RVS_Scheduler *scheduler, RVS_TargetSnapshotStorage *storage)
{
  if (storage) {
    mutex_take(scheduler->recycle_mutex);
    storage->next = scheduler->target_storage_free_first;
    scheduler->target_storage_free_first = storage;
    mutex_drop(scheduler->recycle_mutex);
  }
}

internal RVS_Plan *
rvs_scheduler_plan_from_id_locked(RVS_Scheduler *scheduler, RVS_PlanID plan_id)
{
  if (plan_id == 0) { return 0; }
  for EachNode(plan, RVS_Plan, scheduler->plan_first) {
    if (plan->header.id == plan_id) { return plan; }
  }
  return 0;
}

internal RVS_Plan *
rvs_scheduler_plan_alloc_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation,
                                RVS_PlanKind kind, RVS_PlanID parent)
{
  AssertAlways(operation && kind != RVS_PlanKind_Null && scheduler->next_plan_id != max_U64);
  if (parent != 0) {
    RVS_Plan *parent_plan = rvs_scheduler_plan_from_id_locked(scheduler, parent);
    AssertAlways(parent_plan && parent_plan->operation_request_id == operation->request->request_id);
  }
  mutex_take(scheduler->recycle_mutex);
  RVS_Plan *plan = scheduler->plan_free_first;
  if (plan) { scheduler->plan_free_first = plan->next; }
  else { plan = push_array(scheduler->arena, RVS_Plan, 1); }
  MemoryZeroStruct(plan);
  mutex_drop(scheduler->recycle_mutex);

  scheduler->next_plan_id += 1;
  AssertAlways(scheduler->next_plan_id != 0);
  plan->header = (RVS_PlanHeader){ .id = scheduler->next_plan_id, .parent = parent, .kind = kind };
  plan->operation_request_id = operation->request->request_id;
  DLLPushBack(scheduler->plan_first, scheduler->plan_last, plan);
  return plan;
}

internal RVS_Plan *
rvs_scheduler_attach_run_to_address_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation,
                                           RVS_ProgramID target, U64 vaddr)
{
  AssertAlways(operation && operation->key.op == RVS_SchedulerOp_Run && operation->root_plan_id != 0 &&
               operation->active_plan_id == operation->root_plan_id &&
               !dmn_handle_match(target, dmn_handle_zero()) && vaddr != 0);
  RVS_Plan *plan = rvs_scheduler_plan_alloc_locked(scheduler, operation, RVS_PlanKind_RunToAddress,
                                                    operation->root_plan_id);
  plan->run_to_address.target = target;
  plan->run_to_address.vaddr = vaddr;
  operation->active_plan_id = plan->header.id;
  return plan;
}

internal void
rvs_scheduler_plans_release_operation_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation)
{
  for (RVS_Plan *plan = scheduler->plan_first, *next = 0; plan; plan = next) {
    next = plan->next;
    if (plan->operation_request_id != operation->request->request_id) { continue; }
    DLLRemove(scheduler->plan_first, scheduler->plan_last, plan);
    mutex_take(scheduler->recycle_mutex);
    plan->next = scheduler->plan_free_first;
    scheduler->plan_free_first = plan;
    mutex_drop(scheduler->recycle_mutex);
  }
  operation->root_plan_id = 0;
  operation->active_plan_id = 0;
}

internal RVS_ScheduledOperation *
rvs_scheduler_operation_alloc_locked(RVS_Scheduler *scheduler, RVS_RequestPool *expected_pool, RVS_MessageID request_id, RVS_SchedulerKey key, RVS_ProgramID *targets, U64 targets_count, U64 captured_program_state_epoch)
{
  mutex_take(scheduler->recycle_mutex);
  RVS_ScheduledOperation *operation = scheduler->operation_free_first;
  if (operation) {
    scheduler->operation_free_first = operation->next;
  } else {
    operation = push_array(scheduler->arena, RVS_ScheduledOperation, 1);
  }
  MemoryZeroStruct(operation);
  mutex_drop(scheduler->recycle_mutex);
  RVS_Request *request = rvs_request_pool_request_alloc(expected_pool);
  request->ref_count = 1; // caller ownership
  request->request_id = request_id;
  request->reply.request_id = request_id;
  request->reply.result = RVS_Result_Pending;

  operation->scheduler = scheduler;
  operation->request = request;
  operation->ref_count = 1; // active registration ownership
  operation->state = RVS_ScheduledOperationState_Queued;
  operation->key = key;
  operation->captured_program_state_epoch = captured_program_state_epoch;
  if (targets_count != 0) {
    RVS_TargetSnapshotStorage *storage = rvs_scheduler_target_storage_acquire(scheduler, targets_count);
    operation->target_storage = storage;
    operation->targets = storage->targets;
    operation->targets_count = targets_count;
    for EachIndex(target_idx, targets_count) {
      RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, targets[target_idx]);
      if (entry) {
        operation->targets[target_idx] = (RVS_TargetSnapshot){
          .target = entry->target,
          .revision = entry->revision,
          .execution_token = entry->execution_token,
          .is_termination_fenced = entry->is_termination_fenced,
        };
      }
      for (U64 previous_idx = target_idx; previous_idx != 0 && operation->targets[previous_idx].target.u64[0] < operation->targets[previous_idx - 1].target.u64[0]; previous_idx -= 1) {
        Swap(RVS_TargetSnapshot, operation->targets[previous_idx], operation->targets[previous_idx - 1]);
      }
    }
  }
  rvs_request_addref(request); // operation ownership
  if (key.op == RVS_SchedulerOp_Run) {
    RVS_Plan *run_plan = rvs_scheduler_plan_alloc_locked(scheduler, operation, RVS_PlanKind_Run, 0);
    operation->root_plan_id = run_plan->header.id;
    operation->active_plan_id = run_plan->header.id;
  }
  DLLPushBack(scheduler->operation_first, scheduler->operation_last, operation);
  return operation;
}

internal void
rvs_scheduler_operation_addref(RVS_ScheduledOperation *operation)
{
  AssertAlways(operation != 0);
  ins_atomic_u32_inc_eval(&operation->ref_count);
}

internal void
rvs_scheduler_operation_release(RVS_ScheduledOperation *operation)
{
  AssertAlways(operation != 0);
  if (ins_atomic_u32_dec_eval(&operation->ref_count) == 0) {
    RVS_Scheduler *scheduler = operation->scheduler;
    rvs_request_release(operation->request);
    mutex_take(scheduler->recycle_mutex);
    if (operation->target_storage) {
      operation->target_storage->next = scheduler->target_storage_free_first;
      scheduler->target_storage_free_first = operation->target_storage;
    }
    operation->next = scheduler->operation_free_first;
    scheduler->operation_free_first = operation;
    mutex_drop(scheduler->recycle_mutex);
  }
}

internal void
rvs_scheduler_operation_remove_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation)
{
  rvs_scheduler_plans_release_operation_locked(scheduler, operation);
  DLLRemove(scheduler->operation_first, scheduler->operation_last, operation);
}

internal void
rvs_scheduler_operation_key_remove_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation)
{
  if (operation->key_prev) { operation->key_prev->key_next = operation->key_next; }
  else { scheduler->key_first = operation->key_next; }
  if (operation->key_next) { operation->key_next->key_prev = operation->key_prev; }
  else { scheduler->key_last = operation->key_prev; }
  operation->key_next = 0;
  operation->key_prev = 0;
}

internal RVS_Result
rvs_scheduler_register_operation_locked(RVS_Scheduler *scheduler, RVS_RequestPool *expected_pool, RVS_SchedulerKey key, RVS_ScheduledOperation *operation)
{
  RVS_Result result = RVS_Result_Error;
  if (operation && operation->request->pool == expected_pool && rvs_scheduler_key_is_well_formed(key)) {
    result = RVS_Result_Ok;
    for (RVS_ScheduledOperation *n = scheduler->key_first; n; n = n->key_next) {
      if (rvs_scheduler_key_match(n->key, key)) { result = RVS_Result_AlreadyPending; break; }
    }
    if (result == RVS_Result_Ok) {
      operation->key = key;
      operation->is_keyed = 1;
      rvs_scheduler_operation_addref(operation); // keyed registration ownership
      operation->key_prev = scheduler->key_last;
      if (scheduler->key_last) { scheduler->key_last->key_next = operation; }
      else { scheduler->key_first = operation; }
      scheduler->key_last = operation;
    }
  }
  return result;
}

internal RVS_ScheduledOperation *
rvs_scheduler_unregister_operation_locked(RVS_Scheduler *scheduler, RVS_SchedulerKey key, RVS_ScheduledOperation *expected_operation)
{
  for (RVS_ScheduledOperation *n = scheduler->key_first; n; n = n->key_next) {
    if (n == expected_operation && rvs_scheduler_key_match(n->key, key)) {
      rvs_scheduler_operation_key_remove_locked(scheduler, n);
      n->is_keyed = 0;
      return n;
    }
  }
  return 0;
}

internal RVS_ScheduledOperation *
rvs_scheduler_operation_from_request_id_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id)
{
  for EachNode(n, RVS_ScheduledOperation, scheduler->operation_first) {
    if (n->request->request_id == request_id) { return n; }
  }
  return 0;
}

internal void
rvs_scheduler_prepare_reply_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation, RVS_EngineReply *reply)
{
  if (operation->key.op == RVS_SchedulerOp_ReadOnly) {
    reply->program_state_epoch = operation->captured_program_state_epoch;
    RVS_Program *key_entry = rvs_scheduler_program_from_id_locked(scheduler, operation->key.target);
    if (key_entry == 0 || reply->program_state_epoch != key_entry->revision) {
      reply->result = RVS_Result_StaleState;
    }
    for EachIndex(target_idx, operation->targets_count) {
      RVS_TargetSnapshot *snapshot = &operation->targets[target_idx];
      RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, snapshot->target);
      if (entry == 0 || entry->is_termination_fenced || entry->revision != snapshot->revision) {
        reply->result = RVS_Result_StaleState;
        break;
      }
    }
  }
}

internal void
rvs_scheduler_complete_suspended_for_run_admission_locked(RVS_Scheduler *scheduler, RVS_ProgramID *targets, U64 targets_count)
{
  AssertAlways(scheduler->execution_owner.kind == RVS_ExecutionOwnerKind_Null && scheduler->stop_transaction.owner == 0);
  if (!rvs_scheduler_targets_are_stopped_and_unfenced_locked(scheduler, targets, targets_count)) { return; }
  RVS_ScheduledOperation *suspended = 0;
  for EachNode(operation, RVS_ScheduledOperation, scheduler->operation_first) {
    if (operation->key.op == RVS_SchedulerOp_Run && operation->state == RVS_ScheduledOperationState_Suspended) {
      AssertAlways(suspended == 0);
      suspended = operation;
    }
  }
  if (suspended) {
    AssertAlways(suspended->request_completed && suspended->pending_command.kind == RVS_SchedulerCommand_Null);
    suspended->state = RVS_ScheduledOperationState_Completed;
    rvs_scheduler_operation_remove_locked(scheduler, suspended);
    rvs_scheduler_operation_release(suspended);
  }
}

internal RVS_Result
rvs_scheduler_admit_locked(RVS_Scheduler *scheduler, RVS_RequestPool *expected_pool, U64 *next_request_id, RVS_SchedulerOp op, RVS_SchedulerKey key, RVS_ProgramID *targets, U64 targets_count, U64 captured_program_state_epoch, RVS_SchedulerAdmission *admission_out)
{
  MemoryZeroStruct(admission_out);
  RVS_SchedulerOperationRule *rule = rvs_scheduler_operation_rule(op);
  if (rule == 0 || key.op != op || !rvs_scheduler_key_is_well_formed(key)) { return RVS_Result_Error; }
  RVS_ProgramID keyed_target = key.target;
  if (op == RVS_SchedulerOp_ReadOnly) {
    targets = &keyed_target;
    targets_count = 1;
    if (scheduler->stop_transaction.owner != 0) {
      return RVS_Result_AlreadyPending;
    }
    if (rvs_scheduler_targets_are_fenced_or_missing_locked(scheduler, targets, targets_count)) {
      RVS_Request *request = rvs_scheduler_terminal_request_alloc(expected_pool, ins_atomic_u64_inc_eval(next_request_id), RVS_Result_StaleState);
      admission_out->request = request;
      admission_out->is_terminal = 1;
      return RVS_Result_Ok;
    }
  }
  if ((rule->target_shape == RVS_SchedulerTargetShape_None && targets_count != 0) ||
      (rule->target_shape == RVS_SchedulerTargetShape_One && targets_count != 1) ||
      (rule->target_shape == RVS_SchedulerTargetShape_OneOrMore && targets_count == 0)) {
    return RVS_Result_Error;
  }
  if (op != RVS_SchedulerOp_Launch && op != RVS_SchedulerOp_ReadOnly &&
      rvs_scheduler_targets_are_fenced_or_missing_locked(scheduler, targets, targets_count)) {
    return RVS_Result_StaleState;
  }
  if (rule->duplicate == RVS_SchedulerDuplicate_Join) {
    for (RVS_ScheduledOperation *n = scheduler->key_first; n; n = n->key_next) {
      if (rvs_scheduler_key_match(n->key, key) &&
          (op != RVS_SchedulerOp_ReadOnly || n->captured_program_state_epoch == captured_program_state_epoch)) {
        rvs_request_addref(n->request);
        admission_out->operation = n;
        admission_out->request = n->request;
        admission_out->joined = 1;
        return RVS_Result_Ok;
      }
    }
  }
  if (op == RVS_SchedulerOp_Run && scheduler->execution_owner.kind == RVS_ExecutionOwnerKind_Null &&
      scheduler->stop_transaction.owner == 0) {
    rvs_scheduler_complete_suspended_for_run_admission_locked(scheduler, targets, targets_count);
  }
  if (rvs_scheduler_has_conflicting_operation_locked(scheduler, key, targets, targets_count)) {
    return RVS_Result_AlreadyPending;
  }

  RVS_AdmissionRequirement admission_requirement = rule->admission_requirement;
  if (admission_requirement == RVS_AdmissionRequirement_StoppedUnfenced) {
    if ( ! rvs_scheduler_targets_are_stopped_and_unfenced_locked(scheduler, targets, targets_count)) {
      return RVS_Result_AlreadyPending;
    }
  } else if (admission_requirement == RVS_AdmissionRequirement_ActiveOneLease) {
    if (scheduler->phase != RVS_SchedulerPhase_Running || scheduler->stop_transaction.owner != 0 ||
        !rvs_scheduler_targets_are_active_locked(scheduler, targets, targets_count)) {
      return RVS_Result_AlreadyPending;
    }
#if RVS_ENGINE_TESTING
    RVS_ScheduledOperation *owner_operation = scheduler->execution_owner.kind == RVS_ExecutionOwnerKind_Operation ?
                                               rvs_scheduler_operation_from_request_id_locked(scheduler, scheduler->execution_owner.request_id) : 0;
    if (scheduler->execution_owner.kind == RVS_ExecutionOwnerKind_Null ||
        (scheduler->execution_owner.kind == RVS_ExecutionOwnerKind_Operation && owner_operation == 0)) {
      RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, targets[0]);
      scheduler->execution_owner = rvs_execution_owner_operation(entry->execution_owner);
    }
#endif
    if (scheduler->execution_owner.kind != RVS_ExecutionOwnerKind_Operation) { return RVS_Result_AlreadyPending; }
  } else if (admission_requirement == RVS_AdmissionRequirement_LiveUnfenced) {
    if (rvs_scheduler_targets_are_fenced_or_missing_locked(scheduler, targets, targets_count)) {
      return RVS_Result_StaleState;
    }
  }

  RVS_ScheduledOperation *operation = rvs_scheduler_operation_alloc_locked(scheduler, expected_pool, ins_atomic_u64_inc_eval(next_request_id), key, targets, targets_count, captured_program_state_epoch);
  if (rule->duplicate == RVS_SchedulerDuplicate_Join) {
    AssertAlways(rvs_scheduler_register_operation_locked(scheduler, expected_pool, key, operation) == RVS_Result_Ok);
    admission_out->registered = 1;
  }
  if (rule->transition == RVS_SchedulerTransition_Run) {
    AssertAlways(rvs_scheduler_reserve_execution_locked(scheduler, operation->targets, operation->targets_count, operation->request->request_id));
    AssertAlways(scheduler->execution_owner.kind == RVS_ExecutionOwnerKind_Null);
    scheduler->execution_owner = rvs_execution_owner_operation(operation->request->request_id);
  } else if (rule->transition == RVS_SchedulerTransition_Terminate) {
    for EachIndex(target_idx, operation->targets_count) {
      RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, operation->targets[target_idx].target);
      entry->is_termination_fenced = 1;
      rvs_scheduler_commit_target_locked(entry, 0, entry->state, RVS_TargetTransition_TerminationFence);
    }
  }
  admission_out->operation = operation;
  admission_out->request = operation->request;
  return RVS_Result_Ok;
}

internal void
rvs_scheduler_rollback_admission_locked(RVS_Scheduler *scheduler, RVS_SchedulerAdmission *admission)
{
  AssertAlways(admission->operation != 0 && !admission->joined);
  RVS_ScheduledOperation *operation = admission->operation;
  RVS_SchedulerOperationRule *rule = rvs_scheduler_operation_rule(operation->key.op);
  AssertAlways(rule != 0);
  if (rule->transition == RVS_SchedulerTransition_Run) {
    rvs_scheduler_clear_queued_execution_locked(scheduler, operation->request->request_id);
    if (scheduler->execution_owner.kind == RVS_ExecutionOwnerKind_Operation &&
        scheduler->execution_owner.request_id == operation->request->request_id) {
      MemoryZeroStruct(&scheduler->execution_owner);
    }
  } else if (rule->transition == RVS_SchedulerTransition_Terminate) {
    for EachIndex(target_idx, operation->targets_count) {
      RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, operation->targets[target_idx].target);
      entry->is_termination_fenced = operation->targets[target_idx].is_termination_fenced;
      entry->revision = operation->targets[target_idx].revision;
    }
  } else if (rule->transition == RVS_SchedulerTransition_Interrupt && scheduler->stop_transaction.owner == operation) {
    for EachIndex(target_idx, operation->targets_count) {
      if (operation->targets[target_idx].is_selected) {
        RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, operation->targets[target_idx].target);
        rvs_scheduler_commit_target_locked(entry, 0, RVS_TargetExecutionState_RunInFlight, RVS_TargetTransition_BarrierRollback);
      }
    }
    MemoryZeroStruct(&scheduler->stop_transaction);
    MemoryZeroStruct(&scheduler->resume_transaction);
    scheduler->phase = RVS_SchedulerPhase_Running;
  }
  rvs_scheduler_operation_remove_locked(scheduler, operation);
  RVS_ScheduledOperation *registered_operation = 0;
  if (admission->registered) {
    registered_operation = rvs_scheduler_unregister_operation_locked(scheduler, operation->key, operation);
    AssertAlways(registered_operation == operation);
  }
  rvs_request_release(operation->request); // drop unreturned caller ownership
  if (registered_operation) { rvs_scheduler_operation_release(registered_operation); }
  rvs_scheduler_operation_release(operation); // drop active registration ownership
  MemoryZeroStruct(admission);
}

internal RVS_RequestControl *
rvs_request_control_alloc(RVS_Session *session, RVS_ScheduledOperation *operation, B32 registered)
{
  Arena *arena = arena_alloc(.name = "Engine Operation Owner");
  RVS_RequestControl *owner = push_array(arena, RVS_RequestControl, 1);
  owner->arena = arena;
  owner->session = session;
  owner->control = session->control;
  owner->request_id = operation->request->request_id;
  owner->registered = registered;
  rvs_session_addref(session);
  rvs_engine_control_addref(owner->control);
  return owner;
}

void
rvs_request_control_release(RVS_RequestControl *owner)
{
  if (owner == 0) { return; }
  if (owner->registered) {
    Temp scratch = scratch_begin(0, 0);
    RVS_EnginePreparedDecision prepared = {0};
    (void)rvs_control_reduce_scheduler_outcome(owner->session, (RVS_EngineSchedulerOutcome){
      .kind = RVS_EngineSchedulerOutcomeKind_Event,
      .event = {
        .kind = RVS_SchedulerEvent_RequestControlReleased,
        .request = { .request_id = owner->request_id },
      },
    }, scratch.arena, 1, &prepared);
    AssertAlways(prepared.emission_first == 0 && prepared.command_kind == RVS_SchedulerCommand_Null);
    scratch_end(scratch);
  }
  rvs_engine_control_release(owner->control);
  rvs_session_release(owner->session);
  arena_release(owner->arena);
}

RVS_Result
rvs_request_control_cancel(RVS_RequestControl *owner)
{
  if (owner == 0 || owner->request_id == 0 || owner->session == 0) { return RVS_Result_Error; }
  Temp scratch = scratch_begin(0, 0);
  RVS_EnginePreparedDecision prepared = {0};
  RVS_Result result = rvs_control_reduce_scheduler_outcome(owner->session, (RVS_EngineSchedulerOutcome){
    .kind = RVS_EngineSchedulerOutcomeKind_Event,
    .event = (RVS_SchedulerEvent){
      .kind = RVS_SchedulerEvent_PreDispatchCancelled,
      .request = { .request_id = owner->request_id },
    },
  }, scratch.arena, 1, &prepared);
  if (prepared.emission_first && prepared.emission_first->kind == RVS_SchedulerEmission_CompleteRequest) {
    result = RVS_Result_Ok;
  }
  RVS_EngineSchedulerOutcome outcome = rvs_engine_execute_prepared_decision(0, &prepared);
  AssertAlways(outcome.kind == RVS_EngineSchedulerOutcomeKind_Null);
  scratch_end(scratch);
  return result;
}

////////////////////////////////
// Scheduler Reducer

internal void
rvs_scheduler_decision_ignore_stale(RVS_SchedulerDecision *decision)
{
  AssertAlways(decision->result == RVS_Result_Ok);
  AssertAlways(decision->emissions.first == 0 && decision->command.kind == RVS_SchedulerCommand_Null);
  decision->result = RVS_Result_StaleState;
}

internal void
rvs_scheduler_decision_reject(RVS_SchedulerDecision *decision, RVS_Result result)
{
  AssertAlways(result != RVS_Result_Null && result != RVS_Result_Ok && result != RVS_Result_Pending && result != RVS_Result_StaleState);
  AssertAlways(decision->result == RVS_Result_Ok);
  AssertAlways(decision->emissions.first == 0 && decision->command.kind == RVS_SchedulerCommand_Null);
  decision->result = result;
}

internal RVS_SchedulerEmission *
rvs_scheduler_emit_locked(RVS_Scheduler *scheduler, RVS_SchedulerDecision *decision, RVS_SchedulerEmissionKind kind, RVS_ScheduledOperation *operation)
{
  AssertAlways(decision->command.kind == RVS_SchedulerCommand_Null);
  mutex_take(scheduler->recycle_mutex);
  RVS_SchedulerEmission *emission = scheduler->emission_free_first;
  if (emission) {
    scheduler->emission_free_first = emission->next;
  } else {
    emission = push_array(scheduler->arena, RVS_SchedulerEmission, 1);
  }
  MemoryZeroStruct(emission);
  mutex_drop(scheduler->recycle_mutex);
  emission->kind = kind;
  emission->scheduler = scheduler;
  emission->operation = operation;
  if (operation) { rvs_scheduler_operation_addref(operation); }
  SLLQueuePush(decision->emissions.first, decision->emissions.last, emission);
  return emission;
}

internal RVS_SchedulerCommand *
rvs_scheduler_set_command_locked(RVS_Scheduler            *scheduler,
                                 RVS_SchedulerDecision    *decision,
                                 RVS_SchedulerCommandKind  kind,
                                 RVS_ScheduledOperation   *operation)
{
  rvs_scheduler_assert_reducing(scheduler);
  AssertAlways(kind != RVS_SchedulerCommand_Null && operation != 0);
  AssertAlways(decision->result == RVS_Result_Ok && decision->command.kind == RVS_SchedulerCommand_Null);
  AssertAlways(operation->pending_command.kind == RVS_SchedulerCommand_Null);
  AssertAlways(scheduler->next_command_id != max_U64);

  scheduler->next_command_id += 1;
  AssertAlways(scheduler->next_command_id != 0);

  RVS_SchedulerCommandToken token = {
    .request_id = operation->request->request_id,
    .command_id = scheduler->next_command_id,
  };
  decision->command.kind      = kind;
  decision->command.token     = token;
  decision->command.operation = operation;

  operation->pending_command = (RVS_SchedulerPendingCommand){ .kind = kind, .token = token, .has_started = 1 };
  rvs_scheduler_operation_addref(operation);

  return &decision->command;
}

internal B32
rvs_scheduler_consume_command_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation, RVS_SchedulerCommandToken token,
                                     RVS_SchedulerCommandKind expected_kind, B32 expected_phase_matches)
{
  rvs_scheduler_assert_reducing(scheduler);
  RVS_SchedulerPendingCommand *pending = &operation->pending_command;
  if (rvs_scheduler_operation_from_request_id_locked(scheduler, token.request_id) != operation ||
      pending->kind != expected_kind || !pending->has_started ||
      pending->token.command_id != token.command_id || pending->token.request_id != token.request_id ||
      !expected_phase_matches) {
    return 0;
  }
  MemoryZeroStruct(pending);
  return 1;
}

internal void
rvs_scheduler_decision_release(RVS_SchedulerDecision *decision)
{
  for (RVS_SchedulerEmission *emission = decision->emissions.first, *next = 0; emission; emission = next) {
    next = emission->next;
    RVS_Scheduler *scheduler = emission->scheduler;
    if (emission->operation) { rvs_scheduler_operation_release(emission->operation); }
    if (scheduler) {
      mutex_take(scheduler->recycle_mutex);
      emission->next = scheduler->emission_free_first;
      scheduler->emission_free_first = emission;
      mutex_drop(scheduler->recycle_mutex);
    }
  }
  if (decision->command.operation) { rvs_scheduler_operation_release(decision->command.operation); }
  MemoryZeroStruct(decision);
}

internal RVS_SchedulerEmission *
rvs_scheduler_emit_operation_reply_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation, RVS_Result result,
                                           RVS_SchedulerDecision *decision)
{
  rvs_scheduler_assert_reducing(scheduler);
  AssertAlways(!operation->request_completed);
  operation->request_completed = 1;
  RVS_SchedulerEmission *emission = rvs_scheduler_emit_locked(scheduler, decision, RVS_SchedulerEmission_CompleteRequest, operation);
  emission->reply = (RVS_EngineReply){
    .request_id = operation->request->request_id,
    .result = result,
    .kind = operation->key.op == RVS_SchedulerOp_Run ? RVS_EngineReplyKind_Run :
            operation->key.op == RVS_SchedulerOp_Interrupt ? RVS_EngineReplyKind_Interrupt :
            operation->key.op == RVS_SchedulerOp_Launch ? RVS_EngineReplyKind_Launch : RVS_EngineReplyKind_Null,
  };
  return emission;
}

internal void
rvs_scheduler_retire_operation_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation,
                                      RVS_ScheduledOperationState terminal_state)
{
  rvs_scheduler_assert_reducing(scheduler);
  AssertAlways(terminal_state == RVS_ScheduledOperationState_Completed ||
               terminal_state == RVS_ScheduledOperationState_Cancelled ||
               terminal_state == RVS_ScheduledOperationState_Failed);
  AssertAlways(operation->pending_command.kind == RVS_SchedulerCommand_Null);
  if (operation->is_keyed) {
    RVS_ScheduledOperation *keyed_operation = rvs_scheduler_unregister_operation_locked(scheduler, operation->key, operation);
    AssertAlways(keyed_operation == operation);
    rvs_scheduler_operation_release(keyed_operation);
  }
  operation->state = terminal_state;
  rvs_scheduler_operation_remove_locked(scheduler, operation);
  rvs_scheduler_operation_release(operation);
}

internal RVS_SchedulerEmission *
rvs_scheduler_complete_operation_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation, RVS_Result result, RVS_SchedulerDecision *decision)
{
  rvs_scheduler_assert_reducing(scheduler);
  AssertAlways(operation->pending_command.kind == RVS_SchedulerCommand_Null);
  RVS_ScheduledOperation *keyed_operation = 0;
  if (operation->is_keyed) {
    keyed_operation = rvs_scheduler_unregister_operation_locked(scheduler, operation->key, operation);
    AssertAlways(keyed_operation == operation);
  }
  rvs_scheduler_operation_remove_locked(scheduler, operation);
  operation->state = result == RVS_Result_Cancelled ? RVS_ScheduledOperationState_Cancelled :
                     result == RVS_Result_Ok ? RVS_ScheduledOperationState_Completed : RVS_ScheduledOperationState_Failed;
  RVS_SchedulerEmission *emission = rvs_scheduler_emit_operation_reply_locked(scheduler, operation, result, decision);
  if (keyed_operation) { rvs_scheduler_operation_release(keyed_operation); }
  rvs_scheduler_operation_release(operation); // transfer active registration ownership to the emission
  return emission;
}

internal void
rvs_scheduler_abort_operation_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id, RVS_Result result, B32 require_undispatched, RVS_SchedulerDecision *decision)
{
  rvs_scheduler_assert_reducing(scheduler);
  RVS_ScheduledOperation *operation = rvs_scheduler_operation_from_request_id_locked(scheduler, request_id);
  if (operation == 0) {
    if ( ! rvs_scheduler_clear_queued_execution_locked(scheduler, request_id)) {
      rvs_scheduler_decision_ignore_stale(decision);
    }
  } else if (require_undispatched && operation->is_dispatched) {
    rvs_scheduler_decision_reject(decision, RVS_Result_Unsupported);
  } else {
    RVS_SchedulerOperationRule *rule = rvs_scheduler_operation_rule(operation->key.op);
    if (rule && rule->transition == RVS_SchedulerTransition_Run) {
      rvs_scheduler_clear_queued_execution_locked(scheduler, request_id);
      if (scheduler->execution_owner.kind == RVS_ExecutionOwnerKind_Operation &&
          scheduler->execution_owner.request_id == request_id) {
        MemoryZeroStruct(&scheduler->execution_owner);
      }
    } else if (rule && rule->transition == RVS_SchedulerTransition_Interrupt) {
      if (!rvs_scheduler_settle_stop_transaction_locked(scheduler, request_id)) {
        rvs_scheduler_cancel_stop_transaction_locked(scheduler, request_id);
      }
    }
    rvs_scheduler_complete_operation_locked(scheduler, operation, result, decision);
  }
}

internal void
rvs_scheduler_apply_locked(RVS_Scheduler         *scheduler,
                           RVS_SchedulerEvent     event,
                           RVS_SchedulerDecision *decision_out)
{
  AssertAlways(rvs_scheduler_reducer_active == 0);
  rvs_scheduler_reducer_active = scheduler;
  MemoryZeroStruct(decision_out);

  decision_out->result = RVS_Result_Ok;

  if (scheduler->phase == RVS_SchedulerPhase_Exited && event.kind != RVS_SchedulerEvent_Shutdown) {
    rvs_scheduler_decision_ignore_stale(decision_out);
    goto finalize;
  }

  switch (event.kind) {
  case RVS_SchedulerEvent_LaunchStarted: {
    RVS_ScheduledOperation *operation = rvs_scheduler_operation_from_request_id_locked(scheduler, event.launch_started.request_id);
    if (operation == 0 ||
        operation->key.op != RVS_SchedulerOp_Launch ||
        operation->launch_phase != RVS_LaunchPhase_AwaitLaunchStarted ||
        event.launch_started.pid == 0) {
      rvs_scheduler_decision_ignore_stale(decision_out);
    } else {
      operation->launch_phase = RVS_LaunchPhase_AwaitCreateProcess;
      operation->launch_pid   = event.launch_started.pid;
      rvs_scheduler_set_command_locked(scheduler, decision_out, RVS_SchedulerCommand_PumpLaunch, operation);
    }
  } break;

  case RVS_SchedulerEvent_DispatchStarted: {
    RVS_ScheduledOperation *operation = rvs_scheduler_operation_from_request_id_locked(scheduler, event.request.request_id);
    if (operation == 0 || operation->is_dispatched) {
      rvs_scheduler_decision_ignore_stale(decision_out);
    } else if (operation->key.op == RVS_SchedulerOp_Interrupt &&
               !rvs_scheduler_begin_interrupt_transaction_locked(scheduler, operation)) {
      rvs_scheduler_decision_ignore_stale(decision_out);
    } else {
      operation->is_dispatched = 1;
      operation->state = RVS_ScheduledOperationState_Active;
      if (operation->key.op == RVS_SchedulerOp_Interrupt) {
        RVS_StopTransaction *stop = &scheduler->stop_transaction;
        RVS_SchedulerCommand *command = rvs_scheduler_set_command_locked(scheduler, decision_out,
                                                                          RVS_SchedulerCommand_InterruptExecution,
                                                                          operation);
        command->interrupt_execution.execution_request_id = stop->execution_owner;
        stop->interrupt_token = command->token;
        stop->phase = RVS_ControlTransactionPhase_WaitingForInterrupt;
        stop->interrupt_attempt += 1;
        scheduler->phase = RVS_SchedulerPhase_Interrupting;
        for EachIndex(target_idx, operation->targets_count) {
          if (operation->targets[target_idx].is_selected) {
            RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, operation->targets[target_idx].target);
            if (entry && entry->state == RVS_TargetExecutionState_RunInFlight && entry->execution_owner == stop->execution_owner) {
              rvs_scheduler_commit_target_locked(entry, 0, RVS_TargetExecutionState_InterruptPending,
                                                 RVS_TargetTransition_BarrierPause);
            }
          }
        }
      }
    }
  } break;

  case RVS_SchedulerEvent_DispatchAccepted: {
    RVS_ScheduledOperation *operation = rvs_scheduler_operation_from_request_id_locked(scheduler, event.request.request_id);
    RVS_SchedulerOperationRule *rule = operation ? rvs_scheduler_operation_rule(operation->key.op) : 0;
    if ((operation && (rule == 0 || rule->transition != RVS_SchedulerTransition_Run)) ||
        !rvs_scheduler_mark_run_in_flight_locked(scheduler, event.request.request_id)) {
      rvs_scheduler_decision_ignore_stale(decision_out);
    } else {
      MemoryZeroStruct(&scheduler->stop_transaction);
      MemoryZeroStruct(&scheduler->resume_transaction);
      scheduler->run_intent = (RVS_RunIntent){
        .kind = RVS_RunIntentKind_Continue,
        .state = RVS_RunIntentState_Active,
        .execution_owner = event.request.request_id,
      };
      if (scheduler->execution_owner.kind == RVS_ExecutionOwnerKind_Null) {
        scheduler->execution_owner = rvs_execution_owner_operation(event.request.request_id);
      }
      if (operation) { operation->state = RVS_ScheduledOperationState_Active; }
      scheduler->phase = RVS_SchedulerPhase_Running;
    }
  } break;

  case RVS_SchedulerEvent_OperationFailed: {
    RVS_Result result = event.failed.result;
    if (result == RVS_Result_Null || result == RVS_Result_Ok || result == RVS_Result_Pending) { result = RVS_Result_Error; }
    RVS_ScheduledOperation *operation = rvs_scheduler_operation_from_request_id_locked(scheduler, event.failed.request_id);
    if (operation && operation->request_completed) {
      rvs_scheduler_decision_ignore_stale(decision_out);
    } else {
      rvs_scheduler_abort_operation_locked(scheduler, event.failed.request_id, result, 0, decision_out);
    }
  } break;

  case RVS_SchedulerEvent_BackendCompleted: {
    RVS_EngineReply reply = event.completed.reply;
    RVS_ScheduledOperation *operation = rvs_scheduler_operation_from_request_id_locked(scheduler, reply.request_id);
    if (operation == 0 || operation->request_completed) {
      rvs_scheduler_decision_ignore_stale(decision_out);
    } else {
      if (reply.result != RVS_Result_Ok) {
        rvs_scheduler_abort_operation_locked(scheduler, reply.request_id, reply.result, 0, decision_out);
        decision_out->emissions.last->reply = reply;
        break;
      }
      if (operation->key.op == RVS_SchedulerOp_Run) {
        rvs_scheduler_emit_operation_reply_locked(scheduler, operation, reply.result, decision_out)->reply = reply;
      } else {
        rvs_scheduler_complete_operation_locked(scheduler, operation, reply.result, decision_out)->reply = reply;
      }
    }
  } break;

  case RVS_SchedulerEvent_RunFinished: {
    RVS_ScheduledOperation *operation = rvs_scheduler_operation_from_request_id_locked(scheduler, event.request.request_id);
    if ( ! rvs_scheduler_finish_run_locked(scheduler, event.request.request_id)) {
      rvs_scheduler_decision_ignore_stale(decision_out);
    } else {
      if (scheduler->run_intent.execution_owner == event.request.request_id &&
          scheduler->run_intent.state == RVS_RunIntentState_Active) {
        scheduler->run_intent.state = RVS_RunIntentState_Completed;
      }
      scheduler->phase = RVS_SchedulerPhase_Stopped;
      if (scheduler->execution_owner.kind == RVS_ExecutionOwnerKind_Operation &&
          scheduler->execution_owner.request_id == event.request.request_id) {
        MemoryZeroStruct(&scheduler->execution_owner);
      }
      if (operation && operation->key.op == RVS_SchedulerOp_Run) {
        rvs_scheduler_retire_operation_locked(scheduler, operation, RVS_ScheduledOperationState_Completed);
      }
    }
  } break;

  case RVS_SchedulerEvent_DemonEventBatch: {
    RVS_ScheduledOperation *owned_stop_before = scheduler->stop_transaction.owner;
    B32 owned_backend_stable_before = scheduler->stop_transaction.backend_stable;
    U64 owned_stable_generation_before = scheduler->stop_transaction.stable_stop_generation;
    U64 batch_event_count = 0;
    for EachNode(event_node, DMN_EventNode, event.demon_events.events.first) { batch_event_count += 1; }
    if (event.demon_events.dispositions_count < batch_event_count ||
        (event.demon_events.dispositions_count != 0 && event.demon_events.dispositions == 0)) {
      rvs_scheduler_decision_reject(decision_out, RVS_Result_InvalidArgument);
      break;
    }
    RVS_ScheduledOperation *launch = 0;
    if (event.demon_events.source == RVS_SchedulerEventBatchSource_PumpLaunch) {
      launch = rvs_scheduler_operation_from_request_id_locked(scheduler, event.demon_events.command.request_id);
      B32 phase_matches = launch && launch->key.op == RVS_SchedulerOp_Launch &&
                          launch->launch_phase == RVS_LaunchPhase_AwaitCreateProcess;
      if (event.demon_events.request_id != event.demon_events.command.request_id ||
          launch == 0 || !rvs_scheduler_consume_command_locked(scheduler, launch, event.demon_events.command,
                                                                RVS_SchedulerCommand_PumpLaunch, phase_matches)) {
        for EachIndex(disposition_idx, event.demon_events.dispositions_count) {
          event.demon_events.dispositions[disposition_idx] = RVS_SchedulerEventDisposition_Suppress;
        }
        rvs_scheduler_decision_ignore_stale(decision_out);
        break;
      }
    }

    B32 accepts_stop_events = 0;
    if (event.demon_events.source == RVS_SchedulerEventBatchSource_Execution &&
        scheduler->phase == RVS_SchedulerPhase_Running &&
        scheduler->run_intent.state == RVS_RunIntentState_Active &&
        scheduler->run_intent.execution_owner == event.demon_events.request_id &&
        scheduler->stop_transaction.owner == 0) {
      scheduler->stop_transaction = (RVS_StopTransaction){
        .execution_owner = event.demon_events.request_id,
        .phase = RVS_ControlTransactionPhase_CollectingBatch,
        .cycle_epoch = ++scheduler->run_cycle_epoch,
        .interrupt_attempt = 1,
      };
      scheduler->phase = RVS_SchedulerPhase_CollectingStop;
      accepts_stop_events = 1;
    } else if (event.demon_events.source == RVS_SchedulerEventBatchSource_Execution &&
               (scheduler->phase == RVS_SchedulerPhase_Interrupting || scheduler->phase == RVS_SchedulerPhase_CollectingStop) &&
               scheduler->stop_transaction.owner != 0 && !scheduler->stop_transaction.backend_stable &&
               (scheduler->stop_transaction.phase == RVS_ControlTransactionPhase_WaitingForInterrupt ||
                scheduler->stop_transaction.phase == RVS_ControlTransactionPhase_CollectingBatch) &&
               scheduler->stop_transaction.execution_owner == event.demon_events.request_id) {
      scheduler->phase = RVS_SchedulerPhase_CollectingStop;
      scheduler->stop_transaction.phase = RVS_ControlTransactionPhase_CollectingBatch;
      accepts_stop_events = 1;
    }
    DMN_Event *launch_created = 0;
    U64 launch_created_index = 0;
    B32 launch_error = 0;
    B32 launch_exited = 0;
    if (launch && launch->launch_phase == RVS_LaunchPhase_AwaitCreateProcess) {
      U64 candidate_index = 0;
      for EachNode(event_node, DMN_EventNode, event.demon_events.events.first) {
        DMN_Event *demon_event = &event_node->v;
        if (demon_event->kind == DMN_EventKind_CreateProcess && demon_event->system_process_id == launch->launch_pid) {
          launch_created = demon_event;
          launch_created_index = candidate_index;
          break;
        }
        candidate_index += 1;
      }
    }
    U64 event_index = 0;
    for EachNode(event_node, DMN_EventNode, event.demon_events.events.first) {
      DMN_Event *demon_event = &event_node->v;
      if (accepts_stop_events) {
        scheduler->stop_transaction.collected_events_count += 1;
      }
      RVS_Program *entry = rvs_scheduler_program_from_id_locked(scheduler, demon_event->process);
      if (demon_event->kind == DMN_EventKind_CreateThread) {
        B32 is_launch_thread = launch_created && !launch_exited && dmn_handle_match(demon_event->process, launch_created->process);
        if ((entry && entry->state != RVS_TargetState_Removed) || is_launch_thread) {
          rvs_scheduler_thread_add_locked(scheduler, demon_event->process, demon_event->thread);
        }
      } else if (demon_event->kind == DMN_EventKind_ExitThread) {
        RVS_ThreadLedgerEntry *thread = rvs_scheduler_thread_from_id_locked(scheduler, demon_event->thread);
        if (thread) {
          thread->is_removed = 1;
          if (dmn_handle_match(scheduler->selected_thread, thread->thread)) {
            scheduler->selected_target = dmn_handle_zero();
            scheduler->selected_thread = dmn_handle_zero();
          }
        }
      } else if (demon_event->kind == DMN_EventKind_ExitProcess) {
        if (accepts_stop_events) {
          rvs_scheduler_note_primary_stop_locked(scheduler, RVS_StopCause_ProcessExit, demon_event->process);
        }
        if (launch_created && dmn_handle_match(demon_event->process, launch_created->process)) {
          launch_exited = 1;
          for EachNode(thread, RVS_ThreadLedgerEntry, scheduler->thread_first) {
            if (dmn_handle_match(thread->target, launch_created->process)) { thread->is_removed = 1; }
          }
          if (launch_created_index < event.demon_events.dispositions_count) {
            event.demon_events.dispositions[launch_created_index] = RVS_SchedulerEventDisposition_Suppress;
          }
          if (event_index < event.demon_events.dispositions_count) {
            event.demon_events.dispositions[event_index] = RVS_SchedulerEventDisposition_Suppress;
          }
        } else if (entry) {
          rvs_scheduler_retire_target_locked(scheduler, entry, event.demon_events.request_id, demon_event);
        }
      } else if (demon_event->kind == DMN_EventKind_Error) {
        if (launch && launch->launch_phase == RVS_LaunchPhase_AwaitCreateProcess &&
            (demon_event->error_kind == DMN_ErrorKind_NotAttached || demon_event->system_process_id == launch->launch_pid)) {
          launch_error = 1;
          if (demon_event->system_process_id == launch->launch_pid && event_index < event.demon_events.dispositions_count) {
            event.demon_events.dispositions[event_index] = RVS_SchedulerEventDisposition_Suppress;
          }
        }
      } else if (demon_event->kind == DMN_EventKind_Halt) {
        B32 selected = 0;
        if (scheduler->stop_transaction.owner) {
          for EachIndex(target_idx, scheduler->stop_transaction.owner->targets_count) {
            RVS_TargetSnapshot *target = &scheduler->stop_transaction.owner->targets[target_idx];
            if (dmn_handle_match(target->target, demon_event->process)) {
              selected = target->is_selected;
              break;
            }
          }
        }
        if (accepts_stop_events) {
          rvs_scheduler_note_primary_stop_locked(scheduler, selected ? RVS_StopCause_UserInterrupt : RVS_StopCause_Incidental, demon_event->process);
          if (!selected && event_index < event.demon_events.dispositions_count) {
            event.demon_events.dispositions[event_index] = RVS_SchedulerEventDisposition_Suppress;
          }
        }
        if (entry && entry->execution_owner != 0) {
          rvs_scheduler_observe_target_locked(scheduler, (RVS_SchedulerEvent){
            .observed = { .request_id = event.demon_events.request_id, .target = demon_event->process },
          }, DMN_EventKind_Halt);
        }
      } else if (demon_event->kind == DMN_EventKind_Exception || demon_event->kind == DMN_EventKind_Breakpoint) {
        if (accepts_stop_events) {
          RVS_StopCause cause = demon_event->kind == DMN_EventKind_Exception ? RVS_StopCause_Exception : RVS_StopCause_Breakpoint;
          rvs_scheduler_note_primary_stop_locked(scheduler, cause, demon_event->process);
          if (cause == RVS_StopCause_Exception && scheduler->run_intent.state == RVS_RunIntentState_Active) {
            scheduler->run_intent.state = RVS_RunIntentState_Cancelled;
          }
        }
      }
      event_index += 1;
    }
    event_index = 0;
    for EachNode(event_node, DMN_EventNode, event.demon_events.events.first) {
      B32 suppressed = event_index < event.demon_events.dispositions_count &&
                       event.demon_events.dispositions[event_index] == RVS_SchedulerEventDisposition_Suppress;
      if (!suppressed) {
        RVS_SchedulerEmission *emission = rvs_scheduler_emit_locked(scheduler, decision_out, RVS_SchedulerEmission_PublishEvent, 0);
        emission->event = event_node->v;
      }
      event_index += 1;
    }
    if (launch) {
      if (launch_created && !launch_exited && !dmn_handle_match(launch_created->process, dmn_handle_zero())) {
        launch->launch_phase = RVS_LaunchPhase_AwaitTargetRegistration;
        launch->launch_process = launch_created->process;
        RVS_SchedulerCommand *command = rvs_scheduler_set_command_locked(scheduler, decision_out, RVS_SchedulerCommand_PublishTarget, launch);
        command->publish_target.pid = launch->launch_pid;
        command->publish_target.process = launch_created->process;
      } else if (launch_error || launch_exited || launch_created) {
        rvs_scheduler_complete_operation_locked(scheduler, launch, RVS_Result_Error, decision_out);
      } else if (launch->launch_phase == RVS_LaunchPhase_AwaitCreateProcess) {
        rvs_scheduler_set_command_locked(scheduler, decision_out, RVS_SchedulerCommand_PumpLaunch, launch);
      }
    }
    if (owned_stop_before && scheduler->stop_transaction.owner == owned_stop_before) {
      AssertAlways(scheduler->stop_transaction.backend_stable == owned_backend_stable_before &&
                   scheduler->stop_transaction.stable_stop_generation == owned_stable_generation_before);
    }
    if (scheduler->stop_transaction.owner == 0 && scheduler->phase == RVS_SchedulerPhase_CollectingStop) {
      scheduler->stop_transaction.backend_stable = 1;
      scheduler->stop_transaction.phase = RVS_ControlTransactionPhase_Stable;
      scheduler->phase = RVS_SchedulerPhase_PublishingStop;
      scheduler->stop_transaction.event_published = 1;
      scheduler->phase = RVS_SchedulerPhase_Stopped;
    }
  } break;

  case RVS_SchedulerEvent_CommandOutcome: {
    RVS_SchedulerCommandToken token = event.command_outcome.command;
    RVS_SchedulerCommandKind command_kind = event.command_outcome.command_kind;
    RVS_ScheduledOperation *operation = rvs_scheduler_operation_from_request_id_locked(scheduler, token.request_id);
    B32 phase_matches = 0;
    if (operation) {
      if (command_kind == RVS_SchedulerCommand_PumpLaunch) {
        phase_matches = operation->key.op == RVS_SchedulerOp_Launch &&
                        operation->launch_phase == RVS_LaunchPhase_AwaitCreateProcess;
      } else if (command_kind == RVS_SchedulerCommand_PublishTarget) {
        phase_matches = operation->key.op == RVS_SchedulerOp_Launch &&
                        operation->launch_phase == RVS_LaunchPhase_AwaitTargetRegistration;
      } else if (command_kind == RVS_SchedulerCommand_InterruptExecution) {
        RVS_StopTransaction *stop = &scheduler->stop_transaction;
        phase_matches = operation->key.op == RVS_SchedulerOp_Interrupt && stop->owner == operation &&
                        !stop->backend_stable &&
                        stop->interrupt_token.request_id == token.request_id &&
                        stop->interrupt_token.command_id == token.command_id &&
                        (stop->phase == RVS_ControlTransactionPhase_WaitingForInterrupt ||
                         stop->phase == RVS_ControlTransactionPhase_CollectingBatch) &&
                        (scheduler->phase == RVS_SchedulerPhase_Interrupting ||
                         scheduler->phase == RVS_SchedulerPhase_CollectingStop) &&
                        rvs_execution_owner_match(scheduler->execution_owner,
                                                  rvs_execution_owner_control(token.request_id, stop->transaction_id));
      } else if (command_kind == RVS_SchedulerCommand_ResumeTargetSubset) {
        phase_matches = operation->key.op == RVS_SchedulerOp_Interrupt &&
                        scheduler->resume_transaction.owner == operation &&
                        scheduler->phase == RVS_SchedulerPhase_PreparingResume;
      }
    }
    if (operation == 0 || !rvs_scheduler_consume_command_locked(scheduler, operation, token, command_kind, phase_matches)) {
      rvs_scheduler_decision_ignore_stale(decision_out);
      break;
    }

    if (event.command_outcome.result != RVS_Result_Null &&
        event.command_outcome.result != RVS_Result_Ok &&
        event.command_outcome.result != RVS_Result_Pending) {
      rvs_scheduler_abort_operation_locked(scheduler, token.request_id, event.command_outcome.result, 0, decision_out);
    } else if (command_kind == RVS_SchedulerCommand_PublishTarget &&
               event.command_outcome.result == RVS_Result_Ok &&
               operation->launch_pid == event.command_outcome.pid &&
               dmn_handle_match(operation->launch_process, event.command_outcome.target) &&
                rvs_scheduler_program_from_id_locked(scheduler, event.command_outcome.target) == 0) {
      rvs_scheduler_program_add_locked(scheduler, event.command_outcome.target, event.command_outcome.pid);
      RVS_SchedulerEmission *emission = rvs_scheduler_complete_operation_locked(scheduler, operation, RVS_Result_Ok, decision_out);
      emission->reply.launch.program_id = event.command_outcome.target;
      emission->reply.launch.pid = event.command_outcome.pid;
    } else if (command_kind == RVS_SchedulerCommand_InterruptExecution &&
               event.command_outcome.result == RVS_Result_Ok) {
      rvs_scheduler_establish_stable_stop_locked(scheduler, operation, token, decision_out);
    } else if (command_kind == RVS_SchedulerCommand_ResumeTargetSubset &&
               event.command_outcome.result == RVS_Result_Ok) {
      RVS_Result result = rvs_scheduler_finish_resume_transaction_locked(scheduler, token.request_id);
      if (result == RVS_Result_Error) {
        AssertAlways(rvs_scheduler_settle_stop_transaction_locked(scheduler, token.request_id));
      }
      rvs_scheduler_complete_operation_locked(scheduler, operation, result, decision_out);
    } else {
      // The token was valid, but the payload was not. Fail the owning operation rather than leave it suspended.
      rvs_scheduler_abort_operation_locked(scheduler, token.request_id, RVS_Result_Error, 0, decision_out);
    }
  } break;

  case RVS_SchedulerEvent_PreDispatchCancelled: {
    rvs_scheduler_abort_operation_locked(scheduler, event.request.request_id, RVS_Result_Cancelled, 1, decision_out);
  } break;

  case RVS_SchedulerEvent_RequestControlReleased: {
    RVS_ScheduledOperation *operation = rvs_scheduler_operation_from_request_id_locked(scheduler, event.request.request_id);
    if (operation == 0 || !operation->is_keyed) {
      rvs_scheduler_decision_ignore_stale(decision_out);
    } else {
      RVS_ScheduledOperation *registered_operation = rvs_scheduler_unregister_operation_locked(scheduler, operation->key, operation);
      AssertAlways(registered_operation == operation);
      rvs_scheduler_operation_release(registered_operation);
    }
  } break;

  case RVS_SchedulerEvent_ThreadSelected: {
    RVS_Program *target = rvs_scheduler_program_from_id_locked(scheduler, event.thread_selected.target);
    RVS_ThreadLedgerEntry *thread = rvs_scheduler_thread_from_id_locked(scheduler, event.thread_selected.thread);
    if (target == 0 || target->state == RVS_TargetState_Removed || thread == 0 || thread->is_removed ||
        !dmn_handle_match(thread->target, event.thread_selected.target)) {
      rvs_scheduler_decision_ignore_stale(decision_out);
    } else {
      scheduler->selected_target = event.thread_selected.target;
      scheduler->selected_thread = event.thread_selected.thread;
    }
  } break;

  case RVS_SchedulerEvent_Shutdown: {
    for EachNode(operation, RVS_ScheduledOperation, scheduler->operation_first) {
      MemoryZeroStruct(&operation->pending_command);
    }
    MemoryZeroStruct(&scheduler->stop_transaction);
    MemoryZeroStruct(&scheduler->resume_transaction);
    for (RVS_ScheduledOperation *operation = scheduler->operation_first, *next = 0; operation; operation = next) {
      next = operation->next;
      MemoryZeroStruct(&operation->pending_command);
      if (operation->request_completed) {
        rvs_scheduler_retire_operation_locked(scheduler, operation, RVS_ScheduledOperationState_Failed);
      } else {
        rvs_scheduler_complete_operation_locked(scheduler, operation, RVS_Result_EngineStopped, decision_out);
      }
    }
    rvs_scheduler_release_execution_leases_locked(scheduler);
    scheduler->run_intent.state = RVS_RunIntentState_Cancelled;
    scheduler->phase = RVS_SchedulerPhase_Exited;
    scheduler->execution_owner = (RVS_ExecutionOwner){ .kind = RVS_ExecutionOwnerKind_Shutdown };
  } break;

  default: { InvalidPath; } break;
  }
  finalize:;
  if (decision_out->result != RVS_Result_Ok) {
    AssertAlways(decision_out->emissions.first == 0 && decision_out->command.kind == RVS_SchedulerCommand_Null);
    AssertAlways(decision_out->result != RVS_Result_Pending && decision_out->result != RVS_Result_Null);
  }
  rvs_scheduler_assert_execution_owner_locked(scheduler);
  rvs_scheduler_reducer_active = 0;
}
