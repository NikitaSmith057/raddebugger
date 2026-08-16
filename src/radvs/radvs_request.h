// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/radvs_format.h"

#ifndef RADVS_REQUEST_DIAGNOSTICS
# define RADVS_REQUEST_DIAGNOSTICS 0
#endif

typedef struct RADVS_Request RADVS_Request;
struct RADVS_Request
{
  RADVS_Request *next;
  CondVar        complete_cv;
  B32            completed;
  RADVS_Result   result;
#if RADVS_REQUEST_DIAGNOSTICS
  U64            diagnostic_id;
#endif
};

typedef struct
{
  Arena         *arena;
  Mutex          mutex;
  CondVar        available_cv;
  RADVS_Request *first;
  RADVS_Request *last;
  RADVS_Request *free;
#if RADVS_REQUEST_DIAGNOSTICS
  String8        diagnostic_name;
  U64            next_diagnostic_id;
#endif
} RADVS_RequestQueue;

internal void           radvs_request_queue_init          (RADVS_RequestQueue *queue, Arena *arena);
internal void           radvs_request_queue_release       (RADVS_RequestQueue *queue);
#if RADVS_REQUEST_DIAGNOSTICS
internal void           radvs_request_queue_set_diagnostic_name(RADVS_RequestQueue *queue, String8 name);
#endif
internal void           radvs_request_init                (RADVS_Request *request);
internal void           radvs_request_release             (RADVS_Request *request);
internal RADVS_Request *radvs_request_queue_alloc_locked  (RADVS_RequestQueue *queue, U64 request_size, U64 request_align);
internal void           radvs_request_queue_recycle_locked(RADVS_RequestQueue *queue, RADVS_Request *request);
internal void           radvs_request_queue_recycle       (RADVS_RequestQueue *queue, RADVS_Request *request);
internal RADVS_Result   radvs_request_queue_enqueue_locked(RADVS_RequestQueue *queue, RADVS_Request *request);
internal RADVS_Result   radvs_request_queue_wait_locked   (RADVS_RequestQueue *queue, RADVS_Request *request, U64 endt_us);
internal RADVS_Result   radvs_request_queue_submit_locked (RADVS_RequestQueue *queue, RADVS_Request *request);
internal RADVS_Request *radvs_request_queue_pop_locked    (RADVS_RequestQueue *queue);
internal B32            radvs_request_queue_complete_locked(RADVS_RequestQueue *queue, RADVS_Request *request, RADVS_Result result);
internal B32            radvs_request_queue_complete      (RADVS_RequestQueue *queue, RADVS_Request *request, RADVS_Result result);
#define radvs_request_queue_alloc_locked_struct(q, t) (t*)radvs_request_queue_alloc_locked(q, sizeof(t), AlignOf(t))
#define radvs_request_queue_pop_locked_struct(q, t)   (t*)radvs_request_queue_pop_locked(q)

