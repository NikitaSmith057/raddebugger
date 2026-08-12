// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////

#pragma once
#include "demon/demon_core.h"
#include "radvs/rvs_core.h"
#include "radvs/rvs_request.h"
#include "radvs/rvs_queue.h"
#include "radvs/rvs_demon.h"
#include "radvs/rvs_entity.h"

////////////////////////////////

typedef struct RVS_Engine         RVS_Engine;
typedef struct RVS_Session        RVS_Session;
typedef struct RVS_RequestControl RVS_RequestControl;
typedef struct RVS_EngineControl  RVS_EngineControl;

////////////////////////////////

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
RVS_Result rvs_engine_step           (RVS_Engine *engine, RVS_StepKind kind, RVS_StepUnit unit, RVS_ThreadID thread_id, RVS_SubmitInfo *submit_out);
RVS_Result rvs_engine_select_thread  (RVS_Engine *engine, RVS_ThreadID thread_id, RVS_SubmitInfo *submit_out);

RVS_Result rvs_engine_wait_for_event(Arena *arena, RVS_Engine *engine, U64 wait_us, RVS_Event *event_out);
RVS_Result rvs_engine_copy_programs (Arena *arena, RVS_Engine *engine, RVS_Program **program_out, U64 *program_count_out);

RVS_Result rvs_engine_selected_thread(RVS_Engine *engine, RVS_ProgramID *program_id_out, RVS_ThreadID *thread_id_out);
RVS_Result rvs_engine_ack_event      (RVS_Engine *engine, U64 sequence);

////////////////////////////////
// Request API

B32        rvs_request_complete(RVS_Request *request, RVS_CommandReply reply);
U64        rvs_request_addref  (RVS_Request *request);
U64        rvs_request_release (RVS_Request *request);
RVS_Result rvs_request_wait    (RVS_Request *request, U64 wait_us, RVS_CommandReply *reply_out);

RVS_RequestControl * rvs_request_control_alloc  (RVS_SessionControlParams params, RVS_MessageID request_id);
void                 rvs_request_control_release(RVS_RequestControl *control);
RVS_Result           rvs_request_control_cancel (RVS_RequestControl *control);

