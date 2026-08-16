// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
// Includes

#include "radvs/rvs_entity.h"

////////////////////////////////

internal void
rvs_program_ptr_list_push_node(RVS_ProgramPtrList *list, RVS_ProgramPtrNode *n)
{
  DLLPushBack(list->first, list->last, n);
  list->count += 1;
}

internal void
rvs_process_ptr_list_push_node(RVS_ProcessPtrList *list, RVS_ProcessPtrNode *n)
{
  DLLPushBack(list->first, list->last, n);
  list->count += 1;
}

internal void
rvs_thread_ptr_list_push_node(RVS_ThreadPtrList *list, RVS_ThreadPtrNode *n)
{
  DLLPushBack(list->first, list->last, n);
  list->count += 1;
}

internal void
rvs_module_ptr_list_push_node(RVS_ModulePtrList *list, RVS_ModulePtrNode *n)
{
  DLLPushBack(list->first, list->last, n);
  list->count += 1;
}

internal void
rvs_program_ptr_list_remove_node(RVS_ProgramPtrList *list, RVS_ProgramPtrNode *n)
{
  DLLRemove(list->first, list->last, n);
  list->count -= 1;
}

internal void
rvs_process_ptr_list_remove_node(RVS_ProcessPtrList *list, RVS_ProcessPtrNode *n)
{
  DLLRemove(list->first, list->last, n);
  list->count -= 1;
}

internal void
rvs_thread_ptr_list_remove_node(RVS_ThreadPtrList *list, RVS_ThreadPtrNode *n)
{
  DLLRemove(list->first, list->last, n);
  list->count -= 1;
}

internal void
rvs_module_ptr_list_remove_node(RVS_ModulePtrList *list, RVS_ModulePtrNode *n)
{
  DLLRemove(list->first, list->last, n);
  list->count -= 1;
}

////////////////////////////////

internal RVS_Program *
rvs_program_from_process(RVS_Process *process)
{
  RVS_Entity *e;
  for (e = RVS_EntityFromPtr(process); e != 0 && e->parent != 0; e = e->parent);
  AssertAlways(e->kind == RVS_EntityKind_Program);
  return &e->program;
}

////////////////////////////////

internal RVS_EntityStore *
rvs_entity_store_alloc(void)
{
  Arena *arena = arena_alloc(.name = "Entity Store");
  RVS_EntityStore *result = push_array(arena, RVS_EntityStore, 1);
  result->arena          = arena;
  result->program_by_pid = hash_table_init(arena, 1024);
  for EachElement(i, result->entity_by_id) {
    result->entity_by_id[i] = hash_table_init(arena, 1024);
  }
  return result;
}

internal void
rvs_entity_store_release(RVS_EntityStore *store)
{
  arena_release(store->arena);
}

////////////////////////////////

internal RVS_Program *
rvs_program_from_pid(RVS_EntityStore *store, U32 pid)
{
  return hash_table_search_u64_raw(store->program_by_pid, pid);
}

internal RVS_Process *
rvs_process_from_id(RVS_EntityStore *store, RVS_ProcessID id)
{
  return hash_table_search_string_raw(store->entity_by_id[RVS_EntityKind_Process], str8_struct(&id));
}

internal RVS_Thread *
rvs_thread_from_id(RVS_EntityStore *store, RVS_ThreadID id)
{
  return hash_table_search_string_raw(store->entity_by_id[RVS_EntityKind_Thread], str8_struct(&id));
}

internal RVS_Module *
rvs_module_from_id(RVS_EntityStore *store, RVS_ModuleID id)
{
  return hash_table_search_string_raw(store->entity_by_id[RVS_EntityKind_Module], str8_struct(&id));
}

////////////////////////////////

internal RVS_Entity *
rvs_entity_alloc(RVS_EntityStore *store)
{
  RVS_EntityPtrNode *n = store->free_list;
  if (n) {
    SLLStackPop(store->free_list);
  } else {
    RVS_Entity *entity = push_array(store->arena, RVS_Entity, 1);
    n = &entity->entity_ptr;
    n->v = entity;
  }
  return n->v;
}

internal void *
rvs_entity_alloc_raw(RVS_EntityStore *store)
{
  RVS_Entity *entity = rvs_entity_alloc(store);
  return &entity->first;
}

internal void
rvs_entity_recycle(RVS_EntityStore *store, RVS_Entity *entity)
{
  MemoryZeroStruct(entity);
  entity->entity_ptr.v = entity;
  SLLStackPush(store->free_list, &entity->entity_ptr);
}

////////////////////////////////

internal RVS_Event
rvs_event_from_backend_event(RVS_EntityStore *entities, DMN_Event event)
{
  RVS_Event result = { .raw_event = event };

  RVS_Process *process = rvs_process_from_id(entities, rvs_process_id_from_handle(event.process));
  RVS_Thread  *thread  = rvs_thread_from_id(entities, rvs_thread_id_from_handle(event.thread));
  RVS_Module  *module  = rvs_module_from_id(entities, rvs_module_id_from_handle(event.module));

  if (thread) {
    result.program = thread->program;
    result.process = thread->process;
    result.thread  = thread->id;
  }
  if (module && !process) {
    process = rvs_process_from_id(entities, module->process);
  }
  if (process) {
    RVS_Program *program = rvs_program_from_process(process);
    result.program = program->id;
    result.process = process->id;
  }

  if (MemoryIsZeroStruct(&result.process)) { result.process = rvs_process_id_from_handle(event.process); }
  if (MemoryIsZeroStruct(&result.thread))  { result.thread  = rvs_thread_id_from_handle(event.thread);   }

  if (event.kind == DMN_EventKind_ExitProcess) { result.process_exited.exit_code = event.code;       }
  if (event.kind == DMN_EventKind_Error)       { result.error.kind               = event.error_kind; }

  return result;
}

internal void
rvs_entity_store_emit_backend_event(Arena *arena, RVS_EntityStore *entities, DMN_Event event, RVS_EventList *events_out)
{
  rvs_event_list_push(arena, events_out, rvs_event_from_backend_event(entities, event));
}

////////////////////////////////

internal RVS_Result
rvs_entity_store_apply_backend_event(Arena *arena, RVS_EntityStore *entities, DMN_Event event, RVS_EventList *events_out)
{
  RVS_Result result = RVS_Result_Null;

  switch (event.kind) {
  case DMN_EventKind_Null: {
    // null is the empty event and intentionally produces no normalized event
  } break;

  case DMN_EventKind_CreateProcess: {
    RVS_ProcessID process_id = rvs_process_id_from_handle(event.process);
    RVS_Process  *process    = rvs_process_from_id(entities, process_id);

    if (process == 0 || MemoryIsZeroStruct(&process_id)) {
      result = RVS_Result_Error;
      break;
    }

    RVS_Process *parent_process = 0;
    RVS_Program *program = rvs_program_from_pid(entities, event.code);

    // process without a known parent starts a new program
    if (program == 0) {
      RVS_Entity *program_entity = rvs_entity_alloc(entities);
      program_entity->kind = RVS_EntityKind_Program;

      program = &program_entity->program;
      program->id.value = ++entities->next_program_id;
      program->pid      = event.code;

      RVS_ProgramPtrNode *program_node = &program_entity->program_ptr;
      program_node->v = program;
      rvs_program_ptr_list_push_node(&entities->programs, program_node);
      hash_table_push_u64_raw(entities->arena, entities->program_by_pid, program->pid, program);
    }

    // alloc entity for new process
    RVS_Entity *process_entity = rvs_entity_alloc(entities);
    process_entity->kind   = RVS_EntityKind_Process;
    process_entity->parent = parent_process ? RVS_EntityFromPtr(parent_process) : RVS_EntityFromPtr(program);

    // fill out process
    process = &process_entity->process;
    process->id = process_id;

    // append process to the parent
    RVS_ProcessPtrNode *process_node = &process_entity->process_ptr;
    process_node->v = process;
    RVS_ProcessPtrList *process_list = parent_process ? &parent_process->processes : &program->processes;
    rvs_process_ptr_list_push_node(process_list, process_node);

    // id -> process mapping
    hash_table_push_string_raw(entities->arena, entities->entity_by_id[RVS_EntityKind_Process], str8_struct(&process->id), process);

    rvs_entity_store_emit_backend_event(arena, entities, event, events_out);

    result = RVS_Result_Ok;
  } break;

  case DMN_EventKind_ExitProcess: {
    RVS_Process *process = rvs_process_from_id(entities, rvs_process_id_from_handle(event.process));

    if (process == 0) {
      result = RVS_Result_Error;
      break;
    }

    RVS_Entity  *process_entity = RVS_EntityFromPtr(process);
    RVS_Entity  *parent_entity  = process_entity->parent;
    RVS_Program *program        = rvs_program_from_process(process);

    // process exit owns the cleanup of any descendants the backend omitted
    for (RVS_ProcessPtrNode *node = process->processes.first, *next; node; node = next) {
      next = node->next;
      DMN_Event exit_process = event;
      exit_process.process = rvs_handle_from_process_id(node->v->id);
      exit_process.thread  = (DMN_Handle){0};
      exit_process.module  = (DMN_Handle){0};
      exit_process.code    = 0; // TODO: mark child process that it does not have an exit code
      rvs_entity_store_apply_backend_event(arena, entities, exit_process, events_out);
    }

    // cleanup process owned threads
    for (RVS_ThreadPtrNode *node = process->threads.first, *next; node; node = next) {
      next = node->next;
      DMN_Event exit_thread = {
        .kind    = DMN_EventKind_ExitThread,
        .process = rvs_handle_from_process_id(process->id),
        .thread  = rvs_handle_from_thread_id(node->v->id),
        .code    = 0, // TODO: mark thread that it does not have an exit code
      };
      rvs_entity_store_apply_backend_event(arena, entities, exit_thread, events_out);
    }

    // cleanup process owned modules
    for (RVS_ModulePtrNode *node = process->modules.first, *next; node; node = next) {
      next = node->next;
      DMN_Event unload_module = {
        .kind    = DMN_EventKind_UnloadModule,
        .process = rvs_handle_from_process_id(process->id),
        .module  = rvs_handle_from_module_id(node->v->id),
      };
      rvs_entity_store_apply_backend_event(arena, entities, unload_module, events_out);
    }

    // emit create-process-event after its manual child cleanup but before
    // its entity is removed, so all normalized ids are available
    rvs_entity_store_emit_backend_event(arena, entities, event, events_out);

    // remove process from the parent list
    RVS_ProcessPtrList *process_list = 0;
    if (parent_entity->kind == RVS_EntityKind_Program) {
      process_list = &parent_entity->program.processes;
    } else if (parent_entity->kind == RVS_EntityKind_Process) {
      process_list = &parent_entity->process.processes;
    }
    rvs_process_ptr_list_remove_node(process_list, &process_entity->process_ptr);
    hash_table_purge_string(entities->entity_by_id[RVS_EntityKind_Process], str8_struct(&process->id));

    // on last process exit update the program state to be retired
    if (program->processes.count == 0 && !program->is_retired) {
      program->exit_code  = event.code;
      program->is_retired = 1;
      hash_table_purge_u64(entities->program_by_pid, program->pid);
    }

    // recycle process entity
    rvs_entity_recycle(entities, process_entity);

    result = RVS_Result_Ok;
  } break;

  case DMN_EventKind_CreateThread: {
    RVS_Process  *process   = rvs_process_from_id(entities, rvs_process_id_from_handle(event.process));
    RVS_ThreadID  thread_id = rvs_thread_id_from_handle(event.thread);
    RVS_Thread   *thread    = rvs_thread_from_id(entities, thread_id);

    if (process && !thread && !MemoryIsZeroStruct(&thread_id)) {
      // alloc thread entity
      RVS_Entity *thread_entity = rvs_entity_alloc(entities);
      thread_entity->kind   = RVS_EntityKind_Thread;
      thread_entity->parent = RVS_EntityFromPtr(process);

      // fill out thread
      thread = &thread_entity->thread;
      thread->program = rvs_program_from_process(process)->id;
      thread->process = process->id;
      thread->id      = thread_id;
      thread->tid     = event.code;

      // append thread to the process thread list
      RVS_ThreadPtrNode *thread_node = &thread_entity->thread_ptr;
      thread_node->v = thread;
      rvs_thread_ptr_list_push_node(&process->threads, thread_node);

      // id -> thread mapping
      hash_table_push_string_raw(entities->arena, entities->entity_by_id[RVS_EntityKind_Thread], str8_struct(&thread->id), thread);

      rvs_entity_store_emit_backend_event(arena, entities, event, events_out);

      result = RVS_Result_Ok;
    } else {
      // TODO: log invalid event sequence
    }
  } break;

  case DMN_EventKind_ExitThread: {
    RVS_Thread *thread = rvs_thread_from_id(entities, rvs_thread_id_from_handle(event.thread));
    if (thread) {
      rvs_entity_store_emit_backend_event(arena, entities, event, events_out);

      RVS_Entity  *thread_entity = RVS_EntityFromPtr(thread);
      RVS_Process *process       = &thread_entity->parent->process;

      // on exit thread removes itself from the process thread list
      rvs_thread_ptr_list_remove_node(&process->threads, &thread_entity->thread_ptr);

      // purge id -> thread mapping
      hash_table_purge_string(entities->entity_by_id[RVS_EntityKind_Thread], str8_struct(&thread->id));

      // recycle thread entity
      rvs_entity_recycle(entities, thread_entity);

      result = RVS_Result_Ok;
    } else {
      // TODO: log invalid event sequence
    }
  } break;

  case DMN_EventKind_LoadModule: {
    RVS_Process  *process   = rvs_process_from_id(entities, rvs_process_id_from_handle(event.process));
    RVS_ModuleID  module_id = rvs_module_id_from_handle(event.module);
    RVS_Module   *module    = rvs_module_from_id(entities, module_id);

    if (process && !module && !MemoryIsZeroStruct(&module_id)) {
      // alloc module entity
      RVS_Entity *module_entity = rvs_entity_alloc(entities);
      module_entity->kind   = RVS_EntityKind_Module;
      module_entity->parent = RVS_EntityFromPtr(process);

      // fill out module
      module = &module_entity->module;
      module->id      = module_id;
      module->process = process->id;
      module->base    = event.address;
      module->size    = event.size;

      // append module to the process module list
      RVS_ModulePtrNode *node = &module_entity->module_ptr;
      node->v = module;
      rvs_module_ptr_list_push_node(&process->modules, node);

      // id -> module mapping
      hash_table_push_string_raw(entities->arena, entities->entity_by_id[RVS_EntityKind_Module], str8_struct(&module->id), module);

      result = RVS_Result_Ok;
    } else {
      // TODO: log invalid event sequence
    }

    rvs_entity_store_emit_backend_event(arena, entities, event, events_out);
  } break;

  case DMN_EventKind_UnloadModule: {
    rvs_entity_store_emit_backend_event(arena, entities, event, events_out);

    RVS_Module *module = rvs_module_from_id(entities, rvs_module_id_from_handle(event.module));
    if (module) {
      // get module & process
      RVS_Entity  *module_entity = RVS_EntityFromPtr(module);
      RVS_Process *process       = &module_entity->parent->process;

      // remove module from the process module list
      rvs_module_ptr_list_remove_node(&process->modules, &module_entity->module_ptr);

      // purge id -> module mapping
      hash_table_purge_string(entities->entity_by_id[RVS_EntityKind_Module], str8_struct(&module->id));

      // recycle module entity
      rvs_entity_recycle(entities, module_entity);

      result = RVS_Result_Ok;
    } else {
      // TODO: log invalid event sequence
    }
  } break;

  case DMN_EventKind_HandshakeComplete:
  case DMN_EventKind_Error:
  case DMN_EventKind_ModuleDebugInfo:
  case DMN_EventKind_Breakpoint:
  case DMN_EventKind_Trap:
  case DMN_EventKind_SingleStep:
  case DMN_EventKind_Exception:
  case DMN_EventKind_Halt:
  case DMN_EventKind_Memory:
  case DMN_EventKind_DebugString:
  case DMN_EventKind_SetThreadName:
  case DMN_EventKind_SetThreadColor:
  case DMN_EventKind_SetBreakpoint:
  case DMN_EventKind_UnsetBreakpoint:
  case DMN_EventKind_SetVAddrRangeNote:
  case DMN_EventKind_UserLo: {
    rvs_entity_store_emit_backend_event(arena, entities, event, events_out);
    result = RVS_Result_Ok;
  } break;

  case DMN_EventKind_COUNT:
  default: result = RVS_Result_InvalidArgument;
  }

  return result;
}

////////////////////////////////

