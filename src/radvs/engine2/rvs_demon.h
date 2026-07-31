// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/engine2/rvs.h"

typedef enum
{
  RVS_DemonRequest_Null,
  RVS_DemonRequest_Launch,
  RVS_DemonRequest_Run,
  RVS_DemonRequest_Break,
  RVS_DemonRequest_Terminate,
  RVS_DemonRequest_Shutdown
} RVS_DemonRequestType;

typedef struct RVS_DemonInstance;
typedef struct RVS_Demon;

RVS_Result rvs_demon_init(void);
RVS_Result rvs_demon_shutdown(void);

internal RAVS_DemonSession * rvs_demon_alloc(DMN_);

