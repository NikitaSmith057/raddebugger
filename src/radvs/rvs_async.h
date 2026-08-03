// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/rvs.h"

typedef struct RVS_QueueNode RVS_QueueNode;
struct RVS_QueueNode
{
  RVS_QueueNode *next;
};

typedef struct
{
  U64            count;
  RVS_QueueNode *first;
  RVS_QueueNode *last;
} RVS_QueueNodeList;

typedef struct
{
  Arena             *arena;
  Mutex              mutex;
  CondVar            available_cv;
  B32                is_closed;
  U32                waiter_count;
  RVS_QueueNodeList  messages;
  RVS_QueueNodeList  free_list;
  U64                message_size;
  U64                message_align;
} RVS_Queue;

internal RVS_Queue *rvs_queue_alloc(Arena *arena, U64 message_size, U64 message_align);
internal void       rvs_queue_close(RVS_Queue *q);
internal void       rvs_queue_release(RVS_Queue *q);

internal RVS_QueueNode *rvs_queue_alloc_item(RVS_Queue *q);
internal void           rvs_queue_recycle(RVS_Queue *q, RVS_QueueNode *node);
internal RVS_Result     rvs_queue_push(RVS_Queue *q, RVS_QueueNode *node);
internal RVS_QueueNode *rvs_queue_pop(RVS_Queue *q, U64 wait_us);

#define rvs_queue_alloc_struct(q, T) ((T *)rvs_queue_alloc_item(q))
#define rvs_queue_pop_struct(q, T, wait_us) ((T *)rvs_queue_pop(q, wait_us))
