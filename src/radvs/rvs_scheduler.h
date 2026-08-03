// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/rvs_engine.h"

typedef struct RVS_EngineControl         RVS_EngineControl;
typedef struct RVS_RequestPool           RVS_RequestPool;
typedef struct RVS_ScheduledOperation    RVS_ScheduledOperation;
typedef struct RVS_TargetSnapshotStorage RVS_TargetSnapshotStorage;
typedef struct RVS_Scheduler             RVS_Scheduler;

typedef enum
{
  RVS_SchedulerEvent_Null,
  RVS_SchedulerEvent_DispatchStarted,
  RVS_SchedulerEvent_DispatchAccepted,
  RVS_SchedulerEvent_OperationFailed,
  RVS_SchedulerEvent_BackendCompleted,
  RVS_SchedulerEvent_RunFinished,
  RVS_SchedulerEvent_LaunchStarted,
  RVS_SchedulerEvent_DemonEventBatch,
  RVS_SchedulerEvent_CommandOutcome,
  RVS_SchedulerEvent_PreDispatchCancelled,
  RVS_SchedulerEvent_Shutdown,
} RVS_SchedulerEventKind;

typedef enum
{
  RVS_SchedulerDecisionStatus_Applied,
  RVS_SchedulerDecisionStatus_Rejected,
  RVS_SchedulerDecisionStatus_IgnoredStale,
} RVS_SchedulerDecisionStatus;

typedef enum
{
  RVS_SchedulerEmission_Null,
  RVS_SchedulerEmission_RetireTarget,
  RVS_SchedulerEmission_CompleteRequest,
} RVS_SchedulerEmissionKind;

typedef enum
{
  RVS_SchedulerCommand_Null,
  RVS_SchedulerCommand_ResumeTargetSubset,
  RVS_SchedulerCommand_PumpLaunch,
  RVS_SchedulerCommand_PublishTarget,
} RVS_SchedulerCommandKind;

typedef enum
{
  RVS_SchedulerCommandOutcome_Null,
  RVS_SchedulerCommandOutcome_Failed,
  RVS_SchedulerCommandOutcome_PublishTargetCompleted,
  RVS_SchedulerCommandOutcome_ResumeTargetSubsetCompleted,
} RVS_SchedulerCommandOutcomeKind;

typedef enum
{
  RVS_SchedulerEventBatchSource_Execution,
  RVS_SchedulerEventBatchSource_PumpLaunch,
} RVS_SchedulerEventBatchSource;

typedef enum
{
  RVS_TargetState_Stopped,
  RVS_TargetState_Queued,
  RVS_TargetState_Running,
  RVS_TargetState_PausedForEvent,
  RVS_TargetState_StoppedAttention,
  RVS_TargetState_Terminating,
  RVS_TargetState_Removed,
} RVS_TargetState;

typedef RVS_TargetState RVS_TargetExecutionState;
#define RVS_TargetExecutionState_Idle             RVS_TargetState_Stopped
#define RVS_TargetExecutionState_Queued           RVS_TargetState_Queued
#define RVS_TargetExecutionState_RunInFlight      RVS_TargetState_Running
#define RVS_TargetExecutionState_InterruptPending RVS_TargetState_PausedForEvent

typedef enum
{
  RVS_LaunchPhase_AwaitLaunchStarted,
  RVS_LaunchPhase_AwaitCreateProcess,
  RVS_LaunchPhase_AwaitTargetRegistration,
} RVS_LaunchPhase;

typedef enum
{
  RVS_SchedulerOp_Null,
  RVS_SchedulerOp_Launch,
  RVS_SchedulerOp_Run,
  RVS_SchedulerOp_Interrupt,
  RVS_SchedulerOp_Terminate,
  RVS_SchedulerOp_ReadOnly,
  RVS_SchedulerOp_TargetConfiguration,
} RVS_SchedulerOp;

typedef enum
{
  RVS_SchedulerDuplicate_Reject,
  RVS_SchedulerDuplicate_Join,
} RVS_SchedulerDuplicate;

typedef struct
{
  RVS_SchedulerOp op;
  RVS_ProgramID   target;
  U64             identity;
} RVS_SchedulerKey;

typedef enum
{
  RVS_SchedulerEventDisposition_Forward,
  RVS_SchedulerEventDisposition_Suppress,
} RVS_SchedulerEventDisposition;

typedef enum
{
  RVS_SchedulerPhase_Stopped,
  RVS_SchedulerPhase_PreparingResume,
  RVS_SchedulerPhase_Running,
  RVS_SchedulerPhase_Interrupting,
  RVS_SchedulerPhase_CollectingStop,
  RVS_SchedulerPhase_PublishingStop,
  RVS_SchedulerPhase_Exited,
} RVS_SchedulerPhase;

typedef enum
{
  RVS_RunIntentKind_None,
  RVS_RunIntentKind_Continue,
  RVS_RunIntentKind_StepInto,
  RVS_RunIntentKind_StepOver,
  RVS_RunIntentKind_StepOut,
} RVS_RunIntentKind;

typedef enum
{
  RVS_RunIntentState_None,
  RVS_RunIntentState_Active,
  RVS_RunIntentState_Suspended,
  RVS_RunIntentState_Completed,
  RVS_RunIntentState_Cancelled,
} RVS_RunIntentState;

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

typedef struct RVS_TargetLedgerEntry RVS_TargetLedgerEntry;
typedef struct RVS_ThreadLedgerEntry RVS_ThreadLedgerEntry;
struct RVS_TargetLedgerEntry
{
  RVS_TargetLedgerEntry    *next;
  RVS_ProgramID             target;
  RVS_TargetState           state;
  U64                       revision;
  U64                       execution_token;
  RVS_MessageID             execution_owner;
  B32                       is_termination_fenced;
};

struct RVS_ThreadLedgerEntry
{
  RVS_ThreadLedgerEntry *next;
  RVS_ThreadID           thread;
  RVS_ProgramID          target;
  B32                    is_removed;
};

typedef struct
{
  RVS_ProgramID target;
  U64           revision;
  U64           execution_token;
  B32           is_termination_fenced;
  B32           stop_observed;
  B32           exit_observed;
  B32           is_selected;
} RVS_TargetSnapshot;

typedef struct
{
  RVS_RunIntentKind  kind;
  RVS_RunIntentState state;
  RVS_MessageID      execution_owner;
} RVS_RunIntent;

typedef struct
{
  RVS_ScheduledOperation *owner;
  RVS_MessageID           execution_owner;
  U64                     cycle_epoch;
  U32                     interrupt_attempt;
  U64                     collected_events_count;
  RVS_StopCause           primary_cause;
  RVS_ProgramID           primary_target;
  B32                     backend_stable;
  B32                     event_published;
} RVS_StopTransaction;

typedef struct
{
  RVS_ScheduledOperation *owner;
  RVS_MessageID           execution_owner;
  U64                     cycle_epoch;
  U32                     resume_attempt;
} RVS_ResumeTransaction;

typedef struct
{
  RVS_MessageID request_id;
  U64           command_id;
} RVS_SchedulerCommandToken;

typedef struct
{
  RVS_SchedulerCommandKind  kind;
  RVS_SchedulerCommandToken token;
  B32                       has_started;
} RVS_SchedulerPendingCommand;

typedef struct RVS_SchedulerEmission RVS_SchedulerEmission;
struct RVS_SchedulerEmission
{
  RVS_SchedulerEmission    *next;
  RVS_Scheduler            *scheduler;
  RVS_SchedulerEmissionKind kind;
  RVS_ScheduledOperation   *operation;
  RVS_EngineReply           reply;
  U32                       pid;
  U32                       exit_code;
  DMN_Handle                process;
};

typedef struct
{
  RVS_SchedulerCommandKind  kind;
  RVS_SchedulerCommandToken token;
  RVS_ScheduledOperation   *operation;
  union {
    struct {
      RVS_ProgramID *targets;
      U64            targets_count;
      RVS_MessageID  execution_request_id;
    } resume_target_subset;
    struct {
      U32        pid;
      DMN_Handle process;
    } publish_target;
  };
} RVS_SchedulerCommand;

struct RVS_TargetSnapshotStorage
{
  RVS_TargetSnapshotStorage *next;
  U64                        capacity;
  RVS_TargetSnapshot        *targets;
};

typedef struct
{
  RVS_SchedulerEmission *first;
  RVS_SchedulerEmission *last;
} RVS_SchedulerEmissionList;

typedef struct
{
  RVS_SchedulerDecisionStatus status;
  RVS_Result                  result;
  // State and pending-command changes are committed before return. Emissions are
  // ordered, best-effort consequences and never cause scheduler rollback.
  RVS_SchedulerEmissionList   emissions;
  RVS_SchedulerCommand        command;
} RVS_SchedulerDecision;

typedef struct
{
  RVS_SchedulerEventKind kind;
  union {
    struct { RVS_MessageID     request_id; } request;
    struct { RVS_EngineReply   reply;      } completed;
    struct { RVS_MessageID     request_id; U32 pid; } launch_started;
    struct {
      RVS_SchedulerEventBatchSource source;
      RVS_SchedulerCommandToken     command;
      RVS_MessageID                  request_id;
      DMN_EventList                  events;
      RVS_SchedulerEventDisposition *dispositions;
      U64                            dispositions_count;
    } demon_events;
    struct {
      RVS_SchedulerCommandOutcomeKind kind;
      RVS_SchedulerCommandKind        command_kind;
      RVS_SchedulerCommandToken       command;
      RVS_Result                      result;
      RVS_ProgramID                   target;
      U32                             pid;
    } command_outcome;
    struct { RVS_MessageID request_id; RVS_ProgramID target; U64 execution_token; } observed;
  };
} RVS_SchedulerEvent;

struct RVS_Scheduler
{
  Arena                     *arena;
  RVS_ScheduledOperation    *operation_first;
  RVS_ScheduledOperation    *operation_last;
  RVS_ScheduledOperation    *key_first;
  RVS_ScheduledOperation    *key_last;
  Mutex                      recycle_mutex;
  RVS_ScheduledOperation    *operation_free_first;
  RVS_SchedulerEmission     *emission_free_first;
  RVS_TargetSnapshotStorage *target_storage_free_first;
  RVS_TargetLedgerEntry     *target_first;
  RVS_TargetLedgerEntry     *target_last;
  RVS_ThreadLedgerEntry     *thread_first;
  RVS_ThreadLedgerEntry     *thread_last;
  U64                        next_command_id;
  RVS_SchedulerPhase         phase;
  U64                        run_cycle_epoch;
  RVS_RunIntent              run_intent;
  RVS_ProgramID              selected_target;
  RVS_ThreadID               selected_thread;
  RVS_StopTransaction        stop_transaction;
  RVS_ResumeTransaction      resume_transaction;
};

typedef struct
{
  RVS_ScheduledOperation *operation;
  RVS_Request            *request;
  B32                     joined;
  B32                     registered;
  B32                     is_terminal;
} RVS_SchedulerAdmission;

struct RVS_ScheduledOperation
{
  RVS_ScheduledOperation    *next;
  RVS_ScheduledOperation    *prev;
  RVS_ScheduledOperation    *key_next;
  RVS_ScheduledOperation    *key_prev;
  RVS_Scheduler             *scheduler;
  RVS_Request               *request;
  RVS_TargetSnapshot        *targets;
  U64                        targets_count;
  RVS_TargetSnapshotStorage *target_storage;
  U32                        ref_count;
  B32                        is_dispatched;
  B32                        is_keyed;
  U64                        captured_program_state_epoch;
  RVS_LaunchPhase            launch_phase;
  RVS_SchedulerPendingCommand pending_command;
  U32                        launch_pid;
  DMN_Handle                 launch_process;
  RVS_SchedulerKey           key;
};

struct RVS_RequestControl
{
  Arena                  *arena;
  RVS_Session            *session;
  RVS_EngineControl      *control;
  RVS_ScheduledOperation *operation;
  B32                     registered;
};

// The following scheduler mutation APIs require the owning session's control mutex.
internal B32                     rvs_scheduler_key_is_well_formed               (RVS_SchedulerKey key);
internal B32                     rvs_scheduler_key_match                        (RVS_SchedulerKey a, RVS_SchedulerKey b);
internal B32                     rvs_scheduler_keys_conflict                    (RVS_SchedulerKey a, RVS_SchedulerKey b);

internal B32                     rvs_scheduler_has_conflicting_operation_locked (RVS_Scheduler *scheduler, RVS_SchedulerKey key, RVS_ProgramID *targets, U64 targets_count);
internal void                    rvs_scheduler_target_add_locked                (RVS_Scheduler *scheduler, RVS_ProgramID target);
internal RVS_TargetLedgerEntry  *rvs_scheduler_target_from_id_locked            (RVS_Scheduler *scheduler, RVS_ProgramID target);
internal RVS_ThreadLedgerEntry  *rvs_scheduler_thread_from_id_locked            (RVS_Scheduler *scheduler, RVS_ThreadID thread);
internal void                    rvs_scheduler_release_execution_leases_locked  (RVS_Scheduler *scheduler);
internal B32                     rvs_scheduler_execution_blocks_operation_locked(RVS_Scheduler *scheduler, RVS_SchedulerOp op);
internal RVS_ScheduledOperation *rvs_scheduler_operation_alloc_locked           (RVS_Scheduler *scheduler, RVS_RequestPool *expected_pool, RVS_MessageID request_id, RVS_SchedulerKey key, RVS_ProgramID *targets, U64 targets_count, U64 captured_program_state_epoch);

internal void                    rvs_scheduler_operation_addref                 (RVS_ScheduledOperation *operation);
internal void                    rvs_scheduler_operation_release                (RVS_ScheduledOperation *operation);

internal void                    rvs_scheduler_operation_remove_locked          (RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation);
internal RVS_ScheduledOperation *rvs_scheduler_find_active_operation_locked     (RVS_Scheduler *scheduler, RVS_MessageID request_id);
internal RVS_Result              rvs_scheduler_register_operation_locked        (RVS_Scheduler *scheduler, RVS_RequestPool *expected_pool, RVS_SchedulerKey key, RVS_ScheduledOperation *operation);
internal RVS_ScheduledOperation *rvs_scheduler_unregister_operation_locked      (RVS_Scheduler *scheduler, RVS_SchedulerKey key);

internal RVS_RequestControl     *rvs_request_control_alloc                      (RVS_Session *session, RVS_ScheduledOperation *operation, B32 registered);

internal RVS_Result              rvs_scheduler_admit_locked                     (RVS_Scheduler *scheduler, RVS_RequestPool *expected_pool, U64 *next_request_id, RVS_SchedulerOp op, RVS_SchedulerKey key, RVS_ProgramID *targets, U64 targets_count, U64 captured_program_state_epoch, RVS_SchedulerAdmission *admission_out);
internal void                    rvs_scheduler_rollback_admission_locked        (RVS_Scheduler *scheduler, RVS_SchedulerAdmission *admission);
internal void                    rvs_scheduler_apply_locked                     (RVS_Scheduler *scheduler, RVS_SchedulerEvent event, RVS_SchedulerDecision *decision_out);
internal B32                     rvs_scheduler_begin_command_locked             (RVS_Scheduler *scheduler, RVS_SchedulerCommand *command);
internal void                    rvs_scheduler_decision_release                 (RVS_SchedulerDecision *decision);
