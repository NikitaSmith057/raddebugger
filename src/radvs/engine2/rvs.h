// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

typedef enum
{
  RVS_Result_Ok,
  RVS_Result_Timeout,
} RVS_Result;

typedef enum
{
  RVS_WorkerState_Initing,
  RVS_WorkerState_Running,
  RVS_WorkerState_Stopped,
  RVS_WorkerState_Terminating,
  RVS_WorkerState_Exited
} RVS_WorkerState;
