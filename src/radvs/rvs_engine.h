// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "demon/demon_core.h"
#include "radvs/rvs.h"

////////////////////////////////
// Types

typedef struct RVS_Engine RVS_Engine;
typedef struct RVS_Request RVS_Request;
typedef struct RVS_RequestControl RVS_RequestControl;
// Opaque generational DEMON process handle; valid only for an engine-known program.
typedef DMN_Handle RVS_ProgramID;

////////////////////////////////
// Submission

typedef enum
{
  RVS_RequestPolicy_Independent,
  RVS_RequestPolicy_JoinIfEqual,
  RVS_RequestPolicy_RejectIfPending,
} RVS_RequestPolicy;

typedef enum
{
  RVS_OperationClass_Null,
  RVS_OperationClass_ReadOnly,
  RVS_OperationClass_ProgramExecution,
  RVS_OperationClass_SessionLifecycle,
} RVS_OperationClass;

typedef struct
{
  // ReadOnly and ProgramExecution require a known program in this engine session.
  // SessionLifecycle intentionally has no program ID.
  RVS_OperationClass operation_class;
  U64                session_id;
  RVS_ProgramID      program_id;
  U64                operation_id;
} RVS_OperationKey;

typedef struct
{
  RVS_RequestPolicy policy;
  RVS_OperationKey  key;
} RVS_SubmitOptions;

typedef struct
{
  RVS_Request        *request;
  RVS_RequestControl *control;
} RVS_SubmitInfo;

////////////////////////////////
// Replies

typedef enum
{
  RVS_EngineReplyKind_Null,
  RVS_EngineReplyKind_Launch,
  RVS_EngineReplyKind_Run,
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
void       rvs_engine_shutdown(RVS_Engine *engine);

// A zeroed options value submits an independent request. The initial submission receives control.
RVS_Result rvs_engine_launch(RVS_Engine *engine, String8 cmdl, String8 wdir, RVS_SubmitOptions options, RVS_SubmitInfo *submit_out);
// Run always addresses one known program. An independent submission derives its ProgramExecution key.
RVS_Result rvs_engine_run(RVS_Engine *engine, RVS_ProgramID program_id, RVS_SubmitOptions options, RVS_SubmitInfo *submit_out);

////////////////////////////////
// Request API

void       rvs_request_retain(RVS_Request *request);
void       rvs_request_release(RVS_Request *request);
RVS_Result rvs_request_wait(RVS_Request *request, U64 wait_us, RVS_EngineReply *reply_out);

// Only the creator receives this pre-dispatch cancellation capability.
void       rvs_request_control_release(RVS_RequestControl *control);
RVS_Result rvs_request_control_cancel(RVS_RequestControl *control);

////////////////////////////////
// Event API

RVS_Result rvs_engine_wait_for_event(Arena *arena, RVS_Engine *engine, U64 wait_us, RVS_Event *event_out);
