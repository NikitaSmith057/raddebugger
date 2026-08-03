
#include "radvs/rvs_async.h"
#include "radvs/rvs_demon.h"

typedef struct RVS_Demon
{
  Arena                  *arena;
  Arena                  *message_arena;
  Mutex                   mutex;
  RVS_Queue              *queue;
  RVS_ThreadState         state;
  Thread                  worker;
  RVS_DemonReplyCallback *reply_callback;
  void                   *reply_ud;
  RVS_MessageID           active_run_request_id;
  RVS_MessageID           pending_interrupt_request_id;
  DMN_Handle              *pending_interrupt_processes;
  U64                      pending_interrupt_processes_count;
  B32                     is_run_in_flight;
} RVS_Demon;

global RVS_Demon g_rvs_demon;

internal void rvs_demon_worker(void *user_data);
internal RVS_Result rvs_demon_push_message(RVS_Demon *dmn, RVS_DemonMessage *spec);

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
    dmn->message_arena  = arena_alloc();
    dmn->mutex          = mutex_alloc();
    dmn->reply_callback = reply_callback;
    dmn->reply_ud       = reply_ud;
    dmn->queue          = rvs_queue_alloc(dmn->arena, sizeof(RVS_DemonMessage), AlignOf(RVS_DemonMessage));
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
  
  if (ins_atomic_u32_eval(&dmn->state) == RVS_ThreadState_Live) {
    // Interrupt a blocking dmn_ctrl_run so the worker can consume Shutdown.
    dmn_halt(0, 0);

    // send message to the demon worker to shutdown and update thread worker state
    mutex_take(dmn->mutex);
    if (ins_atomic_u32_eval(&dmn->state) == RVS_ThreadState_Live) {
      result = rvs_demon_push_message(dmn, &(RVS_DemonMessage){ .type = RVS_DemonMessage_Shutdown });
      if (result == RVS_Result_Ok) {
        ins_atomic_u32_eval_assign(&dmn->state, RVS_ThreadState_Terminating);
      }
    }
    mutex_drop(dmn->mutex);

    if (result != RVS_Result_Ok) {
      return_result = result;
      goto exit;
    }

    // release DEMON thread resources
    thread_join(dmn->worker, max_U64);
    rvs_queue_release(dmn->queue);
    mutex_release(dmn->mutex);
    arena_release(dmn->message_arena);
    arena_release(dmn->arena);
    MemoryZeroStruct(dmn);
  }

  exit:;
  ProfEnd();
  return return_result;
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
  } break;
  case RVS_DemonMessage_Resume: {
    dst->resume.processes = push_array(arena, DMN_Handle, src->resume.processes_count);
    dst->resume.processes_count = src->resume.processes_count;
    dst->resume.execution_request_id = src->resume.execution_request_id;
    MemoryCopyTyped(dst->resume.processes, src->resume.processes, src->resume.processes_count);
  } break;
  case RVS_DemonMessage_Halt: {
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
  RVS_DemonMessage *message = rvs_queue_alloc_struct(dmn->queue, RVS_DemonMessage);
  rvs_demon_message_copy(dmn->message_arena, message, spec);
  RVS_Result result = rvs_queue_push(dmn->queue, &message->base);
  ProfEnd();
  return result;
}

internal RVS_Result
rvs_demon_send_message(RVS_Demon *dmn, RVS_DemonMessage spec)
{
  ProfBeginFunction();
  RVS_Result result = RVS_Result_Error;

  // A queued halt cannot interrupt the control thread while dmn_ctrl_run blocks.
  if (spec.type == RVS_DemonMessage_Halt) {
    ProfEnd();
    return RVS_Result_Unsupported;
  }

  mutex_take(dmn->mutex);
  if (ins_atomic_u32_eval(&dmn->state) == RVS_ThreadState_Live) {
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

internal RVS_Result
rvs_demon_interrupt(RVS_Demon *dmn, RVS_MessageID request_id, DMN_Handle *selected_processes, U64 selected_processes_count)
{
  if (dmn == 0 || request_id == 0 || selected_processes == 0 || selected_processes_count == 0) { return RVS_Result_Error; }
  RVS_Result result = RVS_Result_Error;
  mutex_take(dmn->mutex);
  if (ins_atomic_u32_eval(&dmn->state) == RVS_ThreadState_Live &&
      dmn->is_run_in_flight && dmn->pending_interrupt_request_id == 0) {
    dmn->pending_interrupt_request_id = request_id;
    dmn->pending_interrupt_processes = push_array(dmn->arena, DMN_Handle, selected_processes_count);
    dmn->pending_interrupt_processes_count = selected_processes_count;
    MemoryCopyTyped(dmn->pending_interrupt_processes, selected_processes, selected_processes_count);
    dmn_halt(0, request_id);
    result = RVS_Result_Ok;
  }
  mutex_drop(dmn->mutex);
  return result;
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
        .event_batch = { .events = events },
      };
      dmn->reply_callback(dmn, &reply, dmn->reply_ud);
      reply = (RVS_DemonReply){0};
      scratch_end(scratch);
    } break;
    case RVS_DemonMessage_Run: {
      Temp scratch = scratch_begin(0, 0);
      mutex_take(dmn->mutex);
      dmn->active_run_request_id = message->request_id;
      dmn->is_run_in_flight = 1;
      mutex_drop(dmn->mutex);
      reply.kind       = RVS_DemonReplyKind_ActionResult;
      reply.request_id = message->request_id;
      reply.action_result.action = RVS_DemonAction_Run;
      reply.action_result.result = RVS_Result_Ok;
      dmn->reply_callback(dmn, &reply, dmn->reply_ud);
      reply = (RVS_DemonReply){0};

      DMN_RunCtrls ctrls = {
        .run_entities              = message->run.processes,
        .run_entity_count          = message->run.processes_count,
        .run_entities_are_processes = 1,
        .run_entities_are_unfrozen = 1,
      };
      DMN_EventList events = dmn_ctrl_run(scratch.arena, ctrl_ctx, &ctrls);
      mutex_take(dmn->mutex);
      RVS_MessageID interrupt_request_id = dmn->pending_interrupt_request_id;
      dmn->active_run_request_id = 0;
      dmn->pending_interrupt_request_id = 0;
      dmn->pending_interrupt_processes = 0;
      dmn->pending_interrupt_processes_count = 0;
      dmn->is_run_in_flight = interrupt_request_id != 0;
      mutex_drop(dmn->mutex);

      if (events.first) {
        reply = (RVS_DemonReply){
          .kind       = RVS_DemonReplyKind_EventBatch,
          .request_id = message->request_id,
          .event_batch = { .events = events },
        };
        dmn->reply_callback(dmn, &reply, dmn->reply_ud);
        reply = (RVS_DemonReply){0};
      }
      if (interrupt_request_id == 0) {
        reply = (RVS_DemonReply){
          .kind       = RVS_DemonReplyKind_ExecutionFinished,
          .request_id = message->request_id,
        };
      }
      scratch_end(scratch);
    } break;
    case RVS_DemonMessage_Resume: {
      Temp scratch = scratch_begin(0, 0);
      mutex_take(dmn->mutex);
      dmn->active_run_request_id = message->resume.execution_request_id;
      dmn->is_run_in_flight = 1;
      mutex_drop(dmn->mutex);
      reply = (RVS_DemonReply){
        .kind = RVS_DemonReplyKind_ActionResult,
        .request_id = message->request_id,
        .action_result = { .action = RVS_DemonAction_Resume, .result = RVS_Result_Ok },
      };
      dmn->reply_callback(dmn, &reply, dmn->reply_ud);
      reply = (RVS_DemonReply){0};
      DMN_EventList events = {0};
      if (message->resume.processes_count != 0) {
        events = dmn_ctrl_run(scratch.arena, ctrl_ctx, &(DMN_RunCtrls){
          .run_entities = message->resume.processes,
          .run_entity_count = message->resume.processes_count,
          .run_entities_are_processes = 1,
          .run_entities_are_unfrozen = 1,
        });
      }
      if (events.first) {
        dmn->reply_callback(dmn, &(RVS_DemonReply){
          .kind = RVS_DemonReplyKind_EventBatch,
          .request_id = message->resume.execution_request_id,
          .event_batch = { .events = events },
        }, dmn->reply_ud);
      }
      mutex_take(dmn->mutex);
      RVS_MessageID interrupt_request_id = dmn->pending_interrupt_request_id;
      dmn->active_run_request_id = 0;
      dmn->pending_interrupt_request_id = 0;
      dmn->pending_interrupt_processes = 0;
      dmn->pending_interrupt_processes_count = 0;
      dmn->is_run_in_flight = interrupt_request_id != 0;
      mutex_drop(dmn->mutex);
      if (interrupt_request_id == 0) {
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
    case RVS_DemonMessage_Halt: {
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

