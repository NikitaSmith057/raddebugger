// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////

#include "radvs/rvs_engine.h"
#include "radvs/rvs_scheduler.h"
#include "radvs/rvs_scheduler.c"

////////////////////////////////

struct RVS_Session
{
  Arena           *arena;
  U32              ref_count;
  RVS_Queue       *event_queue;
  RVS_EntityStore  entities;
  RVS_Scheduler   *scheduler;
};

struct RVS_Engine
{
  Arena             *arena;
  RVS_Queue         *inbox_queue;
  RVS_WorkerState    state;
  Thread             thread;
  RVS_EngineControl *control;
  RVS_RequestPool   *request_pool;
  RVS_Session       *session;
  RVS_Demon         *demon;
};

struct  RVS_EngineControl
{
  Arena *arena;
  Mutex  mutex;
  U32    ref_count;
  B32    is_shutdown;
};

struct RVS_RequestControl
{
  Arena                    *arena;
  RVS_SessionControlParams  params;
  RVS_MessageID             request_id;
};

typedef enum
{
  RVS_EngineMessageKind_Null,
  RVS_EngineMessageKind_DispatchCommand,
  RVS_EngineMessageKind_DemonReply,
  RVS_EngineMessageKind_SchedulerEvent,
  RVS_EngineMessageKind_Shutdown,
} RVS_EngineMessageKind;

typedef struct
{
  RVS_QueueNode          base;
  RVS_EngineMessageKind  kind;
  union {
    struct {
      RVS_Command   command;
      RVS_MessageID request_id;
    };
    RVS_DemonReply demon_reply;
  };
} RVS_EngineMessage;

////////////////////////////////
// Helpers

#define rvs_session_params_from_engine(e) (RVS_SessionControlParams){ .control = (e)->control, .session = (e)->session }
#define rvs_engine_params_from_engine(e)  (RVS_EngineSessionParams) { .engine = (e), .control = (e)->control, .session = (e)->session }

////////////////////////////////

thread_static U32 rvs_control_mutex_depth;

////////////////////////////////

internal void
rvs_control_mutex_take(RVS_EngineControl *control)
{
  AssertAlways(rvs_control_mutex_depth == 0);
  mutex_take(control->mutex);
  rvs_control_mutex_depth = 1;
}

internal void
rvs_control_mutex_drop(RVS_EngineControl *control)
{
  AssertAlways(rvs_control_mutex_depth == 1);
  rvs_control_mutex_depth = 0;
  mutex_drop(control->mutex);
}

internal void
rvs_control_assert_unlocked(void)
{
  AssertAlways(rvs_control_mutex_depth == 0);
}

//////////////////////////////
// Engine Message Queue

internal void
rvs_engine_message_copy(Arena *arena, RVS_EngineMessage *dst, RVS_EngineMessage *src)
{
  *dst = *src;
  switch (src->kind) {
  case RVS_EngineMessageKind_DispatchCommand: { rvs_command_copy(arena, &dst->command, &src->command);             } break;
  case RVS_EngineMessageKind_DemonReply:      { rvs_demon_reply_copy(arena, &dst->demon_reply, &src->demon_reply); } break;
  case RVS_EngineMessageKind_Shutdown:        { /* nothing to copy */ } break;
  default: { InvalidPath; } break;
  }
}

internal void
rvs_engine_message_queue_copy(Arena *arena, void *dst, void *src)
{
  rvs_engine_message_copy(arena, dst, src);
}

internal RVS_Result
rvs_engine_send_message_locked(RVS_Engine *engine, RVS_EngineMessage *spec)
{
  return rvs_queue_push_copy(engine->inbox_queue, spec, rvs_engine_message_queue_copy);
}

internal RVS_Result
rvs_engine_send_message(RVS_Engine *engine, RVS_EngineMessage *spec)
{
  return rvs_engine_send_message_locked(engine, spec);
}

////////////////////////////////
// Engine Control

internal RVS_EngineControl *
rvs_engine_control_alloc(void)
{
  Arena *arena = arena_alloc(.name = "Engine Control");
  RVS_EngineControl *control = push_array(arena, RVS_EngineControl, 1);
  control->arena     = arena;
  control->mutex     = mutex_alloc();
  control->ref_count = 1;
  return control;
}

internal void
rvs_engine_control_addref(RVS_EngineControl *control)
{
  ins_atomic_u32_inc_eval(&control->ref_count);
}

internal void
rvs_engine_control_release(RVS_EngineControl *control)
{
  if (ins_atomic_u32_dec_eval(&control->ref_count) == 0) {
    mutex_release(control->mutex);
    arena_release(control->arena);
  }
}

////////////////////////////////
// Session

typedef struct
{
  RVS_QueueNode base;
  RVS_Event     event;
} RVS_SessionEventMessage;

internal void
rvs_session_event_message_copy(Arena *arena, void *dst_ptr, void *src_ptr)
{
  (void)arena;
  MemoryCopy(dst_ptr, src_ptr, sizeof(RVS_SessionEventMessage));
}

internal void
rvs_session_addref(RVS_Session *session)
{
  ins_atomic_u32_inc_eval(&session->ref_count);
}

internal RVS_Result
rvs_session_push_event(RVS_Session *session, RVS_Event *event)
{
  rvs_control_assert_unlocked();
  return rvs_queue_push_copy(session->event_queue, &(RVS_SessionEventMessage){ .event = *event }, rvs_session_event_message_copy);
}

internal void
rvs_session_close_events(RVS_Session *session)
{
  rvs_queue_close(session->event_queue);
  rvs_entity_store_close(&session->entities);
}

internal RVS_Session *
rvs_session_alloc(RVS_SessionControlParams params, RVS_RequestPool *request_pool)
{
  Arena *arena = arena_alloc(.name = "Debug Engine Session");
  RVS_Session *session = push_array(arena, RVS_Session, 1);
  session->arena       = arena;
  session->ref_count   = 0;
  session->event_queue = rvs_queue_alloc(sizeof(RVS_SessionEventMessage), AlignOf(RVS_SessionEventMessage));
  rvs_entity_store_init(&session->entities, arena, params.control->mutex);
  session->scheduler   = rvs_scheduler_init(arena, request_pool);

  rvs_session_addref(session); // engine
  rvs_session_addref(session); // returned handle

  return session;
}

internal void
rvs_session_release_ref(RVS_Session *session)
{
  if (ins_atomic_u32_dec_eval(&session->ref_count) == 0) { // is this the last ref?
    rvs_queue_release(session->event_queue);
    rvs_entity_store_release(&session->entities);
    rvs_scheduler_release(session->scheduler);
    arena_release(session->arena);
  }
}

internal void
rvs_session_release_engine(RVS_Session *session)
{
  rvs_session_release_ref(session); // drop engine ownership
}

internal RVS_Result
rvs_session_submit(RVS_EngineSessionParams params, RVS_Command command, RVS_SubmitInfo *submit_out)
{
  ProfBeginFunction();

  MemoryZeroStruct(submit_out);

  RVS_Result   result  = RVS_Result_Error;
  RVS_Engine  *engine  = params.engine;
  RVS_Session *session = params.session;

  rvs_control_mutex_take(params.control);

  if (!params.control->is_shutdown &&                                // reject submits to uninited engine
      ins_atomic_u32_eval(&engine->state) && RVS_WorkerState_Live) { // thread worker mus be live -- also engine should sleep here if the worker is still being created
    // allocate a new request for the submitted command
    RVS_Request *request = rvs_request_pool_request_alloc(engine->request_pool);

    // send command to the engine worker
    RVS_EngineMessage message = {
      .kind       = RVS_EngineMessageKind_DispatchCommand,
      .command    = command,
      .request_id = request->id,
    };
    RVS_Result result = rvs_engine_send_message_locked(engine, &message);

    if (result == RVS_Result_Ok) {
      submit_out->request = request;
      submit_out->control = rvs_request_control_alloc((RVS_SessionControlParams){ .control = params.control, .session = params.session }, request->id);
    } else {
      // failed to send command
      rvs_request_pool_release_request(engine->request_pool, request);
      result = RVS_Result_Error; // TODO: better error here?
    }
  } else {
    result = RVS_Result_EngineStopped;
  }

  rvs_control_mutex_drop(params.control);

  ProfEnd();
  return result;
}

void
rvs_session_release(RVS_Session *session)
{
  rvs_session_release_ref(session);
}

////////////////////////////////
// Request Control

internal RVS_RequestControl *
rvs_request_control_alloc(RVS_SessionControlParams params, RVS_MessageID request_id)
{
  Arena              *arena   = arena_alloc(.name = "Engine Request Control");
  RVS_RequestControl *control = push_array(arena, RVS_RequestControl, 1);
  control->arena      = arena;
  control->params     = params;
  control->request_id = request_id;

  rvs_session_addref(params.session);
  rvs_engine_control_addref(params.control);

  return control;
}

void
rvs_request_control_release(RVS_RequestControl *owner)
{
  rvs_engine_control_release(owner->params.control);
  rvs_session_release(owner->params.session);
  arena_release(owner->arena);
}

RVS_Result
rvs_request_control_cancel(RVS_RequestControl *owner)
{
  NotImplemented;
  return RVS_Result_Unsupported;
#if 0
  Temp scratch = scratch_begin(0, 0);

  RVS_SchedulerEvent event = {
    .kind       = RVS_SchedulerEventKind_PreDispatchCancelled,
    .request_id = owner->request_id,
  };
  RVS_SchedulerDecision decision = {0};
  RVS_Result result = rvs_engine_consider_scheduler_event(owner->params, event, scratch.arena, 1, &decision);

  if (result == RVS_Result_Ok) {
    if (decision.emissions.first && decision.emissions.first->kind == RVS_SchedulerEmission_CompleteRequest) {
      AssertAlways(decision.command.kind == RVS_SchedulerCommand_Null);
      rvs_session_execute_scheduler_emissions(owner->params.session, &decision);
      rvs_scheduler_decision_release(&owner->params.session->scheduler, &decision);
    } else {
      result = RVS_Result_Error;
    }
  }

  scratch_end(scratch);
  return result;
#endif
}

////////////////////////////////
// API

RVS_Result
rvs_session_launch(RVS_EngineSessionParams params, String8 cmdl, String8 wdir, RVS_SubmitInfo *submit_out)
{
  // @API_ARG_CHECK
  if (submit_out) { MemoryZeroStruct(submit_out); }
  if (params.session == 0 || submit_out == 0) {
    return RVS_Result_InvalidArgument;
  }

  Temp scratch = scratch_begin(0, 0);
  RVS_Command command = {
    .kind          = RVS_CommandKind_Launch,
    .launch_params = {
      .cmd_line = str8_split_by_string_chars(scratch.arena, cmdl, str8_lit(" "), 0),
      .path     = wdir,
    }
  };
  RVS_Result result = rvs_session_submit(params, command, submit_out);
  scratch_end(scratch);
  return result;
}

RVS_Result
rvs_session_run_many(RVS_EngineSessionParams params, RVS_ProgramID *programs, U64 programs_count, RVS_SubmitInfo *submit_out)
{
  // @API_ARG_CHECK
  if (submit_out) { MemoryZeroStruct(submit_out); }
  if (params.session == 0 || programs == 0 || programs_count == 0 || submit_out == 0)  {
    return RVS_Result_InvalidArgument;
  }
  for EachIndex(program_idx, programs_count) {
    if (MemoryIsZeroStruct(&programs[program_idx])) {
      return RVS_Result_InvalidArgument;
    }
    for EachIndex(previous_idx, program_idx) {
      if (MemoryMatchStruct(&programs[previous_idx], &programs[program_idx])) {
        return RVS_Result_InvalidArgument;
      }
    }
  }

  RVS_Command cmd = {
    .kind = RVS_CommandKind_Run,
    .run  = {
      .programs_count = programs_count,
      .programs       = programs,
      .mode           = RVS_RunMode_Normal,
      .intent         = { .kind = RVS_RunIntentKind_Execute },
    },
  };
  return rvs_session_submit(params, cmd, submit_out);
}

RVS_Result
rvs_session_run_to_address(RVS_EngineSessionParams params, RVS_ProgramID program_id, U64 vaddr, RVS_SubmitInfo *submit_out)
{
  // @API_ARG_CHECK
  if (submit_out) { MemoryZeroStruct(submit_out); }
  if (params.session == 0 || submit_out == 0 || MemoryIsZeroStruct(&program_id) || vaddr == 0) {
    return RVS_Result_InvalidArgument;
  }

  RVS_Command cmd = {
    .kind = RVS_CommandKind_Run,
    .run  = {
      .programs_count = 1,
      .programs       = &program_id,
      .mode           = RVS_RunMode_ToAddress,
      .address        = vaddr,
      .intent         = { .kind = RVS_RunIntentKind_Execute },
    },
  };
  return rvs_session_submit(params, cmd, submit_out);
}

RVS_Result
rvs_session_pause_many(RVS_EngineSessionParams params, RVS_ProgramID *programs, U64 programs_count, RVS_SubmitInfo *submit_out)
{
  // @API_ARG_CHECK
  if (submit_out) { MemoryZeroStruct(submit_out); }
  if (params.session == 0 || programs == 0 || programs_count == 0 || submit_out == 0) {
    return RVS_Result_InvalidArgument;
  }
  for EachIndex(program_idx, programs_count) {
    if (MemoryIsZeroStruct(&programs[program_idx])) {
      return RVS_Result_InvalidArgument;
    }
    for EachIndex(previous_idx, program_idx) {
      if (MemoryMatchStruct(&programs[previous_idx], &programs[program_idx])) {
        return RVS_Result_InvalidArgument;
      }
    }
  }

  RVS_Command cmd = {
    .kind  = RVS_CommandKind_Pause,
    .pause = {
      .programs       = programs,
      .programs_count = programs_count
    }
  };
  return rvs_session_submit(params, cmd, submit_out);
}

RVS_Result
rvs_session_select_thread(RVS_EngineSessionParams params, RVS_ThreadID thread_id, RVS_SubmitInfo *submit_out)
{
  // @API_ARG_CHECK
  if (params.session == 0 || MemoryIsZeroStruct(&thread_id)) {
    return RVS_Result_InvalidArgument;
  }

  RVS_Command cmd = {
    .kind = RVS_CommandKind_SelectThread,
    .select_thread = thread_id,
  };
  return rvs_session_submit(params, cmd, submit_out);
}

RVS_Result
rvs_session_selected_thread(RVS_EngineSessionParams params, RVS_ProgramID *program_id_out, RVS_ThreadID *thread_id_out)
{
  NotImplemented;
  return RVS_Result_Unsupported;
#if 0
  // @API_ARG_CHECK
  if (program_id_out) { MemoryZeroStruct(program_id_out); }
  if (thread_id_out)  { MemoryZeroStruct(thread_id_out); }
  if (params.session == 0 || program_id_out == 0 || thread_id_out == 0) {
    return RVS_Result_InvalidArgument;
  }

  rvs_control_mutex_take(params.control);
  RVS_Result  result = RVS_Result_StaleState;
  RVS_ProgramID selected_target = rvs_program_id_zero();
  RVS_ThreadID selected_thread = rvs_thread_id_zero();
  B32 selected = rvs_scheduler_selected_thread_locked(&params.session->scheduler, &selected_target, &selected_thread);
  RVS_Thread *thread = selected ? rvs_entity_thread_from_id_locked(&params.session->entities, selected_thread) : 0;
  if (thread &&
      !thread->snapshot.is_retired &&
      rvs_program_id_match(thread->snapshot.program, selected_target)) {
    if (program_id_out) { *program_id_out = thread->snapshot.program; }
    if (thread_id_out)  { *thread_id_out  = thread->snapshot.thread; }
    result = RVS_Result_Ok;
  }
  rvs_control_mutex_drop(params.control);

  return result;
#endif
}

RVS_Result
rvs_session_continue(RVS_EngineSessionParams params, RVS_ProgramID program_id, RVS_SubmitInfo *submit_out)
{
  // @API_ARG_CHECK
  if (submit_out) { MemoryZeroStruct(submit_out); }
  if (params.session == 0 || MemoryIsZeroStruct(&program_id) || submit_out == 0) {
    return RVS_Result_InvalidArgument;
  }

  RVS_Command cmd = {
    .kind = RVS_CommandKind_Run,
    .run  = {
      .mode           = RVS_RunMode_Normal,
      .intent         = { .kind = RVS_RunIntentKind_Continue },
      .programs_count = 1,
      .programs       = &program_id,
    },
  };
  return rvs_session_submit(params, cmd, submit_out);
}

RVS_Result
rvs_session_step(RVS_EngineSessionParams  params,
                 RVS_StepKind             kind,
                 RVS_StepUnit             unit,
                 RVS_ThreadID             thread_id,
                 RVS_SubmitInfo          *submit_out)
{
  // @API_ARG_CHECK
  if (submit_out) { MemoryZeroStruct(submit_out); }
  if (params.session == 0 || submit_out == 0 || MemoryIsZeroStruct(&thread_id)) {
    return RVS_Result_InvalidArgument;
  }

  RVS_Command cmd = {
    .kind = RVS_CommandKind_Step,
    .step = {
      .thread_id = thread_id,
      .kind      = kind,
      .unit      = unit,
    }
  };
  return rvs_session_submit(params, cmd, submit_out);
}

RVS_Result
rvs_session_ack_event(RVS_EngineSessionParams params, U64 sequence)
{
  NotImplemented;
  return RVS_Result_Unsupported;
#if 0
  // @API_ARG_CHECK
  if (params.session == 0 || sequence == 0) {
    return RVS_Result_InvalidArgument;
  }

  RVS_SessionControlParams session_params = {
    .control = params.control,
    .session = params.session,
  };
  RVS_SchedulerEvent event = {
    .kind         = RVS_SchedulerEvent_AcknowledgeEvent,
    .acknowledged = { .sequence = sequence }
  };
  RVS_Result result = rvs_control_reduce_scheduler_event_expect_empty(session_params, event, 1);
  return result == RVS_Result_StaleState ? RVS_Result_Ok : result;
#endif
}

RVS_Result
rvs_session_fetch_program(RVS_Session *session, RVS_ProgramID id, U64 wait_us, RVS_ProgramSnapshot *snapshot_out)
{
  // @API_ARG_CHECK
  if (snapshot_out) { MemoryZeroStruct(snapshot_out); }
  if (session == 0 || snapshot_out == 0 || MemoryIsZeroStruct(&id)) {
    return RVS_Result_InvalidArgument;
  }

  rvs_session_addref(session);
  RVS_Result result = rvs_entity_fetch_program(&session->entities, id, wait_us, snapshot_out);
  rvs_session_release(session);

  return result;
}

RVS_Result
rvs_session_fetch_process(RVS_Session *session, RVS_ProcessID id, U64 wait_us, RVS_ProcessSnapshot *snapshot_out)
{
  // @API_ARG_CHECK
  if (snapshot_out) { MemoryZeroStruct(snapshot_out); }
  if (session == 0 || snapshot_out == 0 || MemoryIsZeroStruct(&id)) {
    return RVS_Result_InvalidArgument;
  }

  rvs_session_addref(session);
  RVS_Result result = rvs_entity_fetch_process(&session->entities, id, wait_us, snapshot_out);
  rvs_session_release(session);

  return result;
}

RVS_Result
rvs_session_fetch_thread(RVS_Session *session, RVS_ThreadID id, U64 wait_us, RVS_ThreadSnapshot *snapshot_out)
{
  // @API_ARG_CHECK
  if (snapshot_out) { MemoryZeroStruct(snapshot_out); }
  if (session == 0 || snapshot_out == 0 || MemoryIsZeroStruct(&id)) {
    return RVS_Result_InvalidArgument;
  }

  rvs_session_addref(session);
  RVS_Result result = rvs_entity_fetch_thread(&session->entities, id, wait_us, snapshot_out);
  rvs_session_release(session);

  return result;
}

RVS_Result
rvs_session_wait_for_event(Arena *arena, RVS_Session *session, U64 wait_us, RVS_Event *event_out)
{
  // @API_ARG_CHECK
  if (event_out) { MemoryZeroStruct(event_out); }
  if (session == 0) {
    return RVS_Result_InvalidArgument;
  }

  rvs_session_addref(session);

  RVS_Result               result  = RVS_Result_Timeout;
  RVS_QueuePopResult       pop     = rvs_queue_pop_result(session->event_queue, wait_us);
  RVS_SessionEventMessage *message = (RVS_SessionEventMessage *)pop.node;

  if (message) {
    if (pop.is_closed) {
      result = RVS_Result_EngineStopped;
    } else {
      *event_out = message->event;
      result = RVS_Result_Ok;
    }
    rvs_queue_recycle(session->event_queue, &message->base);
  }

  rvs_session_release(session);

  return result;
}

RVS_Result
rvs_session_copy_programs(Arena *arena, RVS_Session *session, RVS_ProgramSnapshot **snapshots_out, U64 *snapshots_count_out)
{
  // @API_ARG_CHECK
  if (snapshots_out)       { MemoryZeroStruct(snapshots_out);       }
  if (snapshots_count_out) { MemoryZeroStruct(snapshots_count_out); }
  if (arena == 0 || session == 0 || snapshots_out == 0 || snapshots_count_out == 0) {
    return RVS_Result_InvalidArgument;
  }

  rvs_session_addref(session);
  RVS_Result result = rvs_entity_copy_programs(&session->entities, arena, snapshots_out, snapshots_count_out);
  rvs_session_release(session);

  return result;
}

internal void
rvs_engine_demon_reply_callback(RVS_Demon *demon, RVS_DemonReply *reply, void *ud)
{
  (void)demon;
  rvs_control_assert_unlocked();
  RVS_Result result = rvs_engine_send_message(ud, &(RVS_EngineMessage){
                                              .kind        = RVS_EngineMessageKind_DemonReply,
                                              .demon_reply = *reply,
                                              });
  AssertAlways(result == RVS_Result_Ok);
}

internal void rvs_engine_worker(void *user_data);

RVS_Result
rvs_engine_init(RVS_Engine **engine_out)
{
  static RVS_Engine engine;

  // @API_ARG_CHECK
  if (engine_out == 0) {
    return RVS_Result_InvalidArgument;
  }
  MemoryZeroStruct(engine_out);
  if (ins_atomic_u32_eval_cond_assign(&engine.state, RVS_WorkerState_Initing, RVS_WorkerState_Null) != RVS_WorkerState_Null) {
    return RVS_Result_AlreadyInited;
  }

  engine.arena        = arena_alloc(.name = "Debug Engine");
  engine.inbox_queue  = rvs_queue_alloc(sizeof(RVS_EngineMessage), AlignOf(RVS_EngineMessage));
  engine.control      = rvs_engine_control_alloc();
  engine.request_pool = rvs_request_pool_alloc();

  RVS_Result result = rvs_demon_init(&engine, rvs_engine_demon_reply_callback, &engine.demon);
  if (result == RVS_Result_Ok) {
    // create a session (currently supported one session)
    engine.session = rvs_session_alloc((RVS_SessionControlParams){ .control = engine.control }, engine.request_pool);

    // dispatch the engine thread launch
    engine.thread = thread_launch(rvs_engine_worker, &engine);

    // was the engine thread launched?
    if ( ! MemoryIsZeroStruct(&engine.thread)) {
      ins_atomic_u32_eval_assign(&engine.state, RVS_WorkerState_Live);
      *engine_out = &engine;
    } else {
      ins_atomic_u32_eval_assign(&engine.state, RVS_WorkerState_Exited);
      result = RVS_Result_Error;
    }
  } else {
    ins_atomic_u32_eval_assign(&engine.state, RVS_WorkerState_Exited);
  }

  if (result != RVS_Result_Ok) {
    rvs_request_pool_destroy(engine.request_pool);
    rvs_engine_control_release(engine.control);
    rvs_queue_release(engine.inbox_queue);
    arena_release(engine.arena);
  }

  return result;
}

RVS_Result
rvs_engine_shutdown(RVS_Engine *engine)
{
  // @API_ARG_CHECK
  if (engine == 0) {
    return RVS_Result_InvalidArgument;
  }
  if (ins_atomic_u32_eval_cond_assign(&engine->state, RVS_WorkerState_Terminating, RVS_WorkerState_Live) != RVS_WorkerState_Live) {
    return RVS_Result_EngineStopped;
  }

  rvs_control_mutex_take(engine->control);
  engine->control->is_shutdown = 1;
  rvs_control_mutex_drop(engine->control);

  RVS_Command    cmd    = { .kind = RVS_CommandKind_Exit };
  RVS_SubmitInfo submit = {0};
  RVS_Result     result = rvs_session_submit(rvs_engine_params_from_engine(engine), cmd, &submit);

  if (result == RVS_Result_Ok) {
    RVS_CommandReply reply = {0};
    result = rvs_request_wait(submit.request, max_U64, &reply);

    if (result == RVS_Result_Ok) {
      // shutdown the DEMON thread
      AssertAlways(rvs_demon_shutdown(engine->demon) == RVS_Result_Ok); // TODO: report bad demon exit
      rvs_demon_release_resources(engine->demon);

      // wait for the engine worker to exit
      thread_join(engine->thread, max_U64);

      // release request pool
      rvs_request_pool_release_engine(engine->request_pool);

      // release engine thread resources
      rvs_queue_release(engine->inbox_queue);

      // release session
      if (engine->session) {
        rvs_control_mutex_take(engine->control);
        rvs_session_close_events(engine->session); 
        rvs_session_release_engine(engine->session);
        engine->session = 0;
        rvs_control_mutex_drop(engine->control);
      }

      rvs_engine_control_release(engine->control);
      arena_release(engine->arena);
      MemoryZeroStruct(engine);
    }
  }

  return result;
}

RVS_Result
rvs_engine_launch(RVS_Engine *engine, String8 cmdl, String8 wdir, RVS_SubmitInfo *submit_out)
{
  return rvs_session_launch(rvs_engine_params_from_engine(engine), cmdl, wdir, submit_out);
}

RVS_Result
rvs_engine_run(RVS_Engine *engine, RVS_ProgramID *programs, U64 programs_count, RVS_SubmitInfo *submit_out)
{
  return rvs_session_run_many(rvs_engine_params_from_engine(engine), programs, programs_count, submit_out);
}

RVS_Result
rvs_engine_run_to_address(RVS_Engine *engine, RVS_ProgramID program_id, U64 vaddr, RVS_SubmitInfo *submit_out)
{
  return rvs_session_run_to_address(rvs_engine_params_from_engine(engine), program_id, vaddr, submit_out);
}

RVS_Result
rvs_engine_interrupt(RVS_Engine *engine, RVS_ProgramID *programs, U64 programs_count, RVS_SubmitInfo *submit_out)
{
  return rvs_session_pause_many(rvs_engine_params_from_engine(engine), programs, programs_count, submit_out);
}

RVS_Result
rvs_engine_select_thread(RVS_Engine *engine, RVS_ThreadID thread_id, RVS_SubmitInfo *submit_out)
{
  return rvs_session_select_thread(rvs_engine_params_from_engine(engine), thread_id, submit_out);
}

RVS_Result
rvs_engine_selected_thread(RVS_Engine *engine, RVS_ProgramID *program_id_out, RVS_ThreadID *thread_id_out)
{
  return rvs_session_selected_thread(rvs_engine_params_from_engine(engine), program_id_out, thread_id_out);
}

RVS_Result
rvs_engine_continue(RVS_Engine *engine, RVS_ProgramID program_id, RVS_SubmitInfo *submit_out)
{
  return rvs_session_continue(rvs_engine_params_from_engine(engine), program_id, submit_out);
}

RVS_Result
rvs_engine_step(RVS_Engine *engine, RVS_StepKind kind, RVS_StepUnit unit, RVS_ThreadID thread_id, RVS_SubmitInfo *submit_out)
{
  return rvs_session_step(rvs_engine_params_from_engine(engine), kind, unit, thread_id, submit_out);
}

RVS_Result
rvs_engine_wait_for_event(Arena *arena, RVS_Engine *engine, U64 wait_us, RVS_Event *event_out)
{
  return rvs_session_wait_for_event(arena, engine->session, wait_us, event_out);
}

RVS_Result
rvs_engine_ack_event(RVS_Engine *engine, U64 sequence)
{
  return rvs_session_ack_event(rvs_engine_params_from_engine(engine), sequence);
}

RVS_Result
rvs_engine_copy_programs(Arena *arena, RVS_Engine *engine, RVS_ProgramSnapshot **snapshots_out, U64 *snapshots_count_out)
{
  return rvs_session_copy_programs(arena, engine->session, snapshots_out, snapshots_count_out);
}

RVS_Result
rvs_engine_fetch_program(RVS_Engine *engine, RVS_ProgramID id, U64 wait_us, RVS_ProgramSnapshot *snapshot_out)
{
  return rvs_session_fetch_program(engine->session, id, wait_us, snapshot_out);
}

RVS_Result
rvs_engine_fetch_process(RVS_Engine *engine, RVS_ProcessID id, U64 wait_us, RVS_ProcessSnapshot *snapshot_out)
{
  return rvs_session_fetch_process(engine->session, id, wait_us, snapshot_out);
}

RVS_Result
rvs_engine_fetch_thread(RVS_Engine *engine, RVS_ThreadID id, U64 wait_us, RVS_ThreadSnapshot *snapshot_out)
{
  return rvs_session_fetch_thread(engine->session, id, wait_us, snapshot_out);
}

////////////////////////////////
// Engine Worker

#if 0
internal RVS_SchedulerEvent
rvs_engine_execute_scheduler_decision_unlocked(RVS_Engine *engine, RVS_SchedulerDecision *decision)
{
  rvs_control_assert_unlocked();
  rvs_session_execute_scheduler_emissions(engine->session, decision);

  RVS_SchedulerCommand *command         = &decision->command;
  RVS_SchedulerEvent    immediate_event = {0};

  RVS_Result result = RVS_Result_Ok;

  switch (command->kind) {
  case RVS_SchedulerCommand_LaunchExecution: {
    result = rvs_engine_send_demon_message(engine, (RVS_DemonMessage){
      .type       = RVS_DemonMessage_Launch,
      .request_id = command->token.request_id,
      .launch     = { .params = command->launch_execution.params },
    });
  } break;
  case RVS_SchedulerCommand_RunExecution: {
    result = command->run_execution.prepare_result;
    if (result == RVS_Result_Ok) {
      result = rvs_engine_send_demon_message(engine,
                                             (RVS_DemonMessage){
                                               .type       = RVS_DemonMessage_Run,
                                               .request_id = command->token.request_id,
                                               .run = {
                                                 .processes       = command->run_execution.processes,
                                                 .processes_count = command->run_execution.processes_count,
                                                 .traps           = command->run_execution.traps
                                             }});
    } else {
      InvalidPath;
    }
  } break;
  case RVS_SchedulerCommand_InterruptExecution: {
    result = rvs_engine_send_demon_message(engine,
                                           (RVS_DemonMessage){
                                             .type       = RVS_DemonMessage_InterruptExecution,
                                             .request_id = command->token.request_id,
                                             .interrupt_execution = {
                                               .execution_request_id = command->interrupt_execution.execution_request_id,
                                               .command_id           = command->token.command_id
                                           }});
  } break;
  case RVS_SchedulerCommand_ResumeTargetSubset: {
    result = command->resume_target_subset.execution.prepare_result;
    if (result == RVS_Result_Ok) {
      result = rvs_engine_send_demon_message(engine,
                                             (RVS_DemonMessage){
                                               .type       = RVS_DemonMessage_Resume,
                                               .request_id = command->token.request_id,
                                               .resume = {
                                                 .processes            = command->resume_target_subset.execution.processes,
                                                 .processes_count      = command->resume_target_subset.execution.processes_count,
                                                 .execution_request_id = command->resume_target_subset.execution_request_id,
                                                 .command_id           = command->token.command_id,
                                                 .traps                = command->resume_target_subset.execution.traps
                                            }});
    }
  } break;
  case RVS_SchedulerCommand_PumpLaunch: {
    result = rvs_engine_send_demon_message(engine,
                                           (RVS_DemonMessage){
                                             .type = RVS_DemonMessage_Pump,
                                             .request_id = command->token.request_id,
                                             .pump = { .command_id = command->token.command_id
                                           }});
  } break;
  case RVS_SchedulerCommand_PublishTarget: {
    immediate_event = (RVS_SchedulerEvent){
      .kind = RVS_SchedulerEvent_CommandOutcome,
      .command_outcome = {
        .command_kind = command->kind,
        .command      = command->token,
        .result       = RVS_Result_Ok,
        .process      = rvs_process_id_from_handle(command->publish_target.process),
        .pid          = command->publish_target.pid
      }
    };
  } break;
  default: { InvalidPath; }
  }

  if (immediate_event.kind == RVS_SchedulerEvent_Null &&
      command->kind != RVS_SchedulerCommand_Null &&
      result != RVS_Result_Ok) {
    immediate_event = (RVS_SchedulerEvent){
      .kind            = RVS_SchedulerEvent_CommandOutcome,
      .command_outcome = {
        .command_kind = command->kind,
        .command = command->token,
        .result = result
      }
    };
  }

  return immediate_event;
}

internal void
rvs_session_execute_scheduler_emissions(RVS_Session *session, RVS_SchedulerDecision *decision)
{
  rvs_control_assert_unlocked();
  for EachNode(emission, RVS_SchedulerEmission, decision->emissions.first) {
    if (emission->kind == RVS_SchedulerEmission_CompleteRequest) {
      rvs_request_complete(emission->operation->request, emission->reply);
    } else if (emission->kind == RVS_SchedulerEmission_PublishEvent) {
      RVS_Result result = rvs_session_push_event(session, &emission->event);
      AssertAlways(result == RVS_Result_Ok || result == RVS_Result_EngineStopped);
    }
  }
}

internal RVS_Result
rvs_engine_consider_scheduler_event(Arena                    *arena,
                                    RVS_SessionControlParams  params,
                                    RVS_SchedulerEvent        event,
                                    B32                       reject_if_shutdown,
                                    RVS_SchedulerDecision    *decision_out)
{
  AssertAlways(params.control != 0 && params.session != 0);
  RVS_Result result = RVS_Result_EngineStopped;
  MemoryZeroStruct(decision_out);
  rvs_control_mutex_take(params.control);
  if (params.control->is_shutdown && event.kind == RVS_SchedulerEvent_CommandOutcome &&
      event.command_outcome.command_kind == RVS_SchedulerCommand_PublishTarget) {
    event.command_outcome.result = RVS_Result_EngineStopped;
  }
  if (reject_if_shutdown && params.control->is_shutdown) {
  } else {
    rvs_scheduler_consider_event_locked(&params.session->scheduler, event, effect_arena, decision_out);
    result = decision_out->result;
  }
  rvs_control_mutex_drop(params.control);
  return result;
}

internal RVS_Result
rvs_control_reduce_scheduler_event_expect_empty(RVS_SessionControlParams params, RVS_SchedulerEvent event, B32 reject_if_shutdown)
{
  Temp scratch = scratch_begin(0, 0);
  RVS_SchedulerDecision decision = {0};
  RVS_Result result = rvs_engine_consider_scheduler_event(scratch.arena, params, event, reject_if_shutdown, &decision);
  AssertAlways(decision.emissions.first == 0);
  AssertAlways(decision.command.kind == RVS_SchedulerCommand_Null);
  rvs_scheduler_decision_release(&params.session->scheduler, &decision);
  return result;
}

internal RVS_Result
rvs_engine_drive_scheduler_event(RVS_Engine *engine, RVS_SchedulerEvent event)
{
  enum { immediate_transition_limit = 64 };
  RVS_Result initial_result = RVS_Result_EngineStopped;

  for (U32 transition_idx = 0;; transition_idx += 1) {
    Temp scratch = scratch_begin(0, 0);

    RVS_SchedulerDecision decision = {0};
    RVS_Result result = rvs_engine_consider_scheduler_event(scratch.arena, rvs_session_params_from_engine(engine), event, 0, &decision);
    if (transition_idx == 0) { initial_result = result; }

    event = rvs_engine_execute_scheduler_decision_unlocked(engine, &decision);
    rvs_scheduler_decision_release(&engine->session->scheduler, &decision);

    scratch_end(scratch);

    if (event.kind == RVS_SchedulerEvent_Null) { break; }
    if (transition_idx + 1 >= immediate_transition_limit) {
      AssertAlways(event.kind == RVS_SchedulerEvent_CommandOutcome);
      RVS_Result enqueue_result = rvs_engine_send_message(engine, &(RVS_EngineMessage){
        .type            = RVS_EngineMessageKind_SchedulerEvent,
        .scheduler_event = event,
      });
      AssertAlways(enqueue_result == RVS_Result_Ok || enqueue_result == RVS_Result_EngineStopped);
      break;
    }
  }
  return initial_result;
}
internal void
rvs_engine_submit_scheduler_failure(RVS_Engine *engine, RVS_MessageID request_id, RVS_Result result)
{
  rvs_engine_drive_scheduler_event(engine, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_OperationFailed,
    .failed = { .request_id = request_id, .result = result },
  });
}
internal void
rvs_engine_process_command(RVS_Engine *engine, RVS_MessageID request_id, RVS_Command *command)
{
  RVS_Scheduler *scheduler = &engine->session->scheduler;
  rvs_scheduler_process_command(scheduler, command);
}

internal void
rvs_engine_complete_reply(RVS_Engine *engine, RVS_CommandReply reply)
{
  rvs_engine_drive_scheduler_event(engine,
    (RVS_SchedulerEvent){
      .kind = RVS_SchedulerEvent_BackendCompleted,
      .completed = { .reply = reply },
    }
  );
}

internal void
rvs_engine_process_demon_reply(RVS_Engine *engine, RVS_DemonReply *reply)
{
#if 0
  switch (reply->kind) {
  case RVS_DemonReplyKind_LaunchStarted: {
    ProfBegin("DEMON Reply: LaunchStarted");

    RVS_SchedulerEvent event = {
      .kind           = RVS_SchedulerEvent_LaunchStarted,
      .launch_started = {
        .request_id = reply->request_id,
        .pid        = reply->launch_started.pid,
      },
    };
    rvs_engine_drive_scheduler_event(engine, event);

    ProfEnd();
  } break;

  case RVS_DemonReplyKind_ActionResult: {
    RVS_DemonAction action = reply->action_result.action;
    RVS_Result result = reply->action_result.result;
    if (action == RVS_DemonAction_Launch) {
      if (result != RVS_Result_Ok) {
        rvs_engine_complete_reply(engine, (RVS_EngineReply){
          .request_id = reply->request_id,
          .result = result,
          .kind = RVS_EngineReplyKind_Launch,
        });
      }
    } else if (action == RVS_DemonAction_Run) {
      rvs_engine_drive_scheduler_event(engine, (RVS_SchedulerEvent){
        .kind = result == RVS_Result_Ok ? RVS_SchedulerEvent_DispatchAccepted : RVS_SchedulerEvent_OperationFailed,
        .failed = { .request_id = reply->request_id, .result = result },
      });
      if (result == RVS_Result_Ok) {
        rvs_engine_complete_reply(engine, (RVS_EngineReply){
          .request_id = reply->request_id,
          .result = result,
          .kind = RVS_EngineReplyKind_Run,
        });
      }
    } else if (action == RVS_DemonAction_Resume) {
      rvs_engine_drive_scheduler_event(engine, (RVS_SchedulerEvent){
        .kind = RVS_SchedulerEvent_CommandOutcome,
        .command_outcome = {
          .command_kind = RVS_SchedulerCommand_ResumeTargetSubset,
          .command = { .request_id = reply->request_id, .command_id = reply->action_result.command_id },
          .result = result,
        },
      });
    } else if (action == RVS_DemonAction_Terminate) {
      // Termination has no scheduler vertical slice yet.
    } else {
      InvalidPath;
    }
  } break;

  case RVS_DemonReplyKind_EventBatch: {
    U64 raw_events_count = 0;
    for EachNode(n, DMN_EventNode, reply->event_batch.events.first) {
      raw_events_count += 1;
    }
    Temp scratch = scratch_begin(0, 0);
    RVS_SchedulerEventDisposition *dispositions = push_array(scratch.arena, RVS_SchedulerEventDisposition, raw_events_count);
    rvs_engine_drive_scheduler_event(engine, (RVS_SchedulerEvent){
      .kind = RVS_SchedulerEvent_DemonEventBatch,
      .demon_events = {
        .source = reply->event_batch.command_id == 0 ? RVS_SchedulerEventBatchSource_Execution : RVS_SchedulerEventBatchSource_PumpLaunch,
        .command = { .request_id = reply->request_id, .command_id = reply->event_batch.command_id },
        .request_id = reply->request_id,
        .events = reply->event_batch.events,
        .dispositions = dispositions,
        .dispositions_count = raw_events_count,
      },
    });
    scratch_end(scratch);
  } break;

  case RVS_DemonReplyKind_ExecutionStopped: {
    rvs_engine_drive_scheduler_event(engine, (RVS_SchedulerEvent){
      .kind = RVS_SchedulerEvent_CommandOutcome,
      .command_outcome = {
        .command_kind = RVS_SchedulerCommand_InterruptExecution,
        .command = { .request_id = reply->request_id, .command_id = reply->execution_stopped.command_id },
        .result = RVS_Result_Ok,
      },
    });
  } break;

  case RVS_DemonReplyKind_ExecutionFinished: {
    rvs_engine_drive_scheduler_event(engine, (RVS_SchedulerEvent){
      .kind = RVS_SchedulerEvent_RunFinished,
      .request = { .request_id = reply->request_id },
    });
  } break;

  default: { InvalidPath; } break;
  }
#endif
}
#endif

internal RVS_Result
rvs_engine_send_demon_message(RVS_Engine *engine, RVS_DemonMessage message)
{
  rvs_control_assert_unlocked();
  if (ins_atomic_u32_eval(&engine->state) != RVS_WorkerState_Live) { return RVS_Result_EngineStopped; }
#if RVS_ENGINE_TESTING
  if (ins_atomic_u32_eval_cond_assign(&engine->test_fail_demon_message_type,
                                      RVS_DemonMessage_Null,
                                      (U32)message.type) == (U32)message.type) {
    return RVS_Result_Error;
  }
#endif
  RVS_Result result = rvs_demon_send_message(engine->demon, message);
  if (result != RVS_Result_Ok && ins_atomic_u32_eval(&engine->state) != RVS_WorkerState_Live) {
    result = RVS_Result_EngineStopped;
  }
  return result;
}

internal void
rvs_engine_scheduler_step(RVS_Engine *engine, RVS_SchedulerInput root)
{
  Temp scratch = scratch_begin(0, 0);

  rvs_scheduler_apply(engine->session->scheduler, root);

  for (B32 keep_running = 1; keep_running;) {
    Temp temp = temp_begin(scratch.arena);

    RVS_SchedulerEffect *effect = rvs_scheduler_pump(temp.arena, engine->session->scheduler);

    switch (effect->kind) {
    case RVS_SchedulerEffectKind_SendBackendMessage: {
      // send message to the backend worker
      RVS_Result send_result = rvs_engine_send_demon_message(engine, effect->backend_message);

      // feed back send result into the scheduler
      RVS_SchedulerInput input = {
        .kind                  = RVS_InputSourceKind_BackendSendResult,
        .backend_send_result.v = send_result
      };
      rvs_scheduler_apply(engine->session->scheduler, input);
    } break;

    case RVS_SchedulerEffectKind_Null: { keep_running = 0; } break;
    default: InvalidPath;
    }

    temp_end(temp);
  }

  scratch_end(scratch);
}

internal void
rvs_engine_drive_scheduler(RVS_Engine *engine, RVS_SchedulerInput root)
{
  if (root.kind == RVS_InputSourceKind_BackendReply &&
      root.backend_reply.v.kind == RVS_DemonReplyKind_EventBatch) {
    for EachNode(n, DMN_EventNode, root.backend_reply.v.event_batch.events.first) {
    }
  } else {
    rvs_engine_scheduler_step(engine, root);
  }
}

internal void
rvs_engine_worker(void *user_data)
{
  RVS_Engine *engine = user_data;

  for (B32 keep_going = 1; keep_going;) {
    // wait for an engine message
    RVS_EngineMessage *message = rvs_queue_pop_struct(engine->inbox_queue, RVS_EngineMessage, max_U64);
    if (message == 0) { continue; }

    // funnel message to the appropriate handler
    RVS_SchedulerInput input;
    switch (message->kind) {
    case RVS_EngineMessageKind_DispatchCommand: {
      input = (RVS_SchedulerInput){
        .kind    = RVS_InputSourceKind_Command,
        .command = {
          .request_id = message->request_id,
          .v          = message->command
        }
      };
      rvs_engine_drive_scheduler(engine, input);
    } break;
    case RVS_EngineMessageKind_DemonReply: {
      input = (RVS_SchedulerInput){
        .kind          = RVS_InputSourceKind_BackendReply,
        .backend_reply.v = message->demon_reply,
      };
      rvs_engine_drive_scheduler(engine, input);
    } break;
    case RVS_EngineMessageKind_Shutdown: { keep_going = 0; } break;
    default: { InvalidPath; } break;
    }

    // dispose of the message
    rvs_queue_recycle(engine->inbox_queue, &message->base);
  }

  ins_atomic_u32_eval_assign(&engine->state, RVS_WorkerState_Exited);
}

