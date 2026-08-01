
#include "radvs/rvs_protocol.h"
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
} RVS_Demon;

internal void rvs_demon_worker(void *user_data);

RVS_Result
rvs_demon_init(void *reply_ud, RVS_DemonReplyCallback *reply_callback, RVS_Demon **dmn_out)
{
  local_persist RVS_Demon dmn;
  
  RVS_Result result = RVS_Result_Null;
  RVS_ThreadState state = ins_atomic_u32_eval_cond_assign(&dmn.state, RVS_ThreadState_Initing, RVS_ThreadState_Null);
  if (state == RVS_ThreadState_Null) {
    dmn.state          = RVS_ThreadState_Initing;
    dmn.arena          = arena_alloc();
    dmn.mutex          = mutex_alloc();
    dmn.reply_callback = reply_callback;
    dmn.reply_ud       = reply_ud;
    dmn.queue          = rvs_queue_alloc(dmn.arena, sizeof(RVS_DemonMessage), AlignOf(RVS_DemonMessage));
    dmn.worker         = thread_launch(rvs_demon_worker, 0);
    if ( ! MemoryIsZeroStruct(&dmn.worker)) {
      result = RVS_Result_Ok;
    }
  }

  // wait for the DEMON thread to boot
  for (; ins_atomic_u32_eval(&dmn.state) != RVS_ThreadState_Initing; ) { sleep_ms(1); }

  if (dmn_out) {
    *dmn_out = &dmn;
  }

  return result;
}

RVS_Result
rvs_demon_shutdown(void)
{
  NotImplemented;
  //mutex_release(g_dmn_instance.mutex);
  return RVS_Result_Error;
}

internal void
rvs_demon_recycle_request(RVS_QueueMessage *request)
{
  NotImplemented;
}

internal RVS_DemonMessage *
rvs_demon_message_copy(Arena *arena, RVS_DemonMessage *src)
{
  RVS_DemonMessage *dst = push_array_no_zero(arena, RVS_DemonMessage, 1);

  *dst = *src;
  switch (src->type) {
  case RVS_DemonMessage_Launch: {
    dst->launch.params = *process_launch_params_copy(arena, &src->launch.params);
  } break;
  case RVS_DemonMessage_LaunchAck: {
  } break;
  case RVS_DemonMessage_Run: {
    dst->run.process_handles = push_array(arena, DMN_Handle, src->run.process_count);
    dst->run.process_count   = src->run.process_count;
    MemoryCopyTyped(dst->run.process_handles, src->run.process_handles, src->run.process_count);
  } break;
  case RVS_DemonMessage_Halt: {
  } break;
  case RVS_DemonMessage_Terminate: {
    dst->terminate.process_handles = push_array_no_zero(arena, DMN_Handle, src->terminate.process_count);
    dst->terminate.process_count   = src->terminate.process_count;
  } break;
  default: { InvalidPath; } break;
  }

  return dst;
}

internal RVS_Result
rvs_demon_send_message(RVS_Demon *dmn, RVS_DemonMessage spec, RVS_MessageID *reply_id_out)
{
  RVS_Result result = RVS_Result_Error;

  MutexScope(dmn->mutex) {
    // alloc new DEMON message
    RVS_DemonMessage *message = (RVS_DemonMessage *)rvs_queue_alloc_message(dmn->queue);

    // copy message contents
    message = rvs_demon_message_copy(dmn->message_arena, &spec);
    if (result != RVS_Result_Ok) { goto exit; }

    // send message to the DEMON worker
    result = rvs_queue_push(dmn->queue, &message->base);
    if (result != RVS_Result_Ok) { goto exit; }

    if (reply_id_out) {
      *reply_id_out = message->base.reply_id;
    }

    exit:;
    if (result != RVS_Result_Ok) {
      // TODO: free message
      NotImplemented;
    }
  }

  return result;
}

internal RVS_Result
rvs_demon_launch(RVS_Demon *dmn, ProcessLaunchParams params, RVS_MessageID *reply_id_out)
{
  RVS_DemonMessage spec = {
    .type   = RVS_DemonMessage_Launch,
    .launch = { .params = params }
  };
  return rvs_demon_send_message(dmn, spec, reply_id_out);
}

internal void
rvs_demon_worker(void *user_data)
{
  RVS_Demon *dmn = user_data;

  // initialize DEMON platform state
  dmn_init();

  // grant current thread access to the DEMON API
  DMN_CtrlCtx *ctrl_ctx = dmn_ctrl_begin();

  for (;;) {
    // wait for the requests from the engine thread
    RVS_DemonMessage *message = (RVS_DemonMessage *)rvs_queue_pop(dmn->queue, max_U64);

    // process the message
    RVS_DemonMessage reply = {0};
    switch (message->type) {
    case RVS_DemonMessage_Launch: {
      reply.type           = RVS_DemonMessage_LaunchAck;
      reply.launch_ack.pid = dmn_ctrl_launch(ctrl_ctx, &message->launch.params);
    } break;
    default: { InvalidPath; } break;
    }

    // reply to the engine thread
    dmn->reply_callback(message->base.reply_id, &reply, dmn->reply_ud);
  }
}

