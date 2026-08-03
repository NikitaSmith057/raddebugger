#include "radvs/rvs_engine.h"
#include "radvs/rvs_async.h"
#include "radvs/rvs_demon.h"
#include "radvs/rvs_request.h"
#include "radvs/rvs_schedule.h"

////////////////////////////////
// Types

typedef struct
{
  RVS_EngineCommandKind kind;
  union {
    struct {
      ProcessLaunchParams params;
    } launch;
    struct {
      U64            programs_count;
      RVS_ProgramID *programs;
    } run;
  };
} RVS_EngineCommand;

typedef enum
{
  RVS_EngineMessageType_Null,
  RVS_EngineMessageType_Command,
  RVS_EngineMessageType_DemonReply,
  RVS_EngineMessageType_Shutdown,
} RVS_EngineMessageType;

typedef struct
{
  RVS_QueueNode          base;
  RVS_EngineMessageType  type;
  RVS_Session           *session;
  union {
    struct {
      RVS_EngineCommand command;
      RVS_MessageID     request_id;
    };
    struct {
      RVS_Demon      *source;
      RVS_DemonReply *reply;
      ArenaNode      *arena_node;
    } demon_reply;
  };
} RVS_EngineMessage;

struct RVS_EngineControl
{
  Arena *arena;
  Mutex  mutex;
  U32    ref_count;
  B32    is_shutdown;
};

struct RVS_Engine
{
  Arena          *arena;
  Mutex           arena_mutex;
  RVS_Queue      *inbox_queue;
  RVS_MessageID   next_request_id;
  RVS_ThreadState state;
  Thread          thread;

  RVS_EngineControl *control;
  RVS_RequestPool   *request_pool;
  RVS_Session       *session;

  RVS_Demon *demon;
  Arena     *demon_reply_arena;
  Mutex      demon_reply_mutex;
  ArenaNode *demon_reply_arena_active_list;
  ArenaNode *demon_reply_arena_free_list;

#if RVS_ENGINE_TESTING
  U32 test_fail_command_enqueue;
  U32 test_hold_before_dispatch;
  U32 test_is_held_before_dispatch;
  U32 test_fail_demon_message_type;
#endif
};

#include "radvs/rvs_session.h"

////////////////////////////////
// Message Transport

internal void
rvs_engine_command_copy(Arena *arena, RVS_EngineCommand *dst, RVS_EngineCommand *src)
{
  ProfBeginFunction();
  *dst = *src;

  switch (src->kind) {
  case RVS_EngineCommandKind_Launch: {
    dst->launch.params = *process_launch_params_copy(arena, &src->launch.params);
  } break;
  case RVS_EngineCommandKind_Run: {
    dst->run.programs = push_array(arena, RVS_ProgramID, src->run.programs_count);
    dst->run.programs_count = src->run.programs_count;
    MemoryCopyTyped(dst->run.programs, src->run.programs, src->run.programs_count);
  } break;
  default: { InvalidPath; } break;
  }
  ProfEnd();
}

internal void
rvs_engine_message_copy(Arena *arena, RVS_EngineMessage *dst, RVS_EngineMessage *src)
{
  ProfBeginFunction();
  RVS_QueueNode base = dst->base;
  *dst = *src;
  dst->base = base;

  switch (src->type) {
  case RVS_EngineMessageType_Command: {
    rvs_engine_command_copy(arena, &dst->command, &src->command);
  } break;
  case RVS_EngineMessageType_DemonReply: {
  } break;
  case RVS_EngineMessageType_Shutdown: {
  } break;
  default: { InvalidPath; } break;
  }
  ProfEnd();
}

internal RVS_Result
rvs_engine_send_message_locked(RVS_Engine *engine, RVS_EngineMessage *spec)
{
  ProfBeginFunction();
#if RVS_ENGINE_TESTING
  if (spec->type == RVS_EngineMessageType_Command &&
      ins_atomic_u32_eval_cond_assign(&engine->test_fail_command_enqueue, 0, 1) == 1) {
    ProfEnd();
    return RVS_Result_Error;
  }
#endif
  RVS_EngineMessage *message = rvs_queue_alloc_struct(engine->inbox_queue, RVS_EngineMessage);
  rvs_engine_message_copy(engine->arena, message, spec);
  RVS_Result result = rvs_queue_push(engine->inbox_queue, &message->base);
  ProfEnd();
  return result;
}

internal RVS_Result
rvs_engine_send_message(RVS_Engine *engine, RVS_EngineMessage *spec)
{
  ProfBeginFunction();
  mutex_take(engine->arena_mutex);
  RVS_Result result = rvs_engine_send_message_locked(engine, spec);
  mutex_drop(engine->arena_mutex);
  ProfEnd();
  return result;
}

////////////////////////////////
internal RVS_EngineControl *
rvs_engine_control_alloc(void)
{
  ProfBeginFunction();
  Arena *arena = arena_alloc(.name = "Engine Control");
  RVS_EngineControl *control = push_array(arena, RVS_EngineControl, 1);
  control->arena = arena;
  control->mutex = mutex_alloc();
  control->ref_count = 1;
  ProfEnd();
  return control;
}

internal void
rvs_engine_control_addref(RVS_EngineControl *control)
{
  ProfBeginFunction();
  ins_atomic_u32_inc_eval(&control->ref_count);
  ProfEnd();
}

internal void
rvs_engine_control_release(RVS_EngineControl *control)
{
  ProfBeginFunction();
  if (ins_atomic_u32_dec_eval(&control->ref_count) == 0) {
    mutex_release(control->mutex);
    arena_release(control->arena);
  }
  ProfEnd();
}

#include "radvs/rvs_request.c"
#include "radvs/rvs_schedule.c"
#include "radvs/rvs_session.c"

internal B32
rvs_engine_request_mark_dispatched(RVS_Engine *engine, RVS_MessageID request_id, RVS_EngineCommand *command)
{
  ProfBeginFunction();
  B32 result = 0;
  mutex_take(engine->control->mutex);
  RVS_Session *session = engine->session;
  if ( ! engine->control->is_shutdown) {
    RVS_ScheduledOperation *operation = rvs_scheduler_operation_mark_dispatched_locked(&session->scheduler, request_id);
    if (operation) {
      if (operation->key.operation_class == RVS_OperationClass_SessionExecution) {
        AssertAlways(command->kind == RVS_EngineCommandKind_Run);
        for EachIndex(program_idx, command->run.programs_count) {
          rvs_session_bump_program_state_epoch_locked(session, command->run.programs[program_idx]);
        }
      }
      result = 1;
    }
  }
  mutex_drop(engine->control->mutex);
  ProfEnd();
  return result;
}

internal void
rvs_engine_retire_undispatched_request(RVS_Engine *engine, RVS_MessageID request_id)
{
  ProfBeginFunction();
  RVS_ScheduledOperation *operation = 0;
  mutex_take(engine->control->mutex);
  RVS_Session *session = engine->session;
  operation = rvs_scheduler_retire_undispatched_operation_locked(&session->scheduler, request_id);
  mutex_drop(engine->control->mutex);

  if (operation) {
    rvs_scheduler_operation_release(operation); // drop active registration ownership
  }
  ProfEnd();
}

internal void
rvs_engine_publish_pending_requests(RVS_Engine *engine, RVS_Result result)
{
  ProfBeginFunction();
  mutex_take(engine->control->mutex);
  for EachNode(operation, RVS_ScheduledOperation, engine->session->scheduler.operation_first) {
    rvs_request_complete(operation->request, (RVS_EngineReply){
      .request_id = operation->request->request_id,
      .result     = result,
      .kind       = operation->request->reply.kind,
    });
  }
  mutex_drop(engine->control->mutex);
  ProfEnd();
}

internal void
rvs_engine_release_active_requests(RVS_Engine *engine, RVS_Result pending_result)
{
  ProfBeginFunction();
  mutex_take(engine->control->mutex);
  RVS_Session *session = engine->session;
  RVS_ScheduledOperation *first = rvs_scheduler_take_active_operations_locked(&session->scheduler);
  mutex_drop(engine->control->mutex);

  for (RVS_ScheduledOperation *n = first, *next = 0; n; n = next) {
    next = n->next;
    n->next = 0;
    n->prev = 0;
    rvs_request_complete(n->request, (RVS_EngineReply){
      .request_id = n->request->request_id,
      .result     = pending_result,
      .kind       = n->request->reply.kind,
    });
    rvs_scheduler_operation_release(n); // drop active registration ownership
  }
  ProfEnd();
}

internal void
rvs_engine_clear_operation_keys(RVS_Engine *engine)
{
  ProfBeginFunction();
  mutex_take(engine->control->mutex);
  RVS_Session *session = engine->session;
  RVS_ScheduledOperation *first = rvs_scheduler_take_operation_keys_locked(&session->scheduler);
  mutex_drop(engine->control->mutex);

  for (RVS_ScheduledOperation *n = first, *next = 0; n; n = next) {
    next = n->key_next;
    n->key_next = 0;
    n->key_prev = 0;
    rvs_scheduler_operation_release(n); // drop keyed registration ownership
  }
  ProfEnd();
}

// Completion

internal void
rvs_engine_complete_reply(RVS_Engine *engine, RVS_EngineReply reply)
{
  ProfBeginFunction();
  RVS_ScheduledOperation *operation = 0;
  mutex_take(engine->control->mutex);
  RVS_Session *session = engine->session;
  operation = rvs_scheduler_find_active_operation_locked(&session->scheduler, reply.request_id);
  if (operation) {
    if (operation->key.operation_class == RVS_OperationClass_SessionExecution && reply.result != RVS_Result_Ok) {
      rvs_scheduler_clear_queued_execution_locked(&session->scheduler, reply.request_id);
    }
    rvs_scheduler_operation_remove_locked(&session->scheduler, operation);
    rvs_session_prepare_reply_locked(session, operation, &reply);
  }
  mutex_drop(engine->control->mutex);

  if (operation) {
    rvs_request_complete(operation->request, reply);
    rvs_scheduler_operation_release(operation); // drop active registration ownership
  }
  ProfEnd();
}

internal B32
rvs_engine_begin_launch(RVS_Engine *engine, RVS_MessageID request_id, U32 pid)
{
  ProfBeginFunction();
  B32 result = 0;
  mutex_take(engine->control->mutex);
  RVS_ScheduledOperation *operation = rvs_scheduler_find_active_operation_locked(&engine->session->scheduler, request_id);
  if (operation) {
    mutex_take(operation->request->mutex);
    if (operation->command_kind == RVS_EngineCommandKind_Launch &&
        operation->request->reply.result == RVS_Result_Pending && operation->launch_pid == 0) {
      operation->launch_pid = pid;
      result = 1;
    }
    mutex_drop(operation->request->mutex);
  }
  mutex_drop(engine->control->mutex);
  ProfEnd();
  return result;
}

internal B32
rvs_engine_complete_launch(RVS_Engine *engine, RVS_MessageID request_id, RVS_Result result, U32 pid, DMN_Handle process)
{
  ProfBeginFunction();
  B32 completed = 0;
  RVS_ScheduledOperation *operation = 0;
  RVS_EngineReply reply = {
    .request_id = request_id,
    .result     = result,
    .kind       = RVS_EngineReplyKind_Launch,
  };

  mutex_take(engine->control->mutex);
  RVS_Session *session = engine->session;
  RVS_ScheduledOperation *candidate = rvs_scheduler_find_active_operation_locked(&session->scheduler, request_id);
  if (candidate) {
    mutex_take(candidate->request->mutex);
    B32 matching_launch = candidate->command_kind == RVS_EngineCommandKind_Launch &&
                           (result != RVS_Result_Ok || candidate->launch_pid == pid);
    if (candidate->request->reply.result == RVS_Result_Pending && matching_launch) {
      if (result == RVS_Result_Ok && pid != 0 && !dmn_handle_match(process, dmn_handle_zero())) {
        RVS_Program *prog = rvs_session_program_add_locked(session, pid, process);
        reply.launch.program_id = prog->id;
        reply.launch.pid        = prog->pid;
      } else if (result == RVS_Result_Ok) {
        reply.result = RVS_Result_Error;
      }
      rvs_session_prepare_reply_locked(session, candidate, &reply);
      rvs_scheduler_operation_remove_locked(&session->scheduler, candidate);
      operation = candidate;
      completed = 1;
    }
    mutex_drop(candidate->request->mutex);
  }
  mutex_drop(engine->control->mutex);

  if (operation) {
    rvs_request_complete(operation->request, reply);
    rvs_scheduler_operation_release(operation); // drop active registration ownership
  }
  ProfEnd();
  return completed;
}

internal B32
rvs_engine_launch_is_pending(RVS_Engine *engine, RVS_MessageID request_id)
{
  ProfBeginFunction();
  B32 result = 0;
  mutex_take(engine->control->mutex);
  RVS_ScheduledOperation *operation = rvs_scheduler_find_active_operation_locked(&engine->session->scheduler, request_id);
  if (operation) {
    mutex_take(operation->request->mutex);
    result = operation->command_kind == RVS_EngineCommandKind_Launch &&
             operation->launch_pid != 0 && operation->request->reply.result == RVS_Result_Pending;
    mutex_drop(operation->request->mutex);
  }
  mutex_drop(engine->control->mutex);
  ProfEnd();
  return result;
}

internal B32
rvs_engine_launch_matches_pid(RVS_Engine *engine, RVS_MessageID request_id, U32 pid)
{
  ProfBeginFunction();
  B32 result = 0;
  mutex_take(engine->control->mutex);
  RVS_ScheduledOperation *operation = rvs_scheduler_find_active_operation_locked(&engine->session->scheduler, request_id);
  if (operation) {
    mutex_take(operation->request->mutex);
    result = operation->command_kind == RVS_EngineCommandKind_Launch &&
             operation->launch_pid == pid && operation->request->reply.result == RVS_Result_Pending;
    mutex_drop(operation->request->mutex);
  }
  mutex_drop(engine->control->mutex);
  ProfEnd();
  return result;
}

internal U32
rvs_engine_launch_pid(RVS_Engine *engine, RVS_MessageID request_id)
{
  ProfBeginFunction();
  U32 result = 0;
  mutex_take(engine->control->mutex);
  RVS_ScheduledOperation *operation = rvs_scheduler_find_active_operation_locked(&engine->session->scheduler, request_id);
  if (operation) {
    mutex_take(operation->request->mutex);
    if (operation->command_kind == RVS_EngineCommandKind_Launch && operation->request->reply.result == RVS_Result_Pending) {
      result = operation->launch_pid;
    }
    mutex_drop(operation->request->mutex);
  }
  mutex_drop(engine->control->mutex);
  ProfEnd();
  return result;
}

internal RVS_Result
rvs_engine_send_demon_message(RVS_Engine *engine, RVS_DemonMessage message)
{
#if RVS_ENGINE_TESTING
  if (ins_atomic_u32_eval_cond_assign(&engine->test_fail_demon_message_type,
                                      RVS_DemonMessage_Null,
                                      (U32)message.type) == (U32)message.type) {
    return RVS_Result_Error;
  }
#endif
  return rvs_demon_send_message(engine->demon, message);
}

internal void
rvs_engine_pump_launch(RVS_Engine *engine, RVS_MessageID request_id)
{
  ProfBeginFunction();
  RVS_Result result = rvs_engine_send_demon_message(engine, (RVS_DemonMessage){
    .type       = RVS_DemonMessage_Pump,
    .request_id = request_id,
  });
  if (result != RVS_Result_Ok) {
    rvs_engine_complete_launch(engine, request_id, RVS_Result_Error, 0, dmn_handle_zero());
  }
  ProfEnd();
}

////////////////////////////////
// Events

////////////////////////////////
// DEMON Output

internal void
rvs_engine_recycle_demon_reply_arena(RVS_Engine *engine, ArenaNode *arena_node)
{
  ProfBeginFunction();
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
  ProfEnd();
}

internal RVS_Result
rvs_engine_push_demon_reply(RVS_Engine *engine, RVS_Demon *source, RVS_DemonReply *reply)
{
  ProfBeginFunction();
  mutex_take(engine->demon_reply_mutex);

  ArenaNode *arena_node = engine->demon_reply_arena_free_list;
  if (arena_node) {
    SLLStackPop(engine->demon_reply_arena_free_list);
  } else {
    arena_node    = push_array(engine->demon_reply_arena, ArenaNode, 1);
    arena_node->v = arena_alloc(.name = "Engine DEMON Reply");
  }
  arena_clear(arena_node->v);

  RVS_DemonReply *reply_copy = push_array(arena_node->v, RVS_DemonReply, 1);
  rvs_demon_reply_copy(arena_node->v, reply_copy, reply);
  SLLStackPush(engine->demon_reply_arena_active_list, arena_node);

  RVS_EngineMessage message = {
    .type = RVS_EngineMessageType_DemonReply,
    .demon_reply = {
      .source     = source,
      .reply      = reply_copy,
      .arena_node = arena_node,
    },
  };
  RVS_Result result = rvs_engine_send_message(engine, &message);

  // on failure, put resources on the free lists
  if (result != RVS_Result_Ok) {
    AssertAlways(engine->demon_reply_arena_active_list == arena_node);
    SLLStackPop(engine->demon_reply_arena_active_list);
    arena_clear(arena_node->v);
    SLLStackPush(engine->demon_reply_arena_free_list, arena_node);
  }

  mutex_drop(engine->demon_reply_mutex);
  ProfEnd();
  return result;
}

internal void
rvs_engine_demon_reply_callback(RVS_Demon *demon, RVS_DemonReply *reply, void *ud)
{
  ProfBeginFunction();
  AssertAlways(rvs_engine_push_demon_reply(ud, demon, reply) == RVS_Result_Ok);
  ProfEnd();
}

////////////////////////////////
// Worker Dispatch

// Command and Output Dispatch

internal void
rvs_engine_process_command(RVS_Engine *engine, RVS_Session *session, RVS_MessageID request_id, RVS_EngineCommand *command)
{
  ProfBeginFunction();
#if RVS_ENGINE_TESTING
  if (ins_atomic_u32_eval(&engine->test_hold_before_dispatch)) {
    ins_atomic_u32_eval_assign(&engine->test_is_held_before_dispatch, 1);
    while (ins_atomic_u32_eval(&engine->test_hold_before_dispatch)) {
      sleep_ms(1);
    }
    ins_atomic_u32_eval_assign(&engine->test_is_held_before_dispatch, 0);
  }
#endif
  if ( ! rvs_engine_request_mark_dispatched(engine, request_id, command)) {
    rvs_engine_retire_undispatched_request(engine, request_id);
    ProfEnd();
    return;
  }

  switch (command->kind) {
  case RVS_EngineCommandKind_Launch: {
    RVS_DemonMessage spec = {
      .type       = RVS_DemonMessage_Launch,
      .request_id = request_id,
      .launch     = { .params = command->launch.params },
    };

    RVS_Result result = rvs_engine_send_demon_message(engine, spec);

    // failed to send a message to the DEMON thread -- reply with the error code
    if (result != RVS_Result_Ok) {
      rvs_engine_complete_reply(engine, (RVS_EngineReply){
        .request_id = request_id,
        .result     = result,
        .kind       = RVS_EngineReplyKind_Launch,
      });
    }
  } break;
  case RVS_EngineCommandKind_Run: {
    Temp scratch = scratch_begin(0, 0);
    DMN_Handle *processes = push_array(scratch.arena, DMN_Handle, command->run.programs_count);
    B32 all_programs_found = rvs_session_programs_to_processes(session, command->run.programs, command->run.programs_count, processes);

    RVS_Result result = RVS_Result_Error;
    if (all_programs_found) {
      result = rvs_engine_send_demon_message(engine, (RVS_DemonMessage){
        .type       = RVS_DemonMessage_Run,
        .request_id = request_id,
        .run = {
          .processes = processes,
          .processes_count = command->run.programs_count,
        },
      });
    }
    if (result != RVS_Result_Ok) {
      rvs_engine_complete_reply(engine, (RVS_EngineReply){
        .request_id = request_id,
        .result     = result,
        .kind       = RVS_EngineReplyKind_Run,
      });
    }
    scratch_end(scratch);
  } break;
  default: { InvalidPath; } break;
  }
  ProfEnd();
}

internal void
rvs_engine_process_demon_reply(RVS_Engine *engine, RVS_DemonReply *reply)
{
  ProfBeginFunction();
  switch (reply->kind) {
  case RVS_DemonReplyKind_LaunchStarted: {
    if (rvs_engine_begin_launch(engine, reply->request_id, reply->launch_started.pid)) {
      rvs_engine_pump_launch(engine, reply->request_id);
    } else {
      rvs_engine_complete_launch(engine, reply->request_id, RVS_Result_Error, 0, dmn_handle_zero());
    }
  } break;

  case RVS_DemonReplyKind_Launch: {
    // A successful launch is completed only from a PID-matched CreateProcess event.
    RVS_Result result = reply->result == RVS_Result_Ok ? RVS_Result_Error : reply->result;
    rvs_engine_complete_launch(engine, reply->request_id, result, 0, dmn_handle_zero());
  } break;

  case RVS_DemonReplyKind_Run: {
    mutex_take(engine->control->mutex);
    if (reply->result == RVS_Result_Ok) {
      rvs_scheduler_mark_run_in_flight_locked(&engine->session->scheduler, reply->request_id);
    } else {
      rvs_scheduler_clear_queued_execution_locked(&engine->session->scheduler, reply->request_id);
    }
    mutex_drop(engine->control->mutex);
    rvs_engine_complete_reply(engine, (RVS_EngineReply){
      .request_id = reply->request_id,
      .result     = reply->result,
      .kind       = RVS_EngineReplyKind_Run,
    });
  } break;

  case RVS_DemonReplyKind_EventBatch: {
    DMN_Event *launch_event = 0;
    B32 launch_error = 0;
    B32 launch_exited = 0;
    U32 launch_pid = rvs_engine_launch_pid(engine, reply->request_id);
    for EachNode(n, DMN_EventNode, reply->event_batch.events.first) {
      if (n->v.kind == DMN_EventKind_Error) {
        launch_error |= n->v.error_kind == DMN_ErrorKind_NotAttached ||
                        (launch_pid != 0 && n->v.system_process_id == launch_pid);
      } else if (n->v.kind == DMN_EventKind_CreateProcess &&
                 rvs_engine_launch_matches_pid(engine, reply->request_id, n->v.system_process_id)) {
        launch_event = &n->v;
      } else if (n->v.kind == DMN_EventKind_ExitProcess && launch_event &&
                 dmn_handle_match(n->v.process, launch_event->process)) {
        // DEMON event batches preserve platform order, so this exit belongs to the
        // target only after its matching CreateProcess event has established a handle.
        launch_exited = 1;
      }
    }
    if (launch_event && !launch_exited) {
      rvs_engine_complete_launch(engine, reply->request_id, RVS_Result_Ok, launch_event->system_process_id, launch_event->process);
    } else if (launch_error || launch_exited) {
      rvs_engine_complete_launch(engine, reply->request_id, RVS_Result_Error, 0, dmn_handle_zero());
    }
    B32 launch_failed = launch_exited || (launch_event == 0 && launch_error);
    for EachNode(n, DMN_EventNode, reply->event_batch.events.first) {
      B32 suppress_event = launch_failed &&
                           ((launch_event && dmn_handle_match(n->v.process, launch_event->process)) ||
                            (launch_pid != 0 && n->v.system_process_id == launch_pid));
      if (suppress_event) { continue; }
      RVS_Result push_result = rvs_session_push_event(engine->session, &n->v);
      AssertAlways(push_result == RVS_Result_Ok || push_result == RVS_Result_EngineStopped);
    }
    if (rvs_engine_launch_is_pending(engine, reply->request_id)) {
      rvs_engine_pump_launch(engine, reply->request_id);
    }
  } break;

  case RVS_DemonReplyKind_RunFinished: {
    mutex_take(engine->control->mutex);
    rvs_scheduler_finish_run_locked(&engine->session->scheduler, reply->request_id);
    mutex_drop(engine->control->mutex);
  } break;

  default: { InvalidPath; } break;
  }
  ProfEnd();
}

internal void
rvs_engine_worker(void *user_data)
{
  ProfBeginFunction();
  RVS_Engine *engine = user_data;

  for (;;) {
    RVS_EngineMessage *message = rvs_queue_pop_struct(engine->inbox_queue, RVS_EngineMessage, max_U64);
    if (message == 0) { continue; }

    B32 should_exit = 0;
    switch (message->type) {
    case RVS_EngineMessageType_Command: {
      rvs_engine_process_command(engine, message->session, message->request_id, &message->command);
    } break;

    case RVS_EngineMessageType_DemonReply: {
      rvs_engine_process_demon_reply(engine, message->demon_reply.reply);
      rvs_engine_recycle_demon_reply_arena(engine, message->demon_reply.arena_node);
    } break;

    case RVS_EngineMessageType_Shutdown: {
      should_exit = 1;
    } break;

    default: { InvalidPath; } break;
    }

    rvs_queue_recycle(engine->inbox_queue, &message->base);

    if (should_exit) {
      break;
    }
  }

  ins_atomic_u32_eval_assign(&engine->state, RVS_ThreadState_Exited);
  ProfEnd();
}

////////////////////////////////
// API

RVS_Result
rvs_engine_init(RVS_Engine **engine_out)
{
  ProfBeginFunction();
  RVS_Result result = RVS_Result_Error;
  static RVS_Engine engine = {0};
  if (engine_out == 0) {
    goto exit;
  }
  if (ins_atomic_u32_eval_cond_assign(&engine.state, RVS_ThreadState_Initing, RVS_ThreadState_Null) != RVS_ThreadState_Null) {
    goto exit;
  }

  engine.arena              = arena_alloc(.name = "Engine");
  engine.inbox_queue        = rvs_queue_alloc(engine.arena, sizeof(RVS_EngineMessage), AlignOf(RVS_EngineMessage));
  engine.arena_mutex        = mutex_alloc();
  engine.control            = rvs_engine_control_alloc();
  engine.request_pool       = rvs_request_pool_alloc();
  engine.demon_reply_arena = arena_alloc(.name = "Engine DEMON Reply Nodes");
  engine.demon_reply_mutex = mutex_alloc();

  result = rvs_demon_init(&engine, rvs_engine_demon_reply_callback, &engine.demon);
  if (result != RVS_Result_Ok) {
    ins_atomic_u32_eval_assign(&engine.state, RVS_ThreadState_Exited);
    goto exit;
  }

  engine.thread = thread_launch(rvs_engine_worker, &engine);
  if (MemoryIsZeroStruct(&engine.thread)) {
    ins_atomic_u32_eval_assign(&engine.state, RVS_ThreadState_Exited);
    result = RVS_Result_Error;
    goto exit;
  }

  ins_atomic_u32_eval_assign(&engine.state, RVS_ThreadState_Live);
  *engine_out = &engine;
  result = RVS_Result_Ok;

  exit:;
  ProfEnd();
  return result;
}

RVS_Result
rvs_engine_create_session(RVS_Engine *engine, RVS_Session **session_out)
{
  RVS_Result result = RVS_Result_Error;
  if (session_out) { *session_out = 0; }
  if (engine == 0 || session_out == 0) {
    return result;
  }
  mutex_take(engine->control->mutex);
  if (engine->control->is_shutdown || ins_atomic_u32_eval(&engine->state) != RVS_ThreadState_Live) {
    result = RVS_Result_EngineStopped;
  } else if (engine->session) {
    result = RVS_Result_Unsupported;
  } else {
    engine->session = rvs_session_alloc(engine);
    *session_out = engine->session;
    result = RVS_Result_Ok;
  }
  mutex_drop(engine->control->mutex);
  return result;
}

void
rvs_engine_shutdown(RVS_Engine *engine)
{
  ProfBeginFunction();
  if (ins_atomic_u32_eval_cond_assign(&engine->state, RVS_ThreadState_Terminating, RVS_ThreadState_Live) != RVS_ThreadState_Live) {
    goto exit;
  }

  mutex_take(engine->control->mutex);
  engine->control->is_shutdown = 1;
#if RVS_ENGINE_TESTING
  ins_atomic_u32_eval_assign(&engine->test_hold_before_dispatch, 0);
#endif
  if (engine->session) {
    rvs_session_close_events_locked(engine->session);
  }
  mutex_drop(engine->control->mutex);

  // Revoke controls before completing only requests that have not reached a terminal reply.
  if (engine->session) {
    rvs_engine_publish_pending_requests(engine, RVS_Result_EngineStopped);
  }

  // shutdown the DEMON thread
  AssertAlways(rvs_demon_shutdown(engine->demon) == RVS_Result_Ok);

  // shutdown the engine thread
  RVS_EngineMessage shutdown = { .type = RVS_EngineMessageType_Shutdown };
  AssertAlways(rvs_engine_send_message(engine, &shutdown) == RVS_Result_Ok);
  thread_join(engine->thread, max_U64);

  // Release engine ownership; retained requests keep their immutable terminal replies.
  if (engine->session) {
    rvs_engine_release_active_requests(engine, RVS_Result_EngineStopped);
    rvs_engine_clear_operation_keys(engine);
  }
  rvs_request_pool_release_engine(engine->request_pool);

  // release arenas for DEMON outputs after both workers have drained them
  mutex_take(engine->demon_reply_mutex);
  AssertAlways(engine->demon_reply_arena_active_list == 0);
  for EachNode(n, ArenaNode, engine->demon_reply_arena_free_list) { arena_release(n->v); }
  engine->demon_reply_arena_free_list = 0;
  mutex_drop(engine->demon_reply_mutex);

  // release engine thread resources
  rvs_queue_release(engine->inbox_queue);
  if (engine->session) {
    mutex_take(engine->control->mutex);
    rvs_session_release_engine(engine->session);
    engine->session = 0;
    mutex_drop(engine->control->mutex);
  }
  rvs_engine_control_release(engine->control);
  mutex_release(engine->arena_mutex);
  mutex_release(engine->demon_reply_mutex);
  arena_release(engine->demon_reply_arena);
  arena_release(engine->arena);
  MemoryZeroStruct(engine);

  exit:;
  ProfEnd();
}

////////////////////////////////
// Enum

internal String8
rvs_string_from_command_kind(RVS_EngineCommandKind v)
{
  switch (v) {
#define X(id, ...) case RVS_EngineCommandKind_##id: return str8_lit(Stringify(id));
  RVS_ENGINE_COMMAND_XLIST
#undef X
  default: break;
  }
  return str8_zero();
}

internal String8
rvs_help_from_command_kind(RVS_EngineCommandKind v)
{
  switch (v) {
  case RVS_EngineCommandKind_Null: break;
#define X(id, op_class, policy, help, ...) case RVS_EngineCommandKind_##id: return str8_lit(help);
  RVS_ENGINE_COMMAND_XLIST
#undef X
  }
  return str8_zero();
}

internal RVS_EngineCommandKind
rvs_command_kind_from_string(String8 v)
{
#define X(id, ...) if (str8_matchi(str8_lit(Stringify(id)), v)) return RVS_EngineCommandKind_##id;
  RVS_ENGINE_COMMAND_XLIST
#undef X
  return RVS_EngineCommandKind_Null;
}

internal RVS_OperationClass
rvs_op_class_from_engine_command_kind(RVS_EngineCommandKind v)
{
  switch (v) {
#define X(id, op_class, ...) case RVS_EngineCommandKind_##id: return op_class;
  RVS_ENGINE_COMMAND_XLIST
#undef X
  default: break;
  }
  return RVS_OperationClass_Null;
}

internal RVS_RequestPolicy
rvs_request_policy_from_engine_command_kind(RVS_EngineCommandKind v)
{
  switch (v) {
#define X(id, op_class, policy, ...) case RVS_EngineCommandKind_##id: return policy;
    RVS_ENGINE_COMMAND_XLIST
#undef X
  default: break;
  }
  return RVS_RequestPolicy_Null;
}
