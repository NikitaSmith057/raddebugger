// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

typedef enum
{
  RVS_Result_Null,
  RVS_Result_Ok,
  RVS_Result_Timeout,
  RVS_Result_Error,
} RVS_Result;

typedef enum
{
  RVS_ThreadState_Null,
  RVS_ThreadState_Initing,
  RVS_ThreadState_Running,
  RVS_ThreadState_Stopped,
  RVS_ThreadState_Terminating,
  RVS_ThreadState_Exited
} RVS_ThreadState;


