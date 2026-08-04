// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "demon/demon_core.h"
#include "radvs/rvs.h"

////////////////////////////////
// Types

typedef struct RVS_Engine RVS_Engine;
typedef struct RVS_Session RVS_Session;
typedef struct RVS_Request RVS_Request;
typedef struct RVS_RequestControl RVS_RequestControl;
typedef DMN_Handle RVS_ProgramID;
typedef DMN_Handle RVS_ThreadID;

typedef enum
{
  RVS_StepKind_Into,
  RVS_StepKind_Over,
  RVS_StepKind_Out,
} RVS_StepKind;

typedef enum
{
  RVS_StepUnit_Statement,
  RVS_StepUnit_Line,
  RVS_StepUnit_Instruction,
} RVS_StepUnit;

typedef struct
{
  RVS_Request        *request;
  RVS_RequestControl *control;
} RVS_SubmitInfo;

// Control-operation terminology:
// - Terminate permanently ends selected targets and completes after confirmed exits.
// - Interrupt and Break temporarily stop execution; a selected-target interrupt completes
//   after confirmed selected stops and resumption of any temporarily halted unselected targets.
// - Stop is reserved as an alias for Terminate, never for temporary interruption.

////////////////////////////////
// Command

#define RVS_ENGINE_COMMAND_XLIST \
  X(Launch, "Launch program and stop at the entry point") \
  X(Run, "Run selected programs") \
  X(Interrupt, "Interrupt selected running programs")

typedef enum
{
  RVS_EngineCommandKind_Null,
#define X(kind, ...) RVS_EngineCommandKind_##kind,
  RVS_ENGINE_COMMAND_XLIST
#undef X
} RVS_EngineCommandKind;

////////////////////////////////
// Replies

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
// Events

typedef DMN_Event RVS_Event;

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
// Runs toward one absolute runtime virtual address. Completion acknowledges that execution resumed.
RVS_Result rvs_session_run_to_address (RVS_Session *session, RVS_ProgramID program_id, U64 vaddr, RVS_SubmitInfo *submit_out);
RVS_Result rvs_session_interrupt_many (RVS_Session *session, RVS_ProgramID *programs, U64 programs_count, RVS_SubmitInfo *submit_out);
RVS_Result rvs_session_interrupt      (RVS_Session *session, RVS_ProgramID program_id, RVS_SubmitInfo *submit_out);
RVS_Result rvs_session_select_thread  (RVS_Session *session, RVS_ProgramID program_id, RVS_ThreadID thread_id);
RVS_Result rvs_session_selected_thread(RVS_Session *session, RVS_ProgramID *program_id_out, RVS_ThreadID *thread_id_out);
RVS_Result rvs_session_continue       (RVS_Session *session, RVS_ProgramID program_id, RVS_SubmitInfo *submit_out);
RVS_Result rvs_session_step           (RVS_Session *session, RVS_StepKind kind, RVS_StepUnit unit, RVS_ThreadID thread_id, RVS_SubmitInfo *submit_out);
RVS_Result rvs_session_wait_for_event (Arena *arena, RVS_Session *session, U64 wait_us, RVS_Event *event_out);

////////////////////////////////
// Request API

void       rvs_request_addref(RVS_Request *request);
void       rvs_request_release(RVS_Request *request);
RVS_Result rvs_request_wait(RVS_Request *request, U64 wait_us, RVS_EngineReply *reply_out);

// Only the creator receives this pre-dispatch cancellation capability. Dispatched workflow
// preemption is engine-internal and is triggered only by Interrupt or Terminate workflows.
void       rvs_request_control_release(RVS_RequestControl *control);
RVS_Result rvs_request_control_cancel(RVS_RequestControl *control);

////////////////////////////////
// Internal

// Enum
internal RVS_EngineCommandKind rvs_command_kind_from_string(String8 v);
internal String8               rvs_string_from_command_kind(RVS_EngineCommandKind v);
internal String8               rvs_help_from_command_kind(RVS_EngineCommandKind v);
