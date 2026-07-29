// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

// Included by radvs_bridge_main.c after the RAD base, OS, demon, and linker
// implementations. DEMON has process-global state, so this file owns one
// worker and exposes one lightweight endpoint to each RADVS engine session.

#include "radvs/radvs_demon.h"
#include "radvs/radvs_request.h"

typedef struct RADVS_DemonRequest RADVS_DemonRequest;
typedef struct RADVS_DemonTarget  RADVS_DemonTarget;
typedef struct RADVS_DemonPendingEvent RADVS_DemonPendingEvent;
typedef struct RADVS_DemonPendingRoute RADVS_DemonPendingRoute;

struct RADVS_DemonRequest
{
  RADVS_Request      base;

  RADVS_DemonClient  *client;
  RADVS_DemonCommand command;
  B32                break_already_signaled;
  U64                control_epoch;

  ProcessLaunchParams launch;
  U32                 pid;

  DMN_Handle *process_handles;
  U64         process_handle_count;
  DMN_Handle  step_thread;
  DMN_Trap   *traps;
  U64         trap_count;

  U64 exit_code;
};

struct RADVS_DemonClient
{
  // Scheduler control token keyed by an Engine-owned session ID.
  Arena *arena;
  RADVS_DemonClient *next;
  U64 owner_id;
  B32 released;
  B32 release_requested;
  B32 run_intent;
  RADVS_DemonClientState state;
};

struct RADVS_DemonTarget
{
  RADVS_DemonTarget *next;
  U64 owner_id;
  DMN_Handle handle;
  U32 process_pid;
};

// TODO: use DMN_EventNode instead
//
// This is an ownership queue, not a second process model. Events remain raw
// until a parent target identifies the Engine session that owns them.
struct RADVS_DemonPendingEvent
{
  RADVS_DemonPendingEvent *next;
  DMN_Event                v;
};

struct RADVS_DemonPendingRoute
{
  RADVS_DemonPendingRoute *next;
  DMN_Handle               process;
  DMN_Handle               parent_process;
  U32                      system_process_id;
  B32                      has_create_process;
  RADVS_DemonPendingEvent *first;
  RADVS_DemonPendingEvent *last;
  U64                      event_count;
};

struct RADVS_Demon
{
  Arena              *arena;
  Arena              *event_arena;
  Arena              *pending_event_arena;
  RADVS_RequestQueue  requests;
  RADVS_DemonTarget  *targets;
  RADVS_DemonTarget  *free_target;
  RADVS_DemonPendingRoute *pending_routes;
  RADVS_DemonClient  *clients;
  B32                 run_active;
  U64                 next_control_epoch;
  U64                 active_control_epoch;
  RADVS_DemonClient  *active_client;
  RADVS_DemonClient  *resume_client;
  DMN_Trap           *active_traps;
  U64                 active_trap_count;
  DMN_Trap           *resume_traps;
  U64                 resume_trap_count;
  RADVS_DemonRawEventRouter *raw_event_router;
  void               *raw_event_router_user_data;
  RADVS_DemonLaunchRouter *launch_router;
  void               *launch_router_user_data;
  B32                 interrupt_requested;
  B32                 interrupt_visible;
  B32                 detach_pending;
  Thread              worker;
};

typedef enum
{
  RADVS_DemonState_Uninitialized,
  RADVS_DemonState_Initializing,
  RADVS_DemonState_Ready,
  RADVS_DemonState_Releasing,
  RADVS_DemonState_Failed,
} RADVS_DemonState;

static RADVS_DemonState  radvs_demon_state = RADVS_DemonState_Uninitialized;
static RADVS_Demon      *radvs_demon       = 0;
static U32               radvs_demon_lifecycle_gate = 0;

internal void
radvs_demon_lifecycle_gate_take(void)
{
  for (; ins_atomic_u32_eval_cond_assign(&radvs_demon_lifecycle_gate, 1, 0) != 0;) {
    Sleep(0);
  }
}

internal void
radvs_demon_lifecycle_gate_drop(void)
{
  ins_atomic_u32_eval_assign(&radvs_demon_lifecycle_gate, 0);
}

internal RADVS_DemonRequest *
radvs_demon_request_alloc_locked(RADVS_Demon *dmn, RADVS_DemonClient *client, RADVS_DemonCommand command)
{
  RADVS_DemonRequest *request = radvs_request_queue_alloc_locked_struct(&dmn->requests, RADVS_DemonRequest);
  request->client  = client;
  request->command = command;
  return request;
}

internal U32
radvs_demon_command_priority(RADVS_DemonCommand command)
{
  switch (command) {
  case RADVS_DemonCommand_Terminate: return 0;
  case RADVS_DemonCommand_Break:     return 1;
  default:                            return 2;
  }
}

internal RADVS_DemonRequest *
radvs_demon_request_pop_next_locked(RADVS_Demon *dmn)
{
  RADVS_Request *best = 0;
  RADVS_Request *best_prev = 0;
  RADVS_Request *prev = 0;
  U32 best_priority = max_U32;
  for EachNode(it, RADVS_Request, dmn->requests.first) {
    RADVS_DemonRequest *request = (RADVS_DemonRequest *)it;
    U32 priority = radvs_demon_command_priority(request->command);
    if (priority < best_priority) {
      best = it;
      best_prev = prev;
      best_priority = priority;
    }
    prev = it;
  }
  if (best != 0) {
    if (best_prev != 0) {
      best_prev->next = best->next;
    } else {
      dmn->requests.first = best->next;
    }
    if (dmn->requests.last == best) {
      dmn->requests.last = best_prev;
    }
    best->next = 0;
  }
  return (RADVS_DemonRequest *)best;
}

internal RADVS_Result
radvs_demon_request_submit(RADVS_Demon *dmn, RADVS_DemonRequest *request, U32 *out_pid)
{
  RADVS_Result result = RADVS_Result_Busy;
  MutexScope (dmn->requests.mutex) {
    if (ins_atomic_u32_eval(&radvs_demon_state) == RADVS_DemonState_Ready &&
         request->client != 0 && !request->client->released &&
         (!request->client->release_requested || request->command == RADVS_DemonCommand_ReleaseClient)) {
      if (dmn->run_active) {
        request->control_epoch = dmn->active_control_epoch;
        if (request->command == RADVS_DemonCommand_Break && dmn->active_client == request->client) {
          request->break_already_signaled = 1;
          if (!dmn->interrupt_requested) {
            dmn->interrupt_requested = 1;
            dmn->interrupt_visible = 1;
            dmn_halt(0, 0);
          }
        } else if (!dmn->interrupt_requested) {
          // A scheduler interrupt is an implementation detail. Its Halt event is
          // suppressed and the interrupted owner's run intent is resumed later.
          dmn->interrupt_requested = 1;
          dmn->interrupt_visible = 0;
          dmn_halt(0, 0);
        }
      }
      result = radvs_request_queue_submit_locked(&dmn->requests, &request->base);
      if (out_pid != 0) {
        *out_pid = request->pid;
      }
    }
  }
  return result;
}

internal RADVS_DemonTarget *
radvs_demon_target_alloc(RADVS_Demon *dmn, U64 owner_id, U32 process_pid)
{
  RADVS_DemonTarget *target = dmn->free_target;
  if (target != 0) {
    SLLStackPop(dmn->free_target);
    MemoryZeroStruct(target);
  } else {
    target = push_array(dmn->arena, RADVS_DemonTarget, 1);
  }
  target->owner_id    = owner_id;
  target->process_pid = process_pid;
  SLLStackPush(dmn->targets, target);
  return target;
}

internal void
radvs_demon_target_release(RADVS_Demon *dmn, RADVS_DemonTarget *target)
{
  RADVS_DemonTarget **next = &dmn->targets;
  for (; *next != 0; next = &(*next)->next) {
    if (*next == target) {
      *next = target->next;
      MemoryZeroStruct(target);
      SLLStackPush(dmn->free_target, target);
      break;
    }
  }
}

internal RADVS_DemonTarget *
radvs_demon_target_from_handle(RADVS_Demon *dmn, DMN_Handle handle)
{
  for EachNode(target, RADVS_DemonTarget, dmn->targets) {
    if (dmn_handle_match(target->handle, handle)) {
      return target;
    }
  }
  return 0;
}

internal RADVS_DemonTarget *
radvs_demon_target_from_process_pid(RADVS_Demon *dmn, U32 process_pid)
{
  for EachNode(target, RADVS_DemonTarget, dmn->targets) {
    if (MemoryIsZeroStruct(&target->handle) && target->process_pid == process_pid) {
      return target;
    }
  }
  return 0;
}

internal B32
radvs_demon_target_is_owned_by(RADVS_Demon *dmn, RADVS_DemonClient *client, DMN_Handle handle)
{
  RADVS_DemonTarget *target = radvs_demon_target_from_handle(dmn, handle);
  return target != 0 && target->owner_id == client->owner_id;
}

// TODO: use hash map to map demon handle to 
internal RADVS_DemonPendingRoute *
radvs_demon_pending_route_from_process(RADVS_Demon *dmn, DMN_Handle process)
{
  for EachNode(route, RADVS_DemonPendingRoute, dmn->pending_routes) {
    if (dmn_handle_match(route->process, process)) {
      return route;
    }
  }
  return 0;
}

internal RADVS_DemonPendingRoute *
radvs_demon_pending_route_ensure(RADVS_Demon *dmn, DMN_Handle process)
{
  RADVS_DemonPendingRoute *route = radvs_demon_pending_route_from_process(dmn, process);
  if (route == 0) {
    route = push_array(dmn->pending_event_arena, RADVS_DemonPendingRoute, 1);
    route->process = process;
    SLLStackPush(dmn->pending_routes, route);
  }
  return route;
}

internal void
radvs_demon_pending_event_push(RADVS_Demon *dmn, RADVS_DemonPendingRoute *route, const DMN_Event *event)
{
  RADVS_DemonPendingEvent *node = push_array(dmn->pending_event_arena, RADVS_DemonPendingEvent, 1);
  node->v = *event;
  node->v.string = push_str8_copy(dmn->pending_event_arena, event->string);
  if (event->module_info != 0 && event->module_info != &dmn_module_info_nil) {
    node->v.module_info = push_array(dmn->pending_event_arena, DMN_ModuleInfo, 1);
    MemoryCopyStruct(node->v.module_info, event->module_info);
    node->v.module_info->module_path = push_str8_copy(dmn->pending_event_arena, event->module_info->module_path);
    node->v.module_info->debug_info_path = push_str8_copy(dmn->pending_event_arena, event->module_info->debug_info_path);
  }
  SLLQueuePush(route->first, route->last, node);
  route->event_count += 1;
}

internal void
radvs_demon_pending_route_record(RADVS_Demon *dmn, const DMN_Event *event)
{
  DMN_Handle process = event->process;
  if (MemoryIsZeroStruct(&process)) {
    return;
  }
  RADVS_DemonPendingRoute *route = radvs_demon_pending_route_ensure(dmn, process);
  if (route->event_count == 0) {
    log_infof("radvs: holding unroutable process events: process:[0x%I64x]\n", event->process.u64[0]);
  }
  if (event->kind == DMN_EventKind_CreateProcess) {
    route->parent_process = event->parent_process;
    route->system_process_id = event->system_process_id;
    route->has_create_process = 1;
  }
  radvs_demon_pending_event_push(dmn, route, event);
}

internal void
radvs_demon_pending_route_remove(RADVS_Demon *dmn, RADVS_DemonPendingRoute *route)
{
  RADVS_DemonPendingRoute **next = &dmn->pending_routes;
  for (; *next != 0; next = &(*next)->next) {
    if (*next == route) {
      *next = route->next;
      route->next = 0;
      break;
    }
  }
}

internal DMN_HandleArray
radvs_demon_handles_for_client(Arena *arena, RADVS_Demon *dmn, RADVS_DemonClient *client)
{
  U64 count = 0;
  for EachNode(target, RADVS_DemonTarget, dmn->targets) {
    if (target->owner_id == client->owner_id && !MemoryIsZeroStruct(&target->handle)) {
      count += 1;
    }
  }

  DMN_HandleArray result = {0};
  if (count != 0) {
    result.handles = push_array_no_zero(arena, DMN_Handle, count);
    result.count = count;
    for EachNode(target, RADVS_DemonTarget, dmn->targets) {
      if (target->owner_id == client->owner_id && !MemoryIsZeroStruct(&target->handle)) {
        result.handles[result.count - count] = target->handle;
        count -= 1;
      }
    }
  }
  return result;
}

internal RADVS_Result
radvs_demon_run_ctrls_from_request(Arena *arena, RADVS_Demon *dmn, RADVS_DemonRequest *request, DMN_RunCtrls *out_run_ctrls)
{
  MemoryZeroStruct(out_run_ctrls);
  out_run_ctrls->run_entities_are_processes = 1;
  if (request->command == RADVS_DemonCommand_Step) {
    if (MemoryIsZeroStruct(&request->step_thread)) {
      return RADVS_Result_InvalidArgument;
    }
    out_run_ctrls->single_step_thread = request->step_thread;
  }
  if (request->process_handle_count != 0) {
    for EachIndex(index, request->process_handle_count) {
      if (!radvs_demon_target_is_owned_by(dmn, request->client, request->process_handles[index])) {
        return RADVS_Result_InvalidArgument;
      }
    }
    out_run_ctrls->run_entities = request->process_handles;
    out_run_ctrls->run_entity_count = request->process_handle_count;
    out_run_ctrls->run_entities_are_unfrozen = 1;
  } else {
    // A session-wide Continue runs every target it owns while freezing known
    // targets belonging to other sessions.
    U64 foreign_count = 0;
    for EachNode(target, RADVS_DemonTarget, dmn->targets) {
      if (target->owner_id != request->client->owner_id && !MemoryIsZeroStruct(&target->handle)) {
        foreign_count += 1;
      }
    }
    if (foreign_count != 0) {
      out_run_ctrls->run_entities = push_array_no_zero(arena, DMN_Handle, foreign_count);
      out_run_ctrls->run_entity_count = foreign_count;
      for EachNode(target, RADVS_DemonTarget, dmn->targets) {
        if (target->owner_id != request->client->owner_id && !MemoryIsZeroStruct(&target->handle)) {
          out_run_ctrls->run_entities[out_run_ctrls->run_entity_count - foreign_count] = target->handle;
          foreign_count -= 1;
        }
      }
    }
  }
  for EachIndex(trap_index, request->trap_count) {
    dmn_trap_chunk_list_push(arena, &out_run_ctrls->traps, 64, &request->traps[trap_index]);
  }
  return RADVS_Result_Ok;
}

internal void
radvs_demon_deliver_event(RADVS_Demon *dmn, RADVS_DemonTarget *target, const DMN_Event *event)
{
  (void)dmn;
  if (event->kind == DMN_EventKind_ExitProcess && target != 0) {
    radvs_demon_target_release(dmn, target);
  }
}

internal void radvs_demon_flush_pending_children(RADVS_Demon *dmn, DMN_Handle parent_process, U64 owner_id);

internal void
radvs_demon_flush_pending_route(RADVS_Demon *dmn, RADVS_DemonPendingRoute *route, U64 owner_id)
{
  Assert(route->has_create_process);
  radvs_demon_pending_route_remove(dmn, route);

  RADVS_DemonTarget *target = radvs_demon_target_from_handle(dmn, route->process);
  if (target == 0) {
    target = radvs_demon_target_alloc(dmn, owner_id, route->system_process_id);
    target->handle = route->process;
  }
  for EachNode(event, RADVS_DemonPendingEvent, route->first) {
    radvs_demon_deliver_event(dmn, target, &event->v);
  }
  radvs_demon_flush_pending_children(dmn, route->process, owner_id);
}

internal void
radvs_demon_flush_pending_children(RADVS_Demon *dmn, DMN_Handle parent_process, U64 owner_id)
{
  for (;;) {
    RADVS_DemonPendingRoute *route = 0;
    for EachNode(it, RADVS_DemonPendingRoute, dmn->pending_routes) {
      if (it->has_create_process && dmn_handle_match(it->parent_process, parent_process)) {
        route = it;
        break;
      }
    }
    if (route == 0) {
      break;
    }
    radvs_demon_flush_pending_route(dmn, route, owner_id);
  }
}

internal void
radvs_demon_publish_events(RADVS_Demon *dmn, RADVS_DemonClient *active_client, DMN_EventList events, B32 suppress_internal_halt)
{
  if (dmn->raw_event_router != 0) {
    dmn->raw_event_router(dmn->raw_event_router_user_data, active_client, events, suppress_internal_halt);
  }
  for EachNode(node, DMN_EventNode, events.first) {
    DMN_Event *event = &node->v;
    if (suppress_internal_halt && event->kind == DMN_EventKind_Halt) {
      continue;
    }

    if (event->kind == DMN_EventKind_CreateProcess) {
      RADVS_DemonTarget *target = radvs_demon_target_from_process_pid(dmn, event->system_process_id);
      if (target == 0 && !MemoryIsZeroStruct(&event->parent_process)) {
        RADVS_DemonTarget *parent = radvs_demon_target_from_handle(dmn, event->parent_process);
        if (parent != 0) {
          target = radvs_demon_target_alloc(dmn, parent->owner_id, event->system_process_id);
        }
      }
      if (target == 0) {
        radvs_demon_pending_route_record(dmn, event);
        continue;
      }

      target->handle = event->process;
      RADVS_DemonPendingRoute *route = radvs_demon_pending_route_from_process(dmn, event->process);
      if (route != 0) {
        radvs_demon_pending_route_record(dmn, event);
        radvs_demon_flush_pending_route(dmn, route, target->owner_id);
      } else {
        radvs_demon_deliver_event(dmn, target, event);
        radvs_demon_flush_pending_children(dmn, event->process, target->owner_id);
      }
      continue;
    }

    RADVS_DemonTarget *target = !MemoryIsZeroStruct(&event->process) ? radvs_demon_target_from_handle(dmn, event->process) : 0;
    if (target != 0) {
      radvs_demon_deliver_event(dmn, target, event);
    } else if (!MemoryIsZeroStruct(&event->process)) {
      radvs_demon_pending_route_record(dmn, event);
    } else {
      radvs_demon_deliver_event(dmn, 0, event);
    }
  }
}

internal void
radvs_demon_detach_client_targets(RADVS_Demon *dmn, DMN_CtrlCtx *ctrl_ctx, RADVS_DemonClient *client)
{
  for (RADVS_DemonTarget *target = dmn->targets; target != 0;) {
    RADVS_DemonTarget *next = target->next;
    if (target->owner_id == client->owner_id) {
      if (!MemoryIsZeroStruct(&target->handle)) {
        dmn_ctrl_detach(ctrl_ctx, target->handle);
        dmn->detach_pending = 1;
      }
      radvs_demon_target_release(dmn, target);
    }
    target = next;
  }
}

internal void
radvs_demon_unregister_client(RADVS_Demon *dmn, RADVS_DemonClient *client)
{
  MutexScope (dmn->requests.mutex) {
    RADVS_DemonClient **next = &dmn->clients;
    for (; *next != 0; next = &(*next)->next) {
      if (*next == client) {
        *next = client->next;
        client->next = 0;
        client->released = 1;
        break;
      }
    }
  }
}

internal DMN_EventList
radvs_demon_run_until_event(RADVS_Demon *dmn, DMN_CtrlCtx *ctrl_ctx, RADVS_DemonClient *client, DMN_RunCtrls *run_ctrls, B32 *out_internal_halt)
{
  DMN_EventList events = {0};
  MutexScope (dmn->requests.mutex) {
    dmn->run_active = 1;
    dmn->active_client = client;
    dmn->active_control_epoch = ++dmn->next_control_epoch;
    if (client != 0 && client->state != RADVS_DemonClientState_Terminating && client->state != RADVS_DemonClientState_Closed) {
      client->state = RADVS_DemonClientState_Running;
    }
  }
  do {
    arena_clear(dmn->event_arena);
    events = dmn_ctrl_run(dmn->event_arena, ctrl_ctx, run_ctrls);
    // dmn_halt first creates an internal halter thread. Its creation is
    // intentionally not public, so continue until its exit produces the
    // public Halt event (or another externally visible event arrives).
  } while (events.count == 0);
  B32 internal_halt = 0;
  MutexScope (dmn->requests.mutex) {
    dmn->run_active = 0;
    for EachNode(node, DMN_EventNode, events.first) {
      if (node->v.kind == DMN_EventKind_Halt) {
        internal_halt = dmn->interrupt_requested && !dmn->interrupt_visible;
        break;
      }
    }
    if (internal_halt && client != 0 && client->run_intent && !client->released && !client->release_requested &&
        client->state != RADVS_DemonClientState_Terminating) {
      dmn->resume_client = client;
      dmn->resume_traps = dmn->active_traps;
      dmn->resume_trap_count = dmn->active_trap_count;
    } else if (client != 0 && client->state == RADVS_DemonClientState_Running) {
      client->state = RADVS_DemonClientState_Stopped;
    }
    dmn->active_client = 0;
    dmn->active_control_epoch = 0;
    dmn->interrupt_requested = 0;
    dmn->interrupt_visible = 0;
  }
  if (out_internal_halt != 0) {
    *out_internal_halt = internal_halt;
  }
  return events;
}

internal void
radvs_demon_worker(void *user_data)
{
  RADVS_Demon *dmn = user_data;
  
  // initialize demon thread context
  dmn_init();
  DMN_CtrlCtx *ctrl_ctx = dmn_ctrl_begin();
  ins_atomic_u32_eval_assign(&radvs_demon_state, RADVS_DemonState_Ready);

  for (;;) {
    RADVS_DemonRequest *request = 0;
    RADVS_DemonClient *resume_client = 0;
    MutexScope (dmn->requests.mutex) {
      for (; dmn->requests.first == 0 && resume_client == 0;) {
        if (dmn->resume_client != 0) {
          RADVS_DemonClient *candidate = dmn->resume_client;
          dmn->resume_client = 0;
          if (candidate->run_intent && !candidate->released && !candidate->release_requested &&
              candidate->state != RADVS_DemonClientState_Terminating) {
            resume_client = candidate;
            break;
          }
        }
        cond_var_wait(dmn->requests.available_cv, dmn->requests.mutex, max_U64);
      }
      if (dmn->requests.first != 0) {
        request = radvs_demon_request_pop_next_locked(dmn);
      }
    }

    if (resume_client != 0) {
      Temp scratch = scratch_begin(0, 0);
      RADVS_DemonRequest resume_request = {
        .client = resume_client,
        .command = RADVS_DemonCommand_Run,
        .traps = dmn->resume_traps,
        .trap_count = dmn->resume_trap_count,
      };
      DMN_RunCtrls run_ctrls = {0};
      if (radvs_demon_run_ctrls_from_request(scratch.arena, dmn, &resume_request, &run_ctrls) == RADVS_Result_Ok) {
        dmn->active_traps = resume_request.traps;
        dmn->active_trap_count = resume_request.trap_count;
        B32 internal_halt = 0;
        DMN_EventList events = radvs_demon_run_until_event(dmn, ctrl_ctx, resume_client, &run_ctrls, &internal_halt);
        radvs_demon_publish_events(dmn, resume_client, events, internal_halt);
      }
      scratch_end(scratch);
      continue;
    }

    switch (request->command) {
    case RADVS_DemonCommand_Launch: {
      request->pid = dmn_ctrl_launch(ctrl_ctx, &request->launch);

      // good launch? -> alloc demon client
      if (request->pid) {
        radvs_demon_target_alloc(dmn, request->client->owner_id, request->pid);
        if (dmn->launch_router != 0) {
          dmn->launch_router(dmn->launch_router_user_data, request->client, request->pid);
        }
      }

      // complete the engine request
      radvs_request_queue_complete(&dmn->requests, &request->base, request->pid != 0 ? RADVS_Result_Ok : RADVS_Result_InvalidArgument);
    } break;

    case RADVS_DemonCommand_Run:
    case RADVS_DemonCommand_Step: {
      Temp scratch = scratch_begin(0, 0);

      DMN_RunCtrls run_ctrls = {0};
      RADVS_Result result    = radvs_demon_run_ctrls_from_request(scratch.arena, dmn, request, &run_ctrls);
      if (result == RADVS_Result_Ok) {
        dmn->active_traps = request->traps;
        dmn->active_trap_count = request->trap_count;
        MutexScope (dmn->requests.mutex) {
          request->client->run_intent = 1;
          if (request->client->state != RADVS_DemonClientState_Terminating) {
            request->client->state = RADVS_DemonClientState_Runnable;
          }
        }

        // acknowledge before the blocking control call so each engine worker
        // remains interactive while this global worker waits for an event
        radvs_request_queue_complete(&dmn->requests, &request->base, RADVS_Result_Ok);

        B32 internal_halt = 0;
        DMN_EventList events = radvs_demon_run_until_event(dmn, ctrl_ctx, request->client, &run_ctrls, &internal_halt);
        radvs_demon_publish_events(dmn, request->client, events, internal_halt);
        radvs_request_queue_recycle(&dmn->requests, &request->base);
      } else {
        MutexScope (dmn->requests.mutex) {
          request->client->run_intent = 0;
        }
        radvs_request_queue_complete(&dmn->requests, &request->base, result);
      }

      scratch_end(scratch);
    } break;

    case RADVS_DemonCommand_Break: {
      if (!request->break_already_signaled) {
        dmn_halt(0, 0);
        radvs_request_queue_complete(&dmn->requests, &request->base, RADVS_Result_Ok);
        DMN_RunCtrls run_ctrls = { .run_entities_are_processes = 1 };
        B32 internal_halt = 0;
        DMN_EventList events = radvs_demon_run_until_event(dmn, ctrl_ctx, request->client, &run_ctrls, &internal_halt);
        radvs_demon_publish_events(dmn, request->client, events, internal_halt);
      } else {
        radvs_request_queue_complete(&dmn->requests, &request->base, RADVS_Result_Ok);
      }
    } break;

    case RADVS_DemonCommand_UpdateTraps: {
      if (dmn->resume_client == request->client) {
        dmn->resume_traps = request->traps;
        dmn->resume_trap_count = request->trap_count;
      }
      radvs_request_queue_complete(&dmn->requests, &request->base, RADVS_Result_Ok);
    } break;

    case RADVS_DemonCommand_Terminate: {
      Temp scratch = scratch_begin(0, 0);
      DMN_HandleArray targets = {0};
      RADVS_Result result = RADVS_Result_Ok;
      MutexScope (dmn->requests.mutex) {
        request->client->run_intent = 0;
        request->client->state = RADVS_DemonClientState_Terminating;
        if (dmn->resume_client == request->client) {
          dmn->resume_client = 0;
        }
      }
      if (request->process_handle_count != 0) {
        for EachIndex(index, request->process_handle_count) {
          if (!radvs_demon_target_is_owned_by(dmn, request->client, request->process_handles[index])) {
            result = RADVS_Result_InvalidArgument;
            break;
          }
        }
        targets.handles = request->process_handles;
        targets.count = request->process_handle_count;
      } else {
        targets = radvs_demon_handles_for_client(scratch.arena, dmn, request->client);
        if (targets.count == 0) {
          result = RADVS_Result_Busy;
        }
      }

      for EachIndex(index, targets.count) {
        if (result != RADVS_Result_Ok) {
          break;
        }
        if (!dmn_ctrl_kill(ctrl_ctx, targets.handles[index], (U32)request->exit_code)) {
          result = RADVS_Result_InvalidArgument;
        }
      }

      if (result == RADVS_Result_Ok) {
        B32 *exited = push_array(scratch.arena, B32, targets.count);
        U64 exit_count = 0;
        for (; exit_count < targets.count;) {
          DMN_RunCtrls run_ctrls = {
            .run_entities = targets.handles,
            .run_entity_count = targets.count,
            .run_entities_are_unfrozen = 1,
            .run_entities_are_processes = 1,
          };
          B32 internal_halt = 0;
          DMN_EventList events = radvs_demon_run_until_event(dmn, ctrl_ctx, request->client, &run_ctrls, &internal_halt);
          for EachNode(node, DMN_EventNode, events.first) {
            if (node->v.kind == DMN_EventKind_ExitProcess) {
              for EachIndex(index, targets.count) {
                if (!exited[index] && dmn_handle_match(node->v.process, targets.handles[index])) {
                  exited[index] = 1;
                  exit_count += 1;
                }
              }
            }
          }
          radvs_demon_publish_events(dmn, request->client, events, internal_halt);
        }
      }
      radvs_request_queue_complete(&dmn->requests, &request->base, result);
      scratch_end(scratch);
    } break;

    case RADVS_DemonCommand_ReleaseClient: {
      MutexScope (dmn->requests.mutex) {
        request->client->run_intent = 0;
        request->client->state = RADVS_DemonClientState_Closed;
        if (dmn->resume_client == request->client) {
          dmn->resume_client = 0;
        }
      }
      radvs_demon_detach_client_targets(dmn, ctrl_ctx, request->client);
      radvs_demon_unregister_client(dmn, request->client);
      radvs_request_queue_complete(&dmn->requests, &request->base, RADVS_Result_Ok);
    } break;

    case RADVS_DemonCommand_Shutdown: {
      if (dmn->detach_pending) {
        DMN_RunCtrls run_ctrls = { .run_entities_are_processes = 1 };
        B32 internal_halt = 0;
        DMN_EventList events = radvs_demon_run_until_event(dmn, ctrl_ctx, 0, &run_ctrls, &internal_halt);
        radvs_demon_publish_events(dmn, 0, events, internal_halt);
      }
      radvs_request_queue_complete(&dmn->requests, &request->base, RADVS_Result_Ok);
      return;
    } break;

    default: {
      radvs_request_queue_complete(&dmn->requests, &request->base, RADVS_Result_InvalidArgument);
    } break;
    }
  }
}

RADVS_Result
radvs_demon_init(void)
{
  radvs_demon_lifecycle_gate_take();
  U32 state = ins_atomic_u32_eval_cond_assign(&radvs_demon_state,
                                               RADVS_DemonState_Initializing,
                                               RADVS_DemonState_Uninitialized);
  if (state == RADVS_DemonState_Uninitialized) {
    Arena *arena = arena_alloc(.reserve_size = MB(1), .commit_size = KB(64));
    RADVS_Demon *dmn = push_array(arena, RADVS_Demon, 1);
    dmn->arena       = arena;
    dmn->event_arena = arena_alloc(.reserve_size = MB(1), .commit_size = KB(64));
    dmn->pending_event_arena = arena_alloc(.reserve_size = MB(1), .commit_size = KB(64));
    radvs_request_queue_init(&dmn->requests, arena);
#if RADVS_REQUEST_DIAGNOSTICS
    radvs_request_queue_set_diagnostic_name(&dmn->requests, str8_lit("demon"));
#endif
    radvs_demon = dmn;

    dmn->worker = thread_launch(radvs_demon_worker, dmn);
    if (dmn->worker.u64[0] == 0) {
      radvs_demon = 0;
      radvs_request_queue_release(&dmn->requests);
      arena_release(dmn->pending_event_arena);
      arena_release(dmn->event_arena);
      arena_release(arena);
      ins_atomic_u32_eval_assign(&radvs_demon_state, RADVS_DemonState_Failed);
    }
  }

  radvs_demon_lifecycle_gate_drop();

  for (; ins_atomic_u32_eval(&radvs_demon_state) == RADVS_DemonState_Initializing;) {
    Sleep(0);
  }

  return radvs_demon != 0 ? RADVS_Result_Ok : RADVS_Result_OutOfMemory;
}

RADVS_Result
radvs_demon_shutdown(void)
{
  radvs_demon_lifecycle_gate_take();
  if (ins_atomic_u32_eval(&radvs_demon_state) == RADVS_DemonState_Uninitialized) {
    radvs_demon_lifecycle_gate_drop();
    return RADVS_Result_Ok;
  }
  if (ins_atomic_u32_eval(&radvs_demon_state) != RADVS_DemonState_Ready || radvs_demon == 0) {
    radvs_demon_lifecycle_gate_drop();
    return RADVS_Result_Busy;
  }

  RADVS_Demon *dmn = radvs_demon;
  RADVS_Result result = RADVS_Result_Busy;
  MutexScope (dmn->requests.mutex) {
    if (dmn->clients == 0) {
      ins_atomic_u32_eval_assign(&radvs_demon_state, RADVS_DemonState_Releasing);
      RADVS_DemonRequest *request = radvs_demon_request_alloc_locked(dmn, 0, RADVS_DemonCommand_Shutdown);
      if (dmn->run_active) {
        dmn_halt(0, 0);
      }
      result = radvs_request_queue_submit_locked(&dmn->requests, &request->base);
    }
  }
  if (result == RADVS_Result_Ok) {
    thread_join(dmn->worker, max_U64);
    dmn_release();
    radvs_request_queue_release(&dmn->requests);
    arena_release(dmn->pending_event_arena);
    arena_release(dmn->event_arena);
    arena_release(dmn->arena);
    radvs_demon = 0;
    ins_atomic_u32_eval_assign(&radvs_demon_state, RADVS_DemonState_Uninitialized);
  } else {
    ins_atomic_u32_eval_assign(&radvs_demon_state, RADVS_DemonState_Ready);
  }
  radvs_demon_lifecycle_gate_drop();
  return result;
}

RADVS_Result
radvs_demon_alloc(RADVS_DemonClient **out_client)
{
  if (out_client == 0) {
    return RADVS_Result_InvalidArgument;
  }
  *out_client = 0;
  if (ins_atomic_u32_eval(&radvs_demon_state) != RADVS_DemonState_Ready || radvs_demon == 0) {
    return RADVS_Result_InvalidArgument;
  }

  Arena *arena = arena_alloc(.reserve_size = KB(64), .commit_size = KB(16));
  RADVS_DemonClient *client = push_array(arena, RADVS_DemonClient, 1);
  client->arena = arena;
  MutexScope (radvs_demon->requests.mutex) {
    SLLStackPush(radvs_demon->clients, client);
  }
  *out_client = client;
  return RADVS_Result_Ok;
}

RADVS_Result
radvs_demon_client_set_owner_id(RADVS_DemonClient *client, U64 owner_id)
{
  if (client == 0 || owner_id == 0 || radvs_demon == 0) {
    return RADVS_Result_InvalidArgument;
  }
  RADVS_Result result = RADVS_Result_InvalidArgument;
  MutexScope (radvs_demon->requests.mutex) {
    if (!client->released && !client->release_requested && client->owner_id == 0) {
      client->owner_id = owner_id;
      result = RADVS_Result_Ok;
    }
  }
  return result;
}

void
radvs_demon_client_release(RADVS_DemonClient *client)
{
  if (client == 0) {
    return;
  }

  RADVS_Demon *dmn = radvs_demon;
  if (dmn == 0) {
    return;
  }
  RADVS_DemonRequest *request = 0;
  MutexScope (dmn->requests.mutex) {
    if (!client->released && !client->release_requested) {
      client->release_requested = 1;
      request = radvs_demon_request_alloc_locked(dmn, client, RADVS_DemonCommand_ReleaseClient);
    }
  }
  if (request == 0) {
    return;
  }
  RADVS_Result result = radvs_demon_request_submit(dmn, request, 0);
  radvs_request_queue_recycle(&dmn->requests, &request->base);
  if (result == RADVS_Result_Ok) {
    arena_release(client->arena);
  } else {
    MutexScope (dmn->requests.mutex) {
      client->release_requested = 0;
    }
  }
}

RADVS_Result
radvs_demon_launch(RADVS_DemonClient *client, const ProcessLaunchParams *params, U32 *out_pid)
{
  if (client == 0 || params == 0 || params->cmd_line.first == 0 || out_pid == 0) {
    return RADVS_Result_InvalidArgument;
  }

  RADVS_Demon        *dmn     = radvs_demon;
  RADVS_DemonRequest *request = 0;
  MutexScope (dmn->requests.mutex) {
    request = radvs_demon_request_alloc_locked(dmn, client, RADVS_DemonCommand_Launch);
    request->launch.path               = push_str8_copy(dmn->arena, params->path);
    request->launch.inherit_env        = params->inherit_env;
    request->launch.debug_subprocesses = params->debug_subprocesses;
    request->launch.consoleless        = params->consoleless;
    request->launch.stdout_file        = params->stdout_file;
    request->launch.stderr_file        = params->stderr_file;
    request->launch.stdin_file         = params->stdin_file;
    for EachNode(arg, String8Node, params->cmd_line.first) {
      str8_list_push(dmn->arena, &request->launch.cmd_line, push_str8_copy(dmn->arena, arg->string));
    }
    for EachNode(env, String8Node, params->env.first) {
      str8_list_push(dmn->arena, &request->launch.env, push_str8_copy(dmn->arena, env->string));
    }
  }
  RADVS_Result result = radvs_demon_request_submit(dmn, request, out_pid);
  radvs_request_queue_recycle(&dmn->requests, &request->base);
  return result;
}

RADVS_Result
radvs_demon_run(RADVS_DemonClient *client, const DMN_Handle *process_handles, U64 process_handle_count)
{
  if (client == 0 || (process_handle_count != 0 && process_handles == 0)) {
    return RADVS_Result_InvalidArgument;
  }
  RADVS_Demon *dmn = radvs_demon;
  RADVS_DemonRequest *request = 0;
  MutexScope (dmn->requests.mutex) {
    request = radvs_demon_request_alloc_locked(dmn, client, RADVS_DemonCommand_Run);
    request->process_handle_count = process_handle_count;
    if (process_handle_count != 0) {
      request->process_handles = push_array_no_zero(dmn->arena, DMN_Handle, process_handle_count);
      MemoryCopy(request->process_handles, process_handles, process_handle_count * sizeof(*process_handles));
    }
  }
  RADVS_Result result = radvs_demon_request_submit(dmn, request, 0);
  if (result != RADVS_Result_Ok) {
    radvs_request_queue_recycle(&dmn->requests, &request->base);
  }
  return result;
}

RADVS_Result
radvs_demon_set_raw_event_router(RADVS_DemonRawEventRouter *router, void *user_data)
{
  if (radvs_demon == 0) {
    return RADVS_Result_Busy;
  }
  MutexScope (radvs_demon->requests.mutex) {
    radvs_demon->raw_event_router = router;
    radvs_demon->raw_event_router_user_data = user_data;
  }
  return RADVS_Result_Ok;
}

RADVS_Result
radvs_demon_set_launch_router(RADVS_DemonLaunchRouter *router, void *user_data)
{
  if (radvs_demon == 0) {
    return RADVS_Result_Busy;
  }
  MutexScope (radvs_demon->requests.mutex) {
    radvs_demon->launch_router = router;
    radvs_demon->launch_router_user_data = user_data;
  }
  return RADVS_Result_Ok;
}

RADVS_Result
radvs_demon_update_traps(RADVS_DemonClient *client, const DMN_Trap *traps, U64 trap_count)
{
  if (client == 0 || (trap_count != 0 && traps == 0)) {
    return RADVS_Result_InvalidArgument;
  }
  RADVS_Demon *dmn = radvs_demon;
  if (dmn == 0) {
    return RADVS_Result_Busy;
  }
  RADVS_DemonRequest *request = 0;
  RADVS_Result result = RADVS_Result_Busy;
  MutexScope (dmn->requests.mutex) {
    if (ins_atomic_u32_eval(&radvs_demon_state) == RADVS_DemonState_Ready && !client->released && !client->release_requested) {
      request = radvs_demon_request_alloc_locked(dmn, client, RADVS_DemonCommand_UpdateTraps);
      request->trap_count = trap_count;
      if (trap_count != 0) {
        request->traps = push_array_no_zero(dmn->arena, DMN_Trap, trap_count);
        MemoryCopy(request->traps, traps, trap_count * sizeof(*traps));
      }
      result = RADVS_Result_Ok;
    }
  }
  if (result == RADVS_Result_Ok) {
    result = radvs_demon_request_submit(dmn, request, 0);
    radvs_request_queue_recycle(&dmn->requests, &request->base);
  }
  return result;
}

RADVS_Result
radvs_demon_run_with_traps(RADVS_DemonClient *client, const DMN_Handle *process_handles, U64 process_handle_count, const DMN_Trap *traps, U64 trap_count)
{
  if (client == 0 || (process_handle_count != 0 && process_handles == 0) || (trap_count != 0 && traps == 0)) {
    return RADVS_Result_InvalidArgument;
  }

  RADVS_Demon *dmn = radvs_demon;
  RADVS_DemonRequest *request = 0;
  MutexScope (dmn->requests.mutex) {
    request = radvs_demon_request_alloc_locked(dmn, client, RADVS_DemonCommand_Run);
    request->process_handle_count = process_handle_count;
    if (process_handle_count != 0) {
      request->process_handles = push_array_no_zero(dmn->arena, DMN_Handle, process_handle_count);
      MemoryCopy(request->process_handles, process_handles, process_handle_count * sizeof(*process_handles));
    }
    request->trap_count = trap_count;
    if (trap_count != 0) {
      request->traps = push_array_no_zero(dmn->arena, DMN_Trap, trap_count);
      MemoryCopy(request->traps, traps, trap_count * sizeof(*traps));
    }
  }
  RADVS_Result result = radvs_demon_request_submit(dmn, request, 0);
  if (result != RADVS_Result_Ok) {
    radvs_request_queue_recycle(&dmn->requests, &request->base);
  }
  return result;
}

RADVS_Result
radvs_demon_step_with_traps(RADVS_DemonClient *client, DMN_Handle process, DMN_Handle thread, const DMN_Trap *traps, U64 trap_count)
{
  if (client == 0 || MemoryIsZeroStruct(&process) || MemoryIsZeroStruct(&thread) || (trap_count != 0 && traps == 0)) {
    return RADVS_Result_InvalidArgument;
  }

  RADVS_Demon *dmn = radvs_demon;
  RADVS_DemonRequest *request = 0;
  MutexScope (dmn->requests.mutex) {
    request = radvs_demon_request_alloc_locked(dmn, client, RADVS_DemonCommand_Step);
    request->process_handles = push_array_no_zero(dmn->arena, DMN_Handle, 1);
    request->process_handles[0] = process;
    request->process_handle_count = 1;
    request->step_thread = thread;
    request->trap_count = trap_count;
    if (trap_count != 0) {
      request->traps = push_array_no_zero(dmn->arena, DMN_Trap, trap_count);
      MemoryCopy(request->traps, traps, trap_count * sizeof(*traps));
    }
  }
  RADVS_Result result = radvs_demon_request_submit(dmn, request, 0);
  if (result != RADVS_Result_Ok) {
    radvs_request_queue_recycle(&dmn->requests, &request->base);
  }
  return result;
}

RADVS_Result
radvs_demon_break(RADVS_DemonClient *client)
{
  if (client == 0 || client->released) {
    return RADVS_Result_InvalidArgument;
  }

  RADVS_Demon *dmn = radvs_demon;
  RADVS_DemonRequest *request = 0;
  MutexScope (dmn->requests.mutex) {
    request = radvs_demon_request_alloc_locked(dmn, client, RADVS_DemonCommand_Break);
  }
  RADVS_Result result = radvs_demon_request_submit(dmn, request, 0);
  radvs_request_queue_recycle(&dmn->requests, &request->base);
  return result;
}

RADVS_Result
radvs_demon_terminate(RADVS_DemonClient *client, const DMN_Handle *process_handles, U64 process_handle_count)
{
  if (client == 0 || (process_handle_count != 0 && process_handles == 0)) {
    return RADVS_Result_InvalidArgument;
  }

  RADVS_Demon *dmn = radvs_demon;
  RADVS_DemonRequest *request = 0;
  MutexScope (dmn->requests.mutex) {
    request = radvs_demon_request_alloc_locked(dmn, client, RADVS_DemonCommand_Terminate);
    request->process_handle_count = process_handle_count;
    if (process_handle_count != 0) {
      request->process_handles = push_array_no_zero(dmn->arena, DMN_Handle, process_handle_count);
      MemoryCopy(request->process_handles, process_handles, process_handle_count * sizeof(*process_handles));
    }
  }
  RADVS_Result result = radvs_demon_request_submit(dmn, request, 0);
  radvs_request_queue_recycle(&dmn->requests, &request->base);
  return result;
}
