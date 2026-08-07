// This file is included only by rvs_engine.c.

#include "radvs/rvs_request.h"

internal RVS_RequestPool *
rvs_request_pool_alloc(void)
{
  Arena *arena = arena_alloc(.name = "Engine Request Pool");
  RVS_RequestPool *pool = push_array(arena, RVS_RequestPool, 1);
  pool->arena         = arena;
  pool->mutex         = mutex_alloc();
  pool->request_by_id = hash_table_init(arena, 1024);
  return pool;
}

internal void
rvs_request_pool_destroy(RVS_RequestPool *pool)
{
  AssertAlways(pool->live_requests_count == 0);
  for EachNode(n, RVS_RequestPoolNode, pool->free_list) {
    cond_var_release(n->request.cv);
    mutex_release(n->request.mutex);
  }
  mutex_release(pool->mutex);
  arena_release(pool->arena);
}

internal RVS_Request *
rvs_request_pool_request_alloc(RVS_RequestPool *pool)
{
  mutex_take(pool->mutex);
  AssertAlways( ! pool->engine_released);

  RVS_RequestPoolNode *node = pool->free_list;
  RVS_Request *request = 0;

  if (node) {
    request = &node->request;
    Mutex   mutex = request->mutex;
    CondVar cv    = request->cv;
    MemoryZeroStruct(request);
    pool->free_list = node->next;

    request->mutex = mutex;
    request->cv = cv;
  } else {
    node = push_array(pool->arena, RVS_RequestPoolNode, 1);
    request = &node->request;
    request->mutex = mutex_alloc();
    request->cv = cond_var_alloc();
  }
  request->pool = pool;

  // self register the request
  hash_table_push_u64_raw(pool->arena, pool->request_by_id, request->id, request);

  pool->live_requests_count += 1;

  mutex_drop(pool->mutex);
  return request;
}

internal U64
rvs_request_pool_release_request(RVS_RequestPool *pool, RVS_Request *request)
{
  U64 was_released = 0;

  mutex_take(pool->mutex);

  if (pool->live_requests_count > 0) {
    // move request node to the free list
    SLLStackPush(pool->free_list, CastFromMember(RVS_RequestPoolNode, request, request));

    // update pool state
    pool->live_requests_count -= 1;

    was_released = 1;
  }

  mutex_drop(pool->mutex);

  return was_released;
}

internal void
rvs_request_pool_release_engine(RVS_RequestPool *pool)
{
  mutex_take(pool->mutex);
  AssertAlways( ! pool->engine_released);
  pool->engine_released = 1;
  B32 destroy_pool = pool->live_requests_count == 0;
  mutex_drop(pool->mutex);

  if (destroy_pool) {
    rvs_request_pool_destroy(pool);
  }
}

internal RVS_Request *
rvs_request_from_id(RVS_RequestPool *pool, RVS_MessageID id)
{
  return hash_table_search_u64_raw(pool->request_by_id, id);
}

B32
rvs_request_complete(RVS_Request *request, RVS_CommandReply reply)
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

U64
rvs_request_addref(RVS_Request *request)
{
  AssertAlways(request->pool);
  return ins_atomic_u32_inc_eval(&request->ref_count);
}

U64
rvs_request_release(RVS_Request *request)
{
  AssertAlways(request->pool);
  if (ins_atomic_u32_dec_eval(&request->ref_count) == 0) {
    return rvs_request_pool_release_request(request->pool, request);
  }
  return 0;
}

RVS_Result
rvs_request_wait(RVS_Request *request, U64 wait_us, RVS_CommandReply *reply_out)
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

