// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
// Includes

#pragma once
#include "base/base_core.h"
#include "base/base_arena.h"
#include "base/base_threads.h"
#include "radvs/rvs_core.h"

////////////////////////////////
// Generic Entity

typedef enum
{
  RVS_EntityKind_Root,
  RVS_EntityKind_Program,
  RVS_EntityKind_Process,
  RVS_EntityKind_Thread,
  RVS_EntityKind_Module,
  RVS_EntityKind_Count
} RVS_EntityKind;

typedef struct RVS_Program RVS_Program;
typedef struct RVS_Process RVS_Process;
typedef struct RVS_Thread  RVS_Thread;
typedef struct RVS_Module  RVS_Module;
typedef struct RVS_Entity  RVS_Entity;

typedef struct RVS_ProcessPtrNode RVS_ProcessPtrNode;
struct RVS_ProcessPtrNode
{
  RVS_Process        *v;
  RVS_ProcessPtrNode *next;
  RVS_ProcessPtrNode *prev;
};

typedef struct RVS_ThreadPtrNode RVS_ThreadPtrNode;
struct RVS_ThreadPtrNode 
{
  RVS_Thread        *v;
  RVS_ThreadPtrNode *next;
  RVS_ThreadPtrNode *prev;
};

typedef struct RVS_ModulePtrNode RVS_ModulePtrNode;
struct RVS_ModulePtrNode
{
   RVS_Module        *v;
   RVS_ModulePtrNode *next;
   RVS_ModulePtrNode *prev;
};

typedef struct RVS_ProgramPtrNode RVS_ProgramPtrNode;
struct RVS_ProgramPtrNode
{
  RVS_Program        *v;
  RVS_ProgramPtrNode *next;
  RVS_ProgramPtrNode *prev;
};

typedef struct RVS_EntityPtrNode RVS_EntityPtrNode;
struct RVS_EntityPtrNode
{
  RVS_Entity        *v;
  RVS_EntityPtrNode *next;
  RVS_EntityPtrNode *prev;
};

typedef struct
{
  U64                 count;
  RVS_ProgramPtrNode *first;
  RVS_ProgramPtrNode *last;
} RVS_ProgramPtrList;

typedef struct
{
  U64                 count;
  RVS_ProcessPtrNode *first;
  RVS_ProcessPtrNode *last;
} RVS_ProcessPtrList;

typedef struct
{
  U64                count;
  RVS_ThreadPtrNode *first;
  RVS_ThreadPtrNode *last;
} RVS_ThreadPtrList;

typedef struct
{
  U64                count;
  RVS_ModulePtrNode *first;
  RVS_ModulePtrNode *last;
} RVS_ModulePtrList;

typedef struct
{
  U64                count;
  RVS_EntityPtrNode *first;
  RVS_EntityPtrNode *last;
} RVS_EntityPtrList;

struct RVS_Program
{
  RVS_ProgramID      id;
  U32                pid;
  U32                exit_code;
  B32                is_retired;
  RVS_ProcessPtrList processes;
};

struct RVS_Process
{
  RVS_ProcessID      id;
  U32                exit_code;
  B32                is_retired;
  RVS_ThreadPtrList  threads;
  RVS_ModulePtrList  modules;
  RVS_ProcessPtrList processes;
};

struct RVS_Thread
{
  RVS_ProgramID program;
  RVS_ProcessID process;
  RVS_ThreadID  id;
  U32           tid;
  B32           is_retired;
};

struct RVS_Module
{
  RVS_ModuleID  id;
  RVS_ProcessID process;
  U64           base;
  U64           size;
};

struct RVS_Entity
{
  RVS_Entity     *parent;
  RVS_EntityKind  kind;
  union {
    U8             first;
    RVS_Program    program;
    RVS_Process    process;
    RVS_Thread     thread;
    RVS_Module     module;
  };
  union {
    RVS_ProgramPtrNode program_ptr;
    RVS_ProcessPtrNode process_ptr;
    RVS_ThreadPtrNode  thread_ptr;
    RVS_ModulePtrNode  module_ptr;
    RVS_EntityPtrNode  entity_ptr;
  };
};

#define RVS_EntityFromPtr(ptr) (RVS_Entity *)(((U8*)ptr) - OffsetOf(RVS_Entity, first))

////////////////////////////////
// Entity Store

typedef struct
{
  Arena             *arena;
  RVS_EntityPtrNode *free_list;
  HashTable         *program_by_pid; // (RVS_Program *, U32)
  HashTable         *entity_by_id[RVS_EntityKind_Count];
  U64                next_program_id;
  RVS_ProgramPtrList programs;
} RVS_EntityStore;

////////////////////////////////

internal void rvs_program_ptr_list_push_node(RVS_ProgramPtrList *list, RVS_ProgramPtrNode *n);
internal void rvs_process_ptr_list_push_node(RVS_ProcessPtrList *list, RVS_ProcessPtrNode *n);
internal void rvs_thread_ptr_list_push_node (RVS_ThreadPtrList  *list, RVS_ThreadPtrNode  *n);
internal void rvs_module_ptr_list_push_node (RVS_ModulePtrList  *list, RVS_ModulePtrNode  *n);

internal void rvs_program_ptr_list_remove_node(RVS_ProgramPtrList *list, RVS_ProgramPtrNode *n);
internal void rvs_process_ptr_list_remove_node(RVS_ProcessPtrList *list, RVS_ProcessPtrNode *n);
internal void rvs_thread_ptr_list_remove_node (RVS_ThreadPtrList  *list, RVS_ThreadPtrNode  *n);
internal void rvs_module_ptr_list_remove_node (RVS_ModulePtrList  *list, RVS_ModulePtrNode  *n);

////////////////////////////////

internal RVS_EntityStore * rvs_entity_store_alloc(void);
internal void              rvs_entity_store_release(RVS_EntityStore *store);

////////////////////////////////

internal RVS_Program * rvs_program_from_pid(RVS_EntityStore *store, U32 pid);
internal RVS_Process * rvs_process_from_id (RVS_EntityStore *store, RVS_ProcessID id);
internal RVS_Thread *  rvs_thread_from_id  (RVS_EntityStore *store, RVS_ThreadID id);
internal RVS_Module *  rvs_module_from_id  (RVS_EntityStore *store, RVS_ModuleID id);

////////////////////////////////

internal RVS_Result rvs_entity_store_apply_backend_event(Arena *arena, RVS_EntityStore *entities, DMN_Event event, RVS_EventList *events_out);

////////////////////////////////

