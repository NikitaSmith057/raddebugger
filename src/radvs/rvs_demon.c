
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

global RVS_Demon g_rvs_demon;

internal void rvs_demon_worker(void *user_data);
internal RVS_Result rvs_demon_push_message(RVS_Demon *dmn, RVS_DemonMessage *spec, RVS_MessageID *reply_id_out);

RVS_Result
rvs_demon_init(void *reply_ud, RVS_DemonReplyCallback *reply_callback, RVS_Demon **dmn_out)
{
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
    if (ins_atomic_u32_eval(&dmn->state) != RVS_ThreadState_Running) {
      // TODO: handle the error
      NotImplemented;
    }
  }

  if (dmn_out) {
    *dmn_out = dmn;
  }

  return result;
}

RVS_Result
rvs_demon_shutdown(RVS_Demon *dmn)
{
  RVS_Result result = RVS_Result_Error;
  
  if (ins_atomic_u32_eval(&dmn->state) == RVS_ThreadState_Running) {
    // send message to the demon worker to shutdown and update thread worker state
    mutex_take(dmn->mutex);
    if (ins_atomic_u32_eval(&dmn->state) == RVS_ThreadState_Running) {
      result = rvs_demon_push_message(dmn, &(RVS_DemonMessage){ .type = RVS_DemonMessage_Shutdown }, 0);
      if (result == RVS_Result_Ok) {
        ins_atomic_u32_eval_assign(&dmn->state, RVS_ThreadState_Terminating);
      }
    }
    mutex_drop(dmn->mutex);

    if (result != RVS_Result_Ok) {
      return result;
    }

    // release DEMON thread resources
    thread_join(dmn->worker, max_U64);
    rvs_queue_release(dmn->queue);
    mutex_release(dmn->mutex);
    arena_release(dmn->message_arena);
    arena_release(dmn->arena);
    MemoryZeroStruct(dmn);
  }
  
  return RVS_Result_Ok;
}

internal void
rvs_demon_recycle_request(RVS_QueueMessage *request)
{
  NotImplemented;
}

internal void
rvs_demon_message_copy(Arena *arena, RVS_DemonMessage *dst, RVS_DemonMessage *src)
{
  RVS_QueueMessage base = dst->base;
  *dst = *src;
  dst->base = base;
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
  case RVS_DemonMessage_Shutdown: {
  } break;
  default: { InvalidPath; } break;
  }
}

internal RVS_Result
rvs_demon_push_message(RVS_Demon *dmn, RVS_DemonMessage *spec, RVS_MessageID *reply_id_out)
{
  RVS_DemonMessage *message = (RVS_DemonMessage *)rvs_queue_alloc_message(dmn->queue); // alloc message
  rvs_demon_message_copy(dmn->message_arena, message, spec);                           // fill out message
  RVS_Result result = rvs_queue_push(dmn->queue, &message->base);                      // push message to the DEMON thread queue
  if (result == RVS_Result_Ok && reply_id_out) {
    *reply_id_out = message->base.reply_id;                                            // export reply message identifier
  }
  return result;
}

internal RVS_Result
rvs_demon_send_message(RVS_Demon *dmn, RVS_DemonMessage spec, RVS_MessageID *reply_id_out)
{
  RVS_Result result = RVS_Result_Error;

  mutex_take(dmn->mutex);
  if (ins_atomic_u32_eval(&dmn->state) == RVS_ThreadState_Running) {
    result = rvs_demon_push_message(dmn, &spec, reply_id_out);
  }
  mutex_drop(dmn->mutex);

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
  ins_atomic_u32_eval_assign(&dmn->state, RVS_ThreadState_Running);

  for (;;) {
    // wait for the requests from the engine thread
    RVS_DemonMessage *message = (RVS_DemonMessage *)rvs_queue_pop(dmn->queue, max_U64);

    // process the message
    RVS_DemonMessage reply = {0};
    B32 should_exit = 0;
    switch (message->type) {
    case RVS_DemonMessage_Launch: {
      reply.type           = RVS_DemonMessage_LaunchAck;
      reply.launch_ack.pid = dmn_ctrl_launch(ctrl_ctx, &message->launch.params);
    } break;
    case RVS_DemonMessage_Shutdown: {
      should_exit = 1;
    } break;
    default: { InvalidPath; } break;
    }

    // reply to the engine thread
    if (reply.type != RVS_DemonMessage_Null) {
      RVS_MessageID reply_id = message->reply_to ? message->reply_to : message->base.reply_id;
      dmn->reply_callback(reply_id, &reply, dmn->reply_ud);
    }
    rvs_queue_recycle(dmn->queue, &message->base);
    if (should_exit) {
      break;
    }
  }

  dmn_release();
  ins_atomic_u32_eval_assign(&dmn->state, RVS_ThreadState_Exited);
}

