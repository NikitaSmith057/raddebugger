#pragma once

typedef enum
{
  RVS_BackendState_Idle,
  RVS_BackendState_Pumping,
  RVS_BackendState_Running,
  RVS_BackendState_Interrupting,
} RVS_BackendStateKind;

typedef struct
{
  RVS_QueueNode base;
  RVS_MessageID id;
} RVS_BackendMessage;

typedef enum
{
  RVS_BackendInterruptCapability_Null,
  RVS_BackendInterruptCapability_GlobalWithResume,
} RVS_BackendInterruptCapability;

enum
{
  RVS_DemonCommand_PumpEvent = RVS_CommandKind_UserLo,
};


