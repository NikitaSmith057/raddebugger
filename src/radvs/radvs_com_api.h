// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include <stdint.h>

typedef struct RADVS_EngineSession RADVS_EngineSession;

typedef int32_t RADVS_ComResult;
enum
{
  RADVS_ComResult_Null            = 0,
  RADVS_ComResult_Ok              = 1,
  RADVS_ComResult_InvalidArgument = 2,
  RADVS_ComResult_OutOfMemory     = 3,
  RADVS_ComResult_NotImplemented  = 4,
  RADVS_ComResult_AbiMismatch     = 5,
  RADVS_ComResult_Busy            = 6,
  RADVS_ComResult_Timeout         = 7,
  RADVS_ComResult_Conflict        = 8,
};

typedef struct RADVS_ComString16
{
  uint16_t *str;
  uint64_t  size;
} RADVS_ComString16;

typedef enum RADVS_ComEventKind
{
  RADVS_ComEventKind_Null,
  RADVS_ComEventKind_CreateProcess,
  RADVS_ComEventKind_ExitProcess,
  RADVS_ComEventKind_CreateThread,
  RADVS_ComEventKind_ExitThread,
  RADVS_ComEventKind_Stop,
  RADVS_ComEventKind_Break,
  RADVS_ComEventKind_Exception,
  RADVS_ComEventKind_DebugString,
  RADVS_ComEventKind_Other,
} RADVS_ComEventKind;

typedef struct RADVS_ComEvent
{
  RADVS_ComEventKind event_kind;
  const uint8_t     *message;
  uint64_t           message_size;
  uint32_t           code;
  uint32_t           system_thread_id;
  uint32_t           exception_repeated;
} RADVS_ComEvent;

typedef struct RADVS_ComThread
{
  uint32_t system_thread_id;
  uint32_t state;
} RADVS_ComThread;

#ifdef __cplusplus
extern "C" {
#endif

RADVS_ComResult radvs_com_session_alloc           (RADVS_EngineSession **out_session);
void            radvs_com_session_release         (RADVS_EngineSession *session);
RADVS_ComResult radvs_com_session_launch16        (RADVS_EngineSession *session, RADVS_ComString16 exe, RADVS_ComString16 args, RADVS_ComString16 wdir, uint32_t *out_pid);
RADVS_ComResult radvs_com_session_run             (RADVS_EngineSession *session);
RADVS_ComResult radvs_com_session_break           (RADVS_EngineSession *session);
RADVS_ComResult radvs_com_session_terminate       (RADVS_EngineSession *session);
void            radvs_com_session_close_event_wait(RADVS_EngineSession *session);
RADVS_ComResult radvs_com_session_wait_event      (RADVS_EngineSession *session);
RADVS_ComResult radvs_com_session_poll_event      (RADVS_EngineSession *session, RADVS_ComEvent *out_event);
RADVS_ComResult radvs_com_session_copy_threads    (RADVS_EngineSession *session, RADVS_ComThread *buffer, uint64_t buffer_count, uint64_t *out_count);

C_LINKAGE_END
