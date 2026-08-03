// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/rvs_engine.h"

typedef struct RVS_RequestPool RVS_RequestPool;
typedef struct RVS_RequestPoolNode RVS_RequestPoolNode;

struct RVS_RequestPool
{
  Arena               *arena;
  Mutex                mutex;
  RVS_RequestPoolNode *free_first;
  U64                  live_requests_count;
  B32                  engine_released;
};

struct RVS_Request
{
  RVS_RequestPool *pool;
  Mutex            mutex;
  CondVar          cv;
  U32              ref_count;
  RVS_MessageID    request_id;
  RVS_EngineReply  reply;
};

struct RVS_RequestPoolNode
{
  RVS_RequestPoolNode *next;
  RVS_Request          request;
};

internal RVS_RequestPool *rvs_request_pool_alloc(void);
internal void             rvs_request_pool_release_engine(RVS_RequestPool *pool);
internal RVS_Request     *rvs_request_pool_request_alloc(RVS_RequestPool *pool);
internal B32              rvs_request_complete(RVS_Request *request, RVS_EngineReply reply);
