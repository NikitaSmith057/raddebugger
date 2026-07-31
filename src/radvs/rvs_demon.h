// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/rvs.h"
#include "radvs/rvs_protocol.h"

typedef struct RVS_Demon RVS_Demon;

typedef enum
{
  RVS_DemonRequest_Null,
  RVS_DemonRequest_Launch,
  RVS_DemonRequest_Run,
  RVS_DemonRequest_Halt,
  RVS_DemonRequest_Terminate,
  RVS_DemonRequest_Shutdown
} RVS_DemonRequestType;

typedef struct
{
  RVS_QueueMessage     base;
  RVS_DemonRequestType type;
  struct {
    ProcessLaunchParams params;   // process launch params
    U32                 *pid_out; // PID of the launched processs
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
  RVS_DemonReplyType_Null,
  RVS_DemonReplyType_LaunchAck,
} RVS_DemonReplyType;

typedef struct
{
  RVS_ReplyID        id;
  RVS_DemonReplyType type;
  union {
    struct {
      U32 pid;
    } launch_ack;
  };
} RVS_DemonReply;

RVS_Result rvs_demon_init(void *reply_ud, RVS_ReplyCallback *reply_callback, RVS_Demon **dmn_out);
RVS_Result rvs_demon_shutdown(void);

internal RVS_Result rvs_demon_alloc       (void);
internal RVS_Result rvs_demon_release     (void);
internal RVS_Result rvs_demon_send_message(RVS_Demon *dmn, RVS_DemonMessage message_spec, RVS_ReplyID *reply_id_out);



