// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#include "radvs/radvs_ad7_bridge.h"
#include "radvs/radvs_com_api.h"
#include "radvs/radvs_engine.h"

#pragma comment(lib, "oleaut32")

typedef enum RADVS_AD7_State
{
  RADVS_AD7_State_New,
  RADVS_AD7_State_Launching,
  RADVS_AD7_State_AwaitProgramCreate,
  RADVS_AD7_State_Starting,
  RADVS_AD7_State_Running,
  RADVS_AD7_State_Stopped,
  RADVS_AD7_State_Terminating,
  RADVS_AD7_State_AwaitProgramDestroyAck,
  RADVS_AD7_State_Closed,
} RADVS_AD7_State;

typedef struct RADVS_AD7_TranslatedEvent
{
  GUID             iid;
  DWORD            attributes;
  U64              sequence;
  DWORD            exit_code;
  THREADPROPERTIES thread;
  EXCEPTION_INFO   exception;
} RADVS_AD7_TranslatedEvent;

struct RADVS_AD7_Session
{
  Arena               *arena;
  Arena               *event_arena;
  Arena               *deferred_event_arena;
  RADVS_EngineSession *engine_session;

  Mutex   mutex;
  CondVar closing_cv;
  U32     in_flight_calls;
  B32     closing;
  B32     event_wait_active;
  B32     event_wait_closed;

  RADVS_AD7_State state;
  U64             next_event_sequence;
  U64             pending_destroy_sequence;
  DMN_Handle      stopped_thread_handle;
  U32             program_system_process_id;
  B32             event_batch_received;
  B32             load_complete_sent;

  B32                        pending_event_valid;
  RADVS_AD7_TranslatedEvent  pending_event;
  String8                    pending_text;
  B32                        deferred_event_valid;
  RADVS_AD7_TranslatedEvent  deferred_event;
  String8                    deferred_text;
};

static const GUID radvs_ad7_engine_id = {0x97bd1aec, 0x93d9, 0x4748, {0xb2, 0x8d, 0xe7, 0xe8, 0xec, 0xa0, 0xf7, 0x81}};

// msdbg.h declares these C IID constants, but the bridge's existing link does
// not consume the AD2 SDK import library that normally defines them. Selectany
// keeps the C translation unit self-contained and permits that library to be
// linked by another host without creating duplicate-definition failures.
__declspec(selectany) const IID IID_IDebugProgramDestroyEvent2 = {0xe147e9e3, 0x6440, 0x4073, {0xa7, 0xb7, 0xa6, 0x55, 0x92, 0xc7, 0x14, 0xb5}};
__declspec(selectany) const IID IID_IDebugThreadCreateEvent2   = {0x2090ccfc, 0x70c5, 0x491d, {0xa5, 0xe8, 0xba, 0xd2, 0xdd, 0x9e, 0xe3, 0xea}};
__declspec(selectany) const IID IID_IDebugThreadDestroyEvent2  = {0x2c3b7532, 0xa36f, 0x4a6e, {0x90, 0x72, 0x49, 0xbe, 0x64, 0x9b, 0x85, 0x41}};
__declspec(selectany) const IID IID_IDebugLoadCompleteEvent2   = {0xb1844850, 0x1349, 0x45d4, {0x9f, 0x12, 0x49, 0x52, 0x12, 0xf5, 0xeb, 0x0b}};
__declspec(selectany) const IID IID_IDebugBreakEvent2          = {0xc7405d1d, 0xe24b, 0x44e0, {0xb7, 0x07, 0xd8, 0xa5, 0xa4, 0xe1, 0x64, 0x1b}};
__declspec(selectany) const IID IID_IDebugExceptionEvent2      = {0x51a94113, 0x8788, 0x4a54, {0xae, 0x15, 0x08, 0xb7, 0x4f, 0xf9, 0x22, 0xd0}};
__declspec(selectany) const IID IID_IDebugOutputStringEvent2   = {0x569c4bb1, 0x7b82, 0x46fc, {0xae, 0x28, 0x45, 0x36, 0xdd, 0xad, 0x75, 0x3e}};
__declspec(selectany) const IID IID_IDebugStopCompleteEvent2   = {0x3dca9dcd, 0xfb09, 0x4af1, {0xa9, 0x26, 0x45, 0xf2, 0x93, 0xd4, 0x8b, 0x2d}};

internal HRESULT
radvs_ad7_hresult_from_result(RADVS_Result result)
{
  switch (result) {
  case RADVS_Result_Ok:              return S_OK;
  case RADVS_Result_InvalidArgument: return E_INVALIDARG;
  case RADVS_Result_OutOfMemory:     return E_OUTOFMEMORY;
  case RADVS_Result_NotImplemented:  return E_NOTIMPL;
  case RADVS_Result_AbiMismatch:     return E_NOINTERFACE;
  case RADVS_Result_Busy:            return HRESULT_FROM_WIN32(ERROR_BUSY);
  case RADVS_Result_Timeout:         return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  case RADVS_Result_Conflict:        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
  default:                           return E_FAIL;
  }
}

internal HRESULT
radvs_ad7_buffer_hresult_from_result(RADVS_Result result)
{
  return result == RADVS_Result_OutOfMemory ? HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) : radvs_ad7_hresult_from_result(result);
}

internal B32
radvs_ad7_string_is_valid(String8 string)
{
  return string.size == 0 || string.str != 0;
}

internal HRESULT
radvs_ad7_alloc_output_string(String8 source, BSTR *out)
{
  if (out == 0) {
    return E_POINTER;
  }
  *out = 0;
  if (source.size == 0) {
    return S_OK;
  }

  Temp scratch = scratch_begin(0, 0);
  String16 utf16 = str16_from_8(scratch.arena, source);
  HRESULT result = E_OUTOFMEMORY;
  if (utf16.size <= max_U32) {
    BSTR string = SysAllocStringLen((OLECHAR *)utf16.str, (UINT)utf16.size);
    if (string != 0) {
      *out = string;
      result = S_OK;
    }
  }
  scratch_end(scratch);
  return result;
}

internal RADVS_Result
radvs_ad7_session_enter(RADVS_AD7_Session *session, B32 event_wait)
{
  if (session == 0) {
    return RADVS_Result_InvalidArgument;
  }
  RADVS_Result result = RADVS_Result_Busy;
  MutexScope (session->mutex) {
    if (!session->closing && (!event_wait || (!session->event_wait_active && !session->event_wait_closed))) {
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
radvs_ad7_session_leave(RADVS_AD7_Session *session, B32 event_wait)
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

internal THREADPROPERTIES
radvs_ad7_event_thread_properties(U32 system_thread_id)
{
  THREADPROPERTIES result = {0};
  if (system_thread_id != 0) {
    result.dwFields      = TPF_ID | TPF_STATE;
    result.dwThreadId    = system_thread_id;
    result.dwThreadState = THREADSTATE_STOPPED;
  }
  return result;
}

internal THREADPROPERTIES
radvs_ad7_thread_properties_from_desc(const RADVS_ThreadDesc *thread, THREADPROPERTY_FIELDS fields)
{
  THREADPROPERTIES result = {0};
  result.dwFields = fields & (TPF_ID | TPF_STATE | TPF_NAME);
  if ((fields & TPF_ID) != 0) {
    result.dwThreadId = thread->system_thread_id;
  }
  if ((fields & TPF_STATE) != 0) {
    result.dwThreadState = THREADSTATE_STOPPED;
  }
  return result;
}

internal RADVS_AD7_TranslatedEvent
radvs_ad7_translated_event(GUID iid, DWORD attributes, const RADVS_Event *event)
{
  RADVS_AD7_TranslatedEvent result = {0};
  result.iid = iid;
  result.attributes = attributes;
  result.thread = radvs_ad7_event_thread_properties(event->system_thread_id);
  return result;
}

internal void
radvs_ad7_set_pending_event(RADVS_AD7_Session *session, RADVS_AD7_TranslatedEvent event, String8 text)
{
  arena_clear(session->event_arena);
  session->pending_event = event;
  session->pending_event.sequence = ++session->next_event_sequence;
  session->pending_text = push_str8_copy(session->event_arena, text);
  session->pending_event_valid = 1;
}

internal void
radvs_ad7_set_deferred_event(RADVS_AD7_Session *session, RADVS_AD7_TranslatedEvent event, String8 text)
{
  arena_clear(session->deferred_event_arena);
  session->deferred_event       = event;
  session->deferred_text        = push_str8_copy(session->deferred_event_arena, text);
  session->deferred_event_valid = 1;
}

internal HRESULT
radvs_ad7_deliver_pending_event(RADVS_AD7_Session *session,
                                GUID              *out_event_iid,
                                DWORD             *out_attributes,
                                U64               *out_sequence,
                                DWORD             *out_exit_code,
                                THREADPROPERTIES  *out_thread,
                                EXCEPTION_INFO    *out_exception,
                                BSTR              *out_text)
{
  HRESULT result = radvs_ad7_alloc_output_string(session->pending_text, out_text);
  if (SUCCEEDED(result)) {
    *out_event_iid  = session->pending_event.iid;
    *out_attributes = session->pending_event.attributes;
    *out_sequence   = session->pending_event.sequence;
    *out_exit_code  = session->pending_event.exit_code;
    *out_thread     = session->pending_event.thread;
    *out_exception  = session->pending_event.exception;
    session->pending_event_valid = 0;
  }
  return result;
}

internal B32
radvs_ad7_map_event(RADVS_AD7_Session *session, const RADVS_Event *event)
{
  Temp scratch = scratch_begin(0, 0);
  B32 is_ok = 0;

  const DMN_Event *raw = &event->raw;
  switch (raw->kind) {
  default: break;

  case DMN_EventKind_CreateProcess: {
    if (session->program_system_process_id == 0 && event->system_process_id != 0) {
      session->program_system_process_id = event->system_process_id;
    }
  } break;

  case DMN_EventKind_CreateThread: {
    if (event->thread_created) {
      RADVS_AD7_TranslatedEvent translated = radvs_ad7_translated_event(IID_IDebugThreadCreateEvent2, EVENT_ASYNCHRONOUS, event);
      radvs_ad7_set_pending_event(session, translated, str8_zero());
      is_ok = 1;
    }
  } break;

  case DMN_EventKind_ExitThread: {
    RADVS_AD7_TranslatedEvent translated = radvs_ad7_translated_event(IID_IDebugThreadDestroyEvent2, EVENT_ASYNCHRONOUS, event);
    translated.exit_code = raw->code;
    if (dmn_handle_match(session->stopped_thread_handle, event->thread_handle)) {
      session->stopped_thread_handle = dmn_handle_zero();
    }
    radvs_ad7_set_pending_event(session, translated, str8_zero());
    is_ok = 1;
  } break;

  case DMN_EventKind_HandshakeComplete: {
    if (!session->load_complete_sent) {
      session->load_complete_sent = 1;
      session->state              = RADVS_AD7_State_Running;
      RADVS_AD7_TranslatedEvent translated = radvs_ad7_translated_event(IID_IDebugLoadCompleteEvent2, EVENT_ASYNCHRONOUS, event);
      radvs_ad7_set_pending_event(session, translated, str8_zero());
      return 1;
    }
  } break;

  case DMN_EventKind_Trap:
  case DMN_EventKind_Breakpoint:
  case DMN_EventKind_SingleStep:
  case DMN_EventKind_Exception:
  case DMN_EventKind_Halt: {
    if (dmn_handle_match(event->thread_handle, dmn_handle_zero())) { break; }

    session->state                 = RADVS_AD7_State_Stopped;
    session->stopped_thread_handle = event->thread_handle;
    session->event_batch_received  = 0;

    GUID    iid  = IID_IDebugStopCompleteEvent2;
    String8 text = str8_zero();
    if (raw->kind == DMN_EventKind_Exception) {
      iid  = IID_IDebugExceptionEvent2;
      text = push_str8f(scratch.arena, "Debuggee exception 0x%08X", raw->code);
    } else if (raw->kind == DMN_EventKind_Halt) {
      iid = IID_IDebugBreakEvent2;
    }

    RADVS_AD7_TranslatedEvent translated = radvs_ad7_translated_event(iid, EVENT_ASYNC_STOP, event);
    if (raw->kind == DMN_EventKind_Exception) {
      translated.exception.dwCode  = raw->code;
      translated.exception.dwState = raw->exception_repeated ? EXCEPTION_STOP_SECOND_CHANCE : EXCEPTION_STOP_FIRST_CHANCE;
    }

    if (event->thread_created) {
      RADVS_AD7_TranslatedEvent thread_created = radvs_ad7_translated_event(IID_IDebugThreadCreateEvent2, EVENT_ASYNCHRONOUS, event);
      radvs_ad7_set_deferred_event(session, translated, text);
      radvs_ad7_set_pending_event(session, thread_created, str8_zero());
    } else {
      radvs_ad7_set_pending_event(session, translated, text);
    }

    is_ok = 1;
  } break;

  case DMN_EventKind_DebugString: {
    RADVS_AD7_TranslatedEvent translated = radvs_ad7_translated_event(IID_IDebugOutputStringEvent2, EVENT_ASYNCHRONOUS, event);
    radvs_ad7_set_pending_event(session, translated, raw->string);
    is_ok = 1;
  } break;

  case DMN_EventKind_Error: {
    String8 text = push_str8f(scratch.arena, "RADVS DEMON error %u", raw->error_kind);
    RADVS_AD7_TranslatedEvent translated = radvs_ad7_translated_event(IID_IDebugOutputStringEvent2, EVENT_ASYNCHRONOUS, event);
    radvs_ad7_set_pending_event(session, translated, text);
    is_ok = 1;
  } break;

  case DMN_EventKind_ExitProcess: {
    if (event->session_process_count != 0) {
      if (session->state == RADVS_AD7_State_Terminating) {
        session->state = RADVS_AD7_State_Running;
      }
      break;
    }

    session->state                 = RADVS_AD7_State_AwaitProgramDestroyAck;
    session->stopped_thread_handle = dmn_handle_zero();

    RADVS_AD7_TranslatedEvent translated = radvs_ad7_translated_event(IID_IDebugProgramDestroyEvent2, EVENT_SYNCHRONOUS, event);
    translated.exit_code = raw->code;
    radvs_ad7_set_pending_event(session, translated, str8_zero());
    session->pending_destroy_sequence = session->pending_event.sequence;

    is_ok = 1;
  } break;
  }

  scratch_end(scratch);
  return 0;
}

internal RADVS_Result
radvs_ad7_start_after_program_create(RADVS_AD7_Session *session)
{
  RADVS_Result result = RADVS_Result_Ok;
  MutexScope (session->mutex) {
    if (session->state != RADVS_AD7_State_AwaitProgramCreate) {
      result = RADVS_Result_Busy;
    } else {
      session->state = RADVS_AD7_State_Starting;
    }
  }
  if (result == RADVS_Result_Ok) {
    result = radvs_engine_session_run(session->engine_session, 0, 0);
  }
  if (result != RADVS_Result_Ok) {
    MutexScope (session->mutex) {
      if (session->state == RADVS_AD7_State_Starting) {
        session->state = RADVS_AD7_State_AwaitProgramCreate;
      }
    }
  }
  return result;
}

U32 RADVS_AD7_CALL
radvs_ad7_bridge_abi_version(void)
{
  return RADVS_AD7_BRIDGE_ABI_VERSION;
}

HRESULT RADVS_AD7_CALL
radvs_ad7_bridge_session_create(RADVS_AD7_Session **out_session)
{
  if (out_session == 0) {
    return E_POINTER;
  }
  *out_session = 0;

  RADVS_EngineSession *engine_session = 0;
  RADVS_Result result = (RADVS_Result)radvs_com_session_alloc(&engine_session);
  if (result != RADVS_Result_Ok) {
    return radvs_ad7_hresult_from_result(result);
  }

  Arena *arena = arena_alloc();
  RADVS_AD7_Session *session = push_array(arena, RADVS_AD7_Session, 1);
  session->arena                = arena;
  session->event_arena          = arena_alloc();
  session->deferred_event_arena = arena_alloc();
  session->engine_session       = engine_session;
  session->mutex                = mutex_alloc();
  session->closing_cv           = cond_var_alloc();
  session->state                = RADVS_AD7_State_New;
  *out_session = session;
  return S_OK;
}

HRESULT RADVS_AD7_CALL
radvs_ad7_bridge_session_destroy(RADVS_AD7_Session *session)
{
  if (session == 0) {
    return E_INVALIDARG;
  }

  mutex_take(session->mutex);
  if (session->closing) {
    mutex_drop(session->mutex);
    return HRESULT_FROM_WIN32(ERROR_BUSY);
  }
  session->closing           = 1;
  session->event_wait_closed = 1;
  cond_var_broadcast(session->closing_cv);
  mutex_drop(session->mutex);

  radvs_engine_session_close_event_wait(session->engine_session);
  mutex_take(session->mutex);
  for (; session->in_flight_calls != 0;) {
    cond_var_wait(session->closing_cv, session->mutex, max_U64);
  }
  mutex_drop(session->mutex);

  RADVS_EngineSession *engine_session       = session->engine_session;
  Arena               *arena                = session->arena;
  Arena               *event_arena          = session->event_arena;
  Arena               *deferred_event_arena = session->deferred_event_arena;
  CondVar              closing_cv           = session->closing_cv;
  Mutex                mutex                = session->mutex;
  cond_var_release(closing_cv);
  mutex_release(mutex);
  arena_release(deferred_event_arena);
  arena_release(event_arena);
  arena_release(arena);
  radvs_com_session_release(engine_session);
  return S_OK;
}

HRESULT RADVS_AD7_CALL
radvs_ad7_bridge_session_launch(RADVS_AD7_Session *session,
                                String8 exe,
                                String8 args,
                                String8 wdir,
                                AD_PROCESS_ID *out_process_id)
{
  if (out_process_id == 0) {
    return E_POINTER;
  }
  MemoryZeroStruct(out_process_id);
  if (!radvs_ad7_string_is_valid(exe) || !radvs_ad7_string_is_valid(args) ||
      !radvs_ad7_string_is_valid(wdir) || exe.size == 0) {
    return E_INVALIDARG;
  }

  RADVS_Result result = radvs_ad7_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    MutexScope (session->mutex) {
      if (session->state != RADVS_AD7_State_New) {
        result = RADVS_Result_Busy;
      } else {
        session->state = RADVS_AD7_State_Launching;
      }
    }
    U32 pid = 0;
    if (result == RADVS_Result_Ok) {
      result = radvs_engine_session_launch(session->engine_session, exe, args, wdir, &pid);
    }
    MutexScope (session->mutex) {
      if (result == RADVS_Result_Ok) {
        session->program_system_process_id = pid;
        session->state = RADVS_AD7_State_AwaitProgramCreate;
      } else if (session->state == RADVS_AD7_State_Launching) {
        session->state = RADVS_AD7_State_New;
      }
    }
    if (result == RADVS_Result_Ok) {
      out_process_id->ProcessIdType = AD_PROCESS_ID_SYSTEM;
      out_process_id->ProcessId.dwProcessId = pid;
    }
    radvs_ad7_session_leave(session, 0);
  }
  return radvs_ad7_hresult_from_result(result);
}

HRESULT RADVS_AD7_CALL
radvs_ad7_bridge_continue_synchronous_event(RADVS_AD7_Session *session, U64 sequence)
{
  RADVS_Result result = radvs_ad7_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    if (sequence == 0) {
      result = radvs_ad7_start_after_program_create(session);
    } else {
      MutexScope (session->mutex) {
        if (sequence != session->pending_destroy_sequence) {
          result = RADVS_Result_InvalidArgument;
        } else {
          session->pending_destroy_sequence = 0;
          if (session->state == RADVS_AD7_State_AwaitProgramDestroyAck) {
            session->state = RADVS_AD7_State_Closed;
          }
          cond_var_broadcast(session->closing_cv);
        }
      }
    }
    radvs_ad7_session_leave(session, 0);
  }
  return radvs_ad7_hresult_from_result(result);
}

HRESULT RADVS_AD7_CALL
radvs_ad7_bridge_run(RADVS_AD7_Session *session)
{
  RADVS_Result result = radvs_ad7_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    B32 start_after_program_create = 0;
    DMN_Handle stopped_thread = {0};
    MutexScope (session->mutex) {
      if (session->state == RADVS_AD7_State_AwaitProgramCreate) {
        start_after_program_create = 1;
      } else if (session->state != RADVS_AD7_State_Stopped) {
        result = RADVS_Result_Busy;
      } else {
        stopped_thread = session->stopped_thread_handle;
        session->stopped_thread_handle = dmn_handle_zero();
        session->event_batch_received = 0;
        session->state = RADVS_AD7_State_Running;
      }
    }
    if (result == RADVS_Result_Ok && start_after_program_create) {
      result = radvs_ad7_start_after_program_create(session);
    } else if (result == RADVS_Result_Ok) {
      result = radvs_engine_session_run(session->engine_session, 0, 0);
    }
    if (result != RADVS_Result_Ok && !start_after_program_create) {
      MutexScope (session->mutex) {
        if (session->state == RADVS_AD7_State_Running) {
          session->state = RADVS_AD7_State_Stopped;
          session->stopped_thread_handle = stopped_thread;
        }
      }
    }
    radvs_ad7_session_leave(session, 0);
  }
  return radvs_ad7_hresult_from_result(result);
}

HRESULT RADVS_AD7_CALL
radvs_ad7_bridge_break(RADVS_AD7_Session *session)
{
  RADVS_Result result = radvs_ad7_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    MutexScope (session->mutex) {
      if (session->state != RADVS_AD7_State_Starting && session->state != RADVS_AD7_State_Running) {
        result = RADVS_Result_Busy;
      }
    }
    if (result == RADVS_Result_Ok) {
      result = radvs_engine_session_break(session->engine_session);
    }
    radvs_ad7_session_leave(session, 0);
  }
  return radvs_ad7_hresult_from_result(result);
}

HRESULT RADVS_AD7_CALL
radvs_ad7_bridge_terminate(RADVS_AD7_Session *session)
{
  RADVS_Result result = radvs_ad7_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    RADVS_AD7_State previous_state = RADVS_AD7_State_New;
    MutexScope (session->mutex) {
      if (session->state == RADVS_AD7_State_Closed ||
          session->state == RADVS_AD7_State_AwaitProgramDestroyAck ||
          session->state == RADVS_AD7_State_Launching) {
        result = RADVS_Result_Busy;
      } else {
        previous_state = session->state;
        session->state = RADVS_AD7_State_Terminating;
      }
    }
    if (result == RADVS_Result_Ok) {
      result = radvs_engine_session_terminate(session->engine_session, 0, 0);
    }
    if (result != RADVS_Result_Ok) {
      MutexScope (session->mutex) {
        if (session->state == RADVS_AD7_State_Terminating) {
          session->state = previous_state;
        }
      }
    }
    radvs_ad7_session_leave(session, 0);
  }
  return radvs_ad7_hresult_from_result(result);
}

HRESULT RADVS_AD7_CALL
radvs_ad7_bridge_close_event_wait(RADVS_AD7_Session *session)
{
  RADVS_Result result = radvs_ad7_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    MutexScope (session->mutex) {
      session->event_wait_closed = 1;
      cond_var_broadcast(session->closing_cv);
    }
    radvs_engine_session_close_event_wait(session->engine_session);
    radvs_ad7_session_leave(session, 0);
  }
  return radvs_ad7_hresult_from_result(result);
}

HRESULT RADVS_AD7_CALL
radvs_ad7_bridge_wait_event(RADVS_AD7_Session *session,
                            DWORD timeout_ms,
                            GUID *out_event_iid,
                            DWORD *out_attributes,
                            U64 *out_sequence,
                            DWORD *out_exit_code,
                            THREADPROPERTIES *out_thread,
                            EXCEPTION_INFO *out_exception,
                            BSTR *out_text)
{
  if (out_event_iid == 0 || out_attributes == 0 || out_sequence == 0 ||
      out_exit_code == 0 || out_thread == 0 || out_exception == 0 || out_text == 0) {
    return E_POINTER;
  }
  MemoryZeroStruct(out_event_iid);
  *out_attributes = 0;
  *out_sequence = 0;
  *out_exit_code = 0;
  MemoryZeroStruct(out_thread);
  MemoryZeroStruct(out_exception);
  *out_text = 0;

  RADVS_Result result = radvs_ad7_session_enter(session, 1);
  if (result != RADVS_Result_Ok) {
    return radvs_ad7_hresult_from_result(result);
  }

  U64 deadline_us = max_U64;
  if (timeout_ms != INFINITE) {
    deadline_us = now_time_us() + (U64)timeout_ms * 1000;
  }
  HRESULT hresult = S_FALSE;
  for (;;) {
    B32 delivered = 0;
    B32 wait_closed = 0;
    B32 wait_for_destroy_ack = 0;
    MutexScope (session->mutex) {
      if (session->event_wait_closed || session->state == RADVS_AD7_State_Closed) {
        wait_closed = 1;
      } else if (session->pending_event_valid) {
        hresult = radvs_ad7_deliver_pending_event(session,
                                                  out_event_iid,
                                                  out_attributes,
                                                  out_sequence,
                                                  out_exit_code,
                                                  out_thread,
                                                  out_exception,
                                                  out_text);
        delivered = 1;
      } else if (session->deferred_event_valid) {
        RADVS_AD7_TranslatedEvent deferred_event = session->deferred_event;
        String8 deferred_text = session->deferred_text;
        session->deferred_event_valid = 0;
        radvs_ad7_set_pending_event(session, deferred_event, deferred_text);
        arena_clear(session->deferred_event_arena);
        session->deferred_text = str8_zero();
        hresult = radvs_ad7_deliver_pending_event(session,
                                                  out_event_iid,
                                                  out_attributes,
                                                  out_sequence,
                                                  out_exit_code,
                                                  out_thread,
                                                  out_exception,
                                                  out_text);
        delivered = 1;
      } else if (session->pending_destroy_sequence != 0) {
        wait_for_destroy_ack = 1;
      }
    }
    if (wait_closed) {
      hresult = HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
      break;
    }
    if (delivered) {
      break;
    }
    if (wait_for_destroy_ack) {
      U64 now_us = now_time_us();
      if (deadline_us != max_U64 && now_us >= deadline_us) {
        hresult = S_FALSE;
        break;
      }
      U64 wait_us = deadline_us == max_U64 ? max_U64 : deadline_us - now_us;
      mutex_take(session->mutex);
      if (session->pending_destroy_sequence != 0 && !session->event_wait_closed) {
        cond_var_wait(session->closing_cv, session->mutex, wait_us);
      }
      mutex_drop(session->mutex);
      continue;
    }

    RADVS_Event event = {0};
    result = radvs_engine_session_poll_event(session->engine_session, &event);
    if (result == RADVS_Result_Busy) {
      B32 resume_after_batch = 0;
      MutexScope (session->mutex) {
        if (session->event_batch_received &&
            (session->state == RADVS_AD7_State_Starting || session->state == RADVS_AD7_State_Running)) {
          session->event_batch_received = 0;
          resume_after_batch = 1;
        }
      }
      if (resume_after_batch) {
        result = radvs_engine_session_run(session->engine_session, 0, 0);
        if (result != RADVS_Result_Ok) {
          hresult = radvs_ad7_hresult_from_result(result);
          break;
        }
        continue;
      }

      U64 now_us = now_time_us();
      if (deadline_us != max_U64 && now_us >= deadline_us) {
        hresult = S_FALSE;
        break;
      }
      U64 wait_us = deadline_us == max_U64 ? max_U64 / 2 : deadline_us - now_us;
      result = radvs_engine_session_wait_event(session->engine_session, wait_us);
      if (result == RADVS_Result_Busy) {
        B32 closed = 0;
        MutexScope (session->mutex) {
          closed = session->event_wait_closed;
        }
        hresult = closed ? HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED) : S_FALSE;
        break;
      }
      if (result != RADVS_Result_Ok) {
        hresult = radvs_ad7_hresult_from_result(result);
        break;
      }
      continue;
    }
    if (result != RADVS_Result_Ok) {
      hresult = radvs_ad7_hresult_from_result(result);
      break;
    }

    B32 mapped = 0;
    MutexScope (session->mutex) {
      if (session->state == RADVS_AD7_State_Starting || session->state == RADVS_AD7_State_Running) {
        session->event_batch_received = 1;
      }
      mapped = radvs_ad7_map_event(session, &event);
    }
    if (mapped) {
      continue;
    }
  }

  radvs_ad7_session_leave(session, 1);
  return hresult;
}

internal HRESULT
radvs_ad7_session_alloc_static_string(RADVS_AD7_Session *session, String8 source, BSTR *out)
{
  if (out == 0) {
    return E_POINTER;
  }
  *out = 0;
  RADVS_Result result = radvs_ad7_session_enter(session, 0);
  if (result != RADVS_Result_Ok) {
    return radvs_ad7_hresult_from_result(result);
  }
  HRESULT hresult = radvs_ad7_alloc_output_string(source, out);
  radvs_ad7_session_leave(session, 0);
  return hresult;
}

HRESULT RADVS_AD7_CALL
radvs_ad7_bridge_get_program_name(RADVS_AD7_Session *session, BSTR *out_name)
{
  return radvs_ad7_session_alloc_static_string(session, str8_lit("RAD program"), out_name);
}

HRESULT RADVS_AD7_CALL
radvs_ad7_bridge_get_host_name(RADVS_AD7_Session *session, GETHOSTNAME_TYPE type, BSTR *out_name)
{
  (void)type;
  return radvs_ad7_bridge_get_host_machine_name(session, out_name);
}

HRESULT RADVS_AD7_CALL
radvs_ad7_bridge_get_host_pid(RADVS_AD7_Session *session, AD_PROCESS_ID *out_process_id)
{
  if (out_process_id == 0) {
    return E_POINTER;
  }
  MemoryZeroStruct(out_process_id);
  RADVS_Result result = radvs_ad7_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    MutexScope (session->mutex) {
      if (session->program_system_process_id == 0) {
        result = RADVS_Result_Busy;
      } else {
        out_process_id->ProcessIdType = AD_PROCESS_ID_SYSTEM;
        out_process_id->ProcessId.dwProcessId = session->program_system_process_id;
      }
    }
    radvs_ad7_session_leave(session, 0);
  }
  return radvs_ad7_hresult_from_result(result);
}

HRESULT RADVS_AD7_CALL
radvs_ad7_bridge_get_host_machine_name(RADVS_AD7_Session *session, BSTR *out_name)
{
  if (out_name == 0) {
    return E_POINTER;
  }
  *out_name = 0;
  RADVS_Result result = radvs_ad7_session_enter(session, 0);
  if (result != RADVS_Result_Ok) {
    return radvs_ad7_hresult_from_result(result);
  }
  String8 machine_name = get_system_info()->machine_name;
  HRESULT hresult = E_FAIL;
  if (machine_name.size != 0) {
    hresult = radvs_ad7_alloc_output_string(machine_name, out_name);
  }
  radvs_ad7_session_leave(session, 0);
  return hresult;
}

HRESULT RADVS_AD7_CALL
radvs_ad7_bridge_get_engine_info(RADVS_AD7_Session *session, BSTR *out_name, GUID *out_engine_id)
{
  if (out_name == 0 || out_engine_id == 0) {
    return E_POINTER;
  }
  *out_engine_id = radvs_ad7_engine_id;
  return radvs_ad7_session_alloc_static_string(session, str8_lit("RAD Debug Engine"), out_name);
}

HRESULT RADVS_AD7_CALL
radvs_ad7_bridge_copy_threads(RADVS_AD7_Session *session,
                              THREADPROPERTIES *buffer,
                              U64 buffer_count,
                              U64 *out_count)
{
  if (out_count == 0) {
    return E_POINTER;
  }
  *out_count = 0;
  if (buffer == 0 && buffer_count != 0) {
    return E_INVALIDARG;
  }

  RADVS_Result result = radvs_ad7_session_enter(session, 0);
  if (result == RADVS_Result_Ok) {
    U64 native_count = 0;
    result = radvs_engine_session_copy_threads(session->engine_session, 0, 0, &native_count);
    if (result == RADVS_Result_OutOfMemory) {
      result = RADVS_Result_Ok;
    }
    *out_count = native_count;
    if (result == RADVS_Result_Ok && native_count > buffer_count) {
      result = RADVS_Result_OutOfMemory;
    }
    if (result == RADVS_Result_Ok && native_count != 0) {
      Temp scratch = scratch_begin(0, 0);
      RADVS_ThreadDesc *threads = push_array_no_zero(scratch.arena, RADVS_ThreadDesc, native_count);
      result = radvs_engine_session_copy_threads(session->engine_session, threads, native_count, &native_count);
      *out_count = native_count;
      if (result == RADVS_Result_Ok) {
        for (U64 index = 0; index < native_count; index += 1) {
          buffer[index] = radvs_ad7_thread_properties_from_desc(&threads[index], TPF_ID | TPF_STATE);
        }
      }
      scratch_end(scratch);
    }
    radvs_ad7_session_leave(session, 0);
  }
  return radvs_ad7_buffer_hresult_from_result(result);
}

HRESULT RADVS_AD7_CALL
radvs_ad7_bridge_get_thread_properties(RADVS_AD7_Session *session,
                                       DWORD system_thread_id,
                                       THREADPROPERTY_FIELDS fields,
                                       THREADPROPERTIES *out_properties,
                                       BSTR *out_name)
{
  if (out_properties == 0 || out_name == 0) {
    return E_POINTER;
  }
  MemoryZeroStruct(out_properties);
  *out_name = 0;
  if (system_thread_id == 0) {
    return E_INVALIDARG;
  }

  RADVS_Result result = radvs_ad7_session_enter(session, 0);
  HRESULT hresult = radvs_ad7_hresult_from_result(result);
  if (result == RADVS_Result_Ok) {
    Temp scratch = scratch_begin(0, 0);
    RADVS_ThreadDesc thread = {.system_thread_id = system_thread_id};
    *out_properties = radvs_ad7_thread_properties_from_desc(&thread, fields);
    String8 name = (fields & TPF_NAME) != 0 ? push_str8f(scratch.arena, "Thread %u", system_thread_id) :
                                             str8_zero();
    hresult = radvs_ad7_alloc_output_string(name, out_name);
    scratch_end(scratch);
    radvs_ad7_session_leave(session, 0);
  }
  return hresult;
}

////////////////////////////////

#define BRIDGE_NOT_IMPLEMENTED return 0;

HRESULT
BRIDGE_FN(IDebugEngineLaunch2_LaunchSuspended)(RADVS_AD7_Session *session,
                                               LPCOLESTR pszServer,
                                               IDebugPort2 *pPort,
                                               LPCOLESTR pszExe,
                                               LPCOLESTR pszArgs,
                                               LPCOLESTR pszDir,
                                               BSTR bstrEnv,
                                               LPCOLESTR pszOptions,
                                               LAUNCH_FLAGS dwLaunchFlags,
                                               DWORD hStdInput,
                                               DWORD hStdOutput,
                                               DWORD hStdError,
                                               IDebugEventCallback2 *pCallback,
                                               IDebugProcess2 **ppProcess)
{
  BRIDGE_NOT_IMPLEMENTED;
}

HRESULT
BRIDGE_FN(IDebugEngineLaunch2_ResumeProcess)(RADVS_AD7_Session *session, IDebugProcess2 *pProcess)
{
  BRIDGE_NOT_IMPLEMENTED;
}

HRESULT
BRIDGE_FN(IDebugEngineLaunch2_CanTerminateProcess)(RADVS_AD7_Session *session, IDebugProcess2 *pProcess)
{
  BRIDGE_NOT_IMPLEMENTED;
}

HRESULT
BRIDGE_FN(IDebugEngineLaunch2_TerminateProcess)(RADVS_AD7_Session *session, IDebugProcess2 *pProcess)
{
  BRIDGE_NOT_IMPLEMENTED;
}

