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

internal RVS_Queue *
rvs_queue_alloc(Arena *arena, U64 message_size, U64 message_align)
{
  RVS_Queue *q = push_array(arena, RVS_Queue, 1);
  q->arena         = arena;
  q->mutex         = mutex_alloc();
  q->available_cv  = cond_var_alloc();
  q->message_size  = message_size;
  q->message_align = message_align;
  return q;
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
  mutex_take(q->mutex);

  RVS_QueueMessage *r = rvs_queue_message_list_pop(&q->free_list);
  if (r) {
    CondVar complete_cv = r->complete_cv;
    MemoryZero(r, q->message_size);
    r->complete_cv = complete_cv;
  } else {
    r = arena_push(q->arena, q->message_size, q->message_align, 1);
    r->complete_cv = cond_var_alloc();
  }
  r->queue    = q;
  r->reply_id = ins_atomic_u64_inc_eval(&q->next_reply_id);

  mutex_drop(q->mutex);
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
  AssertAlways(r->queue == q);
  mutex_take(q->mutex);
  rvs_queue_message_list_push_node(&q->free_list, r);
  mutex_drop(q->mutex);
}

internal RVS_Result
rvs_queue_push(RVS_Queue *q, RVS_QueueMessage *r)
{
  AssertAlways(r->queue == q);

  mutex_take(q->mutex);
  AssertAlways(r->status == RVS_QueueMessageStatus_Null);
  r->status = RVS_QueueMessageStatus_Pending;
  rvs_queue_message_list_push_node(&q->messages, r);
  cond_var_broadcast(q->available_cv);
  mutex_drop(q->mutex);

  return RVS_Result_Ok;
}

internal RVS_QueueMessage *
rvs_queue_pop(RVS_Queue *q, U64 wait_us)
{
  RVS_QueueMessage *result = 0;
  U64 endt_us = max_U64;
  if (wait_us != max_U64) {
    U64 now_us = now_time_us();
    endt_us = now_us + Min(wait_us, max_U64 - now_us);
  }

  mutex_take(q->mutex);
  while (q->messages.count == 0) {
    if ( ! cond_var_wait(q->available_cv, q->mutex, endt_us)) {
      break;
    }
  }
  result = rvs_queue_message_list_pop(&q->messages);
  mutex_drop(q->mutex);

  return result;
}

internal B32
rvs_queue_wait_for(RVS_Queue *q, RVS_QueueMessage *r, U64 wait_us)
{
  U64 endt_us = max_U64;
  if (wait_us != max_U64) {
    U64 now_us = now_time_us();
    endt_us = now_us + Min(wait_us, max_U64 - now_us);
  }

  mutex_take(q->mutex);
  while (r->status == RVS_QueueMessageStatus_Pending) {
    if ( ! cond_var_wait(r->complete_cv, q->mutex, endt_us)) {
      mutex_drop(q->mutex);
      return 0;
    }
  }
  mutex_drop(q->mutex);
  return r->status == RVS_QueueMessageStatus_Complete;
}

internal RVS_Result
rvs_queue_send_message(RVS_Queue *q, RVS_QueueMessage *r, U64 wait_us)
{
  RVS_Result result = rvs_queue_push(q, r);
  if (result != RVS_Result_Ok) {
    return result;
  }
  if ( ! rvs_queue_wait_for(q, r, wait_us)) {
    return RVS_Result_Timeout;
  }
  return r->result;
}

internal B32
rvs_queue_complete(RVS_Queue *q, RVS_QueueMessage *r, RVS_Result result)
{
  B32 is_completed = 0;

  mutex_take(q->mutex);
  if (r->status == RVS_QueueMessageStatus_Pending) {
    r->result = result;
    r->status = RVS_QueueMessageStatus_Complete;
    cond_var_broadcast(r->complete_cv);
    is_completed = 1;
  }
  mutex_drop(q->mutex);

  return is_completed;
}

