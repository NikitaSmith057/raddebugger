// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/rvs_engine.h"
#include "radvs/rvs_async.h"
#include "radvs/rvs_schedule.h"

typedef struct RVS_Program RVS_Program;

struct RVS_Program
{
  RVS_Program   *next;
  Arena         *arena;
  RVS_ProgramID  id;
  U32            pid;
  DMN_Handle     process;
  U64            state_epoch;
};

struct RVS_Session
{
  Arena             *arena;
  RVS_Engine        *engine;
  RVS_EngineControl *control;
  U32                ref_count;
  B32                engine_released;
  RVS_Queue         *event_queue;
  Arena             *program_arena;
  RVS_Program       *first_program;
  RVS_Program       *last_program;
  RVS_Scheduler      scheduler;
};

internal RVS_Session *rvs_session_alloc(RVS_Engine *engine);
internal void         rvs_session_release_engine(RVS_Session *session);
internal RVS_Program *rvs_session_program_from_id_locked(RVS_Session *session, RVS_ProgramID program_id);
internal U64          rvs_session_program_state_epoch_locked(RVS_Session *session, RVS_ProgramID program_id);
internal void         rvs_session_bump_program_state_epoch_locked(RVS_Session *session, RVS_ProgramID program_id);
internal B32          rvs_session_operation_key_resolves_locked(RVS_Session *session, RVS_OperationKey key);
internal RVS_Program *rvs_session_program_add_locked(RVS_Session *session, U32 pid, DMN_Handle process);
internal B32          rvs_session_programs_to_processes(RVS_Session *session, RVS_ProgramID *programs, U64 programs_count, DMN_Handle *processes_out);
internal void         rvs_session_prepare_reply_locked(RVS_Session *session, RVS_ScheduledOperation *operation, RVS_EngineReply *reply);
internal RVS_Result   rvs_session_push_event(RVS_Session *session, RVS_Event *event);
internal void         rvs_session_close_events_locked(RVS_Session *session);
