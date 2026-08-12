// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
// Includes

#pragma once

#include "base/base_core.h"
#include "base/base_strings.h"
#include "base/base_arena.h"
#include "linker/hash_table.h"

#include "radvs/rvs_core.h"
#include "radvs/rvs_request.h"
#include "radvs/rvs_backend.h"
#include "radvs/rvs_demon.h"
#include "radvs/rvs_entity.h"

////////////////////////////////

//
// Scheduler Effect
//

typedef struct RVS_Operation RVS_Operation; // owner of produced effect

typedef enum
{
  RVS_EffectKind_Null,

  RVS_EffectKind_BackendSendResult,
  RVS_EffectKind_DispatchOperation,

  RVS_EffectKind_OperationCompleted,
  RVS_EffectKind_BackendRun,
  RVS_EffectKind_SendBackendMessage,
  RVS_EffectKind_CompleteCommand,
  RVS_EffectKind_CreateProgram,
} RVS_EffectKind;

typedef union
{
  RVS_Command      command;
  RVS_DemonMessage backend_message;
  RVS_CommandReply command_reply;
  struct {
    RVS_ProgramID program;
    RVS_ProcessID process;
    RVS_ProcessID parent_process;
    U32           pid;
  } create_program;
  struct {
    RVS_ProcessID *processes;
    U64            processes_count;
  } backend_run;
} RVS_EffectValue;

typedef struct
{
  RVS_EffectID     id;
  RVS_EffectKind   kind;
  RVS_Operation   *operation;
  RVS_EffectValue  v;
} RVS_Effect;

typedef struct RVS_EffectPtrNode RVS_EffectPtrNode;
struct RVS_EffectPtrNode
{
  RVS_EffectPtrNode *next;
  RVS_Effect        *v;
};

////////////////////////////////
// Scheduler Message

typedef enum
{
  RVS_SchedulerMessageKind_Null,

  // requests
  RVS_SchedulerMessageKind_Command,

  // notifications
  RVS_SchedulerMessageKind_EffectComplete,
  RVS_SchedulerMessageKind_BackendSendResult,
  RVS_SchedulerMessageKind_BackendReply,
  RVS_SchedulerMessageKind_BackendEvent,
  RVS_SchedulerMessageKind_NewProgram,
  RVS_SchedulerMessageKind_CommandCompleteResult,
} RVS_SchedulerMessageKind;

typedef struct
{
  RVS_SchedulerMessageKind kind;
  union {
    struct {
      RVS_MessageID request_id;
      RVS_Command   v;
    } command;

    struct {
      RVS_Result    send_result;
      RVS_MessageID request_id;
    } backend_send_result;

    RVS_Event      backend_event;
    RVS_DemonReply backend_reply;

    struct {
      RVS_MessageID request_id;
      RVS_ProgramID program;
    } new_program;

    struct {
      B32         is_complete_ok;
      RVS_Effect *effect;
    } command_complete_result;
  };
} RVS_SchedulerMessage;

////////////////////////////////

typedef struct RVS_Plan      RVS_Plan;
typedef struct RVS_Scheduler RVS_Scheduler;

typedef U32 RVS_PlanState;
enum RVS_PlanState
{
  RVS_PlanState_End,
  RVS_PlanState_Begin,
  RVS_PlanState_UserLo,
};

typedef enum
{
  RVS_PlanStatus_Null,
  RVS_PlanStatus_NoStateMatch,
  RVS_PlanStatus_Ok,
} RVS_PlanStatus;

typedef enum
{
  // plan does not need the effect to complete to advance
  RVS_PlanWaitKind_Null,

  // wait for a new event from the backend
  RVS_PlanWaitKind_Event,

  // wait for the effect to complete and return the result of the effect
  RVS_PlanWaitKind_EffectCompletion,

  RVS_PlanWaitKind_BackendReply,
} RVS_PlanWaitKind;

typedef struct
{
  RVS_PlanStatus      status;
  RVS_PlanState       next_state;
  RVS_PlanWaitKind  next_wait;
  RVS_Effect         *effect;
} RVS_PlanResult;

#define RVS_PLAN_FUNC(name) RVS_PlanResult name(Arena *arena, RVS_Scheduler *sch, RVS_Operation *operation, RVS_Plan *plan, RVS_SchedulerMessage *message, U64 state)
typedef RVS_PLAN_FUNC(RVS_PlanSig);

struct RVS_Plan
{
  RVS_Plan         *parent;
  RVS_Plan         *next;
  RVS_PlanSig      *sig;
  void             *ud; // user data context for the plan callback
  RVS_PlanState     state;
  RVS_PlanWaitKind  wait;
  RVS_Effect       *wait_effect;
};

typedef struct RVS_PlanNode RVS_PlanNode;
struct RVS_PlanNode
{
  struct RVS_PlanNode *next;
  RVS_Plan v;
};

typedef struct RVS_PlanPtrNode RVS_PlanPtrNode;
struct RVS_PlanPtrNode
{
  RVS_PlanPtrNode *next;
  RVS_Plan        *v;
};

typedef struct
{
  U64           count;
  RVS_PlanNode *first;
  RVS_PlanNode *last;
} RVS_PlanList;

////////////////////////////////

struct RVS_Operation
{
  // temp memory
  ArenaNode *arena_node;
  Arena     *arena;

  // alloc
  RVS_Operation *next;
  RVS_Operation *prev;

  // command
  RVS_MessageID request_id;
  RVS_Command   command;

  // plan
  RVS_Epoch apply_epoch;
  RVS_Plan *root_plan;
  RVS_Plan *active_plan;

  RVS_EffectPtrNode *effect_first;
  RVS_EffectPtrNode *effect_last;
};

////////////////////////////////

struct RVS_Scheduler
{
  Arena *arena;
  
  // request pool
  RVS_RequestPool *request_pool; // TODO: rename to the request store
  RVS_EntityStore *entities;

  // execution state
  RVS_StopState      stop_state;
  RVS_Operation     *active_operation;
  RVS_Operation     *operation_first;
  RVS_Operation     *operation_last;
  RVS_EffectID       next_effect_id;

  // backend state
  RVS_BackendStateKind backend_state_kind;
  RVS_Epoch            run_cycle_epoch;
  RVS_MessageID        active_backend_request_id;
  RVS_EffectID         active_effect_id;

  // operation state on the arenas
  ArenaNode *op_arena_first;
  ArenaNode *op_arena_last;
  ArenaNode *op_arena_free_list;

  RVS_PlanPtrNode *free_plan_nodes;
  RVS_Operation   *free_operations;
  RVS_Effect      *free_effects;

  HashMap *plan_by_name; // (String8, RVS_Plan)
  HashMap *op_by_name;   // (RVS_Operation, RVS_Command)
};

////////////////////////////////

internal RVS_Scheduler * rvs_scheduler_init(Arena *arena, RVS_RequestPool *request_pool);
internal void            rvs_scheduler_release(RVS_Scheduler *scheduler);

internal RVS_Result   rvs_scheduler_apply(RVS_Scheduler *scheduler, RVS_EntityStore *entity_store, RVS_SchedulerMessage message);
internal RVS_Effect * rvs_scheduler_pump_effect(Arena *arena, RVS_Scheduler *scheduler);

