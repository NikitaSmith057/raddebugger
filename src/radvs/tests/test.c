// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#define BUILD_CONSOLE_INTERFACE 1
#define BUILD_TITLE "RAD VS Tests"
#define DMN_INIT_MANUAL 1
#define RVS_ENGINE_TESTING 1

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

internal void
rvs_request_wait_test_thread(void *user_data)
{
  RVS_RequestWaitTest *test = user_data;
  test->result = rvs_request_wait(test->request, max_U64, &test->reply);
  rvs_request_release(test->request);
}

internal RVS_Request *
rvs_test_pending_launch(RVS_Engine *engine, U32 pid)
{
  RVS_Request *request = 0;
  AssertAlways(rvs_engine_submit_command(engine, (RVS_EngineCommand){ .kind = RVS_EngineCommandKind_Launch }, RVS_RequestPolicy_Independent, (RVS_OperationKey){0}, &request, 0) == RVS_Result_Ok);
  AssertAlways(rvs_engine_begin_launch(engine, request->request_id, pid));
  return request;
}

internal void
entry_point(CmdLine *cmdline)
{
  (void)cmdline;

  RVS_Engine *engine = 0;
  AssertAlways(rvs_engine_init(&engine) == RVS_Result_Ok);

  RVS_SubmitInfo launch_submit = {0};
  AssertAlways(rvs_engine_launch(engine, str8_lit("C:\\Windows\\System32\\where.exe"), str8_zero(), (RVS_SubmitOptions){0}, &launch_submit) == RVS_Result_Ok);
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
  AssertAlways(rvs_engine_run(engine, dmn_handle_zero(), (RVS_SubmitOptions){0}, &invalid_run_submit) == RVS_Result_Error);
  AssertAlways(invalid_run_submit.request == 0 && invalid_run_submit.control == 0);

  RVS_ProgramID invalid_program_id = { .u64 = { max_U64 } };
  RVS_SubmitInfo invalid_program_submit = {0};
  AssertAlways(rvs_engine_run(engine, invalid_program_id, (RVS_SubmitOptions){0}, &invalid_program_submit) == RVS_Result_Error);
  AssertAlways(invalid_program_submit.request == 0 && invalid_program_submit.control == 0);

  RVS_OperationKey invalid_run_key = {
    .operation_class = RVS_OperationClass_ProgramExecution,
    .session_id      = 2,
    .program_id      = reply.launch.program_id,
  };
  RVS_SubmitInfo invalid_key_submit = {0};
  AssertAlways(rvs_engine_run(engine, reply.launch.program_id, (RVS_SubmitOptions){ .key = invalid_run_key }, &invalid_key_submit) == RVS_Result_Error);
  AssertAlways(invalid_key_submit.request == 0 && invalid_key_submit.control == 0);

  RVS_SubmitInfo valid_run_submit = {0};
  AssertAlways(rvs_engine_run(engine, reply.launch.program_id, (RVS_SubmitOptions){0}, &valid_run_submit) == RVS_Result_Ok);
  AssertAlways(valid_run_submit.request != 0 && valid_run_submit.control != 0);
  RVS_EngineReply valid_run_reply = {0};
  AssertAlways(rvs_request_wait(valid_run_submit.request, max_U64, &valid_run_reply) == RVS_Result_Ok);
  AssertAlways(valid_run_reply.kind == RVS_EngineReplyKind_Run);
  AssertAlways(valid_run_reply.result == RVS_Result_Ok);
  rvs_request_release(valid_run_submit.request);
  rvs_request_control_release(valid_run_submit.control);

  // Exercise delayed launch correlation without letting the worker dispatch synthetic commands.
  rvs_engine_test_set_dispatch_hold(engine, 1, 0);
  rvs_engine_test_set_launch_pump(engine, 1, 0);

  RVS_Request *multi_batch_launch = rvs_test_pending_launch(engine, 100);
  DMN_EventNode unrelated_error = { .v = {
    .kind              = DMN_EventKind_Error,
    .error_kind        = DMN_ErrorKind_UnexpectedFailure,
    .system_process_id = 200,
  } };
  rvs_engine_process_demon_output(engine, &(RVS_DemonOutput){
    .kind       = RVS_DemonOutputKind_EventBatch,
    .request_id = multi_batch_launch->request_id,
    .event_batch = { .events = { .first = &unrelated_error, .last = &unrelated_error, .count = 1 } },
  });
  rvs_engine_process_demon_output(engine, &(RVS_DemonOutput){
    .kind       = RVS_DemonOutputKind_EventBatch,
    .request_id = multi_batch_launch->request_id,
  });
  AssertAlways(rvs_engine_launch_is_pending(engine, multi_batch_launch->request_id));
  DMN_Handle multi_batch_process = { .u64 = { 1000 } };
  DMN_EventNode matching_create = { .v = {
    .kind              = DMN_EventKind_CreateProcess,
    .process           = multi_batch_process,
    .system_process_id = 100,
  } };
  rvs_engine_process_demon_output(engine, &(RVS_DemonOutput){
    .kind       = RVS_DemonOutputKind_EventBatch,
    .request_id = multi_batch_launch->request_id,
    .event_batch = { .events = { .first = &matching_create, .last = &matching_create, .count = 1 } },
  });
  RVS_EngineReply multi_batch_reply = {0};
  AssertAlways(rvs_request_wait(multi_batch_launch, max_U64, &multi_batch_reply) == RVS_Result_Ok);
  AssertAlways(multi_batch_reply.result == RVS_Result_Ok);
  rvs_request_release(multi_batch_launch);

  Temp event_scratch = scratch_begin(0, 0);
  RVS_Event event = {0};
  while (rvs_engine_wait_for_event(event_scratch.arena, engine, 0, &event) == RVS_Result_Ok) {}
  scratch_end(event_scratch);

  RVS_Request *exited_launch = rvs_test_pending_launch(engine, 101);
  DMN_Handle exited_process = { .u64 = { 1001 } };
  DMN_EventNode exiting_create = { .v = {
    .kind              = DMN_EventKind_CreateProcess,
    .process           = exited_process,
    .system_process_id = 101,
  } };
  DMN_EventNode exiting_process = { .v = {
    .kind    = DMN_EventKind_ExitProcess,
    .process = exited_process,
  } };
  exiting_create.next = &exiting_process;
  rvs_engine_process_demon_output(engine, &(RVS_DemonOutput){
    .kind       = RVS_DemonOutputKind_EventBatch,
    .request_id = exited_launch->request_id,
    .event_batch = { .events = { .first = &exiting_create, .last = &exiting_process, .count = 2 } },
  });
  RVS_EngineReply exited_reply = {0};
  AssertAlways(rvs_request_wait(exited_launch, max_U64, &exited_reply) == RVS_Result_Ok);
  AssertAlways(exited_reply.result == RVS_Result_Error);
  Temp exited_event_scratch = scratch_begin(0, 0);
  AssertAlways(rvs_engine_wait_for_event(exited_event_scratch.arena, engine, 0, &event) == RVS_Result_Timeout);
  scratch_end(exited_event_scratch);
  rvs_request_release(exited_launch);

  RVS_Request *failed_pump_launch = rvs_test_pending_launch(engine, 102);
  rvs_engine_test_set_launch_pump(engine, 1, 1);
  rvs_engine_pump_launch(engine, failed_pump_launch->request_id);
  RVS_EngineReply failed_pump_reply = {0};
  AssertAlways(rvs_request_wait(failed_pump_launch, max_U64, &failed_pump_reply) == RVS_Result_Ok);
  AssertAlways(failed_pump_reply.result == RVS_Result_Error);
  rvs_request_release(failed_pump_launch);

  rvs_engine_test_set_launch_pump(engine, 0, 0);
  rvs_engine_test_set_dispatch_hold(engine, 0, 0);

  Temp scratch = scratch_begin(0, 0);
  String8List cmd_line = str8_split_by_string_chars(scratch.arena, str8_lit("C:\\Windows\\System32\\where.exe"), str8_lit(" "), 0);
  RVS_EngineCommand command = {
    .kind   = RVS_EngineCommandKind_Launch,
    .launch = { .params = { .cmd_line = cmd_line } },
  };
  RVS_OperationKey key = {
    .session_id   = 1,
    .operation_id = 1,
  };
  RVS_Request *joined_request_a = 0;
  RVS_Request *joined_request_b = 0;
  RVS_RequestControl *joined_owner_a = 0;
  RVS_RequestControl *joined_owner_b = 0;
  RVS_Request *ownerless_request = 0;
  AssertAlways(rvs_engine_submit_command(engine, command, RVS_RequestPolicy_JoinIfEqual, key, &ownerless_request, 0) == RVS_Result_Error);
  AssertAlways(ownerless_request == 0);
  RVS_OperationKey invalid_query_key = {
    .operation_class = RVS_OperationClass_ReadOnly,
    .session_id      = 1,
    .operation_id    = 2,
  };
  RVS_RequestControl *invalid_query_owner = 0;
  AssertAlways(rvs_engine_submit_command(engine, command, RVS_RequestPolicy_JoinIfEqual, invalid_query_key, &ownerless_request, &invalid_query_owner) == RVS_Result_Error);
  AssertAlways(ownerless_request == 0 && invalid_query_owner == 0);
  AssertAlways(rvs_engine_submit_command(engine, command, RVS_RequestPolicy_Independent, invalid_query_key, &ownerless_request, &invalid_query_owner) == RVS_Result_Error);
  AssertAlways(ownerless_request == 0 && invalid_query_owner == 0);
  RVS_OperationKey invalid_session_query_key = {
    .operation_class = RVS_OperationClass_ReadOnly,
    .program_id      = { .u64 = { 1 } },
    .operation_id    = 2,
  };
  AssertAlways(rvs_engine_submit_command(engine, command, RVS_RequestPolicy_Independent, invalid_session_query_key, &ownerless_request, &invalid_query_owner) == RVS_Result_Error);
  AssertAlways(ownerless_request == 0 && invalid_query_owner == 0);
  RVS_OperationKey invalid_execution_key = {
    .operation_class = RVS_OperationClass_ProgramExecution,
    .session_id      = 1,
    .program_id      = { .u64 = { max_U64 } },
    .operation_id    = 3,
  };
  AssertAlways(rvs_engine_submit_command(engine, command, RVS_RequestPolicy_RejectIfPending, invalid_execution_key, &ownerless_request, &invalid_query_owner) == RVS_Result_Error);
  AssertAlways(ownerless_request == 0 && invalid_query_owner == 0);
  RVS_OperationKey foreign_lifecycle_key = {
    .operation_class = RVS_OperationClass_SessionLifecycle,
    .session_id      = 2,
    .operation_id    = 4,
  };
  AssertAlways(rvs_engine_submit_command(engine, command, RVS_RequestPolicy_Independent, foreign_lifecycle_key, &ownerless_request, &invalid_query_owner) == RVS_Result_Error);
  AssertAlways(ownerless_request == 0 && invalid_query_owner == 0);
  AssertAlways(rvs_engine_submit_command(engine, command, RVS_RequestPolicy_JoinIfEqual, key, &joined_request_a, &joined_owner_a) == RVS_Result_Ok);
  AssertAlways(rvs_engine_submit_command(engine, command, RVS_RequestPolicy_JoinIfEqual, key, &joined_request_b, &joined_owner_b) == RVS_Result_Ok);
  scratch_end(scratch);

  AssertAlways(joined_request_a == joined_request_b);
  AssertAlways(joined_owner_a != 0);
  AssertAlways(joined_owner_b == 0);
  AssertAlways(rvs_request_wait(joined_request_a, max_U64, 0) == RVS_Result_Ok);
  AssertAlways(rvs_request_wait(joined_request_b, max_U64, 0) == RVS_Result_Ok);
  rvs_request_control_release(joined_owner_a);
  rvs_request_release(joined_request_a);
  rvs_request_release(joined_request_b);

  RVS_OperationKey lifecycle_key = {
    .operation_class = RVS_OperationClass_SessionLifecycle,
    .session_id   = 1,
    .operation_id = 2,
  };
  RVS_OperationKey program_a_key = {
    .operation_class = RVS_OperationClass_ProgramExecution,
    .session_id   = 1,
    .program_id   = { .u64 = { 1 } },
    .operation_id = 3,
  };
  RVS_OperationKey program_b_same_target_key = {
    .operation_class = RVS_OperationClass_ProgramExecution,
    .session_id   = 1,
    .program_id   = { .u64 = { 2 } },
    .operation_id = 4,
  };
  RVS_OperationKey program_b_other_target_key = {
    .operation_class = RVS_OperationClass_ProgramExecution,
    .session_id   = 1,
    .program_id   = { .u64 = { 2 } },
    .operation_id = 5,
  };
  AssertAlways(rvs_operation_keys_conflict(lifecycle_key, program_a_key));
  AssertAlways( ! rvs_operation_keys_conflict(program_a_key, program_b_same_target_key));
  AssertAlways( ! rvs_operation_keys_conflict(program_a_key, program_b_other_target_key));
  RVS_OperationKey query_key = {
    .operation_class   = RVS_OperationClass_ReadOnly,
    .session_id        = 1,
    .program_id        = { .u64 = { 1 } },
    .operation_id      = 6,
  };
  AssertAlways(rvs_operation_keys_conflict(lifecycle_key, query_key));
  AssertAlways( ! rvs_operation_keys_conflict(query_key, program_a_key));

  RVS_ProgramID known_program_id = reply.launch.program_id;
  AssertAlways( ! dmn_handle_match(known_program_id, dmn_handle_zero()));

  RVS_OperationKey cancelled_key = {
    .operation_class = RVS_OperationClass_SessionLifecycle,
    .session_id      = 1,
    .operation_id    = 1,
  };
  rvs_engine_test_set_dispatch_hold(engine, 1, 0);
  RVS_SubmitInfo cancelled_submit = {0};
  AssertAlways(rvs_engine_launch(engine, str8_lit("C:\\Windows\\System32\\where.exe"), str8_zero(), (RVS_SubmitOptions){ .policy = RVS_RequestPolicy_JoinIfEqual, .key = cancelled_key }, &cancelled_submit) == RVS_Result_Ok);
  RVS_Request *cancelled_request = cancelled_submit.request;
  RVS_RequestControl *cancelled_owner = cancelled_submit.control;
  AssertAlways(rvs_request_control_cancel(cancelled_owner) == RVS_Result_Ok);
  rvs_engine_test_set_dispatch_hold(engine, 0, 0);
  RVS_EngineReply cancelled_reply = {0};
  AssertAlways(rvs_request_wait(cancelled_request, max_U64, &cancelled_reply) == RVS_Result_Ok);
  AssertAlways(cancelled_reply.result == RVS_Result_Cancelled);
  rvs_request_control_release(cancelled_owner);
  rvs_request_release(cancelled_request);

  RVS_OperationKey dispatched_key = {
    .operation_class = RVS_OperationClass_SessionLifecycle,
    .session_id      = 1,
    .operation_id    = 1,
  };
  rvs_engine_test_set_dispatch_hold(engine, 0, 1);
  RVS_SubmitInfo dispatched_submit = {0};
  AssertAlways(rvs_engine_launch(engine, str8_lit("C:\\Windows\\System32\\where.exe"), str8_zero(), (RVS_SubmitOptions){ .policy = RVS_RequestPolicy_JoinIfEqual, .key = dispatched_key }, &dispatched_submit) == RVS_Result_Ok);
  RVS_Request *dispatched_request = dispatched_submit.request;
  RVS_RequestControl *dispatched_owner = dispatched_submit.control;
  rvs_engine_test_wait_until_dispatched(engine);
  AssertAlways(rvs_request_control_cancel(dispatched_owner) == RVS_Result_Unsupported);
  rvs_engine_test_set_dispatch_hold(engine, 0, 0);
  RVS_EngineReply dispatched_reply = {0};
  AssertAlways(rvs_request_wait(dispatched_request, max_U64, &dispatched_reply) == RVS_Result_Ok);
  AssertAlways(dispatched_reply.result != RVS_Result_Cancelled);
  rvs_request_control_release(dispatched_owner);
  rvs_request_release(dispatched_request);

  RVS_OperationKey query_a_key = {
    .operation_class = RVS_OperationClass_ReadOnly,
    .session_id      = 1,
    .program_id      = known_program_id,
    .operation_id    = 1,
  };
  rvs_engine_test_set_dispatch_hold(engine, 0, 1);
  RVS_SubmitInfo query_a_submit = {0};
  AssertAlways(rvs_engine_launch(engine, str8_lit("C:\\Windows\\System32\\where.exe"), str8_zero(), (RVS_SubmitOptions){ .policy = RVS_RequestPolicy_JoinIfEqual, .key = query_a_key }, &query_a_submit) == RVS_Result_Ok);
  RVS_Request *query_a = query_a_submit.request;
  RVS_RequestControl *query_a_owner = query_a_submit.control;
  rvs_engine_test_wait_until_dispatched(engine);
  rvs_engine_test_invalidate_program(engine, 1, known_program_id);
  rvs_engine_test_set_dispatch_hold(engine, 0, 0);
  RVS_EngineReply query_a_reply = {0};
  AssertAlways(rvs_request_wait(query_a, max_U64, &query_a_reply) == RVS_Result_Ok);
  AssertAlways(query_a_reply.result == RVS_Result_StaleState);
  rvs_request_control_release(query_a_owner);
  rvs_request_release(query_a);

  RVS_OperationKey query_b_key = query_a_key;
  query_b_key.operation_id = 2;
  rvs_engine_test_set_dispatch_hold(engine, 0, 1);
  RVS_SubmitInfo query_b_submit = {0};
  AssertAlways(rvs_engine_launch(engine, str8_lit("C:\\Windows\\System32\\where.exe"), str8_zero(), (RVS_SubmitOptions){ .policy = RVS_RequestPolicy_JoinIfEqual, .key = query_b_key }, &query_b_submit) == RVS_Result_Ok);
  RVS_Request *query_b = query_b_submit.request;
  RVS_RequestControl *query_b_owner = query_b_submit.control;
  rvs_engine_test_wait_until_dispatched(engine);
  rvs_engine_test_invalidate_program(engine, 1, (RVS_ProgramID){ .u64 = { 2 } });
  rvs_engine_test_set_dispatch_hold(engine, 0, 0);
  RVS_EngineReply query_b_reply = {0};
  AssertAlways(rvs_request_wait(query_b, max_U64, &query_b_reply) == RVS_Result_Ok);
  AssertAlways(query_b_reply.result != RVS_Result_StaleState);
  rvs_request_control_release(query_b_owner);
  rvs_request_release(query_b);

  RVS_OperationKey query_c_key = query_a_key;
  query_c_key.operation_id = 3;
  rvs_engine_test_set_dispatch_hold(engine, 0, 1);
  RVS_SubmitInfo query_c_submit = {0};
  AssertAlways(rvs_engine_launch(engine, str8_lit("C:\\Windows\\System32\\where.exe"), str8_zero(), (RVS_SubmitOptions){ .policy = RVS_RequestPolicy_JoinIfEqual, .key = query_c_key }, &query_c_submit) == RVS_Result_Ok);
  RVS_Request *query_c = query_c_submit.request;
  RVS_RequestControl *query_c_owner = query_c_submit.control;
  rvs_engine_test_wait_until_dispatched(engine);
  rvs_engine_test_invalidate_session(engine, 1);
  rvs_engine_test_set_dispatch_hold(engine, 0, 0);
  RVS_EngineReply query_c_reply = {0};
  AssertAlways(rvs_request_wait(query_c, max_U64, &query_c_reply) == RVS_Result_Ok);
  AssertAlways(query_c_reply.result == RVS_Result_StaleState);

  Temp shutdown_scratch = scratch_begin(0, 0);
  String8List shutdown_cmd_line = str8_split_by_string_chars(shutdown_scratch.arena, str8_lit("C:\\Windows\\System32\\where.exe"), str8_lit(" "), 0);
  RVS_EngineCommand shutdown_command = {
    .kind   = RVS_EngineCommandKind_Launch,
    .launch = { .params = { .cmd_line = shutdown_cmd_line } },
  };
  RVS_OperationKey shutdown_key = {
    .operation_class = RVS_OperationClass_SessionLifecycle,
    .session_id      = 1,
    .operation_id    = 1,
  };
  RVS_Request *shutdown_request = 0;
  RVS_RequestControl *shutdown_owner = 0;
  rvs_engine_test_set_dispatch_hold(engine, 1, 0);
  AssertAlways(rvs_engine_submit_command(engine, shutdown_command, RVS_RequestPolicy_JoinIfEqual, shutdown_key, &shutdown_request, &shutdown_owner) == RVS_Result_Ok);
  scratch_end(shutdown_scratch);
  rvs_engine_shutdown(engine);

  RVS_EngineReply shutdown_reply = {0};
  AssertAlways(rvs_request_wait(shutdown_request, max_U64, &shutdown_reply) == RVS_Result_Ok);
  AssertAlways(shutdown_reply.result == RVS_Result_EngineStopped);
  RVS_EngineReply query_c_reply_after_shutdown = {0};
  AssertAlways(rvs_request_wait(query_c, max_U64, &query_c_reply_after_shutdown) == RVS_Result_Ok);
  AssertAlways(query_c_reply_after_shutdown.result == query_c_reply.result);
  AssertAlways(rvs_request_control_cancel(shutdown_owner) == RVS_Result_EngineStopped);
  rvs_request_control_release(shutdown_owner);
  rvs_request_release(shutdown_request);
  rvs_request_control_release(query_c_owner);
  rvs_request_release(query_c);
}
