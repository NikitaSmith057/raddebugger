// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/rvs_engine.h"
#include "radvs/rvs_async.h"
#include "radvs/rvs_entity.h"
#include "radvs/rvs_scheduler.h"

struct RVS_Session
{
  Arena             *arena;
  RVS_Engine        *engine;
  RVS_EngineControl *control;
  U32                ref_count;
  B32                engine_released;
  RVS_Queue         *event_queue;
  RVS_EntityStore    entities;
  RVS_Scheduler      scheduler;
};

internal RVS_Session *rvs_session_alloc(RVS_Engine *engine);
internal void         rvs_session_release_engine(RVS_Session *session);
internal B32          rvs_session_operation_key_resolves_locked(RVS_Session *session, RVS_SchedulerKey key);
internal RVS_Result   rvs_session_push_event(RVS_Session *session, RVS_Event *event);
internal void         rvs_session_close_events(RVS_Session *session);
