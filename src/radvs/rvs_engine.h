// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////

#pragma once
#include "demon/demon_core.h"
#include "radvs/rvs_core.h"
#include "radvs/rvs_request.h"
#include "radvs/rvs_async.h"
#include "radvs/rvs_demon.h"
#include "radvs/rvs_entity.h"

////////////////////////////////

typedef struct RVS_Engine         RVS_Engine;
typedef struct RVS_Session        RVS_Session;
typedef struct RVS_RequestControl RVS_RequestControl;
typedef struct RVS_EngineControl  RVS_EngineControl;

////////////////////////////////

typedef enum
{
  RVS_EventKind_First = DMN_EventKind_UserLo,
  RVS_EventKind_Error,
  RVS_EventKind_Stopped,
  RVS_EventKind_ProgramDestroyed,
} RVS_EventKind;

typedef struct
{
  U64           sequence;
  RVS_EventKind kind;
  RVS_ProgramID program;
  RVS_ProcessID process;
  RVS_ThreadID  thread;
  U32           pid;
  U32           tid;
  DMN_Event     raw_event; // backend event
  union {
    struct { U32 exit_code;      } process_exited;
    struct { DMN_ErrorKind kind; } error;
    struct {
      RVS_ProgramID primary_program;
      RVS_ProcessID primary_process;
      RVS_ThreadID  selected_thread;
      U32           pid;
      U32           tid;
      RVS_StopCause primary_cause;
      U64           stable_stop_generation;
    } stopped;
  };
} RVS_Event;

typedef struct
{
  RVS_Request        *request;
  RVS_RequestControl *control;
} RVS_SubmitInfo;

////////////////////////////////

typedef struct
{
  RVS_EngineControl *control;
  RVS_Session       *session;
} RVS_SessionControlParams;

typedef struct
{
  RVS_Engine        *engine;
  RVS_EngineControl *control;
  RVS_Session       *session;
} RVS_EngineSessionParams;

////////////////////////////////
// Engine API

RVS_Result rvs_engine_init(RVS_Engine **engine_out);
RVS_Result rvs_engine_shutdown(RVS_Engine *engine);

RVS_Result rvs_engine_launch         (RVS_Engine *engine, String8 cmdl, String8 wdir, RVS_SubmitInfo *submit_out);
RVS_Result rvs_engine_run            (RVS_Engine *engine, RVS_ProgramID *programs, U64 programs_count, RVS_SubmitInfo *submit_out);
RVS_Result rvs_engine_interrupt      (RVS_Engine *engine, RVS_ProgramID *programs, U64 programs_count, RVS_SubmitInfo *submit_out);
RVS_Result rvs_engine_continue       (RVS_Engine *engine, RVS_ProgramID program_id, RVS_SubmitInfo *submit_out);
RVS_Result rvs_engine_wait_for_event (Arena *arena, RVS_Engine *engine, U64 wait_us, RVS_Event *event_out);
RVS_Result rvs_engine_step           (RVS_Engine *engine, RVS_StepKind kind, RVS_StepUnit unit, RVS_ThreadID thread_id, RVS_SubmitInfo *submit_out);
RVS_Result rvs_engine_select_thread  (RVS_Engine *engine, RVS_ThreadID thread_id, RVS_SubmitInfo *submit_out);

RVS_Result rvs_engine_run_to_address (RVS_Engine *engine, RVS_ProgramID program_id, U64 vaddr, RVS_SubmitInfo *submit_out);
RVS_Result rvs_engine_selected_thread(RVS_Engine *engine, RVS_ProgramID *program_id_out, RVS_ThreadID *thread_id_out);
RVS_Result rvs_engine_ack_event      (RVS_Engine *engine, U64 sequence);
RVS_Result rvs_engine_fetch_program  (RVS_Engine *engine, RVS_ProgramID id, U64 wait_us, RVS_ProgramSnapshot *snapshot_out);
RVS_Result rvs_engine_fetch_process  (RVS_Engine *engine, RVS_ProcessID id, U64 wait_us, RVS_ProcessSnapshot *snapshot_out);
RVS_Result rvs_engine_fetch_thread   (RVS_Engine *engine, RVS_ThreadID id, U64 wait_us, RVS_ThreadSnapshot *snapshot_out);
RVS_Result rvs_engine_copy_programs  (Arena *arena, RVS_Engine *engine, RVS_ProgramSnapshot **snapshots_out, U64 *snapshots_count_out);

////////////////////////////////
// Request API

B32        rvs_request_complete(RVS_Request *request, RVS_CommandReply reply);
U64        rvs_request_addref  (RVS_Request *request);
U64        rvs_request_release (RVS_Request *request);
RVS_Result rvs_request_wait    (RVS_Request *request, U64 wait_us, RVS_CommandReply *reply_out);

RVS_RequestControl * rvs_request_control_alloc  (RVS_SessionControlParams params, RVS_MessageID request_id);
void                 rvs_request_control_release(RVS_RequestControl *control);
RVS_Result           rvs_request_control_cancel (RVS_RequestControl *control);

