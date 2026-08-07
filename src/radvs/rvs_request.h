// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////

#pragma once
#include "radvs/rvs_core.h"

////////////////////////////////
// Request

typedef struct RVS_RequestPool RVS_RequestPool;

typedef struct
{
  RVS_RequestPool  *pool;
  U32               ref_count;
  RVS_MessageID     id;
  RVS_CommandReply  reply;
  Mutex             mutex;
  CondVar           cv;
} RVS_Request;

typedef struct RVS_RequestPoolNode RVS_RequestPoolNode;
struct RVS_RequestPoolNode
{
  RVS_RequestPoolNode *next;
  RVS_Request          request;
};

struct RVS_RequestPool
{
  Arena               *arena;
  Mutex                mutex;
  HashTable           *request_by_id;
  RVS_RequestPoolNode *free_list;
  RVS_MessageID        next_request_id;
  U64                  live_requests_count;
  B32                  engine_released;
};

////////////////////////////////
// Internal API

internal RVS_RequestPool * rvs_request_pool_alloc          (void);
internal void              rvs_request_pool_destroy        (RVS_RequestPool *pool);
internal RVS_Request *     rvs_request_pool_request_alloc  (RVS_RequestPool *pool);
internal U64  rvs_request_pool_release_request(RVS_RequestPool *pool, RVS_Request *request);
internal void rvs_request_pool_release_engine (RVS_RequestPool *pool);
internal RVS_Request * rvs_request_from_id(RVS_RequestPool *pool, RVS_MessageID id);
