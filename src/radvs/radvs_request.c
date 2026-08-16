// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#include "radvs/radvs_request.h"

internal void
radvs_request_queue_init(RADVS_RequestQueue *queue, Arena *arena)
{
  queue->arena        = arena;
  queue->mutex        = mutex_alloc();
  queue->available_cv = cond_var_alloc();
#if RADVS_REQUEST_DIAGNOSTICS
  queue->diagnostic_name    = str8_zero();
  queue->next_diagnostic_id = 0;
#endif
}

internal void
radvs_request_queue_release(RADVS_RequestQueue *queue)
{
  cond_var_release(queue->available_cv);
  mutex_release(queue->mutex);
}

#if RADVS_REQUEST_DIAGNOSTICS
internal void
radvs_request_queue_set_diagnostic_name(RADVS_RequestQueue *queue, String8 name)
{
  queue->diagnostic_name = name;
}
#endif

internal void
radvs_request_init(RADVS_Request *request)
{
  request->complete_cv = cond_var_alloc();
}

internal void
radvs_request_release(RADVS_Request *request)
{
  cond_var_release(request->complete_cv);
  MemoryZeroStruct(request);
}

internal RADVS_Request *
radvs_request_queue_alloc_locked(RADVS_RequestQueue *queue, U64 request_size, U64 request_align)
{
  RADVS_Request *request = queue->free;
  if (request != 0) {
    SLLStackPop(queue->free);
    CondVar complete_cv = request->complete_cv;
    MemoryZero(request, request_size);
    request->complete_cv = complete_cv;
  } else {
    request = arena_push(queue->arena, request_size, Max(8, request_align), 1);
    radvs_request_init(request);
  }
#if RADVS_REQUEST_DIAGNOSTICS
  request->diagnostic_id = ++queue->next_diagnostic_id;
#endif
  return request;
}

internal void
radvs_request_queue_recycle_locked(RADVS_RequestQueue *queue, RADVS_Request *request)
{
  SLLStackPush(queue->free, request);
}

internal void
radvs_request_queue_recycle(RADVS_RequestQueue *queue, RADVS_Request *request)
{
  MutexScope (queue->mutex) {
    radvs_request_queue_recycle_locked(queue, request);
  }
}

internal RADVS_Result
radvs_request_queue_enqueue_locked(RADVS_RequestQueue *queue, RADVS_Request *request)
{
  if (request->completed) {
    return request->result;
  }
  SLLQueuePush(queue->first, queue->last, request);
#if RADVS_REQUEST_DIAGNOSTICS
  log_infof("radvs request queue:%S id:%I64u sent\n", queue->diagnostic_name, request->diagnostic_id);
#endif
  cond_var_broadcast(queue->available_cv);
  return RADVS_Result_Ok;
}

internal RADVS_Result
radvs_request_queue_wait_locked(RADVS_RequestQueue *queue, RADVS_Request *request, U64 endt_us)
{
  for (; !request->completed;) {
    if (endt_us != max_U64 && now_time_us() >= endt_us) {
      radvs_request_queue_complete_locked(queue, request, RADVS_Result_Timeout);
      break;
    }
    cond_var_wait(request->complete_cv, queue->mutex, endt_us);
  }
  return request->result;
}

internal RADVS_Result
radvs_request_queue_submit_locked(RADVS_RequestQueue *queue, RADVS_Request *request)
{
  RADVS_Result result = radvs_request_queue_enqueue_locked(queue, request);
  if (result == RADVS_Result_Ok) {
    result = radvs_request_queue_wait_locked(queue, request, max_U64);
  }
  return result;
}

internal RADVS_Request *
radvs_request_queue_pop_locked(RADVS_RequestQueue *queue)
{
  RADVS_Request *request = queue->first;
  SLLQueuePop(queue->first, queue->last);
#if RADVS_REQUEST_DIAGNOSTICS
  if (request != 0) {
    log_infof("radvs request queue:%S id:%I64u received\n", queue->diagnostic_name, request->diagnostic_id);
  }
#endif
  return request;
}

internal B32
radvs_request_queue_complete_locked(RADVS_RequestQueue *queue, RADVS_Request *request, RADVS_Result result)
{
  if (request->completed) {
    return 0;
  }
  request->result    = result;
  request->completed = 1;
#if RADVS_REQUEST_DIAGNOSTICS
  log_infof("radvs request queue:%S id:%I64u completed result:%u\n", queue->diagnostic_name, request->diagnostic_id, result);
#endif
  cond_var_broadcast(request->complete_cv);
  return 1;
}

internal B32
radvs_request_queue_complete(RADVS_RequestQueue *queue, RADVS_Request *request, RADVS_Result result)
{
  B32 completed = 0;
  MutexScope (queue->mutex) {
    completed = radvs_request_queue_complete_locked(queue, request, result);
  }
  return completed;
}
