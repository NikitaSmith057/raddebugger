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
    RVS_DemonMessage demon_reply; // snapshot of a demon reply message
  };
} RVS_EngineMessage;

typedef struct RVS_EngineReply RVS_EngineReply;
struct RVS_EngineReply
{
  RVS_EngineReply  *next;
  RVS_EngineReply  *prev;
  RVS_EngineMessage message;
  RVS_MessageID       id;
  RVS_Result        result;
};

typedef struct RVS_Program RVS_Program;
struct RVS_Program
{
  RVS_Program *next;
  Arena       *arena;
  U32          pid;
};

#define RVS_EVENT_XLIST  \
  X(Launch, "LaunchAck") \

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
    RVS_MessageID reply_id; // message identifier for the completion reply
    U32           pid;
  } launch_ack;
} RVS_Event;

typedef enum
{
  RVS_NotificationKind_Null,
  RVS_NotificationKind_Reply,
  RVS_NotificationKind_Event
} RVS_NotificationKind;

typedef struct
{
  RVS_NotificationKind kind;
  union {
    RVS_Reply reply;
    RVS_Event event;
  };
} RVS_Notification;

RVS_Result rvs_engine_wait_for_notification(Arena *arena, RVS_Engine *engine, U64 wait_us, RVS_Notification *notification_out);

