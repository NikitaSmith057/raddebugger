
#include "radvs/rvs_queue.h"
#include "radvs/rvs_demon.h"

typedef struct RVS_DemonInterrupt
{
  U64           command_id;
  RVS_MessageID request_id;
  RVS_MessageID execution_request_id;
} RVS_DemonInterrupt;

typedef struct RVS_Demon
{
  Arena                  *arena;
  Mutex                   mutex;
  RVS_Queue              *queue;
  RVS_WorkerState         state;
  Thread                  worker;
  RVS_DemonReplyCallback *reply_callback;
  void                   *reply_ud;
  RVS_DemonMessage       *active_message;
  B32                     is_run_in_flight;
  RVS_DemonInterrupt      pending_interrupt;
} RVS_Demon;

global RVS_Demon g_rvs_demon;

typedef struct
{
  RVS_Demon  *demon;
  RVS_Result  run_result;
} RVS_DemonRunStarted;

////////////////////////////////

internal void rvs_demon_worker(void *user_data);
internal RVS_Result rvs_demon_push_message(RVS_Demon *dmn, RVS_DemonMessage *spec);

////////////////////////////////

internal RVS_BackendInterruptCapability
rvs_backend_interrupt_capability(void *ud)
{
  RVS_Demon *dmn = ud;
  return dmn ? RVS_BackendInterruptCapability_GlobalWithResume : RVS_BackendInterruptCapability_Null;
}

////////////////////////////////

RVS_Result
rvs_demon_init(void *reply_ud, RVS_DemonReplyCallback *reply_callback, RVS_Demon **dmn_out)
{
  ProfBeginFunction();
  RVS_Demon *dmn = &g_rvs_demon;
  
  RVS_Result result = RVS_Result_Error;
  RVS_WorkerState state = ins_atomic_u32_eval_cond_assign(&dmn->state, RVS_WorkerState_Initing, RVS_WorkerState_Null);
  if (state == RVS_WorkerState_Null) {
    // alloc resources for the DEMON thread
    dmn->arena          = arena_alloc();
    dmn->mutex          = mutex_alloc();
    dmn->reply_callback = reply_callback;
    dmn->reply_ud       = reply_ud;
    dmn->queue          = rvs_queue_alloc(sizeof(RVS_DemonMessage), AlignOf(RVS_DemonMessage));
    dmn->worker         = thread_launch(rvs_demon_worker, dmn);
    if ( ! MemoryIsZeroStruct(&dmn->worker)) {
      result = RVS_Result_Ok;
    } else {
      ins_atomic_u32_eval_assign(&dmn->state, RVS_WorkerState_Exited);
    }

    // wait for the DEMON thread to initialize
    while (ins_atomic_u32_eval(&dmn->state) == RVS_WorkerState_Initing) { sleep_ms(1); }
    if (ins_atomic_u32_eval(&dmn->state) != RVS_WorkerState_Live) {
      // TODO: handle the error
      NotImplemented;
    }
  }

  if (dmn_out) {
    *dmn_out = dmn;
  }

  ProfEnd();
  return result;
}

RVS_Result
rvs_demon_shutdown(RVS_Demon *dmn)
{
  ProfBeginFunction();
  RVS_Result result = RVS_Result_Error;
  RVS_Result return_result = RVS_Result_Ok;

  mutex_take(dmn->mutex);
  if (ins_atomic_u32_eval(&dmn->state) == RVS_WorkerState_Live) {
    // Close command admission while keeping the backend alive for the wakeup.
    ins_atomic_u32_eval_assign(&dmn->state, RVS_WorkerState_Exiting);
  }
  mutex_drop(dmn->mutex);

  if (ins_atomic_u32_eval(&dmn->state) == RVS_WorkerState_Exiting) {
    // The worker may have published Run acceptance just before entering the native run loop.
    B32 backend_wakeup_ready = 0;
    for (U32 wake_attempt = 0; wake_attempt < 30000; wake_attempt += 1) {
      mutex_take(dmn->mutex);
      B32 is_run_in_flight = dmn->is_run_in_flight;
      mutex_drop(dmn->mutex);

      if (!is_run_in_flight || dmn_halt(0, 0)) {
        backend_wakeup_ready = 1;
        break;
      }

      sleep_ms(1);
    }

    if (backend_wakeup_ready) {
      RVS_DemonMessage exit_spec = { .base.kind = RVS_CommandKind_Exit };
      result = rvs_demon_push_message(dmn, &exit_spec); // message backend to start the shutdown sequence
    }
  }

  if (result == RVS_Result_Ok) {
    thread_join(dmn->worker, max_U64);
  } else if (ins_atomic_u32_eval(&dmn->state) != RVS_WorkerState_Exited) {
    return_result = result;
  }

  ProfEnd();
  return return_result;
}

internal void
rvs_demon_release_resources(RVS_Demon *dmn)
{
  if (dmn && dmn->arena) {
    AssertAlways(ins_atomic_u32_eval(&dmn->state) == RVS_WorkerState_Exited);
    rvs_queue_release(dmn->queue);
    mutex_release(dmn->mutex);
    arena_release(dmn->arena);
    MemoryZeroStruct(dmn);
  }
}

internal void
rvs_demon_traps_copy(Arena *arena, DMN_TrapChunkList *dst, DMN_TrapChunkList *src)
{
  MemoryZeroStruct(dst);
  for EachNode(node, DMN_TrapChunkNode, src->first) {
    for EachIndex(trap_idx, node->count) {
      dmn_trap_chunk_list_push(arena, dst, 64, &node->v[trap_idx]);
    }
  }
}

internal B32
rvs_demon_traps_validate(DMN_TrapChunkList *traps)
{
  for EachNode(node, DMN_TrapChunkNode, traps->first) {
    for EachIndex(trap_idx, node->count) {
      DMN_Trap *trap = &node->v[trap_idx];
      if (trap->flags == 0) {
        U8 byte = 0;
        if (dmn_handle_match(trap->process, dmn_handle_zero()) || trap->vaddr == max_U64 ||
            dmn_process_read(trap->process, r1u64(trap->vaddr, trap->vaddr + 1), &byte) != 1 ||
            !dmn_process_write(trap->process, r1u64(trap->vaddr, trap->vaddr + 1), &byte)) {
          return 0;
        }
      }
    }
  }
  return 1;
}

internal void
rvs_demon_message_copy(Arena *arena, RVS_DemonMessage *dst, RVS_DemonMessage *src)
{
  ProfBeginFunction();

  RVS_BackendMessage base = dst->base;

  *dst = *src;
  dst->base = base;

  switch (src->base.kind) {
  case RVS_CommandKind_Launch: {
    dst->launch.params = *process_launch_params_copy(arena, &src->launch.params);
  } break;

  case RVS_CommandKind_Run: {
    dst->run.processes = push_array(arena, DMN_Handle, src->run.processes_count);
    dst->run.processes_count = src->run.processes_count;
    MemoryCopyTyped(dst->run.processes, src->run.processes, src->run.processes_count);
    rvs_demon_traps_copy(arena, &dst->run.traps, &src->run.traps);
  } break;

  case RVS_CommandKind_Stop: {
    dst->stop.process_handles = push_array_no_zero(arena, DMN_Handle, src->stop.process_count);
    dst->stop.process_count   = src->stop.process_count;
    MemoryCopyTyped(dst->stop.process_handles, src->stop.process_handles, src->stop.process_count);
  } break;

  case RVS_CommandKind_Pause:
  case (RVS_CommandKind)RVS_DemonCommand_PumpEvent: {
    // no pointers to copy
  } break;

  default: { InvalidPath; } break;
  }
  ProfEnd();
}

internal void
rvs_demon_message_queue_copy(Arena *arena, void *dst, void *src)
{
  rvs_demon_message_copy(arena, dst, src);
}

internal void
rvs_demon_event_copy(Arena *arena, DMN_Event *dst, DMN_Event *src)
{
  ProfBeginFunction();
  *dst = *src;
  dst->string = str8_copy(arena, src->string);
  if (src->module_info) {
    dst->module_info = push_array(arena, DMN_ModuleInfo, 1);
    *dst->module_info = *src->module_info;
    dst->module_info->module_path     = str8_copy(arena, src->module_info->module_path);
    dst->module_info->debug_info_path = str8_copy(arena, src->module_info->debug_info_path);
  }
  ProfEnd();
}

internal void
rvs_demon_reply_copy(Arena *arena, RVS_DemonReply *dst, RVS_DemonReply *src)
{
  ProfBeginFunction();

  *dst = *src;

  switch (src->kind) {
  case RVS_DemonReplyKind_EventBatch: {
    dst->event_batch = (DMN_EventList){0};
    for EachNode(n, DMN_EventNode, src->event_batch.first) {
      DMN_Event *event = dmn_event_list_push(arena, &dst->event_batch);
      rvs_demon_event_copy(arena, event, &n->v);
    }
  } break;

  case RVS_DemonReplyKind_CommandResult:
  case RVS_DemonReplyKind_LaunchStarted:
  case RVS_DemonReplyKind_ExecutionStopped:
  case RVS_DemonReplyKind_ExecutionFinished: {
    // no pointers to copy
  } break;

  default: InvalidPath;
  }

  ProfEnd();
}

internal RVS_Result
rvs_demon_push_message(RVS_Demon *dmn, RVS_DemonMessage *spec)
{
  ProfBeginFunction();
  RVS_Result result = rvs_queue_push_copy(dmn->queue, spec, rvs_demon_message_queue_copy);
  ProfEnd();
  return result;
}

internal RVS_Result
rvs_demon_send_message(RVS_Demon *dmn, RVS_DemonMessage spec)
{
  ProfBeginFunction();
  mutex_take(dmn->mutex);
  RVS_Result result = rvs_demon_push_message(dmn, &spec);
  mutex_drop(dmn->mutex);
  ProfEnd();
  return result;
}

internal void
rvs_demon_worker(void *user_data)
{
  ProfBeginFunction();
  Temp scratch = scratch_begin(0, 0);
  RVS_Demon *dmn = user_data;

  // initialize DEMON platform state
  dmn_init();

  // grant current thread access to the DEMON API
  DMN_CtrlCtx *ctrl_ctx = dmn_ctrl_begin();
  ins_atomic_u32_eval_assign(&dmn->state, RVS_WorkerState_Live);

  for (B32 keep_running = 1; keep_running;) {
    Temp temp = temp_begin(scratch.arena);

    // wait for the requests from the engine thread
    RVS_DemonMessage *message = rvs_queue_pop_struct(dmn->queue, RVS_DemonMessage, max_U64);

    // process the message
    RVS_DemonReply reply = {0};
    if (ins_atomic_u32_eval(&dmn->state) == RVS_WorkerState_Exiting && message->base.kind != RVS_CommandKind_Exit) {
      rvs_queue_recycle(dmn->queue, &message->base.base);
      continue;
    }

    switch (message->base.kind) {
    case RVS_CommandKind_Launch: {
      U32 pid = dmn_ctrl_launch(ctrl_ctx, &message->launch.params);
      if (pid == 0) {
        reply = (RVS_DemonReply){
          .kind   = RVS_DemonReplyKind_CommandResult,
          .result = RVS_Result_Error,
          .id     = message->base.id,
        };
      } else {
        reply = (RVS_DemonReply){
          .kind       = RVS_DemonReplyKind_LaunchStarted,
          .id         = message->base.id,
          .launch_pid = pid,
        };
      }
    } break;

    case RVS_CommandKind_Run: {
      // validate run traps
      if ( ! rvs_demon_traps_validate(&message->run.traps)) {
        reply = (RVS_DemonReply){
          .kind   = RVS_DemonReplyKind_CommandResult,
          .id     = message->base.id,
          .result = RVS_Result_InvalidArgument,
        };
        break;
      }

      // enter run state
      mutex_take(dmn->mutex);
      dmn->active_message   = message;
      dmn->is_run_in_flight = 1;
      mutex_drop(dmn->mutex);

      // reply that backend running the command 
      reply = (RVS_DemonReply){
        .kind   = RVS_DemonReplyKind_CommandResult,
        .id     = message->base.id,
        .result = RVS_Result_Ok,
      };
      dmn->reply_callback(dmn, &reply, dmn->reply_ud);

      // run the backend
      RVS_DemonRunStarted started = { .demon = dmn };
      DMN_RunCtrls ctrls = {
        .run_entities               = message->run.processes,
        .run_entity_count           = message->run.processes_count,
        .run_entities_are_processes = 1,
        .run_entities_are_unfrozen  = 1,
        .traps                      = message->run.traps,
      };
      DMN_EventList event_batch = dmn_ctrl_run(scratch.arena, ctrl_ctx, &ctrls);

      // leave run state
      mutex_take(dmn->mutex);
      MemoryZeroStruct(&dmn->pending_interrupt);
      dmn->active_message   = 0;
      dmn->is_run_in_flight = 0;
      mutex_drop(dmn->mutex);

      // emit reply
      reply = (RVS_DemonReply){
        .kind        = RVS_DemonReplyKind_EventBatch,
        .result      = started.run_result,
        .id          = message->base.id,
        .event_batch = event_batch
      };
    } break;

    case RVS_CommandKind_Stop: {
      reply = (RVS_DemonReply){
        .kind   = RVS_DemonReplyKind_CommandResult,
        .id     = message->base.id,
        .result = RVS_Result_Ok
      };
      for EachIndex(process_idx, message->stop.process_count) {
        if ( ! dmn_ctrl_kill(ctrl_ctx, message->stop.process_handles[process_idx], 0)) {
          reply.result = RVS_Result_Error;
          break;
        }
      }
    } break;

    case RVS_CommandKind_Pause: {
      NotImplemented;
      AssertAlways(message->base.id != 0);
    } break;

    case RVS_CommandKind_Exit: {
      keep_running = 0;
    } break;

    default: { InvalidPath; } break;
    }

    // publish completed commands and event batches
    if (reply.kind != RVS_DemonReplyKind_Null) {
      dmn->reply_callback(dmn, &reply, dmn->reply_ud);
    }

    // recycle the message
    rvs_queue_recycle(dmn->queue, &message->base.base);
    
    temp_end(temp);
  }

  dmn_release();
  ins_atomic_u32_eval_assign(&dmn->state, RVS_WorkerState_Exited);
  scratch_end(scratch);
  ProfEnd();
}

