// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

#include "radvs/radvs_format.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BRIDGE_NO_A7_COM_INTERFACE

typedef struct RADVS_Session RADVS_Session;

typedef enum RADVS_AD7_EventKind
{
  RADVS_AD7_EventKind_Null,
  RADVS_AD7_EventKind_ProgramCreated,
  RADVS_AD7_EventKind_LoadComplete,
  RADVS_AD7_EventKind_ThreadCreated,
  RADVS_AD7_EventKind_ThreadExited,
  RADVS_AD7_EventKind_Stopped,
  RADVS_AD7_EventKind_Output,
  RADVS_AD7_EventKind_ProgramDestroyed,
} RADVS_AD7_EventKind;

// values from enum_EVENTATTRIBUTES
typedef enum RADVS_AD7_EventAttributes
{
  RADVS_AD7_EventAttributes_Asynchronous = 1,
  RADVS_AD7_EventAttributes_Synchronous  = 2,
  RADVS_AD7_EventAttributes_AsyncStop    = 4,
} RADVS_AD7_EventAttributes;

typedef enum RADVS_AD7_StopReason
{
  RADVS_AD7_StopReason_Null,
  RADVS_AD7_StopReason_Generic,
  RADVS_AD7_StopReason_Step,
  RADVS_AD7_StopReason_Exception,
  RADVS_AD7_StopReason_Break,
} RADVS_AD7_StopReason;

typedef struct RADVS_AD7_Event
{
  RADVS_Event event;
  uint64_t    sequence;
  uint64_t    text_size;
  uint32_t    kind;
  uint32_t    attributes;
  uint32_t    stop_reason;
} RADVS_AD7_Event;

typedef struct RADVS_AD7_ProgramDesc
{
  DMN_Handle process_handle;
  DMN_Handle parent_process_handle;
  uint32_t   system_process_id;
  uint32_t   state;
  uint64_t   program_name_offset;
  uint64_t   program_name_size;
  uint64_t   host_name_offset;
  uint64_t   host_name_size;
  uint64_t   engine_name_offset;
  uint64_t   engine_name_size;
  uint64_t   engine_id_offset;
  uint64_t   engine_id_size;
} RADVS_AD7_ProgramDesc;

//
// ABI
//

#define RADVS_BRIDGE_ABI_VERSION 21u

#ifdef _WIN32
# define RADVS_EXPORT __declspec(dllexport)
# define RADVS_CALL   __cdecl
#else
# define RADVS_EXPORT
# define RADVS_CALL
#endif

#define BRIDGE_FN(name) name

RADVS_EXPORT uint32_t RADVS_CALL radvs_bridge_abi_version(void);

//
// A7 COM-Interface
//

#define Bridge_A7_Com_XList \
  X(int32_t, IDebugEvent2_Wait,                            RADVS_Session *session, uint32_t timeout_ms, RADVS_AD7_Event *out_event, char *text_buffer, uint64_t text_buffer_size, uint64_t *out_text_size) \
  X(int32_t, IDebugEvent2_Acknowledge,                     RADVS_Session *session, uint64_t sequence) \
  X(int32_t, IDebugEvent2_GetAttributes,                   RADVS_Session *session, uint64_t sequence, uint32_t *out_attributes) \
  X(int32_t, IDebugEngineLaunch2_LaunchSuspended,          const wchar_t *exe, const wchar_t *cmd_line, const wchar_t *wdir, RADVS_Session **session /* out */, uint32_t *system_pid /* out */) \
  X(int32_t, IDebugEngineLaunch2_CanTerminateProcess,      RADVS_Session *session, DMN_Handle process_handle) \
  X(int32_t, IDebugEngineLaunch2_ResumeProcess,            RADVS_Session *session, DMN_Handle process_handle) \
  X(int32_t, IDebugEngineLaunch2_TerminateProcess,         RADVS_Session *session, DMN_Handle process_handle) \
  X(void,    IDebugEngine2_DestroySession,                 RADVS_Session *session) \
  X(int32_t, IDebugEngine2_Attach,                         RADVS_Session *session, void *programs, void *program_nodes, uint32_t program_count, void *event_callback, uint32_t attach_reason) \
  X(int32_t, IDebugEngine2_CauseBreak,                     RADVS_Session *session) \
  X(int32_t, IDebugEngine2_ContinueFromSynchronousEvent,   RADVS_Session *session) \
  X(int32_t, IDebugEngine2_CreatePendingBreakpoint,        RADVS_Session *session, const RADVS_BreakpointSpec *request, uint64_t *out_breakpoint_id) \
  X(int32_t, IDebugEngine2_CreateBreakpoint,               RADVS_Session *session, const wchar_t *source_path, uint32_t line, uint32_t column, uint64_t *out_breakpoint_id) \
  X(int32_t, IDebugEngine2_CreateAddressBreakpoint,        RADVS_Session *session, uint64_t address, uint32_t enabled, uint32_t address_mode, uint64_t *out_breakpoint_id) \
  X(int32_t, IDebugEngine2_SetBreakpointEnabled,           RADVS_Session *session, uint64_t breakpoint_id, uint32_t enabled) \
  X(int32_t, IDebugEngine2_DeleteBreakpoint,               RADVS_Session *session, uint64_t breakpoint_id) \
  X(int32_t, IDebugEngine2_DestroyProgram,                 RADVS_Session *session, void *program) \
  X(int32_t, IDebugEngine2_EnumPrograms,                   RADVS_Session *session, RADVS_AD7_ProgramDesc *buffer, uint64_t buffer_count, uint64_t *out_count, char *text_buffer, uint64_t text_buffer_size, uint64_t *out_text_size) \
  X(int32_t, IDebugEngine2_GetEngineId,                    RADVS_Session *session, void *out_engine_id) \
  X(int32_t, IDebugEngine2_RemoveAllSetExceptions,         RADVS_Session *session, const void *exception_type) \
  X(int32_t, IDebugEngine2_RemoveSetException,             RADVS_Session *session, const void *exception_info) \
  X(int32_t, IDebugEngine2_SetException,                   RADVS_Session *session, const void *exception_info) \
  X(int32_t, IDebugEngine2_SetLocale,                      RADVS_Session *session, uint16_t language_id) \
  X(int32_t, IDebugEngine2_SetMetric,                      RADVS_Session *session, const wchar_t *metric, void *value) \
  X(int32_t, IDebugEngine2_SetRegistryRoot,                RADVS_Session *session, const wchar_t *registry_root) \
  X(int32_t, IDebugProgram2_Attach,                        RADVS_Session *session, void *event_callback) \
  X(int32_t, IDebugProgram2_CanDetach,                     RADVS_Session *session) \
  X(int32_t, IDebugProgram2_GetDescriptor,                 RADVS_Session *session, RADVS_AD7_ProgramDesc *out_desc, char *text_buffer, uint64_t text_buffer_size, uint64_t *out_text_size) \
  X(int32_t, IDebugProgram2_GetDescriptorForProcess,       RADVS_Session *session, DMN_Handle process_handle, RADVS_AD7_ProgramDesc *out_desc, char *text_buffer, uint64_t text_buffer_size, uint64_t *out_text_size) \
  X(int32_t, IDebugProgram2_CopyThreads,                   RADVS_Session *session, RADVS_ThreadDesc *buffer, uint64_t buffer_count, uint64_t *out_count) \
  X(int32_t, IDebugProgram2_CopyModules,                   RADVS_Session *session, RADVS_ModuleDesc *buffer, uint64_t buffer_count, uint64_t *out_count) \
  X(int32_t, IDebugProgram2_Detach,                        RADVS_Session *session) \
  X(int32_t, IDebugProgram2_EnumCodeContexts,              RADVS_Session *session, void *document_position, void **out_contexts) \
  X(int32_t, IDebugProgram2_EnumCodePaths,                 RADVS_Session *session, const wchar_t *hint, void *start_context, void *stack_frame, int32_t source, void **out_paths, void **out_safety_context) \
  X(int32_t, IDebugProgram2_EnumModules,                   RADVS_Session *session, RADVS_ModuleDesc *buffer, uint64_t buffer_count, uint64_t *out_count) \
  X(int32_t, IDebugProgram2_EnumThreads,                   RADVS_Session *session, RADVS_ThreadDesc *buffer, uint64_t buffer_count, uint64_t *out_count) \
  X(int32_t, IDebugProgram2_Execute,                       RADVS_Session *session) \
  X(int32_t, IDebugProgram2_GetDebugProperty,              RADVS_Session *session, void **out_property) \
  X(int32_t, IDebugProgram2_GetDisassemblyStream,          RADVS_Session *session, uint32_t scope, void *code_context, void **out_stream) \
  X(int32_t, IDebugProgram2_GetENCUpdate,                  RADVS_Session *session, void **out_update) \
  X(int32_t, IDebugProgram2_GetEngineInfo,                 RADVS_Session *session, char *name_buffer, uint64_t name_buffer_size, uint64_t *out_name_size, void *out_engine_id) \
  X(int32_t, IDebugProgram2_GetMemoryBytes,                RADVS_Session *session, void **out_memory_bytes) \
  X(int32_t, IDebugProgram2_GetName,                       RADVS_Session *session, char *name_buffer, uint64_t name_buffer_size, uint64_t *out_name_size) \
  X(int32_t, IDebugProgram2_GetProcess,                    RADVS_Session *session, DMN_Handle *out_process_handle) \
  X(int32_t, IDebugProgram2_GetProgramId,                  RADVS_Session *session, void *out_program_id) \
  X(int32_t, IDebugProgram2_ResolveSourcePosition,         RADVS_Session *session, const wchar_t *source_path, uint32_t line, uint32_t column) \
  X(int32_t, IDebugProgram2_Continue,                      RADVS_Session *session, DMN_Handle thread_handle) \
  X(int32_t, IDebugProgram2_CauseBreak,                    RADVS_Session *session) \
  X(int32_t, IDebugProgram2_Step,                          RADVS_Session *session, DMN_Handle thread_handle, uint32_t step_kind, uint32_t step_unit) \
  X(int32_t, IDebugProgram2_Terminate,                     RADVS_Session *session) \
  X(int32_t, IDebugProgram2_WriteDump,                     RADVS_Session *session, uint32_t dump_type, const wchar_t *dump_url) \
  X(int32_t, IDebugProcess2_Attach,                        RADVS_Session *session, DMN_Handle process_handle, void *event_callback, const void *engine_ids, uint32_t engine_count, int32_t *out_engine_attach_results) \
  X(int32_t, IDebugProcess2_CanDetach,                     RADVS_Session *session, DMN_Handle process_handle) \
  X(int32_t, IDebugProcess2_CauseBreak,                    RADVS_Session *session, DMN_Handle process_handle) \
  X(int32_t, IDebugProcess2_Detach,                        RADVS_Session *session, DMN_Handle process_handle) \
  X(int32_t, IDebugProcess2_EnumPrograms,                  RADVS_Session *session, DMN_Handle process_handle, RADVS_AD7_ProgramDesc *buffer, uint64_t buffer_count, uint64_t *out_count, char *text_buffer, uint64_t text_buffer_size, uint64_t *out_text_size) \
  X(int32_t, IDebugProcess2_EnumThreads,                   RADVS_Session *session, DMN_Handle process_handle, RADVS_ThreadDesc *buffer, uint64_t buffer_count, uint64_t *out_count) \
  X(int32_t, IDebugProcess2_GetAttachedSessionName,        RADVS_Session *session, DMN_Handle process_handle, char *text_buffer, uint64_t text_buffer_size, uint64_t *out_text_size) \
  X(int32_t, IDebugProcess2_GetInfo,                       RADVS_Session *session, DMN_Handle process_handle, uint32_t fields, void *out_process_info) \
  X(int32_t, IDebugProcess2_GetName,                       RADVS_Session *session, DMN_Handle process_handle, uint32_t name_kind, char *text_buffer, uint64_t text_buffer_size, uint64_t *out_text_size) \
  X(int32_t, IDebugProcess2_GetPhysicalProcessId,          RADVS_Session *session, DMN_Handle process_handle, uint32_t *out_system_pid) \
  X(int32_t, IDebugProcess2_GetPort,                       RADVS_Session *session, DMN_Handle process_handle, void **out_port) \
  X(int32_t, IDebugProcess2_GetProcessId,                  RADVS_Session *session, DMN_Handle process_handle, void *out_process_id) \
  X(int32_t, IDebugProcess2_GetServer,                     RADVS_Session *session, DMN_Handle process_handle, void **out_server) \
  X(int32_t, IDebugProcess2_Terminate,                     RADVS_Session *session, DMN_Handle process_handle) \
  X(int32_t, IDebugThread2_CanSetNextStatement,            RADVS_Session *session, DMN_Handle thread_handle, void *stack_frame, void *code_context) \
  X(int32_t, IDebugThread2_EnumFrameInfo,                  RADVS_Session *session, DMN_Handle thread_handle, uint32_t field_spec, uint32_t radix, void **out_frames) \
  X(int32_t, IDebugThread2_GetName,                        RADVS_Session *session, DMN_Handle thread_handle, char *text_buffer, uint64_t text_buffer_size, uint64_t *out_text_size) \
  X(int32_t, IDebugThread2_GetLogicalThread,               RADVS_Session *session, DMN_Handle thread_handle, void *stack_frame, void **out_logical_thread) \
  X(int32_t, IDebugThread2_GetProgram,                     RADVS_Session *session, DMN_Handle thread_handle, void **out_program) \
  X(int32_t, IDebugThread2_GetThreadId,                    RADVS_Session *session, DMN_Handle thread_handle, uint32_t *out_thread_id) \
  X(int32_t, IDebugThread2_GetThreadProperties,            RADVS_Session *session, DMN_Handle thread_handle, uint32_t fields, void *out_thread_properties) \
  X(int32_t, IDebugThread2_GetTopFrame,                    RADVS_Session *session, DMN_Handle thread_handle, RADVS_FrameInfo *out_frame, char *source_path_buffer, uint64_t source_path_buffer_size, uint64_t *out_source_path_size) \
  X(int32_t, IDebugThread2_Resume,                         RADVS_Session *session, DMN_Handle thread_handle, uint32_t *out_suspend_count) \
  X(int32_t, IDebugThread2_SetNextStatement,               RADVS_Session *session, DMN_Handle thread_handle, void *stack_frame, void *code_context) \
  X(int32_t, IDebugThread2_SetThreadName,                  RADVS_Session *session, DMN_Handle thread_handle, const wchar_t *name) \
  X(int32_t, IDebugThread2_Step,                           RADVS_Session *session, DMN_Handle thread_handle, uint32_t step_kind, uint32_t step_unit) \
  X(int32_t, IDebugThread2_Suspend,                        RADVS_Session *session, DMN_Handle thread_handle, uint32_t *out_suspend_count) \
  X(int32_t, IDebugThread2_SetInstructionPointer,          RADVS_Session *session, DMN_Handle thread_handle, uint64_t address) \
  X(int32_t, IDebugMemoryBytes2_GetSize,                   RADVS_Session *session, DMN_Handle process_handle, uint64_t *out_size) \
  X(int32_t, IDebugMemoryBytes2_ReadAt,                    RADVS_Session *session, DMN_Handle process_handle, uint64_t address, uint32_t byte_count, void *buffer, uint32_t *out_read, uint32_t *out_unreadable) \
  X(int32_t, IDebugMemoryBytes2_WriteAt,                   RADVS_Session *session, DMN_Handle process_handle, uint64_t address, uint32_t byte_count, const void *buffer) \
  X(int32_t, IDebugMemoryBytes2_Read,                      RADVS_Session *session, DMN_Handle process_handle, uint64_t address, void *buffer, uint64_t buffer_size, uint64_t *out_size) \

#if !defined(BRIDGE_NO_A7_COM_INTERFACE)
# define X(ret, fn, ...) RADVS_EXPORT ret RADVS_CALL BRIDGE_FN(fn)(__VA_ARGS__);
Bridge_A7_Com_XList
# undef X
#endif

//
// Helpers
//

// TODO: factor out
#define RadVs_SessionScope(session, event_wait_us, result)                                \
  for (RADVS_Result enter_result = radvs_session_enter(session, event_wait_us), once = 0; \
       once == 0 && (result = enter_result) && result == RADVS_Result_Ok;                 \
       radvs_session_leave(session, event_wait_us), once = 1)

// TODO: print out function with arguments after the error message
#define RADVS_Result_NotImplemented(text, result)                                             \
  do {                                                                                        \
    result = RADVS_Result_NotImplemented;                                                     \
    radvs_fprintf(stderr, "RADVS: %s %s:%llu: %s\n", __FUNCTION__, __FILE__, __LINE__, text); \
  } while (0)

#define BRIDGE_NOT_IMPLEMENTED(result) \
  RADVS_Result_NotImplemented("BRIDGE: COM-interface attempted to access an end-point that is not implemented", result); \

#ifdef __cplusplus
}
#endif

