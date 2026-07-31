#include "radvs/rvs_engine.h"

struct RVS_Engine
{
  Arena     *arena;
  RVS_Queue *queue;
  RVS_Demon *dmn;

  Arena       *program_arena;
  RVS_Program *first_program;
  RVS_Program *last_program;

  Mutex      demon_reply_mutex;
  Arena     *demon_reply_arena;
  ArenaNode *demon_reply_arena_active_list;
  ArenaNode *demon_reply_arena_free_list;
};

RVS_Result
rvs_engine_send_message(RVS_Engine *engine, RVS_EngineMessage message, RVS_ReplyID *reply_id_out)
{
  NotImplemented;
  return RVS_Result_Error;
}

RVS_Result
rvs_engine_wait_for_reply(Arena *arena, RVS_Engine *engine, RVS_ReplyID reply_id, U64 wait_us, RVS_EngineMessage *reply_out)
{
  NotImplemented;
  return RVS_Result_Error;
}

RVS_Result
rvs_engine_launch_async(RVS_Engine *engine, String8 cmdl, String8 wdir, RVS_ReplyID *reply_id_out)
{
  Temp scratch = scratch_begin(0,0);

  String8List           cmd_line = str8_split_by_string_chars(scratch.arena, cmdl, str8_lit(" "), 0);
  U32                   pid      = 0;
  ProcessLaunchParams   params   = { .cmd_line = cmd_line, .path = wdir };
  RVS_DemonMessage      spec     = { .type = RVS_DemonRequest_Launch, .launch = { .params = params, .pid_out = &pid } };
  RVS_ReplyID           reply_id = 0;
  RVS_Result            result   = rvs_demon_send_message(engine->dmn, spec, &reply_id);
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

  RVS_ReplyID reply_id;
  result = rvs_engine_launch_async(engine, cmdl, wdir, &reply_id);
  if (result != RVS_Result_Ok) { goto exit; }

  RVS_EngineMessage reply  = {0};
  result = rvs_engine_wait_for_reply(scratch.arena, engine, reply_id, wait_us, &reply);
  if (result != RVS_Result_Ok) { goto exit; }

  AssertAlways(reply.type == RVS_EngineMessageType_DemonReply);
  AssertAlways(reply.demon_reply.type == RVS_DemonReplyType_LaunchAck);
  if (pid_out) {
    *pid_out = reply.demon_reply.launch_ack.pid;
  }

  exit:;
  scratch_end(scratch);
  return result;
}

internal RVS_ReplyID
rvs_engine_demon_reply_callback(RVS_ReplyID reply_id, RVS_DemonReply *reply, void *ud)
{
  RVS_Engine *engine = ud;

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

  arena_clear(reply_arena);
  RVS_EngineMessage message = {
    .type        = RVS_EngineMessageType_DemonReply,
    .demon_reply = rvs_demon_reply_copy(reply_arena, reply),
  };
  RVS_ReplyID engine_reply_id;
  rvs_engine_send_message(engine, message, &engine_reply_id);

  return engine_reply_id;
}

internal void
rvs_engine_worker(void *user_data)
{
  RVS_Engine *engine = user_data;
  
  for (;;) {
    RVS_EngineMessage *message = (RVS_EngineMessage *)rvs_queue_pop(engine->queue, max_U64);

    switch (message->type) {
    case RVS_EngineMessageType_DemonReply: {
      RVS_DemonReply *demon_reply = &message->demon_reply;
      switch (demon_reply->type) {
      case RVS_DemonReplyType_LaunchAck: {
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


