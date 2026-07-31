// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#include "rvs_request.h"

internal void
rvs_request_list_push_node(RVS_RequestList *list, RVS_Request *r)
{
  SLLQueuePush(list->first, list->last, r);
  list->count += 1;
}

internal RVS_Request *
rvs_request_list_push(Arena *arena, RVS_RequestList *list)
{
  RVS_Request *r = push_array(arena, RVS_Request, 1);
  rvs_request_list_push_node(list, r);
  return r;
}

internal RVS_Request *
rvs_request_list_pop(RVS_RequestList *list)
{
  if (list->count) {
    RVS_Request *result = list->first;
    list->count -= 1;
    SLLQueuePop(list->first, list->last);
    return result;
  }
  return 0;
}

internal void
rvs_request_queue_init(RVS_RequestQueue *q, Arena *a)
{
  q->arena        = a;
  q->mutex        = mutex_alloc();
  q->available_cv = cond_var_alloc();
}

internal void
rvs_request_queue_release(RVS_RequestQueue *q)
{
  cond_var_release(q->available_cv);
  mutex_release(q->mutex);
}

internal RVS_Request *
rvs_request_alloc(RVS_RequestQueue *q, U64 message_size, U64 message_align)
{
  RVS_Request *r = r = rvs_request_list_pop(&q->free_list);
  if (r) {
    CondVar complete_cv = r->complete_cv;
    MemoryZero(r, message_size);
    r->complete_cv = complete_cv;
  } else {
    r = arena_push(q->arena, message_size, Max(8, message_size), 1);
    r->complete_cv = cond_var_alloc();
  }
  return r;
}

internal void
rvs_request_release(RVS_Request *r)
{
  cond_var_release(r->complete_cv);
  MemoryZeroStruct(r);
}

internal void
rvs_request_queue_recycle(RVS_RequestQueue *q, RVS_Request *r)
{
  MutexScope(q->mutex) {
    rvs_request_list_push_node(&q->free_list, r);
  }
}

internal B32
rvs_request_queue_push(RVS_RequestQueue *q, RVS_Request *r)
{
  AssertAlways(r->status == RVS_RequestStatus_Null);
  r->status = RVS_RequestStatus_Pending;
  rvs_request_list_push_node(&q->requests, r);
  cond_var_broadcast(q->available_cv);
  return 1;
}

internal B32
rvs_request_queue_wait_for(RVS_RequestQueue *q, RVS_Request *r, U64 wait_us)
{
  for (U64 endt_us = now_time_us() + wait_us; r->status == RVS_RequestStatus_Pending; ) {
    if (wait_us != max_U64 && now_time_us() >= endt_us) {
      rvs_request_queue_complete(q, r, RVS_Result_Timeout);
      return 0;
    }
    cond_var_wait(r->complete_cv, q->mutex, endt_us);
  }
  return 1;
}

internal B32
rvs_request_queue_submit(RVS_RequestQueue *q, RVS_Request *r, U64 wait_us)
{
  B32 is_ok = rvs_request_queue_push(q, r);
  if (is_ok) {
    is_ok = rvs_request_queue_wait(q, r, wait_us);
  }
  return is_ok;
}

internal B32
rvs_request_queue_complete(RVS_RequestQueue *q, RVS_Request *r, RVS_Result result)
{
  B32 is_completed = 0;
  MutexScope(q->mutex) {
    r->result = result;
    r->status = RVS_RequestStatus_Complete;
    cond_var_broadcast(r->complete_cv);
  }
}

