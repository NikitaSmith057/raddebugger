// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#include "radvs_bridge.h"
#include "radvs_demon.h"
#include "radvs_engine.h"
#include "radvs_symbol_service.h"
#include "radvs_tasker.h"

typedef enum RADVS_AD7_State
{
  RADVS_AD7_State_AwaitProgramCreate,
  RADVS_AD7_State_Starting,
  RADVS_AD7_State_Running,
  RADVS_AD7_State_Stopped,
  RADVS_AD7_State_Terminating,
  RADVS_AD7_State_AwaitProgramDestroyAck,
  RADVS_AD7_State_Closed,
} RADVS_AD7_State;

struct RADVS_Session
{
  Arena                *arena;
  Arena                *event_arena;

  U64                   engine_sessions_count;
  RADVS_EngineSession  *engine_session;

  Mutex                 mutex;
  CondVar               closing_cv;
  U32                   in_flight_calls;
  B32                   closing;
  B32                   event_wait_active;

  RADVS_AD7_State       ad7_state;
  U64                   next_event_sequence;
  DMN_Handle            stopped_thread_handle;
  U64                   pending_destroy_sequence;
  DMN_Handle            program_process_handle;
  U32                   program_system_process_id;
  B32                   event_batch_received;
  B32                   load_complete_sent;
  B32                   pending_event_valid;
  B32                   deferred_event_valid;
  RADVS_AD7_Event       pending_event;
  RADVS_AD7_Event       deferred_event;
  String8               pending_text;
  String8               program_name;
};

typedef enum RADVS_BridgeState
{
  RADVS_BridgeState_Uninitialized,
  RADVS_BridgeState_Ready,
  RADVS_BridgeState_Releasing,
} RADVS_BridgeState;

static U32 radvs_bridge_state = RADVS_BridgeState_Uninitialized;
static U32 radvs_bridge_lifecycle_gate = 0;
static U64 radvs_bridge_session_count = 0;
static RADVS_SymbolService *radvs_bridge_symbol_service = 0;
static RADVS_Engine *radvs_bridge_engine = 0;

//
// HResult Error Code
//

#define RADVS_HRESULT_S_OK    ((int32_t)0x00000000)
#define RADVS_HRESULT_S_FALSE ((int32_t)0x00000001)

#define RADVS_HRESULT_SEVERITY(hr) (((hr) >> 31) & 1)

#define RADVS_HRESULT_E_NOTIMPL         ((int32_t)0x80004001u)
#define RADVS_HRESULT_E_NOINTERFACE     ((int32_t)0x80004002u)
#define RADVS_HRESULT_E_FAIL            ((int32_t)0x80004005u)
#define RADVS_HRESULT_E_OUTOFMEMORY     ((int32_t)0x8007000Eu)
#define RADVS_HRESULT_E_INVALIDARG      ((int32_t)0x80070057u)

#define RADVS_HRESULT_FROM_WIN32(error) ((int32_t)(0x80070000u | ((error) & 0xffffu)))

#define RADVS_HRESULT_ERROR_BUSY           RADVS_HRESULT_FROM_WIN32(170u)
#define RADVS_HRESULT_ERROR_TIMEOUT        RADVS_HRESULT_FROM_WIN32(1460u)
#define RADVS_HRESULT_ERROR_ALREADY_EXISTS RADVS_HRESULT_FROM_WIN32(183u)
#define RADVS_HRESULT_INSUFFICIENT_BUFFER  RADVS_HRESULT_FROM_WIN32(122u)

internal int32_t
radvs_hresult_from_result(RADVS_Result result)
{
  switch (result) {
  case RADVS_Result_Null:            return 0;
  case RADVS_Result_Ok:              return RADVS_HRESULT_S_OK;
  case RADVS_Result_InvalidArgument: return RADVS_HRESULT_E_INVALIDARG;
  case RADVS_Result_OutOfMemory:     return RADVS_HRESULT_E_OUTOFMEMORY;
  case RADVS_Result_NotImplemented:  return RADVS_HRESULT_E_NOTIMPL;
  case RADVS_Result_AbiMismatch:     return RADVS_HRESULT_E_NOINTERFACE;
  case RADVS_Result_Busy:            return RADVS_HRESULT_ERROR_BUSY;
  case RADVS_Result_Timeout:         return RADVS_HRESULT_ERROR_TIMEOUT;
  case RADVS_Result_Conflict:        return RADVS_HRESULT_ERROR_ALREADY_EXISTS;
  }
  return RADVS_HRESULT_E_FAIL;
}

internal int32_t
radvs_buffer_hresult_from_result(RADVS_Result result)
{
  return result == RADVS_Result_OutOfMemory ? RADVS_HRESULT_INSUFFICIENT_BUFFER : radvs_hresult_from_result(result);
}

internal void
radvs_bridge_lifecycle_gate_take(void)
{
  for (; ins_atomic_u32_eval_cond_assign(&radvs_bridge_lifecycle_gate, 1, 0) != 0;) {
    Sleep(0);
  }
}

internal void
radvs_bridge_lifecycle_gate_drop(void)
{
  ins_atomic_u32_eval_assign(&radvs_bridge_lifecycle_gate, 0);
}

internal RADVS_Result
radvs_bridge_start_locked(void)
{
  U32 state = ins_atomic_u32_eval(&radvs_bridge_state);
  if (state == RADVS_BridgeState_Ready) {
    return RADVS_Result_Ok;
  }
  if (state != RADVS_BridgeState_Uninitialized) {
    return RADVS_Result_Busy;
  }

  w32_base_init_system();
  w32_base_init_entities();
  RADVS_Result result = radvs_tasker_init();
  if (result == RADVS_Result_Ok) {
    result = radvs_symbol_service_init(&radvs_bridge_symbol_service);
  }
  if (result == RADVS_Result_Ok) {
    result = radvs_demon_init();
  }
  if (result == RADVS_Result_Ok) {
    result = radvs_engine_alloc(radvs_bridge_symbol_service, &radvs_bridge_engine);
  }
  if (result == RADVS_Result_Ok) {
    ins_atomic_u32_eval_assign(&radvs_bridge_state, RADVS_BridgeState_Ready);
  } else {
    radvs_engine_release(radvs_bridge_engine);
    radvs_bridge_engine = 0;
    radvs_tasker_release();
    radvs_symbol_service_release(radvs_bridge_symbol_service);
    radvs_bridge_symbol_service = 0;
    w32_base_release_entities();
  }
  return result;
}

internal RADVS_Result
radvs_bridge_release_locked(void)
{
  if (ins_atomic_u32_eval(&radvs_bridge_state) == RADVS_BridgeState_Uninitialized) {
    return RADVS_Result_Ok;
  }
  if (ins_atomic_u32_eval(&radvs_bridge_state) != RADVS_BridgeState_Ready || radvs_bridge_session_count != 0) {
    return RADVS_Result_Busy;
  }

  ins_atomic_u32_eval_assign(&radvs_bridge_state, RADVS_BridgeState_Releasing);
  RADVS_Result result = radvs_engine_release(radvs_bridge_engine);
  if (result == RADVS_Result_Ok) {
    radvs_bridge_engine = 0;
    result = radvs_demon_shutdown();
  }
  if (result == RADVS_Result_Ok) {
    radvs_symbol_service_prepare_release(radvs_bridge_symbol_service);
    radvs_tasker_release();
    radvs_symbol_service_release(radvs_bridge_symbol_service);
    radvs_bridge_symbol_service = 0;
    w32_base_release_entities();
    ins_atomic_u32_eval_assign(&radvs_bridge_state, RADVS_BridgeState_Uninitialized);
  } else {
    ins_atomic_u32_eval_assign(&radvs_bridge_state, RADVS_BridgeState_Ready);
  }
  return result;
}

internal RADVS_Result
radvs_session_enter(RADVS_Session *session, B32 event_wait)
{
  if (session == 0) {
    return RADVS_Result_InvalidArgument;
  }

  //
  // 
  //

  RADVS_Result result = RADVS_Result_Busy;
  MutexScope (session->mutex) {
    if (!session->closing && (!event_wait || !session->event_wait_active)) {
      //
      //
      //
      session->in_flight_calls += 1;

      if (event_wait) {
        session->event_wait_active = 1;
      }

      result = RADVS_Result_Ok;
    }
  }
  return result;
}

internal void
radvs_session_leave(RADVS_Session *session, B32 event_wait)
{
  MutexScope (session->mutex) {
    if (event_wait) {
      session->event_wait_active = 0;
    }
    Assert(session->in_flight_calls != 0);
    session->in_flight_calls -= 1;
    if (session->closing && session->in_flight_calls == 0) {
      cond_var_broadcast(session->closing_cv);
    }
  }
}

internal RADVS_Result
radvs_session_create(RADVS_Session **out_session)
{
  if (out_session == 0) {
    return RADVS_Result_InvalidArgument;
  }
  *out_session = 0;

  radvs_bridge_lifecycle_gate_take();
  RADVS_Result result = radvs_bridge_start_locked();
  if (result == RADVS_Result_Ok) {
    radvs_bridge_session_count += 1;
  }
  radvs_bridge_lifecycle_gate_drop();
  if (result != RADVS_Result_Ok) {
    return result;
  }

  Arena *arena = arena_alloc(.reserve_size = KB(64), .commit_size = KB(16));
  RADVS_Session *session = push_array(arena, RADVS_Session, 1);
  session->arena = arena;
  session->event_arena = arena_alloc(.reserve_size = KB(64), .commit_size = KB(16));
  session->mutex = mutex_alloc();
  session->closing_cv = cond_var_alloc();
  session->ad7_state = RADVS_AD7_State_AwaitProgramCreate;
  result = radvs_engine_session_alloc(radvs_bridge_engine, &session->engine_session);
  if (result != RADVS_Result_Ok) {
    radvs_engine_session_release(session->engine_session);
    cond_var_release(session->closing_cv);
    mutex_release(session->mutex);
    arena_release(session->event_arena);
    arena_release(arena);
    radvs_bridge_lifecycle_gate_take();
    radvs_bridge_session_count -= 1;
    if (radvs_bridge_session_count == 0) {
      radvs_bridge_release_locked();
    }
    radvs_bridge_lifecycle_gate_drop();
    return result;
  }

  *out_session = session;
  return RADVS_Result_Ok;
}

internal void
radvs_session_destroy(RADVS_Session *session)
{
  if (session == 0) {
    return;
  }

  mutex_take(session->mutex);
  if (session->closing) {
    mutex_drop(session->mutex);
    return;
  }
  session->closing = 1;
  mutex_drop(session->mutex);

  // Wake the one native event consumer before waiting for its bridge call to drain.
  radvs_engine_session_close_event_wait(session->engine_session);
  mutex_take(session->mutex);
  for (; session->in_flight_calls != 0;) {
    cond_var_wait(session->closing_cv, session->mutex, max_U64);
  }
  mutex_drop(session->mutex);

  radvs_engine_session_release(session->engine_session);
  cond_var_release(session->closing_cv);
  mutex_release(session->mutex);
  arena_release(session->event_arena);
  arena_release(session->arena);

  radvs_bridge_lifecycle_gate_take();
  Assert(radvs_bridge_session_count != 0);
  radvs_bridge_session_count -= 1;
  if (radvs_bridge_session_count == 0) {
    radvs_bridge_release_locked();
  }
  radvs_bridge_lifecycle_gate_drop();
}

uint32_t
radvs_bridge_abi_version(void)
{
  return RADVS_BRIDGE_ABI_VERSION;
}

#if !defined(BRIDGE_NO_A7_COM_INTERFACE)

int32_t
BRIDGE_FN(IDebugEngineLaunch2_LaunchSuspended)(const wchar_t *exe, const wchar_t *cmd_line, const wchar_t *wdir, RADVS_Session **out_session, uint32_t *out_system_pid)
{
  if (exe == 0 || out_session == 0 || out_system_pid == 0) {
    return RADVS_HRESULT_E_INVALIDARG;
  }
  *out_session = 0;
  *out_system_pid = 0;
  RADVS_Session *session = 0;
  RADVS_Result result = radvs_session_create(&session);
  if (result == RADVS_Result_Ok) {
    String16 exe16 = str16_cstring((U16 *)exe);
    String16 cmd_line16 = cmd_line ? str16_cstring((U16 *)cmd_line) : str16_zero();
    String16 wdir16 = wdir ? str16_cstring((U16 *)wdir) : str16_zero();
    MutexScope (session->mutex) {
      if (session->ad7_state != RADVS_AD7_State_AwaitProgramCreate) {
        result = RADVS_Result_Busy;
      }
    }
    if (result == RADVS_Result_Ok) {
      result = radvs_engine_session_launch16(session->engine_session, exe16, cmd_line16, wdir16, out_system_pid);
    }
    if (result == RADVS_Result_Ok) {
      MutexScope (session->mutex) {
        session->program_system_process_id = *out_system_pid;
        session->program_name = str8_from_16(session->arena, exe16);
      }
    }
  }
  if (result != RADVS_Result_Ok) {
    radvs_session_destroy(session);
    return radvs_hresult_from_result(result);
  }
  *out_session = session;
  return RADVS_HRESULT_S_OK;
}

void
BRIDGE_FN(IDebugEngine2_DestroySession)(RADVS_Session *session)
{
  radvs_session_destroy(session);
}

int32_t
BRIDGE_FN(IDebugEngine2_ContinueFromSynchronousEvent)(RADVS_Session *session)
{
  RADVS_Result result = radvs_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    MutexScope (session->mutex) {
      if (session->ad7_state != RADVS_AD7_State_AwaitProgramCreate) {
        result = RADVS_Result_Busy;
      } else {
        session->ad7_state = RADVS_AD7_State_Starting;
      }
    }
    if (result == RADVS_Result_Ok) {
      result = radvs_engine_session_run(session->engine_session, 0, 0);
    }
    if (result != RADVS_Result_Ok) {
      MutexScope (session->mutex) {
        if (session->ad7_state == RADVS_AD7_State_Starting) {
          session->ad7_state = RADVS_AD7_State_AwaitProgramCreate;
        }
      }
    }
    radvs_session_leave(session, 0);
  }
  return radvs_hresult_from_result(result);
}

int32_t
BRIDGE_FN(IDebugEngine2_CreateBreakpoint)(RADVS_Session *session, const wchar_t *source_path, uint32_t line, uint32_t column, uint64_t *out_breakpoint_id)
{
  if (!session || !out_breakpoint_id) {
    return RADVS_HRESULT_E_INVALIDARG;
  }
  *out_breakpoint_id = 0;
  RADVS_Result result = 0;
  BRIDGE_NOT_IMPLEMENTED(result);
  return radvs_hresult_from_result(result);
}

int32_t
BRIDGE_FN(IDebugEngine2_CreateAddressBreakpoint)(RADVS_Session *session, uint64_t address, uint32_t enabled, uint32_t address_mode, uint64_t *out_breakpoint_id)
{
  if (!session || !out_breakpoint_id) {
    return RADVS_HRESULT_E_INVALIDARG;
  }
  *out_breakpoint_id = 0;
  RADVS_Result result = radvs_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    RADVS_BreakpointSpec spec = {
      .location_kind  = RADVS_BreakpointLocationKind_Address,
      .condition_kind = RADVS_BreakpointConditionKind_Always,
      .enabled        = enabled != 0,
      .address_mode   = address_mode,
      .address        = address,
    };
    result = radvs_engine_session_create_breakpoint(session->engine_session, &spec, out_breakpoint_id);
    radvs_session_leave(session, 0);
  }
  return radvs_hresult_from_result(result);
}

int32_t
BRIDGE_FN(IDebugEngine2_SetBreakpointEnabled)(RADVS_Session *session, uint64_t breakpoint_id, uint32_t enabled)
{
  RADVS_Result result = radvs_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    result = radvs_engine_session_set_breakpoint_enabled(session->engine_session, breakpoint_id, enabled);
    radvs_session_leave(session, 0);
  }
  return radvs_hresult_from_result(result);
}

int32_t
BRIDGE_FN(IDebugEngine2_DeleteBreakpoint)(RADVS_Session *session, uint64_t breakpoint_id)
{
  RADVS_Result result = radvs_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    result = radvs_engine_session_remove_breakpoint(session->engine_session, breakpoint_id);
    radvs_session_leave(session, 0);
  }
  return radvs_hresult_from_result(result);
}

int32_t
BRIDGE_FN(IDebugProgram2_GetDescriptor)(RADVS_Session *session, RADVS_AD7_ProgramDesc *out_desc, char *text_buffer, uint64_t text_buffer_size, uint64_t *out_text_size)
{
  if (out_desc == 0 || out_text_size == 0 || (text_buffer == 0 && text_buffer_size != 0)) {
    return RADVS_HRESULT_E_INVALIDARG;
  }
  RADVS_Result result = radvs_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    String8 host_name = get_system_info()->machine_name;
    if (host_name.size == 0) {
      result = RADVS_Result_InvalidArgument;
    } else {
      String8 engine_name = str8_lit("RAD Debug Engine");
      String8 engine_id = str8_lit("97bd1aec-93d9-4748-b28d-e7e8eca0f781");
      MutexScope (session->mutex) {
        if (session->program_system_process_id == 0) {
          result = RADVS_Result_Busy;
        } else {
          String8 program_name = session->program_name.size ? session->program_name : str8_lit("RAD program");
          U64 text_size = program_name.size + host_name.size + engine_name.size + engine_id.size;
          *out_desc = (RADVS_AD7_ProgramDesc){
            .process_handle        = session->program_process_handle,
            .parent_process_handle = dmn_handle_zero(),
            .system_process_id     = session->program_system_process_id,
            .state                 = session->ad7_state,
            .program_name_offset   = 0,
            .program_name_size     = program_name.size,
            .host_name_offset      = program_name.size,
            .host_name_size        = host_name.size,
            .engine_name_offset    = program_name.size + host_name.size,
            .engine_name_size      = engine_name.size,
            .engine_id_offset      = program_name.size + host_name.size + engine_name.size,
            .engine_id_size        = engine_id.size,
          };
          *out_text_size = text_size;
          if (text_size != 0 && (text_buffer == 0 || text_buffer_size < text_size)) {
            result = RADVS_Result_OutOfMemory;
          } else if (text_size != 0) {
            MemoryCopy(text_buffer + out_desc->program_name_offset, program_name.str, program_name.size);
            MemoryCopy(text_buffer + out_desc->host_name_offset, host_name.str, host_name.size);
            MemoryCopy(text_buffer + out_desc->engine_name_offset, engine_name.str, engine_name.size);
            MemoryCopy(text_buffer + out_desc->engine_id_offset, engine_id.str, engine_id.size);
          }
        }
      }
    }
    radvs_session_leave(session, 0);
  }
  return radvs_buffer_hresult_from_result(result);
}

int32_t
BRIDGE_FN(IDebugProgram2_GetDescriptorForProcess)(RADVS_Session *session, DMN_Handle process_handle, RADVS_AD7_ProgramDesc *out_desc, char *text_buffer, uint64_t text_buffer_size, uint64_t *out_text_size)
{
  if (MemoryIsZeroStruct(&process_handle) || out_desc == 0 || out_text_size == 0 || (text_buffer == 0 && text_buffer_size != 0)) {
    return RADVS_HRESULT_E_INVALIDARG;
  }
  RADVS_Result result = radvs_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    RADVS_ProcessDesc process = {0};
    result = radvs_engine_session_get_process_desc(session->engine_session, process_handle, &process);
    if (result == RADVS_Result_Ok) {
      String8 host_name = get_system_info()->machine_name;
      if (host_name.size == 0) {
        result = RADVS_Result_InvalidArgument;
      } else {
        Temp scratch = scratch_begin(0, 0);
        String8 engine_name = str8_lit("RAD Debug Engine");
        String8 engine_id = str8_lit("97bd1aec-93d9-4748-b28d-e7e8eca0f781");
        String8 child_name = push_str8f(scratch.arena, "RAD process %u", process.system_process_id);
        MutexScope (session->mutex) {
          String8 program_name = dmn_handle_match(process.process_handle, session->program_process_handle) && session->program_name.size ? session->program_name : child_name;
          U64 text_size = program_name.size + host_name.size + engine_name.size + engine_id.size;
          *out_desc = (RADVS_AD7_ProgramDesc){
            .process_handle        = process.process_handle,
            .parent_process_handle = process.parent_process_handle,
            .system_process_id     = process.system_process_id,
            .state                 = process.state,
            .program_name_offset   = 0,
            .program_name_size     = program_name.size,
            .host_name_offset      = program_name.size,
            .host_name_size        = host_name.size,
            .engine_name_offset    = program_name.size + host_name.size,
            .engine_name_size      = engine_name.size,
            .engine_id_offset      = program_name.size + host_name.size + engine_name.size,
            .engine_id_size        = engine_id.size,
          };
          *out_text_size = text_size;
          if (text_size != 0 && (text_buffer == 0 || text_buffer_size < text_size)) {
            result = RADVS_Result_OutOfMemory;
          } else if (text_size != 0) {
            MemoryCopy(text_buffer + out_desc->program_name_offset, program_name.str, program_name.size);
            MemoryCopy(text_buffer + out_desc->host_name_offset, host_name.str, host_name.size);
            MemoryCopy(text_buffer + out_desc->engine_name_offset, engine_name.str, engine_name.size);
            MemoryCopy(text_buffer + out_desc->engine_id_offset, engine_id.str, engine_id.size);
          }
        }
        scratch_end(scratch);
      }
    }
    radvs_session_leave(session, 0);
  }
  return radvs_buffer_hresult_from_result(result);
}

int32_t
BRIDGE_FN(IDebugProgram2_CopyThreads)(RADVS_Session *session, RADVS_ThreadDesc *buffer, uint64_t buffer_count, uint64_t *out_count)
{
  if (out_count == 0 || (buffer == 0 && buffer_count != 0)) {
    return RADVS_HRESULT_E_INVALIDARG;
  }
  RADVS_Result result = radvs_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    result = radvs_engine_session_copy_threads(session->engine_session, buffer, buffer_count, out_count);
    radvs_session_leave(session, 0);
  }
  return radvs_buffer_hresult_from_result(result);
}

int32_t
BRIDGE_FN(IDebugProgram2_CopyModules)(RADVS_Session *session, RADVS_ModuleDesc *buffer, uint64_t buffer_count, uint64_t *out_count)
{
  if (out_count == 0 || (buffer == 0 && buffer_count != 0)) {
    return RADVS_HRESULT_E_INVALIDARG;
  }
  RADVS_Result result = radvs_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    result = radvs_engine_session_copy_modules(session->engine_session, buffer, buffer_count, out_count);
    radvs_session_leave(session, 0);
  }
  return radvs_buffer_hresult_from_result(result);
}

int32_t
BRIDGE_FN(IDebugProgram2_ResolveSourcePosition)(RADVS_Session *session, const wchar_t *source_path, uint32_t line, uint32_t column)
{
  RADVS_Result result = 0;
  BRIDGE_NOT_IMPLEMENTED(result);
  return radvs_hresult_from_result(result);
}

int32_t
BRIDGE_FN(IDebugProgram2_Continue)(RADVS_Session *session, DMN_Handle thread_handle)
{
  RADVS_Result result = radvs_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    DMN_Handle stopped_thread_handle = {0};
    MutexScope (session->mutex) {
      if (session->ad7_state != RADVS_AD7_State_Stopped) {
        result = RADVS_Result_Busy;
      } else if (!MemoryIsZeroStruct(&thread_handle) && !dmn_handle_match(thread_handle, session->stopped_thread_handle)) {
        result = RADVS_Result_InvalidArgument;
      } else {
        stopped_thread_handle = session->stopped_thread_handle;
        session->ad7_state = RADVS_AD7_State_Running;
        session->stopped_thread_handle = dmn_handle_zero();
        session->event_batch_received = 0;
      }
    }
    if (result == RADVS_Result_Ok) {
      result = radvs_engine_session_run_thread(session->engine_session, stopped_thread_handle);
    }
    if (result != RADVS_Result_Ok) {
      MutexScope (session->mutex) {
        if (session->ad7_state == RADVS_AD7_State_Running) {
          session->ad7_state = RADVS_AD7_State_Stopped;
          session->stopped_thread_handle = stopped_thread_handle;
        }
      }
    }
    radvs_session_leave(session, 0);
  }
  return radvs_hresult_from_result(result);
}

int32_t
BRIDGE_FN(IDebugProgram2_CauseBreak)(RADVS_Session *session)
{
  RADVS_Result result = radvs_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    MutexScope (session->mutex) {
      if (session->ad7_state != RADVS_AD7_State_Starting && session->ad7_state != RADVS_AD7_State_Running) {
        result = RADVS_Result_Busy;
      }
    }
    if (result == RADVS_Result_Ok) {
      result = radvs_engine_session_break(session->engine_session);
    }
    radvs_session_leave(session, 0);
  }
  return radvs_hresult_from_result(result);
}

int32_t
BRIDGE_FN(IDebugProcess2_Terminate)(RADVS_Session *session, DMN_Handle process_handle)
{
  RADVS_Result result = radvs_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    RADVS_AD7_State previous_state = RADVS_AD7_State_AwaitProgramCreate;
    MutexScope (session->mutex) {
      if (session->ad7_state == RADVS_AD7_State_Closed || session->ad7_state == RADVS_AD7_State_AwaitProgramDestroyAck) {
        result = RADVS_Result_Busy;
      } else {
        if (MemoryIsZeroStruct(&process_handle)) {
          process_handle = session->program_process_handle;
        }
        previous_state = session->ad7_state;
        session->ad7_state = RADVS_AD7_State_Terminating;
      }
    }
    if (result == RADVS_Result_Ok) {
      result = !MemoryIsZeroStruct(&process_handle) ? radvs_engine_session_terminate_process(session->engine_session, process_handle) :
                                                      radvs_engine_session_terminate(session->engine_session, 0, 0);
    }
    if (result != RADVS_Result_Ok) {
      MutexScope (session->mutex) {
        if (session->ad7_state == RADVS_AD7_State_Terminating) {
          session->ad7_state = previous_state;
        }
      }
    }
    radvs_session_leave(session, 0);
  }
  return radvs_hresult_from_result(result);
}

int32_t
BRIDGE_FN(IDebugThread2_GetName)(RADVS_Session *session, DMN_Handle thread_handle, char *text_buffer, uint64_t text_buffer_size, uint64_t *out_text_size)
{
  if (MemoryIsZeroStruct(&thread_handle) || out_text_size == 0 || (text_buffer == 0 && text_buffer_size != 0)) {
    return RADVS_HRESULT_E_INVALIDARG;
  }
  RADVS_Result result = radvs_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    RADVS_ThreadDesc thread = {0};
    result = radvs_engine_session_get_thread_desc(session->engine_session, thread_handle, &thread);
    if (result == RADVS_Result_Ok) {
      Temp scratch = scratch_begin(0, 0);
      String8 name = push_str8f(scratch.arena, "Thread %u", thread.system_thread_id);
      *out_text_size = name.size;
      if (text_buffer == 0 || text_buffer_size < name.size) {
        result = RADVS_Result_OutOfMemory;
      } else {
        MemoryCopy(text_buffer, name.str, name.size);
      }
      scratch_end(scratch);
    }
    radvs_session_leave(session, 0);
  }
  return radvs_buffer_hresult_from_result(result);
}

int32_t
BRIDGE_FN(IDebugThread2_GetTopFrame)(RADVS_Session *session, DMN_Handle thread_handle, RADVS_FrameInfo *out_frame, char *source_path_buffer, uint64_t source_path_buffer_size, uint64_t *out_source_path_size)
{
  if (MemoryIsZeroStruct(&thread_handle) || out_frame == 0 || out_source_path_size == 0) {
    return RADVS_HRESULT_E_INVALIDARG;
  }
  RADVS_Result result = radvs_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    MutexScope (session->mutex) {
      if (session->ad7_state != RADVS_AD7_State_Stopped || !dmn_handle_match(thread_handle, session->stopped_thread_handle)) {
        result = RADVS_Result_Busy;
      }
    }
    if (result == RADVS_Result_Ok) {
      U64 source_path_size = 0;
      result = radvs_engine_session_get_top_frame(session->engine_session, thread_handle, out_frame, source_path_buffer, source_path_buffer_size, &source_path_size);
      *out_source_path_size = source_path_size;
    }
    radvs_session_leave(session, 0);
  }
  return radvs_buffer_hresult_from_result(result);
}

int32_t
BRIDGE_FN(IDebugThread2_Step)(RADVS_Session *session, DMN_Handle thread_handle, uint32_t step_kind, uint32_t step_unit)
{
  (void)step_kind;
  (void)step_unit;
  if (MemoryIsZeroStruct(&thread_handle)) {
    return RADVS_HRESULT_E_INVALIDARG;
  }
  RADVS_Result result = radvs_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    MutexScope (session->mutex) {
      if (session->ad7_state != RADVS_AD7_State_Stopped || !dmn_handle_match(thread_handle, session->stopped_thread_handle)) {
        result = RADVS_Result_Busy;
      }
    }
    if (result == RADVS_Result_Ok) {
      BRIDGE_NOT_IMPLEMENTED(result);
    }
    radvs_session_leave(session, 0);
  }
  return radvs_hresult_from_result(result);
}

int32_t
BRIDGE_FN(IDebugThread2_SetInstructionPointer)(RADVS_Session *session, DMN_Handle thread_handle, uint64_t address)
{
  if (MemoryIsZeroStruct(&thread_handle) || address == 0) {
    return RADVS_HRESULT_E_INVALIDARG;
  }
  RADVS_Result result = radvs_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    MutexScope (session->mutex) {
      if (session->ad7_state != RADVS_AD7_State_Stopped || !dmn_handle_match(thread_handle, session->stopped_thread_handle)) {
        result = RADVS_Result_Busy;
      }
    }
    if (result == RADVS_Result_Ok) {
      BRIDGE_NOT_IMPLEMENTED(result);
    }
    radvs_session_leave(session, 0);
  }
  return radvs_hresult_from_result(result);
}

int32_t
BRIDGE_FN(IDebugMemoryBytes2_Read)(RADVS_Session *session, DMN_Handle process_handle, uint64_t address, void *buffer, uint64_t buffer_size, uint64_t *out_size)
{
  if (!session || !out_size) {
    return RADVS_HRESULT_E_INVALIDARG;
  }
  U64 bytes_read = 0;
  RADVS_Result result = radvs_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    result = radvs_engine_session_read_memory(session->engine_session, process_handle, address, buffer, buffer_size, &bytes_read);
    radvs_session_leave(session, 0);
  }
  *out_size = bytes_read;
  return radvs_hresult_from_result(result);
}

int32_t
BRIDGE_FN(IDebugEvent2_Wait)(RADVS_Session *session, uint32_t timeout_ms, RADVS_AD7_Event *out_event, char *text_buffer, uint64_t text_buffer_size, uint64_t *out_text_size)
{
  if (out_event == 0 || out_text_size == 0 || (text_buffer == 0 && text_buffer_size != 0)) {
    return RADVS_HRESULT_E_INVALIDARG;
  }
  RADVS_Result result = radvs_session_enter(session, 1);
  if (result == RADVS_Result_Ok) {
    U64 endt_us = now_time_us() + (U64)timeout_ms * 1000;
    for (;;) {
      result = RADVS_Result_Busy;
      B32 delivered = 0;
      MutexScope (session->mutex) {
        if (session->pending_event_valid) {
          *out_event = session->pending_event;
          *out_text_size = session->pending_text.size;
          if (session->pending_text.size != 0 && (text_buffer == 0 || text_buffer_size < session->pending_text.size)) {
            result = RADVS_Result_OutOfMemory;
          } else {
            if (session->pending_text.size != 0) {
              MemoryCopy(text_buffer, session->pending_text.str, session->pending_text.size);
            }
            session->pending_event_valid = 0;
            result = RADVS_Result_Ok;
          }
          delivered = 1;
        } else if (session->deferred_event_valid) {
          RADVS_AD7_Event event = session->deferred_event;
          session->deferred_event_valid = 0;
          arena_clear(session->event_arena);
          session->pending_event = event;
          session->pending_event.sequence = ++session->next_event_sequence;
          session->pending_text = str8_zero();
          session->pending_event.text_size = 0;
          session->pending_event_valid = 1;
          *out_event = session->pending_event;
          *out_text_size = 0;
          session->pending_event_valid = 0;
          result = RADVS_Result_Ok;
          delivered = 1;
        }
      }
      if (delivered) {
        break;
      }

      RADVS_Event event = {0};
      result = radvs_engine_session_poll_event(session->engine_session, &event);
      if (result == RADVS_Result_Busy) {
        B32 resume_after_batch = 0;
        MutexScope (session->mutex) {
          if (session->event_batch_received &&
              (session->ad7_state == RADVS_AD7_State_Starting || session->ad7_state == RADVS_AD7_State_Running)) {
            session->event_batch_received = 0;
            resume_after_batch = 1;
          }
        }
        if (resume_after_batch) {
          result = radvs_engine_session_run(session->engine_session, 0, 0);
          if (result != RADVS_Result_Ok) {
            break;
          }
          continue;
        }
        U64 now_us = now_time_us();
        if (now_us >= endt_us) {
          result = RADVS_Result_Busy;
          break;
        }
        result = radvs_engine_session_wait_event(session->engine_session, endt_us - now_us);
        if (result != RADVS_Result_Ok) {
          break;
        }
        continue;
      }
      if (result != RADVS_Result_Ok) {
        break;
      }

      B32 mapped = 0;
      MutexScope (session->mutex) {
        if (session->ad7_state == RADVS_AD7_State_Starting || session->ad7_state == RADVS_AD7_State_Running) {
          session->event_batch_received = 1;
        }

        const DMN_Event *raw = &event.raw;
        switch (raw->kind) {
        default: break;

        case DMN_EventKind_CreateProcess: {
          if (MemoryIsZeroStruct(&session->program_process_handle)) {
            session->program_process_handle = event.process_handle;
            if (event.system_process_id != 0) {
              session->program_system_process_id = event.system_process_id;
            }
          }
          RADVS_AD7_Event ad7_event = {0};
          ad7_event.event = event;
          ad7_event.kind = RADVS_AD7_EventKind_ProgramCreated;
          ad7_event.attributes = RADVS_AD7_EventAttributes_Asynchronous;
          arena_clear(session->event_arena);
          session->pending_event = ad7_event;
          session->pending_event.sequence = ++session->next_event_sequence;
          session->pending_text = str8_zero();
          session->pending_event.text_size = 0;
          session->pending_event_valid = 1;
          mapped = 1;
        } break;

        case DMN_EventKind_CreateThread: {
          if (event.thread_created) {
            RADVS_AD7_Event ad7_event = {0};
            ad7_event.event = event;
            ad7_event.kind = RADVS_AD7_EventKind_ThreadCreated;
            ad7_event.attributes = RADVS_AD7_EventAttributes_Asynchronous;
            arena_clear(session->event_arena);
            session->pending_event = ad7_event;
            session->pending_event.sequence = ++session->next_event_sequence;
            session->pending_text = str8_zero();
            session->pending_event.text_size = 0;
            session->pending_event_valid = 1;
            mapped = 1;
          }
        } break;

        case DMN_EventKind_ExitThread: {
          RADVS_AD7_Event ad7_event = {0};
          ad7_event.event = event;
          ad7_event.kind = RADVS_AD7_EventKind_ThreadExited;
          ad7_event.attributes = RADVS_AD7_EventAttributes_Asynchronous;
          if (dmn_handle_match(session->stopped_thread_handle, event.thread_handle)) {
            session->stopped_thread_handle = dmn_handle_zero();
          }
          arena_clear(session->event_arena);
          session->pending_event = ad7_event;
          session->pending_event.sequence = ++session->next_event_sequence;
          session->pending_text = str8_zero();
          session->pending_event.text_size = 0;
          session->pending_event_valid = 1;
          mapped = 1;
        } break;

        case DMN_EventKind_HandshakeComplete: {
          if (!session->load_complete_sent) {
            session->load_complete_sent = 1;
            session->ad7_state = RADVS_AD7_State_Running;
            RADVS_AD7_Event ad7_event = {0};
            ad7_event.event = event;
            ad7_event.kind = RADVS_AD7_EventKind_LoadComplete;
            ad7_event.attributes = RADVS_AD7_EventAttributes_Asynchronous;
            arena_clear(session->event_arena);
            session->pending_event = ad7_event;
            session->pending_event.sequence = ++session->next_event_sequence;
            session->pending_text = str8_zero();
            session->pending_event.text_size = 0;
            session->pending_event_valid = 1;
            mapped = 1;
          }
        } break;

        case DMN_EventKind_Trap:
        case DMN_EventKind_Breakpoint:
        case DMN_EventKind_SingleStep:
        case DMN_EventKind_Exception:
        case DMN_EventKind_Halt: {
          if (!dmn_handle_match(event.thread_handle, dmn_handle_zero())) {
            session->ad7_state = RADVS_AD7_State_Stopped;
            session->stopped_thread_handle = event.thread_handle;
            session->event_batch_received = 0;
            RADVS_AD7_Event ad7_event = {0};
            ad7_event.event = event;
            ad7_event.kind = RADVS_AD7_EventKind_Stopped;
            ad7_event.attributes = RADVS_AD7_EventAttributes_AsyncStop;
            if (raw->kind == DMN_EventKind_Exception) {
              ad7_event.stop_reason = RADVS_AD7_StopReason_Exception;
            } else if (raw->kind == DMN_EventKind_Halt) {
              ad7_event.stop_reason = RADVS_AD7_StopReason_Break;
            } else if (raw->kind == DMN_EventKind_SingleStep) {
              ad7_event.stop_reason = RADVS_AD7_StopReason_Step;
            } else {
              ad7_event.stop_reason = RADVS_AD7_StopReason_Generic;
            }
            if (event.thread_created) {
              RADVS_AD7_Event created_event = {0};
              created_event.event = event;
              created_event.kind = RADVS_AD7_EventKind_ThreadCreated;
              created_event.attributes = RADVS_AD7_EventAttributes_Asynchronous;
              session->deferred_event = ad7_event;
              session->deferred_event_valid = 1;
              arena_clear(session->event_arena);
              session->pending_event = created_event;
            } else {
              arena_clear(session->event_arena);
              session->pending_event = ad7_event;
            }
            session->pending_event.sequence = ++session->next_event_sequence;
            session->pending_text = str8_zero();
            session->pending_event.text_size = 0;
            session->pending_event_valid = 1;
            mapped = 1;
          }
        } break;

        case DMN_EventKind_DebugString: {
          RADVS_AD7_Event ad7_event = {0};
          ad7_event.event = event;
          ad7_event.kind = RADVS_AD7_EventKind_Output;
          ad7_event.attributes = RADVS_AD7_EventAttributes_Asynchronous;
          arena_clear(session->event_arena);
          session->pending_event = ad7_event;
          session->pending_event.sequence = ++session->next_event_sequence;
          session->pending_text = push_str8_copy(session->event_arena, raw->string);
          session->pending_event.text_size = session->pending_text.size;
          session->pending_event_valid = 1;
          mapped = 1;
        } break;

        case DMN_EventKind_Error: {
          Temp scratch = scratch_begin(0, 0);
          String8 text = push_str8f(scratch.arena, "RADVS DEMON error %u", raw->error_kind);
          RADVS_AD7_Event ad7_event = {0};
          ad7_event.event = event;
          ad7_event.kind = RADVS_AD7_EventKind_Output;
          ad7_event.attributes = RADVS_AD7_EventAttributes_Asynchronous;
          arena_clear(session->event_arena);
          session->pending_event = ad7_event;
          session->pending_event.sequence = ++session->next_event_sequence;
          session->pending_text = push_str8_copy(session->event_arena, text);
          session->pending_event.text_size = session->pending_text.size;
          session->pending_event_valid = 1;
          mapped = 1;
          scratch_end(scratch);
        } break;

        case DMN_EventKind_ExitProcess: {
          if (event.session_process_count == 0) {
            session->ad7_state = RADVS_AD7_State_AwaitProgramDestroyAck;
          } else if (session->ad7_state == RADVS_AD7_State_Terminating) {
            session->ad7_state = RADVS_AD7_State_Running;
          }
          session->stopped_thread_handle = dmn_handle_zero();
          RADVS_AD7_Event ad7_event = {0};
          ad7_event.event = event;
          ad7_event.kind = RADVS_AD7_EventKind_ProgramDestroyed;
          ad7_event.attributes = RADVS_AD7_EventAttributes_Synchronous;
          arena_clear(session->event_arena);
          session->pending_event = ad7_event;
          session->pending_event.sequence = ++session->next_event_sequence;
          session->pending_text = str8_zero();
          session->pending_event.text_size = 0;
          session->pending_event_valid = 1;
          session->pending_destroy_sequence = session->pending_event.sequence;
          mapped = 1;
        } break;
        }
      }
      if (mapped) {
        continue;
      }
    }
    radvs_session_leave(session, 1);
  }
  return result == RADVS_Result_Busy ? RADVS_HRESULT_S_FALSE : radvs_buffer_hresult_from_result(result);
}

int32_t
BRIDGE_FN(IDebugEvent2_Acknowledge)(RADVS_Session *session, uint64_t sequence)
{
  if (sequence == 0) {
    return RADVS_HRESULT_E_INVALIDARG;
  }
  RADVS_Result result = radvs_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    MutexScope (session->mutex) {
      if (sequence != session->pending_destroy_sequence) {
        result = RADVS_Result_InvalidArgument;
      } else {
        session->pending_destroy_sequence = 0;
        if (session->ad7_state == RADVS_AD7_State_AwaitProgramDestroyAck) {
          session->ad7_state = RADVS_AD7_State_Closed;
        }
      }
    }
    radvs_session_leave(session, 0);
  }
  return radvs_hresult_from_result(result);
}

#else

#define BRIDGE_A7_COM_STUB_RETURN_int32_t(result) return radvs_hresult_from_result(result)
#define BRIDGE_A7_COM_STUB_RETURN_void(result)    return
#define BRIDGE_A7_COM_STUB_RETURN_(ret, result)   BRIDGE_A7_COM_STUB_RETURN_##ret(result)
#define BRIDGE_A7_COM_STUB_RETURN(ret, result)    BRIDGE_A7_COM_STUB_RETURN_(ret, result)

#define X(ret, fn, ...)                                    \
  RADVS_EXPORT ret RADVS_CALL BRIDGE_FN(fn)(__VA_ARGS__) { \
    RADVS_Result result = 0;                               \
    BRIDGE_NOT_IMPLEMENTED(result);                        \
    BRIDGE_A7_COM_STUB_RETURN(ret, result);                \
  }

  Bridge_A7_Com_XList

#undef X
#undef BRIDGE_A7_COM_STUB_RETURN
#undef BRIDGE_A7_COM_STUB_RETURN_
#undef BRIDGE_A7_COM_STUB_RETURN_void
#undef BRIDGE_A7_COM_STUB_RETURN_int32_t

#endif
