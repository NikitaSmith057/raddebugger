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
  for EachNode(request, RVS_Request, scheduler->request_first) {
    if (rvs_operation_keys_conflict(request->key, key)) { return 1; }
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

internal RVS_RequestControl *
rvs_request_control_alloc(RVS_Session *session, RVS_Request *request, RVS_OperationKey key, B32 registered)
{
  Arena *arena = arena_alloc(.name = "Engine Operation Owner");
  RVS_RequestControl *owner = push_array(arena, RVS_RequestControl, 1);
  owner->arena = arena;
  owner->session = session;
  owner->control = session->control;
  owner->request = request;
  owner->key = key;
  owner->registered = registered;
  rvs_engine_control_addref(owner->control);
  rvs_request_addref(request);
  return owner;
}

internal RVS_Request *
rvs_session_request_alloc_locked(RVS_Session *session, RVS_RequestPool *pool, RVS_MessageID request_id, RVS_OperationKey key)
{
  RVS_Request *request = rvs_request_pool_request_alloc(pool);
  request->ref_count = 2;
  request->request_id = request_id;
  request->key = key;
  request->reply.request_id = request_id;
  request->reply.result = RVS_Result_Pending;
  DLLPushBack(session->scheduler.request_first, session->scheduler.request_last, request);
  return request;
}

internal void
rvs_scheduler_request_remove_locked(RVS_Scheduler *scheduler, RVS_Request *request)
{
  DLLRemove(scheduler->request_first, scheduler->request_last, request);
}

internal void
rvs_scheduler_request_key_remove_locked(RVS_Scheduler *scheduler, RVS_Request *request)
{
  if (request->key_prev) { request->key_prev->key_next = request->key_next; }
  else { scheduler->key_first = request->key_next; }
  if (request->key_next) { request->key_next->key_prev = request->key_prev; }
  else { scheduler->key_last = request->key_prev; }
  request->key_next = 0;
  request->key_prev = 0;
}

internal RVS_Result
rvs_session_register_operation_locked(RVS_Session *session, RVS_OperationKey key, RVS_Request *request)
{
  RVS_Result result = RVS_Result_Error;
  if (request && request->pool == session->engine->request_pool && rvs_operation_key_is_complete(key)) {
    result = RVS_Result_Ok;
    for (RVS_Request *n = session->scheduler.key_first; n; n = n->key_next) {
      if (rvs_operation_key_match(n->key, key)) { result = RVS_Result_AlreadyPending; break; }
    }
    if (result == RVS_Result_Ok) {
      request->key = key;
      rvs_request_addref(request);
      request->key_prev = session->scheduler.key_last;
      if (session->scheduler.key_last) { session->scheduler.key_last->key_next = request; }
      else { session->scheduler.key_first = request; }
      session->scheduler.key_last = request;
    }
  }
  return result;
}

internal RVS_Request *
rvs_scheduler_unregister_operation_locked(RVS_Scheduler *scheduler, RVS_OperationKey key)
{
  for (RVS_Request *n = scheduler->key_first; n; n = n->key_next) {
    if (rvs_operation_key_match(n->key, key)) {
      rvs_scheduler_request_key_remove_locked(scheduler, n);
      return n;
    }
  }
  return 0;
}

internal RVS_Request *
rvs_scheduler_find_active_request_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id)
{
  for EachNode(n, RVS_Request, scheduler->request_first) {
    if (n->request_id == request_id) { return n; }
  }
  return 0;
}

internal RVS_Request *
rvs_scheduler_request_mark_dispatched_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id)
{
  RVS_Request *request = rvs_scheduler_find_active_request_locked(scheduler, request_id);
  if (request) {
    Mutex request_mutex = request->mutex;
    mutex_take(request_mutex);
    if (request->reply.result == RVS_Result_Pending) {
      request->is_dispatched = 1;
    } else {
      request = 0;
    }
    mutex_drop(request_mutex);
  }
  return request;
}

internal RVS_Request *
rvs_scheduler_retire_undispatched_request_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id)
{
  RVS_Request *request = rvs_scheduler_find_active_request_locked(scheduler, request_id);
  if (request) {
    Mutex request_mutex = request->mutex;
    mutex_take(request_mutex);
    if (request->reply.result != RVS_Result_Pending && !request->is_dispatched) {
      if (request->key.operation_class == RVS_OperationClass_SessionExecution) {
        rvs_scheduler_clear_queued_execution_locked(scheduler, request_id);
      }
      rvs_scheduler_request_remove_locked(scheduler, request);
    } else {
      request = 0;
    }
    mutex_drop(request_mutex);
  }
  return request;
}

internal RVS_Request *
rvs_scheduler_take_active_requests_locked(RVS_Scheduler *scheduler)
{
  RVS_Request *first = scheduler->request_first;
  scheduler->request_first = 0;
  scheduler->request_last = 0;
  return first;
}

internal RVS_Request *
rvs_scheduler_take_operation_keys_locked(RVS_Scheduler *scheduler)
{
  RVS_Request *first = scheduler->key_first;
  scheduler->key_first = 0;
  scheduler->key_last = 0;
  return first;
}

internal RVS_Result
rvs_scheduler_admit_locked(RVS_Session *session, RVS_RequestPool *pool, U64 *next_request_id, RVS_RequestPolicy policy, RVS_OperationKey key, U64 captured_program_state_epoch, RVS_SchedulerAdmission *admission_out)
{
  MemoryZeroStruct(admission_out);
  if (policy == RVS_RequestPolicy_JoinIfEqual) {
    for (RVS_Request *n = session->scheduler.key_first; n; n = n->key_next) {
      if (rvs_operation_key_match(n->key, key) &&
          (key.operation_class != RVS_OperationClass_ReadOnly || n->captured_program_state_epoch == captured_program_state_epoch)) {
        rvs_request_addref(n);
        admission_out->request = n;
        admission_out->joined = 1;
        return RVS_Result_Ok;
      }
    }
  }
  if (rvs_scheduler_has_conflicting_operation_locked(&session->scheduler, key)) {
    return RVS_Result_AlreadyPending;
  }

  RVS_Request *request = rvs_session_request_alloc_locked(session, pool, ins_atomic_u64_inc_eval(next_request_id), key);
  request->captured_program_state_epoch = captured_program_state_epoch;
  if (policy == RVS_RequestPolicy_JoinIfEqual) {
    AssertAlways(rvs_session_register_operation_locked(session, key, request) == RVS_Result_Ok);
    admission_out->registered = 1;
  }
  if (key.operation_class == RVS_OperationClass_SessionExecution) {
    AssertAlways(rvs_scheduler_reserve_execution_locked(&session->scheduler, request->request_id));
  }
  admission_out->request = request;
  return RVS_Result_Ok;
}

internal void
rvs_scheduler_rollback_admission_locked(RVS_Scheduler *scheduler, RVS_SchedulerAdmission *admission)
{
  AssertAlways(admission->request != 0 && !admission->joined);
  RVS_Request *request = admission->request;
  if (request->key.operation_class == RVS_OperationClass_SessionExecution) {
    rvs_scheduler_clear_queued_execution_locked(scheduler, request->request_id);
  }
  rvs_scheduler_request_remove_locked(scheduler, request);
  RVS_Request *registered_request = 0;
  if (admission->registered) {
    registered_request = rvs_scheduler_unregister_operation_locked(scheduler, request->key);
    AssertAlways(registered_request == request);
  }
  rvs_request_release(request);
  rvs_request_release(request);
  if (registered_request) { rvs_request_release(registered_request); }
  MemoryZeroStruct(admission);
}

void
rvs_request_control_release(RVS_RequestControl *owner)
{
  if (owner == 0) { return; }
  RVS_Request *registered_request = 0;
  mutex_take(owner->control->mutex);
  if (owner->registered && !owner->control->is_shutdown) {
    registered_request = rvs_scheduler_unregister_operation_locked(&owner->session->scheduler, owner->key);
  }
  mutex_drop(owner->control->mutex);
  if (registered_request) { rvs_request_release(registered_request); }
  rvs_request_release(owner->request);
  rvs_engine_control_release(owner->control);
  arena_release(owner->arena);
}

RVS_Result
rvs_request_control_cancel(RVS_RequestControl *owner)
{
  if (owner == 0 || owner->request == 0 || owner->session == 0) { return RVS_Result_Error; }
  RVS_Result result = RVS_Result_Error;
  mutex_take(owner->control->mutex);
  if (owner->control->is_shutdown) {
    result = RVS_Result_EngineStopped;
  } else {
    RVS_Request *request = rvs_scheduler_find_active_request_locked(&owner->session->scheduler, owner->request->request_id);
    if (request == owner->request) {
      mutex_take(request->mutex);
      if (request->is_dispatched) { result = RVS_Result_Unsupported; }
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
