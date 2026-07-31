// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#include "radvs/radvs_tasker.h"

typedef struct RADVS_Tasker RADVS_Tasker;

// TODO(clanker): 
struct RADVS_Tasker
{
  Arena             *arena;
  RADVS_RequestQueue requests;
  RADVS_Task        *all_tasks;
  U64                next_token;
  B32                shutdown_requested;
  Thread             worker;
};

typedef enum
{
  RADVS_TaskerState_Uninitialized,
  RADVS_TaskerState_Initializing,
  RADVS_TaskerState_Ready,
  RADVS_TaskerState_Failed,
} RADVS_TaskerState;

static U32          radvs_tasker_state = RADVS_TaskerState_Uninitialized;
static RADVS_Tasker *radvs_tasker = 0;
static U32          radvs_tasker_lifecycle_gate = 0;

internal void
radvs_tasker_lifecycle_gate_take(void)
{
  for (; ins_atomic_u32_eval_cond_assign(&radvs_tasker_lifecycle_gate, 1, 0) != 0;) {
    Sleep(0);
  }
}

internal void
radvs_tasker_lifecycle_gate_drop(void)
{
  ins_atomic_u32_eval_assign(&radvs_tasker_lifecycle_gate, 0);
}

internal RADVS_Tasker *
radvs_tasker_from_global(void)
{
  return ins_atomic_u32_eval(&radvs_tasker_state) == RADVS_TaskerState_Ready ? radvs_tasker : 0;
}

internal void
radvs_tasker_worker(void *user_data)
{
  RADVS_Tasker *tasker = user_data;
  for (;;) {
    RADVS_Task *task = 0;
    B32 should_shutdown = 0;
    MutexScope (tasker->requests.mutex) {
      for (; !tasker->requests.first && !tasker->shutdown_requested;) {
        cond_var_wait(tasker->requests.available_cv, tasker->requests.mutex, max_U64);
      }
      if (!tasker->shutdown_requested && tasker->requests.first) {
        task = (RADVS_Task *)radvs_request_queue_pop_locked(&tasker->requests);
        if (!task->request.completed) {
          task->executing = 1;
        } else {
          task = 0;
        }
      }
      should_shutdown = tasker->shutdown_requested;
    }

    if (should_shutdown) {
      return;
    }
    if (task != 0) {
      RADVS_Result result = task->proc(task->user_data);
      MutexScope (tasker->requests.mutex) {
        task->executing = 0;
        if (result != RADVS_Result_Busy) {
          radvs_request_queue_complete_locked(&tasker->requests, &task->request, result);
        }
      }
    }
  }
}

RADVS_Result
radvs_tasker_init(void)
{
  radvs_tasker_lifecycle_gate_take();
  RADVS_Tasker *tasker = radvs_tasker_from_global();
  if (tasker == 0) {
    ins_atomic_u32_eval_assign(&radvs_tasker_state, RADVS_TaskerState_Initializing);
    Arena *arena = arena_alloc(.reserve_size = MB(1), .commit_size = KB(64));
    tasker = push_array(arena, RADVS_Tasker, 1);
    tasker->arena = arena;
    radvs_request_queue_init(&tasker->requests, arena);
#if RADVS_REQUEST_DIAGNOSTICS
    radvs_request_queue_set_diagnostic_name(&tasker->requests, str8_lit("tasker"));
#endif
    tasker->worker = thread_launch(radvs_tasker_worker, tasker);
    if (tasker->worker.u64[0] != 0) {
      radvs_tasker = tasker;
      ins_atomic_u32_eval_assign(&radvs_tasker_state, RADVS_TaskerState_Ready);
    } else {
      radvs_request_queue_release(&tasker->requests);
      arena_release(arena);
      ins_atomic_u32_eval_assign(&radvs_tasker_state, RADVS_TaskerState_Failed);
      radvs_tasker_lifecycle_gate_drop();
      return RADVS_Result_OutOfMemory;
    }
  }
  radvs_tasker_lifecycle_gate_drop();
  return RADVS_Result_Ok;
}

void
radvs_tasker_release(void)
{
  radvs_tasker_lifecycle_gate_take();
  RADVS_Tasker *tasker = radvs_tasker_from_global();
  if (tasker != 0) {
    MutexScope (tasker->requests.mutex) {
      tasker->shutdown_requested = 1;
      for EachNode(task, RADVS_Task, tasker->all_tasks) {
        radvs_request_queue_complete_locked(&tasker->requests, &task->request, RADVS_Result_Busy);
      }
      cond_var_broadcast(tasker->requests.available_cv);
    }
    thread_join(tasker->worker, max_U64);
    radvs_request_queue_release(&tasker->requests);
    arena_release(tasker->arena);
    radvs_tasker = 0;
    ins_atomic_u32_eval_assign(&radvs_tasker_state, RADVS_TaskerState_Uninitialized);
  }
  radvs_tasker_lifecycle_gate_drop();
}

void
radvs_task_init(RADVS_Task *task, RADVS_TaskProc *proc, void *user_data, U64 endt_us)
{
  MemoryZeroStruct(task);
  radvs_request_init(&task->request);
  task->proc = proc;
  task->user_data = user_data;
  task->endt_us = endt_us;
}

void
radvs_task_release(RADVS_Task *task)
{
  if (task != 0) {
    radvs_request_release(&task->request);
    MemoryZeroStruct(task);
  }
}

RADVS_Result
radvs_tasker_submit(RADVS_Task *task)
{
  RADVS_Tasker *tasker = radvs_tasker_from_global();
  if (tasker == 0 || task == 0 || task->proc == 0 || task->request.complete_cv.u64[0] == 0) {
    return RADVS_Result_InvalidArgument;
  }

  RADVS_Result result = RADVS_Result_Busy;
  MutexScope (tasker->requests.mutex) {
    if (!tasker->shutdown_requested && task->tasker == 0) {
      task->tasker = tasker;
      task->token = ++tasker->next_token;
      task->next = tasker->all_tasks;
      tasker->all_tasks = task;
      result = radvs_request_queue_enqueue_locked(&tasker->requests, &task->request);
    }
  }
  return result;
}

void
radvs_tasker_cancel(RADVS_Task *task)
{
  if (task != 0 && task->tasker != 0) {
    RADVS_Tasker *tasker = task->tasker;
    radvs_request_queue_complete(&tasker->requests, &task->request, RADVS_Result_Busy);
  }
}

RADVS_Result
radvs_tasker_wait(RADVS_Task *task)
{
  if (task == 0 || task->tasker == 0) {
    return RADVS_Result_InvalidArgument;
  }
  RADVS_Tasker *tasker = task->tasker;
  RADVS_Result result = RADVS_Result_InvalidArgument;
  MutexScope (tasker->requests.mutex) {
    result = radvs_request_queue_wait_locked(&tasker->requests, &task->request,
                                             task->endt_us ? task->endt_us : max_U64);
  }
  return result;
}

RADVS_Result
radvs_tasker_complete(RADVS_Task *task, RADVS_Result result)
{
  if (task == 0 || task->tasker == 0) {
    return RADVS_Result_InvalidArgument;
  }
  RADVS_Tasker *tasker = task->tasker;
  MutexScope (tasker->requests.mutex) {
    if (!radvs_request_queue_complete_locked(&tasker->requests, &task->request, result)) {
      result = task->request.result;
    }
  }
  return result;
}

RADVS_TaskStatus
radvs_tasker_status(RADVS_Task *task)
{
  RADVS_TaskStatus status = {0};
  if (task == 0 || task->tasker == 0) {
    status.result = RADVS_Result_InvalidArgument;
    return status;
  }
  RADVS_Tasker *tasker = task->tasker;
  MutexScope (tasker->requests.mutex) {
    status.completed = task->request.completed;
    status.result = task->request.completed ? task->request.result : RADVS_Result_Busy;
  }
  return status;
}

U64
radvs_tasker_token(RADVS_Task *task)
{
  return task != 0 ? task->token : 0;
}
