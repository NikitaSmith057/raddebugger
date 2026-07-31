// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/engine/radvs_format.h"

typedef enum RADVS_DemonCommand
{
  RADVS_DemonCommand_Null,
  RADVS_DemonCommand_Launch,
  RADVS_DemonCommand_Run,
  RADVS_DemonCommand_Step,
  RADVS_DemonCommand_Break,
  RADVS_DemonCommand_UpdateTraps,
  RADVS_DemonCommand_Terminate,
  RADVS_DemonCommand_ReleaseClient,
  RADVS_DemonCommand_Shutdown,
} RADVS_DemonCommand;

typedef enum RADVS_DemonClientState
{
  RADVS_DemonClientState_Runnable,
  RADVS_DemonClientState_Running,
  RADVS_DemonClientState_Stopped,
  RADVS_DemonClientState_Terminating,
  RADVS_DemonClientState_Closed,
} RADVS_DemonClientState;

typedef struct RADVS_Demon       RADVS_Demon;
typedef struct RADVS_DemonClient RADVS_DemonClient;

RADVS_Result radvs_demon_init(void);
RADVS_Result radvs_demon_shutdown(void);
// Caller must initialize DEMON before allocating an Engine control token.

typedef void RADVS_DemonRawEventRouter(void *user_data, RADVS_DemonClient *active_client, DMN_EventList events, B32 suppress_internal_halt);
typedef void RADVS_DemonLaunchRouter(void *user_data, RADVS_DemonClient *client, U32 process_pid);

RADVS_Result radvs_demon_alloc(RADVS_DemonClient **out_client);
RADVS_Result radvs_demon_client_set_owner_id(RADVS_DemonClient *client, U64 owner_id);
RADVS_Result radvs_demon_set_raw_event_router(RADVS_DemonRawEventRouter *router, void *user_data);
RADVS_Result radvs_demon_set_launch_router(RADVS_DemonLaunchRouter *router, void *user_data);
void         radvs_demon_client_release(RADVS_DemonClient *client);
RADVS_Result radvs_demon_launch   (RADVS_DemonClient *client, const ProcessLaunchParams *params, U32 *out_pid);
RADVS_Result radvs_demon_run      (RADVS_DemonClient *client, const DMN_Handle *process_handles, U64 process_handle_count);
RADVS_Result radvs_demon_update_traps(RADVS_DemonClient *client, const DMN_Trap *traps, U64 trap_count);
RADVS_Result radvs_demon_run_with_traps(RADVS_DemonClient *client, const DMN_Handle *process_handles, U64 process_handle_count, const DMN_Trap *traps, U64 trap_count);
RADVS_Result radvs_demon_step_with_traps(RADVS_DemonClient *client, DMN_Handle process, DMN_Handle thread, const DMN_Trap *traps, U64 trap_count);
RADVS_Result radvs_demon_break    (RADVS_DemonClient *client);
RADVS_Result radvs_demon_terminate(RADVS_DemonClient *client, const DMN_Handle *process_handles, U64 process_handle_count);
