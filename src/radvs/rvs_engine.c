// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////

#include "base/base_core.h"
#include "base/base_arena.h"
#include "base/base_strings.h"
#include "base/base_processes.h"

#include "radvs/rvs_engine.h"
#include "radvs/rvs_plan.h"
#include "radvs/rvs_plan.c"

////////////////////////////////

struct RVS_Session
{
  Arena           *arena;
  U32              ref_count;
  RVS_Queue       *event_queue;
  RVS_EntityStore *entities;
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

internal RVS_Session *
rvs_session_alloc(RVS_SessionControlParams params, RVS_RequestPool *request_pool)
{
  Arena *arena = arena_alloc(.name = "Debug Engine Session");
  RVS_Session *session = push_array(arena, RVS_Session, 1);
  session->arena       = arena;
  session->ref_count   = 0;
  session->event_queue = rvs_queue_alloc(sizeof(RVS_SessionEventMessage), AlignOf(RVS_SessionEventMessage));
  session->entities    = rvs_entity_store_alloc();
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
    rvs_entity_store_release(session->entities);
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

  if (!params.control->is_shutdown &&
      ins_atomic_u32_eval(&engine->state) == RVS_WorkerState_Live) {
    // allocate a new request for the submitted command
    RVS_Request *request = rvs_request_pool_request_alloc(engine->request_pool);

    // send command to the engine worker
    RVS_EngineMessage message = {
      .kind       = RVS_EngineMessageKind_DispatchCommand,
      .command    = command,
      .request_id = request->id,
    };
    result = rvs_engine_send_message_locked(engine, &message);

    if (result == RVS_Result_Ok) {
      submit_out->request = request;
      submit_out->control = rvs_request_control_alloc((RVS_SessionControlParams){ .control = params.control, .session = params.session }, request->id);
    } else {
      // failed to send command
      rvs_request_release(request);
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
      .programs.count = programs_count,
      .programs.v     = programs,
      .intent         = { .kind = RVS_RunIntentKind_Execute },
    },
  };
  return rvs_session_submit(params, cmd, submit_out);
}

#if 0
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
      .programs.count = 1,
      .programs.v     = &program_id,
      .mode           = RVS_RunMode_ToAddress,
      .address        = vaddr,
      .intent         = { .kind = RVS_RunIntentKind_Execute },
    },
  };
  return rvs_session_submit(params, cmd, submit_out);
}
#endif

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
      .programs.v     = programs,
      .programs.count = programs_count
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
rvs_session_wait_for_event(Arena *arena, RVS_Session *session, U64 wait_us, RVS_Event *event_out)
{
  // @API_ARG_CHECK
  if (event_out) { MemoryZeroStruct(event_out); }
  if (session == 0) { return RVS_Result_InvalidArgument; }

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
rvs_session_copy_programs(Arena *arena, RVS_EngineSessionParams params, RVS_Program **programs_out, U64 *programs_count_out)
{
  // @API_ARG_CHECK
  MemoryZeroStruct(programs_out);
  MemoryZeroStruct(programs_count_out);
  if (params.session == 0) { return RVS_Result_InvalidArgument; }

  RVS_Result result = RVS_Result_Ok;

  rvs_control_mutex_take(params.control);
  {
    RVS_Session     *session  = params.session;
    RVS_EntityStore *entities = session->entities;

    RVS_Program *programs    = push_array(arena, RVS_Program, entities->programs.count);
    U64          program_idx = 0;
    for EachNode(n, RVS_ProgramPtrNode, entities->programs.first) {
      programs[program_idx++] = *n->v;
    }

    *programs_out       = programs;
    *programs_count_out = entities->programs.count;
  }
  rvs_control_mutex_drop(params.control);

  return result;
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
      .intent         = { .kind = RVS_RunIntentKind_Continue },
      .programs.count = 1,
      .programs.v     = &program_id,
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
  if (ins_atomic_u32_eval_cond_assign(&engine->state, RVS_WorkerState_Exiting, RVS_WorkerState_Live) != RVS_WorkerState_Live) {
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

#if 0
RVS_Result
rvs_engine_run_to_address(RVS_Engine *engine, RVS_ProgramID program_id, U64 vaddr, RVS_SubmitInfo *submit_out)
{
  return rvs_session_run_to_address(rvs_engine_params_from_engine(engine), program_id, vaddr, submit_out);
}
#endif

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
rvs_engine_copy_programs(Arena *arena, RVS_Engine *engine, RVS_Program **program_out, U64 *program_count_out)
{
  return rvs_session_copy_programs(arena, rvs_engine_params_from_engine(engine), program_out, program_count_out);
}

RVS_Result
rvs_engine_ack_event(RVS_Engine *engine, U64 sequence)
{
  return rvs_session_ack_event(rvs_engine_params_from_engine(engine), sequence);
}

////////////////////////////////
//
// Engine Worker waits on messages from outside, unwraps and executes requested debugger command then replies back to
// the sender with the result of the operation.
//

internal void
rvs_engine_scheduler_advance(RVS_Engine *engine, RVS_SchedulerMessage input)
{
  Temp scratch = scratch_begin(0, 0);

  // feed new message to the scheduler
  rvs_scheduler_apply(engine->session->scheduler, engine->session->entities, input);

  for (B32 keep_running = 1; keep_running;) {
    Temp temp = temp_begin(scratch.arena);

    RVS_Effect *effect = rvs_scheduler_pump_effect(temp.arena, engine->session->scheduler);

    switch (effect->kind) {
    case RVS_EffectKind_SendBackendMessage: {
      // send message to the backend
      RVS_Result send_result = rvs_demon_send_message(engine->demon, effect->v.backend_message);

      // apply backend message send result
      RVS_SchedulerMessage message = {
        .kind                = RVS_SchedulerMessageKind_BackendSendResult,
        .backend_send_result = {
          .send_result = send_result,
          .request_id  = effect->v.backend_message.base.id,
        },
      };
      rvs_scheduler_apply(engine->session->scheduler, engine->session->entities, message);
    } break;

    case RVS_EffectKind_CompleteCommand: {
      // complete the command request
      RVS_Request *request        = rvs_request_from_id(engine->request_pool, effect->v.command_reply.request_id);
      B32          is_complete_ok = rvs_request_complete(request, effect->v.command_reply);

      // notify scheduler with the completion status
      RVS_SchedulerMessage message = {
        .kind                    = RVS_SchedulerMessageKind_CommandCompleteResult,
        .command_complete_result = {
          .is_complete_ok = is_complete_ok,
          .effect         = effect,
        }
      };
      rvs_scheduler_apply(engine->session->scheduler, engine->session->entities, message);
    } break;

    case RVS_EffectKind_BackendRun: {
      NotImplemented;
    } break;

    case RVS_EffectKind_Null: { keep_running = 0; } break;
    default: InvalidPath;
    }

    temp_end(temp);
  }

  scratch_end(scratch);
}

internal void
rvs_engine_worker(void *user_data)
{
  RVS_Engine *engine = user_data;

  for (B32 keep_going = 1; keep_going;) {
    // wait for an engine message
    RVS_EngineMessage *engine_message = rvs_queue_pop_struct(engine->inbox_queue, RVS_EngineMessage, max_U64);
    if (engine_message == 0) { continue; }

    // funnel message to the appropriate handler
    switch (engine_message->kind) {
    case RVS_EngineMessageKind_DispatchCommand: {
      RVS_SchedulerMessage scheduler_message = {
        .kind    = RVS_SchedulerMessageKind_Command,
        .command = {
          .request_id = engine_message->request_id,
          .v          = engine_message->command
        }
      };
      rvs_engine_scheduler_advance(engine, scheduler_message);
    } break;
      
    case RVS_EngineMessageKind_DemonReply: {
      // feed reply back to the scheduler
      RVS_SchedulerMessage scheduler_message = {
        .kind          = RVS_SchedulerMessageKind_BackendReply,
        .backend_reply = engine_message->demon_reply,
      };
      rvs_engine_scheduler_advance(engine, scheduler_message);
    } break;

    case RVS_EngineMessageKind_Shutdown: { keep_going = 0; } break;
    default: { InvalidPath; } break;
    }

    // dispose of the message
    rvs_queue_recycle(engine->inbox_queue, &engine_message->base);
  }

  ins_atomic_u32_eval_assign(&engine->state, RVS_WorkerState_Exited);
}

