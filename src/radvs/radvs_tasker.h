// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/radvs_request.h"

typedef struct RADVS_Task RADVS_Task;
typedef RADVS_Result RADVS_TaskProc(void *user_data);

typedef struct
{
  B32          completed;
  RADVS_Result result;
} RADVS_TaskStatus;

// Tasks are caller-owned and must remain valid until the Tasker has stopped.
struct RADVS_Task
{
  RADVS_Request  request;
  RADVS_Task    *next;
  RADVS_TaskProc *proc;
  void           *user_data;
  U64             token;
  U64             endt_us;
  void           *tasker;
  B32             executing;
};

// A task proc returning Busy has deferred completion to an external producer.
RADVS_Result radvs_tasker_init(void);
void         radvs_tasker_release(void);

void         radvs_task_init(RADVS_Task *task, RADVS_TaskProc *proc, void *user_data, U64 endt_us);
void         radvs_task_release(RADVS_Task *task);
RADVS_Result radvs_tasker_submit(RADVS_Task *task);
void         radvs_tasker_cancel(RADVS_Task *task);
RADVS_Result radvs_tasker_wait(RADVS_Task *task);
RADVS_Result radvs_tasker_complete(RADVS_Task *task, RADVS_Result result);
RADVS_TaskStatus radvs_tasker_status(RADVS_Task *task);
U64          radvs_tasker_token(RADVS_Task *task);
