#include "radvs/rvs_engine.h"
#include "radvs/rvs_demon.h"

////////////////////////////////
// Types

typedef struct RVS_Program RVS_Program;

#define RVS_ENGINE_COMMAND_XLIST \
  X(Launch, RVS_OperationClass_SessionLifecycle) \
  X(Run,    RVS_OperationClass_ProgramExecution)

typedef enum
{
  RVS_EngineCommandKind_Null,
#define X(kind, ...) RVS_EngineCommandKind_##kind,
  RVS_ENGINE_COMMAND_XLIST
#undef X
} RVS_EngineCommandKind;

internal RVS_OperationClass
rvs_operation_class_from_engine_command_kind(RVS_EngineCommandKind kind)
{
  switch (kind) {
  case RVS_EngineCommandKind_Null: return RVS_OperationClass_Null;
#define X(kind, operation_class) case RVS_EngineCommandKind_##kind: return operation_class;
  RVS_ENGINE_COMMAND_XLIST
#undef X
  }
  return RVS_OperationClass_Null;
}

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

typedef struct
{
  RVS_EngineCommand command;
  RVS_RequestPolicy policy;
  RVS_OperationKey  key;
} RVS_EngineSubmission;

internal B32
rvs_engine_submission_from_fixed_command(RVS_EngineCommand command, RVS_RequestPolicy policy, RVS_EngineSubmission *submission_out)
{
  RVS_OperationClass operation_class = rvs_operation_class_from_engine_command_kind(command.kind);
  if (operation_class != RVS_OperationClass_SessionLifecycle) {
    return 0;
  }
  *submission_out = (RVS_EngineSubmission){
    .command = command,
    .policy  = policy,
    .key     = {
      .operation_class = operation_class,
      .operation_id    = command.kind,
    },
  };
  return 1;
}

typedef enum
{
  RVS_EngineMessageType_Null,
  RVS_EngineMessageType_Command,
  RVS_EngineMessageType_DemonOutput,
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
      RVS_Demon       *source;
      RVS_DemonOutput *output;
      ArenaNode       *arena_node;
    } demon_output;
  };
} RVS_EngineMessage;

struct RVS_Program
{
  RVS_Program   *next;
  Arena         *arena;
  RVS_ProgramID  id;
  U32            pid;
  DMN_Handle     process;
  U64            state_epoch;
};

typedef struct
{
  RVS_QueueNode base;
  RVS_Event     event;
} RVS_EngineEventMessage;

typedef struct
{
  Arena       *arena;
  Mutex        mutex;
  RVS_Request *free_first;
  U64          live_requests_count;
  B32          engine_released;
} RVS_RequestPool;

typedef struct
{
  Arena           *arena;
  Mutex            mutex;
  U32              ref_count;
  B32              is_shutdown;
} RVS_EngineControl;

struct RVS_RequestControl
{
  Arena             *arena;
  RVS_Session       *session;
  RVS_EngineControl *control;
  RVS_Request       *request;
  RVS_OperationKey   key;
  B32                registered;
};

struct RVS_Session
{
  Arena             *arena;
  RVS_Engine        *engine;
  RVS_EngineControl *control;
  U32                ref_count;
  B32                engine_released;
  RVS_Queue         *event_queue;
  Arena             *program_arena;
  RVS_Program       *first_program;
  RVS_Program       *last_program;
  RVS_Request       *request_first;
  RVS_Request       *request_last;
  RVS_Request       *key_first;
  RVS_Request       *key_last;
};

struct RVS_Request
{
  RVS_Request       *next;
  RVS_Request       *prev;
  RVS_Request       *key_next;
  RVS_Request       *key_prev;
  RVS_RequestPool   *pool;
  RVS_Session       *session;
  Mutex              mutex;
  CondVar            cv;
  U32                ref_count;
  B32                is_dispatched;
  U64                captured_program_state_epoch;
  RVS_MessageID      request_id;
  U32                launch_pid;
  RVS_EngineCommandKind command_kind;
  RVS_RequestPolicy  policy;
  RVS_OperationKey   key;
  RVS_EngineReply    reply;
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
  Arena     *demon_output_arena;
  Mutex      demon_output_mutex;
  ArenaNode *demon_output_arena_active_list;
  ArenaNode *demon_output_arena_free_list;

};

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
  case RVS_EngineMessageType_DemonOutput: {
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
// Requests

// Operation Key Validation

internal B32
rvs_operation_key_is_complete(RVS_OperationKey key)
{
  return key.operation_id != 0;
}

internal B32
rvs_operation_key_is_well_formed_for_policy(RVS_RequestPolicy policy, RVS_OperationKey key)
{
  if ((key.operation_class == RVS_OperationClass_ReadOnly ||
       key.operation_class == RVS_OperationClass_ProgramExecution) &&
      dmn_handle_match(key.program_id, dmn_handle_zero())) {
    return 0;
  }
  if (policy == RVS_RequestPolicy_Independent) { return 1; }
  if ( ! rvs_operation_key_is_complete(key)) { return 0; }
  if (policy == RVS_RequestPolicy_JoinIfEqual) {
    return 1;
  }
  if (policy == RVS_RequestPolicy_RejectIfPending) {
    return key.operation_class == RVS_OperationClass_SessionLifecycle ||
            (key.operation_class == RVS_OperationClass_ProgramExecution && !dmn_handle_match(key.program_id, dmn_handle_zero()));
  }
  return 0;
}

internal RVS_Program *
rvs_session_program_from_id_locked(RVS_Session *session, RVS_ProgramID program_id)
{
  for EachNode(program, RVS_Program, session->first_program) {
    if (dmn_handle_match(program->id, program_id)) {
      return program;
    }
  }
  return 0;
}

// Program State

internal U64
rvs_session_program_state_epoch_locked(RVS_Session *session, RVS_ProgramID program_id)
{
  ProfBeginFunction();
  U64 result = 0;
  RVS_Program *program = rvs_session_program_from_id_locked(session, program_id);
  if (program) {
    result = program->state_epoch;
  }
  ProfEnd();
  return result;
}

internal void
rvs_session_bump_program_state_epoch_locked(RVS_Session *session, RVS_ProgramID program_id)
{
  ProfBeginFunction();
  RVS_Program *program = rvs_session_program_from_id_locked(session, program_id);
  if (program) {
    program->state_epoch += 1;
  }
  ProfEnd();
}

internal void
rvs_session_bump_program_state_epochs_locked(RVS_Session *session)
{
  ProfBeginFunction();
  for EachNode(program, RVS_Program, session->first_program) {
    program->state_epoch += 1;
  }
  ProfEnd();
}

internal B32
rvs_session_operation_key_resolves_locked(RVS_Session *session, RVS_OperationKey key)
{
  if (key.operation_class == RVS_OperationClass_Null) {
    return 1;
  }
  if (key.operation_class == RVS_OperationClass_SessionLifecycle) {
    return 1;
  }

  return rvs_session_program_from_id_locked(session, key.program_id) != 0;
}

internal B32
rvs_operation_key_match(RVS_OperationKey a, RVS_OperationKey b)
{
  return a.operation_class == b.operation_class &&
          dmn_handle_match(a.program_id, b.program_id) &&
         a.operation_id == b.operation_id;
}

internal B32
rvs_operation_keys_conflict(RVS_OperationKey a, RVS_OperationKey b)
{
  if (a.operation_class == RVS_OperationClass_SessionLifecycle || b.operation_class == RVS_OperationClass_SessionLifecycle) {
    return 1;
  }
  if (a.operation_class == RVS_OperationClass_ReadOnly || b.operation_class == RVS_OperationClass_ReadOnly) {
    return 0;
  }
  if (a.operation_class != RVS_OperationClass_ProgramExecution || b.operation_class != RVS_OperationClass_ProgramExecution) {
    return 0;
  }
  return dmn_handle_match(a.program_id, b.program_id);
}

// Request Pool

internal B32
rvs_session_has_conflicting_operation_locked(RVS_Session *session, RVS_OperationKey key)
{
  ProfBeginFunction();
  B32 result = 0;
  if (key.operation_class != RVS_OperationClass_Null) {
    for EachNode(request, RVS_Request, session->request_first) {
      if (request->key.operation_class != RVS_OperationClass_Null && rvs_operation_keys_conflict(request->key, key)) {
        result = 1;
        break;
      }
    }
  }
  ProfEnd();
  return result;
}

internal RVS_RequestPool *
rvs_request_pool_alloc(void)
{
  ProfBeginFunction();
  Arena *arena = arena_alloc(.name = "Engine Request Pool");
  RVS_RequestPool *pool = push_array(arena, RVS_RequestPool, 1);
  pool->arena = arena;
  pool->mutex = mutex_alloc();
  ProfEnd();
  return pool;
}

internal void
rvs_request_pool_destroy(RVS_RequestPool *pool)
{
  ProfBeginFunction();
  AssertAlways(pool->live_requests_count == 0);
  for (RVS_Request *request = pool->free_first; request; request = request->next) {
    cond_var_release(request->cv);
    mutex_release(request->mutex);
  }
  mutex_release(pool->mutex);
  arena_release(pool->arena);
  ProfEnd();
}

// Engine Control

internal RVS_Request *
rvs_request_pool_request_alloc(RVS_RequestPool *pool)
{
  ProfBeginFunction();
  mutex_take(pool->mutex);
  AssertAlways( ! pool->engine_released);

  RVS_Request *request = pool->free_first;
  if (request) {
    pool->free_first = request->next;
    Mutex mutex = request->mutex;
    CondVar cv = request->cv;
    MemoryZeroStruct(request);
    request->mutex = mutex;
    request->cv = cv;
  } else {
    request = push_array(pool->arena, RVS_Request, 1);
    request->mutex = mutex_alloc();
    request->cv = cond_var_alloc();
  }
  request->pool = pool;
  pool->live_requests_count += 1;

  mutex_drop(pool->mutex);
  ProfEnd();
  return request;
}

internal void
rvs_request_pool_request_release(RVS_Request *request)
{
  ProfBeginFunction();
  RVS_RequestPool *pool = request->pool;
  mutex_take(pool->mutex);
  AssertAlways(pool->live_requests_count != 0);
  request->next = pool->free_first;
  pool->free_first = request;
  pool->live_requests_count -= 1;
  B32 destroy_pool = pool->engine_released && pool->live_requests_count == 0;
  mutex_drop(pool->mutex);

  if (destroy_pool) {
    rvs_request_pool_destroy(pool);
  }
  ProfEnd();
}

internal void
rvs_request_pool_release_engine(RVS_RequestPool *pool)
{
  ProfBeginFunction();
  mutex_take(pool->mutex);
  AssertAlways( ! pool->engine_released);
  pool->engine_released = 1;
  B32 destroy_pool = pool->live_requests_count == 0;
  mutex_drop(pool->mutex);

  if (destroy_pool) {
    rvs_request_pool_destroy(pool);
  }
  ProfEnd();
}

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
rvs_engine_control_retain(RVS_EngineControl *control)
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

internal RVS_Session *
rvs_session_alloc(RVS_Engine *engine)
{
  Arena *arena = arena_alloc(.name = "Session");
  RVS_Session *session = push_array(arena, RVS_Session, 1);
  session->arena = arena;
  session->engine = engine;
  session->control = engine->control;
  session->ref_count = 2; // engine ownership plus the returned handle
  session->event_queue = rvs_queue_alloc(arena, sizeof(RVS_EngineEventMessage), AlignOf(RVS_EngineEventMessage));
  session->program_arena = arena_alloc(.name = "Session Programs");
  rvs_engine_control_retain(session->control);
  return session;
}

internal void
rvs_session_release_ref(RVS_Session *session)
{
  if (ins_atomic_u32_dec_eval(&session->ref_count) == 0) {
    AssertAlways(session->engine_released);
    rvs_queue_release(session->event_queue);
    rvs_engine_control_release(session->control);
    arena_release(session->arena);
  }
}

internal void
rvs_session_release_engine(RVS_Session *session)
{
  for EachNode(program, RVS_Program, session->first_program) {
    arena_release(program->arena);
  }
  arena_release(session->program_arena);
  session->program_arena = 0;
  session->first_program = 0;
  session->last_program = 0;
  session->engine = 0;
  session->engine_released = 1;
  rvs_session_release_ref(session); // drop engine ownership
}

internal RVS_RequestControl *
rvs_request_control_alloc(RVS_Session *session, RVS_Request *request, RVS_OperationKey key, B32 registered)
{
  ProfBeginFunction();
  Arena *arena = arena_alloc(.name = "Engine Operation Owner");
  RVS_RequestControl *owner = push_array(arena, RVS_RequestControl, 1);
  owner->arena      = arena;
  owner->session    = session;
  owner->control    = session->control;
  owner->request    = request;
  owner->key        = key;
  owner->registered = registered;
  rvs_engine_control_retain(owner->control);
  rvs_request_retain(request);
  ProfEnd();
  return owner;
}

// Active Requests

internal RVS_Request *
rvs_session_request_alloc_locked(RVS_Session *session, RVS_RequestPolicy policy, RVS_OperationKey key)
{
  ProfBeginFunction();
  RVS_Request *request = rvs_request_pool_request_alloc(session->engine->request_pool);
  request->ref_count        = 2; // engine ownership plus the returned handle
  request->request_id       = ins_atomic_u64_inc_eval(&session->engine->next_request_id);
  request->session          = session;
  request->policy           = policy;
  request->key              = key;
  request->reply.request_id = request->request_id;
  request->reply.result     = RVS_Result_Pending;
  DLLPushBack(session->request_first, session->request_last, request);
  ProfEnd();
  return request;
}

internal void
rvs_session_request_remove_locked(RVS_Session *session, RVS_Request *request)
{
  ProfBeginFunction();
  DLLRemove(session->request_first, session->request_last, request);
  ProfEnd();
}

internal void
rvs_session_request_key_remove_locked(RVS_Session *session, RVS_Request *request)
{
  ProfBeginFunction();
  if (request->key_prev) { request->key_prev->key_next = request->key_next; }
  else                   { session->key_first = request->key_next; }
  if (request->key_next) { request->key_next->key_prev = request->key_prev; }
  else                   { session->key_last = request->key_prev; }
  request->key_next = 0;
  request->key_prev = 0;
  ProfEnd();
}

internal RVS_Result
rvs_session_register_operation_locked(RVS_Session *session, RVS_OperationKey key, RVS_Request *request)
{
  ProfBeginFunction();
  RVS_Result result = RVS_Result_Error;
  if (request && request->pool == session->engine->request_pool && request->policy == RVS_RequestPolicy_JoinIfEqual && rvs_operation_key_is_complete(key)) {
    result = RVS_Result_Ok;
    for (RVS_Request *n = session->key_first; n; n = n->key_next) {
      if (rvs_operation_key_match(n->key, key)) {
        result = RVS_Result_AlreadyPending;
        break;
      }
    }
    if (result == RVS_Result_Ok) {
      request->key = key;
      rvs_request_retain(request); // retain terminal joins until the bridge unregisters the operation key
      request->key_prev = session->key_last;
      if (session->key_last) { session->key_last->key_next = request; }
      else                   { session->key_first = request; }
      session->key_last = request;
    }
  }
  ProfEnd();
  return result;
}

internal RVS_Request *
rvs_session_unregister_operation_locked(RVS_Session *session, RVS_OperationKey key)
{
  ProfBeginFunction();
  RVS_Request *result = 0;
  for (RVS_Request *n = session->key_first; n; n = n->key_next) {
    if (rvs_operation_key_match(n->key, key)) {
      result = n;
      rvs_session_request_key_remove_locked(session, result);
      break;
    }
  }
  ProfEnd();
  return result;
}

internal B32
rvs_request_complete(RVS_Request *request, RVS_EngineReply reply)
{
  ProfBeginFunction();
  B32 completed = 0;
  mutex_take(request->mutex);
  if (request->reply.result == RVS_Result_Pending) {
    request->reply = reply;
    cond_var_broadcast(request->cv);
    completed = 1;
  }
  mutex_drop(request->mutex);
  ProfEnd();
  return completed;
}

internal RVS_Request *
rvs_session_find_active_request_locked(RVS_Session *session, RVS_MessageID request_id)
{
  ProfBeginFunction();
  RVS_Request *result = 0;
  for EachNode(n, RVS_Request, session->request_first) {
    if (n->request_id == request_id) {
      result = n;
      break;
    }
  }
  ProfEnd();
  return result;
}

internal B32
rvs_engine_request_mark_dispatched(RVS_Engine *engine, RVS_MessageID request_id)
{
  ProfBeginFunction();
  B32 result = 0;
  mutex_take(engine->control->mutex);
  RVS_Session *session = engine->session;
  RVS_Request *request = rvs_session_find_active_request_locked(session, request_id);
  if (request) {
    mutex_take(request->mutex);
    if (request->reply.result == RVS_Result_Pending) {
      request->is_dispatched = 1;
      if (request->key.operation_class == RVS_OperationClass_ProgramExecution) {
        rvs_session_bump_program_state_epoch_locked(session, request->key.program_id);
      }
      result = 1;
    }
    mutex_drop(request->mutex);
  }
  mutex_drop(engine->control->mutex);
  ProfEnd();
  return result;
}

internal void
rvs_engine_retire_undispatched_request(RVS_Engine *engine, RVS_MessageID request_id)
{
  ProfBeginFunction();
  RVS_Request *request = 0;
  mutex_take(engine->control->mutex);
  RVS_Session *session = engine->session;
  RVS_Request *candidate = rvs_session_find_active_request_locked(session, request_id);
  if (candidate) {
    mutex_take(candidate->mutex);
    if (candidate->reply.result != RVS_Result_Pending && !candidate->is_dispatched) {
      request = candidate;
      rvs_session_request_remove_locked(session, request);
    }
    mutex_drop(candidate->mutex);
  }
  mutex_drop(engine->control->mutex);

  if (request) {
    rvs_request_release(request); // drop engine ownership
  }
  ProfEnd();
}

internal void
rvs_engine_publish_pending_requests(RVS_Engine *engine, RVS_Result result)
{
  ProfBeginFunction();
  mutex_take(engine->control->mutex);
  for EachNode(request, RVS_Request, engine->session->request_first) {
    rvs_request_complete(request, (RVS_EngineReply){
      .request_id = request->request_id,
      .result     = result,
      .kind       = request->reply.kind,
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
  RVS_Request *first = session->request_first;
  session->request_first = 0;
  session->request_last = 0;
  mutex_drop(engine->control->mutex);

  for (RVS_Request *n = first, *next = 0; n; n = next) {
    next = n->next;
    n->next = 0;
    n->prev = 0;
    rvs_request_complete(n, (RVS_EngineReply){
      .request_id = n->request_id,
      .result     = pending_result,
      .kind       = n->reply.kind,
    });
    rvs_request_release(n); // drop engine ownership
  }
  ProfEnd();
}

internal void
rvs_engine_clear_operation_keys(RVS_Engine *engine)
{
  ProfBeginFunction();
  mutex_take(engine->control->mutex);
  RVS_Session *session = engine->session;
  RVS_Request *first = session->key_first;
  session->key_first = 0;
  session->key_last = 0;
  mutex_drop(engine->control->mutex);

  for (RVS_Request *n = first, *next = 0; n; n = next) {
    next = n->key_next;
    n->key_next = 0;
    n->key_prev = 0;
    rvs_request_release(n); // drop operation-key ownership
  }
  ProfEnd();
}

// Completion

internal void
rvs_session_prepare_reply_locked(RVS_Session *session, RVS_Request *request, RVS_EngineReply *reply)
{
  if (request->key.operation_class == RVS_OperationClass_ReadOnly) {
    reply->program_state_epoch = request->captured_program_state_epoch;
    if (reply->program_state_epoch != rvs_session_program_state_epoch_locked(session, request->key.program_id)) {
      reply->result = RVS_Result_StaleState;
    }
  } else if (request->key.operation_class == RVS_OperationClass_SessionLifecycle && reply->result == RVS_Result_Ok) {
    rvs_session_bump_program_state_epochs_locked(session);
  }
}

internal void
rvs_engine_complete_reply(RVS_Engine *engine, RVS_EngineReply reply)
{
  ProfBeginFunction();
  RVS_Request *request = 0;
  mutex_take(engine->control->mutex);
  RVS_Session *session = engine->session;
  request = rvs_session_find_active_request_locked(session, reply.request_id);
  if (request) {
    rvs_session_request_remove_locked(session, request);
    rvs_session_prepare_reply_locked(session, request, &reply);
  }
  mutex_drop(engine->control->mutex);

  if (request) {
    rvs_request_complete(request, reply);
    rvs_request_release(request); // drop engine ownership
  }
  ProfEnd();
}

internal B32
rvs_engine_begin_launch(RVS_Engine *engine, RVS_MessageID request_id, U32 pid)
{
  ProfBeginFunction();
  B32 result = 0;
  mutex_take(engine->control->mutex);
  RVS_Request *request = rvs_session_find_active_request_locked(engine->session, request_id);
  if (request) {
    mutex_take(request->mutex);
    if (request->command_kind == RVS_EngineCommandKind_Launch &&
        request->reply.result == RVS_Result_Pending && request->launch_pid == 0) {
      request->launch_pid = pid;
      result = 1;
    }
    mutex_drop(request->mutex);
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
  RVS_Request *request = 0;
  RVS_EngineReply reply = {
    .request_id = request_id,
    .result     = result,
    .kind       = RVS_EngineReplyKind_Launch,
  };

  mutex_take(engine->control->mutex);
  RVS_Session *session = engine->session;
  RVS_Request *candidate = rvs_session_find_active_request_locked(session, request_id);
  if (candidate) {
    mutex_take(candidate->mutex);
    B32 matching_launch = candidate->command_kind == RVS_EngineCommandKind_Launch &&
                          (result != RVS_Result_Ok || candidate->launch_pid == pid);
    if (candidate->reply.result == RVS_Result_Pending && matching_launch) {
      if (result == RVS_Result_Ok && pid != 0 && !dmn_handle_match(process, dmn_handle_zero())) {
        RVS_Program *prog = push_array(session->program_arena, RVS_Program, 1);
        prog->arena = arena_alloc(.name = "Engine Program");
        prog->id    = process;
        prog->pid   = pid;
        prog->process = process;
        prog->state_epoch = 1;
        SLLQueuePush(session->first_program, session->last_program, prog);
        reply.launch.program_id = prog->id;
        reply.launch.pid        = prog->pid;
      } else if (result == RVS_Result_Ok) {
        reply.result = RVS_Result_Error;
      }
      rvs_session_prepare_reply_locked(session, candidate, &reply);
      rvs_session_request_remove_locked(session, candidate);
      request = candidate;
      completed = 1;
    }
    mutex_drop(candidate->mutex);
  }
  mutex_drop(engine->control->mutex);

  if (request) {
    rvs_request_complete(request, reply);
    rvs_request_release(request); // drop engine ownership
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
  RVS_Request *request = rvs_session_find_active_request_locked(engine->session, request_id);
  if (request) {
    mutex_take(request->mutex);
    result = request->command_kind == RVS_EngineCommandKind_Launch &&
             request->launch_pid != 0 && request->reply.result == RVS_Result_Pending;
    mutex_drop(request->mutex);
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
  RVS_Request *request = rvs_session_find_active_request_locked(engine->session, request_id);
  if (request) {
    mutex_take(request->mutex);
    result = request->command_kind == RVS_EngineCommandKind_Launch &&
             request->launch_pid == pid && request->reply.result == RVS_Result_Pending;
    mutex_drop(request->mutex);
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
  RVS_Request *request = rvs_session_find_active_request_locked(engine->session, request_id);
  if (request) {
    mutex_take(request->mutex);
    if (request->command_kind == RVS_EngineCommandKind_Launch && request->reply.result == RVS_Result_Pending) {
      result = request->launch_pid;
    }
    mutex_drop(request->mutex);
  }
  mutex_drop(engine->control->mutex);
  ProfEnd();
  return result;
}

internal void
rvs_engine_pump_launch(RVS_Engine *engine, RVS_MessageID request_id)
{
  ProfBeginFunction();
  if (rvs_demon_send_message(engine->demon, (RVS_DemonMessage){
    .type       = RVS_DemonMessage_Pump,
    .request_id = request_id,
  }) != RVS_Result_Ok) {
    rvs_engine_complete_launch(engine, request_id, RVS_Result_Error, 0, dmn_handle_zero());
  }
  ProfEnd();
}

////////////////////////////////
// Events

internal RVS_Result
rvs_session_push_event(RVS_Session *session, RVS_Event *event)
{
  ProfBeginFunction();
  RVS_EngineEventMessage *message = rvs_queue_alloc_struct(session->event_queue, RVS_EngineEventMessage);
  RVS_Result result = RVS_Result_EngineStopped;
  if (message) {
    rvs_demon_event_copy(session->arena, &message->event, event);
    result = rvs_queue_push(session->event_queue, &message->base);
  }
  ProfEnd();
  return result;
}

////////////////////////////////
// DEMON Output

internal void
rvs_engine_recycle_demon_output_arena(RVS_Engine *engine, ArenaNode *arena_node)
{
  ProfBeginFunction();
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
  ProfEnd();
}

internal RVS_Result
rvs_engine_push_demon_output(RVS_Engine *engine, RVS_Demon *source, RVS_DemonOutput *output)
{
  ProfBeginFunction();
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

  RVS_EngineMessage message = {
    .type = RVS_EngineMessageType_DemonOutput,
    .demon_output = {
      .source     = source,
      .output     = output_copy,
      .arena_node = arena_node,
    },
  };
  RVS_Result result = rvs_engine_send_message(engine, &message);

  // on failure, put resources on the free lists
  if (result != RVS_Result_Ok) {
    AssertAlways(engine->demon_output_arena_active_list == arena_node);
    SLLStackPop(engine->demon_output_arena_active_list);
    arena_clear(arena_node->v);
    SLLStackPush(engine->demon_output_arena_free_list, arena_node);
  }

  mutex_drop(engine->demon_output_mutex);
  ProfEnd();
  return result;
}

internal void
rvs_engine_demon_output_callback(RVS_Demon *demon, RVS_DemonOutput *output, void *ud)
{
  ProfBeginFunction();
  AssertAlways(rvs_engine_push_demon_output(ud, demon, output) == RVS_Result_Ok);
  ProfEnd();
}

////////////////////////////////
// Worker Dispatch

// Command and Output Dispatch

internal void
rvs_engine_process_command(RVS_Engine *engine, RVS_Session *session, RVS_MessageID request_id, RVS_EngineCommand *command)
{
  ProfBeginFunction();
  if ( ! rvs_engine_request_mark_dispatched(engine, request_id)) {
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

    RVS_Result result = rvs_demon_send_message(engine->demon, spec);

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
    B32 all_programs_found = command->run.programs_count != 0;
    DMN_Handle *processes = push_array(scratch.arena, DMN_Handle, command->run.programs_count);
    for EachIndex(program_idx, command->run.programs_count) {
      RVS_Program *found = 0;
      for EachNode(program, RVS_Program, session->first_program) {
        if (dmn_handle_match(program->id, command->run.programs[program_idx])) {
          found = program;
          break;
        }
      }
      if (found == 0 || dmn_handle_match(found->process, dmn_handle_zero())) {
        all_programs_found = 0;
        break;
      }
      processes[program_idx] = found->process;
    }

    RVS_Result result = RVS_Result_Error;
    if (all_programs_found) {
      result = rvs_demon_send_message(engine->demon, (RVS_DemonMessage){
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
rvs_engine_process_demon_output(RVS_Engine *engine, RVS_DemonOutput *output)
{
  ProfBeginFunction();
  switch (output->kind) {
  case RVS_DemonOutputKind_LaunchStarted: {
    if (rvs_engine_begin_launch(engine, output->request_id, output->launch_started.pid)) {
      rvs_engine_pump_launch(engine, output->request_id);
    } else {
      rvs_engine_complete_launch(engine, output->request_id, RVS_Result_Error, 0, dmn_handle_zero());
    }
  } break;

  case RVS_DemonOutputKind_Reply: {
    switch (output->reply.reply.kind) {
    case RVS_DemonReplyKind_Launch: {
      // A successful launch is completed only from a PID-matched CreateProcess event.
      RVS_Result result = output->reply.result == RVS_Result_Ok ? RVS_Result_Error : output->reply.result;
      rvs_engine_complete_launch(engine, output->request_id, result, 0, dmn_handle_zero());
    } break;
    case RVS_DemonReplyKind_Run: {
      rvs_engine_complete_reply(engine, (RVS_EngineReply){
        .request_id = output->request_id,
        .result     = output->reply.result,
        .kind       = RVS_EngineReplyKind_Run,
      });
    } break;
    default: { InvalidPath; } break;
    }
  } break;

  case RVS_DemonOutputKind_EventBatch: {
    DMN_Event *launch_event = 0;
    B32 launch_error = 0;
    B32 launch_exited = 0;
    U32 launch_pid = rvs_engine_launch_pid(engine, output->request_id);
    for EachNode(n, DMN_EventNode, output->event_batch.events.first) {
      if (n->v.kind == DMN_EventKind_Error) {
        launch_error |= n->v.error_kind == DMN_ErrorKind_NotAttached ||
                        (launch_pid != 0 && n->v.system_process_id == launch_pid);
      } else if (n->v.kind == DMN_EventKind_CreateProcess &&
                 rvs_engine_launch_matches_pid(engine, output->request_id, n->v.system_process_id)) {
        launch_event = &n->v;
      } else if (n->v.kind == DMN_EventKind_ExitProcess && launch_event &&
                 dmn_handle_match(n->v.process, launch_event->process)) {
        // DEMON event batches preserve platform order, so this exit belongs to the
        // target only after its matching CreateProcess event has established a handle.
        launch_exited = 1;
      }
    }
    if (launch_event && !launch_exited) {
      rvs_engine_complete_launch(engine, output->request_id, RVS_Result_Ok, launch_event->system_process_id, launch_event->process);
    } else if (launch_error || launch_exited) {
      rvs_engine_complete_launch(engine, output->request_id, RVS_Result_Error, 0, dmn_handle_zero());
    }
    B32 launch_failed = launch_exited || (launch_event == 0 && launch_error);
    for EachNode(n, DMN_EventNode, output->event_batch.events.first) {
      B32 suppress_event = launch_failed &&
                           ((launch_event && dmn_handle_match(n->v.process, launch_event->process)) ||
                            (launch_pid != 0 && n->v.system_process_id == launch_pid));
      if (suppress_event) { continue; }
      RVS_Result push_result = rvs_session_push_event(engine->session, &n->v);
      AssertAlways(push_result == RVS_Result_Ok || push_result == RVS_Result_EngineStopped);
    }
    if (rvs_engine_launch_is_pending(engine, output->request_id)) {
      rvs_engine_pump_launch(engine, output->request_id);
    }
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

    case RVS_EngineMessageType_DemonOutput: {
      rvs_engine_process_demon_output(engine, message->demon_output.output);
      rvs_engine_recycle_demon_output_arena(engine, message->demon_output.arena_node);
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
  engine.demon_output_arena = arena_alloc(.name = "Engine DEMON Output Nodes");
  engine.demon_output_mutex = mutex_alloc();

  result = rvs_demon_init(&engine, rvs_engine_demon_output_callback, &engine.demon);
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

  ins_atomic_u32_eval_assign(&engine.state, RVS_ThreadState_Running);
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
  if (engine->control->is_shutdown || ins_atomic_u32_eval(&engine->state) != RVS_ThreadState_Running) {
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
  if (ins_atomic_u32_eval_cond_assign(&engine->state, RVS_ThreadState_Terminating, RVS_ThreadState_Running) != RVS_ThreadState_Running) {
    goto exit;
  }

  mutex_take(engine->control->mutex);
  engine->control->is_shutdown = 1;
  if (engine->session) {
    rvs_queue_close(engine->session->event_queue);
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
  mutex_take(engine->demon_output_mutex);
  AssertAlways(engine->demon_output_arena_active_list == 0);
  for EachNode(n, ArenaNode, engine->demon_output_arena_free_list) { arena_release(n->v); }
  engine->demon_output_arena_free_list = 0;
  mutex_drop(engine->demon_output_mutex);

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
  mutex_release(engine->demon_output_mutex);
  arena_release(engine->demon_output_arena);
  arena_release(engine->arena);
  MemoryZeroStruct(engine);

  exit:;
  ProfEnd();
}

// Public APIs and test helpers construct a complete scheduling submission before calling this.
internal RVS_Result
rvs_session_submit(RVS_Session *session, RVS_EngineSubmission submission, RVS_SubmitInfo *submit_out)
{
  ProfBeginFunction();
  RVS_Result result = RVS_Result_Error;
  RVS_RequestPolicy policy = submission.policy;
  RVS_OperationKey key = submission.key;

  AssertAlways(session != 0);
  AssertAlways(submit_out != 0);
  AssertAlways(rvs_operation_key_is_well_formed_for_policy(policy, key));

  MemoryZeroStruct(submit_out);
  mutex_take(session->control->mutex);
  if (session->control->is_shutdown || session->engine_released || session->engine == 0 ||
      ins_atomic_u32_eval(&session->engine->state) != RVS_ThreadState_Running) {
    result = RVS_Result_EngineStopped;
    goto exit_control_mutex;
  }
  RVS_Engine *engine = session->engine;
  mutex_take(engine->arena_mutex);

  if ( ! rvs_session_operation_key_resolves_locked(session, key)) {
    goto exit_arena_mutex;
  }
  U64 captured_program_state_epoch = 0;
  if (key.operation_class == RVS_OperationClass_ReadOnly) {
    captured_program_state_epoch = rvs_session_program_state_epoch_locked(session, key.program_id);
  }
  if (policy == RVS_RequestPolicy_JoinIfEqual) {
    for (RVS_Request *n = session->key_first; n; n = n->key_next) {
      if (rvs_operation_key_match(n->key, key) &&
          (key.operation_class != RVS_OperationClass_ReadOnly ||
           n->captured_program_state_epoch == captured_program_state_epoch)) {
        rvs_request_retain(n);
        submit_out->request = n;
        result = RVS_Result_Ok;
        break;
      }
    }
  }
  if (result == RVS_Result_Error && rvs_session_has_conflicting_operation_locked(session, key)) {
    result = RVS_Result_AlreadyPending;
  }

  RVS_Request *request = 0;
  if (result == RVS_Result_Error) {
    request = rvs_session_request_alloc_locked(session, policy, key);
    request->command_kind = submission.command.kind;
    request->captured_program_state_epoch = captured_program_state_epoch;
    if (policy == RVS_RequestPolicy_JoinIfEqual) {
      AssertAlways(rvs_session_register_operation_locked(session, key, request) == RVS_Result_Ok);
    }
  }
  if (result == RVS_Result_Ok || result == RVS_Result_AlreadyPending) {
    goto exit_arena_mutex;
  }

  RVS_EngineMessage message = {
    .type    = RVS_EngineMessageType_Command,
    .session = session,
    .command = submission.command,
  };
  message.request_id = request->request_id;

  result = rvs_engine_send_message_locked(engine, &message);

  if (result == RVS_Result_Ok) {
    submit_out->request = request;
    submit_out->control = rvs_request_control_alloc(session, request, key, policy == RVS_RequestPolicy_JoinIfEqual);
  } else {
    rvs_session_request_remove_locked(session, request);
    RVS_Request *registered_request = 0;
    if (policy == RVS_RequestPolicy_JoinIfEqual) {
      registered_request = rvs_session_unregister_operation_locked(session, key);
      AssertAlways(registered_request == request);
    }
    rvs_request_release(request); // drop engine ownership
    rvs_request_release(request); // drop the unreturned caller ownership
    if (registered_request) {
      rvs_request_release(registered_request); // drop operation-key ownership
    }
  }
  exit_arena_mutex:;
  mutex_drop(engine->arena_mutex);

  exit_control_mutex:;
  mutex_drop(session->control->mutex);

  exit:;
  ProfEnd();
  return result;
}

RVS_Result
rvs_session_launch(RVS_Session *session, String8 cmdl, String8 wdir, RVS_SubmitInfo *submit_out)
{
  ProfBeginFunction();
  RVS_Result result = RVS_Result_Error;
  if (submit_out) { MemoryZeroStruct(submit_out); }
  if (session == 0 || submit_out == 0) {
    goto exit;
  }
  {
    Temp scratch = scratch_begin(0, 0);
    String8List cmd_line = str8_split_by_string_chars(scratch.arena, cmdl, str8_lit(" "), 0);
    RVS_EngineCommand command = {
      .kind   = RVS_EngineCommandKind_Launch,
      .launch = { .params = { .cmd_line = cmd_line, .path = wdir } },
    };
    RVS_EngineSubmission submission = {0};
    AssertAlways(rvs_engine_submission_from_fixed_command(command, RVS_RequestPolicy_RejectIfPending, &submission));
    result = rvs_session_submit(session, submission, submit_out);
    scratch_end(scratch);
  }

  exit:;
  ProfEnd();
  return result;
}

void
rvs_session_retain(RVS_Session *session)
{
  AssertAlways(session != 0);
  ins_atomic_u32_inc_eval(&session->ref_count);
}

void
rvs_session_release(RVS_Session *session)
{
  if (session) {
    rvs_session_release_ref(session);
  }
}

void
rvs_request_retain(RVS_Request *request)
{
  ProfBeginFunction();
  AssertAlways(request != 0);
  ins_atomic_u32_inc_eval(&request->ref_count);
  ProfEnd();
}

void
rvs_request_release(RVS_Request *request)
{
  ProfBeginFunction();
  AssertAlways(request != 0);
  if (ins_atomic_u32_dec_eval(&request->ref_count) == 0) {
    rvs_request_pool_request_release(request);
  }
  ProfEnd();
}

RVS_Result
rvs_session_run(RVS_Session *session, RVS_ProgramID program_id, RVS_SubmitOptions options, RVS_SubmitInfo *submit_out)
{
  ProfBeginFunction();
  RVS_Result result = RVS_Result_Error;
  if (submit_out) { MemoryZeroStruct(submit_out); }
  if (session == 0 || submit_out == 0) {
    goto exit;
  }
  RVS_OperationKey key = options.key;
  if (options.policy == RVS_RequestPolicy_Independent && key.operation_class == RVS_OperationClass_Null) {
    key = (RVS_OperationKey){
      .operation_class = RVS_OperationClass_ProgramExecution,
      .program_id      = program_id,
    };
  }
  if (key.operation_class != RVS_OperationClass_ProgramExecution ||
      !dmn_handle_match(key.program_id, program_id) ||
      !rvs_operation_key_is_well_formed_for_policy(options.policy, key)) {
    goto exit;
  }
  RVS_EngineCommand command = {
    .kind = RVS_EngineCommandKind_Run,
    .run  = {
      .programs_count = 1,
      .programs       = &program_id,
    },
  };
  RVS_EngineSubmission submission = {
    .command = command,
    .policy  = options.policy,
    .key     = key,
  };
  result = rvs_session_submit(session, submission, submit_out);

  exit:;
  ProfEnd();
  return result;
}

RVS_Result
rvs_session_wait_for_event(Arena *arena, RVS_Session *session, U64 wait_us, RVS_Event *event_out)
{
  ProfBeginFunction();

  RVS_Result result = RVS_Result_Error;
  if (session == 0) {
    goto exit;
  }
  rvs_session_retain(session);
  mutex_take(session->control->mutex);
  B32 is_stopped = session->control->is_shutdown || session->engine_released;
  RVS_Queue *event_queue = session->event_queue;
  mutex_drop(session->control->mutex);
  if (is_stopped) {
    result = RVS_Result_EngineStopped;
    goto exit_session;
  }

  RVS_EngineEventMessage *message = rvs_queue_pop_struct(event_queue, RVS_EngineEventMessage, wait_us);
  mutex_take(session->control->mutex);
  is_stopped = session->control->is_shutdown || session->engine_released;
  mutex_drop(session->control->mutex);
  if (message) {
    if ( ! is_stopped) {
      rvs_demon_event_copy(arena, event_out, &message->event);
      result = RVS_Result_Ok;
    }
    rvs_queue_recycle(event_queue, &message->base);
  }
  if (result != RVS_Result_Ok) {
    if (is_stopped) {
      result = RVS_Result_EngineStopped;
    } else {
      result = RVS_Result_Timeout;
    }
  }

  exit_session:;
  rvs_session_release(session);
  exit:;
  ProfEnd();
  return result;
}

RVS_Result
rvs_request_wait(RVS_Request *request, U64 wait_us, RVS_EngineReply *reply_out)
{
  ProfBeginFunction();
  RVS_Result result = RVS_Result_Error;
  if (request == 0) {
    goto exit;
  }

  U64 endt_us = max_U64;
  if (wait_us != max_U64) {
    U64 now_us = now_time_us();
    endt_us = now_us + Min(wait_us, max_U64 - now_us);
  }

  mutex_take(request->mutex);
  while (request->reply.result == RVS_Result_Pending) {
    if ( ! cond_var_wait(request->cv, request->mutex, endt_us)) {
      result = RVS_Result_Timeout;
      goto exit_request_mutex;
    }
  }

  if (reply_out) {
    *reply_out = request->reply;
  }
  result = RVS_Result_Ok;

  exit_request_mutex:;
  mutex_drop(request->mutex);

  exit:;
  ProfEnd();
  return result;
}

void
rvs_request_control_release(RVS_RequestControl *owner)
{
  ProfBeginFunction();
  if (owner == 0) {
    goto exit;
  }

  RVS_Request *registered_request = 0;
  mutex_take(owner->control->mutex);
  if (owner->registered && !owner->control->is_shutdown) {
    registered_request = rvs_session_unregister_operation_locked(owner->session, owner->key);
  }
  mutex_drop(owner->control->mutex);
  if (registered_request) { rvs_request_release(registered_request); }

  rvs_request_release(owner->request);
  rvs_engine_control_release(owner->control);
  arena_release(owner->arena);

  exit:;
  ProfEnd();
}

RVS_Result
rvs_request_control_cancel(RVS_RequestControl *owner)
{
  ProfBeginFunction();
  RVS_Result result = RVS_Result_Error;
  if (owner == 0 || owner->request == 0 || owner->session == 0) {
    goto exit;
  }

  mutex_take(owner->control->mutex);
  if (owner->control->is_shutdown) {
    result = RVS_Result_EngineStopped;
    goto exit_control_mutex;
  }
  RVS_Request *request = rvs_session_find_active_request_locked(owner->session, owner->request->request_id);
  if (request == owner->request) {
    mutex_take(request->mutex);
    if (request->is_dispatched) {
      result = RVS_Result_Unsupported;
    } else if (request->reply.result == RVS_Result_Pending) {
      request->reply = (RVS_EngineReply){
      .request_id = request->request_id,
      .result     = RVS_Result_Cancelled,
      .kind       = request->reply.kind,
      };
      cond_var_broadcast(request->cv);
      result = RVS_Result_Ok;
    }
    mutex_drop(request->mutex);
  }
  exit_control_mutex:;
  mutex_drop(owner->control->mutex);

  exit:;
  ProfEnd();
  return result;
}
