#include "radvs/rvs_engine.h"

struct RVS_Engine
{
  Arena     *arena;
  RVS_Queue *queue;
  Mutex      message_mutex;
  RVS_Demon *dmn;

  Arena         *program_arena;
  RVS_ProgramID  next_program_id;
  RVS_Program   *first_program;
  RVS_Program   *last_program;

  CondVar          reply_cv;
  Mutex            reply_mutex;
  RVS_EngineReply *reply_first;
  RVS_EngineReply *reply_last;
  RVS_EngineReply *reply_free_list;

  Thread thread;
};

internal void
rvs_engine_message_copy(Arena *arena, RVS_EngineMessage *dst, RVS_EngineMessage *src)
{
  RVS_QueueMessage base = dst->base;
  *dst = *src;
  dst->base = base;

  switch (src->type) {
  case RVS_EngineMessageType_Launch: {
    dst->launch.params = *process_launch_params_copy(arena, &src->launch.params);
  } break;
  case RVS_EngineMessageType_DemonReply: {
    rvs_demon_message_copy(arena, &dst->demon_reply.message, &src->demon_reply.message);
  } break;
  default: { InvalidPath; } break;
  }
}

internal RVS_Result
rvs_engine_enqueue_message(RVS_Engine *engine, RVS_EngineMessage *spec, RVS_MessageID *reply_id_out)
{
  RVS_Result result = RVS_Result_Error;

  // The engine arena is shared by API callers and the DEMON callback.
  mutex_take(engine->message_mutex);
  RVS_EngineMessage *message = (RVS_EngineMessage *)rvs_queue_alloc_message(engine->queue);
  rvs_engine_message_copy(engine->arena, message, spec);
  result = rvs_queue_push(engine->queue, &message->base);
  if (result == RVS_Result_Ok && reply_id_out) {
    *reply_id_out = message->base.reply_id;
  }
  mutex_drop(engine->message_mutex);

  return result;
}

internal RVS_EngineReply *
rvs_engine_reply_alloc(RVS_Engine *engine, RVS_MessageID reply_id)
{
  RVS_EngineReply *reply = engine->reply_free_list;
  if (reply) {
    SLLStackPop(engine->reply_free_list);
    MemoryZeroStruct(reply);
  } else {
    reply = push_array(engine->arena, RVS_EngineReply, 1);
  }
  reply->reply.reply_id = reply_id;
  reply->reply.result   = RVS_Result_Null;
  DLLPushBack(engine->reply_first, engine->reply_last, reply);
  return reply;
}

internal void
rvs_engine_complete_reply(RVS_Engine *engine, RVS_Reply reply)
{
  mutex_take(engine->reply_mutex);
  for EachNode(n, RVS_EngineReply, engine->reply_first) {
    if (n->reply.reply_id == reply.reply_id) {
      n->reply = reply;
      cond_var_broadcast(engine->reply_cv);
      break;
    }
  }
  mutex_drop(engine->reply_mutex);
}

internal void
rvs_engine_worker(void *user_data)
{
  RVS_Engine *engine = user_data;

  for (;;) {
    RVS_EngineMessage *message = (RVS_EngineMessage *)rvs_queue_pop(engine->queue, max_U64);
    if (message == 0) {
      continue;
    }

    switch (message->type) {
    case RVS_EngineMessageType_Launch: {
      RVS_DemonMessage spec = {
        .type     = RVS_DemonMessage_Launch,
        .reply_to = message->base.reply_id,
        .launch   = { .params = message->launch.params },
      };
      RVS_Result result = rvs_demon_send_message(engine->dmn, spec, 0);
      if (result != RVS_Result_Ok) {
        rvs_engine_complete_reply(engine, (RVS_Reply){
          .reply_id = message->base.reply_id,
          .result   = result,
          .kind     = RVS_ReplyKind_LaunchAck,
        });
      }
    } break;

    case RVS_EngineMessageType_DemonReply: {
      RVS_DemonMessage *demon_reply = &message->demon_reply.message;
      switch (demon_reply->type) {
      case RVS_DemonMessage_LaunchAck: {
        RVS_Program *prog = push_array(engine->program_arena, RVS_Program, 1);
        prog->arena = arena_alloc(.name = "Engine Program");
        prog->id    = ++engine->next_program_id;
        prog->pid   = demon_reply->launch_ack.pid;
        SLLQueuePush(engine->first_program, engine->last_program, prog);

        rvs_engine_complete_reply(engine, (RVS_Reply){
          .reply_id = message->demon_reply.reply_id,
          .result   = RVS_Result_Ok,
          .kind     = RVS_ReplyKind_LaunchAck,
          .launch_ack = {
            .program_id = prog->id,
            .pid        = prog->pid,
          },
        });
      } break;
      default: { InvalidPath; } break;
      }
    } break;

    default: { InvalidPath; } break;
    }

    rvs_queue_recycle(engine->queue, &message->base);
  }
}

// Called on the DEMON worker thread after it completes a request.
internal RVS_MessageID
rvs_engine_demon_reply_callback(RVS_MessageID reply_id, RVS_DemonMessage *reply, void *ud)
{
  RVS_Engine *engine = ud;
  RVS_EngineMessage message = {
    .type = RVS_EngineMessageType_DemonReply,
    .demon_reply = {
      .message  = *reply,
      .reply_id = reply_id,
    },
  };
  rvs_engine_enqueue_message(engine, &message, 0);
  return reply_id;
}

RVS_Result
rvs_engine_init(RVS_Engine **engine_out)
{
  if (engine_out == 0) {
    return RVS_Result_Error;
  }

  static RVS_Engine engine = {0};
  engine.arena         = arena_alloc();
  engine.queue         = rvs_queue_alloc(engine.arena, sizeof(RVS_EngineMessage), AlignOf(RVS_EngineMessage));
  engine.message_mutex = mutex_alloc();
  engine.program_arena = arena_alloc();
  engine.reply_cv      = cond_var_alloc();
  engine.reply_mutex   = mutex_alloc();

  RVS_Result result = rvs_demon_init(&engine, rvs_engine_demon_reply_callback, &engine.dmn);
  if (result != RVS_Result_Ok) {
    return result;
  }

  engine.thread = thread_launch(rvs_engine_worker, &engine);
  if (MemoryIsZeroStruct(&engine.thread)) {
    return RVS_Result_Error;
  }

  *engine_out = &engine;
  return RVS_Result_Ok;
}

void
rvs_engine_shutdown(RVS_Engine *engine)
{
  (void)engine;
  NotImplemented;
}

RVS_Result
rvs_engine_send_message(RVS_Engine *engine, RVS_EngineMessage message, RVS_MessageID *reply_id_out)
{
  // Register the reply before making the command visible to the worker.
  mutex_take(engine->message_mutex);
  RVS_EngineMessage *queued_message = (RVS_EngineMessage *)rvs_queue_alloc_message(engine->queue);
  rvs_engine_message_copy(engine->arena, queued_message, &message);
  RVS_MessageID reply_id = queued_message->base.reply_id;
  mutex_take(engine->reply_mutex);
  rvs_engine_reply_alloc(engine, reply_id);
  mutex_drop(engine->reply_mutex);

  RVS_Result result = rvs_queue_push(engine->queue, &queued_message->base);
  mutex_drop(engine->message_mutex);
  if (result != RVS_Result_Ok) {
    return result;
  }

  if (reply_id_out) {
    *reply_id_out = reply_id;
  }
  return RVS_Result_Ok;
}

RVS_Result
rvs_engine_wait_for_reply(Arena *arena, RVS_Engine *engine, RVS_MessageID reply_id, U64 wait_us, RVS_Reply *reply_out)
{
  (void)arena;

  U64 endt_us = max_U64;
  if (wait_us != max_U64) {
    U64 now_us = now_time_us();
    endt_us = now_us + Min(wait_us, max_U64 - now_us);
  }

  mutex_take(engine->reply_mutex);

  RVS_EngineReply *reply = 0;
  for EachNode(n, RVS_EngineReply, engine->reply_first) {
    if (n->reply.reply_id == reply_id) {
      reply = n;
      break;
    }
  }
  if (reply == 0) {
    mutex_drop(engine->reply_mutex);
    return RVS_Result_Error;
  }

  while (reply->reply.result == RVS_Result_Null) {
    if ( ! cond_var_wait(engine->reply_cv, engine->reply_mutex, endt_us)) {
      mutex_drop(engine->reply_mutex);
      return RVS_Result_Timeout;
    }
  }

  RVS_Result result = reply->reply.result;
  if (reply_out) {
    *reply_out = reply->reply;
  }
  DLLRemove(engine->reply_first, engine->reply_last, reply);
  SLLStackPush(engine->reply_free_list, reply);
  mutex_drop(engine->reply_mutex);

  return result;
}

RVS_Result
rvs_engine_launch_async(RVS_Engine *engine, String8 cmdl, String8 wdir, RVS_MessageID *reply_id_out)
{
  Temp scratch = scratch_begin(0, 0);
  String8List cmd_line = str8_split_by_string_chars(scratch.arena, cmdl, str8_lit(" "), 0);
  RVS_EngineMessage message = {
    .type   = RVS_EngineMessageType_Launch,
    .launch = { .params = { .cmd_line = cmd_line, .path = wdir } },
  };
  RVS_Result result = rvs_engine_send_message(engine, message, reply_id_out);
  scratch_end(scratch);
  return result;
}

RVS_Result
rvs_engine_launch(RVS_Engine *engine, String8 cmdl, String8 wdir, U64 wait_us, U32 *pid_out)
{
  Temp scratch = scratch_begin(0, 0);
  RVS_MessageID reply_id = 0;
  RVS_Result result = rvs_engine_launch_async(engine, cmdl, wdir, &reply_id);
  if (result == RVS_Result_Ok) {
    RVS_Reply reply = {0};
    result = rvs_engine_wait_for_reply(scratch.arena, engine, reply_id, wait_us, &reply);
    if (result == RVS_Result_Ok && reply.kind == RVS_ReplyKind_LaunchAck && pid_out) {
      *pid_out = reply.launch_ack.pid;
    }
  }
  scratch_end(scratch);
  return result;
}

RVS_Result
rvs_engine_wait_for_event(RVS_Engine *engine, U64 wait_us, RVS_Event *event_out)
{
  (void)engine;
  (void)wait_us;
  (void)event_out;
  return RVS_Result_Error;
}
