// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/rvs_engine.h"

typedef struct RVS_EngineControl RVS_EngineControl;
typedef struct RVS_RequestPool RVS_RequestPool;

typedef enum
{
  RVS_SessionExecutionState_Idle,
  RVS_SessionExecutionState_Queued,
  RVS_SessionExecutionState_RunInFlight,
} RVS_SessionExecutionState;

typedef struct
{
  RVS_Request *request_first;
  RVS_Request *request_last;
  RVS_Request *key_first;
  RVS_Request *key_last;
  RVS_SessionExecutionState execution_state;
  RVS_MessageID             execution_request_id;
} RVS_Scheduler;

typedef struct
{
  RVS_Request *request;
  B32          joined;
  B32          registered;
} RVS_SchedulerAdmission;

struct RVS_RequestControl
{
  Arena             *arena;
  RVS_Session       *session;
  RVS_EngineControl *control;
  RVS_Request       *request;
  RVS_OperationKey   key;
  B32                registered;
};

// The following scheduler mutation APIs require session->control->mutex.
internal B32          rvs_operation_key_is_well_formed_for_policy(RVS_RequestPolicy policy, RVS_OperationKey key);
internal B32          rvs_operation_key_match(RVS_OperationKey a, RVS_OperationKey b);
internal B32          rvs_operation_keys_conflict(RVS_OperationKey a, RVS_OperationKey b);
internal B32          rvs_session_has_conflicting_operation_locked(RVS_Session *session, RVS_OperationKey key);
internal B32          rvs_session_reserve_execution_locked(RVS_Session *session, RVS_MessageID request_id);
internal B32          rvs_session_mark_run_in_flight_locked(RVS_Session *session, RVS_MessageID request_id);
internal B32          rvs_session_clear_queued_execution_locked(RVS_Session *session, RVS_MessageID request_id);
internal B32          rvs_session_finish_run_locked(RVS_Session *session, RVS_MessageID request_id);
internal B32          rvs_session_execution_blocks_operation_locked(RVS_Session *session, RVS_OperationClass operation_class);
internal RVS_Request *rvs_session_request_alloc_locked(RVS_Session *session, RVS_RequestPool *pool, RVS_MessageID request_id, RVS_OperationKey key);
internal void         rvs_session_request_remove_locked(RVS_Session *session, RVS_Request *request);
internal RVS_Request *rvs_session_find_active_request_locked(RVS_Session *session, RVS_MessageID request_id);
internal RVS_Request *rvs_session_request_mark_dispatched_locked(RVS_Session *session, RVS_MessageID request_id);
internal RVS_Result   rvs_session_register_operation_locked(RVS_Session *session, RVS_OperationKey key, RVS_Request *request);
internal RVS_Request *rvs_session_unregister_operation_locked(RVS_Session *session, RVS_OperationKey key);
internal RVS_Request *rvs_session_retire_undispatched_request_locked(RVS_Session *session, RVS_MessageID request_id);
internal RVS_Request *rvs_session_take_active_requests_locked(RVS_Session *session);
internal RVS_Request *rvs_session_take_operation_keys_locked(RVS_Session *session);
internal RVS_RequestControl *rvs_request_control_alloc(RVS_Session *session, RVS_Request *request, RVS_OperationKey key, B32 registered);
internal RVS_Result rvs_scheduler_admit_locked(RVS_Session *session, RVS_RequestPool *pool, U64 *next_request_id, RVS_RequestPolicy policy, RVS_OperationKey key, U64 captured_program_state_epoch, RVS_SchedulerAdmission *admission_out);
internal void       rvs_scheduler_rollback_admission_locked(RVS_Session *session, RVS_SchedulerAdmission *admission);
