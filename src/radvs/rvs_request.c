// This file is included only by rvs_engine.c.

#include "radvs/rvs_request.h"

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
