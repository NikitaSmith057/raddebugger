// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/rvs.h"
#include "radvs/rvs_async.h"

typedef struct RVS_Demon RVS_Demon;

typedef enum
{
  RVS_DemonInterruptCapability_Null,
  RVS_DemonInterruptCapability_GlobalWithResume,
} RVS_DemonInterruptCapability;

typedef enum
{
  RVS_DemonMessage_Null,
  RVS_DemonMessage_Launch,
  RVS_DemonMessage_Pump,
  RVS_DemonMessage_Run,
  RVS_DemonMessage_Resume,
  RVS_DemonMessage_InterruptExecution,
  RVS_DemonMessage_Terminate,
  RVS_DemonMessage_Shutdown
} RVS_DemonMessageType;

typedef struct
{
  RVS_QueueNode        base;
  RVS_DemonMessageType type;
  RVS_MessageID        request_id; // engine request that owns this completion
  union {
    struct {
      ProcessLaunchParams params; // process launch params
    } launch;
    struct {
      U64 command_id;
    } pump;
    struct {
      DMN_Handle *processes;
      U64         processes_count;
      DMN_TrapChunkList traps;
    } run;
    struct {
      DMN_Handle     *processes;
      U64             processes_count;
      RVS_MessageID   execution_request_id;
      U64             command_id;
      DMN_TrapChunkList traps;
    } resume;
    struct {
      RVS_MessageID execution_request_id;
      U64           command_id;
    } interrupt_execution;
    struct {
      DMN_Handle *process_handles;
      U64         process_count;
    } terminate;
  };
} RVS_DemonMessage;

typedef enum
{
  RVS_DemonAction_Null,
  RVS_DemonAction_Launch,
  RVS_DemonAction_Run,
  RVS_DemonAction_Resume,
  RVS_DemonAction_Terminate,
} RVS_DemonAction;

typedef enum
{
  RVS_DemonReplyKind_Null,
  RVS_DemonReplyKind_LaunchStarted,
  RVS_DemonReplyKind_ActionResult,
  RVS_DemonReplyKind_EventBatch,
  // Fences prior batches from this interrupt attempt: the active execution lease is no longer executing.
  // It does not imply that the engine has completed client-visible publication of those batches.
  RVS_DemonReplyKind_ExecutionStopped,
  RVS_DemonReplyKind_ExecutionFinished,
} RVS_DemonReplyKind;

typedef struct
{
  RVS_DemonReplyKind kind;
  RVS_MessageID      request_id;
  union {
    struct {
      U32 pid;
    } launch_started;
    struct {
      RVS_DemonAction action;
      RVS_Result      result;
      U64             command_id;
    } action_result;
    struct {
      DMN_EventList events;
      U64           command_id;
    } event_batch;
    struct {
      U64 command_id;
    } execution_stopped;
  };
} RVS_DemonReply;

typedef void (RVS_DemonReplyCallback)(RVS_Demon *demon, RVS_DemonReply *reply, void *ud);

RVS_Result rvs_demon_init(void *reply_ud, RVS_DemonReplyCallback *reply_callback, RVS_Demon **dmn_out);
RVS_Result rvs_demon_shutdown(RVS_Demon *dmn);
internal void rvs_demon_release_resources(RVS_Demon *dmn);

internal RVS_Result rvs_demon_alloc       (void);
internal RVS_Result rvs_demon_release     (void);
internal void       rvs_demon_message_copy(Arena *arena, RVS_DemonMessage *dst, RVS_DemonMessage *src);
internal void       rvs_demon_event_copy  (Arena *arena, DMN_Event *dst, DMN_Event *src);
internal void       rvs_demon_reply_copy  (Arena *arena, RVS_DemonReply *dst, RVS_DemonReply *src);
internal RVS_Result rvs_demon_send_message(RVS_Demon *dmn, RVS_DemonMessage message_spec);
internal RVS_DemonInterruptCapability rvs_demon_interrupt_capability(RVS_Demon *dmn);



