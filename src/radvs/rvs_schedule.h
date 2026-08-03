// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/rvs_engine.h"

typedef struct RVS_EngineControl RVS_EngineControl;
typedef struct RVS_RequestPool RVS_RequestPool;
typedef struct RVS_ScheduledOperation RVS_ScheduledOperation;

typedef enum
{
  RVS_SessionExecutionState_Idle,
  RVS_SessionExecutionState_Queued,
  RVS_SessionExecutionState_RunInFlight,
} RVS_SessionExecutionState;

typedef struct
{
  Arena                  *arena;
  RVS_ScheduledOperation *operation_first;
  RVS_ScheduledOperation *operation_last;
  RVS_ScheduledOperation *key_first;
  RVS_ScheduledOperation *key_last;
  RVS_ScheduledOperation *free_first;
  RVS_SessionExecutionState execution_state;
  RVS_MessageID             execution_request_id; // lease survives Run request completion until RunFinished
} RVS_Scheduler;

typedef struct
{
  RVS_ScheduledOperation *operation;
  B32                     joined;
  B32                     registered;
} RVS_SchedulerAdmission;

struct RVS_ScheduledOperation
{
  RVS_ScheduledOperation *next;
  RVS_ScheduledOperation *prev;
  RVS_ScheduledOperation *key_next;
  RVS_ScheduledOperation *key_prev;
  RVS_Scheduler          *scheduler;
  RVS_Request            *request;
  RVS_ProgramID          *targets;
  U64                     targets_count;
  U32                     ref_count;
  B32                     is_dispatched;
  U64                     captured_program_state_epoch;
  U32                     launch_pid;
  RVS_EngineCommandKind   command_kind;
  RVS_RequestPolicy       policy;
  RVS_OperationKey        key;
};

struct RVS_RequestControl
{
  Arena             *arena;
  RVS_Session       *session;
  RVS_EngineControl *control;
  RVS_ScheduledOperation *operation;
  B32                registered;
};

// The following scheduler mutation APIs require the owning session's control mutex.
internal B32          rvs_operation_key_is_well_formed_for_policy(RVS_RequestPolicy policy, RVS_OperationKey key);
internal B32          rvs_operation_key_match(RVS_OperationKey a, RVS_OperationKey b);
internal B32          rvs_operation_keys_conflict(RVS_OperationKey a, RVS_OperationKey b);
internal B32          rvs_scheduler_has_conflicting_operation_locked(RVS_Scheduler *scheduler, RVS_OperationKey key);
internal B32          rvs_scheduler_reserve_execution_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id);
internal B32          rvs_scheduler_mark_run_in_flight_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id);
internal B32          rvs_scheduler_clear_queued_execution_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id);
internal B32          rvs_scheduler_finish_run_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id);
internal B32          rvs_scheduler_execution_blocks_operation_locked(RVS_Scheduler *scheduler, RVS_OperationClass operation_class);
internal RVS_ScheduledOperation *rvs_scheduler_operation_alloc_locked(RVS_Scheduler *scheduler, RVS_RequestPool *expected_pool, RVS_MessageID request_id, RVS_RequestPolicy policy, RVS_OperationKey key, RVS_ProgramID *targets, U64 targets_count, U64 captured_program_state_epoch);
internal void                    rvs_scheduler_operation_addref(RVS_ScheduledOperation *operation);
internal void                    rvs_scheduler_operation_release(RVS_ScheduledOperation *operation);
internal void                    rvs_scheduler_operation_remove_locked(RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation);
internal RVS_ScheduledOperation *rvs_scheduler_find_active_operation_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id);
internal RVS_ScheduledOperation *rvs_scheduler_operation_mark_dispatched_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id);
internal RVS_Result              rvs_scheduler_register_operation_locked(RVS_Scheduler *scheduler, RVS_RequestPool *expected_pool, RVS_OperationKey key, RVS_ScheduledOperation *operation);
internal RVS_ScheduledOperation *rvs_scheduler_unregister_operation_locked(RVS_Scheduler *scheduler, RVS_OperationKey key);
internal RVS_ScheduledOperation *rvs_scheduler_retire_undispatched_operation_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id);
internal RVS_ScheduledOperation *rvs_scheduler_take_active_operations_locked(RVS_Scheduler *scheduler);
internal RVS_ScheduledOperation *rvs_scheduler_take_operation_keys_locked(RVS_Scheduler *scheduler);
internal RVS_RequestControl *rvs_request_control_alloc(RVS_Session *session, RVS_ScheduledOperation *operation, B32 registered);
internal RVS_Result rvs_scheduler_admit_locked(RVS_Scheduler *scheduler, RVS_RequestPool *expected_pool, U64 *next_request_id, RVS_RequestPolicy policy, RVS_OperationKey key, RVS_ProgramID *targets, U64 targets_count, U64 captured_program_state_epoch, RVS_SchedulerAdmission *admission_out);
internal void       rvs_scheduler_rollback_admission_locked(RVS_Scheduler *scheduler, RVS_SchedulerAdmission *admission);
