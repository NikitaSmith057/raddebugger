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

#include "radvs/rvs_async.c"
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

typedef struct
{
  RVS_QueueNode base;
  U64           value;
} RVS_QueueTestMessage;

typedef struct
{
  RVS_ScheduledOperation *operation;
  U64                     releases;
} RVS_OperationReleaseStress;

typedef struct
{
  RVS_SchedulerEventKind  kind;
  RVS_MessageID           source_request_id;
  U64                     target_index;
  RVS_SchedulerEmissionKind expected_emission;
  RVS_SchedulerCommandKind  expected_command;
  RVS_SchedulerDecisionStatus expected_status;
  U64                     expected_targets_count;
} RVS_ReducerEventTest;

internal void rvs_event_wait_test_thread(void *user_data);
internal void rvs_test_wait_until_event_waiting(RVS_Session *session);
internal void rvs_engine_shutdown_test_thread(void *user_data);
internal void rvs_test_wait_until_shutdown(RVS_Session *session);
internal void rvs_test_wait_until_dispatch_held(RVS_Engine *engine);
internal void rvs_test_wait_until_execution_state(RVS_Session *session, RVS_ProgramID target, RVS_TargetExecutionState state);
internal RVS_Request *rvs_test_request_alloc(RVS_Session *session, RVS_SchedulerKey key, RVS_ScheduledOperation **operation_out);
internal B32 rvs_test_launch_started(RVS_Engine *engine, RVS_MessageID request_id, U32 expected_pid);
internal B32 rvs_test_launch_event(RVS_Engine *engine, RVS_MessageID request_id, DMN_Event event);
internal RVS_SchedulerCommandToken rvs_test_launch_pump_token(RVS_Engine *engine, RVS_MessageID request_id);
internal void rvs_operation_release_stress_thread(void *user_data);

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

  Arena *queue_arena = arena_alloc(.name = "RVS Queue Test");
  RVS_Queue *queue = rvs_queue_alloc(queue_arena, sizeof(RVS_QueueTestMessage), AlignOf(RVS_QueueTestMessage));
  RVS_QueueTestMessage *queued = rvs_queue_alloc_struct(queue, RVS_QueueTestMessage);
  RVS_QueueTestMessage *rejected = rvs_queue_alloc_struct(queue, RVS_QueueTestMessage);
  AssertAlways(queued != 0 && rejected != 0);
  queued->value = 1;
  AssertAlways(rvs_queue_push(queue, &queued->base) == RVS_Result_Ok);
  rvs_queue_close(queue);
  AssertAlways(rvs_queue_push(queue, &rejected->base) == RVS_Result_EngineStopped);
  rvs_queue_recycle(queue, &rejected->base);
  RVS_QueueTestMessage *popped = rvs_queue_pop_struct(queue, RVS_QueueTestMessage, 0);
  AssertAlways(popped == queued && popped->value == 1);
  rvs_queue_recycle(queue, &popped->base);
  AssertAlways(rvs_queue_alloc_item(queue) == 0);
  rvs_queue_release(queue);
  arena_release(queue_arena);

  Temp terminate_copy_scratch = scratch_begin(0, 0);
  DMN_Handle terminate_handles[] = { { .u64 = { 1 } }, { .u64 = { 2 } } };
  RVS_DemonMessage terminate_copy = {0};
  rvs_demon_message_copy(terminate_copy_scratch.arena, &terminate_copy, &(RVS_DemonMessage){
    .type = RVS_DemonMessage_Terminate,
    .terminate = { .process_handles = terminate_handles, .process_count = ArrayCount(terminate_handles) },
  });
  AssertAlways(terminate_copy.terminate.process_count == ArrayCount(terminate_handles));
  AssertAlways(terminate_copy.terminate.process_handles != terminate_handles);
  AssertAlways(dmn_handle_match(terminate_copy.terminate.process_handles[0], terminate_handles[0]));
  AssertAlways(dmn_handle_match(terminate_copy.terminate.process_handles[1], terminate_handles[1]));

  U64 copy_command_id = 0xfedcba9876543210ull;
  RVS_DemonMessage pump_copy = {0};
  rvs_demon_message_copy(terminate_copy_scratch.arena, &pump_copy, &(RVS_DemonMessage){
    .type = RVS_DemonMessage_Pump,
    .pump = { .command_id = copy_command_id },
  });
  AssertAlways(pump_copy.pump.command_id == copy_command_id);
  RVS_DemonMessage resume_copy = {0};
  rvs_demon_message_copy(terminate_copy_scratch.arena, &resume_copy, &(RVS_DemonMessage){
    .type = RVS_DemonMessage_Resume,
    .resume = {
      .processes = terminate_handles,
      .processes_count = ArrayCount(terminate_handles),
      .execution_request_id = 42,
      .command_id = copy_command_id,
    },
  });
  AssertAlways(resume_copy.resume.command_id == copy_command_id && resume_copy.resume.execution_request_id == 42);
  AssertAlways(resume_copy.resume.processes != terminate_handles && resume_copy.resume.processes_count == ArrayCount(terminate_handles));
  DMN_EventList copy_events = {0};
  *dmn_event_list_push(terminate_copy_scratch.arena, &copy_events) = (DMN_Event){ .kind = DMN_EventKind_Halt };
  RVS_DemonReply event_batch_copy = {0};
  rvs_demon_reply_copy(terminate_copy_scratch.arena, &event_batch_copy, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_EventBatch,
    .event_batch = { .events = copy_events, .command_id = copy_command_id },
  });
  AssertAlways(event_batch_copy.event_batch.command_id == copy_command_id);
  AssertAlways(event_batch_copy.event_batch.events.first != copy_events.first);
  RVS_DemonReply action_result_copy = {0};
  rvs_demon_reply_copy(terminate_copy_scratch.arena, &action_result_copy, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_ActionResult,
    .action_result = { .action = RVS_DemonAction_Resume, .result = RVS_Result_Ok, .command_id = copy_command_id },
  });
  AssertAlways(action_result_copy.action_result.command_id == copy_command_id);
  scratch_end(terminate_copy_scratch);

  RVS_Engine *engine = 0;
  AssertAlways(rvs_engine_init(&engine) == RVS_Result_Ok);
  AssertAlways(rvs_demon_interrupt_capability(engine->demon) == RVS_DemonInterruptCapability_GlobalWithResume);
  AssertAlways(rvs_demon_interrupt(engine->demon, 1, &(DMN_Handle){0}, 1) == RVS_Result_Error);
  AssertAlways(rvs_demon_send_message(engine->demon, (RVS_DemonMessage){ .type = RVS_DemonMessage_Halt, .request_id = 1 }) == RVS_Result_Unsupported);
  AssertAlways(rvs_scheduler_operation_rule(RVS_SchedulerOp_Run)->admission_requirement == RVS_AdmissionRequirement_StoppedUnfenced);
  AssertAlways(rvs_scheduler_operation_rule(RVS_SchedulerOp_Interrupt)->admission_requirement == RVS_AdmissionRequirement_ActiveOneLease);
  AssertAlways(rvs_scheduler_operation_rule(RVS_SchedulerOp_Terminate)->admission_requirement == RVS_AdmissionRequirement_LiveUnfenced);
  RVS_Session *session = 0;
  AssertAlways(rvs_engine_create_session(engine, &session) == RVS_Result_Ok);
  RVS_Session *second_session = 0;
  AssertAlways(rvs_engine_create_session(engine, &second_session) == RVS_Result_Unsupported);
  AssertAlways(RVS_SchedulerOp_Null == 0);
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

  RVS_SubmitInfo second_launch_submit = {0};
  AssertAlways(rvs_session_launch(session, str8_lit("C:\\Windows\\System32\\where.exe"), str8_zero(), &second_launch_submit) == RVS_Result_Ok);
  RVS_EngineReply second_launch_reply = {0};
  AssertAlways(rvs_request_wait(second_launch_submit.request, max_U64, &second_launch_reply) == RVS_Result_Ok);
  AssertAlways(second_launch_reply.result == RVS_Result_Ok);
  rvs_request_release(second_launch_submit.request);
  rvs_request_control_release(second_launch_submit.control);

  RVS_SubmitInfo third_launch_submit = {0};
  AssertAlways(rvs_session_launch(session, str8_lit("C:\\Windows\\System32\\where.exe"), str8_zero(), &third_launch_submit) == RVS_Result_Ok);
  RVS_EngineReply third_launch_reply = {0};
  AssertAlways(rvs_request_wait(third_launch_submit.request, max_U64, &third_launch_reply) == RVS_Result_Ok);
  AssertAlways(third_launch_reply.result == RVS_Result_Ok);
  rvs_request_release(third_launch_submit.request);
  rvs_request_control_release(third_launch_submit.control);

  RVS_SchedulerKey lifecycle_key = {
    .op = RVS_SchedulerOp_Launch,
    .identity = RVS_EngineCommandKind_Launch,
  };
  RVS_Request *correlated_launch = rvs_test_request_alloc(session, lifecycle_key, 0);
  AssertAlways(rvs_test_launch_started(engine, correlated_launch->request_id, 100));
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
    .event_batch = { .events = correlation_events, .command_id = rvs_test_launch_pump_token(engine, correlated_launch->request_id).command_id },
  });
  RVS_EngineReply correlated_launch_reply = {0};
  AssertAlways(rvs_request_wait(correlated_launch, max_U64, &correlated_launch_reply) == RVS_Result_Ok);
  AssertAlways(correlated_launch_reply.result == RVS_Result_Ok);
  AssertAlways(correlated_launch_reply.launch.pid == 100);
  rvs_request_release(correlated_launch);
  scratch_end(correlation_scratch);

  RVS_Request *exited_launch = rvs_test_request_alloc(session, lifecycle_key, 0);
  AssertAlways(rvs_test_launch_started(engine, exited_launch->request_id, 200));
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
    .event_batch = { .events = exit_events, .command_id = rvs_test_launch_pump_token(engine, exited_launch->request_id).command_id },
  });
  RVS_EngineReply exited_launch_reply = {0};
  AssertAlways(rvs_request_wait(exited_launch, max_U64, &exited_launch_reply) == RVS_Result_Ok);
  AssertAlways(exited_launch_reply.result == RVS_Result_Error);
  rvs_request_release(exited_launch);
  scratch_end(exit_scratch);

  RVS_Request *duplicate_start_launch = rvs_test_request_alloc(session, lifecycle_key, 0);
  AssertAlways(rvs_test_launch_started(engine, duplicate_start_launch->request_id, 400));
  AssertAlways( ! rvs_test_launch_started(engine, duplicate_start_launch->request_id, 400));
  Temp mismatched_scratch = scratch_begin(0, 0);
  DMN_EventList mismatched_events = {0};
  *dmn_event_list_push(mismatched_scratch.arena, &mismatched_events) = (DMN_Event){ .kind = DMN_EventKind_CreateProcess, .system_process_id = 401, .process = { .u64 = { 4 } } };
  RVS_SchedulerCommandToken mismatched_token = rvs_test_launch_pump_token(engine, duplicate_start_launch->request_id);
  RVS_SchedulerEventDisposition mismatched_dispositions[1] = {0};
  RVS_SchedulerDecision stale_pump_decision = {0};
  rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = {
      .source = RVS_SchedulerEventBatchSource_PumpLaunch,
      .command = { .request_id = mismatched_token.request_id, .command_id = mismatched_token.command_id + 1 },
      .request_id = duplicate_start_launch->request_id,
      .events = mismatched_events,
      .dispositions = mismatched_dispositions,
      .dispositions_count = 1,
    },
  }, &stale_pump_decision);
  AssertAlways(stale_pump_decision.status == RVS_SchedulerDecisionStatus_IgnoredStale);
  rvs_scheduler_decision_release(&stale_pump_decision);
  AssertAlways(rvs_test_launch_pump_token(engine, duplicate_start_launch->request_id).command_id == mismatched_token.command_id);
  RVS_SchedulerDecision mismatched_decision = {0};
  rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = {
      .source = RVS_SchedulerEventBatchSource_PumpLaunch,
      .command = mismatched_token,
      .request_id = duplicate_start_launch->request_id,
      .events = mismatched_events,
      .dispositions = mismatched_dispositions,
      .dispositions_count = 1,
    },
  }, &mismatched_decision);
  AssertAlways(mismatched_decision.command.kind == RVS_SchedulerCommand_PumpLaunch);
  AssertAlways(mismatched_decision.command.token.command_id != mismatched_token.command_id);
  rvs_scheduler_decision_release(&mismatched_decision);
  scratch_end(mismatched_scratch);
  AssertAlways(rvs_test_launch_event(engine, duplicate_start_launch->request_id, (DMN_Event){ .kind = DMN_EventKind_Error, .system_process_id = 400 }));
  RVS_EngineReply duplicate_start_reply = {0};
  AssertAlways(rvs_request_wait(duplicate_start_launch, max_U64, &duplicate_start_reply) == RVS_Result_Ok);
  AssertAlways(duplicate_start_reply.result == RVS_Result_Error);
  rvs_request_release(duplicate_start_launch);

  RVS_Request *retry_launch = rvs_test_request_alloc(session, lifecycle_key, 0);
  AssertAlways(rvs_test_launch_started(engine, retry_launch->request_id, 450));
  RVS_SchedulerCommandToken retry_token = rvs_test_launch_pump_token(engine, retry_launch->request_id);
  RVS_SchedulerDecision retry_decision = {0};
  rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = {
      .source = RVS_SchedulerEventBatchSource_PumpLaunch,
      .command = retry_token,
      .request_id = retry_launch->request_id,
      .dispositions = (RVS_SchedulerEventDisposition[1]){0},
      .dispositions_count = 1,
    },
  }, &retry_decision);
  AssertAlways(retry_decision.command.kind == RVS_SchedulerCommand_PumpLaunch);
  AssertAlways(retry_decision.command.token.command_id != retry_token.command_id);
  rvs_scheduler_decision_release(&retry_decision);
  AssertAlways(rvs_test_launch_event(engine, retry_launch->request_id, (DMN_Event){ .kind = DMN_EventKind_Error, .system_process_id = 450 }));
  RVS_EngineReply retry_reply = {0};
  AssertAlways(rvs_request_wait(retry_launch, max_U64, &retry_reply) == RVS_Result_Ok);
  AssertAlways(retry_reply.result == RVS_Result_Error);
  rvs_request_release(retry_launch);

  RVS_Request *suppressed_launch = rvs_test_request_alloc(session, lifecycle_key, 0);
  AssertAlways(rvs_test_launch_started(engine, suppressed_launch->request_id, 475));
  Temp suppressed_scratch = scratch_begin(0, 0);
  DMN_EventList suppressed_events = {0};
  *dmn_event_list_push(suppressed_scratch.arena, &suppressed_events) = (DMN_Event){ .kind = DMN_EventKind_CreateProcess, .system_process_id = 475, .process = { .u64 = { 6 } } };
  *dmn_event_list_push(suppressed_scratch.arena, &suppressed_events) = (DMN_Event){ .kind = DMN_EventKind_ExitProcess, .system_process_id = 475, .process = { .u64 = { 6 } } };
  RVS_SchedulerEventDisposition suppressed_dispositions[2] = {0};
  RVS_SchedulerCommandToken suppressed_token = rvs_test_launch_pump_token(engine, suppressed_launch->request_id);
  RVS_SchedulerDecision suppressed_decision = {0};
  rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = {
      .request_id = suppressed_launch->request_id,
      .source = RVS_SchedulerEventBatchSource_PumpLaunch,
      .command = suppressed_token,
      .events = suppressed_events,
      .dispositions = suppressed_dispositions,
      .dispositions_count = ArrayCount(suppressed_dispositions),
    },
  }, &suppressed_decision);
  AssertAlways(suppressed_decision.emissions.first && suppressed_decision.emissions.first->kind == RVS_SchedulerEmission_CompleteRequest);
  AssertAlways(suppressed_dispositions[0] == RVS_SchedulerEventDisposition_Suppress);
  AssertAlways(suppressed_dispositions[1] == RVS_SchedulerEventDisposition_Suppress);
  rvs_engine_execute_scheduler_decision(engine, &suppressed_decision);
  scratch_end(suppressed_scratch);
  RVS_EngineReply suppressed_reply = {0};
  AssertAlways(rvs_request_wait(suppressed_launch, max_U64, &suppressed_reply) == RVS_Result_Ok);
  AssertAlways(suppressed_reply.result == RVS_Result_Error);
  rvs_request_release(suppressed_launch);

  RVS_Request *missing_process_launch = rvs_test_request_alloc(session, lifecycle_key, 0);
  AssertAlways(rvs_test_launch_started(engine, missing_process_launch->request_id, 500));
  AssertAlways(rvs_test_launch_event(engine, missing_process_launch->request_id, (DMN_Event){ .kind = DMN_EventKind_CreateProcess, .system_process_id = 500 }));
  RVS_EngineReply missing_process_reply = {0};
  AssertAlways(rvs_request_wait(missing_process_launch, max_U64, &missing_process_reply) == RVS_Result_Ok);
  AssertAlways(missing_process_reply.result == RVS_Result_Error);
  rvs_request_release(missing_process_launch);

  RVS_Request *duplicate_resolve_launch = rvs_test_request_alloc(session, lifecycle_key, 0);
  DMN_Handle duplicate_resolve_process = { .u64 = { 5 } };
  AssertAlways(rvs_test_launch_started(engine, duplicate_resolve_launch->request_id, 600));
  Temp duplicate_create_scratch = scratch_begin(0, 0);
  DMN_EventList duplicate_create_events = {0};
  *dmn_event_list_push(duplicate_create_scratch.arena, &duplicate_create_events) = (DMN_Event){ .kind = DMN_EventKind_CreateProcess, .system_process_id = 600, .process = duplicate_resolve_process };
  *dmn_event_list_push(duplicate_create_scratch.arena, &duplicate_create_events) = (DMN_Event){ .kind = DMN_EventKind_CreateProcess, .system_process_id = 600, .process = { .u64 = { 7 } } };
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_EventBatch,
    .request_id = duplicate_resolve_launch->request_id,
    .event_batch = { .events = duplicate_create_events, .command_id = rvs_test_launch_pump_token(engine, duplicate_resolve_launch->request_id).command_id },
  });
  scratch_end(duplicate_create_scratch);
  RVS_EngineReply duplicate_resolve_reply = {0};
  AssertAlways(rvs_request_wait(duplicate_resolve_launch, max_U64, &duplicate_resolve_reply) == RVS_Result_Ok);
  AssertAlways(duplicate_resolve_reply.result == RVS_Result_Ok);
  AssertAlways(dmn_handle_match(duplicate_resolve_reply.launch.program_id, duplicate_resolve_process));
  AssertAlways(duplicate_resolve_reply.launch.pid == 600);
  rvs_request_release(duplicate_resolve_launch);

  Temp retirement_scratch = scratch_begin(0, 0);
  DMN_EventList retirement_events = {0};
  *dmn_event_list_push(retirement_scratch.arena, &retirement_events) = (DMN_Event){
    .kind = DMN_EventKind_ExitProcess,
    .system_process_id = 600,
    .process = duplicate_resolve_process,
    .code = 123,
  };
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_EventBatch,
    .event_batch = { .events = retirement_events },
  });
  scratch_end(retirement_scratch);
  mutex_take(session->control->mutex);
  RVS_TargetLedgerEntry *retired_entry = rvs_scheduler_target_from_id_locked(&session->scheduler, duplicate_resolve_process);
  RVS_Program *retired_program = rvs_session_program_from_id_locked(session, duplicate_resolve_process);
  AssertAlways(retired_entry->state == RVS_TargetState_Removed && retired_entry->is_termination_fenced);
  AssertAlways(retired_entry->execution_owner == 0 && retired_entry->execution_token == 0);
  AssertAlways(retired_program && retired_program->lifecycle == RVS_ProgramLifecycle_Removed && retired_program->exit_code == 123);
  mutex_drop(session->control->mutex);
  RVS_SubmitInfo retired_run_submit = {0};
  AssertAlways(rvs_session_run(session, duplicate_resolve_process, &retired_run_submit) == RVS_Result_StaleState);
  AssertAlways(retired_run_submit.request == 0 && retired_run_submit.control == 0);

  RVS_Request *pump_failure_launch = rvs_test_request_alloc(session, lifecycle_key, 0);
  engine->test_scheduler_effect_execution_max_depth = 0;
  ins_atomic_u32_eval_assign(&engine->test_fail_demon_message_type, RVS_DemonMessage_Pump);
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_LaunchStarted,
    .request_id = pump_failure_launch->request_id,
    .launch_started.pid = 300,
  });
  RVS_EngineReply pump_failure_reply = {0};
  AssertAlways(rvs_request_wait(pump_failure_launch, max_U64, &pump_failure_reply) == RVS_Result_Ok);
  AssertAlways(pump_failure_reply.result == RVS_Result_Error);
  AssertAlways(engine->test_scheduler_effect_execution_max_depth == 1);
  rvs_request_release(pump_failure_launch);

  RVS_Request *malformed_outcome_launch = rvs_test_request_alloc(session, lifecycle_key, 0);
  AssertAlways(rvs_test_launch_started(engine, malformed_outcome_launch->request_id, 350));
  RVS_SchedulerCommandToken malformed_outcome_token = rvs_test_launch_pump_token(engine, malformed_outcome_launch->request_id);
  RVS_SchedulerDecision malformed_outcome_decision = {0};
  rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_CommandOutcome,
    .command_outcome = {
      .kind = RVS_SchedulerCommandOutcome_Failed,
      .command_kind = RVS_SchedulerCommand_PumpLaunch,
      .command = malformed_outcome_token,
      .result = RVS_Result_Pending,
    },
  }, &malformed_outcome_decision);
  AssertAlways(malformed_outcome_decision.emissions.first &&
               malformed_outcome_decision.emissions.first->reply.result == RVS_Result_Error);
  rvs_engine_execute_scheduler_decision(engine, &malformed_outcome_decision);
  RVS_EngineReply malformed_outcome_reply = {0};
  AssertAlways(rvs_request_wait(malformed_outcome_launch, max_U64, &malformed_outcome_reply) == RVS_Result_Ok);
  AssertAlways(malformed_outcome_reply.result == RVS_Result_Error);
  rvs_request_release(malformed_outcome_launch);

  RVS_ScheduledOperation *malformed_dispatch_operation = 0;
  RVS_Request *malformed_dispatch_request = rvs_test_request_alloc(session, lifecycle_key, &malformed_dispatch_operation);
  RVS_ProgramID malformed_dispatch_program = reply.launch.program_id;
  AssertAlways( ! rvs_engine_request_mark_dispatched(engine, malformed_dispatch_request->request_id,
                                                      &(RVS_EngineCommand){
                                                        .kind = RVS_EngineCommandKind_Run,
                                                        .run = { .programs_count = 1, .programs = &malformed_dispatch_program },
                                                      }));
  AssertAlways( ! malformed_dispatch_operation->is_dispatched);
  mutex_take(session->control->mutex);
  rvs_scheduler_operation_remove_locked(&session->scheduler, malformed_dispatch_operation);
  mutex_drop(session->control->mutex);
  rvs_request_release(malformed_dispatch_request);
  rvs_scheduler_operation_release(malformed_dispatch_operation);

  RVS_SchedulerKey read_only_key = {
    .op = RVS_SchedulerOp_ReadOnly,
    .target = reply.launch.program_id,
    .identity = 1,
  };
  RVS_ScheduledOperation *stale_operation = 0;
  RVS_Request *stale_read = rvs_test_request_alloc(session, read_only_key, &stale_operation);
  mutex_take(session->control->mutex);
  stale_operation->captured_program_state_epoch = rvs_session_program_state_epoch_locked(session, read_only_key.target);
  rvs_scheduler_target_from_id_locked(&session->scheduler, read_only_key.target)->revision += 1;
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
  RVS_ProgramID unknown_programs[] = { invalid_program_id };
  RVS_SubmitInfo unknown_programs_submit = {0};
  AssertAlways(rvs_session_run_many(session, unknown_programs, ArrayCount(unknown_programs), &unknown_programs_submit) == RVS_Result_Error);
  AssertAlways(unknown_programs_submit.request == 0 && unknown_programs_submit.control == 0);

  RVS_ProgramID run_programs[] = { second_launch_reply.launch.program_id, third_launch_reply.launch.program_id };
  mutex_take(session->control->mutex);
  U64 first_program_epoch = rvs_session_program_state_epoch_locked(session, run_programs[0]);
  U64 second_program_epoch = rvs_session_program_state_epoch_locked(session, run_programs[1]);
  U64 first_execution_token = rvs_scheduler_target_from_id_locked(&session->scheduler, run_programs[0])->execution_token;
  U64 second_execution_token = rvs_scheduler_target_from_id_locked(&session->scheduler, run_programs[1])->execution_token;
  mutex_drop(session->control->mutex);
  RVS_SubmitInfo valid_run_submit = {0};
  AssertAlways(rvs_session_run_many(session, run_programs, ArrayCount(run_programs), &valid_run_submit) == RVS_Result_Ok);
  AssertAlways(valid_run_submit.request != 0 && valid_run_submit.control != 0);
  RVS_EngineReply valid_run_reply = {0};
  AssertAlways(rvs_request_wait(valid_run_submit.request, max_U64, &valid_run_reply) == RVS_Result_Ok);
  AssertAlways(valid_run_reply.kind == RVS_EngineReplyKind_Run);
  AssertAlways(valid_run_reply.result == RVS_Result_Ok);
  rvs_request_release(valid_run_submit.request);
  rvs_request_control_release(valid_run_submit.control);
  rvs_test_wait_until_execution_state(session, run_programs[0], RVS_TargetExecutionState_Idle);
  rvs_test_wait_until_execution_state(session, run_programs[1], RVS_TargetExecutionState_Idle);
  mutex_take(session->control->mutex);
  AssertAlways(rvs_session_program_state_epoch_locked(session, run_programs[0]) > first_program_epoch);
  AssertAlways(rvs_session_program_state_epoch_locked(session, run_programs[1]) > second_program_epoch);
  AssertAlways(rvs_scheduler_target_from_id_locked(&session->scheduler, run_programs[0])->execution_token > first_execution_token);
  AssertAlways(rvs_scheduler_target_from_id_locked(&session->scheduler, run_programs[1])->execution_token > second_execution_token);
  mutex_drop(session->control->mutex);

  RVS_ThreadID selected_thread = { .u64 = { 0x100 } };
  Temp thread_scratch = scratch_begin(0, 0);
  DMN_EventList thread_events = {0};
  *dmn_event_list_push(thread_scratch.arena, &thread_events) = (DMN_Event){
    .kind = DMN_EventKind_CreateThread,
    .process = run_programs[1],
    .thread = selected_thread,
  };
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_EventBatch,
    .event_batch = { .events = thread_events },
  });
  scratch_end(thread_scratch);
  AssertAlways(rvs_session_select_thread(session, run_programs[1], selected_thread) == RVS_Result_Ok);
  RVS_ProgramID selected_program = {0};
  RVS_ThreadID selected_thread_query = {0};
  AssertAlways(rvs_session_selected_thread(session, &selected_program, &selected_thread_query) == RVS_Result_Ok);
  AssertAlways(dmn_handle_match(selected_program, run_programs[1]) && dmn_handle_match(selected_thread_query, selected_thread));
  RVS_SubmitInfo unsupported_step = {0};
  AssertAlways(rvs_session_step(session, RVS_StepKind_Into, selected_thread, &unsupported_step) == RVS_Result_Unsupported);
  AssertAlways(unsupported_step.request == 0 && unsupported_step.control == 0);
  Temp thread_exit_scratch = scratch_begin(0, 0);
  DMN_EventList thread_exit_events = {0};
  *dmn_event_list_push(thread_exit_scratch.arena, &thread_exit_events) = (DMN_Event){
    .kind = DMN_EventKind_ExitThread,
    .process = run_programs[1],
    .thread = selected_thread,
  };
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_EventBatch,
    .event_batch = { .events = thread_exit_events },
  });
  scratch_end(thread_exit_scratch);
  AssertAlways(rvs_session_selected_thread(session, &selected_program, &selected_thread_query) == RVS_Result_StaleState);

  // An exception supersedes the active Continue intent after the full backend batch is collected.
  RVS_MessageID exception_run_request_id = 0x0800;
  RVS_TargetSnapshot exception_target = { .target = run_programs[0] };
  mutex_take(session->control->mutex);
  AssertAlways(rvs_scheduler_reserve_execution_locked(&session->scheduler, &exception_target, 1, exception_run_request_id));
  AssertAlways(rvs_scheduler_mark_run_in_flight_locked(&session->scheduler, exception_run_request_id));
  session->scheduler.phase = RVS_SchedulerPhase_Running;
  session->scheduler.run_intent = (RVS_RunIntent){
    .kind = RVS_RunIntentKind_Continue,
    .state = RVS_RunIntentState_Active,
    .execution_owner = exception_run_request_id,
  };
  U64 exception_cycle_epoch = session->scheduler.run_cycle_epoch + 1;
  Temp exception_scratch = scratch_begin(0, 0);
  DMN_EventList exception_events = {0};
  *dmn_event_list_push(exception_scratch.arena, &exception_events) = (DMN_Event){ .kind = DMN_EventKind_Exception, .process = run_programs[0] };
  RVS_SchedulerEventDisposition exception_dispositions[1] = {0};
  RVS_SchedulerDecision exception_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = {
      .request_id = exception_run_request_id,
      .events = exception_events,
      .dispositions = exception_dispositions,
      .dispositions_count = ArrayCount(exception_dispositions),
    },
  }, &exception_decision);
  AssertAlways(exception_decision.emissions.first == 0 && exception_decision.command.kind == RVS_SchedulerCommand_Null);
  AssertAlways(session->scheduler.phase == RVS_SchedulerPhase_Stopped);
  AssertAlways(session->scheduler.run_intent.state == RVS_RunIntentState_Cancelled);
  AssertAlways(session->scheduler.run_cycle_epoch == exception_cycle_epoch);
  AssertAlways(session->scheduler.stop_transaction.cycle_epoch == exception_cycle_epoch);
  AssertAlways(session->scheduler.stop_transaction.interrupt_attempt == 1);
  AssertAlways(session->scheduler.stop_transaction.primary_cause == RVS_StopCause_Exception);
  AssertAlways(exception_dispositions[0] == RVS_SchedulerEventDisposition_Forward);
  session->scheduler.run_intent.state = RVS_RunIntentState_Active;
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = {
      .request_id = exception_run_request_id,
      .events = exception_events,
      .dispositions = exception_dispositions,
      .dispositions_count = ArrayCount(exception_dispositions),
    },
  }, &exception_decision);
  AssertAlways(exception_decision.emissions.first == 0 && exception_decision.command.kind == RVS_SchedulerCommand_Null);
  AssertAlways(session->scheduler.phase == RVS_SchedulerPhase_Stopped);
  AssertAlways(session->scheduler.run_intent.state == RVS_RunIntentState_Active);
  AssertAlways(session->scheduler.run_cycle_epoch == exception_cycle_epoch);
  session->scheduler.run_intent.state = RVS_RunIntentState_Cancelled;
  AssertAlways(rvs_scheduler_finish_run_locked(&session->scheduler, exception_run_request_id));
  rvs_scheduler_decision_release(&exception_decision);
  scratch_end(exception_scratch);
  mutex_drop(session->control->mutex);

  RVS_SubmitInfo invalid_command_submit = {0};
  AssertAlways(rvs_session_submit(session, (RVS_EngineCommand){0}, &invalid_command_submit) == RVS_Result_Error);
  AssertAlways(invalid_command_submit.request == 0 && invalid_command_submit.control == 0);
  RVS_SubmitInfo zero_program_run_submit = {0};
  AssertAlways(rvs_session_submit(session, (RVS_EngineCommand){
    .kind = RVS_EngineCommandKind_Run,
  }, &zero_program_run_submit) == RVS_Result_Error);
  AssertAlways(zero_program_run_submit.request == 0 && zero_program_run_submit.control == 0);
  RVS_ProgramID duplicate_programs[] = { reply.launch.program_id, reply.launch.program_id };
  RVS_SubmitInfo duplicate_programs_submit = {0};
  AssertAlways(rvs_session_run_many(session, duplicate_programs, ArrayCount(duplicate_programs), &duplicate_programs_submit) == RVS_Result_Error);
  AssertAlways(duplicate_programs_submit.request == 0 && duplicate_programs_submit.control == 0);
  RVS_SubmitInfo null_program_pointer_submit = {0};
  AssertAlways(rvs_session_submit(session, (RVS_EngineCommand){
    .kind = RVS_EngineCommandKind_Run,
    .run = { .programs_count = 1 },
  }, &null_program_pointer_submit) == RVS_Result_Error);
  AssertAlways(null_program_pointer_submit.request == 0 && null_program_pointer_submit.control == 0);
  RVS_ProgramID null_program = {0};
  RVS_SubmitInfo null_program_submit = {0};
  AssertAlways(rvs_session_submit(session, (RVS_EngineCommand){
    .kind = RVS_EngineCommandKind_Run,
    .run = { .programs_count = 1, .programs = &null_program },
  }, &null_program_submit) == RVS_Result_Error);
  AssertAlways(null_program_submit.request == 0 && null_program_submit.control == 0);
  rvs_test_wait_until_execution_state(session, reply.launch.program_id, RVS_TargetExecutionState_Idle);

  // These failure paths cannot occur during normal engine operation.
  ins_atomic_u32_eval_assign(&engine->test_fail_command_enqueue, 1);
  RVS_SubmitInfo enqueue_failure_submit = {0};
  AssertAlways(rvs_session_run(session, reply.launch.program_id, &enqueue_failure_submit) == RVS_Result_Error);
  AssertAlways(enqueue_failure_submit.request == 0 && enqueue_failure_submit.control == 0);
  rvs_test_wait_until_execution_state(session, reply.launch.program_id, RVS_TargetExecutionState_Idle);

  ins_atomic_u32_eval_assign(&engine->test_hold_before_dispatch, 1);
  RVS_SubmitInfo cancelled_run_submit = {0};
  AssertAlways(rvs_session_run(session, reply.launch.program_id, &cancelled_run_submit) == RVS_Result_Ok);
  rvs_test_wait_until_dispatch_held(engine);
  rvs_test_wait_until_execution_state(session, reply.launch.program_id, RVS_TargetExecutionState_Queued);
  RVS_SubmitInfo blocked_run_submit = {0};
  RVS_ProgramID other_program_id = { .u64 = { max_U64 - 1 } };
  AssertAlways(rvs_session_run(session, other_program_id, &blocked_run_submit) == RVS_Result_AlreadyPending);
  RVS_SubmitInfo blocked_launch_submit = {0};
  AssertAlways(rvs_session_launch(session, str8_lit("C:\\Windows\\System32\\where.exe"), str8_zero(), &blocked_launch_submit) == RVS_Result_AlreadyPending);
  mutex_take(session->control->mutex);
  AssertAlways( ! rvs_scheduler_execution_blocks_operation_locked(&session->scheduler, RVS_SchedulerOp_ReadOnly));
  mutex_drop(session->control->mutex);
  AssertAlways(rvs_request_control_cancel(cancelled_run_submit.control) == RVS_Result_Ok);
  rvs_test_wait_until_execution_state(session, reply.launch.program_id, RVS_TargetExecutionState_Idle);
  ins_atomic_u32_eval_assign(&engine->test_hold_before_dispatch, 0);
  RVS_EngineReply cancelled_run_reply = {0};
  AssertAlways(rvs_request_wait(cancelled_run_submit.request, max_U64, &cancelled_run_reply) == RVS_Result_Ok);
  AssertAlways(cancelled_run_reply.result == RVS_Result_Cancelled);
  rvs_request_release(cancelled_run_submit.request);
  rvs_request_control_release(cancelled_run_submit.control);
  rvs_test_wait_until_execution_state(session, reply.launch.program_id, RVS_TargetExecutionState_Idle);

  ins_atomic_u32_eval_assign(&engine->test_fail_demon_message_type, RVS_DemonMessage_Run);
  RVS_SubmitInfo demon_send_failure_submit = {0};
  AssertAlways(rvs_session_run(session, reply.launch.program_id, &demon_send_failure_submit) == RVS_Result_Ok);
  RVS_EngineReply demon_send_failure_reply = {0};
  AssertAlways(rvs_request_wait(demon_send_failure_submit.request, max_U64, &demon_send_failure_reply) == RVS_Result_Ok);
  AssertAlways(demon_send_failure_reply.result == RVS_Result_Error);
  rvs_request_release(demon_send_failure_submit.request);
  rvs_request_control_release(demon_send_failure_submit.control);
  rvs_test_wait_until_execution_state(session, reply.launch.program_id, RVS_TargetExecutionState_Idle);

  // Exercise DEMON reply transitions directly without fabricating backend failures.
  RVS_MessageID test_run_request_id = 0x1000;
  RVS_TargetSnapshot test_target = { .target = reply.launch.program_id };
  mutex_take(session->control->mutex);
  AssertAlways(rvs_scheduler_reserve_execution_locked(&session->scheduler, &test_target, 1, test_run_request_id));
  AssertAlways( ! rvs_scheduler_mark_run_in_flight_locked(&session->scheduler, test_run_request_id + 1));
  mutex_drop(session->control->mutex);
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_ActionResult,
    .request_id = test_run_request_id,
    .action_result = { .action = RVS_DemonAction_Run, .result = RVS_Result_Ok },
  });
  rvs_test_wait_until_execution_state(session, reply.launch.program_id, RVS_TargetExecutionState_RunInFlight);
  RVS_SchedulerKey interrupt_key = {
    .op = RVS_SchedulerOp_Interrupt,
    .identity = 1,
  };
  mutex_take(session->control->mutex);
  session->scheduler.phase = RVS_SchedulerPhase_Running;
  U64 interrupt_cycle_epoch = session->scheduler.run_cycle_epoch + 1;
  RVS_SchedulerAdmission interrupt_admission = {0};
  AssertAlways(rvs_scheduler_admit_locked(&session->scheduler, session->engine->request_pool, &session->engine->next_request_id, RVS_SchedulerOp_Interrupt, interrupt_key, &reply.launch.program_id, 1, 0, &interrupt_admission) == RVS_Result_Ok);
  AssertAlways(session->scheduler.run_cycle_epoch == interrupt_cycle_epoch);
  AssertAlways(session->scheduler.stop_transaction.cycle_epoch == interrupt_cycle_epoch);
  AssertAlways(session->scheduler.stop_transaction.interrupt_attempt == 1);
  AssertAlways(interrupt_admission.operation->targets[0].execution_token == rvs_scheduler_target_from_id_locked(&session->scheduler, reply.launch.program_id)->execution_token);
  AssertAlways(rvs_scheduler_target_from_id_locked(&session->scheduler, reply.launch.program_id)->state == RVS_TargetExecutionState_InterruptPending);
  Temp stop_scratch = scratch_begin(0, 0);
  DMN_EventList stop_events = {0};
  *dmn_event_list_push(stop_scratch.arena, &stop_events) = (DMN_Event){ .kind = DMN_EventKind_Halt, .process = reply.launch.program_id };
  RVS_SchedulerDecision stop_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = { .request_id = test_run_request_id, .events = stop_events, .dispositions = (RVS_SchedulerEventDisposition[1]){0}, .dispositions_count = 1 },
  }, &stop_decision);
  AssertAlways(stop_decision.command.kind == RVS_SchedulerCommand_ResumeTargetSubset);
  AssertAlways(stop_decision.command.resume_target_subset.targets_count == 0);
  AssertAlways(stop_decision.command.token.command_id != 0);
  AssertAlways(session->scheduler.resume_transaction.cycle_epoch == interrupt_cycle_epoch);
  AssertAlways(session->scheduler.resume_transaction.resume_attempt == 1);
  AssertAlways(session->scheduler.run_cycle_epoch == interrupt_cycle_epoch);
  rvs_scheduler_decision_release(&stop_decision);
  scratch_end(stop_scratch);
  MemoryZeroStruct(&interrupt_admission.operation->pending_command);
  AssertAlways(rvs_scheduler_finish_resume_transaction_locked(&session->scheduler, interrupt_admission.request->request_id) == RVS_Result_Ok);
  AssertAlways(session->scheduler.run_cycle_epoch == interrupt_cycle_epoch);
  rvs_scheduler_operation_remove_locked(&session->scheduler, interrupt_admission.operation);
  rvs_request_release(interrupt_admission.request); // drop unreturned caller ownership
  rvs_scheduler_operation_release(interrupt_admission.operation); // drop active registration ownership
  mutex_drop(session->control->mutex);
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_EventBatch,
    .request_id = test_run_request_id,
  });
  rvs_test_wait_until_execution_state(session, reply.launch.program_id, RVS_TargetExecutionState_Idle);
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_ExecutionFinished,
    .request_id = test_run_request_id + 1,
  });
  rvs_test_wait_until_execution_state(session, reply.launch.program_id, RVS_TargetExecutionState_Idle);
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_ExecutionFinished,
    .request_id = test_run_request_id,
  });
  rvs_test_wait_until_execution_state(session, reply.launch.program_id, RVS_TargetExecutionState_Idle);

  // A selected exit preempts the interrupt workflow rather than reporting a successful stop.
  mutex_take(session->control->mutex);
  AssertAlways(rvs_scheduler_reserve_execution_locked(&session->scheduler, &test_target, 1, test_run_request_id));
  AssertAlways(rvs_scheduler_mark_run_in_flight_locked(&session->scheduler, test_run_request_id));
  U64 exited_interrupt_cycle_epoch = session->scheduler.run_cycle_epoch + 1;
  RVS_SchedulerAdmission exited_interrupt_admission = {0};
  AssertAlways(rvs_scheduler_admit_locked(&session->scheduler, session->engine->request_pool, &session->engine->next_request_id, RVS_SchedulerOp_Interrupt, interrupt_key, &reply.launch.program_id, 1, 0, &exited_interrupt_admission) == RVS_Result_Ok);
  AssertAlways(session->scheduler.run_cycle_epoch == exited_interrupt_cycle_epoch);
  AssertAlways(session->scheduler.stop_transaction.cycle_epoch == exited_interrupt_cycle_epoch);
  AssertAlways(session->scheduler.stop_transaction.interrupt_attempt == 1);
  Temp selected_exit_scratch = scratch_begin(0, 0);
  DMN_EventList selected_exit_events = {0};
  *dmn_event_list_push(selected_exit_scratch.arena, &selected_exit_events) = (DMN_Event){ .kind = DMN_EventKind_ExitProcess, .process = reply.launch.program_id };
  RVS_SchedulerDecision selected_exit_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = { .request_id = test_run_request_id, .events = selected_exit_events, .dispositions = (RVS_SchedulerEventDisposition[1]){0}, .dispositions_count = 1 },
  }, &selected_exit_decision);
  AssertAlways(selected_exit_decision.command.kind == RVS_SchedulerCommand_ResumeTargetSubset);
  rvs_scheduler_decision_release(&selected_exit_decision);
  scratch_end(selected_exit_scratch);
  AssertAlways(session->scheduler.resume_transaction.cycle_epoch == exited_interrupt_cycle_epoch);
  AssertAlways(session->scheduler.resume_transaction.resume_attempt == 1);
  MemoryZeroStruct(&exited_interrupt_admission.operation->pending_command);
  AssertAlways(rvs_scheduler_finish_resume_transaction_locked(&session->scheduler, exited_interrupt_admission.request->request_id) == RVS_Result_StaleState);
  AssertAlways(session->scheduler.run_cycle_epoch == exited_interrupt_cycle_epoch);
  rvs_scheduler_operation_remove_locked(&session->scheduler, exited_interrupt_admission.operation);
  rvs_request_release(exited_interrupt_admission.request); // drop unreturned caller ownership
  rvs_scheduler_operation_release(exited_interrupt_admission.operation); // drop active registration ownership
  mutex_drop(session->control->mutex);

  RVS_SchedulerKey terminate_key = {
    .op = RVS_SchedulerOp_Terminate,
    .identity = 1,
  };
  mutex_take(session->control->mutex);
  RVS_ProgramID terminate_target = second_launch_reply.launch.program_id;
  RVS_TargetLedgerEntry *terminate_entry = rvs_scheduler_target_from_id_locked(&session->scheduler, terminate_target);
  U64 revision = terminate_entry->revision;
  RVS_SchedulerAdmission terminate_admission = {0};
  AssertAlways(rvs_scheduler_admit_locked(&session->scheduler, session->engine->request_pool, &session->engine->next_request_id, RVS_SchedulerOp_Terminate, terminate_key, &terminate_target, 1, 0, &terminate_admission) == RVS_Result_Ok);
  AssertAlways(terminate_entry->is_termination_fenced);
  AssertAlways(terminate_entry->revision > revision);
  RVS_SchedulerAdmission fenced_query = {0};
  AssertAlways(rvs_scheduler_admit_locked(&session->scheduler, session->engine->request_pool, &session->engine->next_request_id, RVS_SchedulerOp_ReadOnly, (RVS_SchedulerKey){ .op = RVS_SchedulerOp_ReadOnly, .target = terminate_target, .identity = 2 }, 0, 0, 0, &fenced_query) == RVS_Result_Ok);
  AssertAlways(fenced_query.is_terminal && fenced_query.request->reply.result == RVS_Result_StaleState);
  rvs_request_release(fenced_query.request);
  rvs_scheduler_rollback_admission_locked(&session->scheduler, &terminate_admission);
  AssertAlways( ! terminate_entry->is_termination_fenced);
  AssertAlways(terminate_entry->revision == revision);
  mutex_drop(session->control->mutex);

  // Table-drive the GlobalWithResume reducer over a two-target execution lease.
  RVS_MessageID reducer_run_request_id = 0x2000;
  mutex_take(session->control->mutex);
  RVS_TargetSnapshot reducer_run_targets[] = {
    { .target = run_programs[0] },
    { .target = run_programs[1] },
  };
  AssertAlways(rvs_scheduler_reserve_execution_locked(&session->scheduler, reducer_run_targets, ArrayCount(reducer_run_targets), reducer_run_request_id));
  AssertAlways(rvs_scheduler_mark_run_in_flight_locked(&session->scheduler, reducer_run_request_id));
  U64 reducer_cycle_epoch = session->scheduler.run_cycle_epoch + 1;
  RVS_SchedulerAdmission reducer_interrupt_admission = {0};
  AssertAlways(rvs_scheduler_admit_locked(&session->scheduler, session->engine->request_pool,
                                          &session->engine->next_request_id, RVS_SchedulerOp_Interrupt,
                                          interrupt_key, &run_programs[0], 1, 0,
                                          &reducer_interrupt_admission) == RVS_Result_Ok);
  AssertAlways(session->scheduler.run_cycle_epoch == reducer_cycle_epoch);
  AssertAlways(session->scheduler.stop_transaction.cycle_epoch == reducer_cycle_epoch);
  AssertAlways(session->scheduler.stop_transaction.interrupt_attempt == 1);
  RVS_ScheduledOperation *reducer_interrupt_operation = reducer_interrupt_admission.operation;
  RVS_Request *reducer_interrupt_request = reducer_interrupt_operation->request;

  RVS_ReducerEventTest reducer_event_tests[] = {
    { RVS_SchedulerEvent_DemonEventBatch, reducer_run_request_id + 1, 0, RVS_SchedulerEmission_Null, RVS_SchedulerCommand_Null,               RVS_SchedulerDecisionStatus_Applied,      0 },
    { RVS_SchedulerEvent_DemonEventBatch, reducer_run_request_id,     0, RVS_SchedulerEmission_Null, RVS_SchedulerCommand_Null,               RVS_SchedulerDecisionStatus_Applied,      0 },
    { RVS_SchedulerEvent_DemonEventBatch, reducer_run_request_id,     0, RVS_SchedulerEmission_Null, RVS_SchedulerCommand_Null,               RVS_SchedulerDecisionStatus_Applied,      0 },
    { RVS_SchedulerEvent_DemonEventBatch, reducer_run_request_id,     1, RVS_SchedulerEmission_Null, RVS_SchedulerCommand_ResumeTargetSubset, RVS_SchedulerDecisionStatus_Applied,      1 },
    { RVS_SchedulerEvent_DemonEventBatch, reducer_run_request_id,     1, RVS_SchedulerEmission_Null, RVS_SchedulerCommand_Null,               RVS_SchedulerDecisionStatus_Applied,      0 },
    { RVS_SchedulerEvent_CommandOutcome,  reducer_interrupt_request->request_id, 1, RVS_SchedulerEmission_Null, RVS_SchedulerCommand_Null, RVS_SchedulerDecisionStatus_IgnoredStale, 0 },
    { RVS_SchedulerEvent_CommandOutcome,  reducer_interrupt_request->request_id, 0, RVS_SchedulerEmission_CompleteRequest, RVS_SchedulerCommand_Null, RVS_SchedulerDecisionStatus_Applied, 0 },
    { RVS_SchedulerEvent_CommandOutcome,  reducer_interrupt_request->request_id, 0, RVS_SchedulerEmission_Null, RVS_SchedulerCommand_Null, RVS_SchedulerDecisionStatus_IgnoredStale, 0 },
  };
  RVS_SchedulerCommandToken reducer_resume_token = {0};
  for EachIndex(test_idx, ArrayCount(reducer_event_tests)) {
    RVS_ReducerEventTest *test = &reducer_event_tests[test_idx];
    RVS_SchedulerDecision decision = {0};
    RVS_SchedulerEvent event = { .kind = test->kind };
    RVS_SchedulerEventDisposition dispositions[1] = {0};
    Temp event_scratch = scratch_begin(0, 0);
    if (test->kind == RVS_SchedulerEvent_DemonEventBatch) {
      DMN_EventList events = {0};
      *dmn_event_list_push(event_scratch.arena, &events) = (DMN_Event){ .kind = DMN_EventKind_Halt, .process = run_programs[test->target_index] };
      event.demon_events.request_id = test->source_request_id;
      event.demon_events.events = events;
      event.demon_events.dispositions = dispositions;
      event.demon_events.dispositions_count = 1;
    } else if (test->kind == RVS_SchedulerEvent_CommandOutcome) {
      event.command_outcome.kind = RVS_SchedulerCommandOutcome_ResumeTargetSubsetCompleted;
      event.command_outcome.command_kind = RVS_SchedulerCommand_ResumeTargetSubset;
      event.command_outcome.command = reducer_resume_token;
      event.command_outcome.command.command_id += test->target_index;
      event.command_outcome.result = RVS_Result_Ok;
    }
    rvs_scheduler_apply_locked(&session->scheduler, event, &decision);
    AssertAlways((decision.emissions.first ? decision.emissions.first->kind : RVS_SchedulerEmission_Null) == test->expected_emission);
    AssertAlways(decision.command.kind == test->expected_command && decision.status == test->expected_status);
    if (decision.command.kind == RVS_SchedulerCommand_ResumeTargetSubset) {
      AssertAlways(decision.command.resume_target_subset.targets_count == test->expected_targets_count);
      AssertAlways(rvs_scheduler_begin_command_locked(&session->scheduler, &decision.command));
      reducer_resume_token = decision.command.token;
    }
    AssertAlways(session->scheduler.run_cycle_epoch == reducer_cycle_epoch);
    if (test_idx == 0) {
      AssertAlways(session->scheduler.phase == RVS_SchedulerPhase_Interrupting);
      for EachIndex(target_idx, reducer_interrupt_operation->targets_count) {
        AssertAlways( ! reducer_interrupt_operation->targets[target_idx].stop_observed);
      }
    } else if (test_idx == 3 || test_idx == 4) {
      AssertAlways(session->scheduler.resume_transaction.cycle_epoch == reducer_cycle_epoch);
      AssertAlways(session->scheduler.resume_transaction.resume_attempt == 1);
    }
    rvs_scheduler_decision_release(&decision);
    scratch_end(event_scratch);
  }
  AssertAlways(rvs_scheduler_finish_run_locked(&session->scheduler, reducer_run_request_id));
  rvs_request_release(reducer_interrupt_request); // drop the unreturned submit ownership

  RVS_MessageID reducer_exit_run_request_id = reducer_run_request_id + 1;
  AssertAlways(rvs_scheduler_reserve_execution_locked(&session->scheduler, reducer_run_targets, ArrayCount(reducer_run_targets), reducer_exit_run_request_id));
  AssertAlways(rvs_scheduler_mark_run_in_flight_locked(&session->scheduler, reducer_exit_run_request_id));
  U64 reducer_exit_cycle_epoch = session->scheduler.run_cycle_epoch + 1;
  RVS_SchedulerAdmission exit_interrupt_admission = {0};
  AssertAlways(rvs_scheduler_admit_locked(&session->scheduler, session->engine->request_pool,
                                          &session->engine->next_request_id, RVS_SchedulerOp_Interrupt,
                                          interrupt_key, &run_programs[0], 1, 0,
                                          &exit_interrupt_admission) == RVS_Result_Ok);
  RVS_ScheduledOperation *exit_interrupt_operation = exit_interrupt_admission.operation;
  RVS_Request *exit_interrupt_request = exit_interrupt_operation->request;
  AssertAlways(session->scheduler.run_cycle_epoch == reducer_exit_cycle_epoch);
  AssertAlways(session->scheduler.stop_transaction.cycle_epoch == reducer_exit_cycle_epoch);
  AssertAlways(session->scheduler.stop_transaction.interrupt_attempt == 1);
  RVS_ReducerEventTest exit_event_tests[] = {
    { RVS_SchedulerEvent_DemonEventBatch, reducer_exit_run_request_id, 1, RVS_SchedulerEmission_Null,         RVS_SchedulerCommand_Null,               RVS_SchedulerDecisionStatus_Applied, 0 },
    { RVS_SchedulerEvent_DemonEventBatch, reducer_exit_run_request_id, 0, RVS_SchedulerEmission_RetireTarget, RVS_SchedulerCommand_ResumeTargetSubset, RVS_SchedulerDecisionStatus_Applied, 1 },
    { RVS_SchedulerEvent_CommandOutcome,  exit_interrupt_request->request_id, 0, RVS_SchedulerEmission_CompleteRequest, RVS_SchedulerCommand_Null, RVS_SchedulerDecisionStatus_Applied, 0 },
  };
  RVS_SchedulerCommandToken exit_resume_token = {0};
  for EachIndex(test_idx, ArrayCount(exit_event_tests)) {
    RVS_ReducerEventTest *test = &exit_event_tests[test_idx];
    RVS_SchedulerDecision decision = {0};
    RVS_SchedulerEvent event = { .kind = test->kind };
    RVS_SchedulerEventDisposition dispositions[1] = {0};
    Temp event_scratch = scratch_begin(0, 0);
    if (test->kind == RVS_SchedulerEvent_DemonEventBatch) {
      DMN_EventList events = {0};
      *dmn_event_list_push(event_scratch.arena, &events) = (DMN_Event){
        .kind = test_idx == 0 ? DMN_EventKind_Halt : DMN_EventKind_ExitProcess,
        .process = run_programs[test->target_index],
      };
      event.demon_events.request_id = test->source_request_id;
      event.demon_events.events = events;
      event.demon_events.dispositions = dispositions;
      event.demon_events.dispositions_count = 1;
    } else if (test->kind == RVS_SchedulerEvent_CommandOutcome) {
      event.command_outcome.kind = RVS_SchedulerCommandOutcome_ResumeTargetSubsetCompleted;
      event.command_outcome.command_kind = RVS_SchedulerCommand_ResumeTargetSubset;
      event.command_outcome.command = exit_resume_token;
      event.command_outcome.result = RVS_Result_Ok;
    }
    rvs_scheduler_apply_locked(&session->scheduler, event, &decision);
    AssertAlways((decision.emissions.first ? decision.emissions.first->kind : RVS_SchedulerEmission_Null) == test->expected_emission);
    AssertAlways(decision.command.kind == test->expected_command && decision.status == test->expected_status);
    AssertAlways(session->scheduler.run_cycle_epoch == reducer_exit_cycle_epoch);
    if (test_idx == 1) {
      AssertAlways(decision.emissions.first->next == 0);
      AssertAlways(decision.command.resume_target_subset.targets_count == test->expected_targets_count);
      AssertAlways(rvs_scheduler_begin_command_locked(&session->scheduler, &decision.command));
      exit_resume_token = decision.command.token;
      AssertAlways(session->scheduler.resume_transaction.cycle_epoch == reducer_exit_cycle_epoch);
      AssertAlways(session->scheduler.resume_transaction.resume_attempt == 1);
    }
    if (test->kind == RVS_SchedulerEvent_CommandOutcome) { AssertAlways(decision.emissions.first->reply.result == RVS_Result_StaleState); }
    rvs_scheduler_decision_release(&decision);
    scratch_end(event_scratch);
  }
  AssertAlways(rvs_scheduler_finish_run_locked(&session->scheduler, reducer_exit_run_request_id));
  rvs_request_release(exit_interrupt_request); // drop the unreturned submit ownership

  // Events after the stop barrier is satisfied must amend the final resume effect.
  RVS_ProgramID batch_targets[] = {
    { .u32 = { 0x3001, 1 } },
    { .u32 = { 0x3002, 1 } },
  };
  for EachIndex(target_idx, ArrayCount(batch_targets)) {
    rvs_scheduler_target_add_locked(&session->scheduler, batch_targets[target_idx]);
  }
  RVS_MessageID batch_run_request_id = 0x3000;
  RVS_TargetSnapshot batch_run_targets[] = {
    { .target = batch_targets[0] },
    { .target = batch_targets[1] },
  };
  AssertAlways(rvs_scheduler_reserve_execution_locked(&session->scheduler, batch_run_targets, ArrayCount(batch_run_targets), batch_run_request_id));
  AssertAlways(rvs_scheduler_mark_run_in_flight_locked(&session->scheduler, batch_run_request_id));
  session->scheduler.phase = RVS_SchedulerPhase_Running;
  RVS_SchedulerAdmission batch_interrupt_admission = {0};
  AssertAlways(rvs_scheduler_admit_locked(&session->scheduler, session->engine->request_pool,
                                          &session->engine->next_request_id, RVS_SchedulerOp_Interrupt,
                                          (RVS_SchedulerKey){ .op = RVS_SchedulerOp_Interrupt, .identity = 2 },
                                          &batch_targets[0], 1, 0, &batch_interrupt_admission) == RVS_Result_Ok);
  Temp batch_scratch = scratch_begin(0, 0);
  DMN_EventList batch_events = {0};
  *dmn_event_list_push(batch_scratch.arena, &batch_events) = (DMN_Event){ .kind = DMN_EventKind_Halt,        .process = batch_targets[0] };
  *dmn_event_list_push(batch_scratch.arena, &batch_events) = (DMN_Event){ .kind = DMN_EventKind_Halt,        .process = batch_targets[1] };
  *dmn_event_list_push(batch_scratch.arena, &batch_events) = (DMN_Event){ .kind = DMN_EventKind_ExitProcess, .process = batch_targets[1] };
  *dmn_event_list_push(batch_scratch.arena, &batch_events) = (DMN_Event){ .kind = DMN_EventKind_ExitProcess, .process = batch_targets[0] };
  RVS_SchedulerEventDisposition batch_dispositions[4] = {0};
  RVS_SchedulerDecision batch_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = {
      .request_id = batch_run_request_id,
      .events = batch_events,
      .dispositions = batch_dispositions,
      .dispositions_count = ArrayCount(batch_dispositions),
    },
  }, &batch_decision);
  RVS_SchedulerEmission *batch_emission = batch_decision.emissions.first;
  AssertAlways(batch_emission && batch_emission->kind == RVS_SchedulerEmission_RetireTarget);
  batch_emission = batch_emission->next;
  AssertAlways(batch_emission && batch_emission->kind == RVS_SchedulerEmission_RetireTarget);
  AssertAlways(batch_emission->next == 0);
  AssertAlways(batch_decision.command.kind == RVS_SchedulerCommand_ResumeTargetSubset);
  AssertAlways(batch_decision.command.resume_target_subset.targets_count == 0);
  for EachIndex(target_idx, batch_interrupt_admission.operation->targets_count) {
    AssertAlways(batch_interrupt_admission.operation->targets[target_idx].stop_observed);
    AssertAlways(batch_interrupt_admission.operation->targets[target_idx].exit_observed);
  }
  rvs_scheduler_decision_release(&batch_decision);
  scratch_end(batch_scratch);
  MemoryZeroStruct(&batch_interrupt_admission.operation->pending_command);
  AssertAlways(rvs_scheduler_finish_resume_transaction_locked(&session->scheduler, batch_interrupt_admission.request->request_id) == RVS_Result_StaleState);
  rvs_scheduler_operation_remove_locked(&session->scheduler, batch_interrupt_admission.operation);
  rvs_request_release(batch_interrupt_admission.request);
  rvs_scheduler_operation_release(batch_interrupt_admission.operation);
  mutex_drop(session->control->mutex);

  mutex_take(session->control->mutex);
  AssertAlways(rvs_scheduler_reserve_execution_locked(&session->scheduler, &test_target, 1, test_run_request_id));
  AssertAlways(rvs_scheduler_clear_queued_execution_locked(&session->scheduler, test_run_request_id));
  AssertAlways(rvs_scheduler_reserve_execution_locked(&session->scheduler, &test_target, 1, test_run_request_id));
  mutex_drop(session->control->mutex);
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_ActionResult,
    .request_id = test_run_request_id,
    .action_result = { .action = RVS_DemonAction_Run, .result = RVS_Result_Error },
  });
  rvs_test_wait_until_execution_state(session, reply.launch.program_id, RVS_TargetExecutionState_Idle);

  RVS_SchedulerKey join_key = {
    .op       = RVS_SchedulerOp_ReadOnly,
    .target   = reply.launch.program_id,
    .identity = 1,
  };
  mutex_take(session->control->mutex);
  RVS_ScheduledOperation *join_operation = rvs_scheduler_operation_alloc_locked(&session->scheduler,
                                                                                   session->engine->request_pool,
                                                                                   ins_atomic_u64_inc_eval(&session->engine->next_request_id),
                                                                                   join_key, 0, 0, 0);
  RVS_Request *join_request = join_operation->request;
  AssertAlways(rvs_scheduler_register_operation_locked(&session->scheduler, session->engine->request_pool, join_key, join_operation) == RVS_Result_Ok);
  AssertAlways(rvs_scheduler_register_operation_locked(&session->scheduler, session->engine->request_pool, join_key, join_operation) == RVS_Result_AlreadyPending);
  AssertAlways(rvs_scheduler_unregister_operation_locked(&session->scheduler, join_key) == join_operation);
  rvs_scheduler_operation_remove_locked(&session->scheduler, join_operation);
  mutex_drop(session->control->mutex);
  rvs_request_release(join_request); // drop caller ownership
  rvs_scheduler_operation_release(join_operation); // drop keyed registration ownership
  rvs_scheduler_operation_release(join_operation); // drop active registration ownership

  RVS_RequestPool *foreign_pool = rvs_request_pool_alloc();
  mutex_take(session->control->mutex);
  RVS_ScheduledOperation *foreign_operation = rvs_scheduler_operation_alloc_locked(&session->scheduler,
                                                                                      foreign_pool,
                                                                                      ins_atomic_u64_inc_eval(&session->engine->next_request_id),
                                                                                      join_key, 0, 0, 0);
  RVS_Request *foreign_request = foreign_operation->request;
  AssertAlways(foreign_operation == join_operation); // final releases recycle operations across request pools
  AssertAlways(rvs_scheduler_register_operation_locked(&session->scheduler, session->engine->request_pool, join_key, foreign_operation) == RVS_Result_Error);
  rvs_scheduler_operation_remove_locked(&session->scheduler, foreign_operation);
  mutex_drop(session->control->mutex);
  rvs_request_release(foreign_request); // drop caller ownership
  rvs_scheduler_operation_release(foreign_operation); // drop active registration ownership
  rvs_request_pool_release_engine(foreign_pool);

  mutex_take(session->control->mutex);
  RVS_ScheduledOperation *snapshot_operation = rvs_scheduler_operation_alloc_locked(&session->scheduler,
                                                                                       session->engine->request_pool,
                                                                                       ins_atomic_u64_inc_eval(&session->engine->next_request_id),
                                                                                       join_key, &reply.launch.program_id, 1, 0);
  RVS_TargetSnapshotStorage *snapshot_storage = snapshot_operation->target_storage;
  RVS_Request *snapshot_request = snapshot_operation->request;
  rvs_scheduler_operation_remove_locked(&session->scheduler, snapshot_operation);
  mutex_drop(session->control->mutex);
  rvs_request_release(snapshot_request);
  rvs_scheduler_operation_release(snapshot_operation);

  mutex_take(session->control->mutex);
  RVS_ScheduledOperation *reused_snapshot_operation = rvs_scheduler_operation_alloc_locked(&session->scheduler,
                                                                                              session->engine->request_pool,
                                                                                              ins_atomic_u64_inc_eval(&session->engine->next_request_id),
                                                                                              join_key, &reply.launch.program_id, 1, 0);
  AssertAlways(reused_snapshot_operation->target_storage == snapshot_storage);
  RVS_Request *reused_snapshot_request = reused_snapshot_operation->request;
  rvs_scheduler_operation_remove_locked(&session->scheduler, reused_snapshot_operation);
  RVS_SchedulerDecision recycled_decision = { .result = RVS_Result_Ok };
  RVS_SchedulerEmission *recycled_emission = rvs_scheduler_emit_locked(&session->scheduler, &recycled_decision, RVS_SchedulerEmission_RetireTarget, 0);
  rvs_scheduler_decision_release(&recycled_decision);
  recycled_decision.result = RVS_Result_Ok;
  rvs_scheduler_emit_locked(&session->scheduler, &recycled_decision, RVS_SchedulerEmission_RetireTarget, 0);
  AssertAlways(recycled_decision.emissions.first == recycled_emission);
  rvs_scheduler_decision_release(&recycled_decision);
  mutex_drop(session->control->mutex);
  rvs_request_release(reused_snapshot_request);
  rvs_scheduler_operation_release(reused_snapshot_operation);

  enum { operation_release_stress_thread_count = 4, operation_release_stress_releases_per_thread = 64 };
  mutex_take(session->control->mutex);
  RVS_ScheduledOperation *stress_operation = rvs_scheduler_operation_alloc_locked(&session->scheduler,
                                                                                     session->engine->request_pool,
                                                                                     ins_atomic_u64_inc_eval(&session->engine->next_request_id),
                                                                                     join_key, &reply.launch.program_id, 1, 0);
  RVS_Request *stress_request = stress_operation->request;
  rvs_scheduler_operation_remove_locked(&session->scheduler, stress_operation);
  for EachIndex(ref_idx, operation_release_stress_thread_count*operation_release_stress_releases_per_thread - 1) {
    rvs_scheduler_operation_addref(stress_operation);
  }
  mutex_drop(session->control->mutex);
  rvs_request_release(stress_request); // the concurrent releases own all remaining operation references
  RVS_OperationReleaseStress operation_release_stress = {
    .operation = stress_operation,
    .releases = operation_release_stress_releases_per_thread,
  };
  Thread operation_release_threads[operation_release_stress_thread_count] = {0};
  for EachIndex(thread_idx, operation_release_stress_thread_count) {
    operation_release_threads[thread_idx] = thread_launch(rvs_operation_release_stress_thread, &operation_release_stress);
    AssertAlways( ! MemoryIsZeroStruct(&operation_release_threads[thread_idx]));
  }
  for EachIndex(thread_idx, operation_release_stress_thread_count) {
    thread_join(operation_release_threads[thread_idx], max_U64);
  }
  mutex_take(session->control->mutex);
  RVS_ScheduledOperation *recycled_stress_operation = rvs_scheduler_operation_alloc_locked(&session->scheduler,
                                                                                              session->engine->request_pool,
                                                                                              ins_atomic_u64_inc_eval(&session->engine->next_request_id),
                                                                                              join_key, &reply.launch.program_id, 1, 0);
  AssertAlways(recycled_stress_operation == stress_operation);
  RVS_Request *recycled_stress_request = recycled_stress_operation->request;
  rvs_scheduler_operation_remove_locked(&session->scheduler, recycled_stress_operation);
  mutex_drop(session->control->mutex);
  rvs_request_release(recycled_stress_request);
  rvs_scheduler_operation_release(recycled_stress_operation);

  lifecycle_key.identity = 2;
  RVS_SchedulerKey session_execution_key = {
    .op = RVS_SchedulerOp_Run,
    .identity = RVS_EngineCommandKind_Run,
  };
  AssertAlways(rvs_scheduler_keys_conflict(lifecycle_key, session_execution_key));
  AssertAlways(rvs_scheduler_keys_conflict(session_execution_key, session_execution_key));
  RVS_SchedulerKey query_key = {
    .op       = RVS_SchedulerOp_ReadOnly,
    .target   = { .u64 = { 1 } },
    .identity = 6,
  };
  AssertAlways(rvs_scheduler_keys_conflict(lifecycle_key, query_key));
  AssertAlways( ! rvs_scheduler_keys_conflict(session_execution_key, query_key));

  RVS_EventWaitTest event_waiter = { .session = session };
  Thread event_thread = thread_launch(rvs_event_wait_test_thread, &event_waiter);
  AssertAlways( ! MemoryIsZeroStruct(&event_thread));
  rvs_test_wait_until_event_waiting(session);
  ins_atomic_u32_eval_assign(&engine->test_hold_before_dispatch, 1);
  RVS_SubmitInfo shutdown_held_run_submit = {0};
  AssertAlways(rvs_session_run(session, run_programs[1], &shutdown_held_run_submit) == RVS_Result_Ok);
  rvs_test_wait_until_dispatch_held(engine);
  RVS_Request *shutdown_pending_launch = rvs_test_request_alloc(session, lifecycle_key, 0);
  AssertAlways(rvs_test_launch_started(engine, shutdown_pending_launch->request_id, 700));
  RVS_SchedulerCommandToken shutdown_pending_token = rvs_test_launch_pump_token(engine, shutdown_pending_launch->request_id);
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
  RVS_EngineReply shutdown_pending_reply = {0};
  AssertAlways(rvs_request_wait(shutdown_pending_launch, max_U64, &shutdown_pending_reply) == RVS_Result_Ok);
  AssertAlways(shutdown_pending_reply.result == RVS_Result_EngineStopped);
  rvs_request_release(shutdown_pending_launch);
  mutex_take(session->control->mutex);
  RVS_SchedulerDecision late_command_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_CommandOutcome,
    .command_outcome = {
      .kind = RVS_SchedulerCommandOutcome_Failed,
      .command_kind = RVS_SchedulerCommand_PumpLaunch,
      .command = shutdown_pending_token,
      .result = RVS_Result_Error,
    },
  }, &late_command_decision);
  AssertAlways(late_command_decision.status == RVS_SchedulerDecisionStatus_IgnoredStale);
  rvs_scheduler_decision_release(&late_command_decision);
  mutex_drop(session->control->mutex);
  AssertAlways( ! shutdown_held_run_submit.control->operation->is_dispatched);
  rvs_request_release(shutdown_held_run_submit.request);
  rvs_request_control_release(shutdown_held_run_submit.control);
  rvs_test_wait_until_execution_state(session, run_programs[1], RVS_TargetExecutionState_Idle);
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
rvs_operation_release_stress_thread(void *user_data)
{
  RVS_OperationReleaseStress *stress = user_data;
  for EachIndex(release_idx, stress->releases) {
    rvs_scheduler_operation_release(stress->operation);
  }
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
rvs_test_wait_until_execution_state(RVS_Session *session, RVS_ProgramID target, RVS_TargetExecutionState state)
{
  for (;;) {
    mutex_take(session->control->mutex);
    RVS_TargetLedgerEntry *entry = rvs_scheduler_target_from_id_locked(&session->scheduler, target);
    B32 matches = entry != 0 && entry->state == state;
    mutex_drop(session->control->mutex);
    if (matches) { break; }
    sleep_ms(1);
  }
}

internal B32
rvs_test_launch_started(RVS_Engine *engine, RVS_MessageID request_id, U32 expected_pid)
{
  RVS_SchedulerDecision decision = {0};
  rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_LaunchStarted,
    .launch_started = { .request_id = request_id, .pid = expected_pid },
  }, &decision);
  B32 accepted = decision.command.kind == RVS_SchedulerCommand_PumpLaunch;
  if (accepted) {
    mutex_take(engine->control->mutex);
    AssertAlways(rvs_scheduler_begin_command_locked(&engine->session->scheduler, &decision.command));
    mutex_drop(engine->control->mutex);
  }
  rvs_scheduler_decision_release(&decision);
  return accepted;
}

internal RVS_SchedulerCommandToken
rvs_test_launch_pump_token(RVS_Engine *engine, RVS_MessageID request_id)
{
  mutex_take(engine->control->mutex);
  RVS_ScheduledOperation *operation = rvs_scheduler_find_active_operation_locked(&engine->session->scheduler, request_id);
  AssertAlways(operation && operation->pending_command.kind == RVS_SchedulerCommand_PumpLaunch);
  if (!operation->pending_command.has_started) {
    RVS_SchedulerCommand command = {
      .kind = operation->pending_command.kind,
      .token = operation->pending_command.token,
      .operation = operation,
    };
    AssertAlways(rvs_scheduler_begin_command_locked(&engine->session->scheduler, &command));
  }
  RVS_SchedulerCommandToken token = operation->pending_command.token;
  mutex_drop(engine->control->mutex);
  return token;
}

internal B32
rvs_test_launch_event(RVS_Engine *engine, RVS_MessageID request_id, DMN_Event event)
{
  Temp scratch = scratch_begin(0, 0);
  DMN_EventList events = {0};
  *dmn_event_list_push(scratch.arena, &events) = event;
  RVS_SchedulerEventDisposition *dispositions = push_array(scratch.arena, RVS_SchedulerEventDisposition, 1);
  RVS_SchedulerCommandToken token = rvs_test_launch_pump_token(engine, request_id);
  RVS_SchedulerDecision decision = {0};
  rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = {
      .source = RVS_SchedulerEventBatchSource_PumpLaunch,
      .command = token,
      .request_id = request_id,
      .events = events,
      .dispositions = dispositions,
      .dispositions_count = 1,
    },
  }, &decision);
  B32 accepted = decision.status == RVS_SchedulerDecisionStatus_Applied;
  rvs_engine_execute_scheduler_decision(engine, &decision);
  scratch_end(scratch);
  return accepted;
}

internal RVS_Request *
rvs_test_request_alloc(RVS_Session *session, RVS_SchedulerKey key, RVS_ScheduledOperation **operation_out)
{
  mutex_take(session->control->mutex);
  RVS_ScheduledOperation *operation = rvs_scheduler_operation_alloc_locked(&session->scheduler,
                                                                               session->engine->request_pool,
                                                                               ins_atomic_u64_inc_eval(&session->engine->next_request_id),
                                                                               key, 0, 0, 0);
  mutex_drop(session->control->mutex);
  if (operation_out) { *operation_out = operation; }
  return operation->request;
}
