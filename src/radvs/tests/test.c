// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#define BUILD_CONSOLE_INTERFACE 1
#define BUILD_TITLE "RAD VS Tests"
#define DMN_INIT_MANUAL 1

#include "third_party/radsort/radsort.h"

#include "base/base_inc.h"
#include "x64/x64.h"
#include "win32/win32_inc.h"
#include "coff/coff.h"
#include "coff/coff_parse.h"
#include "pe/pe.h"
#include "linker/hash_table.h"
#include "rdi/rdi_local.h"
#include "arch/arch_inc.h"
#include "demon/demon_inc.h"

#include "base/base_inc.c"
#include "x64/x64.c"
#include "win32/win32_inc.c"
#include "coff/coff.c"
#include "coff/coff_parse.c"
#include "pe/pe.c"
#include "linker/hash_table.c"
#include "rdi/rdi_local.c"
#include "arch/arch_inc.c"
#include "demon/demon_inc.c"

#include "radvs/rvs_protocol.c"
#include "radvs/rvs_demon.c"
#include "radvs/rvs_engine.c"

typedef struct
{
  RVS_Request     *request;
  RVS_Result       result;
  RVS_EngineReply  reply;
} RVS_RequestWaitTest;

typedef struct
{
  RVS_Session *session;
  RVS_Result   result;
} RVS_EventWaitTest;

typedef struct
{
  RVS_Engine *engine;
} RVS_EngineShutdownTest;

internal void rvs_event_wait_test_thread(void *user_data);
internal void rvs_test_wait_until_event_waiting(RVS_Session *session);
internal void rvs_engine_shutdown_test_thread(void *user_data);
internal void rvs_test_wait_until_shutdown(RVS_Session *session);

internal void
rvs_request_wait_test_thread(void *user_data)
{
  RVS_RequestWaitTest *test = user_data;
  test->result = rvs_request_wait(test->request, max_U64, &test->reply);
  rvs_request_release(test->request);
}

internal void
entry_point(CmdLine *cmdline)
{
  (void)cmdline;

  RVS_Engine *engine = 0;
  AssertAlways(rvs_engine_init(&engine) == RVS_Result_Ok);
  RVS_Session *session = 0;
  AssertAlways(rvs_engine_create_session(engine, &session) == RVS_Result_Ok);
  RVS_Session *second_session = 0;
  AssertAlways(rvs_engine_create_session(engine, &second_session) == RVS_Result_Unsupported);

  RVS_SubmitInfo launch_submit = {0};
  AssertAlways(rvs_session_launch(session, str8_lit("C:\\Windows\\System32\\where.exe"), str8_zero(), &launch_submit) == RVS_Result_Ok);
  AssertAlways(launch_submit.request != 0 && launch_submit.control != 0);
  RVS_Request *request = launch_submit.request;

  RVS_RequestWaitTest waiter = { .request = request };
  rvs_request_retain(request);
  Thread thread = thread_launch(rvs_request_wait_test_thread, &waiter);
  AssertAlways( ! MemoryIsZeroStruct(&thread));

  RVS_EngineReply reply = {0};
  AssertAlways(rvs_request_wait(request, max_U64, &reply) == RVS_Result_Ok);
  rvs_request_release(request);
  thread_join(thread, max_U64);

  AssertAlways(waiter.result == RVS_Result_Ok);
  AssertAlways(reply.result == RVS_Result_Ok);
  AssertAlways(waiter.reply.result == RVS_Result_Ok);
  AssertAlways(reply.request_id == waiter.reply.request_id);
  AssertAlways(reply.kind == RVS_EngineReplyKind_Launch);
  AssertAlways(waiter.reply.kind == RVS_EngineReplyKind_Launch);
  rvs_request_control_release(launch_submit.control);

  RVS_SubmitInfo invalid_run_submit = {0};
  AssertAlways(rvs_session_run(session, dmn_handle_zero(), (RVS_SubmitOptions){0}, &invalid_run_submit) == RVS_Result_Error);
  AssertAlways(invalid_run_submit.request == 0 && invalid_run_submit.control == 0);

  RVS_ProgramID invalid_program_id = { .u64 = { max_U64 } };
  RVS_SubmitInfo invalid_program_submit = {0};
  AssertAlways(rvs_session_run(session, invalid_program_id, (RVS_SubmitOptions){0}, &invalid_program_submit) == RVS_Result_Error);
  AssertAlways(invalid_program_submit.request == 0 && invalid_program_submit.control == 0);

  RVS_OperationKey invalid_run_key = {
    .operation_class = RVS_OperationClass_ReadOnly,
    .program_id      = reply.launch.program_id,
  };
  RVS_SubmitInfo invalid_key_submit = {0};
  AssertAlways(rvs_session_run(session, reply.launch.program_id, (RVS_SubmitOptions){ .key = invalid_run_key }, &invalid_key_submit) == RVS_Result_Error);
  AssertAlways(invalid_key_submit.request == 0 && invalid_key_submit.control == 0);
  RVS_SubmitInfo invalid_policy_submit = {0};
  AssertAlways(rvs_session_run(session, reply.launch.program_id, (RVS_SubmitOptions){ .policy = (RVS_RequestPolicy)99, .key = { .operation_class = RVS_OperationClass_ProgramExecution, .program_id = reply.launch.program_id, .operation_id = 1 } }, &invalid_policy_submit) == RVS_Result_Error);
  AssertAlways(invalid_policy_submit.request == 0 && invalid_policy_submit.control == 0);

  RVS_SubmitInfo valid_run_submit = {0};
  AssertAlways(rvs_session_run(session, reply.launch.program_id, (RVS_SubmitOptions){0}, &valid_run_submit) == RVS_Result_Ok);
  AssertAlways(valid_run_submit.request != 0 && valid_run_submit.control != 0);
  RVS_EngineReply valid_run_reply = {0};
  AssertAlways(rvs_request_wait(valid_run_submit.request, max_U64, &valid_run_reply) == RVS_Result_Ok);
  AssertAlways(valid_run_reply.kind == RVS_EngineReplyKind_Run);
  AssertAlways(valid_run_reply.result == RVS_Result_Ok);
  rvs_request_release(valid_run_submit.request);
  rvs_request_control_release(valid_run_submit.control);

  RVS_OperationKey lifecycle_key = {
    .operation_class = RVS_OperationClass_SessionLifecycle,
    .operation_id = 2,
  };
  RVS_OperationKey program_a_key = {
    .operation_class = RVS_OperationClass_ProgramExecution,
    .program_id   = { .u64 = { 1 } },
    .operation_id = 3,
  };
  RVS_OperationKey program_b_same_target_key = {
    .operation_class = RVS_OperationClass_ProgramExecution,
    .program_id   = { .u64 = { 2 } },
    .operation_id = 4,
  };
  RVS_OperationKey program_b_other_target_key = {
    .operation_class = RVS_OperationClass_ProgramExecution,
    .program_id   = { .u64 = { 2 } },
    .operation_id = 5,
  };
  AssertAlways(rvs_operation_keys_conflict(lifecycle_key, program_a_key));
  AssertAlways( ! rvs_operation_keys_conflict(program_a_key, program_b_same_target_key));
  AssertAlways( ! rvs_operation_keys_conflict(program_a_key, program_b_other_target_key));
  RVS_OperationKey query_key = {
    .operation_class   = RVS_OperationClass_ReadOnly,
    .program_id        = { .u64 = { 1 } },
    .operation_id      = 6,
  };
  AssertAlways(rvs_operation_keys_conflict(lifecycle_key, query_key));
  AssertAlways( ! rvs_operation_keys_conflict(query_key, program_a_key));

  RVS_EventWaitTest event_waiter = { .session = session };
  Thread event_thread = thread_launch(rvs_event_wait_test_thread, &event_waiter);
  AssertAlways( ! MemoryIsZeroStruct(&event_thread));
  rvs_test_wait_until_event_waiting(session);
  RVS_EngineShutdownTest shutdown_test = { .engine = engine };
  Thread shutdown_thread = thread_launch(rvs_engine_shutdown_test_thread, &shutdown_test);
  AssertAlways( ! MemoryIsZeroStruct(&shutdown_thread));
  rvs_test_wait_until_shutdown(session);
  RVS_SubmitInfo stopped_submit = {0};
  AssertAlways(rvs_session_launch(session, str8_lit("C:\\Windows\\System32\\where.exe"), str8_zero(), &stopped_submit) == RVS_Result_EngineStopped);
  AssertAlways(stopped_submit.request == 0 && stopped_submit.control == 0);
  thread_join(shutdown_thread, max_U64);
  thread_join(event_thread, max_U64);
  AssertAlways(event_waiter.result == RVS_Result_EngineStopped);
  Temp stopped_event_scratch = scratch_begin(0, 0);
  RVS_Event event = {0};
  AssertAlways(rvs_session_wait_for_event(stopped_event_scratch.arena, session, 0, &event) == RVS_Result_EngineStopped);
  scratch_end(stopped_event_scratch);
  rvs_session_release(session);
}

internal void
rvs_event_wait_test_thread(void *user_data)
{
  RVS_EventWaitTest *test = user_data;
  Arena *arena = arena_alloc();
  RVS_Event event = {0};
  do {
    test->result = rvs_session_wait_for_event(arena, test->session, max_U64, &event);
  } while (test->result == RVS_Result_Ok);
  arena_release(arena);
}

internal void
rvs_test_wait_until_event_waiting(RVS_Session *session)
{
  for (;;) {
    mutex_take(session->event_queue->mutex);
    B32 is_waiting = session->event_queue->waiter_count != 0;
    mutex_drop(session->event_queue->mutex);
    if (is_waiting) { break; }
    sleep_ms(1);
  }
}

internal void
rvs_engine_shutdown_test_thread(void *user_data)
{
  RVS_EngineShutdownTest *test = user_data;
  rvs_engine_shutdown(test->engine);
}

internal void
rvs_test_wait_until_shutdown(RVS_Session *session)
{
  for (;;) {
    mutex_take(session->control->mutex);
    B32 is_shutdown = session->control->is_shutdown;
    mutex_drop(session->control->mutex);
    if (is_shutdown) { break; }
    sleep_ms(1);
  }
}
