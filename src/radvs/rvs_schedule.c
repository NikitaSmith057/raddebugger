// This file is included only by rvs_engine.c after private engine/session types.

#include "radvs/rvs_request.h"

internal B32
rvs_operation_key_is_complete(RVS_OperationKey key)
{
  return key.operation_id != 0;
}

internal B32
rvs_operation_key_is_well_formed_for_policy(RVS_RequestPolicy policy, RVS_OperationKey key)
{
  if (policy != RVS_RequestPolicy_RejectIfPending && policy != RVS_RequestPolicy_JoinIfEqual) { return 0; }
  if ( ! rvs_operation_key_is_complete(key)) { return 0; }
  if (key.operation_class == RVS_OperationClass_ReadOnly) {
    return !dmn_handle_match(key.program_id, dmn_handle_zero());
  }
  return (key.operation_class == RVS_OperationClass_SessionLifecycle ||
          key.operation_class == RVS_OperationClass_SessionExecution) &&
         dmn_handle_match(key.program_id, dmn_handle_zero());
}

internal B32
rvs_operation_key_match(RVS_OperationKey a, RVS_OperationKey b)
{
  return a.operation_class == b.operation_class &&
         dmn_handle_match(a.program_id, b.program_id) &&
         a.operation_id == b.operation_id;
}

internal B32
rvs_operation_keys_conflict(RVS_OperationKey a, RVS_OperationKey b)
{
  if (a.operation_class == RVS_OperationClass_SessionLifecycle || b.operation_class == RVS_OperationClass_SessionLifecycle) { return 1; }
  if (a.operation_class == RVS_OperationClass_ReadOnly || b.operation_class == RVS_OperationClass_ReadOnly) { return 0; }
  return a.operation_class == RVS_OperationClass_SessionExecution ||
         b.operation_class == RVS_OperationClass_SessionExecution;
}

internal B32
rvs_scheduler_has_conflicting_operation_locked(RVS_Scheduler *scheduler, RVS_OperationKey key)
{
  for EachNode(operation, RVS_ScheduledOperation, scheduler->operation_first) {
    if (rvs_operation_keys_conflict(operation->key, key)) { return 1; }
  }
  return 0;
}

internal B32
rvs_scheduler_reserve_execution_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id)
{
  if (request_id == 0 || scheduler->execution_state != RVS_SessionExecutionState_Idle || scheduler->execution_request_id != 0) { return 0; }
  scheduler->execution_state = RVS_SessionExecutionState_Queued;
  scheduler->execution_request_id = request_id;
  return 1;
}

internal B32
rvs_scheduler_mark_run_in_flight_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id)
{
  if (scheduler->execution_state != RVS_SessionExecutionState_Queued || scheduler->execution_request_id != request_id) { return 0; }
  scheduler->execution_state = RVS_SessionExecutionState_RunInFlight;
  return 1;
}

internal B32
rvs_scheduler_clear_queued_execution_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id)
{
  if (scheduler->execution_state != RVS_SessionExecutionState_Queued || scheduler->execution_request_id != request_id) { return 0; }
  scheduler->execution_state = RVS_SessionExecutionState_Idle;
  scheduler->execution_request_id = 0;
  return 1;
}

internal B32
rvs_scheduler_finish_run_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id)
{
  if (scheduler->execution_state != RVS_SessionExecutionState_RunInFlight || scheduler->execution_request_id != request_id) { return 0; }
  scheduler->execution_state = RVS_SessionExecutionState_Idle;
  scheduler->execution_request_id = 0;
  return 1;
}

internal B32
rvs_scheduler_execution_blocks_operation_locked(RVS_Scheduler *scheduler, RVS_OperationClass operation_class)
{
  return scheduler->execution_state != RVS_SessionExecutionState_Idle &&
         (operation_class == RVS_OperationClass_SessionExecution || operation_class == RVS_OperationClass_SessionLifecycle);
}

internal RVS_ScheduledOperation *
rvs_scheduler_operation_alloc_locked(RVS_Scheduler *scheduler, RVS_RequestPool *expected_pool, RVS_MessageID request_id, RVS_RequestPolicy policy, RVS_OperationKey key, RVS_ProgramID *targets, U64 targets_count, U64 captured_program_state_epoch)
{
  RVS_ScheduledOperation *operation = scheduler->free_first;
  if (operation) {
    scheduler->free_first = operation->next;
    MemoryZeroStruct(operation);
  } else {
    operation = push_array(scheduler->arena, RVS_ScheduledOperation, 1);
  }
  RVS_Request *request = rvs_request_pool_request_alloc(expected_pool);
  request->ref_count = 1; // caller ownership
  request->request_id = request_id;
  request->reply.request_id = request_id;
  request->reply.result = RVS_Result_Pending;

  operation->scheduler = scheduler;
  operation->request = request;
  operation->ref_count = 1; // active registration ownership
  operation->policy = policy;
  operation->key = key;
  operation->captured_program_state_epoch = captured_program_state_epoch;
  if (targets_count != 0) {
    operation->targets = push_array(scheduler->arena, RVS_ProgramID, targets_count);
    operation->targets_count = targets_count;
    MemoryCopyTyped(operation->targets, targets, targets_count);
    for EachIndex(target_idx, targets_count) {
      for (U64 previous_idx = target_idx; previous_idx != 0 && operation->targets[previous_idx].u64[0] < operation->targets[previous_idx - 1].u64[0]; previous_idx -= 1) {
        Swap(RVS_ProgramID, operation->targets[previous_idx], operation->targets[previous_idx - 1]);
      }
    }
  }
  rvs_request_addref(request); // operation ownership
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
    operation->next = scheduler->free_first;
    scheduler->free_first = operation;
  }
}

internal void
rvs_scheduler_operation_remove_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation)
{
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
rvs_scheduler_register_operation_locked(RVS_Scheduler *scheduler, RVS_RequestPool *expected_pool, RVS_OperationKey key, RVS_ScheduledOperation *operation)
{
  RVS_Result result = RVS_Result_Error;
  if (operation && operation->request->pool == expected_pool && rvs_operation_key_is_complete(key)) {
    result = RVS_Result_Ok;
    for (RVS_ScheduledOperation *n = scheduler->key_first; n; n = n->key_next) {
      if (rvs_operation_key_match(n->key, key)) { result = RVS_Result_AlreadyPending; break; }
    }
    if (result == RVS_Result_Ok) {
      operation->key = key;
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
rvs_scheduler_unregister_operation_locked(RVS_Scheduler *scheduler, RVS_OperationKey key)
{
  for (RVS_ScheduledOperation *n = scheduler->key_first; n; n = n->key_next) {
    if (rvs_operation_key_match(n->key, key)) {
      rvs_scheduler_operation_key_remove_locked(scheduler, n);
      return n;
    }
  }
  return 0;
}

internal RVS_ScheduledOperation *
rvs_scheduler_find_active_operation_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id)
{
  for EachNode(n, RVS_ScheduledOperation, scheduler->operation_first) {
    if (n->request->request_id == request_id) { return n; }
  }
  return 0;
}

internal RVS_ScheduledOperation *
rvs_scheduler_operation_mark_dispatched_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id)
{
  RVS_ScheduledOperation *operation = rvs_scheduler_find_active_operation_locked(scheduler, request_id);
  if (operation) {
    Mutex request_mutex = operation->request->mutex;
    mutex_take(request_mutex);
    if (operation->request->reply.result == RVS_Result_Pending) {
      operation->is_dispatched = 1;
    } else {
      operation = 0;
    }
    mutex_drop(request_mutex);
  }
  return operation;
}

internal RVS_ScheduledOperation *
rvs_scheduler_retire_undispatched_operation_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id)
{
  RVS_ScheduledOperation *operation = rvs_scheduler_find_active_operation_locked(scheduler, request_id);
  if (operation) {
    Mutex request_mutex = operation->request->mutex;
    mutex_take(request_mutex);
    if (operation->request->reply.result != RVS_Result_Pending && !operation->is_dispatched) {
      if (operation->key.operation_class == RVS_OperationClass_SessionExecution) {
        rvs_scheduler_clear_queued_execution_locked(scheduler, request_id);
      }
      rvs_scheduler_operation_remove_locked(scheduler, operation);
    } else {
      operation = 0;
    }
    mutex_drop(request_mutex);
  }
  return operation;
}

internal RVS_ScheduledOperation *
rvs_scheduler_take_active_operations_locked(RVS_Scheduler *scheduler)
{
  RVS_ScheduledOperation *first = scheduler->operation_first;
  scheduler->operation_first = 0;
  scheduler->operation_last = 0;
  return first;
}

internal RVS_ScheduledOperation *
rvs_scheduler_take_operation_keys_locked(RVS_Scheduler *scheduler)
{
  RVS_ScheduledOperation *first = scheduler->key_first;
  scheduler->key_first = 0;
  scheduler->key_last = 0;
  return first;
}

internal RVS_Result
rvs_scheduler_admit_locked(RVS_Scheduler *scheduler, RVS_RequestPool *expected_pool, U64 *next_request_id, RVS_RequestPolicy policy, RVS_OperationKey key, RVS_ProgramID *targets, U64 targets_count, U64 captured_program_state_epoch, RVS_SchedulerAdmission *admission_out)
{
  MemoryZeroStruct(admission_out);
  if (policy == RVS_RequestPolicy_JoinIfEqual) {
    for (RVS_ScheduledOperation *n = scheduler->key_first; n; n = n->key_next) {
      if (rvs_operation_key_match(n->key, key) &&
          (key.operation_class != RVS_OperationClass_ReadOnly || n->captured_program_state_epoch == captured_program_state_epoch)) {
        rvs_request_addref(n->request);
        admission_out->operation = n;
        admission_out->joined = 1;
        return RVS_Result_Ok;
      }
    }
  }
  if (rvs_scheduler_has_conflicting_operation_locked(scheduler, key)) {
    return RVS_Result_AlreadyPending;
  }

  RVS_ScheduledOperation *operation = rvs_scheduler_operation_alloc_locked(scheduler, expected_pool, ins_atomic_u64_inc_eval(next_request_id), policy, key, targets, targets_count, captured_program_state_epoch);
  if (policy == RVS_RequestPolicy_JoinIfEqual) {
    AssertAlways(rvs_scheduler_register_operation_locked(scheduler, expected_pool, key, operation) == RVS_Result_Ok);
    admission_out->registered = 1;
  }
  if (key.operation_class == RVS_OperationClass_SessionExecution) {
    AssertAlways(rvs_scheduler_reserve_execution_locked(scheduler, operation->request->request_id));
  }
  admission_out->operation = operation;
  return RVS_Result_Ok;
}

internal void
rvs_scheduler_rollback_admission_locked(RVS_Scheduler *scheduler, RVS_SchedulerAdmission *admission)
{
  AssertAlways(admission->operation != 0 && !admission->joined);
  RVS_ScheduledOperation *operation = admission->operation;
  if (operation->key.operation_class == RVS_OperationClass_SessionExecution) {
    rvs_scheduler_clear_queued_execution_locked(scheduler, operation->request->request_id);
  }
  rvs_scheduler_operation_remove_locked(scheduler, operation);
  RVS_ScheduledOperation *registered_operation = 0;
  if (admission->registered) {
    registered_operation = rvs_scheduler_unregister_operation_locked(scheduler, operation->key);
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
  owner->operation = operation;
  owner->registered = registered;
  rvs_engine_control_addref(owner->control);
  rvs_scheduler_operation_addref(operation); // request control ownership
  return owner;
}

void
rvs_request_control_release(RVS_RequestControl *owner)
{
  if (owner == 0) { return; }
  RVS_ScheduledOperation *registered_operation = 0;
  mutex_take(owner->control->mutex);
  if (owner->registered && !owner->control->is_shutdown) {
    registered_operation = rvs_scheduler_unregister_operation_locked(&owner->session->scheduler, owner->operation->key);
  }
  mutex_drop(owner->control->mutex);
  if (registered_operation) { rvs_scheduler_operation_release(registered_operation); }
  rvs_scheduler_operation_release(owner->operation);
  rvs_engine_control_release(owner->control);
  arena_release(owner->arena);
}

RVS_Result
rvs_request_control_cancel(RVS_RequestControl *owner)
{
  if (owner == 0 || owner->operation == 0 || owner->session == 0) { return RVS_Result_Error; }
  RVS_Result result = RVS_Result_Error;
  mutex_take(owner->control->mutex);
  if (owner->control->is_shutdown) {
    result = RVS_Result_EngineStopped;
  } else {
    RVS_ScheduledOperation *operation = rvs_scheduler_find_active_operation_locked(&owner->session->scheduler, owner->operation->request->request_id);
    if (operation == owner->operation) {
      RVS_Request *request = operation->request;
      mutex_take(request->mutex);
      if (operation->is_dispatched) { result = RVS_Result_Unsupported; }
      else if (request->reply.result == RVS_Result_Pending) {
        request->reply = (RVS_EngineReply){ .request_id = request->request_id, .result = RVS_Result_Cancelled, .kind = request->reply.kind };
        cond_var_broadcast(request->cv);
        result = RVS_Result_Ok;
      }
      mutex_drop(request->mutex);
    }
  }
  mutex_drop(owner->control->mutex);
  return result;
}
