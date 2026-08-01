#include "radvs/rvs_engine.h"

struct RVS_Engine
{
  Arena          *arena;
  RVS_Queue      *queue;
  Mutex           message_mutex;
  RVS_ThreadState state;

  Arena         *program_arena;
  RVS_ProgramID  next_program_id;
  RVS_Program   *first_program;
  RVS_Program   *last_program;

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
  case RVS_EngineMessageType_DemonReply: { InvalidPath; } break;
  case RVS_EngineMessageType_Shutdown: {} break;
  default: { InvalidPath; } break;
  }
}

internal void
rvs_engine_recycle_demon_reply_arena(RVS_Engine *engine, ArenaNode *arena_node)
{
  mutex_take(engine->demon_reply_mutex);

  // TODO: replace with a hash map
  ArenaNode **node_ptr = &engine->demon_reply_arena_active_list;
  while (*node_ptr && *node_ptr != arena_node) {
    node_ptr = &(*node_ptr)->next;
  }
  AssertAlways(*node_ptr == arena_node);
  *node_ptr = arena_node->next;

  arena_clear(arena_node->v);
  SLLStackPush(engine->demon_reply_arena_free_list, arena_node);

  mutex_drop(engine->demon_reply_mutex);
}

internal RVS_Result
rvs_engine_push_demon_reply(RVS_Engine *engine, RVS_MessageID reply_id, RVS_DemonMessage *reply)
{
  mutex_take(engine->demon_reply_mutex);

  // get arena for the DEMON reply
  ArenaNode *arena_node = engine->demon_reply_arena_free_list;
  if (arena_node) {
    SLLStackPop(engine->demon_reply_arena_free_list);
  } else {
    arena_node    = push_array(engine->demon_reply_arena, ArenaNode, 1);
    arena_node->v = arena_alloc(.name = "Engine DEMON Reply");
  }
  arena_clear(arena_node->v);

  // copy DEMON reply to the arena
  RVS_DemonMessage *reply_copy = push_array(arena_node->v, RVS_DemonMessage, 1);
  rvs_demon_message_copy(arena_node->v, reply_copy, reply);
  SLLStackPush(engine->demon_reply_arena_active_list, arena_node);

  // send the reply to the DEMON thread
  mutex_take(engine->message_mutex);
  RVS_EngineMessage *message = (RVS_EngineMessage *)rvs_queue_alloc_message(engine->queue);
  message->type                   = RVS_EngineMessageType_DemonReply;
  message->demon_reply.message    = reply_copy;
  message->demon_reply.arena_node = arena_node; // DEMON thread recycles the arenas
  message->demon_reply.reply_id   = reply_id;
  RVS_Result result = rvs_queue_push(engine->queue, &message->base);
  mutex_drop(engine->message_mutex);

  // on failure, put resources on the free lists
  if (result != RVS_Result_Ok) {
    AssertAlways(engine->demon_reply_arena_active_list == arena_node);
    SLLStackPop(engine->demon_reply_arena_active_list);
    arena_clear(arena_node->v);
    SLLStackPush(engine->demon_reply_arena_free_list, arena_node);
  }

  mutex_drop(engine->demon_reply_mutex);
  return result;
}

internal RVS_Result
rvs_engine_push_message(RVS_Engine *engine, RVS_EngineMessage *spec, RVS_MessageID *reply_id_out)
{
  RVS_Result result = RVS_Result_Error;
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

    B32 should_exit = 0;
    switch (message->type) {
    case RVS_EngineMessageType_Shutdown: {
      should_exit = 1;
    } break;

    case RVS_EngineMessageType_Launch: {
      RVS_DemonMessage spec = {
        .type     = RVS_DemonMessage_Launch,
        .reply_to = message->base.reply_id,
        .launch   = { .params = message->launch.params },
      };
      RVS_Result result = rvs_demon_send_message(engine->demon, spec, 0);
      if (result != RVS_Result_Ok) {
        rvs_engine_complete_reply(engine, (RVS_Reply){
          .reply_id = message->base.reply_id,
          .result   = result,
          .kind     = RVS_ReplyKind_LaunchAck,
        });
      }
    } break;

    case RVS_EngineMessageType_DemonReply: {
      RVS_DemonMessage *demon_reply = message->demon_reply.message;
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

      rvs_engine_recycle_demon_reply_arena(engine, message->demon_reply.arena_node);
    } break;

    default: { InvalidPath; } break;
    }

    rvs_queue_recycle(engine->queue, &message->base);

    if (should_exit) {
      break;
    }
  }

  ins_atomic_u32_eval_assign(&engine->state, RVS_ThreadState_Exited);
}

// called on the DEMON thread on completed request
internal void
rvs_engine_demon_reply_callback(RVS_MessageID reply_id, RVS_DemonMessage *reply, void *ud)
{
  AssertAlways(rvs_engine_push_demon_reply(ud, reply_id, reply) == RVS_Result_Ok);
}

RVS_Result
rvs_engine_init(RVS_Engine **engine_out)
{
  if (engine_out == 0) {
    return RVS_Result_Error;
  }

  static RVS_Engine engine = {0};
  if (ins_atomic_u32_eval_cond_assign(&engine.state, RVS_ThreadState_Initing, RVS_ThreadState_Null) != RVS_ThreadState_Null) {
    return RVS_Result_Error;
  }
  engine.arena         = arena_alloc();
  engine.queue         = rvs_queue_alloc(engine.arena, sizeof(RVS_EngineMessage), AlignOf(RVS_EngineMessage));
  engine.message_mutex = mutex_alloc();
  engine.program_arena = arena_alloc();
  engine.reply_cv      = cond_var_alloc();
  engine.reply_mutex   = mutex_alloc();
  engine.demon_reply_arena = arena_alloc(.name = "Engine DEMON Reply Nodes");
  engine.demon_reply_mutex = mutex_alloc();

  RVS_Result result = rvs_demon_init(&engine, rvs_engine_demon_reply_callback, &engine.demon);
  if (result != RVS_Result_Ok) {
    ins_atomic_u32_eval_assign(&engine.state, RVS_ThreadState_Exited);
    return result;
  }

  engine.thread = thread_launch(rvs_engine_worker, &engine);
  if (MemoryIsZeroStruct(&engine.thread)) {
    ins_atomic_u32_eval_assign(&engine.state, RVS_ThreadState_Exited);
    return RVS_Result_Error;
  }

  ins_atomic_u32_eval_assign(&engine.state, RVS_ThreadState_Running);
  *engine_out = &engine;
  return RVS_Result_Ok;
}

void
rvs_engine_shutdown(RVS_Engine *engine)
{
  if (ins_atomic_u32_eval_cond_assign(&engine->state, RVS_ThreadState_Terminating, RVS_ThreadState_Running) != RVS_ThreadState_Running) {
    return;
  }

  // shutdown the DEMON thread
  AssertAlways(rvs_demon_shutdown(engine->demon) == RVS_Result_Ok);

  // shutdown the engine thread
  RVS_EngineMessage shutdown = { .type = RVS_EngineMessageType_Shutdown };
  rvs_engine_push_message(engine, &shutdown, 0);
  thread_join(engine->thread, max_U64);

  // release arenas for DEMON replies
  mutex_take(engine->demon_reply_mutex);
  AssertAlways(engine->demon_reply_arena_active_list == 0); // there must be no DEMON replies in-flight
  for EachNode(n, ArenaNode, engine->demon_reply_arena_free_list) { arena_release(n->v); }
  engine->demon_reply_arena_free_list = 0;
  mutex_drop(engine->demon_reply_mutex);

  // release engine programs
  for EachNode(prog, RVS_Program, engine->first_program) { arena_release(prog->arena); } // release programs

  // release engine thread resources
  rvs_queue_release(engine->queue);
  cond_var_release(engine->reply_cv);
  mutex_release(engine->reply_mutex);
  mutex_release(engine->message_mutex);
  mutex_release(engine->demon_reply_mutex);
  arena_release(engine->program_arena);
  arena_release(engine->demon_reply_arena);
  arena_release(engine->arena);
  MemoryZeroStruct(engine);
}

RVS_Result
rvs_engine_send_message(RVS_Engine *engine, RVS_EngineMessage message, RVS_MessageID *reply_id_out)
{
  // Register the reply before making the command visible to the worker.
  mutex_take(engine->message_mutex);
  if (ins_atomic_u32_eval(&engine->state) != RVS_ThreadState_Running) {
    mutex_drop(engine->message_mutex);
    return RVS_Result_Error;
  }
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
