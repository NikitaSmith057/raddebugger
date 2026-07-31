// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "base/base_inc.h"
#include "arch/arch_inc.h"
#include "demon/demon_core.h"

#define RADVS_Result_XList              \
  X(Null,            "unknown")         \
  X(Ok,              "ok")              \
  X(InvalidArgument, "invalid argument")\
  X(OutOfMemory,     "out of memory")   \
  X(NotImplemented,  "not implemented") \
  X(AbiMismatch,     "ABI mismatch")    \
  X(Busy,            "busy")            \
  X(Timeout,         "timeout")         \
  X(Conflict,        "conflict")

typedef enum RADVS_Result
{
#define X(name, string) RADVS_Result_##name,
  RADVS_Result_XList
#undef X
} RADVS_Result;

typedef U64 RADVS_EntityID;
typedef U64 RADVS_BreakpointID;
typedef U64 RADVS_TrapID;

typedef struct RADVS_FrameInfo
{
  U64 process_state_epoch;
  U64 instruction_pointer;
  U64 module_base_address;
  U64 source_voff_first;
  U64 source_voff_opl;
  U32 source_line;
  U32 source_column;
  U32 has_source;
  U32 reserved;
} RADVS_FrameInfo;

typedef enum RADVS_ProcessState
{
  RADVS_ProcessState_Null,
  RADVS_ProcessState_Launching,
  RADVS_ProcessState_Running,
  RADVS_ProcessState_Stopped,
  RADVS_ProcessState_Exited,
} RADVS_ProcessState;

typedef struct RADVS_Event
{
  DMN_Event          raw;
  DMN_Handle         process_handle;
  DMN_Handle         parent_process_handle;
  DMN_Handle         thread_handle;
  DMN_Handle         module_handle;
  U64                process_state_epoch;
  U32                system_process_id;
  U32                system_thread_id;
  U32                thread_created;
  U32                session_process_count;
  RADVS_BreakpointID breakpoint_id;
} RADVS_Event;

typedef struct RADVS_EventNode
{
  struct RADVS_EventNode *next;
  RADVS_Event             v;
} RADVS_EventNode;

typedef struct RADVS_EventList
{
  RADVS_EventNode *first;
  RADVS_EventNode *last;
  U64              count;
} RADVS_EventList;

internal void radvs_event_list_push  (RADVS_EventList *list, RADVS_EventNode *node);
internal void radvs_event_list_concat(RADVS_EventList *dst, RADVS_EventList *src);

typedef struct RADVS_ProcessDesc
{
  DMN_Handle process_handle;
  DMN_Handle parent_process_handle;
  U32        system_process_id;
  U32        state;
} RADVS_ProcessDesc;

typedef struct RADVS_ThreadDesc
{
  DMN_Handle thread_handle;
  DMN_Handle process_handle;
  U64        process_state_epoch;
  U32        system_thread_id;
  U32        state;
} RADVS_ThreadDesc;

typedef struct RADVS_ModuleDesc
{
  DMN_Handle module_handle;
  DMN_Handle process_handle;
  U64        base_address;
  U64        size;
  U32        symbol_state;
  U32        reserved;
} RADVS_ModuleDesc;

typedef enum RADVS_BreakpointVisibility
{
  RADVS_BreakpointVisibility_User,
  RADVS_BreakpointVisibility_Internal,
} RADVS_BreakpointVisibility;

typedef enum RADVS_BreakpointLocationKind
{
  RADVS_BreakpointLocationKind_Address,
  RADVS_BreakpointLocationKind_Source,
} RADVS_BreakpointLocationKind;

typedef enum RADVS_BreakpointConditionKind
{
  RADVS_BreakpointConditionKind_Always,
  RADVS_BreakpointConditionKind_Expression,
} RADVS_BreakpointConditionKind;

typedef enum RADVS_AddressBreakpointMode
{
  RADVS_AddressBreakpointMode_Auto,
  RADVS_AddressBreakpointMode_Software,
  RADVS_AddressBreakpointMode_Hardware,
} RADVS_AddressBreakpointMode;

typedef struct RADVS_SourceBreakpointSpec
{
  String8 path;
  U32     line;
  U32     column;
} RADVS_SourceBreakpointSpec;

typedef struct RADVS_BreakpointSpec
{
  U32 location_kind;
  U32 condition_kind;
  U32 enabled;
  U32 address_mode;
  union {
    U64                      address;
    RADVS_SourceBreakpointSpec source;
  };
  String8 condition_expression;
} RADVS_BreakpointSpec;

typedef struct RADVS_BreakpointInfo
{
  RADVS_BreakpointID   breakpoint_id;
  RADVS_BreakpointSpec spec;
  U32                  binding_count;
  U32                  enabled_binding_count;
  RADVS_Result         binding_result;
} RADVS_BreakpointInfo;

internal void radvs_event_list_push(RADVS_EventList *list, RADVS_EventNode *node);
internal void radvs_event_list_concat(RADVS_EventList *dst, RADVS_EventList *src);
