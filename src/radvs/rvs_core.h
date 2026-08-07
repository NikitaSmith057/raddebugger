// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

////////////////////////////////

typedef U64 RVS_MessageID;
typedef U64 RVS_Epoch;

////////////////////////////////

typedef enum
{
  RVS_Result_Null,
  RVS_Result_Ok,
  RVS_Result_Busy,
  RVS_Result_Timeout,
  RVS_Result_Error,
  RVS_Result_Pending,
  RVS_Result_AlreadyPending,
  RVS_Result_Cancelled,
  RVS_Result_AlreadyInited,
  RVS_Result_EngineStopped,
  RVS_Result_StaleState,
  RVS_Result_Unsupported,
  RVS_Result_InvalidArgument,
  RVS_Result_InvalidTtraps,
} RVS_Result;

#define RVS_COMMAND_XLIST                                       \
  X(Launch,       "Launch program and stop at the entry point") \
  X(Run,          "Run selected programs")                      \
  X(Pause,        "Pauses running programs")                    \
  X(Step,         "Execute a stepping command")                 \
  X(SelectThread, "Select a thread")                            \
  X(Exit,         "Shutdown debug engine")

typedef enum
{
  RVS_CommandKind_Null,
#define X(kind, ...) RVS_CommandKind_##kind,
  RVS_COMMAND_XLIST
#undef X
} RVS_CommandKind;


////////////////////////////////
// Core Identifiers

typedef struct { union { U64 value; U64 u64[1]; U32 u32[2];         }; } RVS_ProgramID;
typedef struct { union { DMN_Handle handle; U64 u64[2]; U32 u32[4]; }; } RVS_ProcessID;
typedef struct { union { DMN_Handle handle; U64 u64[2]; U32 u32[4]; }; } RVS_ThreadID;
typedef struct { union { DMN_Handle handle; U64 u64[2]; U32 u32[4]; }; } RVS_ModuleID;

////////////////////////////////

typedef enum
{
  RVS_WorkerState_Null,
  RVS_WorkerState_Initing,
  RVS_WorkerState_Live,
  RVS_WorkerState_Stopped,
  RVS_WorkerState_Terminating,
  RVS_WorkerState_Exited
} RVS_WorkerState;

////////////////////////////////

typedef enum
{
  RVS_StepKind_Into,
  RVS_StepKind_Over,
  RVS_StepKind_Out,
} RVS_StepKind;

typedef enum
{
  RVS_StepUnit_Instruction,
  RVS_StepUnit_Line,
  RVS_StepUnit_Statement,
} RVS_StepUnit;

typedef enum
{
  RVS_RunIntentKind_Continue,
  RVS_RunIntentKind_Execute,
  RVS_RunIntentKind_Step,
} RVS_RunIntentKind;

typedef enum
{
  RVS_RunMode_Normal,
  RVS_RunMode_ToAddress,
} RVS_RunMode;

typedef enum
{
  RVS_RunIntentState_None,
  RVS_RunIntentState_Active,
  RVS_RunIntentState_Suspended,
  RVS_RunIntentState_Completed,
  RVS_RunIntentState_Cancelled,
} RVS_RunIntentState;

typedef struct
{
  RVS_RunIntentKind  kind;
  RVS_StepKind       step_kind;
  RVS_StepUnit       step_unit;
  RVS_ThreadID       thread;
  RVS_RunIntentState state;
} RVS_RunIntent;

typedef struct
{
  U64             programs_count;
  RVS_ProgramID  *programs;
  RVS_RunIntent   intent;
  RVS_RunMode     mode;
  U64             address;
} RVS_RunInfo;

typedef enum
{
  RVS_StopCause_None,
  RVS_StopCause_Incidental,
  RVS_StopCause_RunCompletion,
  RVS_StopCause_UserPause,
  RVS_StopCause_Breakpoint,
  RVS_StopCause_Exception,
  RVS_StopCause_ProcessExit,
} RVS_StopCause;

typedef struct {
  RVS_CommandKind kind;
  union {
    ProcessLaunchParams launch_params;
    RVS_RunInfo         run;
    RVS_RunInfo         pause;
    RVS_ThreadID        select_thread;
    struct {
      RVS_ThreadID thread_id;
      RVS_StepKind kind;
      RVS_StepUnit unit;
    } step;
  };
} RVS_Command;

typedef enum
{
  RVS_CommandReplyKind_Null,
  RVS_CommandReplyKind_LaunchAck,
} RVS_CommandReplyKind;

typedef struct
{
  RVS_MessageID        request_id;
  RVS_Result           result;
  U64                  program_state_epoch;
  RVS_CommandReplyKind kind;
  union {
    struct {
      RVS_ProgramID program_id;
      U32           pid;
    } launch_ack;
  };
} RVS_CommandReply;

////////////////////////////////

#define rvs_process_id_from_handle(dmn) (((RVS_ProcessID){ .handle = dmn } })
#define rvs_thread_id_from_handle(dmn)  (((RVS_ThreadID ){ .handle = dmn } })
#define rvs_module_id_from_handle(dmn)  ((RVS_ModuleID  ){ .handle = dmn } })

#define rvs_handle_from_process_id(id) ((id).handle)
#define rvs_handle_from_thread_id(id)  ((id).handle)
#define rvs_handle_from_module_id(id)  ((id).handle)

internal void rvs_run_copy(Arena *arena, RVS_RunInfo *dst, RVS_RunInfo *src);

internal RVS_CommandKind rvs_command_kind_from_string(String8 v);
internal String8         rvs_string_from_command_kind(RVS_CommandKind v);
internal String8         rvs_help_from_command_kind(RVS_CommandKind v);

