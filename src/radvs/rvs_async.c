// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#include "rvs_async.h"

internal void
rvs_queue_node_list_push(RVS_QueueNodeList *list, RVS_QueueNode *node)
{
  SLLQueuePush(list->first, list->last, node);
  list->count += 1;
}

internal RVS_QueueNode *
rvs_queue_node_list_pop(RVS_QueueNodeList *list)
{
  RVS_QueueNode *result = 0;
  if (list->count != 0) {
    result = list->first;
    list->count -= 1;
    SLLQueuePop(list->first, list->last);
  }
  return result;
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
rvs_queue_close(RVS_Queue *q)
{
  mutex_take(q->mutex);
  q->is_closed = 1;
  cond_var_broadcast(q->available_cv);
  mutex_drop(q->mutex);
}

internal void
rvs_queue_release(RVS_Queue *q)
{
  cond_var_release(q->available_cv);
  mutex_release(q->mutex);
}

internal RVS_QueueNode *
rvs_queue_alloc_item(RVS_Queue *q)
{
  mutex_take(q->mutex);
  RVS_QueueNode *node = 0;
  if ( ! q->is_closed) {
    node = rvs_queue_node_list_pop(&q->free_list);
    if (node) {
      MemoryZero(node, q->message_size);
    } else {
      node = arena_push(q->arena, q->message_size, q->message_align, 1);
    }
  }
  mutex_drop(q->mutex);
  return node;
}

internal void
rvs_queue_recycle(RVS_Queue *q, RVS_QueueNode *node)
{
  mutex_take(q->mutex);
  rvs_queue_node_list_push(&q->free_list, node);
  mutex_drop(q->mutex);
}

internal RVS_Result
rvs_queue_push(RVS_Queue *q, RVS_QueueNode *node)
{
  mutex_take(q->mutex);
  RVS_Result result = RVS_Result_EngineStopped;
  if ( ! q->is_closed) {
    rvs_queue_node_list_push(&q->messages, node);
    cond_var_broadcast(q->available_cv);
    result = RVS_Result_Ok;
  }
  mutex_drop(q->mutex);
  return result;
}

internal RVS_QueueNode *
rvs_queue_pop(RVS_Queue *q, U64 wait_us)
{
  U64 endt_us = max_U64;
  if (wait_us != max_U64) {
    U64 now_us = now_time_us();
    endt_us = now_us + Min(wait_us, max_U64 - now_us);
  }

  mutex_take(q->mutex);
  while (q->messages.count == 0 && !q->is_closed) {
    q->waiter_count += 1;
    B32 is_signaled = cond_var_wait(q->available_cv, q->mutex, endt_us);
    q->waiter_count -= 1;
    if ( ! is_signaled) {
      break;
    }
  }
  RVS_QueueNode *node = rvs_queue_node_list_pop(&q->messages);
  mutex_drop(q->mutex);

  return node;
}
