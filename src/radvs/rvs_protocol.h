// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/rvs.h"

typedef U64 RVS_ReplyID;

typedef struct RVS_Queue RVS_Queue;

typedef enum
{
  RVS_QueueMessageStatus_Null,
  RVS_QueueMessageStatus_Pending,
  RVS_QueueMessageStatus_Complete
} RVS_QueueMessageStatus;

typedef struct RVS_QueueMessage RVS_QueueMessage;
struct RVS_QueueMessage
{
  RVS_QueueMessage       *next;
  RVS_Queue              *queue;
  Mutex                   complete_mutex;
  CondVar                 complete_cv;
  RVS_QueueMessageStatus  status;
  RVS_Result              result;
  RVS_ReplyID             reply_id;
  U64                     user_data[2];
};

typedef struct
{
  U64               count;
  RVS_QueueMessage *first;
  RVS_QueueMessage *last;
} RVS_QueueMessageList;

struct RVS_Queue
{
  Arena                *arena;
  Mutex                 mutex;
  CondVar               available_cv;
  RVS_QueueMessageList  messages;
  RVS_QueueMessageList  free_list;
  U64 next_reply_id;
  U64 message_size;
  U64 message_align;
};

typedef void (RVS_ReplyCallback)(RVS_ReplyID reply_id, String8 reply_data, void *ud);

// request 
internal void rvs_queue_alloc  (RVS_Queue *q, Arena *a, U64 message_size, U64 message_align);
internal void rvs_queue_release(RVS_Queue *q);

// queue 
internal RVS_QueueMessage *rvs_queue_alloc_message(RVS_Queue *q);
internal void rvs_queue_recycle (RVS_Queue *q, RVS_QueueMessage *r);
internal B32  rvs_queue_push    (RVS_Queue *q, RVS_QueueMessage *r);
internal B32  rvs_queue_wait_for(RVS_Queue *q, RVS_QueueMessage *r, U64 wait_us);
internal B32  rvs_queue_send_message(RVS_Queue *q, RVS_QueueMessage *r, U64 wait_us);
internal RVS_QueueMessage *rvs_queue_pop (RVS_Queue *q, U64 wait_us);
#define rvs_queue_alloc_struct(q, T) (T*)rvs_queue_alloc(q, sizeof(T), AlignOf(T))
#define rvs_queue_pop_struct(q, T)   (T*)rvs_queue_pop(q)

