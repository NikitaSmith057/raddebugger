// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include <windows.h>
#include <msdbg.h>

#include "base/base_inc.h"

#define RADVS_AD7_BRIDGE_ABI_VERSION 1u

#if defined(_WIN32)
# define RADVS_AD7_EXPORT __declspec(dllexport)
# define RADVS_AD7_CALL   __cdecl
#else
# define RADVS_AD7_EXPORT
# define RADVS_AD7_CALL
#endif

typedef struct RADVS_AD7_Session RADVS_AD7_Session;

#ifdef __cplusplus
extern "C" {
#endif

RADVS_AD7_EXPORT U32 RADVS_AD7_CALL radvs_ad7_bridge_abi_version(void);

RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_session_create (RADVS_AD7_Session **out_session);
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_session_destroy(RADVS_AD7_Session *session);

// Launch strings are borrowed for the duration of this call. The engine copies
// everything it needs before returning.
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_session_launch(RADVS_AD7_Session *session,
                                                                        String8 exe,
                                                                        String8 args,
                                                                        String8 wdir,
                                                                        AD_PROCESS_ID *out_process_id);

// Sequence zero starts the launch after the initial ProgramCreate event. A
// nonzero sequence acknowledges the matching synchronous ProgramDestroy event.
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_continue_synchronous_event(RADVS_AD7_Session *session,
                                                                                     U64 sequence);
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_run(RADVS_AD7_Session *session);
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_break(RADVS_AD7_Session *session);
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_terminate(RADVS_AD7_Session *session);
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_close_event_wait(RADVS_AD7_Session *session);

// All output String8 values use caller-owned storage. On entry, str points at
// the buffer and size is its capacity. On return, size is the required/actual
// byte count. A short buffer returns ERROR_INSUFFICIENT_BUFFER. For event text,
// the event remains pending and the same sequence is returned by the retry.
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_wait_event(RADVS_AD7_Session *session,
                                                                    DWORD timeout_ms,
                                                                    GUID *out_event_iid,
                                                                    DWORD *out_attributes,
                                                                    U64 *out_sequence,
                                                                    DWORD *out_exit_code,
                                                                    THREADPROPERTIES *out_thread,
                                                                    EXCEPTION_INFO *out_exception,
                                                                    String8 *in_out_text);

RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_get_program_name(RADVS_AD7_Session *session, String8 *in_out_name);
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_get_host_name(RADVS_AD7_Session *session, GETHOSTNAME_TYPE type, String8 *in_out_name);
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_get_host_pid(RADVS_AD7_Session *session, AD_PROCESS_ID *out_process_id);
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_get_host_machine_name(RADVS_AD7_Session *session, String8 *in_out_name);
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_get_engine_info(RADVS_AD7_Session *session, String8 *in_out_name, GUID *out_engine_id);
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_copy_threads(RADVS_AD7_Session *session, THREADPROPERTIES *buffer, U64 buffer_count, U64 *out_count);
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_get_thread_properties(RADVS_AD7_Session *session, DWORD system_thread_id, THREADPROPERTY_FIELDS fields, THREADPROPERTIES *out_properties, String8 *in_out_name);

#ifdef __cplusplus
}
#endif
