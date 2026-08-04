// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/rvs_engine.h"

typedef enum
{
  RVS_EntityKind_Root,
  RVS_EntityKind_Program,
  RVS_EntityKind_Process,
  RVS_EntityKind_Thread,
  RVS_EntityKind_Module,
} RVS_EntityKind;

typedef struct RVS_Entity RVS_Entity;
typedef struct RVS_Program RVS_Program;
typedef struct RVS_Process RVS_Process;
typedef struct RVS_Thread RVS_Thread;
typedef struct RVS_Module RVS_Module;

struct RVS_Entity
{
  RVS_Entity     *next;
  RVS_Entity     *parent;
  RVS_Entity     *first_child;
  RVS_Entity     *last_child;
  RVS_EntityKind  kind;
};

struct RVS_Program
{
  RVS_Entity          entity;
  RVS_ProgramSnapshot snapshot;
};

struct RVS_Process
{
  RVS_Entity          entity;
  RVS_ProcessSnapshot snapshot;
};

struct RVS_Thread
{
  RVS_Entity         entity;
  RVS_ThreadSnapshot snapshot;
};

// Module behavior is intentionally scaffolding-only, but modules retain the
// same pointer-free model shape as other process children.
typedef struct
{
  RVS_ProcessID process;
  U64           base;
  U64           size;
} RVS_ModuleSnapshot;

struct RVS_Module
{
  RVS_Entity         entity;
  RVS_ModuleSnapshot snapshot;
};

typedef struct
{
  RVS_Entity      root;
  Arena          *arena;
  Mutex           mutex; // owned by the session control; see _locked helpers.
  CondVar         cv;
  B32             is_closed;
} RVS_EntityStore;

typedef struct
{
  RVS_ProgramID program;
  B32           program_retired;
} RVS_EntityProcessExit;

internal void rvs_entity_store_init(RVS_EntityStore *store, Arena *arena, Mutex mutex);
internal void rvs_entity_store_close(RVS_EntityStore *store);
internal void rvs_entity_store_release(RVS_EntityStore *store);

// These helpers expose pointers only while the session control mutex is held.
internal RVS_Program *rvs_entity_program_from_id_locked(RVS_EntityStore *store, RVS_ProgramID id);
internal RVS_Process *rvs_entity_process_from_id_locked(RVS_EntityStore *store, RVS_ProcessID id);
internal RVS_Thread  *rvs_entity_thread_from_id_locked (RVS_EntityStore *store, RVS_ThreadID id);
internal RVS_Program *rvs_entity_program_create_locked (RVS_EntityStore *store, RVS_ProgramID id, U32 pid);
internal RVS_Process *rvs_entity_process_create_locked (RVS_EntityStore *store, RVS_ProgramID program, RVS_ProcessID id, RVS_ProcessID parent, U32 pid);
internal RVS_EntityProcessExit rvs_entity_process_exit_locked(RVS_EntityStore *store, RVS_ProcessID id, U32 exit_code);
internal RVS_Thread  *rvs_entity_thread_create_locked  (RVS_EntityStore *store, RVS_ProcessID process, RVS_ThreadID id, U32 tid);
internal void         rvs_entity_thread_exit_locked    (RVS_EntityStore *store, RVS_ThreadID id);
internal void         rvs_entity_program_ack_destroyed_locked(RVS_EntityStore *store, RVS_ProgramID id);
internal B32          rvs_entity_live_process_for_program_locked(RVS_EntityStore *store, RVS_ProgramID id, RVS_ProcessSnapshot *snapshot_out);
internal void         rvs_entity_copy_live_processes_locked    (RVS_EntityStore *store, Arena *arena, RVS_ProcessSnapshot **snapshots_out,
                                                                 U64 *snapshots_count_out);
internal B32          rvs_entity_program_snapshot_locked(RVS_EntityStore *store, RVS_ProgramID id, RVS_ProgramSnapshot *snapshot_out);

internal RVS_Result rvs_entity_copy_programs(RVS_EntityStore *store, Arena *arena,
                                             RVS_ProgramSnapshot **snapshots_out, U64 *snapshots_count_out);
internal RVS_Result rvs_entity_fetch_program(RVS_EntityStore *store, RVS_ProgramID id, U64 wait_us, RVS_ProgramSnapshot *snapshot_out);
internal RVS_Result rvs_entity_fetch_process(RVS_EntityStore *store, RVS_ProcessID id, U64 wait_us, RVS_ProcessSnapshot *snapshot_out);
internal RVS_Result rvs_entity_fetch_thread (RVS_EntityStore *store, RVS_ThreadID id, U64 wait_us, RVS_ThreadSnapshot *snapshot_out);
