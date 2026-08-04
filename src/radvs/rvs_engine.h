// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

////////////////////////////////

#include "demon/demon_core.h"
#include "radvs/rvs.h"

////////////////////////////////

typedef struct RVS_Engine         RVS_Engine;
typedef struct RVS_Session        RVS_Session;
typedef struct RVS_Request        RVS_Request;
typedef struct RVS_RequestControl RVS_RequestControl;

typedef struct { union { U64 value; U64 u64[1]; U32 u32[2];         }; } RVS_ProgramID;
typedef struct { union { DMN_Handle handle; U64 u64[2]; U32 u32[4]; }; } RVS_ProcessID;
typedef struct { union { DMN_Handle handle; U64 u64[2]; U32 u32[4]; }; } RVS_ThreadID;

internal RVS_ProgramID rvs_program_id_zero   (void)                             { return (RVS_ProgramID){0}; }
internal B32           rvs_program_id_is_zero(RVS_ProgramID id)                 { return id.value == 0;      }
internal B32           rvs_program_id_match  (RVS_ProgramID a, RVS_ProgramID b) { return a.value == b.value; }

internal RVS_ProcessID rvs_process_id_zero       (void)                             { return (RVS_ProcessID){0};                             }
internal RVS_ProcessID rvs_process_id_from_handle(DMN_Handle handle)                { return (RVS_ProcessID){ .handle = handle };            }
internal B32           rvs_process_id_is_zero    (RVS_ProcessID id)                 { return dmn_handle_match(id.handle, dmn_handle_zero()); }
internal B32           rvs_process_id_match      (RVS_ProcessID a, RVS_ProcessID b) { return dmn_handle_match(a.handle, b.handle);           }
internal DMN_Handle    rvs_process_id_handle     (RVS_ProcessID id)                 { return id.handle;                                      }

internal RVS_ThreadID  rvs_thread_id_zero        (void)                           { return (RVS_ThreadID){0};                              }
internal RVS_ThreadID  rvs_thread_id_from_handle (DMN_Handle handle)              { return (RVS_ThreadID){ .handle = handle };             }
internal B32           rvs_thread_id_is_zero     (RVS_ThreadID id)                { return dmn_handle_match(id.handle, dmn_handle_zero()); }
internal B32           rvs_thread_id_match       (RVS_ThreadID a, RVS_ThreadID b) { return dmn_handle_match(a.handle, b.handle);           }
internal DMN_Handle    rvs_thread_id_handle      (RVS_ThreadID id)                { return id.handle;                                      }

////////////////////////////////

// Entity snapshots are pointer-free values. They are the only entity data
// returned to callers outside the engine's locked model reduction.
typedef struct
{
  RVS_ProgramID id;
  U32           pid;
  U32           exit_code;
  B32           is_retired;
} RVS_ProgramSnapshot;

typedef struct
{
  RVS_ProgramID program;
  RVS_ProcessID process;
  RVS_ProcessID parent_process;
  U32           pid;
  U32           exit_code;
  B32           is_retired;
} RVS_ProcessSnapshot;

typedef struct
{
  RVS_ProgramID program;
  RVS_ProcessID process;
  RVS_ThreadID  thread;
  U32           tid;
  B32           is_retired;
} RVS_ThreadSnapshot;

//////////////////////////////

#define RVS_ENGINE_COMMAND_XLIST \
  X(Launch,    "Launch program and stop at the entry point") \
  X(Run,       "Run selected programs") \
  X(Interrupt, "Interrupt selected running programs")

typedef enum
{
  RVS_EngineCommandKind_Null,
#define X(kind, ...) RVS_EngineCommandKind_##kind,
  RVS_ENGINE_COMMAND_XLIST
#undef X
} RVS_EngineCommandKind;

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
  RVS_RunIntentKind_Continue, // preserve current run intent
  RVS_RunIntentKind_Execute,  // replace the run intent
  RVS_RunIntentKind_Step,     // TODO: step preserves/replaces; is this needed?
} RVS_RunIntentKind;

typedef enum
{
  RVS_RunMode_Normal,
  RVS_RunMode_ToAddress,
} RVS_RunMode;

////////////////////////////////

typedef enum
{
  RVS_EngineReplyKind_Null,
  RVS_EngineReplyKind_Launch,
  RVS_EngineReplyKind_Run,
  RVS_EngineReplyKind_Interrupt,
} RVS_EngineReplyKind;

typedef struct
{
  RVS_MessageID       request_id;
  RVS_Result          result;
  U64                 program_state_epoch;
  RVS_EngineReplyKind kind;
  union {
    struct {
      RVS_ProgramID program_id;
      U32           pid;
    } launch;
  };
} RVS_EngineReply;

////////////////////////////////

typedef enum
{
  RVS_StopCause_None,
  RVS_StopCause_Incidental,
  RVS_StopCause_RunCompletion,
  RVS_StopCause_UserInterrupt,
  RVS_StopCause_Breakpoint,
  RVS_StopCause_Exception,
  RVS_StopCause_ProcessExit,
} RVS_StopCause;

typedef enum
{
  RVS_EventKind_ProcessCreated,
  RVS_EventKind_ProcessExited,
  RVS_EventKind_ThreadCreated,
  RVS_EventKind_ThreadExited,
  RVS_EventKind_Error,
  RVS_EventKind_Stopped,
  RVS_EventKind_ProgramDestroyed,
} RVS_EventKind;

typedef struct
{
  RVS_ProgramID primary_program;
  RVS_ProcessID primary_process;
  RVS_ThreadID  selected_thread;
  U32           pid;
  U32           tid;
  RVS_StopCause primary_cause;
  U64           stable_stop_generation;
} RVS_StoppedEvent;

typedef struct
{
  U64           sequence;
  RVS_EventKind kind;
  RVS_ProgramID program;
  RVS_ProcessID process;
  RVS_ThreadID  thread;
  U32           pid;
  U32           tid;
  union {
    struct { U32 exit_code;      } process_exited;
    struct { DMN_ErrorKind kind; } error;
    RVS_StoppedEvent stopped;
  };
} RVS_Event;

////////////////////////////////

typedef struct
{
  RVS_Request        *request;
  RVS_RequestControl *control;
} RVS_SubmitInfo;

////////////////////////////////
// Engine API

RVS_Result rvs_engine_init(RVS_Engine **engine_out);
RVS_Result rvs_engine_shutdown(RVS_Engine *engine);

// The singleton-backed DEMON implementation supports one active session per engine.
RVS_Result rvs_engine_create_session(RVS_Engine *engine, RVS_Session **session_out);
void       rvs_session_addref(RVS_Session *session);
void       rvs_session_release(RVS_Session *session);

RVS_Result rvs_session_launch         (RVS_Session *session, String8 cmdl, String8 wdir, RVS_SubmitInfo *submit_out);
RVS_Result rvs_session_run_many       (RVS_Session *session, RVS_ProgramID *programs, U64 programs_count, RVS_SubmitInfo *submit_out);
RVS_Result rvs_session_run            (RVS_Session *session, RVS_ProgramID program_id, RVS_SubmitInfo *submit_out);
RVS_Result rvs_session_run_to_address (RVS_Session *session, RVS_ProgramID program_id, U64 vaddr, RVS_SubmitInfo *submit_out);
RVS_Result rvs_session_interrupt_many (RVS_Session *session, RVS_ProgramID *programs, U64 programs_count, RVS_SubmitInfo *submit_out);
RVS_Result rvs_session_interrupt      (RVS_Session *session, RVS_ProgramID program_id, RVS_SubmitInfo *submit_out);
RVS_Result rvs_session_select_thread  (RVS_Session *session, RVS_ProgramID program_id, RVS_ThreadID thread_id);
RVS_Result rvs_session_selected_thread(RVS_Session *session, RVS_ProgramID *program_id_out, RVS_ThreadID *thread_id_out);
RVS_Result rvs_session_continue       (RVS_Session *session, RVS_ProgramID program_id, RVS_SubmitInfo *submit_out);
RVS_Result rvs_session_step           (RVS_Session *session, RVS_StepKind kind, RVS_StepUnit unit, RVS_ThreadID thread_id, RVS_SubmitInfo *submit_out);
RVS_Result rvs_session_wait_for_event (Arena *arena, RVS_Session *session, U64 wait_us, RVS_Event *event_out);
RVS_Result rvs_session_ack_event      (RVS_Session *session, U64 sequence);
RVS_Result rvs_session_copy_programs  (Arena *arena, RVS_Session *session, RVS_ProgramSnapshot **snapshots_out, U64 *snapshots_count_out);
RVS_Result rvs_session_fetch_program  (RVS_Session *session, RVS_ProgramID id, U64 wait_us, RVS_ProgramSnapshot *snapshot_out);
RVS_Result rvs_session_fetch_process  (RVS_Session *session, RVS_ProcessID id, U64 wait_us, RVS_ProcessSnapshot *snapshot_out);
RVS_Result rvs_session_fetch_thread   (RVS_Session *session, RVS_ThreadID id, U64 wait_us, RVS_ThreadSnapshot *snapshot_out);

////////////////////////////////
// Request API

void       rvs_request_addref (RVS_Request *request);
void       rvs_request_release(RVS_Request *request);
RVS_Result rvs_request_wait   (RVS_Request *request, U64 wait_us, RVS_EngineReply *reply_out);

// Only the creator receives this pre-dispatch cancellation capability. Dispatched workflow
// preemption is engine-internal and is triggered only by Interrupt or Terminate workflows.
void       rvs_request_control_release(RVS_RequestControl *control);
RVS_Result rvs_request_control_cancel (RVS_RequestControl *control);

////////////////////////////////
// Internal

// Enum
internal RVS_EngineCommandKind rvs_command_kind_from_string(String8 v);
internal String8               rvs_string_from_command_kind(RVS_EngineCommandKind v);
internal String8               rvs_help_from_command_kind(RVS_EngineCommandKind v);
