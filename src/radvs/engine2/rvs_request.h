// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/engine2/rvs.h"

typedef enum
{
  RVS_RequestStatus_Null,
  RVS_RequestStatus_Pending,
  RVS_RequestStatus_Complete
} RVS_RequestStatus;

typedef struct RVS_Request RVS_Request;
struct RVS_Request
{
  RVS_Request       *next;
  CondVar            complete_cv;
  RVS_RequestStatus  status;
  RVS_Result         result;
};

typedef struct
{
  U64          count;
  RVS_Request *first;
  RVS_Request *last;
} RVS_RequestList;

typedef struct
{
  Arena           *arena;
  Mutex            mutex;
  CondVar          available_cv;
  RVS_RequestList  requests;
  RVS_RequestList  free_list;
} RVS_RequestQueue;

internal void rvs_request_queue_init(RVS_RequestQueue *q, Arena *a);
internal void rvs_request_queue_release(RVS_RequestQueue *q);

internal void rvs_request_queue_alloc   (RVS_RequestQueue *q, U64 message_size, U64 message_align);
internal void rvs_request_queue_recycle (RVS_RequestQueue *q, RVS_Request *r);
internal B32  rvs_request_queue_push    (RVS_RequestQueue *q, RVS_Request *r);
internal B32  rvs_request_queue_wait_for(RVS_RequestQueue *q, RVS_Request *r, U64 wait_us);
internal B32  rvs_request_queue_submit  (RVS_RequestQueue *q, RVS_Request *r, U64 wait_us);
internal RVS_Request *rvs_request_pop (RVS_RequestQueue *q);
#define rvs_request_queue_alloc_struct(q, T) (T*)rvs_request_queue_alloc(q, sizeof(T), AlignOf(T))
#define rvs_request_queue_pop_struct(q, T)   (T*)rvs_request_queue_pop(q)

