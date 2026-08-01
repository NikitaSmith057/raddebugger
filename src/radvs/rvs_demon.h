// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/rvs.h"
#include "radvs/rvs_protocol.h"

typedef struct RVS_Demon RVS_Demon;

typedef enum
{
  RVS_DemonMessage_Null,
  RVS_DemonMessage_Launch,
  RVS_DemonMessage_Run,
  RVS_DemonMessage_Halt,
  RVS_DemonMessage_Terminate,
  RVS_DemonMessage_Shutdown
} RVS_DemonMessageType;

typedef struct
{
  RVS_QueueMessage     base;
  RVS_DemonMessageType type;
  RVS_MessageID        request_id; // engine request that owns this completion
  struct {
    ProcessLaunchParams params;   // process launch params
  } launch;
  struct {
    // processes for DEMON to schedule for a run
    DMN_Handle *process_handles;
    U64         process_count;

    // run traps
    DMN_Trap *traps;
    U64       trap_count;
  } run;
  struct {
    DMN_Handle *process_handles;
    U64         process_count;
  } terminate;
} RVS_DemonMessage;

typedef enum
{
  RVS_DemonReplyKind_Null,
  RVS_DemonReplyKind_Launch,
  RVS_DemonReplyKind_Run,
  RVS_DemonReplyKind_Halt,
  RVS_DemonReplyKind_Terminate,
} RVS_DemonReplyKind;

typedef struct
{
  RVS_DemonReplyKind kind;
  union {
    struct {
      U32 pid;
    } launch;
  };
} RVS_DemonReply;

typedef enum
{
  RVS_DemonOutputKind_Null,
  RVS_DemonOutputKind_Reply,
  RVS_DemonOutputKind_EventBatch,
} RVS_DemonOutputKind;

typedef struct
{
  RVS_DemonOutputKind kind;
  RVS_MessageID       request_id;
  union {
    struct {
      RVS_Result      result;
      RVS_DemonReply  reply;
    } reply;
    struct {
      DMN_EventList events;
    } event_batch;
  };
} RVS_DemonOutput;

typedef void (RVS_DemonOutputCallback)(RVS_Demon *demon, RVS_DemonOutput *output, void *ud);

RVS_Result rvs_demon_init(void *output_ud, RVS_DemonOutputCallback *output_callback, RVS_Demon **dmn_out);
RVS_Result rvs_demon_shutdown(RVS_Demon *dmn);

internal RVS_Result rvs_demon_alloc       (void);
internal RVS_Result rvs_demon_release     (void);
internal void       rvs_demon_message_copy(Arena *arena, RVS_DemonMessage *dst, RVS_DemonMessage *src);
internal void       rvs_demon_event_copy  (Arena *arena, DMN_Event *dst, DMN_Event *src);
internal void       rvs_demon_output_copy (Arena *arena, RVS_DemonOutput *dst, RVS_DemonOutput *src);
internal RVS_Result rvs_demon_send_message(RVS_Demon *dmn, RVS_DemonMessage message_spec, RVS_MessageID *reply_id_out);



