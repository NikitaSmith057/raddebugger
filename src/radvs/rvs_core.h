// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
// Includes

#pragma once
#include "base/base_core.h"
#include "base/base_strings.h"
#include "base/base_arena.h"
#include "base/base_processes.h"
#include "demon/demon.h"

////////////////////////////////

typedef enum
{
  RVS_Result_Null,
  RVS_Result_Ok,
  RVS_Result_CallAgain,

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

////////////////////////////////
// Commands

#define RVS_COMMAND_XLIST                                       \
  X(Launch,       "Launch program and stop at the entry point") \
  X(Run,          "Run selected programs")                      \
  X(Pause,        "Pauses running programs")                    \
  X(Stop,         "Stops processes")                            \
  X(Step,         "Execute a stepping command")                 \
  X(Exit,         "Shutdown debug engine")

typedef enum
{
  RVS_CommandKind_Null,
#define X(kind, ...) RVS_CommandKind_##kind,
  RVS_COMMAND_XLIST
#undef X
  RVS_CommandKind_UserLo,
} RVS_CommandKind;

////////////////////////////////
// Core Identifiers

typedef struct { union { U64 value; U64 u64[1]; U32 u32[2];         }; } RVS_ProgramID;
typedef struct { union { DMN_Handle handle; U64 u64[2]; U32 u32[4]; }; } RVS_ProcessID;
typedef struct { union { DMN_Handle handle; U64 u64[2]; U32 u32[4]; }; } RVS_ThreadID;
typedef struct { union { DMN_Handle handle; U64 u64[2]; U32 u32[4]; }; } RVS_ModuleID;

typedef U64           RVS_MessageID;
typedef RVS_MessageID RVS_BackendMessageID;
typedef RVS_MessageID RVS_EngineMessageID;

typedef U64 RVS_EffectID;
typedef U64 RVS_Epoch;
typedef U64 RVS_Addr;

////////////////////////////////
// Thread Wroker Types

typedef enum
{
  RVS_WorkerState_Null,
  RVS_WorkerState_Initing,
  RVS_WorkerState_Live,
  RVS_WorkerState_Stopped,
  RVS_WorkerState_Exiting,
  RVS_WorkerState_Exited
} RVS_WorkerState;

////////////////////////////////
// Launch Info

typedef struct
{
  ProcessLaunchParams params;
} RVS_LaunchInfo;

////////////////////////////////
// Run Info

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

typedef enum
{
  RVS_RunTargetKind_All,
  RVS_RunTargetKind_Programs,
} RVS_RunTargetKind;

typedef struct
{
  RVS_RunIntent     intent;
  RVS_RunTargetKind target_kind;
  RVS_MessageID     execution_request_id;
  union {
    struct {
      RVS_ProgramID *v;
      U64            count;
    } programs;
  };
  DMN_Handle        *processes;
  U64                processes_count;
  DMN_TrapChunkList  traps;
} RVS_RunInfo;

////////////////////////////////
// Stop State

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

typedef enum
{
  RVS_StopSource_Interrupt,
  RVS_StopSource_Exception,
  RVS_StopSource_ProcessExit,
  RVS_StopSource_ThreadExit,
  RVS_StopSource_BackendFailed,
} RVS_StopSource;

typedef struct
{
  RVS_ProgramID program;
  RVS_ProcessID process;
  RVS_ThreadID  thread;
  RVS_Epoch     epoch;
  RVS_StopCause stop_cause;
} RVS_StopState;

////////////////////////////////
// Debugger Command

typedef struct {
  RVS_CommandKind kind;
  union {
    RVS_LaunchInfo launch;
    RVS_RunInfo    run;
    RVS_RunInfo    pause;
    struct {
      DMN_Handle *process_handles;
      U64         process_count;
    } stop;
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
// Debug Event

enum
{
  RVS_EventKind_First = DMN_EventKind_UserLo,
  RVS_EventKind_Error,
  RVS_EventKind_Stopped,
  RVS_EventKind_ProgramDestroyed,
};

typedef struct
{
  RVS_ProgramID program;
  RVS_ProcessID process;
  RVS_ThreadID  thread;
  DMN_Event     raw_event;
  union {
    struct { U32 exit_code;      } process_exited;
    struct { DMN_ErrorKind kind; } error;
  };
} RVS_Event;

typedef struct RVS_EventNode RVS_EventNode;
struct RVS_EventNode
{
  RVS_Event      v;
  RVS_EventNode *next;
};

typedef struct
{
  U64            count;
  RVS_EventNode *first;
  RVS_EventNode *last;
} RVS_EventList;

////////////////////////////////

#define rvs_process_id_from_handle(dmn) ((RVS_ProcessID){ .handle = (dmn) })
#define rvs_thread_id_from_handle(dmn)  ((RVS_ThreadID ){ .handle = (dmn) })
#define rvs_module_id_from_handle(dmn)  ((RVS_ModuleID ){ .handle = (dmn) })

#define rvs_handle_from_process_id(id) ((id).handle)
#define rvs_handle_from_thread_id(id)  ((id).handle)
#define rvs_handle_from_module_id(id)  ((id).handle)

////////////////////////////////

internal RVS_EventNode * rvs_event_list_push(Arena *arena, RVS_EventList *list, RVS_Event v);

internal void rvs_run_copy(Arena *arena, RVS_RunInfo *dst, RVS_RunInfo *src);

internal RVS_CommandKind rvs_command_kind_from_string(String8 v);
internal String8         rvs_string_from_command_kind(RVS_CommandKind v);
internal String8         rvs_help_from_command_kind(RVS_CommandKind v);

