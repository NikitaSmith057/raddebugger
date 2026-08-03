// This file is included only by rvs_engine.c after private engine/session types.

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
rvs_session_has_conflicting_operation_locked(RVS_Session *session, RVS_OperationKey key)
{
  for EachNode(request, RVS_Request, session->scheduler.request_first) {
    if (rvs_operation_keys_conflict(request->key, key)) { return 1; }
  }
  return 0;
}

internal B32
rvs_session_reserve_execution_locked(RVS_Session *session, RVS_MessageID request_id)
{
  RVS_Scheduler *scheduler = &session->scheduler;
  if (request_id == 0 || scheduler->execution_state != RVS_SessionExecutionState_Idle || scheduler->execution_request_id != 0) { return 0; }
  scheduler->execution_state = RVS_SessionExecutionState_Queued;
  scheduler->execution_request_id = request_id;
  return 1;
}

internal B32
rvs_session_mark_run_in_flight_locked(RVS_Session *session, RVS_MessageID request_id)
{
  RVS_Scheduler *scheduler = &session->scheduler;
  if (scheduler->execution_state != RVS_SessionExecutionState_Queued || scheduler->execution_request_id != request_id) { return 0; }
  scheduler->execution_state = RVS_SessionExecutionState_RunInFlight;
  return 1;
}

internal B32
rvs_session_clear_queued_execution_locked(RVS_Session *session, RVS_MessageID request_id)
{
  RVS_Scheduler *scheduler = &session->scheduler;
  if (scheduler->execution_state != RVS_SessionExecutionState_Queued || scheduler->execution_request_id != request_id) { return 0; }
  scheduler->execution_state = RVS_SessionExecutionState_Idle;
  scheduler->execution_request_id = 0;
  return 1;
}

internal B32
rvs_session_finish_run_locked(RVS_Session *session, RVS_MessageID request_id)
{
  RVS_Scheduler *scheduler = &session->scheduler;
  if (scheduler->execution_state != RVS_SessionExecutionState_RunInFlight || scheduler->execution_request_id != request_id) { return 0; }
  scheduler->execution_state = RVS_SessionExecutionState_Idle;
  scheduler->execution_request_id = 0;
  return 1;
}

internal B32
rvs_session_execution_blocks_operation_locked(RVS_Session *session, RVS_OperationClass operation_class)
{
  return session->scheduler.execution_state != RVS_SessionExecutionState_Idle &&
         (operation_class == RVS_OperationClass_SessionExecution || operation_class == RVS_OperationClass_SessionLifecycle);
}

internal RVS_RequestPool *
rvs_request_pool_alloc(void)
{
  Arena *arena = arena_alloc(.name = "Engine Request Pool");
  RVS_RequestPool *pool = push_array(arena, RVS_RequestPool, 1);
  pool->arena = arena;
  pool->mutex = mutex_alloc();
  return pool;
}

internal void
rvs_request_pool_destroy(RVS_RequestPool *pool)
{
  AssertAlways(pool->live_requests_count == 0);
  for (RVS_Request *request = pool->free_first; request; request = request->next) {
    cond_var_release(request->cv);
    mutex_release(request->mutex);
  }
  mutex_release(pool->mutex);
  arena_release(pool->arena);
}

internal RVS_Request *
rvs_request_pool_request_alloc(RVS_RequestPool *pool)
{
  mutex_take(pool->mutex);
  AssertAlways( ! pool->engine_released);
  RVS_Request *request = pool->free_first;
  if (request) {
    pool->free_first = request->next;
    Mutex mutex = request->mutex;
    CondVar cv = request->cv;
    MemoryZeroStruct(request);
    request->mutex = mutex;
    request->cv = cv;
  } else {
    request = push_array(pool->arena, RVS_Request, 1);
    request->mutex = mutex_alloc();
    request->cv = cond_var_alloc();
  }
  request->pool = pool;
  pool->live_requests_count += 1;
  mutex_drop(pool->mutex);
  return request;
}

internal void
rvs_request_pool_request_release(RVS_Request *request)
{
  RVS_RequestPool *pool = request->pool;
  mutex_take(pool->mutex);
  AssertAlways(pool->live_requests_count != 0);
  request->next = pool->free_first;
  pool->free_first = request;
  pool->live_requests_count -= 1;
  B32 destroy_pool = pool->engine_released && pool->live_requests_count == 0;
  mutex_drop(pool->mutex);
  if (destroy_pool) { rvs_request_pool_destroy(pool); }
}

internal void
rvs_request_pool_release_engine(RVS_RequestPool *pool)
{
  mutex_take(pool->mutex);
  AssertAlways( ! pool->engine_released);
  pool->engine_released = 1;
  B32 destroy_pool = pool->live_requests_count == 0;
  mutex_drop(pool->mutex);
  if (destroy_pool) { rvs_request_pool_destroy(pool); }
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
  request->session = session;
  request->key = key;
  request->reply.request_id = request_id;
  request->reply.result = RVS_Result_Pending;
  DLLPushBack(session->scheduler.request_first, session->scheduler.request_last, request);
  return request;
}

internal void
rvs_session_request_remove_locked(RVS_Session *session, RVS_Request *request)
{
  DLLRemove(session->scheduler.request_first, session->scheduler.request_last, request);
}

internal void
rvs_session_request_key_remove_locked(RVS_Session *session, RVS_Request *request)
{
  if (request->key_prev) { request->key_prev->key_next = request->key_next; }
  else { session->scheduler.key_first = request->key_next; }
  if (request->key_next) { request->key_next->key_prev = request->key_prev; }
  else { session->scheduler.key_last = request->key_prev; }
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
rvs_session_unregister_operation_locked(RVS_Session *session, RVS_OperationKey key)
{
  for (RVS_Request *n = session->scheduler.key_first; n; n = n->key_next) {
    if (rvs_operation_key_match(n->key, key)) {
      rvs_session_request_key_remove_locked(session, n);
      return n;
    }
  }
  return 0;
}

internal B32
rvs_request_complete(RVS_Request *request, RVS_EngineReply reply)
{
  B32 completed = 0;
  mutex_take(request->mutex);
  if (request->reply.result == RVS_Result_Pending) {
    request->reply = reply;
    cond_var_broadcast(request->cv);
    completed = 1;
  }
  mutex_drop(request->mutex);
  return completed;
}

internal RVS_Request *
rvs_session_find_active_request_locked(RVS_Session *session, RVS_MessageID request_id)
{
  for EachNode(n, RVS_Request, session->scheduler.request_first) {
    if (n->request_id == request_id) { return n; }
  }
  return 0;
}

internal RVS_Request *
rvs_session_request_mark_dispatched_locked(RVS_Session *session, RVS_MessageID request_id)
{
  RVS_Request *request = rvs_session_find_active_request_locked(session, request_id);
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
rvs_session_retire_undispatched_request_locked(RVS_Session *session, RVS_MessageID request_id)
{
  RVS_Request *request = rvs_session_find_active_request_locked(session, request_id);
  if (request) {
    Mutex request_mutex = request->mutex;
    mutex_take(request_mutex);
    if (request->reply.result != RVS_Result_Pending && !request->is_dispatched) {
      if (request->key.operation_class == RVS_OperationClass_SessionExecution) {
        rvs_session_clear_queued_execution_locked(session, request_id);
      }
      rvs_session_request_remove_locked(session, request);
    } else {
      request = 0;
    }
    mutex_drop(request_mutex);
  }
  return request;
}

internal RVS_Request *
rvs_session_take_active_requests_locked(RVS_Session *session)
{
  RVS_Request *first = session->scheduler.request_first;
  session->scheduler.request_first = 0;
  session->scheduler.request_last = 0;
  return first;
}

internal RVS_Request *
rvs_session_take_operation_keys_locked(RVS_Session *session)
{
  RVS_Request *first = session->scheduler.key_first;
  session->scheduler.key_first = 0;
  session->scheduler.key_last = 0;
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
  if (rvs_session_has_conflicting_operation_locked(session, key)) {
    return RVS_Result_AlreadyPending;
  }

  RVS_Request *request = rvs_session_request_alloc_locked(session, pool, ins_atomic_u64_inc_eval(next_request_id), key);
  request->captured_program_state_epoch = captured_program_state_epoch;
  if (policy == RVS_RequestPolicy_JoinIfEqual) {
    AssertAlways(rvs_session_register_operation_locked(session, key, request) == RVS_Result_Ok);
    admission_out->registered = 1;
  }
  if (key.operation_class == RVS_OperationClass_SessionExecution) {
    AssertAlways(rvs_session_reserve_execution_locked(session, request->request_id));
  }
  admission_out->request = request;
  return RVS_Result_Ok;
}

internal void
rvs_scheduler_rollback_admission_locked(RVS_Session *session, RVS_SchedulerAdmission *admission)
{
  AssertAlways(admission->request != 0 && !admission->joined);
  RVS_Request *request = admission->request;
  if (request->key.operation_class == RVS_OperationClass_SessionExecution) {
    rvs_session_clear_queued_execution_locked(session, request->request_id);
  }
  rvs_session_request_remove_locked(session, request);
  RVS_Request *registered_request = 0;
  if (admission->registered) {
    registered_request = rvs_session_unregister_operation_locked(session, request->key);
    AssertAlways(registered_request == request);
  }
  rvs_request_release(request);
  rvs_request_release(request);
  if (registered_request) { rvs_request_release(registered_request); }
  MemoryZeroStruct(admission);
}

void
rvs_request_addref(RVS_Request *request)
{
  AssertAlways(request != 0);
  ins_atomic_u32_inc_eval(&request->ref_count);
}

void
rvs_request_release(RVS_Request *request)
{
  AssertAlways(request != 0);
  if (ins_atomic_u32_dec_eval(&request->ref_count) == 0) { rvs_request_pool_request_release(request); }
}

RVS_Result
rvs_request_wait(RVS_Request *request, U64 wait_us, RVS_EngineReply *reply_out)
{
  if (request == 0) { return RVS_Result_Error; }
  U64 endt_us = max_U64;
  if (wait_us != max_U64) {
    U64 now_us = now_time_us();
    endt_us = now_us + Min(wait_us, max_U64 - now_us);
  }
  mutex_take(request->mutex);
  while (request->reply.result == RVS_Result_Pending) {
    if ( ! cond_var_wait(request->cv, request->mutex, endt_us)) {
      mutex_drop(request->mutex);
      return RVS_Result_Timeout;
    }
  }
  if (reply_out) { *reply_out = request->reply; }
  mutex_drop(request->mutex);
  return RVS_Result_Ok;
}

void
rvs_request_control_release(RVS_RequestControl *owner)
{
  if (owner == 0) { return; }
  RVS_Request *registered_request = 0;
  mutex_take(owner->control->mutex);
  if (owner->registered && !owner->control->is_shutdown) {
    registered_request = rvs_session_unregister_operation_locked(owner->session, owner->key);
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
    RVS_Request *request = rvs_session_find_active_request_locked(owner->session, owner->request->request_id);
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
