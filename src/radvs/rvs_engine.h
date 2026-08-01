// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/rvs_demon.h"

typedef U64 RVS_ProgramID;
typedef struct RVS_Engine RVS_Engine;

typedef enum
{
  RVS_EngineMessageType_Null,
  RVS_EngineMessageType_Launch,
  RVS_EngineMessageType_DemonReply,
  RVS_EngineMessageType_Shutdown,
} RVS_EngineMessageType;

typedef struct
{
  RVS_QueueMessage       base;
  RVS_EngineMessageType  type;
  union {
    struct {
      ProcessLaunchParams params;
    } launch;
    struct {
      RVS_DemonMessage *message;
      ArenaNode        *arena_node;
      RVS_MessageID     reply_id;
    } demon_reply;
  };
} RVS_EngineMessage;

typedef enum
{
  RVS_ReplyKind_Null,
  RVS_ReplyKind_LaunchAck,
} RVS_ReplyKind;

typedef struct
{
  RVS_MessageID reply_id;
  RVS_Result    result;
  RVS_ReplyKind kind;
  union {
    struct {
      RVS_ProgramID program_id;
      U32           pid;
    } launch_ack;
  };
} RVS_Reply;

typedef struct RVS_EngineReply RVS_EngineReply;
struct RVS_EngineReply
{
  RVS_EngineReply *next;
  RVS_EngineReply *prev;
  RVS_Reply        reply;
};

typedef struct RVS_Program RVS_Program;
struct RVS_Program
{
  RVS_Program   *next;
  Arena         *arena;
  RVS_ProgramID  id;
  U32            pid;
};

typedef enum
{
  RVS_EventKind_Null,
} RVS_EventKind;

typedef struct
{
  RVS_EventKind kind;
} RVS_Event;

RVS_Result rvs_engine_init(RVS_Engine **engine_out);
void       rvs_engine_shutdown(RVS_Engine *engine);

RVS_Result rvs_engine_launch_async(RVS_Engine *engine, String8 cmdl, String8 wdir, RVS_MessageID *reply_id_out);
RVS_Result rvs_engine_wait_for_reply(Arena *arena, RVS_Engine *engine, RVS_MessageID reply_id, U64 wait_us, RVS_Reply *reply_out);
RVS_Result rvs_engine_launch(RVS_Engine *engine, String8 cmdl, String8 wdir, U64 wait_us, U32 *pid_out);

RVS_Result rvs_engine_wait_for_event(RVS_Engine *engine, U64 wait_us, RVS_Event *event_out);
