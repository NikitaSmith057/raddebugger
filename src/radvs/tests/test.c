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
internal void rvs_test_wait_until_dispatch_held(RVS_Engine *engine);
internal void rvs_test_wait_until_execution_state(RVS_Session *session, RVS_SessionExecutionState state);
internal RVS_Request *rvs_test_request_alloc(RVS_Session *session, RVS_OperationKey key, RVS_EngineCommandKind command_kind);

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
  AssertAlways(RVS_RequestConflictPolicy_RejectIfPending == 0);
  AssertAlways(str8_match(rvs_string_from_command_kind(RVS_EngineCommandKind_Launch), str8_lit("Launch"), 0));
  AssertAlways(str8_match(rvs_string_from_command_kind(RVS_EngineCommandKind_Run), str8_lit("Run"), 0));
  AssertAlways(rvs_string_from_command_kind(RVS_EngineCommandKind_Null).size == 0);

  RVS_SubmitInfo launch_submit = {0};
  AssertAlways(rvs_session_launch(session, str8_lit("C:\\Windows\\System32\\where.exe"), str8_zero(), &launch_submit) == RVS_Result_Ok);
  AssertAlways(launch_submit.request != 0 && launch_submit.control != 0);
  RVS_Request *request = launch_submit.request;

  RVS_RequestWaitTest waiter = { .request = request };
  rvs_request_addref(request);
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

  RVS_OperationKey lifecycle_key = {
    .operation_class = RVS_OperationClass_SessionLifecycle,
    .operation_id = RVS_EngineCommandKind_Launch,
  };
  RVS_Request *correlated_launch = rvs_test_request_alloc(session, lifecycle_key, RVS_EngineCommandKind_Launch);
  AssertAlways(rvs_engine_begin_launch(engine, correlated_launch->request_id, 100));
  Temp correlation_scratch = scratch_begin(0, 0);
  DMN_EventList correlation_events = {0};
  DMN_Event *wrong_pid_event = dmn_event_list_push(correlation_scratch.arena, &correlation_events);
  wrong_pid_event->kind = DMN_EventKind_CreateProcess;
  wrong_pid_event->system_process_id = 101;
  wrong_pid_event->process = (DMN_Handle){ .u64 = { 1 } };
  DMN_Event *matching_pid_event = dmn_event_list_push(correlation_scratch.arena, &correlation_events);
  matching_pid_event->kind = DMN_EventKind_CreateProcess;
  matching_pid_event->system_process_id = 100;
  matching_pid_event->process = (DMN_Handle){ .u64 = { 2 } };
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_EventBatch,
    .request_id = correlated_launch->request_id,
    .event_batch.events = correlation_events,
  });
  RVS_EngineReply correlated_launch_reply = {0};
  AssertAlways(rvs_request_wait(correlated_launch, max_U64, &correlated_launch_reply) == RVS_Result_Ok);
  AssertAlways(correlated_launch_reply.result == RVS_Result_Ok);
  AssertAlways(correlated_launch_reply.launch.pid == 100);
  rvs_request_release(correlated_launch);
  scratch_end(correlation_scratch);

  RVS_Request *exited_launch = rvs_test_request_alloc(session, lifecycle_key, RVS_EngineCommandKind_Launch);
  AssertAlways(rvs_engine_begin_launch(engine, exited_launch->request_id, 200));
  Temp exit_scratch = scratch_begin(0, 0);
  DMN_EventList exit_events = {0};
  DMN_Event *created_event = dmn_event_list_push(exit_scratch.arena, &exit_events);
  created_event->kind = DMN_EventKind_CreateProcess;
  created_event->system_process_id = 200;
  created_event->process = (DMN_Handle){ .u64 = { 3 } };
  DMN_Event *exit_event = dmn_event_list_push(exit_scratch.arena, &exit_events);
  exit_event->kind = DMN_EventKind_ExitProcess;
  exit_event->process = created_event->process;
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_EventBatch,
    .request_id = exited_launch->request_id,
    .event_batch.events = exit_events,
  });
  RVS_EngineReply exited_launch_reply = {0};
  AssertAlways(rvs_request_wait(exited_launch, max_U64, &exited_launch_reply) == RVS_Result_Ok);
  AssertAlways(exited_launch_reply.result == RVS_Result_Error);
  rvs_request_release(exited_launch);
  scratch_end(exit_scratch);

  RVS_Request *pump_failure_launch = rvs_test_request_alloc(session, lifecycle_key, RVS_EngineCommandKind_Launch);
  ins_atomic_u32_eval_assign(&engine->test_fail_demon_pump_send, 1);
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_LaunchStarted,
    .request_id = pump_failure_launch->request_id,
    .launch_started.pid = 300,
  });
  RVS_EngineReply pump_failure_reply = {0};
  AssertAlways(rvs_request_wait(pump_failure_launch, max_U64, &pump_failure_reply) == RVS_Result_Ok);
  AssertAlways(pump_failure_reply.result == RVS_Result_Error);
  rvs_request_release(pump_failure_launch);

  RVS_OperationKey read_only_key = {
    .operation_class = RVS_OperationClass_ReadOnly,
    .program_id = reply.launch.program_id,
    .operation_id = 1,
  };
  RVS_Request *stale_read = rvs_test_request_alloc(session, read_only_key, RVS_EngineCommandKind_Null);
  mutex_take(session->control->mutex);
  stale_read->captured_program_state_epoch = rvs_session_program_state_epoch_locked(session, read_only_key.program_id);
  rvs_session_bump_program_state_epoch_locked(session, read_only_key.program_id);
  mutex_drop(session->control->mutex);
  rvs_engine_complete_reply(engine, (RVS_EngineReply){
    .request_id = stale_read->request_id,
    .result = RVS_Result_Ok,
  });
  RVS_EngineReply stale_read_reply = {0};
  AssertAlways(rvs_request_wait(stale_read, max_U64, &stale_read_reply) == RVS_Result_Ok);
  AssertAlways(stale_read_reply.result == RVS_Result_StaleState);
  rvs_request_release(stale_read);

  RVS_SubmitInfo invalid_run_submit = {0};
  AssertAlways(rvs_session_run(session, dmn_handle_zero(), &invalid_run_submit) == RVS_Result_InvalidArgument);
  AssertAlways(invalid_run_submit.request == 0 && invalid_run_submit.control == 0);

  RVS_ProgramID invalid_program_id = { .u64 = { max_U64 } };
  RVS_SubmitInfo invalid_program_submit = {0};
  AssertAlways(rvs_session_run(session, invalid_program_id, &invalid_program_submit) == RVS_Result_Error);
  AssertAlways(invalid_program_submit.request == 0 && invalid_program_submit.control == 0);

  RVS_SubmitInfo valid_run_submit = {0};
  AssertAlways(rvs_session_run(session, reply.launch.program_id, &valid_run_submit) == RVS_Result_Ok);
  AssertAlways(valid_run_submit.request != 0 && valid_run_submit.control != 0);
  RVS_EngineReply valid_run_reply = {0};
  AssertAlways(rvs_request_wait(valid_run_submit.request, max_U64, &valid_run_reply) == RVS_Result_Ok);
  AssertAlways(valid_run_reply.kind == RVS_EngineReplyKind_Run);
  AssertAlways(valid_run_reply.result == RVS_Result_Ok);
  rvs_request_release(valid_run_submit.request);
  rvs_request_control_release(valid_run_submit.control);
  rvs_test_wait_until_execution_state(session, RVS_SessionExecutionState_Idle);

  RVS_EngineSubmission mismatched_submission = {
    .command.kind = RVS_EngineCommandKind_Launch,
    .conflict_policy = RVS_RequestConflictPolicy_RejectIfPending,
    .key = {
      .operation_class = RVS_OperationClass_ProgramExecution,
      .program_id = reply.launch.program_id,
      .operation_id = RVS_EngineCommandKind_Launch,
    },
  };
  RVS_SubmitInfo mismatched_submit = {0};
  AssertAlways(rvs_session_submit(session, mismatched_submission, &mismatched_submit) == RVS_Result_Error);
  AssertAlways(mismatched_submit.request == 0 && mismatched_submit.control == 0);
  RVS_ProgramID mismatched_run_program = { .u64 = { max_U64 - 1 } };
  RVS_EngineSubmission mismatched_run_submission = {
    .command = {
      .kind = RVS_EngineCommandKind_Run,
      .run = { .programs_count = 1, .programs = &mismatched_run_program },
    },
    .conflict_policy = RVS_RequestConflictPolicy_RejectIfPending,
    .key = {
      .operation_class = RVS_OperationClass_ProgramExecution,
      .program_id = reply.launch.program_id,
      .operation_id = RVS_EngineCommandKind_Run,
    },
  };
  RVS_SubmitInfo mismatched_run_submit = {0};
  AssertAlways(rvs_session_submit(session, mismatched_run_submission, &mismatched_run_submit) == RVS_Result_Error);
  AssertAlways(mismatched_run_submit.request == 0 && mismatched_run_submit.control == 0);
  RVS_EngineSubmission malformed_run_submission = mismatched_run_submission;
  malformed_run_submission.command.run.programs_count = 0;
  malformed_run_submission.command.run.programs = 0;
  malformed_run_submission.key.program_id = reply.launch.program_id;
  RVS_SubmitInfo malformed_run_submit = {0};
  AssertAlways(rvs_session_submit(session, malformed_run_submission, &malformed_run_submit) == RVS_Result_Error);
  AssertAlways(malformed_run_submit.request == 0 && malformed_run_submit.control == 0);
  rvs_test_wait_until_execution_state(session, RVS_SessionExecutionState_Idle);

  // These failure paths cannot occur during normal engine operation.
  ins_atomic_u32_eval_assign(&engine->test_fail_command_enqueue, 1);
  RVS_SubmitInfo enqueue_failure_submit = {0};
  AssertAlways(rvs_session_run(session, reply.launch.program_id, &enqueue_failure_submit) == RVS_Result_Error);
  AssertAlways(enqueue_failure_submit.request == 0 && enqueue_failure_submit.control == 0);
  rvs_test_wait_until_execution_state(session, RVS_SessionExecutionState_Idle);

  ins_atomic_u32_eval_assign(&engine->test_hold_before_dispatch, 1);
  RVS_SubmitInfo cancelled_run_submit = {0};
  AssertAlways(rvs_session_run(session, reply.launch.program_id, &cancelled_run_submit) == RVS_Result_Ok);
  rvs_test_wait_until_dispatch_held(engine);
  rvs_test_wait_until_execution_state(session, RVS_SessionExecutionState_Queued);
  RVS_SubmitInfo blocked_run_submit = {0};
  RVS_ProgramID other_program_id = { .u64 = { max_U64 - 1 } };
  AssertAlways(rvs_session_run(session, other_program_id, &blocked_run_submit) == RVS_Result_AlreadyPending);
  RVS_SubmitInfo blocked_launch_submit = {0};
  AssertAlways(rvs_session_launch(session, str8_lit("C:\\Windows\\System32\\where.exe"), str8_zero(), &blocked_launch_submit) == RVS_Result_AlreadyPending);
  mutex_take(session->control->mutex);
  AssertAlways( ! rvs_session_execution_blocks_operation_locked(session, RVS_OperationClass_ReadOnly));
  mutex_drop(session->control->mutex);
  AssertAlways(rvs_request_control_cancel(cancelled_run_submit.control) == RVS_Result_Ok);
  rvs_test_wait_until_execution_state(session, RVS_SessionExecutionState_Queued);
  ins_atomic_u32_eval_assign(&engine->test_hold_before_dispatch, 0);
  RVS_EngineReply cancelled_run_reply = {0};
  AssertAlways(rvs_request_wait(cancelled_run_submit.request, max_U64, &cancelled_run_reply) == RVS_Result_Ok);
  AssertAlways(cancelled_run_reply.result == RVS_Result_Cancelled);
  rvs_request_release(cancelled_run_submit.request);
  rvs_request_control_release(cancelled_run_submit.control);
  rvs_test_wait_until_execution_state(session, RVS_SessionExecutionState_Idle);

  ins_atomic_u32_eval_assign(&engine->test_fail_demon_run_send, 1);
  RVS_SubmitInfo demon_send_failure_submit = {0};
  AssertAlways(rvs_session_run(session, reply.launch.program_id, &demon_send_failure_submit) == RVS_Result_Ok);
  RVS_EngineReply demon_send_failure_reply = {0};
  AssertAlways(rvs_request_wait(demon_send_failure_submit.request, max_U64, &demon_send_failure_reply) == RVS_Result_Ok);
  AssertAlways(demon_send_failure_reply.result == RVS_Result_Error);
  rvs_request_release(demon_send_failure_submit.request);
  rvs_request_control_release(demon_send_failure_submit.control);
  rvs_test_wait_until_execution_state(session, RVS_SessionExecutionState_Idle);

  // Exercise DEMON reply transitions directly without fabricating backend failures.
  RVS_MessageID test_run_request_id = 0x1000;
  mutex_take(session->control->mutex);
  AssertAlways(rvs_session_reserve_execution_locked(session, test_run_request_id));
  AssertAlways( ! rvs_session_mark_run_in_flight_locked(session, test_run_request_id + 1));
  mutex_drop(session->control->mutex);
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_Run,
    .request_id = test_run_request_id,
    .result = RVS_Result_Ok,
  });
  rvs_test_wait_until_execution_state(session, RVS_SessionExecutionState_RunInFlight);
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_EventBatch,
    .request_id = test_run_request_id,
  });
  rvs_test_wait_until_execution_state(session, RVS_SessionExecutionState_RunInFlight);
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_RunFinished,
    .request_id = test_run_request_id + 1,
  });
  rvs_test_wait_until_execution_state(session, RVS_SessionExecutionState_RunInFlight);
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_RunFinished,
    .request_id = test_run_request_id,
  });
  rvs_test_wait_until_execution_state(session, RVS_SessionExecutionState_Idle);

  mutex_take(session->control->mutex);
  AssertAlways(rvs_session_reserve_execution_locked(session, test_run_request_id));
  AssertAlways(rvs_session_clear_queued_execution_locked(session, test_run_request_id));
  AssertAlways(rvs_session_reserve_execution_locked(session, test_run_request_id));
  mutex_drop(session->control->mutex);
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_Run,
    .request_id = test_run_request_id,
    .result = RVS_Result_Error,
  });
  rvs_test_wait_until_execution_state(session, RVS_SessionExecutionState_Idle);

  RVS_OperationKey join_key = {
    .operation_class = RVS_OperationClass_ReadOnly,
    .program_id      = reply.launch.program_id,
    .operation_id    = 1,
  };
  mutex_take(session->control->mutex);
  RVS_Request *join_request = rvs_session_request_alloc_locked(session, RVS_RequestConflictPolicy_JoinIfEqual, join_key);
  AssertAlways(rvs_session_register_operation_locked(session, join_key, join_request) == RVS_Result_Ok);
  AssertAlways(rvs_session_register_operation_locked(session, join_key, join_request) == RVS_Result_AlreadyPending);
  AssertAlways(rvs_session_unregister_operation_locked(session, join_key) == join_request);
  rvs_session_request_remove_locked(session, join_request);
  mutex_drop(session->control->mutex);
  rvs_request_release(join_request); // drop engine ownership
  rvs_request_release(join_request); // drop caller ownership
  rvs_request_release(join_request); // drop operation-key ownership

  lifecycle_key.operation_id = 2;
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
  ins_atomic_u32_eval_assign(&engine->test_hold_before_dispatch, 1);
  RVS_SubmitInfo shutdown_held_run_submit = {0};
  AssertAlways(rvs_session_run(session, reply.launch.program_id, &shutdown_held_run_submit) == RVS_Result_Ok);
  rvs_test_wait_until_dispatch_held(engine);
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
  RVS_EngineReply shutdown_held_run_reply = {0};
  AssertAlways(rvs_request_wait(shutdown_held_run_submit.request, max_U64, &shutdown_held_run_reply) == RVS_Result_Ok);
  AssertAlways(shutdown_held_run_reply.result == RVS_Result_EngineStopped);
  mutex_take(shutdown_held_run_submit.request->mutex);
  AssertAlways( ! shutdown_held_run_submit.request->is_dispatched);
  mutex_drop(shutdown_held_run_submit.request->mutex);
  rvs_request_release(shutdown_held_run_submit.request);
  rvs_request_control_release(shutdown_held_run_submit.control);
  rvs_test_wait_until_execution_state(session, RVS_SessionExecutionState_Idle);
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

internal void
rvs_test_wait_until_dispatch_held(RVS_Engine *engine)
{
  while ( ! ins_atomic_u32_eval(&engine->test_is_held_before_dispatch)) {
    sleep_ms(1);
  }
}

internal void
rvs_test_wait_until_execution_state(RVS_Session *session, RVS_SessionExecutionState state)
{
  for (;;) {
    mutex_take(session->control->mutex);
    B32 matches = session->execution_state == state;
    mutex_drop(session->control->mutex);
    if (matches) { break; }
    sleep_ms(1);
  }
}

internal RVS_Request *
rvs_test_request_alloc(RVS_Session *session, RVS_OperationKey key, RVS_EngineCommandKind command_kind)
{
  mutex_take(session->control->mutex);
  RVS_Request *request = rvs_session_request_alloc_locked(session, RVS_RequestConflictPolicy_RejectIfPending, key);
  request->command_kind = command_kind;
  mutex_drop(session->control->mutex);
  return request;
}
