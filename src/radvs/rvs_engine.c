#include "radvs/rvs_engine.h"

typedef struct
{
  RVS_QueueNode base;
  RVS_Event     event;
} RVS_EngineEventMessage;

struct RVS_Engine
{
  Arena          *arena;
  RVS_Queue      *queue;
  RVS_Queue      *event_queue;
  Mutex           message_mutex;
  RVS_ThreadState state;
  Thread          thread;
  RVS_MessageID   next_request_id;

  Arena         *program_arena;
  RVS_ProgramID  next_program_id;
  RVS_Program   *first_program;
  RVS_Program   *last_program;

  CondVar          reply_cv;
  Mutex            reply_mutex;
  RVS_EngineReply *reply_first;
  RVS_EngineReply *reply_last;
  RVS_EngineReply *reply_free_list;

  // DEMON
  RVS_Demon *demon;
  Arena     *demon_output_arena;
  Mutex      demon_output_mutex;
  ArenaNode *demon_output_arena_active_list;
  ArenaNode *demon_output_arena_free_list;
};

internal void
rvs_engine_command_copy(Arena *arena, RVS_EngineCommand *dst, RVS_EngineCommand *src)
{
  *dst = *src;

  switch (src->kind) {
  case RVS_EngineCommandKind_Launch: {
    dst->launch.params = *process_launch_params_copy(arena, &src->launch.params);
  } break;
  default: { InvalidPath; } break;
  }
}

internal void
rvs_engine_recycle_demon_output_arena(RVS_Engine *engine, ArenaNode *arena_node)
{
  mutex_take(engine->demon_output_mutex);

  // TODO: replace with a hash map
  ArenaNode **node_ptr = &engine->demon_output_arena_active_list;
  while (*node_ptr && *node_ptr != arena_node) {
    node_ptr = &(*node_ptr)->next;
  }
  AssertAlways(*node_ptr == arena_node);
  *node_ptr = arena_node->next;

  arena_clear(arena_node->v);
  SLLStackPush(engine->demon_output_arena_free_list, arena_node);

  mutex_drop(engine->demon_output_mutex);
}

internal RVS_Result
rvs_engine_push_demon_output(RVS_Engine *engine, RVS_Demon *source, RVS_DemonOutput *output)
{
  mutex_take(engine->demon_output_mutex);

  ArenaNode *arena_node = engine->demon_output_arena_free_list;
  if (arena_node) {
    SLLStackPop(engine->demon_output_arena_free_list);
  } else {
    arena_node    = push_array(engine->demon_output_arena, ArenaNode, 1);
    arena_node->v = arena_alloc(.name = "Engine DEMON Output");
  }
  arena_clear(arena_node->v);

  RVS_DemonOutput *output_copy = push_array(arena_node->v, RVS_DemonOutput, 1);
  rvs_demon_output_copy(arena_node->v, output_copy, output);
  SLLStackPush(engine->demon_output_arena_active_list, arena_node);

  // Queue allocation also uses engine->arena, so serialize it with API requests.
  mutex_take(engine->message_mutex);
  RVS_EngineMessage *message = rvs_queue_alloc_struct(engine->queue, RVS_EngineMessage);
  message->type                    = RVS_EngineMessageType_DemonOutput;
  message->demon_output.source     = source;
  message->demon_output.output     = output_copy;
  message->demon_output.arena_node = arena_node;
  RVS_Result result = rvs_queue_push(engine->queue, &message->base);
  mutex_drop(engine->message_mutex);

  // on failure, put resources on the free lists
  if (result != RVS_Result_Ok) {
    AssertAlways(engine->demon_output_arena_active_list == arena_node);
    SLLStackPop(engine->demon_output_arena_active_list);
    arena_clear(arena_node->v);
    SLLStackPush(engine->demon_output_arena_free_list, arena_node);
  }

  mutex_drop(engine->demon_output_mutex);
  return result;
}

internal RVS_Result
rvs_engine_push_event(RVS_Engine *engine, RVS_Event *event)
{
  mutex_take(engine->message_mutex);
  RVS_EngineEventMessage *message = rvs_queue_alloc_struct(engine->event_queue, RVS_EngineEventMessage);
  rvs_demon_event_copy(engine->arena, &message->event, event);
  RVS_Result result = rvs_queue_push(engine->event_queue, &message->base);
  mutex_drop(engine->message_mutex);
  return result;
}

internal RVS_Result
rvs_engine_push_shutdown(RVS_Engine *engine)
{
  mutex_take(engine->message_mutex);
  RVS_EngineMessage *message = rvs_queue_alloc_struct(engine->queue, RVS_EngineMessage);
  message->type = RVS_EngineMessageType_Shutdown;
  RVS_Result result = rvs_queue_push(engine->queue, &message->base);
  mutex_drop(engine->message_mutex);
  return result;
}

internal RVS_EngineReply *
rvs_engine_reply_alloc(RVS_Engine *engine)
{
  RVS_EngineReply *reply = engine->reply_free_list;
  if (reply) {
    SLLStackPop(engine->reply_free_list);
    MemoryZeroStruct(reply);
  } else {
    reply = push_array(engine->arena, RVS_EngineReply, 1);
  }
  reply->reply.reply_id = ins_atomic_u64_inc_eval(&engine->next_request_id);
  reply->reply.result   = RVS_Result_Null;
  DLLPushBack(engine->reply_first, engine->reply_last, reply);
  return reply;
}

internal RVS_EngineReply *
rvs_engine_reply_alloc_locked(RVS_Engine *engine)
{
  mutex_take(engine->reply_mutex);
  RVS_EngineReply *reply = rvs_engine_reply_alloc(engine);
  mutex_drop(engine->reply_mutex);
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
    RVS_EngineMessage *message = rvs_queue_pop_struct(engine->queue, RVS_EngineMessage, max_U64);
    if (message == 0) { continue; }

    B32 should_exit = 0;
    switch (message->type) {
    case RVS_EngineMessageType_Shutdown: {
      should_exit = 1;
    } break;

    case RVS_EngineMessageType_Command: {
      switch (message->command.kind) {
      case RVS_EngineCommandKind_Launch: {
        RVS_DemonMessage spec = {
          .type       = RVS_DemonMessage_Launch,
          .request_id = message->command.request_id,
          .launch     = { .params = message->command.launch.params },
        };

        RVS_Result result = rvs_demon_send_message(engine->demon, spec);

        // failed to send a message to the DEMON thread -- relpy ack with the error code
        if (result != RVS_Result_Ok) {
          rvs_engine_complete_reply(engine, (RVS_Reply){
            .reply_id = message->command.request_id,
            .result   = result,
            .kind     = RVS_ReplyKind_LaunchAck,
          });
        }
      } break;
      default: { InvalidPath; } break;
      }
    } break;

    case RVS_EngineMessageType_DemonOutput: {
      RVS_DemonOutput *output = message->demon_output.output;
      switch (output->kind) {
      case RVS_DemonOutputKind_Reply: {
        switch (output->reply.reply.kind) {
        case RVS_DemonReplyKind_Launch: {
          RVS_Reply reply = {
            .reply_id = output->request_id,
            .result   = output->reply.result,
            .kind     = RVS_ReplyKind_LaunchAck,
          };

          if (reply.result == RVS_Result_Ok) {
            RVS_Program *prog = push_array(engine->program_arena, RVS_Program, 1);
            prog->arena = arena_alloc(.name = "Engine Program");
            prog->id    = ++engine->next_program_id;
            prog->pid   = output->reply.reply.launch.pid;
            SLLQueuePush(engine->first_program, engine->last_program, prog);

            reply.launch_ack.program_id = prog->id;
            reply.launch_ack.pid        = prog->pid;
          }

          rvs_engine_complete_reply(engine, reply);
        } break;
        default: { InvalidPath; } break;
        }
      } break;

      case RVS_DemonOutputKind_EventBatch: {
        for EachNode(n, DMN_EventNode, output->event_batch.events.first) {
          AssertAlways(rvs_engine_push_event(engine, &n->v) == RVS_Result_Ok);
        }
      } break;

      default: { InvalidPath; } break;
      }

      rvs_engine_recycle_demon_output_arena(engine, message->demon_output.arena_node);
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

// called on the DEMON thread for both command replies and debugger events
internal void
rvs_engine_demon_output_callback(RVS_Demon *demon, RVS_DemonOutput *output, void *ud)
{
  AssertAlways(rvs_engine_push_demon_output(ud, demon, output) == RVS_Result_Ok);
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

  engine.arena             = arena_alloc(.name = "Engine");
  engine.queue             = rvs_queue_alloc(engine.arena, sizeof(RVS_EngineMessage), AlignOf(RVS_EngineMessage));
  engine.event_queue       = rvs_queue_alloc(engine.arena, sizeof(RVS_EngineEventMessage), AlignOf(RVS_EngineEventMessage));
  engine.message_mutex     = mutex_alloc();
  engine.program_arena     = arena_alloc();
  engine.reply_cv          = cond_var_alloc();
  engine.reply_mutex       = mutex_alloc();
  engine.demon_output_arena = arena_alloc(.name = "Engine DEMON Output Nodes");
  engine.demon_output_mutex = mutex_alloc();

  RVS_Result result = rvs_demon_init(&engine, rvs_engine_demon_output_callback, &engine.demon);
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
  AssertAlways(rvs_engine_push_shutdown(engine) == RVS_Result_Ok);
  thread_join(engine->thread, max_U64);

  // release arenas for DEMON outputs after both workers have drained them
  mutex_take(engine->demon_output_mutex);
  AssertAlways(engine->demon_output_arena_active_list == 0);
  for EachNode(n, ArenaNode, engine->demon_output_arena_free_list) { arena_release(n->v); }
  engine->demon_output_arena_free_list = 0;
  mutex_drop(engine->demon_output_mutex);

  // release engine programs
  for EachNode(prog, RVS_Program, engine->first_program) { arena_release(prog->arena); } // release programs

  // release engine thread resources
  rvs_queue_release(engine->queue);
  rvs_queue_release(engine->event_queue);
  cond_var_release(engine->reply_cv);
  mutex_release(engine->reply_mutex);
  mutex_release(engine->message_mutex);
  mutex_release(engine->demon_output_mutex);
  arena_release(engine->program_arena);
  arena_release(engine->demon_output_arena);
  arena_release(engine->arena);
  MemoryZeroStruct(engine);
}

internal RVS_Result
rvs_engine_send_command(RVS_Engine *engine, RVS_EngineCommand spec, RVS_MessageID *reply_id_out)
{
  RVS_Result result = RVS_Result_Error;
  mutex_take(engine->message_mutex);

  if (ins_atomic_u32_eval(&engine->state) == RVS_ThreadState_Running) {
    RVS_EngineReply *reply = rvs_engine_reply_alloc_locked(engine);
    RVS_MessageID reply_id = reply->reply.reply_id;

    RVS_EngineMessage *message = rvs_queue_alloc_struct(engine->queue, RVS_EngineMessage);
    message->type = RVS_EngineMessageType_Command;
    rvs_engine_command_copy(engine->arena, &message->command, &spec);
    message->command.request_id = reply_id;

    result = rvs_queue_push(engine->queue, &message->base);
    
    if (result == RVS_Result_Ok) {
      if (reply_id_out) {
        *reply_id_out = reply_id;
      }
    } else {
      mutex_take(engine->reply_mutex);
      DLLRemove(engine->reply_first, engine->reply_last, reply);
      SLLStackPush(engine->reply_free_list, reply);
      mutex_drop(engine->reply_mutex);
    }
  }

  mutex_drop(engine->message_mutex);
  return result;
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
  RVS_EngineCommand command = {
    .kind   = RVS_EngineCommandKind_Launch,
    .launch = { .params = { .cmd_line = cmd_line, .path = wdir } },
  };
  RVS_Result result = rvs_engine_send_command(engine, command, reply_id_out);
  scratch_end(scratch);
  return result;
}

RVS_Result
rvs_engine_launch(RVS_Engine *engine, String8 cmdl, String8 wdir, U64 wait_us, U32 *pid_out)
{
  Temp scratch = scratch_begin(0, 0);
  RVS_MessageID reply_id = 0;
  RVS_Result    result   = rvs_engine_launch_async(engine, cmdl, wdir, &reply_id);
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
rvs_engine_wait_for_event(Arena *arena, RVS_Engine *engine, U64 wait_us, RVS_Event *event_out)
{
  RVS_EngineEventMessage *message = rvs_queue_pop_struct(engine->event_queue, RVS_EngineEventMessage, wait_us);
  if (message == 0) { return RVS_Result_Timeout; }

  rvs_demon_event_copy(arena, event_out, &message->event);
  rvs_queue_recycle(engine->event_queue, &message->base);

  return RVS_Result_Ok;
}
