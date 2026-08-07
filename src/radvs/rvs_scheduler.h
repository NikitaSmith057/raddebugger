// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////

#pragma once
#include "radvs/rvs_core.h"
#include "radvs/rvs_request.h"
#include "radvs/rvs_demon.h"
#include "radvs/rvs_entity.h"

////////////////////////////////

typedef enum
{
  RVS_InputSourceKind_Null,

  // requests
  RVS_InputSourceKind_Command,

  // notifications
  RVS_InputSourceKind_BackendSendResult,
  RVS_InputSourceKind_BackendReply,
  RVS_InputSourceKind_BackendEvent,
  RVS_InputSourceKind_NewProgram,
} RVS_InputSourceKind;

typedef struct
{
  RVS_InputSourceKind kind;
  union {
    struct {
      RVS_MessageID request_id;
      RVS_Command   v;
    } command;
    struct {
      RVS_Result    v;
      RVS_MessageID request_id;
    } backend_send_result;
    struct {
      DMN_Event event;
    } backend_event;
    struct {
      RVS_DemonReply v;
    } backend_reply;
    RVS_ProgramID new_program;
  };
} RVS_SchedulerInput;

////////////////////////////////

typedef enum
{
  RVS_StopSource_Interrupt,
  RVS_StopSource_Exception,
  RVS_StopSource_ProcessExit,
  RVS_StopSource_ThreadExit,
  RVS_StopSource_BackendFailed,
} RVS_StopSource;

typedef struct
{
  RVS_ProgramID program;
  RVS_ProcessID process;
  RVS_ThreadID  thread;
  RVS_Epoch     epoch;
  RVS_StopCause stop_cause;
} RVS_StopState;

////////////////////////////////

typedef enum
{
  RVS_SchedulerEffectKind_Null,
  RVS_SchedulerEffectKind_SendBackendMessage,
  RVS_SchedulerEffectKind_CompleteCommand,
  RVS_SchedulerEffectKind_MakeProgram
} RVS_SchedulerEffectKind;

typedef U64 RVS_SchedulerEffectID;

typedef struct RVS_SchedulerEffect RVS_SchedulerEffect;
struct RVS_SchedulerEffect
{
  RVS_SchedulerEffect     *next;
  RVS_SchedulerEffectID    id;
  RVS_SchedulerEffectKind  kind;
  union {
    RVS_DemonMessage backend_message;
    RVS_CommandReply command_reply;
    struct { U32 pid; } make_program;
  };
};

////////////////////////////////

typedef struct RVS_Operation RVS_Operation;
typedef struct RVS_Plan      RVS_Plan;
typedef struct RVS_Scheduler RVS_Scheduler;

typedef U32 RVS_PlanState;
enum RVS_PlanState
{
  RVS_PlanState_Null,
  RVS_PlanState_Begin,
  RVS_PlanState_End,
  RVS_PlanState_UserLo,
};

typedef enum
{
  RVS_PlanStatus_Unhandled,
  RVS_PlanStatus_Handled,
  RVS_PlanStatus_AwaitEvent,
  RVS_PlanStatus_AwaitBackendReply,
} RVS_PlanStatus;

typedef struct
{
  RVS_PlanStatus       status;
  RVS_PlanState        next_state;
  RVS_SchedulerEffect *effect;
} RVS_PlanResult;

#define RVS_PLAN_FUNC(name) RVS_PlanResult name(Arena *arena, RVS_EntityStore *entity_store, RVS_Operation *operation, RVS_Plan *plan, RVS_SchedulerInput *input)
typedef RVS_PLAN_FUNC(RVS_PlanSig);

struct RVS_Plan
{
  RVS_Epoch      epoch;
  RVS_PlanState  state;
  RVS_PlanSig   *sig;
  void          *ud;
  RVS_Plan      *parent;
  RVS_Plan      *next;
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
  ArenaNode *arena_node;
  Arena     *arena; // allocated through rvs_scheduler_arena_alloc

  // alloc
  RVS_Operation *next;
  RVS_Operation *prev;

  // command
  RVS_MessageID request_id;
  RVS_Command   command;

  // scheduler
  RVS_Epoch apply_epoch;
  RVS_Plan *root_plan;
  RVS_Plan *active_plan;
};

////////////////////////////////

typedef enum
{
  RVS_BackendState_Idle,
  RVS_BackendState_Running,
  RVS_BackendState_Interrupting,
} RVS_BackendStateKind;

struct RVS_Scheduler
{
  Arena *arena;
  
  // request pool
  RVS_RequestPool *request_pool; // TODO: rename to the request store

  // execution state
  RVS_StopState          stop_state;
  RVS_Operation         *active_operation;
  RVS_Operation         *operation_first;
  RVS_Operation         *operation_last;
  RVS_SchedulerEffectID  next_effect_id;

  // backend state
  RVS_BackendStateKind  kind;
  RVS_Epoch             run_cycle_epoch;
  RVS_MessageID         active_backend_request_id;
  RVS_SchedulerEffectID active_effect_id;

  // operation state on the arenas
  ArenaNode *op_arena_first;
  ArenaNode *op_arena_last;
  ArenaNode *op_arena_free_list;

  RVS_PlanPtrNode     *free_plan_nodes;
  RVS_Operation       *free_operations;
  RVS_SchedulerEffect *free_effects;

  HashMap *plan_by_name; // (String8, RVS_Plan)
  HashMap *op_by_name;   // (RVS_Operation, RVS_Command)
};

////////////////////////////////

internal RVS_Scheduler * rvs_scheduler_init(Arena *arena, RVS_RequestPool *request_pool);
internal void            rvs_scheduler_release(RVS_Scheduler *scheduler);

internal RVS_Result            rvs_scheduler_push(RVS_Scheduler *scheduler, RVS_SchedulerInput input);
internal RVS_SchedulerEffect * rvs_scheduler_pop(Arena *arena, RVS_Scheduler *scheduler);

