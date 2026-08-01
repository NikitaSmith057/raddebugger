#include "radvs/rvs_engine.h"

struct RVS_Engine
{
  Arena     *arena;
  RVS_Queue *queue;
  RVS_Demon *dmn;

  Arena       *program_arena;
  RVS_Program *first_program;
  RVS_Program *last_program;

  // engine reply thread guards
  CondVar          reply_cv;
  Mutex            reply_mutex;
  RVS_EngineReply *reply_first;
  RVS_EngineReply *reply_last;
  RVS_EngineReply *reply_free_list;

  // demon reply thread guards
  RVS_Demon *demon;
  Arena     *demon_reply_arena;
  Mutex      demon_reply_mutex;
  ArenaNode *demon_reply_arena_active_list;
  ArenaNode *demon_reply_arena_free_list;

  Thread thread;
};

RVS_Result
rvs_engine_wait_for_event(RVS_Engine *engine, U64 wait_us, RVS_Event *event_out)
{
  // TODO: how should the DEMON events be supplied to the engine thread here?
  return RVS_Result_Ok;
}

internal void
rvs_engine_worker(void *user_data)
{
  RVS_Engine *engine = user_data;
  
  for (;;) {
    RVS_EngineMessage *message = (RVS_EngineMessage *)rvs_queue_pop(engine->queue, max_U64);

    switch (message->type) {
    case RVS_EngineMessageType_DemonReply: {
      RVS_DemonMessage *demon_reply = &message->demon_reply;
      switch (demon_reply->type) {
      case RVS_DemonMessage_LaunchAck: {
        RVS_Program *prog = push_array(engine->program_arena, RVS_Program, 1);
        prog->arena = arena_alloc_(&(ArenaParams){ .name = "Engine Program" });
        prog->pid   = demon_reply->launch_ack.pid;
        SLLQueuePush(engine->first_program, engine->last_program, prog);
      } break;
      default: { InvalidPath; } break;
      }
    } break;
    default: { InvalidPath; } break;
    }
  }
}

// callback for when DEMON thread completes a message request
internal RVS_MessageID
rvs_engine_demon_reply_callback(RVS_MessageID reply_id, RVS_DemonMessage *reply, void *ud)
{
  RVS_Engine *engine = ud;

  // pick arena for the reply copy
  Arena *reply_arena = 0;
  {
    mutex_take(engine->demon_reply_mutex);

    ArenaNode *reply_arena_node = engine->demon_reply_arena_free_list;
    if (engine->demon_reply_arena_free_list) {
      SLLStackPop(engine->demon_reply_arena_free_list);
    }

    if (reply_arena_node == 0) {
      reply_arena_node    = push_array(engine->demon_reply_arena, ArenaNode, 1);
      reply_arena_node->v = arena_alloc();
    }

    SLLStackPush(engine->demon_reply_arena_active_list, reply_arena_node);
    reply_arena = reply_arena_node->v;

    mutex_drop(engine->demon_reply_mutex);
  }

  // copy & send the engine thread 
  arena_clear(reply_arena);
  RVS_EngineMessage message = {
    .type        = RVS_EngineMessageType_DemonReply,
    .demon_reply = *rvs_demon_message_copy(reply_arena, reply),
  };
  RVS_MessageID engine_reply_id;
  rvs_engine_send_message(engine, message, &engine_reply_id);

  return engine_reply_id;
}

RVS_Result
rvs_engine_init(RVS_Engine **engine_out)
{
  // TODO: check if the engine was already inited
  static RVS_Engine engine = {0};
  engine.arena             = arena_alloc();
  engine.queue             = rvs_queue_alloc(engine.arena, sizeof(RVS_EngineMessage), AlignOf(RVS_EngineMessage));
  engine.program_arena     = arena_alloc();
  engine.reply_cv          = cond_var_alloc();
  engine.reply_mutex       = mutex_alloc();
  engine.demon_reply_arena = arena_alloc();
  engine.demon_reply_mutex = mutex_alloc();
  engine.thread            = thread_launch(rvs_engine_worker, &engine);

  // launch the DEMON thread
  rvs_demon_init(&engine, rvs_engine_demon_reply_callback, &engine.demon);

  *engine_out = &engine;
  return RVS_Result_Ok;
}

void
rvs_engine_shutdown(RVS_Engine *engine)
{
  NotImplemented;
}

internal RVS_EngineMessage *
rvs_engine_message_copy(Arena *arena, RVS_EngineMessage *message)
{
  NotImplemented;
  return 0;
}

RVS_Result
rvs_engine_send_message(RVS_Engine *engine, RVS_EngineMessage message, RVS_MessageID *reply_id_out)
{
  NotImplemented;
  return RVS_Result_Error;
}

RVS_Result
rvs_engine_wait_for_reply(Arena *arena, RVS_Engine *engine, RVS_MessageID reply_id, U64 wait_us, RVS_EngineMessage *reply_out)
{
  RVS_Result result = RVS_Result_Error;

  U64 endt_us = max_U64;
  if (wait_us != max_U64) {
    U64 now_us = now_time_us();
    endt_us = now_us + Min(wait_us, max_U64 - now_us);
  }

  mutex_take(engine->reply_mutex);

  // lookup reply
  RVS_EngineReply *reply = 0;
  for EachNode(n, RVS_EngineReply, engine->reply_first) {
    if (n->id == reply_id) {
      reply = n;
      DLLRemove(engine->reply_first, engine->reply_last, reply);
      break;
    }
  }
  if (reply == 0) { goto exit; }

  // wait for the API
  while (reply->result == RVS_Result_Null) {
    // releases reply mutex while sleeping and reacquires it on return
    if ( ! cond_var_wait(engine->reply_cv, engine->reply_mutex, endt_us)) {
      result = RVS_Result_Timeout;
      goto exit;
    }
  }

  // read out request result
  result = reply->result;

  // was ok? -> copy reply message
  if (result == RVS_Result_Ok) {
    *reply_out = *rvs_engine_message_copy(arena, &reply->message);
  }

  // put the reply message on the free list
  SLLStackPush(engine->reply_free_list, reply);

  exit:;
  mutex_drop(engine->reply_mutex);
  return result;
}

RVS_Result
rvs_engine_launch_async(RVS_Engine *engine, String8 cmdl, String8 wdir, RVS_MessageID *reply_id_out)
{
  Temp scratch = scratch_begin(0,0);

  String8List         cmd_line = str8_split_by_string_chars(scratch.arena, cmdl, str8_lit(" "), 0);
  U32                 pid      = 0;
  ProcessLaunchParams params   = { .cmd_line = cmd_line, .path = wdir };
  RVS_DemonMessage    spec     = { .type = RVS_DemonMessage_Launch, .launch = { .params = params, } };
  RVS_MessageID       reply_id = 0;
  RVS_Result          result   = rvs_demon_send_message(engine->dmn, spec, &reply_id);
  if (result == RVS_Result_Ok && reply_id_out) {
    *reply_id_out = reply_id;
  }

  scratch_end(scratch);
  return pid;
}

RVS_Result
rvs_engine_launch(RVS_Engine *engine, String8 cmdl, String8 wdir, U64 wait_us, U32 *pid_out)
{
  Temp scratch = scratch_begin(0,0);

  RVS_Result result = RVS_Result_Error;

  RVS_MessageID reply_id;
  result = rvs_engine_launch_async(engine, cmdl, wdir, &reply_id);
  if (result != RVS_Result_Ok) { goto exit; }

  RVS_EngineMessage reply  = {0};
  result = rvs_engine_wait_for_reply(scratch.arena, engine, reply_id, wait_us, &reply);
  if (result != RVS_Result_Ok) { goto exit; }

  AssertAlways(reply.type == RVS_EngineMessageType_DemonReply);
  AssertAlways(reply.demon_reply.type == RVS_DemonMessage_LaunchAck);
  if (pid_out) {
    *pid_out = reply.demon_reply.launch_ack.pid;
  }

  exit:;
  scratch_end(scratch);
  return result;
}

RVS_Result
rvs_engine_wait_for_notification(Arena *arena, RVS_Engine *engine, U64 wait_us, RVS_Notification *notification_out)
{
  NotImplemented;
  return RVS_Result_Error;
}
