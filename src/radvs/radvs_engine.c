// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#include "radvs/radvs_engine.h"
#include "radvs/radvs_demon.h"
#include "radvs/radvs_request.h"

#define RADVS_SYMBOL_TICKET_POLL_US 10000

// The implementation below is the per-launch model. Keep its established
// internal names while the public API exposes the shared Engine/session split.
typedef RADVS_Engine RADVS_EngineHost;
#define RADVS_Engine                          RADVS_EngineSession
#define radvs_engine_alloc                    radvs_engine_session_alloc_for_symbol
#define radvs_engine_release                  radvs_engine_session_release__
#define radvs_engine_launch                   radvs_engine_session_launch
#define radvs_engine_launch16                 radvs_engine_session_launch16
#define radvs_engine_run                      radvs_engine_session_run
#define radvs_engine_step_thread              radvs_engine_session_step_thread
#define radvs_engine_break                    radvs_engine_session_break
#define radvs_engine_terminate                radvs_engine_session_terminate
#define radvs_engine_evaluate_expression      radvs_engine_session_evaluate_expression
#define radvs_engine_evaluate_expression16    radvs_engine_session_evaluate_expression16
#define radvs_engine_enumerate_modules        radvs_engine_session_enumerate_modules
#define radvs_engine_enumerate_threads        radvs_engine_session_enumerate_threads
#define radvs_engine_create_breakpoint        radvs_engine_session_create_breakpoint
#define radvs_engine_alloc_breakpoint         radvs_engine_session_alloc_breakpoint
#define radvs_engine_remove_breakpoint        radvs_engine_session_remove_breakpoint
#define radvs_engine_set_breakpoint_enabled   radvs_engine_session_set_breakpoint_enabled
#define radvs_engine_get_breakpoint_info      radvs_engine_session_get_breakpoint_info
#define radvs_engine_breakpoint_update_begin  radvs_engine_session_breakpoint_update_begin
#define radvs_engine_breakpoint_update_end    radvs_engine_session_breakpoint_update_end
#define radvs_engine_read_memory              radvs_engine_session_read_memory
#define radvs_engine_write_memory             radvs_engine_session_write_memory
#define radvs_engine_read_registers           radvs_engine_session_read_registers
#define radvs_engine_write_registers          radvs_engine_session_write_registers
#define radvs_engine_get_top_frame            radvs_engine_session_get_top_frame
#define radvs_engine_get_process_desc         radvs_engine_session_get_process_desc
#define radvs_engine_get_thread_desc          radvs_engine_session_get_thread_desc
#define radvs_engine_copy_threads             radvs_engine_session_copy_threads
#define radvs_engine_copy_modules             radvs_engine_session_copy_modules
#define radvs_engine_run_thread               radvs_engine_session_run_thread
#define radvs_engine_terminate_process        radvs_engine_session_terminate_process
#define radvs_engine_close_event_wait         radvs_engine_session_close_event_wait
#define radvs_engine_wait_event               radvs_engine_session_wait_event
#define radvs_engine_poll_event               radvs_engine_session_poll_event


typedef enum
{
  RADVS_EngineRequestKind_Null,
  RADVS_EngineRequestKind_Launch,
  RADVS_EngineRequestKind_Run,
  RADVS_EngineRequestKind_Step,
  RADVS_EngineRequestKind_Break,
  RADVS_EngineRequestKind_Terminate,
  RADVS_EngineRequestKind_ReadMemory,
  RADVS_EngineRequestKind_ReadRegisters,
  RADVS_EngineRequestKind_GetTopFrame,
} RADVS_EngineRequestKind;

typedef struct
{
  ProcessLaunchParams params;
  U32                 out_pid;
} RADVS_EngineLaunchRequest;

typedef struct
{
  DMN_Handle *handles;
  U64         count;
} RADVS_EngineRunRequest;

typedef struct
{
  DMN_Handle process;
  DMN_Handle thread;
} RADVS_EngineStepRequest;

typedef struct
{
  DMN_Handle process;
  U64        address;
  void      *buffer;
  U64        buffer_size;
  U64        bytes_read;
} RADVS_EngineMemoryReadRequest;

typedef struct
{
  DMN_Handle thread;
  void      *buffer;
  U64        buffer_size;
  U64        bytes_read;
} RADVS_EngineRegisterReadRequest;

typedef struct
{
  DMN_Handle       thread;
  RADVS_FrameInfo *out_frame;
  char            *source_path_buffer;
  U64              source_path_buffer_size;
  U64             *out_source_path_size;
} RADVS_EngineTopFrameRequest;

typedef struct
{
  RADVS_Request           base;
  RADVS_EngineRequestKind kind;

  // user data part of the engine request
  union {
    RADVS_EngineLaunchRequest       launch;
    RADVS_EngineRunRequest          run, terminate, processes;
    RADVS_EngineStepRequest         step;
    RADVS_EngineMemoryReadRequest   memory_read;
    RADVS_EngineRegisterReadRequest register_read;
    RADVS_EngineTopFrameRequest     top_frame;
  };
} RADVS_EngineRequest;

struct RADVS_Entity
{
  RADVS_EntityID    id;
  RADVS_EntityType  type;
  DMN_Handle        handle;
  RADVS_Entity     *parent;
  RADVS_Entity     *next;
  RADVS_Entity     *prev;
  RADVS_Entity     *free_next;
  union {
    RADVS_Root       root;
    RADVS_Process    process;
    RADVS_Thread     thread;
    RADVS_Module     module;
    RADVS_Breakpoint breakpoint;
    RADVS_Trap       trap;
  };
};

struct RADVS_Engine
{
  RADVS_EngineHost *host;
  RADVS_EngineSession *next_in_host;
  U64 id;
  Arena *arena;
  Arena *command_arena;
  Arena *incoming_event_arena;
  Arena *public_event_arena;
  Arena *entity_arena;

  // Bridge callers submit here. The engine worker remains responsive while the
  // separate demon worker is blocked in dmn_ctrl_run.
  RADVS_RequestQueue requests;
  DMN_EventList      incoming_events;
  B32                accepting_requests;
  B32                shutdown_requested;

  Mutex                 public_event_mutex;
  CondVar               public_event_available_cv;
  RADVS_EventList       public_events;
  B32                   event_wait_closed;

  RWMutex         model_mutex;
  RADVS_Entity   *root;
  RADVS_Entity   *free_entity;
  RADVS_EntityID  next_entity_id;
  HashMap         entity_by_id;
  HashMap         entity_by_handle;
  U32             breakpoint_update_depth;
  U64             breakpoint_generation;
  DMN_Trap       *trap_snapshot;
  U64             trap_snapshot_count;
  U64             trap_snapshot_generation;

  Thread              worker;
  RADVS_DemonClient *demon;
  RADVS_SymbolService *symbol_service;
};

internal DMN_EventNode *
radvs_engine_event_alloc(RADVS_Engine *engine)
{
  return push_array(engine->incoming_event_arena, DMN_EventNode, 1);
}

internal void
radvs_engine_event_copy(RADVS_Engine *engine, DMN_EventNode *dst, const DMN_Event *src)
{
  dst->v = *src;
  dst->v.string = push_str8_copy(engine->incoming_event_arena, src->string);
  if (src->module_info && src->module_info != &dmn_module_info_nil) {
    dst->v.module_info                  = push_array(engine->incoming_event_arena, DMN_ModuleInfo, 1);
    MemoryCopyStruct(dst->v.module_info, src->module_info);
    dst->v.module_info->module_path     = push_str8_copy(engine->incoming_event_arena, src->module_info->module_path);
    dst->v.module_info->debug_info_path = push_str8_copy(engine->incoming_event_arena, src->module_info->debug_info_path);
  }
}

internal void
radvs_dmn_event_list_push(DMN_EventList *list, DMN_EventNode *node)
{
  node->next = 0;
  SLLQueuePush(list->first, list->last, node);
  list->count += 1;
}

internal void
radvs_engine_event_list_push_copy(RADVS_Engine *engine, RADVS_EventList *list, const RADVS_Event *event)
{
  RADVS_EventNode *node = push_array(engine->public_event_arena, RADVS_EventNode, 1);
  node->v = *event;
  radvs_event_list_push(list, node);
}

// called from the demon thread to copy out events
internal void
radvs_engine_demon_event(void *user_data, const DMN_Event *event)
{
  RADVS_Engine *engine = user_data;
  MutexScope (engine->requests.mutex) {
    DMN_EventNode *node = radvs_engine_event_alloc(engine); // alloc new event node
    radvs_engine_event_copy(engine, node, event);           // copy out event
    radvs_dmn_event_list_push(&engine->incoming_events, node); // push event copy to the 'incoming events' list
    cond_var_broadcast(engine->requests.available_cv);      // notify the engine thread that a new event is available
  }
}

internal RADVS_EngineRequest *
radvs_engine_request_alloc_locked(RADVS_Engine *engine, RADVS_EngineRequestKind kind)
{
  RADVS_EngineRequest *request = radvs_request_queue_alloc_locked_struct(&engine->requests, RADVS_EngineRequest);
  request->kind = kind;
  return request;
}

internal RADVS_Result
radvs_engine_submit(RADVS_Engine *engine, RADVS_EngineRequest *request)
{
  RADVS_Result result = RADVS_Result_Busy;
  MutexScope (engine->requests.mutex) {
    if (engine->accepting_requests) {
      result = radvs_request_queue_submit_locked(&engine->requests, &request->base);
    }
  }
  return result;
}

internal void
radvs_engine_request_recycle(RADVS_Engine *engine, RADVS_EngineRequest *request)
{
  MutexScope (engine->requests.mutex) {
    radvs_request_queue_recycle_locked(&engine->requests, &request->base);
  }
}

internal RADVS_Entity *
radvs_entity_from_handle(RADVS_Engine *engine, RADVS_EntityType expected_type, DMN_Handle handle)
{
  if (MemoryIsZeroStruct(&handle)) {
    return 0;
  }
  RADVS_Entity *entity = hash_map_search_u64_raw(&engine->entity_by_handle, handle.u64[0]);
  return entity && entity->type == expected_type && dmn_handle_match(entity->handle, handle) ? entity : 0;
}

internal RADVS_Entity *
radvs_entity_from_id(RADVS_Engine *engine, RADVS_EntityType expected_type, RADVS_EntityID id)
{
  if (id == 0) {
    return 0;
  }
  RADVS_Entity *entity = hash_map_search_u64_raw(&engine->entity_by_id, id);
  return entity && entity->type == expected_type ? entity : 0;
}

internal RADVS_Entity *
radvs_entity_alloc(RADVS_Engine *engine)
{
  RADVS_Entity *entity = engine->free_entity;
  if (entity) {
    engine->free_entity = entity->free_next;
    MemoryZeroStruct(entity);
  } else {
    entity = push_array(engine->entity_arena, RADVS_Entity, 1);
  }
  return entity;
}

internal void
radvs_entity_list_push(RADVS_EntityList *list, RADVS_Entity *entity)
{
  entity->prev = list->last;
  entity->next = 0;
  if (list->last != 0) {
    list->last->next = entity;
  } else {
    list->first = entity;
  }
  list->last = entity;
}

internal RADVS_EntityList *
radvs_entity_child_list(RADVS_Entity *parent, RADVS_Entity *child)
{
  if (parent == 0) {
    return 0;
  }
  switch (parent->type) {
  case RADVS_EntityType_Root: {
    switch (child->type) {
    case RADVS_EntityType_Process: return &parent->root.processes;
    case RADVS_EntityType_Breakpoint: {
      return child->breakpoint.visibility == RADVS_BreakpointVisibility_Internal ?
             &parent->root.internal_breakpoints : &parent->root.user_breakpoints;
    } break;
    case RADVS_EntityType_Trap: return &parent->root.traps;
    default:                    return 0;
    }
  } break;

  case RADVS_EntityType_Process: {
    switch (child->type) {
    case RADVS_EntityType_Process: return &parent->process.child_processes;
    case RADVS_EntityType_Thread:  return &parent->process.threads;
    case RADVS_EntityType_Module:  return &parent->process.modules;
    default:                       return 0;
    }
  } break;

  case RADVS_EntityType_Breakpoint: {
    if (parent->breakpoint.info.spec.location_kind == RADVS_BreakpointLocationKind_Source &&
        child->type == RADVS_EntityType_Breakpoint &&
        child->breakpoint.info.spec.location_kind == RADVS_BreakpointLocationKind_Address) {
      return &parent->breakpoint.address_breakpoints;
    }
  } break;

  default: return 0;
  }
  return 0;
}

internal void
radvs_entity_link(RADVS_Entity *parent, RADVS_Entity *entity)
{
  RADVS_EntityList *list = radvs_entity_child_list(parent, entity);
  Assert(list != 0);
  entity->parent = parent;
  radvs_entity_list_push(list, entity);
}

internal void
radvs_entity_unlink(RADVS_Entity *entity)
{
  RADVS_Entity *parent = entity->parent;
  if (parent) {
    RADVS_EntityList *list = radvs_entity_child_list(parent, entity);
    Assert(list != 0);
    if (entity->prev) { entity->prev->next = entity->next; }
    else              { list->first = entity->next;        }
    if (entity->next) { entity->next->prev = entity->prev; }
    else              { list->last = entity->prev;         }
  }
  entity->parent = 0;
  entity->next = 0;
  entity->prev = 0;
}

internal void
radvs_entity_reparent(RADVS_Entity *entity, RADVS_Entity *parent)
{
  if (entity->parent != parent) {
    radvs_entity_unlink(entity);
    radvs_entity_link(parent, entity);
  }
}

internal RADVS_Entity *
radvs_entity_create(RADVS_Engine *engine, RADVS_Entity *parent, RADVS_EntityType type, DMN_Handle handle)
{
  RADVS_Entity *existing = radvs_entity_from_handle(engine, type, handle);
  if (existing) {
    return existing;
  }

  RADVS_Entity *entity = radvs_entity_alloc(engine);
  entity->id     = engine->next_entity_id++;
  entity->type   = type;
  entity->handle = handle;
  radvs_entity_link(parent, entity);
  hash_map_push_u64_raw(engine->entity_arena, &engine->entity_by_id, entity->id, entity);
  if (!MemoryIsZeroStruct(&handle)) {
    hash_map_push_u64_raw(engine->entity_arena, &engine->entity_by_handle, handle.u64[0], entity);
  }
  return entity;
}

internal void radvs_breakpoint_binding_release_locked(RADVS_Engine *engine, RADVS_BreakpointBinding *binding);

internal void
radvs_entity_release(RADVS_Engine *engine, RADVS_Entity *entity)
{
  RADVS_EntityList *lists[4] = {0};
  U64 list_count = 0;
  switch (entity->type) {
  case RADVS_EntityType_Root: {
    lists[list_count++] = &entity->root.processes;
    lists[list_count++] = &entity->root.user_breakpoints;
    lists[list_count++] = &entity->root.internal_breakpoints;
    lists[list_count++] = &entity->root.traps;
  } break;
  case RADVS_EntityType_Process: {
    lists[list_count++] = &entity->process.child_processes;
    lists[list_count++] = &entity->process.threads;
    lists[list_count++] = &entity->process.modules;
  } break;
  case RADVS_EntityType_Breakpoint: {
    lists[list_count++] = &entity->breakpoint.address_breakpoints;
  } break;
  default: break;
  }
  for EachIndex(list_index, list_count) {
    for (RADVS_Entity *child = lists[list_index]->first; child != 0;) {
      RADVS_Entity *next = child->next;
      radvs_entity_release(engine, child);
      child = next;
    }
  }
  if (entity->type == RADVS_EntityType_Module && entity->module.symbol_ticket) {
    radvs_symbol_ticket_cancel(entity->module.symbol_ticket);
    radvs_symbol_ticket_release(entity->module.symbol_ticket);
  }
  if (entity->type == RADVS_EntityType_Breakpoint) {
    for (RADVS_BreakpointBinding *binding = entity->breakpoint.first_binding; binding != 0;) {
      RADVS_BreakpointBinding *next = binding->next;
      radvs_breakpoint_binding_release_locked(engine, binding);
      binding = next;
    }
  }
  radvs_entity_unlink(entity);
  hash_map_purge_u64(&engine->entity_by_id, entity->id);
  if (!MemoryIsZeroStruct(&entity->handle)) {
    hash_map_purge_u64(&engine->entity_by_handle, entity->handle.u64[0]);
  }
  MemoryZeroStruct(entity);
  entity->free_next   = engine->free_entity;
  engine->free_entity = entity;
}

internal void radvs_engine_unbind_process_locked(RADVS_Engine *engine, DMN_Handle process);
internal RADVS_Result radvs_engine_update_demon_traps_locked(RADVS_Engine *engine);

internal void
radvs_process_exit(RADVS_Engine *engine, RADVS_Entity *process)
{
  // A tracked child process outlives its parent as a top-level session target.
  radvs_engine_unbind_process_locked(engine, process->handle);
  radvs_engine_update_demon_traps_locked(engine);
  for (RADVS_Entity *child = process->process.child_processes.first; child != 0;) {
    RADVS_Entity *next = child->next;
    radvs_entity_reparent(child, engine->root);
    child = next;
  }
  for (RADVS_Entity *thread = process->process.threads.first; thread != 0;) {
    RADVS_Entity *next = thread->next;
    radvs_entity_release(engine, thread);
    thread = next;
  }
  for (RADVS_Entity *module = process->process.modules.first; module != 0;) {
    RADVS_Entity *next = module->next;
    radvs_entity_release(engine, module);
    module = next;
  }
  radvs_entity_release(engine, process);
}

internal RADVS_Entity *
radvs_engine_provisional_process_locked(RADVS_Engine *engine, DMN_Handle handle)
{
  RADVS_Entity *process = radvs_entity_from_handle(engine, RADVS_EntityType_Process, handle);
  if (process == 0 && !MemoryIsZeroStruct(&handle)) {
    process = radvs_entity_create(engine, engine->root, RADVS_EntityType_Process, handle);
    process->process.state = RADVS_ProcessState_Launching;
  }
  return process;
}

internal void
radvs_engine_reparent_waiting_processes_recursive_locked(RADVS_Entity *entity, RADVS_Entity *parent)
{
  RADVS_EntityList *processes = entity->type == RADVS_EntityType_Root ? &entity->root.processes :
                                entity->type == RADVS_EntityType_Process ? &entity->process.child_processes : 0;
  for (RADVS_Entity *child = processes ? processes->first : 0; child != 0;) {
    RADVS_Entity *next = child->next;
    if (child != parent &&
        dmn_handle_match(child->process.parent_process, parent->handle)) {
      radvs_entity_reparent(child, parent);
    }
    radvs_engine_reparent_waiting_processes_recursive_locked(child, parent);
    child = next;
  }
}

internal void
radvs_engine_reparent_waiting_processes_locked(RADVS_Engine *engine, RADVS_Entity *parent)
{
  radvs_engine_reparent_waiting_processes_recursive_locked(engine->root, parent);
}

internal U64
radvs_engine_process_count_recursive(RADVS_Entity *process)
{
  U64 count = 1;
  for EachNode(child, RADVS_Entity, process->process.child_processes.first) {
    count += radvs_engine_process_count_recursive(child);
  }
  return count;
}

internal U64
radvs_engine_process_count_all(RADVS_Engine *engine)
{
  U64 count = 0;
  for EachNode(process, RADVS_Entity, engine->root->root.processes.first) {
    count += radvs_engine_process_count_recursive(process);
  }
  return count;
}

inline void radvs_engine_mark_process_running(RADVS_Entity *process);

internal void
radvs_engine_copy_threads_recursive(RADVS_Entity *process, RADVS_ThreadDesc *buffer, U64 *index)
{
  for EachNode(thread, RADVS_Entity, process->process.threads.first) {
    buffer[(*index)++] = (RADVS_ThreadDesc){
      .thread_handle       = thread->handle,
      .process_handle      = process->handle,
      .process_state_epoch = process->process.process_state_epoch,
      .system_thread_id    = thread->thread.system_thread_id,
      .state               = thread->thread.state,
    };
  }
  for EachNode(child, RADVS_Entity, process->process.child_processes.first) {
    radvs_engine_copy_threads_recursive(child, buffer, index);
  }
}

internal void
radvs_engine_copy_modules_recursive(RADVS_Entity *process, RADVS_ModuleDesc *buffer, U64 *index)
{
  for EachNode(module, RADVS_Entity, process->process.modules.first) {
    buffer[(*index)++] = (RADVS_ModuleDesc){
      .module_handle  = module->handle,
      .process_handle = process->handle,
      .base_address   = module->module.base_address,
      .size           = module->module.size,
      .symbol_state   = module->module.symbol_state,
    };
  }
  for EachNode(child, RADVS_Entity, process->process.child_processes.first) {
    radvs_engine_copy_modules_recursive(child, buffer, index);
  }
}

internal void
radvs_engine_mark_processes_running_recursive(RADVS_Entity *process)
{
  radvs_engine_mark_process_running(process);
  for EachNode(child, RADVS_Entity, process->process.child_processes.first) {
    radvs_engine_mark_processes_running_recursive(child);
  }
}

internal U64
radvs_engine_thread_count_recursive(RADVS_Entity *process)
{
  U64 count = 0;
  for EachNode(thread, RADVS_Entity, process->process.threads.first) {
    count += 1;
  }
  for EachNode(child, RADVS_Entity, process->process.child_processes.first) {
    count += radvs_engine_thread_count_recursive(child);
  }
  return count;
}

internal U64
radvs_engine_module_count_recursive(RADVS_Entity *process)
{
  U64 count = 0;
  for EachNode(module, RADVS_Entity, process->process.modules.first) {
    count += 1;
  }
  for EachNode(child, RADVS_Entity, process->process.child_processes.first) {
    count += radvs_engine_module_count_recursive(child);
  }
  return count;
}

internal void
radvs_breakpoint_refresh_info_locked(RADVS_Entity *breakpoint)
{
  RADVS_Breakpoint *bp = &breakpoint->breakpoint;
  bp->info.binding_count = 0;
  bp->info.enabled_binding_count = 0;
  bp->info.binding_result = RADVS_Result_Ok;
  for EachNode(binding, RADVS_BreakpointBinding, bp->first_binding) {
    bp->info.binding_count += 1;
    bp->info.enabled_binding_count += binding->enabled;
    if (binding->result != RADVS_Result_Ok) {
      bp->info.binding_result = binding->result;
    }
  }
}

internal RADVS_Entity *
radvs_engine_trap_from_site_locked(RADVS_Engine *engine, DMN_Handle process, U64 address)
{
  for EachNode(trap, RADVS_Entity, engine->root->root.traps.first) {
    if (dmn_handle_match(trap->trap.dmn.process, process) && trap->trap.dmn.vaddr == address) {
      return trap;
    }
  }
  return 0;
}

internal void
radvs_trap_refresh_mode_locked(RADVS_Entity *trap)
{
  RADVS_Trap *site = &trap->trap;
  B32 has_software = 0;
  B32 has_hardware = 0;
  site->enabled_binding_count = 0;
  site->binding_result = RADVS_Result_Ok;
  for (RADVS_BreakpointBinding *binding = site->first_binding; binding != 0; binding = binding->next_in_trap) {
    if (binding->enabled) {
      site->enabled_binding_count += 1;
      has_software |= binding->requested_mode == RADVS_AddressBreakpointMode_Software;
      has_hardware |= binding->requested_mode == RADVS_AddressBreakpointMode_Hardware;
    }
  }
  if (has_software && has_hardware) {
    site->binding_result = RADVS_Result_Conflict;
  }
  site->effective_mode = has_hardware ? RADVS_AddressBreakpointMode_Hardware : RADVS_AddressBreakpointMode_Software;
  site->dmn.flags = DMN_TrapFlag_BreakOnExecute;
  site->changed_generation += 1;
  for (RADVS_BreakpointBinding *binding = site->first_binding; binding != 0; binding = binding->next_in_trap) {
    binding->result = site->binding_result;
    radvs_breakpoint_refresh_info_locked(binding->breakpoint);
  }
}

internal RADVS_Entity *
radvs_engine_trap_create_locked(RADVS_Engine *engine, DMN_Handle process, U64 address)
{
  RADVS_Entity *trap = radvs_entity_create(engine, engine->root, RADVS_EntityType_Trap, dmn_handle_zero());
  trap->trap.dmn = (DMN_Trap){
    .process = process,
    .vaddr   = address,
    .id      = trap->id,
    .size    = 1,
  };
  trap->trap.effective_mode = RADVS_AddressBreakpointMode_Software;
  trap->trap.binding_result = RADVS_Result_Ok;
  return trap;
}

internal void
radvs_breakpoint_binding_link_locked(RADVS_Entity *breakpoint, RADVS_Entity *trap, RADVS_BreakpointBinding *binding)
{
  binding->breakpoint = breakpoint;
  binding->trap = trap;
  binding->next = 0;
  if (breakpoint->breakpoint.last_binding) {
    breakpoint->breakpoint.last_binding->next = binding;
  } else {
    breakpoint->breakpoint.first_binding = binding;
  }
  breakpoint->breakpoint.last_binding = binding;
  binding->next_in_trap = 0;
  if (trap->trap.last_binding) {
    trap->trap.last_binding->next_in_trap = binding;
  } else {
    trap->trap.first_binding = binding;
  }
  trap->trap.last_binding = binding;
  trap->trap.binding_count += 1;
}

internal void
radvs_breakpoint_binding_unlink_locked(RADVS_BreakpointBinding *binding)
{
  RADVS_Entity *breakpoint = binding->breakpoint;
  RADVS_Entity *trap = binding->trap;
  RADVS_BreakpointBinding *previous = 0;
  for EachNode(it, RADVS_BreakpointBinding, breakpoint->breakpoint.first_binding) {
    if (it == binding) {
      if (previous) { previous->next = binding->next; }
      else          { breakpoint->breakpoint.first_binding = binding->next; }
      if (breakpoint->breakpoint.last_binding == binding) {
        breakpoint->breakpoint.last_binding = previous;
      }
      break;
    }
    previous = it;
  }
  previous = 0;
  for (RADVS_BreakpointBinding *it = trap->trap.first_binding; it != 0; it = it->next_in_trap) {
    if (it == binding) {
      if (previous) { previous->next_in_trap = binding->next_in_trap; }
      else          { trap->trap.first_binding = binding->next_in_trap; }
      if (trap->trap.last_binding == binding) {
        trap->trap.last_binding = previous;
      }
      break;
    }
    previous = it;
  }
  trap->trap.binding_count -= 1;
  binding->breakpoint = 0;
  binding->trap = 0;
  binding->next = 0;
  binding->next_in_trap = 0;
}

internal void
radvs_breakpoint_binding_release_locked(RADVS_Engine *engine, RADVS_BreakpointBinding *binding)
{
  RADVS_Entity *breakpoint = binding->breakpoint;
  RADVS_Entity *trap = binding->trap;
  radvs_breakpoint_binding_unlink_locked(binding);
  radvs_breakpoint_refresh_info_locked(breakpoint);
  radvs_trap_refresh_mode_locked(trap);
  if (trap->trap.binding_count == 0) {
    radvs_entity_release(engine, trap);
  }
}

internal RADVS_Result
radvs_engine_bind_breakpoint_to_process_locked(RADVS_Engine *engine, RADVS_Entity *breakpoint, RADVS_Entity *process)
{
  RADVS_Breakpoint *bp = &breakpoint->breakpoint;
  if (bp->info.spec.location_kind != RADVS_BreakpointLocationKind_Address ||
      bp->info.spec.condition_kind != RADVS_BreakpointConditionKind_Always ||
      bp->info.spec.address == 0) {
    return RADVS_Result_NotImplemented;
  }
  for EachNode(binding, RADVS_BreakpointBinding, bp->first_binding) {
    if (dmn_handle_match(binding->trap->trap.dmn.process, process->handle)) {
      return binding->result;
    }
  }
  RADVS_Entity *trap = radvs_engine_trap_from_site_locked(engine, process->handle, bp->info.spec.address);
  if (trap == 0) {
    trap = radvs_engine_trap_create_locked(engine, process->handle, bp->info.spec.address);
  }
  if (bp->info.spec.enabled &&
      ((bp->info.spec.address_mode == RADVS_AddressBreakpointMode_Software && trap->trap.effective_mode == RADVS_AddressBreakpointMode_Hardware) ||
       (bp->info.spec.address_mode == RADVS_AddressBreakpointMode_Hardware && trap->trap.effective_mode == RADVS_AddressBreakpointMode_Software && trap->trap.enabled_binding_count != 0))) {
    return RADVS_Result_Conflict;
  }
  RADVS_BreakpointBinding *binding = push_array(engine->entity_arena, RADVS_BreakpointBinding, 1);
  binding->enabled = bp->info.spec.enabled;
  binding->requested_mode = bp->info.spec.address_mode;
  radvs_breakpoint_binding_link_locked(breakpoint, trap, binding);
  radvs_trap_refresh_mode_locked(trap);
  radvs_breakpoint_refresh_info_locked(breakpoint);
  return binding->result;
}

internal RADVS_Result
radvs_breakpoint_set_enabled_locked(RADVS_Entity *breakpoint, B32 enabled)
{
  RADVS_Breakpoint *bp = &breakpoint->breakpoint;
  if (bp->info.spec.enabled == enabled) {
    return bp->info.binding_result;
  }
  if (enabled) {
    for EachNode(binding, RADVS_BreakpointBinding, bp->first_binding) {
      for (RADVS_BreakpointBinding *other = binding->trap->trap.first_binding; other != 0; other = other->next_in_trap) {
        if (other != binding && other->enabled && other->requested_mode != binding->requested_mode) {
          return RADVS_Result_Conflict;
        }
      }
    }
  }
  bp->info.spec.enabled = enabled;
  for EachNode(binding, RADVS_BreakpointBinding, bp->first_binding) {
    binding->enabled = enabled;
    radvs_trap_refresh_mode_locked(binding->trap);
  }
  radvs_breakpoint_refresh_info_locked(breakpoint);
  return RADVS_Result_Ok;
}

internal RADVS_Result
radvs_engine_bind_breakpoint_to_processes_recursive_locked(RADVS_Engine *engine, RADVS_Entity *breakpoint, RADVS_Entity *process)
{
  RADVS_Result result = radvs_engine_bind_breakpoint_to_process_locked(engine, breakpoint, process);
  for EachNode(child, RADVS_Entity, process->process.child_processes.first) {
    if (result == RADVS_Result_Ok) {
      result = radvs_engine_bind_breakpoint_to_processes_recursive_locked(engine, breakpoint, child);
    }
  }
  return result;
}

internal void
radvs_engine_bind_breakpoint_list_to_process_locked(RADVS_Engine *engine, RADVS_EntityList *list, RADVS_Entity *process)
{
  for EachNode(breakpoint, RADVS_Entity, list->first) {
    if (breakpoint->breakpoint.info.spec.location_kind == RADVS_BreakpointLocationKind_Address) {
      radvs_engine_bind_breakpoint_to_process_locked(engine, breakpoint, process);
    }
    radvs_engine_bind_breakpoint_list_to_process_locked(engine, &breakpoint->breakpoint.address_breakpoints, process);
  }
}

internal void
radvs_engine_bind_all_breakpoints_to_process_locked(RADVS_Engine *engine, RADVS_Entity *process)
{
  radvs_engine_bind_breakpoint_list_to_process_locked(engine, &engine->root->root.user_breakpoints, process);
  radvs_engine_bind_breakpoint_list_to_process_locked(engine, &engine->root->root.internal_breakpoints, process);
}

internal void
radvs_engine_unbind_process_in_breakpoint_list_locked(RADVS_Engine *engine, RADVS_EntityList *list, DMN_Handle process)
{
  for EachNode(breakpoint, RADVS_Entity, list->first) {
    for (RADVS_BreakpointBinding *binding = breakpoint->breakpoint.first_binding; binding != 0;) {
      RADVS_BreakpointBinding *next = binding->next;
      if (dmn_handle_match(binding->trap->trap.dmn.process, process)) {
        radvs_breakpoint_binding_release_locked(engine, binding);
      }
      binding = next;
    }
    radvs_engine_unbind_process_in_breakpoint_list_locked(engine, &breakpoint->breakpoint.address_breakpoints, process);
  }
}

internal void
radvs_engine_unbind_process_locked(RADVS_Engine *engine, DMN_Handle process)
{
  radvs_engine_unbind_process_in_breakpoint_list_locked(engine, &engine->root->root.user_breakpoints, process);
  radvs_engine_unbind_process_in_breakpoint_list_locked(engine, &engine->root->root.internal_breakpoints, process);
}

internal U64
radvs_engine_enabled_trap_count_locked(RADVS_Engine *engine)
{
  U64 count = 0;
  for EachNode(trap, RADVS_Entity, engine->root->root.traps.first) {
    count += trap->trap.enabled_binding_count != 0 && trap->trap.binding_result == RADVS_Result_Ok;
  }
  return count;
}

internal void
radvs_engine_copy_enabled_traps_locked(RADVS_Engine *engine, DMN_Trap *traps)
{
  U64 index = 0;
  for EachNode(trap, RADVS_Entity, engine->root->root.traps.first) {
    if (trap->trap.enabled_binding_count != 0 && trap->trap.binding_result == RADVS_Result_Ok) {
      traps[index++] = trap->trap.dmn;
    }
  }
}

internal RADVS_Result
radvs_engine_update_demon_traps_locked(RADVS_Engine *engine)
{
  U64 trap_count = radvs_engine_enabled_trap_count_locked(engine);
  engine->trap_snapshot = trap_count != 0 ? push_array_no_zero(engine->entity_arena, DMN_Trap, trap_count) : 0;
  engine->trap_snapshot_count = trap_count;
  if (engine->trap_snapshot != 0) {
    radvs_engine_copy_enabled_traps_locked(engine, engine->trap_snapshot);
  }
  engine->trap_snapshot_generation += 1;
  return radvs_demon_update_traps(engine->demon, engine->trap_snapshot, engine->trap_snapshot_count);
}

internal B32
radvs_engine_source_location_from_process_locked(RADVS_Entity *process, U64 address, RADVS_SymbolLocation *out_location)
{
  for EachNode(module, RADVS_Entity, process->process.modules.first) {
    if (address >= module->module.base_address &&
        address - module->module.base_address < module->module.size &&
        module->module.symbol_state == RADVS_SymbolTicketState_Ready &&
        module->module.symbol_ticket != 0 &&
        radvs_symbol_ticket_location_from_voff(module->module.symbol_ticket,
                                                address - module->module.base_address,
                                                out_location) == RADVS_Result_Ok &&
        out_location->line != 0 && out_location->source_path.size != 0) {
      return 1;
    }
  }
  for EachNode(child, RADVS_Entity, process->process.child_processes.first) {
    if (radvs_engine_source_location_from_process_locked(child, address, out_location)) {
      return 1;
    }
  }
  return 0;
}

internal RADVS_Entity *
radvs_engine_source_group_from_location_locked(RADVS_Engine *engine, String8 path, U32 line, U32 column)
{
  Temp scratch = scratch_begin(0, 0);
  String8 normalized_path = path_normalized_from_string(scratch.arena, path);
  RADVS_Entity *result = 0;
  for EachNode(group, RADVS_Entity, engine->root->root.user_breakpoints.first) {
    RADVS_BreakpointSpec *spec = &group->breakpoint.info.spec;
    if (spec->location_kind == RADVS_BreakpointLocationKind_Source &&
        spec->source.line == line &&
        spec->source.column == column &&
        str8_match(spec->source.path, normalized_path, StringMatchFlag_CaseInsensitive)) {
      result = group;
      break;
    }
  }
  if (result == 0) {
    RADVS_Entity *group = radvs_entity_create(engine, engine->root, RADVS_EntityType_Breakpoint, dmn_handle_zero());
    group->breakpoint.visibility = RADVS_BreakpointVisibility_User;
    group->breakpoint.info.breakpoint_id = group->id;
    group->breakpoint.info.spec = (RADVS_BreakpointSpec){
      .location_kind = RADVS_BreakpointLocationKind_Source,
      .condition_kind = RADVS_BreakpointConditionKind_Always,
      .enabled = 1,
      .source = {
        .path = path_normalized_from_string(engine->entity_arena, path),
        .line = line,
        .column = column,
      },
    };
    group->breakpoint.info.binding_result = RADVS_Result_Ok;
    result = group;
  }
  scratch_end(scratch);
  return result;
}

internal void
radvs_engine_project_address_breakpoints_locked(RADVS_Engine *engine)
{
  for (RADVS_Entity *breakpoint = engine->root->root.user_breakpoints.first; breakpoint != 0;) {
    RADVS_Entity *next = breakpoint->next;
    RADVS_Breakpoint *bp = &breakpoint->breakpoint;
    if (bp->info.spec.location_kind == RADVS_BreakpointLocationKind_Address && bp->source_projection_pending) {
      RADVS_SymbolLocation location = {0};
      for EachNode(process, RADVS_Entity, engine->root->root.processes.first) {
        if (radvs_engine_source_location_from_process_locked(process, bp->info.spec.address, &location)) {
          break;
        }
      }
      if (location.source_path.size != 0) {
        RADVS_Entity *group = radvs_engine_source_group_from_location_locked(engine, location.source_path, location.line, location.column);
        radvs_entity_reparent(breakpoint, group);
        bp->source_projection_pending = 0;
      }
    }
    breakpoint = next;
  }
}

internal RADVS_SymbolTicket *
radvs_engine_queue_symbol_ticket_locked(RADVS_Engine *engine, RADVS_Entity *module_entity)
{
  RADVS_Module *module = &module_entity->module;
  if (module->symbol_ticket) {
    return module->symbol_ticket;
  }

  RADVS_SymbolRequest request = {0};
  request.module_path          = module->path;
  request.debug_info_path      = module->debug_info_path;
  request.debug_info_guid      = module->debug_info_guid;
  request.debug_info_timestamp = module->debug_info_timestamp;
  request.debug_info_age       = module->debug_info_age;
  request.endt_us              = now_time_us() + 30000000;
  RADVS_SymbolTicket *ticket   = 0;
  RADVS_Result result          = radvs_symbol_ticket_submit(engine->symbol_service, request, &ticket);
  module->symbol_ticket        = ticket;
  module->symbol_state         = result == RADVS_Result_Ok ? RADVS_SymbolTicketState_Queued : RADVS_SymbolTicketState_Failed;
  return ticket;
}

internal B32
radvs_engine_reduce_event_locked(RADVS_Engine *engine, const DMN_Event *event, RADVS_Event *out_event)
{
  MemoryZeroStruct(out_event);
  out_event->raw = *event;
  DMN_Handle event_process = event->process;
  DMN_Handle event_thread = event->thread;
  DMN_Handle event_parent_process = event->parent_process;
  RADVS_Entity *event_process_entity = 0;
  if (event->kind != DMN_EventKind_CreateProcess && !MemoryIsZeroStruct(&event_process)) {
    event_process_entity = radvs_engine_provisional_process_locked(engine, event_process);
    if (event->system_process_id != 0) {
      event_process_entity->process.system_process_id = event->system_process_id;
    }
  }
  switch (event->kind) {
  case DMN_EventKind_Null: break;

  case DMN_EventKind_Error: NotImplemented; break;
  case DMN_EventKind_HandshakeComplete: NotImplemented; break;
  case DMN_EventKind_ModuleDebugInfo: NotImplemented; break;
  case DMN_EventKind_Memory: NotImplemented; break;
  case DMN_EventKind_DebugString: break;
  case DMN_EventKind_SetThreadName: NotImplemented; break;
  case DMN_EventKind_SetThreadColor: NotImplemented; break;
  case DMN_EventKind_SetBreakpoint: NotImplemented; break;
  case DMN_EventKind_SetVAddrRangeNote: NotImplemented; break;
  case DMN_EventKind_UnsetBreakpoint: NotImplemented; break;
  case DMN_EventKind_COUNT: InvalidPath; break;

  case DMN_EventKind_CreateProcess: {
    RADVS_Entity *parent = radvs_entity_from_handle(engine, RADVS_EntityType_Process, event_parent_process);
    if (!parent && !MemoryIsZeroStruct(&event_parent_process)) {
      log_infof("radvs: CreateProcess parent unavailable: parent:[0x%I64x], process:[0x%I64x]\n", event->parent_process.u64[0], event->process.u64[0]);
    }
    RADVS_Entity *entity = radvs_entity_from_handle(engine, RADVS_EntityType_Process, event->process);
    B32 is_new_entity = entity == 0;
    if (is_new_entity) {
      entity = radvs_entity_create(engine, parent ? parent : engine->root, RADVS_EntityType_Process, event->process);
    }
    B32 first_create_process = !entity->process.create_process_received;
    radvs_entity_reparent(entity, parent ? parent : engine->root);
    if (is_new_entity) {
      entity->process.state = RADVS_ProcessState_Launching;
    }
    if (event->system_process_id != 0) {
      entity->process.system_process_id = event->system_process_id;
    }
    entity->process.parent_process = event_parent_process;
    entity->process.create_process_received = 1;
    radvs_engine_bind_all_breakpoints_to_process_locked(engine, entity);
    radvs_engine_update_demon_traps_locked(engine);
    radvs_engine_reparent_waiting_processes_locked(engine, entity);
    out_event->process_handle = entity->handle;
    out_event->parent_process_handle = event_parent_process;
    out_event->system_process_id = entity->process.system_process_id;
    if (!first_create_process) {
      return 0;
    }
  } break;

  case DMN_EventKind_ExitProcess: {
    RADVS_Entity *entity = event_process_entity;
    if (entity) {
      if (!entity->process.create_process_received) {
        log_infof("radvs: discarding %I64u deferred AD7 events for process without CreateProcess: process:[0x%I64x]\n",
                  entity->process.pending_public_events.count, event->process.u64[0]);
        radvs_process_exit(engine, entity);
        return 0;
      }
      out_event->process_handle = entity->handle;
      out_event->parent_process_handle = entity->parent && entity->parent->type == RADVS_EntityType_Process ? entity->parent->handle : dmn_handle_zero();
      out_event->system_process_id = entity->process.system_process_id;
      radvs_process_exit(engine, entity);
      out_event->session_process_count = (U32)radvs_engine_process_count_all(engine);
    }
  } break;

  case DMN_EventKind_CreateThread: {
    RADVS_Entity *process = event_process_entity;
    if (!process) {
      return 0;
    }
    B32 created = radvs_entity_from_handle(engine, RADVS_EntityType_Thread, event->thread) == 0;
    RADVS_Entity *entity = radvs_entity_create(engine, process, RADVS_EntityType_Thread, event->thread);
    entity->thread.state = RADVS_ThreadState_Idle;
    entity->thread.arch = event->arch;
    entity->thread.instruction_pointer = event->instruction_pointer;
    entity->thread.system_thread_id = event->system_thread_id;
    out_event->process_handle = process->handle;
    out_event->thread_handle = entity->handle;
    out_event->system_process_id = process->process.system_process_id;
    out_event->system_thread_id = entity->thread.system_thread_id;
    out_event->thread_created = created;
  } break;

  case DMN_EventKind_ExitThread: {
    RADVS_Entity *entity = radvs_entity_from_handle(engine, RADVS_EntityType_Thread, event->thread);
    if (entity && (MemoryIsZeroStruct(&event_process) || dmn_handle_match(entity->parent->handle, event->process))) {
      out_event->process_handle = entity->parent->handle;
      out_event->thread_handle = entity->handle;
      out_event->system_process_id = entity->parent->process.system_process_id;
      out_event->system_thread_id = entity->thread.system_thread_id;
      radvs_entity_release(engine, entity);
    }
  } break;

  case DMN_EventKind_LoadModule: {
    RADVS_Entity *process = event_process_entity;
    if (!process) {
      return 0;
    }
    RADVS_Entity *entity = radvs_entity_create(engine, process, RADVS_EntityType_Module, event->module);
    entity->module.base_address = event->address;
    entity->module.size = event->size;
    entity->module.path = push_str8_copy(engine->entity_arena, event->module_info && event->module_info->module_path.size ? event->module_info->module_path : event->string);
    if (event->module_info) {
      entity->module.debug_info_path = push_str8_copy(engine->entity_arena, event->module_info->debug_info_path);
      entity->module.debug_info_guid = event->module_info->debug_info_guid;
      entity->module.debug_info_timestamp = event->module_info->debug_info_timestamp;
      entity->module.debug_info_age = event->module_info->debug_info_age;
    }
    radvs_engine_queue_symbol_ticket_locked(engine, entity);
    out_event->process_handle = process->handle;
    out_event->module_handle = entity->handle;
    out_event->system_process_id = process->process.system_process_id;
  } break;

  case DMN_EventKind_UnloadModule: {
    RADVS_Entity *entity = radvs_entity_from_handle(engine, RADVS_EntityType_Module, event->module);
    if (entity && (MemoryIsZeroStruct(&event_process) || dmn_handle_match(entity->parent->handle, event->process))) {
      out_event->process_handle = entity->parent->handle;
      out_event->module_handle = entity->handle;
      out_event->system_process_id = entity->parent->process.system_process_id;
      radvs_entity_release(engine, entity);
    }
  } break;

  case DMN_EventKind_Breakpoint:
  case DMN_EventKind_Trap:
  case DMN_EventKind_SingleStep:
  case DMN_EventKind_Exception:
  case DMN_EventKind_Halt: {
    RADVS_Entity *process = event_process_entity;
    RADVS_Entity *thread = radvs_entity_from_handle(engine, RADVS_EntityType_Thread, event->thread);
    if (!process && thread) { process = thread->parent; }
    // DEMON may identify a Halt with its internal halter rather than a debuggee
    // thread. AD7 must report one of the process's real debuggee threads.
    if (!thread && process && event->kind == DMN_EventKind_Halt) {
      thread = process->process.threads.first;
    }
    if (!thread && process && !MemoryIsZeroStruct(&event_thread)) {
      thread = radvs_entity_create(engine, process, RADVS_EntityType_Thread, event->thread);
      thread->thread.state = RADVS_ThreadState_Idle;
      thread->thread.arch = event->arch;
      thread->thread.system_thread_id = event->system_thread_id;
      out_event->thread_created = 1;
    }
    if (process) {
      process->process.state = RADVS_ProcessState_Stopped;
      process->process.process_state_epoch += 1;
      out_event->process_handle = process->handle;
      out_event->system_process_id = process->process.system_process_id;
      out_event->process_state_epoch = process->process.process_state_epoch;
    }
    if (thread) {
      thread->thread.state = RADVS_ThreadState_Idle;
      thread->thread.instruction_pointer = event->instruction_pointer;
      if (event->kind == DMN_EventKind_SingleStep) {
        thread->thread.stop_trap_id = 0;
      }
      out_event->thread_handle = thread->handle;
      out_event->system_thread_id = thread->thread.system_thread_id;
    }
  } break;
  }

  if (MemoryIsZeroStruct(&out_event->process_handle)) {
    RADVS_Entity *process = radvs_entity_from_handle(engine, RADVS_EntityType_Process, event->process);
    if (process) {
      out_event->process_handle = process->handle;
      out_event->system_process_id = process->process.system_process_id;
      out_event->process_state_epoch = process->process.process_state_epoch;
    }
  }
  if (MemoryIsZeroStruct(&out_event->thread_handle)) {
    RADVS_Entity *thread = radvs_entity_from_handle(engine, RADVS_EntityType_Thread, event->thread);
    if (thread) {
      out_event->thread_handle = thread->handle;
      out_event->system_thread_id = thread->thread.system_thread_id;
      if (thread->parent) {
        out_event->process_handle = thread->parent->handle;
        out_event->system_process_id = thread->parent->process.system_process_id;
        out_event->process_state_epoch = thread->parent->process.process_state_epoch;
      }
    }
  }
  return 1;
}

internal B32
radvs_engine_refresh_symbol_tickets_in_process_locked(RADVS_Entity *process)
{
  B32 has_pending_tickets = 0;
  for EachNode(module, RADVS_Entity, process->process.modules.first) {
    RADVS_SymbolTicket *ticket = module->module.symbol_ticket;
    if (ticket != 0) {
      RADVS_SymbolTicketStatus status = radvs_symbol_ticket_status(ticket);
      module->module.symbol_state = status.state;
      has_pending_tickets |= status.state == RADVS_SymbolTicketState_Queued ||
                             status.state == RADVS_SymbolTicketState_Loading;
    }
  }
  for EachNode(child, RADVS_Entity, process->process.child_processes.first) {
    has_pending_tickets |= radvs_engine_refresh_symbol_tickets_in_process_locked(child);
  }
  return has_pending_tickets;
}

internal B32
radvs_engine_refresh_symbol_tickets_locked(RADVS_Engine *engine)
{
  B32 has_pending_tickets = 0;
  for EachNode(process, RADVS_Entity, engine->root->root.processes.first) {
    has_pending_tickets |= radvs_engine_refresh_symbol_tickets_in_process_locked(process);
  }
  radvs_engine_project_address_breakpoints_locked(engine);
  return has_pending_tickets;
}

internal void
radvs_engine_queue_public_event_locked(RADVS_Engine *engine, RADVS_EventList *ready_events, const RADVS_Event *event, B32 should_publish)
{
  if (!should_publish) {
    return;
  }

  RADVS_Entity *process = radvs_entity_from_handle(engine, RADVS_EntityType_Process, event->process_handle);
  if (event->raw.kind == DMN_EventKind_CreateProcess && process != 0) {
    radvs_engine_event_list_push_copy(engine, ready_events, event);
    radvs_event_list_concat(ready_events, &process->process.pending_public_events);
  } else if (process != 0 && !process->process.create_process_received) {
    radvs_engine_event_list_push_copy(engine, &process->process.pending_public_events, event);
  } else {
    radvs_engine_event_list_push_copy(engine, ready_events, event);
  }
}

internal void
radvs_engine_publish_event_list(RADVS_Engine *engine, RADVS_EventList *events)
{
  if (events->first != 0) {
    MutexScope (engine->public_event_mutex) {
      radvs_event_list_concat(&engine->public_events, events);
      cond_var_broadcast(engine->public_event_available_cv);
    }
  }
}

internal RADVS_Result
radvs_engine_terminate_all_targets(RADVS_Engine *engine)
{
  // DEMON owns the complete session target tree, including descendants whose
  // Engine entity is nested below its creating process.
  return radvs_demon_terminate(engine->demon, 0, 0);
}

inline void
radvs_engine_mark_process_running(RADVS_Entity *process)
{
  Assert(process->type == RADVS_EntityType_Process);
  process->process.state = RADVS_ProcessState_Running;
  process->process.process_state_epoch += 1;
  for EachNode(thread, RADVS_Entity, process->process.threads.first) {
    thread->thread.state = RADVS_ThreadState_Busy;
  }
}

internal RADVS_Result
radvs_engine_get_top_frame__(RADVS_Engine *engine, RADVS_EngineTopFrameRequest *request)
{
  MemoryZeroStruct(request->out_frame);
  *request->out_source_path_size = 0;
  RADVS_Result result = RADVS_Result_Busy;
  Temp scratch = scratch_begin(0, 0);
  MutexScopeR (engine->model_mutex) {
    do {
      RADVS_Entity *thread = radvs_entity_from_handle(engine, RADVS_EntityType_Thread, request->thread);
      RADVS_Entity *process = thread ? thread->parent : 0;
      if (!thread || !process || process->process.state != RADVS_ProcessState_Stopped) {
        break;
      }

      ARCH_Info *arch_info = arch_info_from_arch(thread->thread.arch);
      if (arch_info->reg_block_size == 0) {
        result = RADVS_Result_InvalidArgument;
        break;
      }
      void *registers = push_array_no_zero(scratch.arena, U8, arch_info->reg_block_size);
      if (!dmn_thread_read_reg_block(thread->handle, registers)) {
        break;
      }

      U64 instruction_pointer = arch_ip_from_reg_block(arch_info, registers);
      request->out_frame->process_state_epoch = process->process.process_state_epoch;
      request->out_frame->instruction_pointer = instruction_pointer;
      result = RADVS_Result_Ok;

      RADVS_Entity *module_entity = 0;
      for EachNode(module, RADVS_Entity, process->process.modules.first) {
        if (instruction_pointer >= module->module.base_address &&
            instruction_pointer - module->module.base_address < module->module.size) {
          module_entity = module;
          break;
        }
      }
      if (!module_entity || module_entity->module.symbol_state != RADVS_SymbolTicketState_Ready) {
        break;
      }

      RADVS_Module *module = &module_entity->module;
      if (!module->symbol_ticket) {
        break;
      }
      U64 voff = instruction_pointer - module->base_address;
      request->out_frame->module_base_address = module->base_address;
      request->out_frame->source_voff_first = voff;
      request->out_frame->source_voff_opl = voff + 1;
      RADVS_SymbolLocation location = {0};
      if (radvs_symbol_ticket_location_from_voff(module->symbol_ticket, voff, &location) == RADVS_Result_Ok &&
          location.line != 0 && location.source_path.size != 0) {
        request->out_frame->source_voff_first = location.voff_first;
        request->out_frame->source_voff_opl = location.voff_opl;
        request->out_frame->source_line = location.line;
        request->out_frame->source_column = location.column;
        request->out_frame->has_source = 1;
        *request->out_source_path_size = location.source_path.size;
        if (request->source_path_buffer != 0) {
          if (request->source_path_buffer_size < location.source_path.size) {
            result = RADVS_Result_OutOfMemory;
          } else {
            MemoryCopy(request->source_path_buffer, location.source_path.str, location.source_path.size);
          }
        }
      }
    } while (0);
  }
  scratch_end(scratch);
  return result;
}

internal RADVS_Result
radvs_engine_handle_request(RADVS_Engine *engine, RADVS_EngineRequest *request)
{
  RADVS_Result result = RADVS_Result_Null;

  switch (request->kind) {
  case RADVS_EngineRequestKind_Null: break;

  case RADVS_EngineRequestKind_Launch: {
    result = radvs_demon_launch(engine->demon, &request->launch.params, &request->launch.out_pid);
  } break;

  case RADVS_EngineRequestKind_Run: {
    Temp scratch = scratch_begin(0, 0);
    DMN_Trap *traps = 0;
    U64 trap_count = 0;
    MutexScopeR (engine->model_mutex) {
      trap_count = radvs_engine_enabled_trap_count_locked(engine);
      if (trap_count != 0) {
        traps = push_array_no_zero(scratch.arena, DMN_Trap, trap_count);
        radvs_engine_copy_enabled_traps_locked(engine, traps);
      }
    }
    result = radvs_demon_run_with_traps(engine->demon, request->run.handles, request->run.count, traps, trap_count);
    scratch_end(scratch);
    if (result == RADVS_Result_Ok) {
      MutexScopeW (engine->model_mutex) {
        if (request->run.count) {
          for EachIndex(handle_idx, request->run.count) {
            RADVS_Entity *process = radvs_entity_from_handle(engine, RADVS_EntityType_Process, request->run.handles[handle_idx]);
            if (process) {
              radvs_engine_mark_process_running(process);
            }
          }
        } else {
          for EachNode(process, RADVS_Entity, engine->root->root.processes.first) {
            radvs_engine_mark_processes_running_recursive(process);
          }
        }
      }
    }
  } break;

  case RADVS_EngineRequestKind_Step: {
    Temp scratch = scratch_begin(0, 0);
    DMN_Trap *traps = 0;
    U64 trap_count = 0;
    MutexScopeR (engine->model_mutex) {
      trap_count = radvs_engine_enabled_trap_count_locked(engine);
      if (trap_count != 0) {
        traps = push_array_no_zero(scratch.arena, DMN_Trap, trap_count);
        radvs_engine_copy_enabled_traps_locked(engine, traps);
      }
    }
    result = radvs_demon_step_with_traps(engine->demon, request->step.process, request->step.thread, traps, trap_count);
    scratch_end(scratch);
    if (result == RADVS_Result_Ok) {
      MutexScopeW (engine->model_mutex) {
        RADVS_Entity *process = radvs_entity_from_handle(engine, RADVS_EntityType_Process, request->step.process);
        RADVS_Entity *thread = radvs_entity_from_handle(engine, RADVS_EntityType_Thread, request->step.thread);
        if (process) {
          radvs_engine_mark_process_running(process);
        }
        if (thread) {
          thread->thread.run_intent = RADVS_RunIntent_Step;
        }
      }
    }
  } break;

  case RADVS_EngineRequestKind_Break: {
    result = radvs_demon_break(engine->demon);
  } break;

  case RADVS_EngineRequestKind_Terminate: {
    result = request->terminate.count ? radvs_demon_terminate(engine->demon, request->terminate.handles, request->terminate.count) :
                                        radvs_engine_terminate_all_targets(engine);
  } break;

  case RADVS_EngineRequestKind_ReadMemory: {
    RADVS_EngineMemoryReadRequest *read = &request->memory_read;
    if (read->address > max_U64 - read->buffer_size) {
      result = RADVS_Result_InvalidArgument;
      break;
    }

    result = RADVS_Result_Busy;
    MutexScopeR (engine->model_mutex) {
      RADVS_Entity *process = radvs_entity_from_handle(engine, RADVS_EntityType_Process, read->process);
      if (process && process->process.state == RADVS_ProcessState_Stopped) {
        read->bytes_read = dmn_process_read(read->process, r1u64(read->address, read->address + read->buffer_size), read->buffer);
        result = read->bytes_read ? RADVS_Result_Ok : RADVS_Result_Busy;
      }
    }
  } break;

  case RADVS_EngineRequestKind_ReadRegisters: {
    RADVS_EngineRegisterReadRequest *read = &request->register_read;
    result = RADVS_Result_Busy;
    MutexScopeR (engine->model_mutex) {
      RADVS_Entity *thread = radvs_entity_from_handle(engine, RADVS_EntityType_Thread, read->thread);
      if (thread && thread->parent && thread->parent->process.state == RADVS_ProcessState_Stopped) {
        ARCH_Info *arch_info = arch_info_from_arch(thread->thread.arch);
        if (arch_info->reg_block_size == 0 || read->buffer_size < arch_info->reg_block_size) {
          result = RADVS_Result_InvalidArgument;
        } else {
          MemoryZero(read->buffer, arch_info->reg_block_size);
          if (dmn_thread_read_reg_block(read->thread, read->buffer)) {
            read->bytes_read = arch_info->reg_block_size;
            result = RADVS_Result_Ok;
          }
        }
      }
    }
  } break;

  case RADVS_EngineRequestKind_GetTopFrame: {
    result = radvs_engine_get_top_frame__(engine, &request->top_frame);
  } break;
  }

  return result;
}

internal void
radvs_engine_worker(void *user_data)
{
  RADVS_Engine *engine = user_data;
  for (;;) {
    RADVS_EngineRequest *request         = 0;
    DMN_EventList        events          = {0};
    B32                  should_shutdown = 0;
    B32                  has_pending_symbol_tickets = 0;

    MutexScopeW (engine->model_mutex) {
      has_pending_symbol_tickets = radvs_engine_refresh_symbol_tickets_locked(engine);
    }
    // Converter completion has no Engine callback, so only pending tickets use a bounded wait.
    U64 symbol_poll_endt_us = has_pending_symbol_tickets ? now_time_us() + RADVS_SYMBOL_TICKET_POLL_US : max_U64;

    MutexScope (engine->requests.mutex) {
      for (;;) {
        if (engine->requests.first || engine->incoming_events.first || engine->shutdown_requested ||
            (symbol_poll_endt_us != max_U64 && now_time_us() >= symbol_poll_endt_us)) {
          break;
        }
        cond_var_wait(engine->requests.available_cv, engine->requests.mutex, symbol_poll_endt_us);
      }
      if (engine->requests.first) {
        request = (RADVS_EngineRequest *)radvs_request_queue_pop_locked(&engine->requests);
      }
      events = engine->incoming_events;
      MemoryZeroStruct(&engine->incoming_events);
      should_shutdown = engine->shutdown_requested;
    }

    if (events.first) {
      RADVS_EventList ready_events = {0};

      for EachNode(node, DMN_EventNode, events.first) {
        B32 event_should_publish = 0;
        RADVS_Event public_event = {0};
        MutexScopeW (engine->model_mutex) {
          event_should_publish = radvs_engine_reduce_event_locked(engine, &node->v, &public_event);
          radvs_engine_queue_public_event_locked(engine, &ready_events, &public_event, event_should_publish);
        }
      }

      radvs_engine_publish_event_list(engine, &ready_events);
    }

    if (request) {
      RADVS_Result result = radvs_engine_handle_request(engine, request);
      radvs_request_queue_complete(&engine->requests, &request->base, result);
    }

    if (should_shutdown) {
      radvs_demon_client_release(engine->demon);
      engine->demon = 0;
      return;
    }
  }
}

internal RADVS_Engine *
radvs_engine_alloc__(RADVS_SymbolService *symbol_service)
{
  Arena        *arena  = arena_alloc(.reserve_size = MB(1), .commit_size = KB(64));
  RADVS_Engine *engine = push_array(arena, RADVS_Engine, 1);
  engine->arena = arena;
  engine->command_arena      = arena_alloc(.reserve_size = MB(1), .commit_size = KB(64));
  engine->incoming_event_arena = arena_alloc(.reserve_size = MB(1), .commit_size = KB(64));
  engine->public_event_arena = arena_alloc(.reserve_size = MB(1), .commit_size = KB(64));
  engine->entity_arena       = arena_alloc(.reserve_size = MB(1), .commit_size = KB(64));
  radvs_request_queue_init(&engine->requests, engine->command_arena);
#if RADVS_REQUEST_DIAGNOSTICS
  radvs_request_queue_set_diagnostic_name(&engine->requests, str8_lit("engine"));
#endif
  engine->public_event_mutex = mutex_alloc();
  engine->public_event_available_cv = cond_var_alloc();
  engine->model_mutex        = rw_mutex_alloc();
  engine->accepting_requests = 1;
  engine->next_entity_id     = 1;
  engine->symbol_service     = symbol_service;
  engine->root               = radvs_entity_alloc(engine);
  engine->root->type         = RADVS_EntityType_Root;
  return engine;
}

internal void
radvs_engine_release__(RADVS_Engine *engine)
{
  rw_mutex_release(engine->model_mutex);
  cond_var_release(engine->public_event_available_cv);
  mutex_release(engine->public_event_mutex);
  radvs_request_queue_release(&engine->requests);
  arena_release(engine->entity_arena);
  arena_release(engine->public_event_arena);
  arena_release(engine->incoming_event_arena);
  arena_release(engine->command_arena);
  arena_release(engine->arena);
}

RADVS_Result
radvs_engine_alloc(RADVS_SymbolService *symbol_service, RADVS_Engine **out_engine)
{
  if (!symbol_service || !out_engine) {
    return RADVS_Result_InvalidArgument;
  }

  // alloc the engine resources
  RADVS_Engine *engine = radvs_engine_alloc__(symbol_service);

  // alloc demon client
  RADVS_Result result = radvs_demon_alloc(&engine->demon);

  // The bridge owns SymbolService. The engine only retains its explicit
  // dependency while it owns module-ticket associations.
  if (result == RADVS_Result_Ok) {
    engine->worker = thread_launch(radvs_engine_worker, engine);
    if (engine->worker.u64[0] != 0) {
      *out_engine = engine;
    } else {
      radvs_demon_client_release(engine->demon);
      radvs_engine_release__(engine);
      result = RADVS_Result_OutOfMemory;
    }
  }
  // fail -> release engine resources
  else {
    radvs_engine_release__(engine);
  }

  return result;
}

RADVS_Result
radvs_engine_release(RADVS_Engine *engine)
{
  if (!engine) {
    return RADVS_Result_InvalidArgument;
  }

  MutexScope (engine->requests.mutex) {
    engine->accepting_requests = 0;
    engine->shutdown_requested = 1;
    cond_var_broadcast(engine->requests.available_cv);
  }

  thread_join(engine->worker, max_U64);

  MutexScopeW (engine->model_mutex) {
    radvs_entity_release(engine, engine->root);
    engine->root = 0;
  }

  radvs_engine_release__(engine);
  return RADVS_Result_Ok;
}

RADVS_Result
radvs_engine_launch(RADVS_Engine *engine, String8 exe, String8 cmd_line, String8 wdir, U32 *out_pid)
{
  if (!engine || exe.size == 0 || !out_pid) {
    return RADVS_Result_InvalidArgument;
  }

  *out_pid = 0;
  RADVS_EngineRequest *request = 0;
  MutexScope (engine->requests.mutex) {
    request = radvs_engine_request_alloc_locked(engine, RADVS_EngineRequestKind_Launch);
    ProcessLaunchParams *params = &request->launch.params;
    params->path = push_str8_copy(engine->command_arena, wdir);
    params->inherit_env = 1;
    str8_list_push(engine->command_arena, &params->cmd_line, push_str8_copy(engine->command_arena, exe));
    if (cmd_line.size != 0) {
      str8_list_push(engine->command_arena, &params->cmd_line, push_str8_copy(engine->command_arena, cmd_line));
    }
  }
  RADVS_Result result = radvs_engine_submit(engine, request);
  if (result == RADVS_Result_Ok) {
    *out_pid = request->launch.out_pid;
  }
  radvs_engine_request_recycle(engine, request);
  return result;
}

RADVS_Result
radvs_engine_launch16(RADVS_Engine *engine, String16 exe, String16 cmd_line, String16 wdir, U32 *out_pid)
{
  Temp scratch = scratch_begin(0, 0);
  RADVS_Result result = radvs_engine_launch(engine,
                                            str8_from_16(scratch.arena, exe),
                                            str8_from_16(scratch.arena, cmd_line),
                                            str8_from_16(scratch.arena, wdir),
                                            out_pid);
  scratch_end(scratch);
  return result;
}

internal RADVS_Result
radvs_engine_submit_process_handles(RADVS_Engine *engine, RADVS_EngineRequestKind kind, const DMN_Handle *process_handles, U64 process_handle_count)
{
  if (!engine || (process_handle_count && !process_handles)) {
    return RADVS_Result_InvalidArgument;
  }

  RADVS_EngineRequest *request = 0;
  MutexScope (engine->requests.mutex) {
    request = radvs_engine_request_alloc_locked(engine, kind);
    request->processes.count = process_handle_count;
    if (process_handle_count) {
      request->processes.handles = push_array_no_zero(engine->command_arena, DMN_Handle, process_handle_count);
      MemoryCopy(request->processes.handles, process_handles, process_handle_count * sizeof(*process_handles));
    }
  }
  RADVS_Result result = radvs_engine_submit(engine, request);
  radvs_engine_request_recycle(engine, request);
  return result;
}

RADVS_Result
radvs_engine_run(RADVS_Engine *engine, const DMN_Handle *process_handles, U64 process_handle_count)
{
  return radvs_engine_submit_process_handles(engine, RADVS_EngineRequestKind_Run, process_handles, process_handle_count);
}

RADVS_Result
radvs_engine_step_thread(RADVS_Engine *engine, DMN_Handle thread_handle)
{
  if (!engine || MemoryIsZeroStruct(&thread_handle)) {
    return RADVS_Result_InvalidArgument;
  }

  RADVS_Result result = RADVS_Result_Busy;
  DMN_Handle process_handle = {0};
  MutexScopeR (engine->model_mutex) {
    RADVS_Entity *thread = radvs_entity_from_handle(engine, RADVS_EntityType_Thread, thread_handle);
    RADVS_Entity *process = thread ? thread->parent : 0;
    if (thread && process && process->type == RADVS_EntityType_Process && process->process.state == RADVS_ProcessState_Stopped) {
      process_handle = process->handle;
      result = RADVS_Result_Ok;
    }
  }
  if (result != RADVS_Result_Ok) {
    return result;
  }

  RADVS_EngineRequest *request = 0;
  MutexScope (engine->requests.mutex) {
    request = radvs_engine_request_alloc_locked(engine, RADVS_EngineRequestKind_Step);
    request->step.process = process_handle;
    request->step.thread = thread_handle;
  }
  result = radvs_engine_submit(engine, request);
  radvs_engine_request_recycle(engine, request);
  return result;
}

RADVS_Result
radvs_engine_break(RADVS_Engine *engine)
{
  return radvs_engine_submit_process_handles(engine, RADVS_EngineRequestKind_Break, 0, 0);
}

RADVS_Result
radvs_engine_terminate(RADVS_Engine *engine, const DMN_Handle *process_handles, U64 process_handle_count)
{
  return radvs_engine_submit_process_handles(engine, RADVS_EngineRequestKind_Terminate, process_handles, process_handle_count);
}

RADVS_Result
radvs_engine_evaluate_expression(RADVS_Engine *engine, String8 expression)
{
  return !engine || expression.size == 0 ? RADVS_Result_InvalidArgument : RADVS_Result_NotImplemented;
}

RADVS_Result
radvs_engine_evaluate_expression16(RADVS_Engine *engine, String16 expression)
{
  Temp scratch = scratch_begin(0, 0);
  RADVS_Result result = radvs_engine_evaluate_expression(engine, str8_from_16(scratch.arena, expression));
  scratch_end(scratch);
  return result;
}

RADVS_Result
radvs_engine_enumerate_modules(RADVS_Engine *engine, DMN_Handle process_handle, RADVS_ModuleVisitor *visitor, void *user_data)
{
  if (!engine || !visitor) {
    return RADVS_Result_InvalidArgument;
  }
  RADVS_Result result = RADVS_Result_InvalidArgument;
  MutexScopeR (engine->model_mutex) {
    RADVS_Entity *process = radvs_entity_from_handle(engine, RADVS_EntityType_Process, process_handle);
    if (process) {
      for EachNode(module, RADVS_Entity, process->process.modules.first) {
        visitor(user_data, module->handle, &module->module);
      }
      result = RADVS_Result_Ok;
    }
  }
  return result;
}

RADVS_Result
radvs_engine_enumerate_threads(RADVS_Engine *engine, DMN_Handle process_handle, RADVS_ThreadVisitor *visitor, void *user_data)
{
  if (!engine || !visitor) {
    return RADVS_Result_InvalidArgument;
  }
  RADVS_Result result = RADVS_Result_InvalidArgument;
  MutexScopeR (engine->model_mutex) {
    RADVS_Entity *process = radvs_entity_from_handle(engine, RADVS_EntityType_Process, process_handle);
    if (process) {
      for EachNode(thread, RADVS_Entity, process->process.threads.first) {
        visitor(user_data, thread->handle, &thread->thread);
      }
      result = RADVS_Result_Ok;
    }
  }
  return result;
}

internal RADVS_Entity *
radvs_engine_direct_breakpoint_from_spec_in_list_locked(RADVS_EntityList *list, const RADVS_BreakpointSpec *spec)
{
  for EachNode(breakpoint, RADVS_Entity, list->first) {
    RADVS_BreakpointSpec *existing = &breakpoint->breakpoint.info.spec;
    if (existing->location_kind == RADVS_BreakpointLocationKind_Address &&
        existing->condition_kind == RADVS_BreakpointConditionKind_Always &&
        existing->address == spec->address &&
        existing->address_mode == spec->address_mode) {
      return breakpoint;
    }
    RADVS_Entity *child = radvs_engine_direct_breakpoint_from_spec_in_list_locked(&breakpoint->breakpoint.address_breakpoints, spec);
    if (child != 0) {
      return child;
    }
  }
  return 0;
}

internal RADVS_Entity *
radvs_engine_direct_breakpoint_from_spec_locked(RADVS_Engine *engine, const RADVS_BreakpointSpec *spec)
{
  return radvs_engine_direct_breakpoint_from_spec_in_list_locked(&engine->root->root.user_breakpoints, spec);
}

RADVS_Result
radvs_engine_create_breakpoint(RADVS_Engine *engine, const RADVS_BreakpointSpec *spec, RADVS_BreakpointID *out_breakpoint_id)
{
  if (!engine || !spec || !out_breakpoint_id) {
    return RADVS_Result_InvalidArgument;
  }
  *out_breakpoint_id = 0;

  RADVS_BreakpointSpec normalized = *spec;
  normalized.enabled = normalized.enabled != 0;
  if (normalized.location_kind == RADVS_BreakpointLocationKind_Address) {
    if (normalized.address == 0 || normalized.condition_kind != RADVS_BreakpointConditionKind_Always) {
      return normalized.condition_kind == RADVS_BreakpointConditionKind_Expression ? RADVS_Result_NotImplemented : RADVS_Result_InvalidArgument;
    }
    if (normalized.address_mode == RADVS_AddressBreakpointMode_Auto) {
      normalized.address_mode = RADVS_AddressBreakpointMode_Software;
    }
    if (normalized.address_mode != RADVS_AddressBreakpointMode_Software &&
        normalized.address_mode != RADVS_AddressBreakpointMode_Hardware) {
      return RADVS_Result_InvalidArgument;
    }
  } else if (normalized.location_kind == RADVS_BreakpointLocationKind_Source) {
    return RADVS_Result_NotImplemented;
  } else {
    return RADVS_Result_InvalidArgument;
  }

  RADVS_Result result = RADVS_Result_Ok;
  MutexScopeW (engine->model_mutex) {
    RADVS_Entity *breakpoint = radvs_engine_direct_breakpoint_from_spec_locked(engine, &normalized);
    if (breakpoint != 0) {
      result = radvs_breakpoint_set_enabled_locked(breakpoint, normalized.enabled);
      if (result == RADVS_Result_Ok) {
        *out_breakpoint_id = breakpoint->id;
        result = radvs_engine_update_demon_traps_locked(engine);
        if (result != RADVS_Result_Ok) {
          *out_breakpoint_id = 0;
        }
      }
    } else {
      breakpoint = radvs_entity_create(engine, engine->root, RADVS_EntityType_Breakpoint, dmn_handle_zero());
      breakpoint->breakpoint.visibility             = RADVS_BreakpointVisibility_User;
      breakpoint->breakpoint.info.breakpoint_id     = breakpoint->id;
      breakpoint->breakpoint.info.spec              = normalized;
      breakpoint->breakpoint.info.binding_result    = RADVS_Result_Ok;
      breakpoint->breakpoint.source_projection_pending = 1;
      for EachNode(process, RADVS_Entity, engine->root->root.processes.first) {
        RADVS_Result bind_result = radvs_engine_bind_breakpoint_to_processes_recursive_locked(engine, breakpoint, process);
        if (bind_result != RADVS_Result_Ok) {
          result = bind_result;
          break;
        }
      }
      if (result == RADVS_Result_Ok) {
        radvs_engine_project_address_breakpoints_locked(engine);
        *out_breakpoint_id = breakpoint->id;
        engine->breakpoint_generation += 1;
        result = radvs_engine_update_demon_traps_locked(engine);
        if (result != RADVS_Result_Ok) {
          *out_breakpoint_id = 0;
          radvs_entity_release(engine, breakpoint);
          radvs_engine_update_demon_traps_locked(engine);
        }
      } else {
        radvs_entity_release(engine, breakpoint);
      }
    }
  }
  return result;
}

RADVS_Result
radvs_engine_alloc_breakpoint(RADVS_Engine *engine, const RADVS_BreakpointSpec *spec, RADVS_BreakpointID *out_breakpoint_id)
{
  return radvs_engine_create_breakpoint(engine, spec, out_breakpoint_id);
}

RADVS_Result
radvs_engine_remove_breakpoint(RADVS_Engine *engine, RADVS_BreakpointID breakpoint_id)
{
  if (!engine || breakpoint_id == 0) {
    return RADVS_Result_InvalidArgument;
  }
  RADVS_Result result = RADVS_Result_Ok;
  MutexScopeW (engine->model_mutex) {
    RADVS_Entity *breakpoint = radvs_entity_from_id(engine, RADVS_EntityType_Breakpoint, breakpoint_id);
    if (breakpoint) {
      RADVS_Entity *parent = breakpoint->parent;
      B32 remove_empty_source_group = parent != 0 &&
                                      parent->type == RADVS_EntityType_Breakpoint &&
                                      parent->breakpoint.info.spec.location_kind == RADVS_BreakpointLocationKind_Source &&
                                      breakpoint->breakpoint.info.spec.location_kind == RADVS_BreakpointLocationKind_Address;
      radvs_entity_release(engine, breakpoint);
      if (remove_empty_source_group && parent->breakpoint.address_breakpoints.first == 0) {
        radvs_entity_release(engine, parent);
      }
      engine->breakpoint_generation += 1;
      result = radvs_engine_update_demon_traps_locked(engine);
    } else {
      result = RADVS_Result_InvalidArgument;
    }
  }
  return result;
}

RADVS_Result
radvs_engine_set_breakpoint_enabled(RADVS_Engine *engine, RADVS_BreakpointID breakpoint_id, U32 enabled)
{
  if (!engine || breakpoint_id == 0) {
    return RADVS_Result_InvalidArgument;
  }
  RADVS_Result result = RADVS_Result_Ok;
  MutexScopeW (engine->model_mutex) {
    RADVS_Entity *breakpoint = radvs_entity_from_id(engine, RADVS_EntityType_Breakpoint, breakpoint_id);
    if (breakpoint) {
      result = radvs_breakpoint_set_enabled_locked(breakpoint, enabled != 0);
      if (result == RADVS_Result_Ok) {
        engine->breakpoint_generation += 1;
        result = radvs_engine_update_demon_traps_locked(engine);
      }
    } else {
      result = RADVS_Result_InvalidArgument;
    }
  }
  return result;
}

RADVS_Result
radvs_engine_read_memory(RADVS_Engine *engine, DMN_Handle process_handle, U64 address, void *buffer, U64 size, U64 *out_size)
{
  if (!engine || MemoryIsZeroStruct(&process_handle) || address == 0 || !buffer || size == 0 || !out_size) {
    return RADVS_Result_InvalidArgument;
  }
  *out_size = 0;

  RADVS_EngineRequest *request = 0;
  MutexScope (engine->requests.mutex) {
    request = radvs_engine_request_alloc_locked(engine, RADVS_EngineRequestKind_ReadMemory);
    request->memory_read.process     = process_handle;
    request->memory_read.address     = address;
    request->memory_read.buffer      = buffer;
    request->memory_read.buffer_size = size;
  }
  RADVS_Result result = radvs_engine_submit(engine, request);
  *out_size = request->memory_read.bytes_read;
  radvs_engine_request_recycle(engine, request);
  return result;
}

RADVS_Result
radvs_engine_write_memory(RADVS_Engine *engine, DMN_Handle process_handle, U64 address, const void *buffer, U64 size, U64 *out_size)
{
  if (!engine || MemoryIsZeroStruct(&process_handle) || address == 0 || !buffer || !out_size) {
    return RADVS_Result_InvalidArgument;
  }
  *out_size = 0;
  return RADVS_Result_NotImplemented;
}

RADVS_Result
radvs_engine_read_registers(RADVS_Engine *engine, DMN_Handle thread_handle, void *buffer, U64 size, U64 *out_size)
{
  if (!engine || MemoryIsZeroStruct(&thread_handle) || !buffer || size == 0 || !out_size) {
    return RADVS_Result_InvalidArgument;
  }
  *out_size = 0;

  RADVS_EngineRequest *request = 0;
  MutexScope (engine->requests.mutex) {
    request = radvs_engine_request_alloc_locked(engine, RADVS_EngineRequestKind_ReadRegisters);
    request->register_read.thread      = thread_handle;
    request->register_read.buffer      = buffer;
    request->register_read.buffer_size = size;
  }
  RADVS_Result result = radvs_engine_submit(engine, request);
  *out_size = request->register_read.bytes_read;
  radvs_engine_request_recycle(engine, request);
  return result;
}

RADVS_Result
radvs_engine_write_registers(RADVS_Engine *engine, DMN_Handle thread_handle, const void *buffer, U64 size)
{
  return !engine || MemoryIsZeroStruct(&thread_handle) || !buffer || size == 0 ? RADVS_Result_InvalidArgument : RADVS_Result_NotImplemented;
}

RADVS_Result
radvs_engine_get_top_frame(RADVS_Engine *engine, DMN_Handle thread_handle, RADVS_FrameInfo *out_frame, char *source_path_buffer, U64 source_path_buffer_size, U64 *out_source_path_size)
{
  if (!engine || MemoryIsZeroStruct(&thread_handle) || !out_frame || !out_source_path_size ||
      (source_path_buffer == 0 && source_path_buffer_size != 0)) {
    return RADVS_Result_InvalidArgument;
  }
  *out_source_path_size = 0;

  RADVS_EngineRequest *request = 0;
  MutexScope (engine->requests.mutex) {
    request = radvs_engine_request_alloc_locked(engine, RADVS_EngineRequestKind_GetTopFrame);
    request->top_frame.thread                  = thread_handle;
    request->top_frame.out_frame               = out_frame;
    request->top_frame.source_path_buffer      = source_path_buffer;
    request->top_frame.source_path_buffer_size = source_path_buffer_size;
    request->top_frame.out_source_path_size    = out_source_path_size;
  }
  RADVS_Result result = radvs_engine_submit(engine, request);
  radvs_engine_request_recycle(engine, request);
  return result;
}

RADVS_Result
radvs_engine_get_process_desc(RADVS_Engine *engine, DMN_Handle process_handle, RADVS_ProcessDesc *out_desc)
{
  if (!engine || MemoryIsZeroStruct(&process_handle) || !out_desc) {
    return RADVS_Result_InvalidArgument;
  }

  RADVS_Result result = RADVS_Result_InvalidArgument;
  MutexScopeR (engine->model_mutex) {
    RADVS_Entity *process = radvs_entity_from_handle(engine, RADVS_EntityType_Process, process_handle);
    if (process) {
      *out_desc = (RADVS_ProcessDesc){
        .process_handle        = process->handle,
        .parent_process_handle = process->parent && process->parent->type == RADVS_EntityType_Process ? process->parent->handle : dmn_handle_zero(),
        .system_process_id     = process->process.system_process_id,
        .state                 = process->process.state,
      };
      result = RADVS_Result_Ok;
    }
  }

  return result;
}

RADVS_Result
radvs_engine_get_thread_desc(RADVS_Engine *engine, DMN_Handle thread_handle, RADVS_ThreadDesc *out_desc)
{
  if (!engine || MemoryIsZeroStruct(&thread_handle) || !out_desc) {
    return RADVS_Result_InvalidArgument;
  }

  RADVS_Result result = RADVS_Result_InvalidArgument;
  MutexScopeR (engine->model_mutex) {
    RADVS_Entity *thread  = radvs_entity_from_handle(engine, RADVS_EntityType_Thread, thread_handle);
    RADVS_Entity *process = thread ? thread->parent : 0;
    if (thread && process && process->type == RADVS_EntityType_Process) {
      *out_desc = (RADVS_ThreadDesc){
        .thread_handle       = thread->handle,
        .process_handle      = process->handle,
        .process_state_epoch = process->process.process_state_epoch,
        .system_thread_id    = thread->thread.system_thread_id,
        .state               = thread->thread.state,
      };
      result = RADVS_Result_Ok;
    }
  }

  return result;
}

RADVS_Result
radvs_engine_copy_threads(RADVS_Engine *engine, RADVS_ThreadDesc *buffer, U64 buffer_count, U64 *out_count)
{
  if (!engine || !out_count || (!buffer && buffer_count != 0)) {
    return RADVS_Result_InvalidArgument;
  }

  RADVS_Result result = RADVS_Result_Ok;
  MutexScopeR (engine->model_mutex) {
    U64 count = 0;
    for EachNode(process, RADVS_Entity, engine->root->root.processes.first) {
      count += radvs_engine_thread_count_recursive(process);
    }
    *out_count = count;

    if (count > buffer_count) {
      result = RADVS_Result_OutOfMemory;
    } else {
      U64 index = 0;
      for EachNode(process, RADVS_Entity, engine->root->root.processes.first) {
        radvs_engine_copy_threads_recursive(process, buffer, &index);
      }
    }
  }

  return result;
}

RADVS_Result
radvs_engine_copy_modules(RADVS_Engine *engine, RADVS_ModuleDesc *buffer, U64 buffer_count, U64 *out_count)
{
  if (!engine || !out_count || (!buffer && buffer_count != 0)) {
    return RADVS_Result_InvalidArgument;
  }

  RADVS_Result result = RADVS_Result_Ok;
  MutexScopeR (engine->model_mutex) {
    U64 count = 0;
    for EachNode(process, RADVS_Entity, engine->root->root.processes.first) {
      count += radvs_engine_module_count_recursive(process);
    }
    *out_count = count;

    if (count > buffer_count) {
      result = RADVS_Result_OutOfMemory;
    } else {
      U64 index = 0;
      for EachNode(process, RADVS_Entity, engine->root->root.processes.first) {
        radvs_engine_copy_modules_recursive(process, buffer, &index);
      }
    }
  }
  return result;
}

RADVS_Result
radvs_engine_run_thread(RADVS_Engine *engine, DMN_Handle thread_handle)
{
  if (!engine || MemoryIsZeroStruct(&thread_handle)) {
    return RADVS_Result_InvalidArgument;
  }

  RADVS_Result result         = RADVS_Result_InvalidArgument;
  DMN_Handle   process_handle = {0};
  MutexScopeR (engine->model_mutex) {
    RADVS_Entity *thread = radvs_entity_from_handle(engine, RADVS_EntityType_Thread, thread_handle);
    if (thread && thread->parent && thread->parent->type == RADVS_EntityType_Process) {
      process_handle = thread->parent->handle;
      result         = RADVS_Result_Ok;
    }
  }

  if (result == RADVS_Result_Ok) {
    MutexScopeW (engine->model_mutex) {
      RADVS_Entity *thread = radvs_entity_from_handle(engine, RADVS_EntityType_Thread, thread_handle);
      if (thread) {
        thread->thread.run_intent           = RADVS_RunIntent_Continue;
        thread->thread.stop_trap_id         = 0;
      }
    }
  }

  return result == RADVS_Result_Ok ? radvs_engine_run(engine, &process_handle, 1) : result;
}

RADVS_Result
radvs_engine_terminate_process(RADVS_Engine *engine, DMN_Handle process_handle)
{
  if (!engine || MemoryIsZeroStruct(&process_handle)) {
    return RADVS_Result_InvalidArgument;
  }

  RADVS_Result result = RADVS_Result_InvalidArgument;
  MutexScopeR (engine->model_mutex) {
    RADVS_Entity *process = radvs_entity_from_handle(engine, RADVS_EntityType_Process, process_handle);
    if (process) {
      result = RADVS_Result_Ok;
    }
  }

  if (result == RADVS_Result_Ok) {
    result = radvs_engine_terminate(engine, &process_handle, 1);
  }

  return result;
}

RADVS_Result
radvs_engine_get_breakpoint_info(RADVS_Engine *engine, RADVS_BreakpointID breakpoint_id, RADVS_BreakpointInfo *out_info)
{
  if (!engine || breakpoint_id == 0 || !out_info) {
    return RADVS_Result_InvalidArgument;
  }
  RADVS_Result result = RADVS_Result_InvalidArgument;
  MutexScopeR (engine->model_mutex) {
    RADVS_Entity *breakpoint = radvs_entity_from_id(engine, RADVS_EntityType_Breakpoint, breakpoint_id);
    if (breakpoint != 0) {
      *out_info = breakpoint->breakpoint.info;
      result = RADVS_Result_Ok;
    }
  }
  return result;
}

RADVS_Result
radvs_engine_breakpoint_update_begin(RADVS_Engine *engine)
{
  if (!engine) {
    return RADVS_Result_InvalidArgument;
  }
  MutexScopeW (engine->model_mutex) {
    engine->breakpoint_update_depth += 1;
  }
  return RADVS_Result_Ok;
}

RADVS_Result
radvs_engine_breakpoint_update_end(RADVS_Engine *engine)
{
  if (!engine) {
    return RADVS_Result_InvalidArgument;
  }
  RADVS_Result result = RADVS_Result_Ok;
  MutexScopeW (engine->model_mutex) {
    if (engine->breakpoint_update_depth == 0) {
      result = RADVS_Result_InvalidArgument;
    } else {
      engine->breakpoint_update_depth -= 1;
    }
  }
  return result;
}

void
radvs_engine_close_event_wait(RADVS_Engine *engine)
{
  if (engine != 0) {
    MutexScope (engine->public_event_mutex) {
      engine->event_wait_closed = 1;
      cond_var_broadcast(engine->public_event_available_cv);
    }
  }
}

RADVS_Result
radvs_engine_wait_event(RADVS_Engine *engine, U64 timeout_us)
{
  if (!engine) {
    return RADVS_Result_InvalidArgument;
  }
  U64 endt_us = now_time_us() + timeout_us;
  RADVS_Result result = RADVS_Result_Ok;
  MutexScope (engine->public_event_mutex) {
    for (; !engine->public_events.first && !engine->event_wait_closed;) {
      if (timeout_us == 0 || now_time_us() >= endt_us) {
        result = RADVS_Result_Busy;
        break;
      }
      cond_var_wait(engine->public_event_available_cv, engine->public_event_mutex, endt_us);
    }
    if (engine->event_wait_closed && !engine->public_events.first) {
      result = RADVS_Result_Busy;
    }
  }
  return result;
}

RADVS_Result
radvs_engine_poll_event(RADVS_Engine *engine, RADVS_Event *out_event)
{
  if (!engine || !out_event) {
    return RADVS_Result_InvalidArgument;
  }

  RADVS_Result result = RADVS_Result_Busy;
  MutexScope (engine->public_event_mutex) {
    RADVS_EventNode *node = engine->public_events.first;
    if (node) {
      SLLQueuePop(engine->public_events.first, engine->public_events.last);
      engine->public_events.count -= 1;
      *out_event = node->v;
      result = RADVS_Result_Ok;
    }
  }
  return result;
}

#undef RADVS_Engine
#undef radvs_engine_alloc
#undef radvs_engine_release
#undef radvs_engine_launch
#undef radvs_engine_launch16
#undef radvs_engine_run
#undef radvs_engine_step_thread
#undef radvs_engine_break
#undef radvs_engine_terminate
#undef radvs_engine_evaluate_expression
#undef radvs_engine_evaluate_expression16
#undef radvs_engine_enumerate_modules
#undef radvs_engine_enumerate_threads
#undef radvs_engine_create_breakpoint
#undef radvs_engine_alloc_breakpoint
#undef radvs_engine_remove_breakpoint
#undef radvs_engine_set_breakpoint_enabled
#undef radvs_engine_get_breakpoint_info
#undef radvs_engine_breakpoint_update_begin
#undef radvs_engine_breakpoint_update_end
#undef radvs_engine_read_memory
#undef radvs_engine_write_memory
#undef radvs_engine_read_registers
#undef radvs_engine_write_registers
#undef radvs_engine_get_top_frame
#undef radvs_engine_get_process_desc
#undef radvs_engine_get_thread_desc
#undef radvs_engine_copy_threads
#undef radvs_engine_copy_modules
#undef radvs_engine_run_thread
#undef radvs_engine_terminate_process
#undef radvs_engine_close_event_wait
#undef radvs_engine_wait_event
#undef radvs_engine_poll_event

typedef struct RADVS_EngineRoute RADVS_EngineRoute;
typedef struct RADVS_EnginePendingEvent RADVS_EnginePendingEvent;
typedef struct RADVS_EnginePendingRoute RADVS_EnginePendingRoute;

struct RADVS_EngineRoute
{
  RADVS_EngineRoute   *next;
  RADVS_EngineSession *session;
  DMN_Handle           process;
  U32                  process_pid;
};

struct RADVS_EnginePendingEvent
{
  RADVS_EnginePendingEvent *next;
  DMN_Event                 v;
};

struct RADVS_EnginePendingRoute
{
  RADVS_EnginePendingRoute *next;
  DMN_Handle                process;
  DMN_Handle                parent_process;
  U32                       process_pid;
  B32                       has_create_process;
  RADVS_EnginePendingEvent *first;
  RADVS_EnginePendingEvent *last;
};

struct RADVS_Engine
{
  Arena                *arena;
  Mutex                 sessions_mutex;
  Mutex                 router_mutex;
  RADVS_SymbolService  *symbol_service;
  RADVS_EngineSession  *first_session;
  U64                   next_session_id;
  RADVS_EngineRoute    *first_route;
  RADVS_EnginePendingRoute *first_pending_route;
};

internal RADVS_EngineRoute *radvs_engine_router_route_from_pid_locked(RADVS_Engine *engine, U32 process_pid);
internal RADVS_EngineRoute *radvs_engine_router_route_add_locked(RADVS_Engine *engine, RADVS_EngineSession *session, U32 process_pid, DMN_Handle process);

internal RADVS_EngineSession *
radvs_engine_router_session_from_demon_locked(RADVS_Engine *engine, RADVS_DemonClient *demon)
{
  for (RADVS_EngineSession *session = engine->first_session; session != 0; session = session->next_in_host) {
    if (session->demon == demon) {
      return session;
    }
  }
  return 0;
}

internal void
radvs_engine_router_launch_ingress(void *user_data, RADVS_DemonClient *demon, U32 process_pid)
{
  RADVS_Engine *engine = user_data;
  if (engine != 0 && process_pid != 0) {
    MutexScope (engine->router_mutex) {
      MutexScope (engine->sessions_mutex) {
        RADVS_EngineSession *session = radvs_engine_router_session_from_demon_locked(engine, demon);
        if (session != 0 && radvs_engine_router_route_from_pid_locked(engine, process_pid) == 0) {
          radvs_engine_router_route_add_locked(engine, session, process_pid, dmn_handle_zero());
        }
      }
    }
  }
}

internal RADVS_EngineRoute *
radvs_engine_router_route_from_process_locked(RADVS_Engine *engine, DMN_Handle process)
{
  for EachNode(route, RADVS_EngineRoute, engine->first_route) {
    if (dmn_handle_match(route->process, process)) {
      return route;
    }
  }
  return 0;
}

internal RADVS_EngineRoute *
radvs_engine_router_route_from_pid_locked(RADVS_Engine *engine, U32 process_pid)
{
  for EachNode(route, RADVS_EngineRoute, engine->first_route) {
    if (MemoryIsZeroStruct(&route->process) && route->process_pid == process_pid) {
      return route;
    }
  }
  return 0;
}

internal RADVS_EnginePendingRoute *
radvs_engine_router_pending_from_process_locked(RADVS_Engine *engine, DMN_Handle process)
{
  for EachNode(route, RADVS_EnginePendingRoute, engine->first_pending_route) {
    if (dmn_handle_match(route->process, process)) {
      return route;
    }
  }
  return 0;
}

internal RADVS_EnginePendingRoute *
radvs_engine_router_pending_ensure_locked(RADVS_Engine *engine, DMN_Handle process)
{
  RADVS_EnginePendingRoute *route = radvs_engine_router_pending_from_process_locked(engine, process);
  if (route == 0) {
    route = push_array(engine->arena, RADVS_EnginePendingRoute, 1);
    route->process = process;
    SLLStackPush(engine->first_pending_route, route);
  }
  return route;
}

internal void
radvs_engine_router_pending_push_locked(RADVS_Engine *engine, RADVS_EnginePendingRoute *route, const DMN_Event *event)
{
  RADVS_EnginePendingEvent *node = push_array(engine->arena, RADVS_EnginePendingEvent, 1);
  node->v = *event;
  node->v.string = push_str8_copy(engine->arena, event->string);
  if (event->module_info != 0 && event->module_info != &dmn_module_info_nil) {
    node->v.module_info = push_array(engine->arena, DMN_ModuleInfo, 1);
    MemoryCopyStruct(node->v.module_info, event->module_info);
    node->v.module_info->module_path = push_str8_copy(engine->arena, event->module_info->module_path);
    node->v.module_info->debug_info_path = push_str8_copy(engine->arena, event->module_info->debug_info_path);
  }
  SLLQueuePush(route->first, route->last, node);
}

internal void
radvs_engine_router_pending_record_locked(RADVS_Engine *engine, const DMN_Event *event)
{
  DMN_Handle process = event->process;
  if (MemoryIsZeroStruct(&process)) {
    return;
  }
  RADVS_EnginePendingRoute *route = radvs_engine_router_pending_ensure_locked(engine, process);
  if (event->kind == DMN_EventKind_CreateProcess) {
    route->parent_process = event->parent_process;
    route->process_pid = event->system_process_id;
    route->has_create_process = 1;
  }
  radvs_engine_router_pending_push_locked(engine, route, event);
}

internal void
radvs_engine_router_pending_remove_locked(RADVS_Engine *engine, RADVS_EnginePendingRoute *route)
{
  RADVS_EnginePendingRoute **next = &engine->first_pending_route;
  for (; *next != 0; next = &(*next)->next) {
    if (*next == route) {
      *next = route->next;
      route->next = 0;
      break;
    }
  }
}

internal RADVS_EngineRoute *
radvs_engine_router_route_add_locked(RADVS_Engine *engine, RADVS_EngineSession *session, U32 process_pid, DMN_Handle process)
{
  RADVS_EngineRoute *route = push_array(engine->arena, RADVS_EngineRoute, 1);
  route->session = session;
  route->process_pid = process_pid;
  route->process = process;
  SLLStackPush(engine->first_route, route);
  return route;
}

internal void
radvs_engine_router_deliver_locked(RADVS_EngineSession *session, const DMN_Event *event)
{
  if (session != 0) {
    radvs_engine_demon_event(session, event);
  }
}

internal void radvs_engine_router_flush_children_locked(RADVS_Engine *engine, DMN_Handle parent_process, RADVS_EngineSession *session);

internal void
radvs_engine_router_flush_route_locked(RADVS_Engine *engine, RADVS_EnginePendingRoute *pending, RADVS_EngineSession *session)
{
  radvs_engine_router_pending_remove_locked(engine, pending);
  RADVS_EngineRoute *route = radvs_engine_router_route_from_process_locked(engine, pending->process);
  if (route == 0) {
    route = radvs_engine_router_route_add_locked(engine, session, pending->process_pid, pending->process);
  }
  for EachNode(event, RADVS_EnginePendingEvent, pending->first) {
    radvs_engine_router_deliver_locked(route->session, &event->v);
  }
  radvs_engine_router_flush_children_locked(engine, pending->process, route->session);
}

internal void
radvs_engine_router_flush_children_locked(RADVS_Engine *engine, DMN_Handle parent_process, RADVS_EngineSession *session)
{
  for (;;) {
    RADVS_EnginePendingRoute *child = 0;
    for EachNode(route, RADVS_EnginePendingRoute, engine->first_pending_route) {
      if (route->has_create_process && dmn_handle_match(route->parent_process, parent_process)) {
        child = route;
        break;
      }
    }
    if (child == 0) {
      break;
    }
    radvs_engine_router_flush_route_locked(engine, child, session);
  }
}

internal void
radvs_engine_router_remove_route_locked(RADVS_Engine *engine, RADVS_EngineRoute *route)
{
  RADVS_EngineRoute **next = &engine->first_route;
  for (; *next != 0; next = &(*next)->next) {
    if (*next == route) {
      *next = route->next;
      break;
    }
  }
}

internal void
radvs_engine_router_ingress(void *user_data, RADVS_DemonClient *active_demon, DMN_EventList events, B32 suppress_internal_halt)
{
  RADVS_Engine *engine = user_data;
  if (engine == 0) {
    return;
  }
  MutexScope (engine->router_mutex) {
    RADVS_EngineSession *active_session = 0;
    MutexScope (engine->sessions_mutex) {
      active_session = radvs_engine_router_session_from_demon_locked(engine, active_demon);
    }
    for EachNode(node, DMN_EventNode, events.first) {
      DMN_Event *event = &node->v;
      if (suppress_internal_halt && event->kind == DMN_EventKind_Halt) {
        continue;
      }
      if (event->kind == DMN_EventKind_CreateProcess) {
        RADVS_EngineRoute *route = radvs_engine_router_route_from_pid_locked(engine, event->system_process_id);
        if (route == 0 && !MemoryIsZeroStruct(&event->parent_process)) {
          RADVS_EngineRoute *parent = radvs_engine_router_route_from_process_locked(engine, event->parent_process);
          if (parent != 0) {
            route = radvs_engine_router_route_add_locked(engine, parent->session, event->system_process_id, dmn_handle_zero());
          }
        }
        if (route == 0) {
          radvs_engine_router_pending_record_locked(engine, event);
          continue;
        }
        route->process = event->process;
        RADVS_EnginePendingRoute *pending = radvs_engine_router_pending_from_process_locked(engine, event->process);
        if (pending != 0) {
          radvs_engine_router_pending_record_locked(engine, event);
          radvs_engine_router_flush_route_locked(engine, pending, route->session);
        } else {
          radvs_engine_router_deliver_locked(route->session, event);
          radvs_engine_router_flush_children_locked(engine, event->process, route->session);
        }
        continue;
      }
      if (!MemoryIsZeroStruct(&event->process)) {
        RADVS_EngineRoute *route = radvs_engine_router_route_from_process_locked(engine, event->process);
        if (route != 0) {
          radvs_engine_router_deliver_locked(route->session, event);
          if (event->kind == DMN_EventKind_ExitProcess) {
            radvs_engine_router_remove_route_locked(engine, route);
          }
        } else {
          radvs_engine_router_pending_record_locked(engine, event);
        }
      } else if (event->kind == DMN_EventKind_Error) {
        MutexScope (engine->sessions_mutex) {
          for (RADVS_EngineSession *session = engine->first_session; session != 0; session = session->next_in_host) {
            radvs_engine_router_deliver_locked(session, event);
          }
        }
      } else {
        radvs_engine_router_deliver_locked(active_session, event);
      }
    }
  }
}

internal void
radvs_engine_router_unregister_session(RADVS_Engine *engine, RADVS_EngineSession *session)
{
  MutexScope (engine->router_mutex) {
    for (RADVS_EngineRoute *route = engine->first_route; route != 0;) {
      RADVS_EngineRoute *next = route->next;
      if (route->session == session) {
        radvs_engine_router_remove_route_locked(engine, route);
      }
      route = next;
    }
  }
}

RADVS_Result
radvs_engine_alloc(RADVS_SymbolService *symbol_service, RADVS_Engine **out_engine)
{
  if (symbol_service == 0 || out_engine == 0) {
    return RADVS_Result_InvalidArgument;
  }
  *out_engine = 0;
  Arena *arena = arena_alloc(.reserve_size = KB(64), .commit_size = KB(16));
  RADVS_Engine *engine = push_array(arena, RADVS_Engine, 1);
  engine->arena = arena;
  engine->sessions_mutex = mutex_alloc();
  engine->router_mutex = mutex_alloc();
  engine->symbol_service = symbol_service;
  RADVS_Result result = radvs_demon_set_raw_event_router(radvs_engine_router_ingress, engine);
  if (result == RADVS_Result_Ok) {
    result = radvs_demon_set_launch_router(radvs_engine_router_launch_ingress, engine);
  }
  if (result == RADVS_Result_Ok) {
    *out_engine = engine;
  } else {
    mutex_release(engine->router_mutex);
    mutex_release(engine->sessions_mutex);
    arena_release(arena);
  }
  return result;
}

RADVS_Result
radvs_engine_release(RADVS_Engine *engine)
{
  if (engine == 0) {
    return RADVS_Result_InvalidArgument;
  }
  RADVS_Result result = RADVS_Result_Ok;
  MutexScope (engine->sessions_mutex) {
    if (engine->first_session != 0) {
      result = RADVS_Result_Busy;
    }
  }
  if (result == RADVS_Result_Ok) {
    radvs_demon_set_raw_event_router(0, 0);
    radvs_demon_set_launch_router(0, 0);
    mutex_release(engine->router_mutex);
    mutex_release(engine->sessions_mutex);
    arena_release(engine->arena);
  }
  return result;
}

RADVS_Result
radvs_engine_session_alloc(RADVS_Engine *engine, RADVS_EngineSession **out_session)
{
  if (engine == 0 || out_session == 0) {
    return RADVS_Result_InvalidArgument;
  }
  RADVS_Result result = radvs_engine_session_alloc_for_symbol(engine->symbol_service, out_session);
  if (result == RADVS_Result_Ok) {
    U64 session_id = 0;
    MutexScope (engine->sessions_mutex) {
      (*out_session)->host = engine;
      session_id = ++engine->next_session_id;
      (*out_session)->id = session_id;
      (*out_session)->next_in_host = engine->first_session;
      engine->first_session = *out_session;
    }
    result = radvs_demon_client_set_owner_id((*out_session)->demon, session_id);
    if (result != RADVS_Result_Ok) {
      radvs_engine_session_release(*out_session);
      *out_session = 0;
    }
  }
  return result;
}

RADVS_Result
radvs_engine_session_release(RADVS_EngineSession *session)
{
  if (session == 0) {
    return RADVS_Result_InvalidArgument;
  }
  RADVS_Engine *engine = session->host;
  if (engine != 0) {
    radvs_engine_router_unregister_session(engine, session);
    MutexScope (engine->sessions_mutex) {
      RADVS_EngineSession **next = &engine->first_session;
      for (; *next != 0; next = &(*next)->next_in_host) {
        if (*next == session) {
          *next = session->next_in_host;
          session->host = 0;
          session->next_in_host = 0;
          break;
        }
      }
    }
  }
  return radvs_engine_session_release__(session);
}

