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

////////////////////////////////
// Submission

typedef enum
{
  RVS_RequestPolicy_Null,

  // A conflicting keyed request is rejected with RVS_Result_AlreadyPending.
  RVS_RequestPolicy_RejectIfPending,

  // A request with duplicate key is joined; other conflicts are rejected with RVS_Result_AlreadyPending.
  RVS_RequestPolicy_JoinIfEqual,
} RVS_RequestPolicy;

typedef enum
{
  RVS_OperationClass_Null,

  // Request observes state for one known program without taking execution control.
  // It remains admissible while program execution is active and relies on epoch freshness.
  // It does not conflict with other read-only requests.
  // It does not conflict with execution requests.
  // It conflicts with SessionLifecycle, because lifecycle is session-wide lock.
  RVS_OperationClass_ReadOnly,

  // Operation controls multiple programs through the session's single execution controller.
  // It has no single program ID and does not conflict with ReadOnly work.
  RVS_OperationClass_SessionExecution,

  // Operation temporarily requires exclusive access to session-wide backend/topology state.
  // It is rejected while program execution is queued or in flight.
  // It does not invalidate program state epochs.
  // A conflicting keyed submission is rejected with RVS_Result_AlreadyPending.
  RVS_OperationClass_SessionLifecycle,
} RVS_OperationClass;

typedef struct
{
  RVS_OperationClass operation_class;
  RVS_ProgramID      program_id;
  U64                operation_id;
} RVS_OperationKey;

typedef struct
{
  RVS_Request        *request;
  RVS_RequestControl *control;
} RVS_SubmitInfo;

////////////////////////////////
// Command

#define RVS_ENGINE_COMMAND_XLIST \
  X(Launch, RVS_OperationClass_SessionLifecycle, RVS_RequestPolicy_RejectIfPending, "Launch program and stop at the entry point") \
  X(Run,    RVS_OperationClass_SessionExecution, RVS_RequestPolicy_RejectIfPending, "Run selected programs")

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

// The singleton-backed DEMON implementation supports one active session per engine.
RVS_Result rvs_engine_create_session(RVS_Engine *engine, RVS_Session **session_out);
void       rvs_session_addref(RVS_Session *session);
void       rvs_session_release(RVS_Session *session);

RVS_Result rvs_session_launch(RVS_Session *session, String8 cmdl, String8 wdir, RVS_SubmitInfo *submit_out);
RVS_Result rvs_session_run_many(RVS_Session *session, RVS_ProgramID *programs, U64 programs_count, RVS_SubmitInfo *submit_out);
RVS_Result rvs_session_run(RVS_Session *session, RVS_ProgramID program_id, RVS_SubmitInfo *submit_out);
RVS_Result rvs_session_wait_for_event(Arena *arena, RVS_Session *session, U64 wait_us, RVS_Event *event_out);

////////////////////////////////
// Request API

void       rvs_request_addref(RVS_Request *request);
void       rvs_request_release(RVS_Request *request);
RVS_Result rvs_request_wait(RVS_Request *request, U64 wait_us, RVS_EngineReply *reply_out);

// Only the creator receives this pre-dispatch cancellation capability.
void       rvs_request_control_release(RVS_RequestControl *control);
RVS_Result rvs_request_control_cancel(RVS_RequestControl *control);


////////////////////////////////
// Internal

// Enum
internal RVS_EngineCommandKind rvs_command_kind_from_string(String8 v);
internal String8               rvs_string_from_command_kind(RVS_EngineCommandKind v);
internal String8               rvs_help_from_command_kind(RVS_EngineCommandKind v);
internal RVS_OperationClass    rvs_op_class_from_engine_command_kind(RVS_EngineCommandKind kind);
internal RVS_RequestPolicy     rvs_request_policy_from_engine_command_kind(RVS_EngineCommandKind v);
