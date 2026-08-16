// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#include "radvs_com_core.h"
#include "radvs_demon.h"
#include "radvs_engine.h"
#include "radvs_symbol_service.h"
#include "radvs_tasker.h"

typedef enum RADVS_ComRuntimeState
{
  RADVS_ComRuntimeState_Uninitialized,
  RADVS_ComRuntimeState_Ready,
  RADVS_ComRuntimeState_Releasing,
} RADVS_ComRuntimeState;

static U32 radvs_com_runtime_state = RADVS_ComRuntimeState_Uninitialized;
static U32 radvs_com_runtime_gate = 0;
static U64 radvs_com_session_count = 0;
static RADVS_SymbolService *radvs_com_symbol_service = 0;
static RADVS_Engine *radvs_com_engine = 0;

internal void
radvs_com_runtime_gate_take(void)
{
  for (; ins_atomic_u32_eval_cond_assign(&radvs_com_runtime_gate, 1, 0) != 0;) {
    Sleep(0);
  }
}

internal void
radvs_com_runtime_gate_drop(void)
{
  ins_atomic_u32_eval_assign(&radvs_com_runtime_gate, 0);
}

internal RADVS_Result
radvs_com_runtime_start_locked(void)
{
  U32 state = ins_atomic_u32_eval(&radvs_com_runtime_state);
  if (state == RADVS_ComRuntimeState_Ready) {
    return RADVS_Result_Ok;
  }
  if (state != RADVS_ComRuntimeState_Uninitialized) {
    return RADVS_Result_Busy;
  }

  w32_base_init_system();
  w32_base_init_entities();
  RADVS_Result result = radvs_tasker_init();
  if (result == RADVS_Result_Ok) {
    result = radvs_symbol_service_init(&radvs_com_symbol_service);
  }
  if (result == RADVS_Result_Ok) {
    result = radvs_demon_init();
  }
  if (result == RADVS_Result_Ok) {
    result = radvs_engine_alloc(radvs_com_symbol_service, &radvs_com_engine);
  }
  if (result == RADVS_Result_Ok) {
    ins_atomic_u32_eval_assign(&radvs_com_runtime_state, RADVS_ComRuntimeState_Ready);
  } else {
    radvs_engine_release(radvs_com_engine);
    radvs_com_engine = 0;
    radvs_tasker_release();
    radvs_symbol_service_release(radvs_com_symbol_service);
    radvs_com_symbol_service = 0;
    w32_base_release_entities();
  }
  return result;
}

internal RADVS_Result
radvs_com_runtime_release_locked(void)
{
  if (ins_atomic_u32_eval(&radvs_com_runtime_state) == RADVS_ComRuntimeState_Uninitialized) {
    return RADVS_Result_Ok;
  }
  if (ins_atomic_u32_eval(&radvs_com_runtime_state) != RADVS_ComRuntimeState_Ready || radvs_com_session_count != 0) {
    return RADVS_Result_Busy;
  }

  ins_atomic_u32_eval_assign(&radvs_com_runtime_state, RADVS_ComRuntimeState_Releasing);
  RADVS_Result result = radvs_engine_release(radvs_com_engine);
  if (result == RADVS_Result_Ok) {
    radvs_com_engine = 0;
    result = radvs_demon_shutdown();
  }
  if (result == RADVS_Result_Ok) {
    radvs_symbol_service_prepare_release(radvs_com_symbol_service);
    radvs_tasker_release();
    radvs_symbol_service_release(radvs_com_symbol_service);
    radvs_com_symbol_service = 0;
    w32_base_release_entities();
    ins_atomic_u32_eval_assign(&radvs_com_runtime_state, RADVS_ComRuntimeState_Uninitialized);
  } else {
    ins_atomic_u32_eval_assign(&radvs_com_runtime_state, RADVS_ComRuntimeState_Ready);
  }
  return result;
}

RADVS_ComResult
radvs_com_session_alloc(RADVS_EngineSession **out_session)
{
  if (out_session == 0) {
    return RADVS_ComResult_InvalidArgument;
  }
  *out_session = 0;

  radvs_com_runtime_gate_take();
  RADVS_Result result = radvs_com_runtime_start_locked();
  if (result == RADVS_Result_Ok) {
    radvs_com_session_count += 1;
  }
  radvs_com_runtime_gate_drop();
  if (result != RADVS_Result_Ok) {
    return (RADVS_ComResult)result;
  }

  result = radvs_engine_session_alloc(radvs_com_engine, out_session);
  if (result != RADVS_Result_Ok) {
    radvs_com_runtime_gate_take();
    radvs_com_session_count -= 1;
    if (radvs_com_session_count == 0) {
      radvs_com_runtime_release_locked();
    }
    radvs_com_runtime_gate_drop();
  }
  return (RADVS_ComResult)result;
}

void
radvs_com_session_release(RADVS_EngineSession *session)
{
  if (session == 0) {
    return;
  }

  radvs_engine_session_close_event_wait(session);
  radvs_engine_session_release(session);

  radvs_com_runtime_gate_take();
  Assert(radvs_com_session_count != 0);
  radvs_com_session_count -= 1;
  if (radvs_com_session_count == 0) {
    radvs_com_runtime_release_locked();
  }
  radvs_com_runtime_gate_drop();
}

RADVS_ComResult
radvs_com_session_launch16(RADVS_EngineSession *session, RADVS_ComString16 exe, RADVS_ComString16 args, RADVS_ComString16 wdir, uint32_t *out_pid)
{
  String16 exe16 = {(U16 *)exe.str, exe.size};
  String16 args16 = {(U16 *)args.str, args.size};
  String16 wdir16 = {(U16 *)wdir.str, wdir.size};
  return (RADVS_ComResult)radvs_engine_session_launch16(session, exe16, args16, wdir16, (U32 *)out_pid);
}

RADVS_ComResult
radvs_com_session_run(RADVS_EngineSession *session)
{
  return (RADVS_ComResult)radvs_engine_session_run(session, 0, 0);
}

RADVS_ComResult
radvs_com_session_break(RADVS_EngineSession *session)
{
  return (RADVS_ComResult)radvs_engine_session_break(session);
}

RADVS_ComResult
radvs_com_session_terminate(RADVS_EngineSession *session)
{
  return (RADVS_ComResult)radvs_engine_session_terminate(session, 0, 0);
}

void
radvs_com_session_close_event_wait(RADVS_EngineSession *session)
{
  radvs_engine_session_close_event_wait(session);
}

RADVS_ComResult
radvs_com_session_wait_event(RADVS_EngineSession *session)
{
  return (RADVS_ComResult)radvs_engine_session_wait_event(session, max_U64);
}

RADVS_ComResult
radvs_com_session_poll_event(RADVS_EngineSession *session, RADVS_ComEvent *out_event)
{
  if (out_event == 0) {
    return RADVS_ComResult_InvalidArgument;
  }

  RADVS_Event event = {0};
  RADVS_Result result = radvs_engine_session_poll_event(session, &event);
  if (result != RADVS_Result_Ok) {
    return (RADVS_ComResult)result;
  }

  MemoryZeroStruct(out_event);
  out_event->message = event.raw.string.str;
  out_event->message_size = event.raw.string.size;
  out_event->code = event.raw.code;
  out_event->system_thread_id = event.system_thread_id;
  out_event->exception_repeated = event.raw.exception_repeated;
  switch (event.raw.kind) {
  case DMN_EventKind_CreateProcess: out_event->event_kind = RADVS_ComEventKind_CreateProcess; break;
  case DMN_EventKind_ExitProcess:   out_event->event_kind = RADVS_ComEventKind_ExitProcess; break;
  case DMN_EventKind_CreateThread:  out_event->event_kind = RADVS_ComEventKind_CreateThread; break;
  case DMN_EventKind_ExitThread:    out_event->event_kind = RADVS_ComEventKind_ExitThread; break;
  case DMN_EventKind_Halt:          out_event->event_kind = RADVS_ComEventKind_Break; break;
  case DMN_EventKind_Exception:     out_event->event_kind = RADVS_ComEventKind_Exception; break;
  case DMN_EventKind_Breakpoint:
  case DMN_EventKind_Trap:
  case DMN_EventKind_SingleStep:    out_event->event_kind = RADVS_ComEventKind_Stop; break;
  case DMN_EventKind_DebugString:   out_event->event_kind = RADVS_ComEventKind_DebugString; break;
  default:                          out_event->event_kind = RADVS_ComEventKind_Other; break;
  }
  return RADVS_ComResult_Ok;
}

RADVS_ComResult
radvs_com_session_copy_threads(RADVS_EngineSession *session, RADVS_ComThread *buffer, uint64_t buffer_count, uint64_t *out_count)
{
  if (out_count == 0 || (buffer == 0 && buffer_count != 0)) {
    return RADVS_ComResult_InvalidArgument;
  }

  U64 native_count = 0;
  RADVS_Result result = radvs_engine_session_copy_threads(session, 0, 0, &native_count);
  if (result != RADVS_Result_Ok && result != RADVS_Result_OutOfMemory) {
    return (RADVS_ComResult)result;
  }
  *out_count = native_count;
  if (native_count > buffer_count) {
    return RADVS_ComResult_OutOfMemory;
  }
  if (native_count == 0) {
    return RADVS_ComResult_Ok;
  }

  Temp scratch = scratch_begin(0, 0);
  RADVS_ThreadDesc *threads = push_array_no_zero(scratch.arena, RADVS_ThreadDesc, native_count);
  result = radvs_engine_session_copy_threads(session, threads, native_count, &native_count);
  if (result == RADVS_Result_Ok) {
    for (U64 index = 0; index < native_count; index += 1) {
      buffer[index].system_thread_id = threads[index].system_thread_id;
      buffer[index].state = threads[index].state;
    }
    *out_count = native_count;
  }
  scratch_end(scratch);
  return (RADVS_ComResult)result;
}
