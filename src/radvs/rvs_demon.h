// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////

#pragma once
#include "base/base_core.h"
#include "base/base_strings.h"
#include "base/base_arena.h"
#include "base/base_markup.h"
#include "demon/demon.h"
#include "radvs/rvs_core.h"
#include "radvs/rvs_request.h"
#include "radvs/rvs_queue.h"
#include "radvs/rvs_backend.h"

////////////////////////////////

typedef struct RVS_Demon RVS_Demon;

typedef struct
{
  RVS_BackendMessage base;
  RVS_Command        command;
} RVS_DemonMessage;

typedef enum
{
  RVS_DemonReplyKind_Null,
  RVS_DemonReplyKind_CommandResult,
  RVS_DemonReplyKind_LaunchStarted,
  RVS_DemonReplyKind_EventBatch,

  // Fences prior batches from this interrupt attempt: the active execution lease is no longer executing.
  // It does not imply that the engine has completed client-visible publication of those batches.
  RVS_DemonReplyKind_ExecutionStopped,
  RVS_DemonReplyKind_ExecutionFinished,
} RVS_DemonReplyKind;

typedef struct
{
  RVS_DemonReplyKind kind;
  RVS_Result         result;
  RVS_MessageID      id;
  union {
    U32           launch_pid;
    DMN_EventList event_batch;
  };
} RVS_DemonReply;

typedef void (RVS_DemonReplyCallback)(RVS_Demon *demon, RVS_DemonReply *reply, void *ud);

RVS_Result rvs_demon_init(void *reply_ud, RVS_DemonReplyCallback *reply_callback, RVS_Demon **dmn_out);
RVS_Result    rvs_demon_shutdown(RVS_Demon *dmn);

internal void       rvs_demon_release_resources(RVS_Demon *dmn);
internal RVS_Result rvs_demon_alloc            (void);
internal RVS_Result rvs_demon_release          (void);
internal void       rvs_demon_message_copy     (Arena *arena, RVS_DemonMessage *dst, RVS_DemonMessage *src);
internal void       rvs_demon_event_copy       (Arena *arena, DMN_Event *dst, DMN_Event *src);
internal void       rvs_demon_reply_copy       (Arena *arena, RVS_DemonReply *dst, RVS_DemonReply *src);
internal RVS_Result rvs_demon_send_message     (RVS_Demon *dmn, RVS_DemonMessage message_spec);

