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
  RVS_DemonMessage_LaunchAck,
  RVS_DemonMessage_Run,
  RVS_DemonMessage_Halt,
  RVS_DemonMessage_Terminate,
  RVS_DemonMessage_Shutdown
} RVS_DemonMessageType;

typedef struct
{
  RVS_QueueMessage     base;
  RVS_DemonMessageType type;
  RVS_MessageID        reply_to; // engine request that owns this completion
  struct {
    ProcessLaunchParams params;   // process launch params
  } launch;
  struct {
    U32 pid;
  } launch_ack;
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

typedef RVS_MessageID (RVS_DemonReplyCallback)(RVS_MessageID reply_id, RVS_DemonMessage *reply, void *ud);

RVS_Result rvs_demon_init(void *reply_ud, RVS_DemonReplyCallback *reply_callback, RVS_Demon **dmn_out);
RVS_Result rvs_demon_shutdown(void);

internal RVS_Result rvs_demon_alloc       (void);
internal RVS_Result rvs_demon_release     (void);
internal void       rvs_demon_message_copy(Arena *arena, RVS_DemonMessage *dst, RVS_DemonMessage *src);
internal RVS_Result rvs_demon_send_message(RVS_Demon *dmn, RVS_DemonMessage message_spec, RVS_MessageID *reply_id_out);



