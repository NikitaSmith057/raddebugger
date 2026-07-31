// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

//
// thread-safe queue for trasnfering messages between the workers
//

#include "rvs_protocol.h"

internal void
rvs_queue_message_list_push_node(RVS_QueueMessageList *l, RVS_QueueMessage *r)
{
  SLLQueuePush(l->first, l->last, r);
  l->count += 1;
}

internal RVS_QueueMessage *
rvs_queue_message_list_push(Arena *arena, RVS_QueueMessageList *l)
{
  RVS_QueueMessage *r = push_array(arena, RVS_QueueMessage, 1);
  rvs_queue_message_list_push_node(l, r);
  return r;
}

internal RVS_QueueMessage *
rvs_queue_message_list_pop(RVS_QueueMessageList *l)
{
  if (l->count) {
    RVS_QueueMessage *result = l->first;
    l->count -= 1;
    SLLQueuePop(l->first, l->last);
    return result;
  }
  return 0;
}

internal void
rvs_queue_alloc(RVS_Queue *q, Arena *a, U64 message_size, U64 message_align)
{
  q->arena         = a;
  q->mutex         = mutex_alloc();
  q->available_cv  = cond_var_alloc();
  q->message_size  = message_size;
  q->message_align = message_align;
}

internal void
rvs_queue_release(RVS_Queue *q)
{
  cond_var_release(q->available_cv);
  mutex_release(q->mutex);
}

internal RVS_QueueMessage *
rvs_queue_alloc_message(RVS_Queue *q)
{
  RVS_QueueMessage *r = r = rvs_queue_message_list_pop(&q->free_list);
  if (r) {
    CondVar complete_cv = r->complete_cv;
    MemoryZero(r, q->message_size);
    r->complete_cv = complete_cv;
  } else {
    r = arena_push(q->arena, q->message_size, Max(8, q->message_size), 1);
    r->complete_cv = cond_var_alloc();
  }
  r->reply_id = ins_atomic_u64_inc_eval(&q->next_reply_id);
  return r;
}

internal void
rvs_queue_message_release(RVS_QueueMessage *r)
{
  cond_var_release(r->complete_cv);
  MemoryZeroStruct(r);
}

internal void
rvs_queue_recycle(RVS_Queue *q, RVS_QueueMessage *r)
{
  MutexScope(r->queue->mutex) {
    rvs_queue_message_list_push_node(&r->queue->free_list, r);
  }
}

internal B32
rvs_queue_push(RVS_Queue *q, RVS_QueueMessage *r)
{
  AssertAlways(r->status == RVS_QueueMessageStatus_Null);
  r->status = RVS_QueueMessageStatus_Pending;
  rvs_queue_message_list_push_node(&q->messages, r);
  cond_var_broadcast(q->available_cv);
  return 1;
}

internal RVS_QueueMessage *
rvs_queue_pop(RVS_Queue *q, U64 wait_us)
{
  RVS_QueueMessage *result = 0;
  U64 endt_us = now_time_us() + wait_us;
  if (q->messages.count == 0) {
    if (cond_var_wait(q->available_cv, q->mutex, endt_us)) {
      result = rvs_queue_message_list_pop(&q->messages);
    }
  }
  return result;
}

internal B32
rvs_queue_wait_for_message_to_complete(RVS_Queue *q, RVS_QueueMessage *r, U64 wait_us)
{
  for (U64 endt_us = now_time_us() + wait_us; r->status == RVS_QueueMessageStatus_Pending; ) {
    if (wait_us != max_U64 && now_time_us() >= endt_us) {
      rvs_queue_complete(q, r, RVS_Result_Timeout);
      return 0;
    }
    cond_var_wait(r->complete_cv, q->mutex, endt_us);
  }
  return 1;
}

internal B32
rvs_queue_send_message(RVS_Queue *q, RVS_QueueMessage *r, U64 wait_us)
{
  B32 is_ok = rvs_queue_push(q, r);
  if (is_ok) {
    is_ok = rvs_queue_wait(q, r, wait_us);
  }
  return is_ok;
}

internal B32
rvs_queue_complete(RVS_Queue *q, RVS_QueueMessage *r, RVS_Result result)
{
  B32 is_completed = 0;
  MutexScope(q->mutex) {
    r->result = result;
    r->status = RVS_QueueMessageStatus_Complete;
    cond_var_broadcast(r->complete_cv);
  }
}

