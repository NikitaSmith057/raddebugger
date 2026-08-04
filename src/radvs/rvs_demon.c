
#include "radvs/rvs_async.h"
#include "radvs/rvs_demon.h"

typedef struct RVS_Demon
{
  Arena                  *arena;
  Mutex                   mutex;
  RVS_Queue              *queue;
  RVS_ThreadState         state;
  Thread                  worker;
  RVS_DemonReplyCallback *reply_callback;
  void                   *reply_ud;
  RVS_MessageID           active_run_request_id;
  RVS_MessageID           pending_interrupt_request_id;
  RVS_MessageID           pending_interrupt_execution_request_id;
  U64                     pending_interrupt_command_id;
  B32                     is_run_in_flight;
} RVS_Demon;

global RVS_Demon g_rvs_demon;

typedef struct
{
  RVS_Demon       *demon;
  RVS_MessageID    request_id;
  RVS_DemonAction  action;
  U64              command_id;
  B32              called;
  B32              success;
} RVS_DemonRunStarted;

internal void rvs_demon_worker(void *user_data);
internal RVS_Result rvs_demon_push_message(RVS_Demon *dmn, RVS_DemonMessage *spec);

internal void
rvs_demon_run_started(B32 success, void *user_data)
{
  RVS_DemonRunStarted *started = user_data;
  AssertAlways(!started->called);
  started->called = 1;
  started->success = success;
  RVS_DemonReply reply = {
    .kind = RVS_DemonReplyKind_ActionResult,
    .request_id = started->request_id,
    .action_result = {
      .action = started->action,
      .result = success ? RVS_Result_Ok : RVS_Result_Error,
      .command_id = started->command_id,
    },
  };
  started->demon->reply_callback(started->demon, &reply, started->demon->reply_ud);
}

RVS_Result
rvs_demon_init(void *reply_ud, RVS_DemonReplyCallback *reply_callback, RVS_Demon **dmn_out)
{
  ProfBeginFunction();
  RVS_Demon *dmn = &g_rvs_demon;
  
  RVS_Result result = RVS_Result_Error;
  RVS_ThreadState state = ins_atomic_u32_eval_cond_assign(&dmn->state, RVS_ThreadState_Initing, RVS_ThreadState_Null);
  if (state == RVS_ThreadState_Null) {
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
      ins_atomic_u32_eval_assign(&dmn->state, RVS_ThreadState_Exited);
    }

    // wait for the DEMON thread to initialize
    while (ins_atomic_u32_eval(&dmn->state) == RVS_ThreadState_Initing) { sleep_ms(1); }
    if (ins_atomic_u32_eval(&dmn->state) != RVS_ThreadState_Live) {
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
  if (ins_atomic_u32_eval(&dmn->state) == RVS_ThreadState_Live) {
    // Close command admission while keeping the backend alive for the wakeup.
    ins_atomic_u32_eval_assign(&dmn->state, RVS_ThreadState_Terminating);
  }
  mutex_drop(dmn->mutex);

  if (ins_atomic_u32_eval(&dmn->state) == RVS_ThreadState_Terminating) {
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
      result = rvs_demon_push_message(dmn, &(RVS_DemonMessage){ .type = RVS_DemonMessage_Shutdown });
    }
  }
  if (result == RVS_Result_Ok) {
    thread_join(dmn->worker, max_U64);
  } else if (ins_atomic_u32_eval(&dmn->state) != RVS_ThreadState_Exited) {
    return_result = result;
  }
  ProfEnd();
  return return_result;
}

internal void
rvs_demon_release_resources(RVS_Demon *dmn)
{
  if (dmn && dmn->arena) {
    AssertAlways(ins_atomic_u32_eval(&dmn->state) == RVS_ThreadState_Exited);
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
  RVS_QueueNode base = dst->base;
  *dst = *src;
  dst->base = base;
  switch (src->type) {
  case RVS_DemonMessage_Launch: {
    dst->launch.params = *process_launch_params_copy(arena, &src->launch.params);
  } break;
  case RVS_DemonMessage_Pump: {
  } break;
  case RVS_DemonMessage_Run: {
    dst->run.processes = push_array(arena, DMN_Handle, src->run.processes_count);
    dst->run.processes_count = src->run.processes_count;
    MemoryCopyTyped(dst->run.processes, src->run.processes, src->run.processes_count);
    rvs_demon_traps_copy(arena, &dst->run.traps, &src->run.traps);
  } break;
  case RVS_DemonMessage_Resume: {
    dst->resume.processes = push_array(arena, DMN_Handle, src->resume.processes_count);
    dst->resume.processes_count = src->resume.processes_count;
    dst->resume.execution_request_id = src->resume.execution_request_id;
    MemoryCopyTyped(dst->resume.processes, src->resume.processes, src->resume.processes_count);
    rvs_demon_traps_copy(arena, &dst->resume.traps, &src->resume.traps);
  } break;
  case RVS_DemonMessage_InterruptExecution: {
  } break;
  case RVS_DemonMessage_Terminate: {
    dst->terminate.process_handles = push_array_no_zero(arena, DMN_Handle, src->terminate.process_count);
    dst->terminate.process_count   = src->terminate.process_count;
    MemoryCopyTyped(dst->terminate.process_handles, src->terminate.process_handles, src->terminate.process_count);
  } break;
  case RVS_DemonMessage_Shutdown: {
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
  case RVS_DemonReplyKind_LaunchStarted: {
  } break;
  case RVS_DemonReplyKind_ActionResult: {
  } break;
  case RVS_DemonReplyKind_EventBatch: {
    dst->event_batch.events = (DMN_EventList){0};
    for EachNode(n, DMN_EventNode, src->event_batch.events.first) {
      DMN_Event *event = dmn_event_list_push(arena, &dst->event_batch.events);
      rvs_demon_event_copy(arena, event, &n->v);
    }
  } break;
  case RVS_DemonReplyKind_ExecutionStopped: {
  } break;
  case RVS_DemonReplyKind_ExecutionFinished: {
  } break;
  default: { InvalidPath; } break;
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
  RVS_Result result = RVS_Result_Error;

  mutex_take(dmn->mutex);
  if (spec.type == RVS_DemonMessage_InterruptExecution) {
    // This command is necessarily out-of-band: the worker may be blocked in dmn_ctrl_run.
    if (ins_atomic_u32_eval(&dmn->state) == RVS_ThreadState_Live &&
        spec.request_id != 0 && spec.interrupt_execution.command_id != 0 &&
        spec.interrupt_execution.execution_request_id != 0 && dmn->is_run_in_flight &&
        dmn->active_run_request_id == spec.interrupt_execution.execution_request_id &&
        dmn->pending_interrupt_request_id == 0) {
      dmn->pending_interrupt_request_id = spec.request_id;
      dmn->pending_interrupt_execution_request_id = spec.interrupt_execution.execution_request_id;
      dmn->pending_interrupt_command_id = spec.interrupt_execution.command_id;
      if (dmn_halt(spec.interrupt_execution.command_id, spec.request_id)) {
        result = RVS_Result_Ok;
      } else {
        dmn->pending_interrupt_request_id = 0;
        dmn->pending_interrupt_execution_request_id = 0;
        dmn->pending_interrupt_command_id = 0;
      }
    }
  } else if (ins_atomic_u32_eval(&dmn->state) == RVS_ThreadState_Live) {
    AssertAlways(spec.request_id != 0);
    result = rvs_demon_push_message(dmn, &spec);
  }
  mutex_drop(dmn->mutex);

  ProfEnd();
  return result;
}

internal RVS_DemonInterruptCapability
rvs_demon_interrupt_capability(RVS_Demon *dmn)
{
  return dmn ? RVS_DemonInterruptCapability_GlobalWithResume : RVS_DemonInterruptCapability_Null;
}

internal void
rvs_demon_worker(void *user_data)
{
  ProfBeginFunction();
  RVS_Demon *dmn = user_data;

  // initialize DEMON platform state
  dmn_init();

  // grant current thread access to the DEMON API
  DMN_CtrlCtx *ctrl_ctx = dmn_ctrl_begin();
  ins_atomic_u32_eval_assign(&dmn->state, RVS_ThreadState_Live);

  for (B32 keep_running = 1; keep_running;) {
    // wait for the requests from the engine thread
    RVS_DemonMessage *message = rvs_queue_pop_struct(dmn->queue, RVS_DemonMessage, max_U64);

    // process the message
    RVS_DemonReply reply = {0};
    if (ins_atomic_u32_eval(&dmn->state) == RVS_ThreadState_Terminating &&
        message->type != RVS_DemonMessage_Shutdown) {
      rvs_queue_recycle(dmn->queue, &message->base);
      continue;
    }
    switch (message->type) {
    case RVS_DemonMessage_Launch: {
      Temp scratch = scratch_begin(0, 0);
      U32 pid = dmn_ctrl_launch(ctrl_ctx, &message->launch.params);
      reply.request_id             = message->request_id;
      if (pid == 0) {
        reply = (RVS_DemonReply){
          .kind       = RVS_DemonReplyKind_ActionResult,
          .request_id = message->request_id,
          .action_result = { .action = RVS_DemonAction_Launch, .result = RVS_Result_Error },
        };
      } else {
        reply = (RVS_DemonReply){
          .kind       = RVS_DemonReplyKind_LaunchStarted,
          .request_id = message->request_id,
          .launch_started = { .pid = pid },
        };
      }
      scratch_end(scratch);
    } break;
    case RVS_DemonMessage_Pump: {
      Temp scratch = scratch_begin(0, 0);
      DMN_EventList events = dmn_ctrl_pump(scratch.arena, ctrl_ctx);
      reply = (RVS_DemonReply){
        .kind        = RVS_DemonReplyKind_EventBatch,
        .request_id  = message->request_id,
        .event_batch = { .events = events, .command_id = message->pump.command_id },
      };
      dmn->reply_callback(dmn, &reply, dmn->reply_ud);
      reply = (RVS_DemonReply){0};
      scratch_end(scratch);
    } break;
    case RVS_DemonMessage_Run: {
      Temp scratch = scratch_begin(0, 0);
      if (!rvs_demon_traps_validate(&message->run.traps)) {
        reply = (RVS_DemonReply){
          .kind = RVS_DemonReplyKind_ActionResult,
          .request_id = message->request_id,
          .action_result = { .action = RVS_DemonAction_Run, .result = RVS_Result_Error },
        };
        scratch_end(scratch);
        break;
      }
      mutex_take(dmn->mutex);
      B32 run_accepted = ins_atomic_u32_eval(&dmn->state) == RVS_ThreadState_Live;
      if (run_accepted) {
        dmn->active_run_request_id = message->request_id;
        dmn->is_run_in_flight = 1;
      }
      mutex_drop(dmn->mutex);
      if (!run_accepted) { scratch_end(scratch); break; }
      RVS_DemonRunStarted started = {
        .demon = dmn,
        .request_id = message->request_id,
        .action = RVS_DemonAction_Run,
      };
      DMN_RunCtrls ctrls = {
        .run_entities              = message->run.processes,
        .run_entity_count          = message->run.processes_count,
        .run_entities_are_processes = 1,
        .run_entities_are_unfrozen = 1,
        .traps = message->run.traps,
        .run_started = rvs_demon_run_started,
        .run_started_user_data = &started,
      };
      DMN_EventList events = dmn_ctrl_run(scratch.arena, ctrl_ctx, &ctrls);
      AssertAlways(started.called);
      mutex_take(dmn->mutex);
      RVS_MessageID interrupt_request_id = dmn->pending_interrupt_request_id;
      RVS_MessageID interrupt_execution_request_id = dmn->pending_interrupt_execution_request_id;
      U64 interrupt_command_id = dmn->pending_interrupt_command_id;
      dmn->active_run_request_id = 0;
      dmn->pending_interrupt_request_id = 0;
      dmn->pending_interrupt_execution_request_id = 0;
      dmn->pending_interrupt_command_id = 0;
      dmn->is_run_in_flight = 0;
      mutex_drop(dmn->mutex);

      if (started.success && events.first) {
        reply = (RVS_DemonReply){
          .kind       = RVS_DemonReplyKind_EventBatch,
          .request_id = message->request_id,
          .event_batch = { .events = events },
        };
        dmn->reply_callback(dmn, &reply, dmn->reply_ud);
        reply = (RVS_DemonReply){0};
      }
      if (!started.success) {
        reply = (RVS_DemonReply){0};
      } else if (interrupt_request_id != 0) {
        AssertAlways(interrupt_execution_request_id == message->request_id && interrupt_command_id != 0);
        reply = (RVS_DemonReply){
          .kind = RVS_DemonReplyKind_ExecutionStopped,
          .request_id = interrupt_request_id,
          .execution_stopped = { .command_id = interrupt_command_id },
        };
      } else {
        reply = (RVS_DemonReply){
          .kind       = RVS_DemonReplyKind_ExecutionFinished,
          .request_id = message->request_id,
        };
      }
      scratch_end(scratch);
    } break;
    case RVS_DemonMessage_Resume: {
      Temp scratch = scratch_begin(0, 0);
      if (!rvs_demon_traps_validate(&message->resume.traps)) {
        reply = (RVS_DemonReply){
          .kind = RVS_DemonReplyKind_ActionResult,
          .request_id = message->request_id,
          .action_result = {
            .action = RVS_DemonAction_Resume,
            .result = RVS_Result_Error,
            .command_id = message->resume.command_id,
          },
        };
        scratch_end(scratch);
        break;
      }
      mutex_take(dmn->mutex);
      B32 resume_accepted = ins_atomic_u32_eval(&dmn->state) == RVS_ThreadState_Live;
      if (resume_accepted) {
        dmn->active_run_request_id = message->resume.execution_request_id;
        dmn->is_run_in_flight = 1;
      }
      mutex_drop(dmn->mutex);
      if (!resume_accepted) { scratch_end(scratch); break; }
      RVS_DemonRunStarted started = {
        .demon = dmn,
        .request_id = message->request_id,
        .action = RVS_DemonAction_Resume,
        .command_id = message->resume.command_id,
      };
      DMN_EventList events = {0};
      if (message->resume.processes_count != 0) {
        events = dmn_ctrl_run(scratch.arena, ctrl_ctx, &(DMN_RunCtrls){
          .run_entities = message->resume.processes,
          .run_entity_count = message->resume.processes_count,
          .run_entities_are_processes = 1,
          .run_entities_are_unfrozen = 1,
          .traps = message->resume.traps,
          .run_started = rvs_demon_run_started,
          .run_started_user_data = &started,
        });
      } else {
        rvs_demon_run_started(1, &started);
      }
      AssertAlways(started.called);
      if (started.success && events.first) {
        dmn->reply_callback(dmn, &(RVS_DemonReply){
          .kind = RVS_DemonReplyKind_EventBatch,
          .request_id = message->resume.execution_request_id,
          .event_batch = { .events = events },
        }, dmn->reply_ud);
      }
      mutex_take(dmn->mutex);
      RVS_MessageID interrupt_request_id = dmn->pending_interrupt_request_id;
      RVS_MessageID interrupt_execution_request_id = dmn->pending_interrupt_execution_request_id;
      U64 interrupt_command_id = dmn->pending_interrupt_command_id;
      dmn->active_run_request_id = 0;
      dmn->pending_interrupt_request_id = 0;
      dmn->pending_interrupt_execution_request_id = 0;
      dmn->pending_interrupt_command_id = 0;
      dmn->is_run_in_flight = 0;
      mutex_drop(dmn->mutex);
      if (!started.success) {
        reply = (RVS_DemonReply){0};
      } else if (interrupt_request_id != 0) {
        AssertAlways(interrupt_execution_request_id == message->resume.execution_request_id && interrupt_command_id != 0);
        reply = (RVS_DemonReply){
          .kind = RVS_DemonReplyKind_ExecutionStopped,
          .request_id = interrupt_request_id,
          .execution_stopped = { .command_id = interrupt_command_id },
        };
      } else {
        reply = (RVS_DemonReply){
          .kind = RVS_DemonReplyKind_ExecutionFinished,
          .request_id = message->resume.execution_request_id,
        };
      }
      scratch_end(scratch);
    } break;
    case RVS_DemonMessage_Terminate: {
      reply = (RVS_DemonReply){
        .kind = RVS_DemonReplyKind_ActionResult,
        .request_id = message->request_id,
        .action_result = { .action = RVS_DemonAction_Terminate, .result = RVS_Result_Ok },
      };
      for EachIndex(process_idx, message->terminate.process_count) {
        if ( ! dmn_ctrl_kill(ctrl_ctx, message->terminate.process_handles[process_idx], 0)) {
          reply.action_result.result = RVS_Result_Error;
          break;
        }
      }
    } break;
    case RVS_DemonMessage_InterruptExecution: {
      AssertAlways(message->request_id != 0);
    } break;
    case RVS_DemonMessage_Shutdown: {
      keep_running = 0;
    } break;
    default: { InvalidPath; } break;
    }

    // publish completed commands and event batches
    if (reply.kind != RVS_DemonReplyKind_Null) {
      dmn->reply_callback(dmn, &reply, dmn->reply_ud);
    }

    // recycle the message
    rvs_queue_recycle(dmn->queue, &message->base);
  }

  dmn_release();
  ins_atomic_u32_eval_assign(&dmn->state, RVS_ThreadState_Exited);
  ProfEnd();
}

