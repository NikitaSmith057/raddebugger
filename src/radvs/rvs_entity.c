// This file is included only by rvs_engine.c after private engine/session types.

#include "radvs/rvs_entity.h"

internal void
rvs_entity_append_child_locked(RVS_Entity *parent, RVS_Entity *child)
{
  child->parent = parent;
  SLLQueuePush(parent->first_child, parent->last_child, child);
}

internal RVS_Program *
rvs_entity_program_from_id_locked(RVS_EntityStore *store, RVS_ProgramID id)
{
  for EachNode(entity, RVS_Entity, store->root.first_child) {
    if (entity->kind == RVS_EntityKind_Program) {
      RVS_Program *program = CastFromMember(RVS_Program, entity, entity);
      if (rvs_program_id_match(program->snapshot.id, id)) { return program; }
    }
  }
  return 0;
}

internal RVS_Process *
rvs_entity_process_from_node_locked(RVS_Entity *node, RVS_ProcessID id)
{
  for (RVS_Entity *entity = node->first_child; entity; entity = entity->next) {
    if (entity->kind == RVS_EntityKind_Process) {
      RVS_Process *process = CastFromMember(RVS_Process, entity, entity);
      if (rvs_process_id_match(process->snapshot.process, id)) { return process; }
    }
    RVS_Process *found = rvs_entity_process_from_node_locked(entity, id);
    if (found) { return found; }
  }
  return 0;
}

internal RVS_Process *
rvs_entity_process_from_id_locked(RVS_EntityStore *store, RVS_ProcessID id)
{
  return rvs_entity_process_from_node_locked(&store->root, id);
}

internal RVS_Thread *
rvs_entity_thread_from_node_locked(RVS_Entity *node, RVS_ThreadID id)
{
  for (RVS_Entity *entity = node->first_child; entity; entity = entity->next) {
    if (entity->kind == RVS_EntityKind_Thread) {
      RVS_Thread *thread = CastFromMember(RVS_Thread, entity, entity);
      if (rvs_thread_id_match(thread->snapshot.thread, id)) { return thread; }
    }
    RVS_Thread *found = rvs_entity_thread_from_node_locked(entity, id);
    if (found) { return found; }
  }
  return 0;
}

internal RVS_Thread *
rvs_entity_thread_from_id_locked(RVS_EntityStore *store, RVS_ThreadID id)
{
  return rvs_entity_thread_from_node_locked(&store->root, id);
}

internal RVS_Program *
rvs_entity_program_from_node_locked(RVS_Entity *node)
{
  for (; node; node = node->parent) {
    if (node->kind == RVS_EntityKind_Program) { return CastFromMember(RVS_Program, entity, node); }
  }
  return 0;
}

internal void
rvs_entity_store_init(RVS_EntityStore *store, Arena *arena, Mutex mutex)
{
  store->arena     = arena;
  store->mutex     = mutex;
  store->cv        = cond_var_alloc();
  store->root.kind = RVS_EntityKind_Root;
}

internal void
rvs_entity_store_close(RVS_EntityStore *store)
{
  mutex_take(store->mutex);
  store->is_closed = 1;
  cond_var_broadcast(store->cv);
  mutex_drop(store->mutex);
}

internal void
rvs_entity_store_release(RVS_EntityStore *store)
{
  cond_var_release(store->cv);
}

internal void
rvs_entity_changed_locked(RVS_EntityStore *store)
{
  cond_var_broadcast(store->cv);
}

internal RVS_Program *
rvs_entity_program_create_locked(RVS_EntityStore *store, RVS_ProgramID id, U32 pid)
{
  RVS_Program *program = rvs_entity_program_from_id_locked(store, id);
  if (program) { return program; }
  program = push_array(store->arena, RVS_Program, 1);
  program->entity.kind = RVS_EntityKind_Program;
  program->snapshot = (RVS_ProgramSnapshot){ .id = id, .pid = pid };
  rvs_entity_append_child_locked(&store->root, &program->entity);
  rvs_entity_changed_locked(store);
  return program;
}

internal RVS_Process *
rvs_entity_process_create_locked(RVS_EntityStore *store, RVS_ProgramID program_id, RVS_ProcessID id, RVS_ProcessID parent_id, U32 pid)
{
  if (rvs_process_id_is_zero(id)) { return 0; }
  RVS_Process *process = rvs_entity_process_from_id_locked(store, id);
  if (process) { return process; }
  RVS_Entity *parent = &store->root;
  RVS_ProgramID program = program_id;
  RVS_Process *parent_process = rvs_entity_process_from_id_locked(store, parent_id);
  if (parent_process) {
    parent = &parent_process->entity;
    program = parent_process->snapshot.program;
  } else if (!rvs_program_id_is_zero(program_id)) {
    RVS_Program *program_entity = rvs_entity_program_from_id_locked(store, program_id);
    if (program_entity) { parent = &program_entity->entity; }
  }
  process = push_array(store->arena, RVS_Process, 1);
  process->entity.kind = RVS_EntityKind_Process;
  process->snapshot = (RVS_ProcessSnapshot){ .program = program, .process = id, .parent_process = parent_id, .pid = pid };
  rvs_entity_append_child_locked(parent, &process->entity);
  rvs_entity_changed_locked(store);
  return process;
}

internal B32
rvs_entity_node_has_live_program_process_locked(RVS_Entity *node, RVS_ProgramID id)
{
  for (RVS_Entity *entity = node->first_child; entity; entity = entity->next) {
    if (entity->kind == RVS_EntityKind_Process) {
      RVS_Process *process = CastFromMember(RVS_Process, entity, entity);
      if (rvs_program_id_match(process->snapshot.program, id) && !process->snapshot.is_retired) { return 1; }
    }
    if (rvs_entity_node_has_live_program_process_locked(entity, id)) { return 1; }
  }
  return 0;
}

internal RVS_EntityProcessExit
rvs_entity_process_exit_locked(RVS_EntityStore *store, RVS_ProcessID id, U32 exit_code)
{
  RVS_EntityProcessExit result = {0};
  RVS_Process *process = rvs_entity_process_from_id_locked(store, id);
  if (!process || process->snapshot.is_retired) { return result; }
  process->snapshot.is_retired = 1;
  process->snapshot.exit_code = exit_code;
  result.program = process->snapshot.program;
  for (RVS_Entity *entity = process->entity.first_child; entity; entity = entity->next) {
    if (entity->kind == RVS_EntityKind_Thread) {
      (CastFromMember(RVS_Thread, entity, entity))->snapshot.is_retired = 1;
    }
  }
  RVS_Program *program = rvs_entity_program_from_id_locked(store, result.program);
  if (program && !rvs_entity_node_has_live_program_process_locked(&store->root, result.program)) {
    program->snapshot.is_retired = 1;
    program->snapshot.exit_code = exit_code;
    result.program_retired = 1;
  }
  rvs_entity_changed_locked(store);
  return result;
}

internal RVS_Thread *
rvs_entity_thread_create_locked(RVS_EntityStore *store, RVS_ProcessID process_id, RVS_ThreadID id, U32 tid)
{
  if (rvs_thread_id_is_zero(id) || rvs_entity_thread_from_id_locked(store, id)) { return 0; }
  RVS_Process *process = rvs_entity_process_from_id_locked(store, process_id);
  if (!process || process->snapshot.is_retired) { return 0; }
  RVS_Thread *thread = push_array(store->arena, RVS_Thread, 1);
  thread->entity.kind = RVS_EntityKind_Thread;
  thread->snapshot = (RVS_ThreadSnapshot){ .program = process->snapshot.program, .process = process_id, .thread = id, .tid = tid };
  rvs_entity_append_child_locked(&process->entity, &thread->entity);
  rvs_entity_changed_locked(store);
  return thread;
}

internal void
rvs_entity_thread_exit_locked(RVS_EntityStore *store, RVS_ThreadID id)
{
  RVS_Thread *thread = rvs_entity_thread_from_id_locked(store, id);
  if (thread && !thread->snapshot.is_retired) {
    thread->snapshot.is_retired = 1;
    rvs_entity_changed_locked(store);
  }
}

internal void
rvs_entity_program_ack_destroyed_locked(RVS_EntityStore *store, RVS_ProgramID id)
{
  RVS_Entity **entity_ptr = &store->root.first_child;
  while (*entity_ptr) {
    RVS_Entity *entity = *entity_ptr;
    if (entity->kind == RVS_EntityKind_Program && rvs_program_id_match((CastFromMember(RVS_Program, entity, entity))->snapshot.id, id)) {
      *entity_ptr = entity->next;
      if (store->root.last_child == entity) {
        store->root.last_child = 0;
        for (RVS_Entity *last = store->root.first_child; last; last = last->next) { store->root.last_child = last; }
      }
      rvs_entity_changed_locked(store);
      return;
    }
    entity_ptr = &entity->next;
  }
}

internal B32
rvs_entity_live_process_for_program_node_locked(RVS_Entity *node, RVS_ProgramID id, RVS_ProcessSnapshot *snapshot_out)
{
  for (RVS_Entity *entity = node->first_child; entity; entity = entity->next) {
    if (entity->kind == RVS_EntityKind_Process) {
      RVS_ProcessSnapshot snapshot = (CastFromMember(RVS_Process, entity, entity))->snapshot;
      if (rvs_program_id_match(snapshot.program, id) && !snapshot.is_retired) {
        *snapshot_out = snapshot;
        return 1;
      }
    }
    if (rvs_entity_live_process_for_program_node_locked(entity, id, snapshot_out)) { return 1; }
  }
  return 0;
}

internal B32
rvs_entity_live_process_for_program_locked(RVS_EntityStore *store, RVS_ProgramID id, RVS_ProcessSnapshot *snapshot_out)
{
  return rvs_entity_live_process_for_program_node_locked(&store->root, id, snapshot_out);
}

internal U64
rvs_entity_live_process_count_node_locked(RVS_Entity *node)
{
  U64 count = 0;
  for (RVS_Entity *entity = node->first_child; entity; entity = entity->next) {
    if (entity->kind == RVS_EntityKind_Process && !(CastFromMember(RVS_Process, entity, entity))->snapshot.is_retired) {
      count += 1;
    }
    count += rvs_entity_live_process_count_node_locked(entity);
  }
  return count;
}

internal void
rvs_entity_copy_live_processes_node_locked(RVS_Entity *node, RVS_ProcessSnapshot *snapshots, U64 *index)
{
  for (RVS_Entity *entity = node->first_child; entity; entity = entity->next) {
    if (entity->kind == RVS_EntityKind_Process) {
      RVS_ProcessSnapshot snapshot = (CastFromMember(RVS_Process, entity, entity))->snapshot;
      if (!snapshot.is_retired) { snapshots[(*index)++] = snapshot; }
    }
    rvs_entity_copy_live_processes_node_locked(entity, snapshots, index);
  }
}

internal void
rvs_entity_copy_live_processes_locked(RVS_EntityStore *store, Arena *arena, RVS_ProcessSnapshot **snapshots_out,
                                      U64 *snapshots_count_out)
{
  U64 count = rvs_entity_live_process_count_node_locked(&store->root);
  RVS_ProcessSnapshot *snapshots = count ? push_array(arena, RVS_ProcessSnapshot, count) : 0;
  U64 index = 0;
  rvs_entity_copy_live_processes_node_locked(&store->root, snapshots, &index);
  AssertAlways(index == count);
  *snapshots_out = snapshots;
  *snapshots_count_out = count;
}

internal B32
rvs_entity_program_snapshot_locked(RVS_EntityStore *store, RVS_ProgramID id, RVS_ProgramSnapshot *snapshot_out)
{
  RVS_Program *program = rvs_entity_program_from_id_locked(store, id);
  if (!program) { return 0; }
  *snapshot_out = program->snapshot;
  return 1;
}

internal RVS_Result
rvs_entity_copy_programs(RVS_EntityStore *store, Arena *arena, RVS_ProgramSnapshot **snapshots_out, U64 *snapshots_count_out)
{
  mutex_take(store->mutex);
  if (store->is_closed) {
    mutex_drop(store->mutex);
    return RVS_Result_EngineStopped;
  }

  U64 count = 0;
  for EachNode(entity, RVS_Entity, store->root.first_child) {
    if (entity->kind == RVS_EntityKind_Program) { count += 1; }
  }
  RVS_ProgramSnapshot *snapshots = count ? push_array(arena, RVS_ProgramSnapshot, count) : 0;
  U64 index = 0;
  for EachNode(entity, RVS_Entity, store->root.first_child) {
    if (entity->kind == RVS_EntityKind_Program) {
      snapshots[index++] = (CastFromMember(RVS_Program, entity, entity))->snapshot;
    }
  }
  mutex_drop(store->mutex);

  *snapshots_out = snapshots;
  *snapshots_count_out = count;
  return RVS_Result_Ok;
}

internal RVS_Result
rvs_entity_fetch_program(RVS_EntityStore *store, RVS_ProgramID id, U64 wait_us, RVS_ProgramSnapshot *snapshot_out)
{
  U64 endt_us = wait_us == max_U64 ? max_U64 : now_time_us() + wait_us;
  mutex_take(store->mutex);
  for (;;) {
    RVS_Program *program = rvs_entity_program_from_id_locked(store, id);
    if (program) { *snapshot_out = program->snapshot; mutex_drop(store->mutex); return RVS_Result_Ok; }
    if (store->is_closed) { mutex_drop(store->mutex); return RVS_Result_EngineStopped; }
    if (!cond_var_wait(store->cv, store->mutex, endt_us)) { mutex_drop(store->mutex); return RVS_Result_Timeout; }
  }
}

internal RVS_Result
rvs_entity_fetch_process(RVS_EntityStore *store, RVS_ProcessID id, U64 wait_us, RVS_ProcessSnapshot *snapshot_out)
{
  U64 endt_us = wait_us == max_U64 ? max_U64 : now_time_us() + wait_us;
  mutex_take(store->mutex);
  for (;;) {
    RVS_Process *process = rvs_entity_process_from_id_locked(store, id);
    if (process) { *snapshot_out = process->snapshot; mutex_drop(store->mutex); return RVS_Result_Ok; }
    if (store->is_closed) { mutex_drop(store->mutex); return RVS_Result_EngineStopped; }
    if (!cond_var_wait(store->cv, store->mutex, endt_us)) { mutex_drop(store->mutex); return RVS_Result_Timeout; }
  }
}

internal RVS_Result
rvs_entity_fetch_thread(RVS_EntityStore *store, RVS_ThreadID id, U64 wait_us, RVS_ThreadSnapshot *snapshot_out)
{
  U64 endt_us = wait_us == max_U64 ? max_U64 : now_time_us() + wait_us;
  mutex_take(store->mutex);
  for (;;) {
    RVS_Thread *thread = rvs_entity_thread_from_id_locked(store, id);
    if (thread) { *snapshot_out = thread->snapshot; mutex_drop(store->mutex); return RVS_Result_Ok; }
    if (store->is_closed) { mutex_drop(store->mutex); return RVS_Result_EngineStopped; }
    if (!cond_var_wait(store->cv, store->mutex, endt_us)) { mutex_drop(store->mutex); return RVS_Result_Timeout; }
  }
}
