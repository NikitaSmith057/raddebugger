// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

////////////////////////////////

#include "radvs/rvs_engine.h"
#include "radvs/rvs_entity.h"

////////////////////////////////

typedef struct RVS_EngineControl         RVS_EngineControl;
typedef struct RVS_RequestPool           RVS_RequestPool;
typedef struct RVS_ScheduledOperation    RVS_ScheduledOperation;
typedef struct RVS_TargetSnapshotStorage RVS_TargetSnapshotStorage;
typedef struct RVS_Plan                  RVS_Plan;
typedef struct RVS_Scheduler             RVS_Scheduler;

typedef enum
{
  RVS_SchedulerEmission_Null,
  RVS_SchedulerEmission_CompleteRequest,
  RVS_SchedulerEmission_PublishEvent,
} RVS_SchedulerEmissionKind;

typedef enum
{
  RVS_SchedulerCommand_Null,
  RVS_SchedulerCommand_LaunchExecution,
  RVS_SchedulerCommand_RunExecution,
  RVS_SchedulerCommand_InterruptExecution,
  RVS_SchedulerCommand_ResumeTargetSubset,
  RVS_SchedulerCommand_PumpLaunch,
  RVS_SchedulerCommand_PublishTarget,
} RVS_SchedulerCommandKind;

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
  RVS_SchedulerPhase_Running,
  RVS_SchedulerPhase_Exited,
} RVS_SchedulerPhase;

typedef enum
{
  RVS_ScheduledOperationState_Null,
  RVS_ScheduledOperationState_Queued,
  RVS_ScheduledOperationState_Active,
  RVS_ScheduledOperationState_Suspended,
  RVS_ScheduledOperationState_Completed,
  RVS_ScheduledOperationState_Cancelled,
  RVS_ScheduledOperationState_Failed,
} RVS_ScheduledOperationState;

typedef enum
{
  RVS_ControlTransactionPhase_Null,
  RVS_ControlTransactionPhase_WaitingForInterrupt,
  RVS_ControlTransactionPhase_CollectingBatch,
  RVS_ControlTransactionPhase_Stable,
  RVS_ControlTransactionPhase_WaitingForResume,
} RVS_ControlTransactionPhase;

typedef struct RVS_TargetControl RVS_TargetControl;
struct RVS_TargetControl
{
  RVS_TargetControl  *next;
  RVS_ProgramID       id;
  RVS_ProgramID       target;
  U64                 destroy_sequence;
  B32                 destroy_emitted;
  RVS_TargetState     state;
  U64                 revision;
  U64                 execution_token;
  RVS_MessageID       execution_owner;
  B32                 is_termination_fenced;
};

typedef struct
{
  RVS_ProgramID target;
  U64           revision;
  U64           execution_token;
  U64           stable_stop_generation;
  B32           is_termination_fenced;
  B32           stop_observed;
  B32           exit_observed;
  B32           is_selected;
} RVS_TargetSnapshot;

typedef struct
{
  RVS_MessageID request_id;
  U64           command_id;
} RVS_SchedulerCommandToken;

typedef U64 RVS_PlanID;

typedef enum
{
  RVS_PlanKind_Null,
  RVS_PlanKind_Run,
  RVS_PlanKind_RunToAddress,
#if RVS_ENGINE_TESTING
  RVS_PlanKind_Test,
#endif
} RVS_PlanKind;

typedef struct RVS_PlanHeader
{
  RVS_PlanID   id;
  RVS_PlanID   parent;
  RVS_PlanKind kind;
} RVS_PlanHeader;

typedef U64 RVS_StopHandlerID;

typedef enum
{
  RVS_StopPolicySource_Null,
  RVS_StopPolicySource_Plan,
  RVS_StopPolicySource_Handler,
  RVS_StopPolicySource_Fallback,
} RVS_StopPolicySource;

typedef enum
{
  RVS_StableStopDisposition_Unhandled,
  RVS_StableStopDisposition_Continue,
  // Reserved until concrete plans define and test their operation-settlement semantics.
  RVS_StableStopDisposition_Suspend,
  RVS_StableStopDisposition_Complete,
  RVS_StableStopDisposition_Cancel,
  RVS_StableStopDisposition_Fail,
} RVS_StableStopDispositionKind;

typedef struct
{
  RVS_StableStopDispositionKind kind;
  RVS_Result                    result;
} RVS_StableStopDisposition;

typedef struct
{
  // Valid only for the current locked dispatch. Plans retain identities and the generation, not this pointer.
  U64                       stable_stop_generation;
  RVS_MessageID             execution_request_id;
  RVS_SchedulerCommandToken interrupt_token;
  RVS_TargetSnapshot const *targets;
  U64                       targets_count;
  RVS_ProgramID             primary_target;
  RVS_ProcessID             primary_process;
  RVS_ThreadID              primary_thread;
  RVS_StopCause             primary_cause;
  B32                       execution_lease_fully_exited;
  B32                       amended_while_resume_pending;
} RVS_StableStop;

struct RVS_Plan
{
  RVS_Plan                 *next;
  RVS_Plan                 *prev;
  RVS_PlanHeader            header;
  RVS_MessageID             operation_request_id;
  union {
    struct {
      RVS_ProgramID target;
      U64           vaddr;
    } run_to_address;
  };
#if RVS_ENGINE_TESTING
  RVS_StableStopDisposition test_disposition;
#endif
};

typedef struct
{
  RVS_ScheduledOperation *owner;
  RVS_MessageID           execution_request_id;
  RVS_SchedulerCommandToken interrupt_token;
  RVS_ControlTransactionPhase phase;
  U64                     stable_stop_generation;
  RVS_StopCause           primary_cause;
  RVS_ProgramID           primary_target;
  RVS_ProcessID           primary_process;
  RVS_ThreadID            primary_thread;
  // Post-fence exits affect settlement only; they never redispatch policy for this generation.
  B32                     amended_while_resume_pending;
} RVS_StopTransaction;

typedef struct
{
  RVS_SchedulerCommandKind  kind;
  RVS_SchedulerCommandToken token;
  B32                       has_started;
} RVS_SchedulerPendingCommand;

typedef struct
{
  RVS_SchedulerCommandKind  kind;
  RVS_SchedulerCommandToken token;
  RVS_ScheduledOperation   *operation;
  union {
    struct {
      ProcessLaunchParams params;
    } launch_execution;
    struct {
      RVS_ProgramID    *targets;
      U64               targets_count;
      DMN_Handle       *processes;
      U64               processes_count;
      DMN_TrapChunkList traps;
      RVS_Result        prepare_result;
    } run_execution;
    struct {
      // GlobalWithResume interruption covers the complete active execution lease, not only the selected subset.
      RVS_MessageID execution_request_id;
    } interrupt_execution;
    struct {
      RVS_ProgramID *targets;
      U64            targets_count;
      RVS_MessageID  execution_request_id;
      DMN_Handle       *processes;
      U64               processes_count;
      DMN_TrapChunkList traps;
      RVS_Result        prepare_result;
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

typedef struct RVS_SchedulerEmission RVS_SchedulerEmission;
struct RVS_SchedulerEmission
{
  RVS_SchedulerEmission    *next;
  RVS_Scheduler            *scheduler;
  RVS_SchedulerEmissionKind kind;
  RVS_ScheduledOperation   *operation;
  RVS_EngineReply           reply;
  RVS_Event                 event;
};

typedef struct
{
  RVS_SchedulerEmission *first;
  RVS_SchedulerEmission *last;
} RVS_SchedulerEmissionList;

typedef struct
{
  RVS_Result                  result;
  // State and pending-command changes are committed before return. Emissions are
  // ordered, best-effort consequences and never cause scheduler rollback.
  RVS_SchedulerEmissionList   emissions;
  RVS_SchedulerCommand        command;
} RVS_SchedulerDecision;

typedef enum
{
  RVS_RunIntentState_None,
  RVS_RunIntentState_Active,
  RVS_RunIntentState_Suspended,
  RVS_RunIntentState_Completed,
  RVS_RunIntentState_Cancelled,
} RVS_RunIntentState;

typedef struct
{
  RVS_RunIntentKind  kind;
  RVS_StepKind       step_kind;
  RVS_StepUnit       step_unit;
  RVS_ThreadID       thread;
  RVS_RunIntentState state;
} RVS_RunIntent;

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
  RVS_SchedulerEvent_RequestControlReleased,
  RVS_SchedulerEvent_ThreadSelected,
  RVS_SchedulerEvent_AcknowledgeEvent,
  RVS_SchedulerEvent_Shutdown,
} RVS_SchedulerEventKind;

typedef struct
{
  RVS_SchedulerEventKind kind;
  union {
    struct { RVS_MessageID     request_id; } request;
    struct { RVS_MessageID     request_id; RVS_Result result; } failed;
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
      RVS_SchedulerCommandKind  command_kind;
      RVS_SchedulerCommandToken command;
      RVS_Result                result;
      RVS_ProcessID             process;
      U32                       pid;
    } command_outcome;
    struct { RVS_MessageID request_id; RVS_ProgramID target; U64 execution_token; } observed;
    struct { RVS_ProgramID target; RVS_ThreadID thread; } thread_selected;
    struct { U64 sequence; } acknowledged;
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
  RVS_Plan                  *plan_first;
  RVS_Plan                  *plan_last;
  RVS_Plan                  *plan_free_first;
  RVS_SchedulerEmission     *emission_free_first;
  RVS_TargetSnapshotStorage *target_storage_free_first;
  RVS_TargetControl         *target_first;
  RVS_TargetControl         *target_last;
  RVS_EntityStore           *entities;
  U64                        next_command_id;
  U64                        next_program_id;
  U64                        next_event_sequence;
  U64                        next_stable_stop_generation;
  U64                        last_emitted_stable_stop_generation;
  U64                        next_plan_id;
  U64                        last_dispatched_stable_stop_generation;
  RVS_StableStopDispositionKind last_plan_stop_disposition;
  RVS_StableStopDispositionKind last_handler_stop_disposition;
  RVS_SchedulerCommandKind      last_stable_stop_command;
  B32                           last_stable_stop_used_fallback;
  RVS_StopPolicySource          last_stop_policy_source;
  RVS_PlanID                    decisive_plan_id;
  RVS_StopHandlerID             decisive_handler_id;
  RVS_MessageID              active_execution_request_id;
  RVS_SchedulerPhase         phase;
  RVS_RunIntent              run_intent;
  RVS_ProgramID              selected_target;
  RVS_ThreadID               selected_thread;
  RVS_StopTransaction        stop_transaction;

#if RVS_ENGINE_TESTING
  RVS_StableStopDisposition     test_handler_disposition;
  RVS_StopHandlerID             test_handler_id;
  U32                           test_plan_dispatch_count;
  U32                           test_handler_dispatch_count;
  RVS_PlanID                    test_first_plan_visited;
  RVS_PlanID                    test_second_plan_visited;
#endif
};

typedef enum
{
  RVS_SchedulerAdmissionKind_New,
  RVS_SchedulerAdmissionKind_Joined,
  RVS_SchedulerAdmissionKind_Terminal,
} RVS_SchedulerAdmissionKind;

typedef struct
{
  RVS_ScheduledOperation *operation;
  RVS_Request            *request;
  RVS_SchedulerAdmissionKind kind;
} RVS_SchedulerAdmission;

struct RVS_ScheduledOperation
{
  RVS_ScheduledOperation      *next;
  RVS_ScheduledOperation      *prev;
  RVS_ScheduledOperation      *key_next;
  RVS_ScheduledOperation      *key_prev;
  RVS_Scheduler               *scheduler;
  RVS_Request                 *request;
  RVS_TargetSnapshot          *targets;
  U64                          targets_count;
  RVS_TargetSnapshotStorage   *target_storage;
  U32                          ref_count;
  RVS_ScheduledOperationState  state;
  B32                          is_dispatched;
  B32                          is_keyed;
  B32                          request_completed;
  U64                          captured_program_state_epoch;
  RVS_RunIntent                run_intent;
  RVS_LaunchPhase              launch_phase;
  RVS_SchedulerPendingCommand  pending_command;
  RVS_PlanID                   root_plan_id;
  RVS_PlanID                   active_plan_id;
  U32                          launch_pid;
  DMN_Handle                   launch_process;
  RVS_ProgramID                launch_program_id;
  RVS_SchedulerKey             key;
};

struct RVS_RequestControl
{
  Arena             *arena;
  RVS_Session       *session;
  RVS_EngineControl *control;
  RVS_MessageID      request_id;
  B32                registered;
};

// The following scheduler mutation APIs require the owning session's control mutex.
internal B32                     rvs_scheduler_key_is_well_formed               (RVS_SchedulerKey key);
internal B32                     rvs_scheduler_key_match                        (RVS_SchedulerKey a, RVS_SchedulerKey b);
internal B32                     rvs_scheduler_keys_conflict                    (RVS_SchedulerKey a, RVS_SchedulerKey b);

internal B32                     rvs_scheduler_has_conflicting_operation_locked (RVS_Scheduler *scheduler, RVS_SchedulerKey key, RVS_ProgramID *targets, U64 targets_count);
internal void                    rvs_scheduler_target_add_locked                 (RVS_Scheduler *scheduler, RVS_ProgramID id);
internal RVS_TargetControl      *rvs_scheduler_target_from_id_locked            (RVS_Scheduler *scheduler, RVS_ProgramID id);
internal RVS_TargetControl      *rvs_scheduler_target_from_process_locked       (RVS_Scheduler *scheduler, RVS_ProcessID process);
internal void                    rvs_scheduler_release_execution_leases_locked  (RVS_Scheduler *scheduler);
internal B32                     rvs_scheduler_execution_blocks_operation_locked(RVS_Scheduler *scheduler, RVS_SchedulerOp op);
internal RVS_ScheduledOperation *rvs_scheduler_operation_alloc_locked           (RVS_Scheduler *scheduler, RVS_RequestPool *expected_pool, RVS_MessageID request_id, RVS_SchedulerKey key, RVS_ProgramID *targets, U64 targets_count, U64 captured_program_state_epoch);
internal RVS_Plan               *rvs_scheduler_attach_run_to_address_locked      (RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation, RVS_ProgramID target, U64 vaddr);

internal void                    rvs_scheduler_operation_addref                 (RVS_ScheduledOperation *operation);
internal void                    rvs_scheduler_operation_release                (RVS_ScheduledOperation *operation);

internal void                    rvs_scheduler_operation_remove_locked          (RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation);
internal RVS_ScheduledOperation *rvs_scheduler_operation_from_request_id_locked(RVS_Scheduler *scheduler, RVS_MessageID request_id);
internal void                    rvs_scheduler_prepare_reply_locked             (RVS_Scheduler *scheduler, RVS_ScheduledOperation *operation, RVS_EngineReply *reply);
internal void                    rvs_scheduler_prepare_decision_locked          (RVS_Scheduler *scheduler, RVS_SchedulerDecision *decision,
                                                                                   RVS_ProcessSnapshot *processes, U64 processes_count, Arena *arena);
internal RVS_Result              rvs_scheduler_register_operation_locked        (RVS_Scheduler *scheduler, RVS_RequestPool *expected_pool, RVS_SchedulerKey key, RVS_ScheduledOperation *operation);
internal RVS_ScheduledOperation *rvs_scheduler_unregister_operation_locked      (RVS_Scheduler *scheduler, RVS_SchedulerKey key, RVS_ScheduledOperation *expected_operation);

internal RVS_Result              rvs_scheduler_admit_locked                     (RVS_Scheduler *scheduler, RVS_RequestPool *expected_pool, U64 *next_request_id, RVS_SchedulerKey key, RVS_ProgramID *targets, U64 targets_count, U64 captured_program_state_epoch, RVS_SchedulerAdmission *admission_out);
internal void                    rvs_scheduler_rollback_admission_locked        (RVS_Scheduler *scheduler, RVS_SchedulerAdmission *admission);
internal void                    rvs_scheduler_apply_locked                     (RVS_Scheduler *scheduler, RVS_SchedulerEvent event, RVS_SchedulerDecision *decision_out);
internal void                    rvs_scheduler_decision_release                 (RVS_SchedulerDecision *decision);

