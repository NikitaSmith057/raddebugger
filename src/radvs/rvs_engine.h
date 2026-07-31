// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

typedef U64 RVS_ProgramID;
typedef struct RVS_Engine RVS_Engine;

typedef enum
{
  RVS_EngineMessageType_Null,
  RVS_EngineMessageType_DemonReply,
} RVS_EngineMessageType;

typedef struct
{
  RVS_EngineMessageType type;
  union {
    RVS_DemonReply demon_reply;
  };
} RVS_EngineMessage;

typedef struct RVS_Program RVS_Program;
struct RVS_Program
{
  RVS_Program *next;
  Arena       *arena;
  U32          pid;
};

#define RVS_EVENT_XLIST \
  X(Launch, "Launch")

typedef enum
{
#define X(id, ...) RVS_EventKind_##id,
  RVS_EVENT_XLIST
#undef X
} RVS_EventKind;

typedef struct
{
  RVS_EventKind kind;
  struct {
    RVS_ProgramID program_id;
    U32           pid;
  } launch;
} RVS_Event;

