// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/radvs_format.h"
#include "radvs/radvs_symbol_service.h"

typedef struct RADVS_Engine  RADVS_Engine;
typedef struct RADVS_EngineSession RADVS_EngineSession;
typedef struct RADVS_Entity  RADVS_Entity;
typedef struct RADVS_Process RADVS_Process;
typedef struct RADVS_Thread  RADVS_Thread;
typedef struct RADVS_Module  RADVS_Module;
typedef struct RADVS_Trap    RADVS_Trap;

typedef struct
{
  RADVS_Entity *first;
  RADVS_Entity *last;
} RADVS_EntityList;

typedef struct
{
  RADVS_EntityList processes;
  RADVS_EntityList user_breakpoints;
  RADVS_EntityList internal_breakpoints;
  RADVS_EntityList traps;
} RADVS_Root;

typedef enum
{
  RADVS_EntityType_Null,
  RADVS_EntityType_Root,
  RADVS_EntityType_Process,
  RADVS_EntityType_Thread,
  RADVS_EntityType_Module,
  RADVS_EntityType_Breakpoint,
  RADVS_EntityType_Trap,
  RADVS_EntityType_Count,
} RADVS_EntityType;

struct RADVS_Process
{
  RADVS_ProcessState    state;
  U64                   process_state_epoch;
  U32                   system_process_id;
  DMN_Handle            parent_process;
  B32                   create_process_received;
  RADVS_EntityList      child_processes;
  RADVS_EntityList      threads;
  RADVS_EntityList      modules;
  RADVS_EventList       pending_public_events;
};

typedef enum
{
  RADVS_RunIntent_Null,
  RADVS_RunIntent_Continue,
  RADVS_RunIntent_Step,
  RADVS_RunIntent_Breakpoint,
  RADVS_RunIntent_ExceptionDisposition,
} RADVS_RunIntent;

typedef enum RADVS_ThreadState
{
  RADVS_ThreadState_Null,
  RADVS_ThreadState_Busy,
  RADVS_ThreadState_Idle,
} RADVS_ThreadState;

struct RADVS_Thread
{
  RADVS_ThreadState state;
  Arch              arch;
  U64               instruction_pointer;
  U32               system_thread_id;
  RADVS_RunIntent   run_intent;
  RADVS_TrapID      stop_trap_id;
};

struct RADVS_Module
{
  U64                      base_address;
  U64                      size;
  String8                  path;
  String8                  debug_info_path;
  Guid                     debug_info_guid;
  U64                      debug_info_timestamp;
  U64                      debug_info_age;
  RADVS_SymbolTicketState  symbol_state;
  RADVS_SymbolTicket      *symbol_ticket;
};

typedef struct RADVS_BreakpointBinding RADVS_BreakpointBinding;
struct RADVS_BreakpointBinding
{
  RADVS_BreakpointBinding *next;
  RADVS_BreakpointBinding *next_in_trap;
  RADVS_Entity            *breakpoint;
  RADVS_Entity            *trap;
  B32                      enabled;
  U32                      requested_mode;
  RADVS_Result             result;
};

struct RADVS_Trap
{
  DMN_Trap                 dmn;
  RADVS_BreakpointBinding *first_binding;
  RADVS_BreakpointBinding *last_binding;
  U32                      binding_count;
  U32                      enabled_binding_count;
  U32                      effective_mode;
  RADVS_Result             binding_result;
  U64                      changed_generation;
};

typedef struct
{
  RADVS_BreakpointInfo     info;
  U32                      visibility;
  U32                      source_projection_pending;
  RADVS_BreakpointBinding *first_binding;
  RADVS_BreakpointBinding *last_binding;
  RADVS_EntityList         address_breakpoints;
} RADVS_Breakpoint;

typedef void RADVS_ModuleVisitor(void *user_data, DMN_Handle module_handle, const RADVS_Module *module);
typedef void RADVS_ThreadVisitor(void *user_data, DMN_Handle thread_handle, const RADVS_Thread *thread);

#ifdef __cplusplus
extern "C" {
#endif

RADVS_Result radvs_engine_alloc                           (RADVS_SymbolService *symbol_service, RADVS_Engine **out_engine);
RADVS_Result radvs_engine_release                         (RADVS_Engine *engine);
RADVS_Result radvs_engine_session_alloc                   (RADVS_Engine *engine, RADVS_EngineSession **out_session);
RADVS_Result radvs_engine_session_release                 (RADVS_EngineSession *session);
RADVS_Result radvs_engine_session_launch                  (RADVS_EngineSession *session, String8 exe, String8 args, String8 wdir, U32 *out_pid);
RADVS_Result radvs_engine_session_launch16                (RADVS_EngineSession *session, String16 exe, String16 args, String16 wdir, U32 *out_pid);
RADVS_Result radvs_engine_session_run                     (RADVS_EngineSession *session, const DMN_Handle *process_handles, U64 process_handle_count);
RADVS_Result radvs_engine_session_step_thread             (RADVS_EngineSession *session, DMN_Handle thread_handle);
RADVS_Result radvs_engine_session_break                   (RADVS_EngineSession *session);
RADVS_Result radvs_engine_session_terminate               (RADVS_EngineSession *session, const DMN_Handle *process_handles, U64 process_handle_count);
RADVS_Result radvs_engine_session_evaluate_expression     (RADVS_EngineSession *session, String8 expression);
RADVS_Result radvs_engine_session_evaluate_expression16   (RADVS_EngineSession *session, String16 expression);
RADVS_Result radvs_engine_session_enumerate_modules       (RADVS_EngineSession *session, DMN_Handle process_handle, RADVS_ModuleVisitor *visitor, void *user_data);
RADVS_Result radvs_engine_session_enumerate_threads       (RADVS_EngineSession *session, DMN_Handle process_handle, RADVS_ThreadVisitor *visitor, void *user_data);
RADVS_Result radvs_engine_session_create_breakpoint       (RADVS_EngineSession *session, const RADVS_BreakpointSpec *spec, RADVS_BreakpointID *out_breakpoint_id);
RADVS_Result radvs_engine_session_alloc_breakpoint        (RADVS_EngineSession *session, const RADVS_BreakpointSpec *spec, RADVS_BreakpointID *out_breakpoint_id);
RADVS_Result radvs_engine_session_remove_breakpoint       (RADVS_EngineSession *session, RADVS_BreakpointID breakpoint_id);
RADVS_Result radvs_engine_session_set_breakpoint_enabled  (RADVS_EngineSession *session, RADVS_BreakpointID breakpoint_id, U32 enabled);
RADVS_Result radvs_engine_session_get_breakpoint_info     (RADVS_EngineSession *session, RADVS_BreakpointID breakpoint_id, RADVS_BreakpointInfo *out_info);
RADVS_Result radvs_engine_session_breakpoint_update_begin (RADVS_EngineSession *session);
RADVS_Result radvs_engine_session_breakpoint_update_end   (RADVS_EngineSession *session);
RADVS_Result radvs_engine_session_read_memory             (RADVS_EngineSession *session, DMN_Handle process_handle, U64 address, void *buffer, U64 size, U64 *out_size);
RADVS_Result radvs_engine_session_write_memory            (RADVS_EngineSession *session, DMN_Handle process_handle, U64 address, const void *buffer, U64 size, U64 *out_size);
RADVS_Result radvs_engine_session_read_registers          (RADVS_EngineSession *session, DMN_Handle thread_handle, void *buffer, U64 size, U64 *out_size);
RADVS_Result radvs_engine_session_write_registers         (RADVS_EngineSession *session, DMN_Handle thread_handle, const void *buffer, U64 size);
RADVS_Result radvs_engine_session_get_top_frame           (RADVS_EngineSession *session, DMN_Handle thread_handle, RADVS_FrameInfo *out_frame, char *source_path_buffer, U64 source_path_buffer_size, U64 *out_source_path_size);
RADVS_Result radvs_engine_session_get_process_desc        (RADVS_EngineSession *session, DMN_Handle process_handle, RADVS_ProcessDesc *out_desc);
RADVS_Result radvs_engine_session_get_thread_desc         (RADVS_EngineSession *session, DMN_Handle thread_handle, RADVS_ThreadDesc *out_desc);
RADVS_Result radvs_engine_session_copy_threads            (RADVS_EngineSession *session, RADVS_ThreadDesc *buffer, U64 buffer_count, U64 *out_count);
RADVS_Result radvs_engine_session_copy_modules            (RADVS_EngineSession *session, RADVS_ModuleDesc *buffer, U64 buffer_count, U64 *out_count);
RADVS_Result radvs_engine_session_run_thread              (RADVS_EngineSession *session, DMN_Handle thread_handle);
RADVS_Result radvs_engine_session_terminate_process       (RADVS_EngineSession *session, DMN_Handle process_handle);
void         radvs_engine_session_close_event_wait        (RADVS_EngineSession *session);
RADVS_Result radvs_engine_session_wait_event              (RADVS_EngineSession *session, U64 timeout_us);
RADVS_Result radvs_engine_session_poll_event              (RADVS_EngineSession *session, RADVS_Event *event_out);

#ifdef __cplusplus
}
#endif

