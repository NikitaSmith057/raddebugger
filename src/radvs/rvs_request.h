// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/rvs_engine.h"

typedef struct RVS_RequestPool RVS_RequestPool;

struct RVS_RequestPool
{
  Arena       *arena;
  Mutex        mutex;
  RVS_Request *free_first;
  U64          live_requests_count;
  B32          engine_released;
};

struct RVS_Request
{
  RVS_Request          *next;
  RVS_Request          *prev;
  RVS_Request          *key_next;
  RVS_Request          *key_prev;
  RVS_RequestPool      *pool;
  Mutex                 mutex;
  CondVar               cv;
  U32                   ref_count;
  B32                   is_dispatched;
  U64                   captured_program_state_epoch;
  RVS_MessageID         request_id;
  U32                   launch_pid;
  RVS_EngineCommandKind command_kind;
  RVS_OperationKey      key;
  RVS_EngineReply       reply;
};

internal RVS_RequestPool *rvs_request_pool_alloc(void);
internal void             rvs_request_pool_release_engine(RVS_RequestPool *pool);
internal RVS_Request     *rvs_request_pool_request_alloc(RVS_RequestPool *pool);
internal B32              rvs_request_complete(RVS_Request *request, RVS_EngineReply reply);
