// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include <windows.h>
#include <oleauto.h>

#pragma warning(push)
#pragma warning(disable: 5287)
#include <msdbg.h>
#pragma warning(pop)

#include "base/base_inc.h"

#define RADVS_AD7_BRIDGE_ABI_VERSION 2u

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

// Output strings are UTF-16 BSTRs allocated by the bridge. A null BSTR denotes
// an empty string. The caller owns non-null results and must call SysFreeString.
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_wait_event(RADVS_AD7_Session *session,
                                                                    DWORD timeout_ms,
                                                                    GUID *out_event_iid,
                                                                    DWORD *out_attributes,
                                                                    U64 *out_sequence,
                                                                    DWORD *out_exit_code,
                                                                    THREADPROPERTIES *out_thread,
                                                                    EXCEPTION_INFO *out_exception,
                                                                    BSTR *out_text);

RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_get_program_name(RADVS_AD7_Session *session, BSTR *out_name);
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_get_host_name(RADVS_AD7_Session *session, GETHOSTNAME_TYPE type, BSTR *out_name);
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_get_host_pid(RADVS_AD7_Session *session, AD_PROCESS_ID *out_process_id);
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_get_host_machine_name(RADVS_AD7_Session *session, BSTR *out_name);
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_get_engine_info(RADVS_AD7_Session *session, BSTR *out_name, GUID *out_engine_id);
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_copy_threads(RADVS_AD7_Session *session, THREADPROPERTIES *buffer, U64 buffer_count, U64 *out_count);
RADVS_AD7_EXPORT HRESULT RADVS_AD7_CALL radvs_ad7_bridge_get_thread_properties(RADVS_AD7_Session *session, DWORD system_thread_id, THREADPROPERTY_FIELDS fields, THREADPROPERTIES *out_properties, BSTR *out_name);

//
// A7 COM-Interface
//

#define Bridge_A7_Com_XList \
  X(HRESULT, IDebugEvent2_GetAttributes,                   RADVS_AD7_Session *session, DWORD *pdwAttrib) \
  X(HRESULT, IDebugEngineLaunch2_LaunchSuspended,          RADVS_AD7_Session *session, LPCOLESTR pszServer, IDebugPort2 *pPort, LPCOLESTR pszExe, LPCOLESTR pszArgs, LPCOLESTR pszDir, BSTR bstrEnv, LPCOLESTR pszOptions, LAUNCH_FLAGS dwLaunchFlags, DWORD hStdInput, DWORD hStdOutput, DWORD hStdError, IDebugEventCallback2 *pCallback, IDebugProcess2 **ppProcess) \
  X(HRESULT, IDebugEngineLaunch2_ResumeProcess,            RADVS_AD7_Session *session, IDebugProcess2 *pProcess) \
  X(HRESULT, IDebugEngineLaunch2_CanTerminateProcess,      RADVS_AD7_Session *session, IDebugProcess2 *pProcess) \
  X(HRESULT, IDebugEngineLaunch2_TerminateProcess,         RADVS_AD7_Session *session, IDebugProcess2 *pProcess) \
  X(HRESULT, IDebugEngine2_EnumPrograms,                   RADVS_AD7_Session *session, IEnumDebugPrograms2 **ppEnum) \
  X(HRESULT, IDebugEngine2_Attach,                         RADVS_AD7_Session *session, IDebugProgram2 **rgpPrograms, IDebugProgramNode2 **rgpProgramNodes, DWORD celtPrograms, IDebugEventCallback2 *pCallback, ATTACH_REASON dwReason) \
  X(HRESULT, IDebugEngine2_CreatePendingBreakpoint,        RADVS_AD7_Session *session, IDebugBreakpointRequest2 *pBPRequest, IDebugPendingBreakpoint2 **ppPendingBP) \
  X(HRESULT, IDebugEngine2_SetException,                   RADVS_AD7_Session *session, EXCEPTION_INFO *pException) \
  X(HRESULT, IDebugEngine2_RemoveSetException,             RADVS_AD7_Session *session, EXCEPTION_INFO *pException) \
  X(HRESULT, IDebugEngine2_RemoveAllSetExceptions,         RADVS_AD7_Session *session, REFGUID guidType) \
  X(HRESULT, IDebugEngine2_GetEngineId,                    RADVS_AD7_Session *session, GUID *pguidEngine) \
  X(HRESULT, IDebugEngine2_DestroyProgram,                 RADVS_AD7_Session *session, IDebugProgram2 *pProgram) \
  X(HRESULT, IDebugEngine2_ContinueFromSynchronousEvent,   RADVS_AD7_Session *session, IDebugEvent2 *pEvent) \
  X(HRESULT, IDebugEngine2_SetLocale,                      RADVS_AD7_Session *session, WORD wLangID) \
  X(HRESULT, IDebugEngine2_SetRegistryRoot,                RADVS_AD7_Session *session, LPCOLESTR pszRegistryRoot) \
  X(HRESULT, IDebugEngine2_SetMetric,                      RADVS_AD7_Session *session, LPCOLESTR pszMetric, VARIANT varValue) \
  X(HRESULT, IDebugEngine2_CauseBreak,                     RADVS_AD7_Session *session) \
  X(HRESULT, IDebugProgram2_EnumThreads,                   RADVS_AD7_Session *session, IEnumDebugThreads2 **ppEnum) \
  X(HRESULT, IDebugProgram2_GetName,                       RADVS_AD7_Session *session, BSTR *pbstrName) \
  X(HRESULT, IDebugProgram2_GetProcess,                    RADVS_AD7_Session *session, IDebugProcess2 **ppProcess) \
  X(HRESULT, IDebugProgram2_Terminate,                     RADVS_AD7_Session *session) \
  X(HRESULT, IDebugProgram2_Attach,                        RADVS_AD7_Session *session, IDebugEventCallback2 *pCallback) \
  X(HRESULT, IDebugProgram2_CanDetach,                     RADVS_AD7_Session *session) \
  X(HRESULT, IDebugProgram2_Detach,                        RADVS_AD7_Session *session) \
  X(HRESULT, IDebugProgram2_GetProgramId,                  RADVS_AD7_Session *session, GUID *pguidProgramId) \
  X(HRESULT, IDebugProgram2_GetDebugProperty,              RADVS_AD7_Session *session, IDebugProperty2 **ppProperty) \
  X(HRESULT, IDebugProgram2_Execute,                       RADVS_AD7_Session *session) \
  X(HRESULT, IDebugProgram2_Continue,                      RADVS_AD7_Session *session, IDebugThread2 *pThread) \
  X(HRESULT, IDebugProgram2_Step,                          RADVS_AD7_Session *session, IDebugThread2 *pThread, STEPKIND sk, STEPUNIT step) \
  X(HRESULT, IDebugProgram2_CauseBreak,                    RADVS_AD7_Session *session) \
  X(HRESULT, IDebugProgram2_GetEngineInfo,                 RADVS_AD7_Session *session, BSTR *pbstrEngine, GUID *pguidEngine) \
  X(HRESULT, IDebugProgram2_EnumCodeContexts,              RADVS_AD7_Session *session, IDebugDocumentPosition2 *pDocPos, IEnumDebugCodeContexts2 **ppEnum) \
  X(HRESULT, IDebugProgram2_GetMemoryBytes,                RADVS_AD7_Session *session, IDebugMemoryBytes2 **ppMemoryBytes) \
  X(HRESULT, IDebugProgram2_GetDisassemblyStream,          RADVS_AD7_Session *session, DISASSEMBLY_STREAM_SCOPE dwScope, IDebugCodeContext2 *pCodeContext, IDebugDisassemblyStream2 **ppDisassemblyStream) \
  X(HRESULT, IDebugProgram2_EnumModules,                   RADVS_AD7_Session *session, IEnumDebugModules2 **ppEnum) \
  X(HRESULT, IDebugProgram2_GetENCUpdate,                  RADVS_AD7_Session *session, IDebugENCUpdate **ppUpdate) \
  X(HRESULT, IDebugProgram2_EnumCodePaths,                 RADVS_AD7_Session *session, LPCOLESTR pszHint, IDebugCodeContext2 *pStart, IDebugStackFrame2 *pFrame, BOOL fSource, IEnumCodePaths2 **ppEnum, IDebugCodeContext2 **ppSafety) \
  X(HRESULT, IDebugProgram2_WriteDump,                     RADVS_AD7_Session *session, DUMPTYPE DumpType, LPCOLESTR pszDumpUrl) \
  X(HRESULT, IDebugProcess2_GetInfo,                       RADVS_AD7_Session *session, PROCESS_INFO_FIELDS Fields, PROCESS_INFO *pProcessInfo) \
  X(HRESULT, IDebugProcess2_EnumPrograms,                  RADVS_AD7_Session *session, IEnumDebugPrograms2 **ppEnum) \
  X(HRESULT, IDebugProcess2_GetName,                       RADVS_AD7_Session *session, GETNAME_TYPE gnType, BSTR *pbstrName) \
  X(HRESULT, IDebugProcess2_GetServer,                     RADVS_AD7_Session *session, IDebugCoreServer2 **ppServer) \
  X(HRESULT, IDebugProcess2_Terminate,                     RADVS_AD7_Session *session) \
  X(HRESULT, IDebugProcess2_Attach,                        RADVS_AD7_Session *session, IDebugEventCallback2 *pCallback, GUID *rgguidSpecificEngines, DWORD celtSpecificEngines, HRESULT *rghrEngineAttach) \
  X(HRESULT, IDebugProcess2_CanDetach,                     RADVS_AD7_Session *session) \
  X(HRESULT, IDebugProcess2_Detach,                        RADVS_AD7_Session *session) \
  X(HRESULT, IDebugProcess2_GetPhysicalProcessId,          RADVS_AD7_Session *session, AD_PROCESS_ID *pProcessId) \
  X(HRESULT, IDebugProcess2_GetProcessId,                  RADVS_AD7_Session *session, GUID *pguidProcessId) \
  X(HRESULT, IDebugProcess2_GetAttachedSessionName,        RADVS_AD7_Session *session, BSTR *pbstrSessionName) \
  X(HRESULT, IDebugProcess2_EnumThreads,                   RADVS_AD7_Session *session, IEnumDebugThreads2 **ppEnum) \
  X(HRESULT, IDebugProcess2_CauseBreak,                    RADVS_AD7_Session *session) \
  X(HRESULT, IDebugProcess2_GetPort,                       RADVS_AD7_Session *session, IDebugPort2 **ppPort) \
  X(HRESULT, IDebugThread2_EnumFrameInfo,                  RADVS_AD7_Session *session, FRAMEINFO_FLAGS dwFieldSpec, UINT nRadix, IEnumDebugFrameInfo2 **ppEnum) \
  X(HRESULT, IDebugThread2_GetName,                        RADVS_AD7_Session *session, BSTR *pbstrName) \
  X(HRESULT, IDebugThread2_SetThreadName,                  RADVS_AD7_Session *session, LPCOLESTR pszName) \
  X(HRESULT, IDebugThread2_GetProgram,                     RADVS_AD7_Session *session, IDebugProgram2 **ppProgram) \
  X(HRESULT, IDebugThread2_CanSetNextStatement,            RADVS_AD7_Session *session, IDebugStackFrame2 *pStackFrame, IDebugCodeContext2 *pCodeContext) \
  X(HRESULT, IDebugThread2_SetNextStatement,               RADVS_AD7_Session *session, IDebugStackFrame2 *pStackFrame, IDebugCodeContext2 *pCodeContext) \
  X(HRESULT, IDebugThread2_GetThreadId,                    RADVS_AD7_Session *session, DWORD *pdwThreadId) \
  X(HRESULT, IDebugThread2_Suspend,                        RADVS_AD7_Session *session, DWORD *pdwSuspendCount) \
  X(HRESULT, IDebugThread2_Resume,                         RADVS_AD7_Session *session, DWORD *pdwSuspendCount) \
  X(HRESULT, IDebugThread2_GetThreadProperties,            RADVS_AD7_Session *session, THREADPROPERTY_FIELDS dwFields, THREADPROPERTIES *ptp) \
  X(HRESULT, IDebugThread2_GetLogicalThread,               RADVS_AD7_Session *session, IDebugStackFrame2 *pStackFrame, IDebugLogicalThread2 **ppLogicalThread) \
  X(HRESULT, IDebugMemoryBytes2_ReadAt,                    RADVS_AD7_Session *session, IDebugMemoryContext2 *pStartContext, DWORD dwCount, BYTE *rgbMemory, DWORD *pdwRead, DWORD *pdwUnreadable) \
  X(HRESULT, IDebugMemoryBytes2_WriteAt,                   RADVS_AD7_Session *session, IDebugMemoryContext2 *pStartContext, DWORD dwCount, BYTE *rgbMemory) \
  X(HRESULT, IDebugMemoryBytes2_GetSize,                   RADVS_AD7_Session *session, UINT64 *pqwSize)

#define X(ret, fn, ...) RADVS_AD7_EXPORT ret fn(__VA_ARGS__);
  Bridge_A7_Com_XList 
#undef X

#ifdef __cplusplus
}
#endif
