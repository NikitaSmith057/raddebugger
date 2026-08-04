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
#include "radvs/tests/rvs_scheduler_test_support.c"

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
  RVS_Result                  expected_result;
  U64                     expected_targets_count;
} RVS_ReducerEventTest;

internal void rvs_event_wait_test_thread(void *user_data);
internal void rvs_test_wait_until_event_waiting(RVS_Session *session);
internal void rvs_engine_shutdown_test_thread(void *user_data);
internal void rvs_test_wait_until_shutdown(RVS_Session *session);
internal void rvs_test_wait_until_dispatch_held(RVS_Engine *engine);
internal void rvs_test_wait_until_dispatch_prepared(RVS_Engine *engine);
internal void rvs_test_wait_until_execution_state(RVS_Session *session, RVS_ProgramID target, RVS_TargetExecutionState state);
internal RVS_Request *rvs_test_request_alloc(RVS_Session *session, RVS_SchedulerKey key, RVS_ScheduledOperation **operation_out);
internal void rvs_test_activate_interrupt_locked(RVS_Session *session, RVS_SchedulerAdmission *admission);
internal B32 rvs_test_launch_started(RVS_Engine *engine, RVS_MessageID request_id, U32 expected_pid);
internal B32 rvs_test_launch_event(RVS_Engine *engine, RVS_MessageID request_id, DMN_Event event);
internal RVS_SchedulerCommandToken rvs_test_launch_pump_token(RVS_Engine *engine, RVS_MessageID request_id);
internal void rvs_operation_release_stress_thread(void *user_data);
internal void rvs_test_program_identity_bindings(RVS_Engine *engine, RVS_Session *session);
internal void rvs_test_entity_store(void);

internal void
rvs_request_wait_test_thread(void *user_data)
{
  RVS_RequestWaitTest *test = user_data;
  test->result = rvs_request_wait(test->request, max_U64, &test->reply);
  rvs_request_release(test->request);
}

internal void
rvs_test_program_identity_bindings(RVS_Engine *engine, RVS_Session *session)
{
  DMN_Handle processes[] = { { .u64 = { 0x1234 } }, { .u64 = { 0x5678 } } };
  RVS_ProgramID programs[2] = {0};
  for EachIndex(index, ArrayCount(programs)) {
    RVS_Request *request = rvs_test_request_alloc(session, (RVS_SchedulerKey){
      .op = RVS_SchedulerOp_Launch, .identity = RVS_EngineCommandKind_Launch,
    }, 0);
    mutex_take(session->control->mutex);
    RVS_ScheduledOperation *operation = rvs_scheduler_operation_from_request_id_locked(&session->scheduler, request->request_id);
    operation->launch_phase = RVS_LaunchPhase_AwaitLaunchStarted;
    mutex_drop(session->control->mutex);
    AssertAlways(rvs_test_launch_started(engine, request->request_id, 77));
    RVS_SchedulerCommandToken pump = rvs_test_launch_pump_token(engine, request->request_id);
    Temp scratch = scratch_begin(0, 0);
    DMN_EventList events = {0};
    *dmn_event_list_push(scratch.arena, &events) = (DMN_Event){
      .kind = DMN_EventKind_CreateProcess, .process = processes[index], .system_process_id = 77,
    };
    RVS_SchedulerDecision decision = {0};
    rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
      .kind = RVS_SchedulerEvent_DemonEventBatch,
      .demon_events = { .source = RVS_SchedulerEventBatchSource_PumpLaunch, .command = pump,
                        .request_id = request->request_id, .events = events,
                        .dispositions = (RVS_SchedulerEventDisposition[1]){0}, .dispositions_count = 1 },
    }, &decision);
    AssertAlways(decision.command.kind == RVS_SchedulerCommand_PublishTarget);
    RVS_SchedulerCommandToken publish = decision.command.token;
    rvs_scheduler_decision_release(&decision);
    rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
      .kind = RVS_SchedulerEvent_CommandOutcome,
      .command_outcome = { .command_kind = RVS_SchedulerCommand_PublishTarget, .command = publish,
                           .result = RVS_Result_Ok, .process = rvs_process_id_from_handle(processes[index]), .pid = 77 },
    }, &decision);
    AssertAlways(decision.emissions.first && !rvs_program_id_is_zero(decision.emissions.first->reply.launch.program_id));
    programs[index] = decision.emissions.first->reply.launch.program_id;
    rvs_test_execute_scheduler_decision(engine, &decision);
    scratch_end(scratch);
    rvs_request_release(request);
  }
  AssertAlways(!rvs_program_id_match(programs[0], programs[1]));

  Temp scratch = scratch_begin(0, 0);
  DMN_Handle child_process = { .u64 = { 0x9abc } };
  DMN_EventList events = {0};
  *dmn_event_list_push(scratch.arena, &events) = (DMN_Event){
    .kind = DMN_EventKind_CreateProcess, .process = child_process, .parent_process = processes[0], .system_process_id = 78,
  };
  RVS_SchedulerDecision decision = {0};
  rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = { .events = events, .dispositions = (RVS_SchedulerEventDisposition[1]){0}, .dispositions_count = 1 },
  }, &decision);
  AssertAlways(decision.emissions.first && decision.emissions.first->event.kind == RVS_EventKind_ProcessCreated &&
               rvs_program_id_match(decision.emissions.first->event.program, programs[0]));
  rvs_scheduler_decision_release(&decision);
  events = (DMN_EventList){0};
  *dmn_event_list_push(scratch.arena, &events) = (DMN_Event){ .kind = DMN_EventKind_ExitProcess, .process = child_process };
  rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = { .events = events, .dispositions = (RVS_SchedulerEventDisposition[1]){0}, .dispositions_count = 1 },
  }, &decision);
  AssertAlways(decision.emissions.first && decision.emissions.first->event.kind == RVS_EventKind_ProcessExited &&
               decision.emissions.first->next == 0);
  rvs_scheduler_decision_release(&decision);
  mutex_take(session->control->mutex);
  AssertAlways(rvs_scheduler_target_from_id_locked(&session->scheduler, programs[0])->state != RVS_TargetState_Removed);
  mutex_drop(session->control->mutex);
  scratch_end(scratch);
}

internal void
rvs_test_entity_store(void)
{
  Arena *arena = arena_alloc(.name = "Entity Store Test");
  Mutex mutex = mutex_alloc();
  RVS_EntityStore store = {0};
  RVS_ProgramID program_id = { .value = 1 };
  RVS_ProcessID parent_process = { .u64 = { 2 } };
  RVS_ProcessID child_process = { .u64 = { 3 } };
  RVS_ThreadID thread_id = { .u64 = { 4 } };
  rvs_entity_store_init(&store, arena, mutex);

  mutex_take(mutex);
  rvs_entity_program_create_locked(&store, program_id, 77);
  rvs_entity_process_create_locked(&store, program_id, parent_process, rvs_process_id_zero(), 77);
  rvs_entity_process_create_locked(&store, rvs_program_id_zero(), child_process, parent_process, 78);
  rvs_entity_thread_create_locked(&store, child_process, thread_id, 79);
  mutex_drop(mutex);

  RVS_ProgramSnapshot program = {0};
  RVS_ProcessSnapshot process = {0};
  RVS_ThreadSnapshot thread = {0};
  RVS_ProgramSnapshot *programs = 0;
  U64 programs_count = 0;
  AssertAlways(rvs_entity_fetch_program(&store, program_id, 0, &program) == RVS_Result_Ok);
  AssertAlways(rvs_entity_fetch_process(&store, child_process, 0, &process) == RVS_Result_Ok);
  AssertAlways(rvs_entity_fetch_thread(&store, thread_id, 0, &thread) == RVS_Result_Ok);
  AssertAlways(rvs_entity_copy_programs(&store, arena, &programs, &programs_count) == RVS_Result_Ok);
  AssertAlways(program.pid == 77 && rvs_program_id_match(process.program, program_id) &&
                rvs_process_id_match(process.parent_process, parent_process) &&
                rvs_process_id_match(thread.process, child_process) && thread.tid == 79 &&
                programs_count == 1 && rvs_program_id_match(programs[0].id, program_id));
  AssertAlways(rvs_entity_fetch_program(&store, (RVS_ProgramID){ .value = 99 }, 0, &program) == RVS_Result_Timeout);

  rvs_entity_store_close(&store);
  AssertAlways(rvs_entity_fetch_process(&store, (RVS_ProcessID){ .u64 = { 99 } }, 0, &process) == RVS_Result_EngineStopped);
  rvs_entity_store_release(&store);
  mutex_release(mutex);
  arena_release(arena);
}

internal void
entry_point(CmdLine *cmdline)
{
  (void)cmdline;

  rvs_test_entity_store();

  RVS_Queue *queue = rvs_queue_alloc(sizeof(RVS_QueueTestMessage), AlignOf(RVS_QueueTestMessage));
  RVS_QueueTestMessage *queued = rvs_queue_alloc_struct(queue, RVS_QueueTestMessage);
  RVS_QueueTestMessage *rejected = rvs_queue_alloc_struct(queue, RVS_QueueTestMessage);
  AssertAlways(queued != 0 && rejected != 0);
  queued->value = 1;
  AssertAlways(rvs_queue_push(queue, &queued->base) == RVS_Result_Ok);
  rvs_queue_close(queue);
  AssertAlways(rvs_queue_push(queue, &rejected->base) == RVS_Result_EngineStopped);
  RVS_QueueTestMessage *popped = rvs_queue_pop_struct(queue, RVS_QueueTestMessage, 0);
  AssertAlways(popped == queued && popped->value == 1);
  rvs_queue_recycle(queue, &popped->base);
  AssertAlways(rvs_queue_alloc_item(queue) == 0);
  rvs_queue_release(queue);

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
  DMN_TrapChunkList copy_traps = {0};
  DMN_Trap copy_trap = {
    .process = terminate_handles[0],
    .vaddr = 0x12345678,
    .id = copy_command_id,
    .flags = 0,
    .size = 1,
  };
  dmn_trap_chunk_list_push(terminate_copy_scratch.arena, &copy_traps, 8, &copy_trap);
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
      .traps = copy_traps,
    },
  });
  AssertAlways(resume_copy.resume.command_id == copy_command_id && resume_copy.resume.execution_request_id == 42);
  AssertAlways(resume_copy.resume.processes != terminate_handles && resume_copy.resume.processes_count == ArrayCount(terminate_handles));
  AssertAlways(resume_copy.resume.traps.trap_count == 1 && resume_copy.resume.traps.first != copy_traps.first);
  AssertAlways(resume_copy.resume.traps.first->v[0].vaddr == copy_trap.vaddr &&
               resume_copy.resume.traps.first->v[0].id == copy_trap.id);
  RVS_DemonMessage run_copy = {0};
  rvs_demon_message_copy(terminate_copy_scratch.arena, &run_copy, &(RVS_DemonMessage){
    .type = RVS_DemonMessage_Run,
    .run = {
      .processes = terminate_handles,
      .processes_count = ArrayCount(terminate_handles),
      .traps = copy_traps,
    },
  });
  AssertAlways(run_copy.run.traps.trap_count == 1 && run_copy.run.traps.first != copy_traps.first);
  copy_traps.first->v[0].vaddr += 1;
  AssertAlways(run_copy.run.traps.first->v[0].vaddr == copy_trap.vaddr &&
               resume_copy.resume.traps.first->v[0].vaddr == copy_trap.vaddr);
  RVS_DemonMessage interrupt_copy = {0};
  rvs_demon_message_copy(terminate_copy_scratch.arena, &interrupt_copy, &(RVS_DemonMessage){
    .type = RVS_DemonMessage_InterruptExecution,
    .request_id = 7,
    .interrupt_execution = { .execution_request_id = 42, .command_id = copy_command_id },
  });
  AssertAlways(interrupt_copy.request_id == 7 && interrupt_copy.interrupt_execution.execution_request_id == 42 &&
               interrupt_copy.interrupt_execution.command_id == copy_command_id);
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
  RVS_DemonReply execution_stopped_copy = {0};
  rvs_demon_reply_copy(terminate_copy_scratch.arena, &execution_stopped_copy, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_ExecutionStopped,
    .request_id = 7,
    .execution_stopped = { .command_id = copy_command_id },
  });
  AssertAlways(execution_stopped_copy.request_id == 7 && execution_stopped_copy.execution_stopped.command_id == copy_command_id);

  RVS_Queue *demon_reply_queue = rvs_queue_alloc(sizeof(RVS_EngineMessage), AlignOf(RVS_EngineMessage));
  AssertAlways(rvs_queue_push_copy(demon_reply_queue, &(RVS_EngineMessage){
    .type = RVS_EngineMessageType_DemonReply,
    .demon_reply = {
      .kind = RVS_DemonReplyKind_EventBatch,
      .event_batch = { .events = copy_events, .command_id = copy_command_id },
    },
  }, rvs_engine_message_queue_copy) == RVS_Result_Ok);
  scratch_end(terminate_copy_scratch);
  RVS_EngineMessage *queued_demon_reply = rvs_queue_pop_struct(demon_reply_queue, RVS_EngineMessage, 0);
  AssertAlways(queued_demon_reply && queued_demon_reply->demon_reply.kind == RVS_DemonReplyKind_EventBatch &&
               queued_demon_reply->demon_reply.event_batch.command_id == copy_command_id &&
               queued_demon_reply->demon_reply.event_batch.events.first->v.kind == DMN_EventKind_Halt);
  rvs_queue_recycle(demon_reply_queue, &queued_demon_reply->base);
  rvs_queue_release(demon_reply_queue);

  RVS_Engine *engine = 0;
  AssertAlways(rvs_engine_init(&engine) == RVS_Result_Ok);
  AssertAlways(rvs_demon_interrupt_capability(engine->demon) == RVS_DemonInterruptCapability_GlobalWithResume);
  AssertAlways(rvs_demon_send_message(engine->demon, (RVS_DemonMessage){
    .type = RVS_DemonMessage_InterruptExecution,
    .request_id = 1,
    .interrupt_execution = { .execution_request_id = 2, .command_id = 3 },
  }) == RVS_Result_Error);
  RVS_SchedulerOperationRule rule = {0};
  AssertAlways(rvs_scheduler_operation_rule(RVS_SchedulerOp_Run, &rule) &&
               rule.admission_requirement == RVS_AdmissionRequirement_StoppedUnfenced);
  AssertAlways(rvs_scheduler_operation_rule(RVS_SchedulerOp_Interrupt, &rule) &&
               rule.admission_requirement == RVS_AdmissionRequirement_ActiveOneLease);
  AssertAlways(rvs_scheduler_operation_rule(RVS_SchedulerOp_Terminate, &rule) &&
               rule.admission_requirement == RVS_AdmissionRequirement_LiveUnfenced);
  RVS_Session *session = 0;
  AssertAlways(rvs_engine_create_session(engine, &session) == RVS_Result_Ok);
  rvs_test_program_identity_bindings(engine, session);
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
  AssertAlways(rvs_request_control_cancel(launch_submit.control) == RVS_Result_StaleState);
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
  RVS_ThreadID matching_thread_id = { .u64 = { 0x102 } };
  DMN_Event *matching_thread_event = dmn_event_list_push(correlation_scratch.arena, &correlation_events);
  matching_thread_event->kind = DMN_EventKind_CreateThread;
  matching_thread_event->process = matching_pid_event->process;
  matching_thread_event->thread = rvs_thread_id_handle(matching_thread_id);
  engine->test_effect_sequence = 0;
  engine->test_first_emission_sequence = 0;
  engine->test_first_command_sequence = 0;
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_EventBatch,
    .request_id = correlated_launch->request_id,
    .event_batch = { .events = correlation_events, .command_id = rvs_test_launch_pump_token(engine, correlated_launch->request_id).command_id },
  });
  RVS_EngineReply correlated_launch_reply = {0};
  AssertAlways(rvs_request_wait(correlated_launch, max_U64, &correlated_launch_reply) == RVS_Result_Ok);
  AssertAlways(correlated_launch_reply.result == RVS_Result_Ok);
  AssertAlways(correlated_launch_reply.launch.pid == 100);
  mutex_take(session->control->mutex);
  RVS_Thread *matching_thread = rvs_entity_thread_from_id_locked(&session->entities, matching_thread_id);
  AssertAlways(matching_thread && !matching_thread->snapshot.is_retired &&
                rvs_program_id_match(matching_thread->snapshot.program, correlated_launch_reply.launch.program_id));
  mutex_drop(session->control->mutex);
  AssertAlways(engine->test_first_emission_sequence != 0 && engine->test_first_command_sequence != 0 &&
               engine->test_first_emission_sequence < engine->test_first_command_sequence);
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
  RVS_ThreadID post_exit_thread_id = { .u64 = { 0x203 } };
  DMN_Event *post_exit_thread_event = dmn_event_list_push(exit_scratch.arena, &exit_events);
  post_exit_thread_event->kind = DMN_EventKind_CreateThread;
  post_exit_thread_event->process = created_event->process;
  post_exit_thread_event->thread = rvs_thread_id_handle(post_exit_thread_id);
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_EventBatch,
    .request_id = exited_launch->request_id,
    .event_batch = { .events = exit_events, .command_id = rvs_test_launch_pump_token(engine, exited_launch->request_id).command_id },
  });
  RVS_EngineReply exited_launch_reply = {0};
  AssertAlways(rvs_request_wait(exited_launch, max_U64, &exited_launch_reply) == RVS_Result_Ok);
  AssertAlways(exited_launch_reply.result == RVS_Result_Error);
  mutex_take(session->control->mutex);
  AssertAlways(rvs_entity_thread_from_id_locked(&session->entities, post_exit_thread_id) == 0);
  mutex_drop(session->control->mutex);
  rvs_request_release(exited_launch);
  scratch_end(exit_scratch);

  RVS_Request *duplicate_start_launch = rvs_test_request_alloc(session, lifecycle_key, 0);
  AssertAlways(rvs_test_launch_started(engine, duplicate_start_launch->request_id, 400));
  AssertAlways( ! rvs_test_launch_started(engine, duplicate_start_launch->request_id, 400));
  Temp mismatched_scratch = scratch_begin(0, 0);
  DMN_EventList mismatched_events = {0};
  *dmn_event_list_push(mismatched_scratch.arena, &mismatched_events) = (DMN_Event){ .kind = DMN_EventKind_CreateProcess, .system_process_id = 401, .process = { .u64 = { 4 } } };
  RVS_SchedulerCommandToken mismatched_token = rvs_test_launch_pump_token(engine, duplicate_start_launch->request_id);
  RVS_SchedulerDecision malformed_batch_decision = {0};
  rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = {
      .source = RVS_SchedulerEventBatchSource_PumpLaunch,
      .command = mismatched_token,
      .request_id = duplicate_start_launch->request_id,
      .events = mismatched_events,
    },
  }, &malformed_batch_decision);
  AssertAlways(malformed_batch_decision.result == RVS_Result_InvalidArgument);
  rvs_scheduler_decision_release(&malformed_batch_decision);
  AssertAlways(rvs_test_launch_pump_token(engine, duplicate_start_launch->request_id).command_id == mismatched_token.command_id);
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
  AssertAlways(stale_pump_decision.result == RVS_Result_StaleState);
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
  rvs_test_execute_scheduler_decision(engine, &suppressed_decision);
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
  AssertAlways(!rvs_program_id_is_zero(duplicate_resolve_reply.launch.program_id));
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
  RVS_TargetControl *retired_program = rvs_scheduler_target_from_id_locked(&session->scheduler, duplicate_resolve_reply.launch.program_id);
  RVS_Program *retired_entity = rvs_entity_program_from_id_locked(&session->entities, duplicate_resolve_reply.launch.program_id);
  AssertAlways(retired_program && retired_program->state == RVS_TargetState_Removed && retired_program->is_termination_fenced);
  AssertAlways(retired_program->execution_owner == 0 && retired_program->execution_token == 0 && retired_entity->snapshot.exit_code == 123);
  mutex_drop(session->control->mutex);
  RVS_SubmitInfo retired_run_submit = {0};
  AssertAlways(rvs_session_run(session, duplicate_resolve_reply.launch.program_id, &retired_run_submit) == RVS_Result_StaleState);
  AssertAlways(retired_run_submit.request == 0 && retired_run_submit.control == 0);
  RVS_SubmitInfo retired_run_to_submit = {0};
  AssertAlways(rvs_session_run_to_address(session, duplicate_resolve_reply.launch.program_id, 0x1000,
                                          &retired_run_to_submit) == RVS_Result_StaleState);
  AssertAlways(retired_run_to_submit.request == 0 && retired_run_to_submit.control == 0);

  RVS_Request *pump_failure_launch = rvs_test_request_alloc(session, lifecycle_key, 0);
  ins_atomic_u32_eval_assign(&engine->test_fail_demon_message_type, RVS_DemonMessage_Pump);
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_LaunchStarted,
    .request_id = pump_failure_launch->request_id,
    .launch_started.pid = 300,
  });
  RVS_EngineReply pump_failure_reply = {0};
  AssertAlways(rvs_request_wait(pump_failure_launch, max_U64, &pump_failure_reply) == RVS_Result_Ok);
  AssertAlways(pump_failure_reply.result == RVS_Result_Error);
  rvs_request_release(pump_failure_launch);

  RVS_Request *malformed_outcome_launch = rvs_test_request_alloc(session, lifecycle_key, 0);
  AssertAlways(rvs_test_launch_started(engine, malformed_outcome_launch->request_id, 350));
  RVS_SchedulerCommandToken malformed_outcome_token = rvs_test_launch_pump_token(engine, malformed_outcome_launch->request_id);
  RVS_SchedulerDecision malformed_outcome_decision = {0};
  rvs_engine_apply_scheduler_event(engine, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_CommandOutcome,
    .command_outcome = {
      .command_kind = RVS_SchedulerCommand_PumpLaunch,
      .command = malformed_outcome_token,
      .result = RVS_Result_Pending,
    },
  }, &malformed_outcome_decision);
  AssertAlways(malformed_outcome_decision.emissions.first &&
               malformed_outcome_decision.emissions.first->reply.result == RVS_Result_Error);
  rvs_test_execute_scheduler_decision(engine, &malformed_outcome_decision);
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
  stale_operation->captured_program_state_epoch = rvs_scheduler_target_from_id_locked(&session->scheduler, read_only_key.target)->revision;
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
  AssertAlways(rvs_session_run(session, rvs_program_id_zero(), &invalid_run_submit) == RVS_Result_InvalidArgument);
  AssertAlways(invalid_run_submit.request == 0 && invalid_run_submit.control == 0);
  RVS_SubmitInfo invalid_run_to_submit = {0};
  AssertAlways(rvs_session_run_to_address(0, reply.launch.program_id, 0x1000, &invalid_run_to_submit) == RVS_Result_InvalidArgument);
  AssertAlways(rvs_session_run_to_address(session, rvs_program_id_zero(), 0x1000, &invalid_run_to_submit) == RVS_Result_InvalidArgument);
  AssertAlways(rvs_session_run_to_address(session, reply.launch.program_id, 0, &invalid_run_to_submit) == RVS_Result_InvalidArgument);
  AssertAlways(invalid_run_to_submit.request == 0 && invalid_run_to_submit.control == 0);

  RVS_ProgramID invalid_program_id = { .u64 = { max_U64 } };
  RVS_SubmitInfo invalid_program_submit = {0};
  AssertAlways(rvs_session_run(session, invalid_program_id, &invalid_program_submit) == RVS_Result_Error);
  AssertAlways(invalid_program_submit.request == 0 && invalid_program_submit.control == 0);
  AssertAlways(rvs_session_run_to_address(session, invalid_program_id, 0x1000, &invalid_run_to_submit) == RVS_Result_Error);
  AssertAlways(invalid_run_to_submit.request == 0 && invalid_run_to_submit.control == 0);
  RVS_ProgramID unknown_programs[] = { invalid_program_id };
  RVS_SubmitInfo unknown_programs_submit = {0};
  AssertAlways(rvs_session_run_many(session, unknown_programs, ArrayCount(unknown_programs), &unknown_programs_submit) == RVS_Result_Error);
  AssertAlways(unknown_programs_submit.request == 0 && unknown_programs_submit.control == 0);

  RVS_ProgramID run_programs[] = { second_launch_reply.launch.program_id, third_launch_reply.launch.program_id };
  mutex_take(session->control->mutex);
  RVS_TargetControl *reply_program = rvs_scheduler_target_from_id_locked(&session->scheduler, reply.launch.program_id);
  RVS_ProcessSnapshot reply_process_snapshot = {0};
  RVS_ProcessSnapshot first_run_process_snapshot = {0};
  RVS_ProcessSnapshot second_run_process_snapshot = {0};
  AssertAlways(rvs_entity_live_process_for_program_locked(&session->entities, reply_program->id, &reply_process_snapshot));
  AssertAlways(rvs_entity_live_process_for_program_locked(&session->entities, run_programs[0], &first_run_process_snapshot));
  AssertAlways(rvs_entity_live_process_for_program_locked(&session->entities, run_programs[1], &second_run_process_snapshot));
  DMN_Handle reply_process = rvs_process_id_handle(reply_process_snapshot.process);
  DMN_Handle run_processes[] = {
    rvs_process_id_handle(first_run_process_snapshot.process),
    rvs_process_id_handle(second_run_process_snapshot.process),
  };
  U64 first_program_epoch = rvs_scheduler_target_from_id_locked(&session->scheduler, run_programs[0])->revision;
  U64 second_program_epoch = rvs_scheduler_target_from_id_locked(&session->scheduler, run_programs[1])->revision;
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
  AssertAlways(rvs_scheduler_target_from_id_locked(&session->scheduler, run_programs[0])->revision > first_program_epoch);
  AssertAlways(rvs_scheduler_target_from_id_locked(&session->scheduler, run_programs[1])->revision > second_program_epoch);
  AssertAlways(rvs_scheduler_target_from_id_locked(&session->scheduler, run_programs[0])->execution_token > first_execution_token);
  AssertAlways(rvs_scheduler_target_from_id_locked(&session->scheduler, run_programs[1])->execution_token > second_execution_token);
  mutex_drop(session->control->mutex);

  RVS_ThreadID selected_thread = { .u64 = { 0x100 } };
  Temp thread_scratch = scratch_begin(0, 0);
  DMN_EventList thread_events = {0};
  *dmn_event_list_push(thread_scratch.arena, &thread_events) = (DMN_Event){
    .kind = DMN_EventKind_CreateThread,
    .process = run_processes[1],
    .thread = rvs_thread_id_handle(selected_thread),
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
  AssertAlways(rvs_program_id_match(selected_program, run_programs[1]) && rvs_thread_id_match(selected_thread_query, selected_thread));
  RVS_SubmitInfo unsupported_step = {0};
  AssertAlways(rvs_session_step(session, RVS_StepKind_Into, RVS_StepUnit_Line, selected_thread, &unsupported_step) == RVS_Result_Unsupported);
  AssertAlways(unsupported_step.request == 0 && unsupported_step.control == 0);
  AssertAlways(rvs_session_step(session, RVS_StepKind_Into, (RVS_StepUnit)3, selected_thread, &unsupported_step) == RVS_Result_InvalidArgument);
  Temp thread_exit_scratch = scratch_begin(0, 0);
  DMN_EventList thread_exit_events = {0};
  *dmn_event_list_push(thread_exit_scratch.arena, &thread_exit_events) = (DMN_Event){
    .kind = DMN_EventKind_ExitThread,
    .process = run_processes[1],
    .thread = rvs_thread_id_handle(selected_thread),
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
  RVS_ThreadID exception_selected_thread = { .u64 = { 0x101 } };
  mutex_take(session->control->mutex);
  rvs_entity_thread_create_locked(&session->entities, rvs_process_id_from_handle(run_processes[0]), exception_selected_thread, 0);
  session->scheduler.selected_target = run_programs[0];
  session->scheduler.selected_thread = exception_selected_thread;
  AssertAlways(rvs_scheduler_reserve_execution_locked(&session->scheduler, &exception_target, 1, exception_run_request_id));
  AssertAlways(rvs_scheduler_mark_run_in_flight_locked(&session->scheduler, exception_run_request_id));
  session->scheduler.active_execution_request_id = exception_run_request_id;
  session->scheduler.phase = RVS_SchedulerPhase_Running;
  session->scheduler.run_intent = (RVS_RunIntent){
    .kind = RVS_RunIntentKind_Continue,
    .state = RVS_RunIntentState_Active,
  };
  Temp exception_scratch = scratch_begin(0, 0);
  DMN_EventList exception_events = {0};
  *dmn_event_list_push(exception_scratch.arena, &exception_events) = (DMN_Event){ .kind = DMN_EventKind_Exception, .process = run_processes[0] };
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
  AssertAlways(exception_decision.emissions.first && exception_decision.emissions.first->kind == RVS_SchedulerEmission_PublishEvent &&
               exception_decision.emissions.first->next == 0 && exception_decision.command.kind == RVS_SchedulerCommand_Null);
  AssertAlways(exception_decision.emissions.first->event.kind == RVS_EventKind_Stopped);
  AssertAlways(rvs_program_id_match(exception_decision.emissions.first->event.stopped.primary_program, run_programs[0]));
  AssertAlways(rvs_thread_id_match(exception_decision.emissions.first->event.stopped.selected_thread, exception_selected_thread));
  AssertAlways(exception_decision.emissions.first->event.stopped.primary_cause == RVS_StopCause_Exception);
  AssertAlways(exception_decision.emissions.first->event.stopped.stable_stop_generation != 0);
  AssertAlways(session->scheduler.phase == RVS_SchedulerPhase_Stopped);
  AssertAlways(session->scheduler.run_intent.state == RVS_RunIntentState_Cancelled);
  AssertAlways(session->scheduler.stop_transaction.execution_request_id == exception_run_request_id);
  AssertAlways(session->scheduler.stop_transaction.primary_cause == RVS_StopCause_Exception);
  AssertAlways(exception_dispositions[0] == RVS_SchedulerEventDisposition_Suppress);
  rvs_scheduler_decision_release(&exception_decision);
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
  session->scheduler.run_intent.state = RVS_RunIntentState_Cancelled;
  AssertAlways(rvs_scheduler_finish_run_locked(&session->scheduler, exception_run_request_id));
  session->scheduler.active_execution_request_id = 0;
  rvs_scheduler_decision_release(&exception_decision);
  scratch_end(exception_scratch);
  mutex_drop(session->control->mutex);

  RVS_SubmitInfo zero_program_run_submit = {0};
  AssertAlways(rvs_session_run(session, rvs_program_id_zero(), &zero_program_run_submit) == RVS_Result_InvalidArgument);
  AssertAlways(zero_program_run_submit.request == 0 && zero_program_run_submit.control == 0);
  RVS_ProgramID duplicate_programs[] = { reply.launch.program_id, reply.launch.program_id };
  RVS_SubmitInfo duplicate_programs_submit = {0};
  AssertAlways(rvs_session_run_many(session, duplicate_programs, ArrayCount(duplicate_programs), &duplicate_programs_submit) == RVS_Result_InvalidArgument);
  AssertAlways(duplicate_programs_submit.request == 0 && duplicate_programs_submit.control == 0);
  RVS_SubmitInfo null_program_pointer_submit = {0};
  AssertAlways(rvs_session_run_many(session, 0, 1, &null_program_pointer_submit) == RVS_Result_InvalidArgument);
  AssertAlways(null_program_pointer_submit.request == 0 && null_program_pointer_submit.control == 0);
  RVS_ProgramID null_program = {0};
  RVS_SubmitInfo null_program_submit = {0};
  AssertAlways(rvs_session_run_many(session, &null_program, 1, &null_program_submit) == RVS_Result_InvalidArgument);
  AssertAlways(null_program_submit.request == 0 && null_program_submit.control == 0);
  RVS_SubmitInfo malformed_run_to_submit = {0};
  AssertAlways(rvs_session_run_to_address(session, reply.launch.program_id, 0, &malformed_run_to_submit) == RVS_Result_InvalidArgument);
  AssertAlways(malformed_run_to_submit.request == 0 && malformed_run_to_submit.control == 0);
  rvs_test_wait_until_execution_state(session, reply.launch.program_id, RVS_TargetExecutionState_Idle);

  // These failure paths cannot occur during normal engine operation.
  ins_atomic_u32_eval_assign(&engine->test_fail_command_enqueue, 1);
  RVS_SubmitInfo enqueue_failure_submit = {0};
  AssertAlways(rvs_session_run(session, reply.launch.program_id, &enqueue_failure_submit) == RVS_Result_Error);
  AssertAlways(enqueue_failure_submit.request == 0 && enqueue_failure_submit.control == 0);
  rvs_test_wait_until_execution_state(session, reply.launch.program_id, RVS_TargetExecutionState_Idle);
  mutex_take(session->control->mutex);
  U64 plan_count_before_failed_run_to = 0;
  for EachNode(plan, RVS_Plan, session->scheduler.plan_first) { plan_count_before_failed_run_to += 1; }
  mutex_drop(session->control->mutex);
  ins_atomic_u32_eval_assign(&engine->test_fail_command_enqueue, 1);
  RVS_SubmitInfo run_to_enqueue_failure_submit = {0};
  AssertAlways(rvs_session_run_to_address(session, reply.launch.program_id, 0x1000,
                                          &run_to_enqueue_failure_submit) == RVS_Result_Error);
  AssertAlways(run_to_enqueue_failure_submit.request == 0 && run_to_enqueue_failure_submit.control == 0);
  mutex_take(session->control->mutex);
  U64 plan_count_after_failed_run_to = 0;
  for EachNode(plan, RVS_Plan, session->scheduler.plan_first) { plan_count_after_failed_run_to += 1; }
  AssertAlways(plan_count_after_failed_run_to == plan_count_before_failed_run_to);
  mutex_drop(session->control->mutex);
  rvs_test_wait_until_execution_state(session, reply.launch.program_id, RVS_TargetExecutionState_Idle);

  ins_atomic_u32_eval_assign(&engine->test_hold_before_dispatch, 1);
  RVS_SubmitInfo cancelled_run_submit = {0};
  AssertAlways(rvs_session_run_to_address(session, reply.launch.program_id, 0x2345, &cancelled_run_submit) == RVS_Result_Ok);
  rvs_test_wait_until_dispatch_held(engine);
  rvs_test_wait_until_execution_state(session, reply.launch.program_id, RVS_TargetExecutionState_Queued);
  mutex_take(session->control->mutex);
  RVS_ScheduledOperation *cancelled_run_operation = rvs_scheduler_operation_from_request_id_locked(
    &session->scheduler, cancelled_run_submit.control->request_id);
  RVS_Plan *cancelled_run_to_plan = rvs_scheduler_plan_from_id_locked(&session->scheduler,
                                                                      cancelled_run_operation->active_plan_id);
  AssertAlways(cancelled_run_to_plan && cancelled_run_to_plan->header.kind == RVS_PlanKind_RunToAddress &&
               cancelled_run_to_plan->run_to_address.vaddr == 0x2345 &&
               rvs_program_id_match(cancelled_run_to_plan->run_to_address.target, reply.launch.program_id));
  RVS_PlanID cancelled_run_to_plan_id = cancelled_run_to_plan->header.id;
  mutex_drop(session->control->mutex);
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
  mutex_take(session->control->mutex);
  AssertAlways(rvs_scheduler_plan_from_id_locked(&session->scheduler, cancelled_run_to_plan_id) == 0);
  mutex_drop(session->control->mutex);
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

  RVS_SubmitInfo invalid_address_submit = {0};
  AssertAlways(rvs_session_run_to_address(session, reply.launch.program_id, max_U64 - 1,
                                          &invalid_address_submit) == RVS_Result_Ok);
  RVS_EngineReply invalid_address_reply = {0};
  AssertAlways(rvs_request_wait(invalid_address_submit.request, max_U64, &invalid_address_reply) == RVS_Result_Ok);
  AssertAlways(invalid_address_reply.kind == RVS_EngineReplyKind_Run && invalid_address_reply.result == RVS_Result_Error);
  rvs_request_release(invalid_address_submit.request);
  rvs_request_control_release(invalid_address_submit.control);
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
  ins_atomic_u32_eval_assign(&engine->test_fail_demon_message_type, RVS_DemonMessage_InterruptExecution);
  RVS_SubmitInfo interrupt_send_failure_submit = {0};
  AssertAlways(rvs_session_interrupt(session, reply.launch.program_id, &interrupt_send_failure_submit) == RVS_Result_Ok);
  RVS_EngineReply interrupt_send_failure_reply = {0};
  AssertAlways(rvs_request_wait(interrupt_send_failure_submit.request, max_U64, &interrupt_send_failure_reply) == RVS_Result_Ok);
  AssertAlways(interrupt_send_failure_reply.result == RVS_Result_Error);
  mutex_take(session->control->mutex);
  AssertAlways(session->scheduler.active_execution_request_id == test_run_request_id);
  AssertAlways(session->scheduler.stop_transaction.owner == 0);
  AssertAlways(rvs_scheduler_target_from_id_locked(&session->scheduler, reply.launch.program_id)->state == RVS_TargetExecutionState_RunInFlight);
  mutex_drop(session->control->mutex);
  rvs_request_release(interrupt_send_failure_submit.request);
  rvs_request_control_release(interrupt_send_failure_submit.control);
  RVS_SchedulerKey interrupt_key = {
    .op = RVS_SchedulerOp_Interrupt,
    .identity = 1,
  };
  mutex_take(session->control->mutex);
  session->scheduler.phase = RVS_SchedulerPhase_Running;
  RVS_SchedulerAdmission interrupt_admission = {0};
  AssertAlways(rvs_scheduler_admit_locked(&session->scheduler, session->engine->request_pool, &session->engine->next_request_id, interrupt_key, &reply.launch.program_id, 1, 0, &interrupt_admission) == RVS_Result_Ok);
  AssertAlways(session->scheduler.stop_transaction.owner == 0);
  RVS_SchedulerEventDisposition predispatch_stop_disposition = RVS_SchedulerEventDisposition_Forward;
  RVS_SchedulerDecision predispatch_stop_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = {
      .request_id = test_run_request_id,
       .events = { .first = &(DMN_EventNode){ .v = { .kind = DMN_EventKind_Halt, .process = reply_process } } },
      .dispositions = &predispatch_stop_disposition,
      .dispositions_count = 1,
    },
  }, &predispatch_stop_decision);
  AssertAlways(session->scheduler.stop_transaction.owner == 0);
  AssertAlways(predispatch_stop_decision.command.kind == RVS_SchedulerCommand_Null);
  rvs_scheduler_decision_release(&predispatch_stop_decision);
  MemoryZeroStruct(&session->scheduler.stop_transaction);
  session->scheduler.phase = RVS_SchedulerPhase_Running;
  session->scheduler.run_intent.state = RVS_RunIntentState_Active;
  U64 dispatched_generation_before_interrupt = session->scheduler.last_dispatched_stable_stop_generation;
  rvs_test_activate_interrupt_locked(session, &interrupt_admission);
  RVS_SchedulerCommandToken interrupt_stop_token = interrupt_admission.operation->pending_command.token;
  AssertAlways(session->scheduler.stop_transaction.execution_request_id == test_run_request_id);
  AssertAlways(session->scheduler.stop_transaction.phase == RVS_ControlTransactionPhase_WaitingForInterrupt);
  AssertAlways(session->scheduler.active_execution_request_id == test_run_request_id);
  AssertAlways(interrupt_admission.operation->targets[0].execution_token == rvs_scheduler_target_from_id_locked(&session->scheduler, reply.launch.program_id)->execution_token);
  AssertAlways(rvs_scheduler_target_from_id_locked(&session->scheduler, reply.launch.program_id)->state == RVS_TargetExecutionState_InterruptPending);
  Temp stop_scratch = scratch_begin(0, 0);
  DMN_EventList stop_events = {0};
  *dmn_event_list_push(stop_scratch.arena, &stop_events) = (DMN_Event){ .kind = DMN_EventKind_Halt, .process = reply_process };
  RVS_SchedulerDecision stop_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = { .request_id = test_run_request_id, .events = stop_events, .dispositions = (RVS_SchedulerEventDisposition[1]){0}, .dispositions_count = 1 },
  }, &stop_decision);
  AssertAlways(stop_decision.command.kind == RVS_SchedulerCommand_Null);
  AssertAlways(session->scheduler.stop_transaction.phase == RVS_ControlTransactionPhase_CollectingBatch);
  AssertAlways(session->scheduler.last_dispatched_stable_stop_generation == dispatched_generation_before_interrupt);
  rvs_scheduler_decision_release(&stop_decision);
  RVS_SchedulerDecision stale_stop_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_CommandOutcome,
    .command_outcome = {
      .command_kind = RVS_SchedulerCommand_InterruptExecution,
      .command = { .request_id = interrupt_stop_token.request_id, .command_id = interrupt_stop_token.command_id + 1 },
      .result = RVS_Result_Ok,
    },
  }, &stale_stop_decision);
  AssertAlways(stale_stop_decision.result == RVS_Result_StaleState);
  rvs_scheduler_decision_release(&stale_stop_decision);
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_CommandOutcome,
    .command_outcome = {
      .command_kind = RVS_SchedulerCommand_InterruptExecution,
      .command = interrupt_stop_token,
      .result = RVS_Result_Ok,
    },
  }, &stop_decision);
  AssertAlways(stop_decision.command.kind == RVS_SchedulerCommand_ResumeTargetSubset);
  AssertAlways(stop_decision.command.resume_target_subset.targets_count == 0);
  AssertAlways(stop_decision.command.token.command_id != 0);
  AssertAlways(session->scheduler.stop_transaction.stable_stop_generation != 0);
  AssertAlways(session->scheduler.last_dispatched_stable_stop_generation ==
               session->scheduler.stop_transaction.stable_stop_generation);
  AssertAlways(session->scheduler.last_plan_stop_disposition == RVS_StableStopDisposition_Unhandled &&
               session->scheduler.last_handler_stop_disposition == RVS_StableStopDisposition_Unhandled &&
               session->scheduler.last_stop_policy_source == RVS_StopPolicySource_Fallback &&
               session->scheduler.last_stable_stop_used_fallback &&
               session->scheduler.last_stable_stop_command == RVS_SchedulerCommand_ResumeTargetSubset);
  AssertAlways(interrupt_admission.operation->targets[0].stable_stop_generation ==
               session->scheduler.stop_transaction.stable_stop_generation);
  AssertAlways(session->scheduler.stop_transaction.phase == RVS_ControlTransactionPhase_WaitingForResume);
  rvs_scheduler_decision_release(&stop_decision);
  scratch_end(stop_scratch);
  MemoryZeroStruct(&interrupt_admission.operation->pending_command);
  AssertAlways(rvs_scheduler_finish_stop_resume_locked(&session->scheduler, interrupt_admission.request->request_id) == RVS_Result_Ok);
  AssertAlways(session->scheduler.active_execution_request_id == 0);
  AssertAlways(session->scheduler.run_intent.state == RVS_RunIntentState_Suspended);
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

  // A selected exit preempts the interrupt workflow even when the batch request ID is stale.
  mutex_take(session->control->mutex);
  AssertAlways(rvs_scheduler_reserve_execution_locked(&session->scheduler, &test_target, 1, test_run_request_id));
  AssertAlways(rvs_scheduler_mark_run_in_flight_locked(&session->scheduler, test_run_request_id));
  session->scheduler.phase = RVS_SchedulerPhase_Running;
  session->scheduler.run_intent = (RVS_RunIntent){
    .kind = RVS_RunIntentKind_Continue,
    .state = RVS_RunIntentState_Active,
  };
  RVS_SchedulerAdmission exited_interrupt_admission = {0};
  AssertAlways(rvs_scheduler_admit_locked(&session->scheduler, session->engine->request_pool, &session->engine->next_request_id, interrupt_key, &reply.launch.program_id, 1, 0, &exited_interrupt_admission) == RVS_Result_Ok);
  rvs_test_activate_interrupt_locked(session, &exited_interrupt_admission);
  RVS_SchedulerCommandToken exited_interrupt_stop_token = exited_interrupt_admission.operation->pending_command.token;
  AssertAlways(session->scheduler.stop_transaction.execution_request_id == test_run_request_id);
  Temp selected_exit_scratch = scratch_begin(0, 0);
  DMN_EventList selected_exit_events = {0};
  *dmn_event_list_push(selected_exit_scratch.arena, &selected_exit_events) = (DMN_Event){ .kind = DMN_EventKind_ExitProcess, .process = reply_process };
  RVS_SchedulerDecision selected_exit_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = { .request_id = test_run_request_id + 1, .events = selected_exit_events, .dispositions = (RVS_SchedulerEventDisposition[1]){0}, .dispositions_count = 1 },
  }, &selected_exit_decision);
  AssertAlways(selected_exit_decision.command.kind == RVS_SchedulerCommand_Null);
  rvs_scheduler_decision_release(&selected_exit_decision);
  scratch_end(selected_exit_scratch);
  RVS_Request *exited_interrupt_request = exited_interrupt_admission.request;
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_CommandOutcome,
    .command_outcome = {
      .command_kind = RVS_SchedulerCommand_InterruptExecution,
      .command = exited_interrupt_stop_token,
      .result = RVS_Result_Ok,
    },
  }, &selected_exit_decision);
  AssertAlways(selected_exit_decision.command.kind == RVS_SchedulerCommand_Null);
  AssertAlways(selected_exit_decision.emissions.first &&
               selected_exit_decision.emissions.first->kind == RVS_SchedulerEmission_PublishEvent &&
               selected_exit_decision.emissions.first->event.kind == RVS_EventKind_Stopped &&
               selected_exit_decision.emissions.first->event.stopped.primary_cause == RVS_StopCause_ProcessExit &&
               selected_exit_decision.emissions.first->next &&
               selected_exit_decision.emissions.first->next->reply.result == RVS_Result_StaleState);
  rvs_scheduler_decision_release(&selected_exit_decision);
  AssertAlways(session->scheduler.run_intent.state == RVS_RunIntentState_Completed);
  AssertAlways(session->scheduler.stop_transaction.owner == 0);
  rvs_request_release(exited_interrupt_request); // drop unreturned caller ownership
  mutex_drop(session->control->mutex);

  RVS_SchedulerKey terminate_key = {
    .op = RVS_SchedulerOp_Terminate,
    .identity = 1,
  };
  mutex_take(session->control->mutex);
  RVS_ProgramID terminate_target = second_launch_reply.launch.program_id;
  RVS_TargetControl *terminate_entry = rvs_scheduler_target_from_id_locked(&session->scheduler, terminate_target);
  U64 revision = terminate_entry->revision;
  RVS_SchedulerAdmission terminate_admission = {0};
  AssertAlways(rvs_scheduler_admit_locked(&session->scheduler, session->engine->request_pool, &session->engine->next_request_id, terminate_key, &terminate_target, 1, 0, &terminate_admission) == RVS_Result_Ok);
  AssertAlways(terminate_entry->is_termination_fenced);
  AssertAlways(terminate_entry->revision > revision);
  RVS_SchedulerAdmission fenced_query = {0};
  AssertAlways(rvs_scheduler_admit_locked(&session->scheduler, session->engine->request_pool, &session->engine->next_request_id, (RVS_SchedulerKey){ .op = RVS_SchedulerOp_ReadOnly, .target = terminate_target, .identity = 2 }, 0, 0, 0, &fenced_query) == RVS_Result_Ok);
  AssertAlways(fenced_query.kind == RVS_SchedulerAdmissionKind_Terminal && fenced_query.request->reply.result == RVS_Result_StaleState);
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
  session->scheduler.phase = RVS_SchedulerPhase_Running;
  session->scheduler.run_intent = (RVS_RunIntent){
    .kind = RVS_RunIntentKind_Continue,
    .state = RVS_RunIntentState_Active,
  };
  Temp stale_pump_scratch = scratch_begin(0, 0);
  DMN_EventList stale_pump_events = {0};
  *dmn_event_list_push(stale_pump_scratch.arena, &stale_pump_events) = (DMN_Event){
    .kind = DMN_EventKind_Halt,
    .process = run_processes[0],
  };
  RVS_SchedulerEventDisposition stale_pump_disposition = RVS_SchedulerEventDisposition_Forward;
  RVS_SchedulerDecision stale_active_pump_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = {
      .source = RVS_SchedulerEventBatchSource_PumpLaunch,
      .command = { .request_id = reducer_run_request_id, .command_id = 1 },
      .request_id = reducer_run_request_id,
      .events = stale_pump_events,
      .dispositions = &stale_pump_disposition,
      .dispositions_count = 1,
    },
  }, &stale_active_pump_decision);
  AssertAlways(stale_active_pump_decision.result == RVS_Result_StaleState);
  AssertAlways(stale_pump_disposition == RVS_SchedulerEventDisposition_Suppress);
  AssertAlways(session->scheduler.phase == RVS_SchedulerPhase_Running);
  AssertAlways(session->scheduler.stop_transaction.owner == 0);
  rvs_scheduler_decision_release(&stale_active_pump_decision);
  RVS_SchedulerEventDisposition stale_execution_disposition = RVS_SchedulerEventDisposition_Forward;
  RVS_SchedulerDecision stale_execution_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = {
      .request_id = reducer_run_request_id + 1,
      .events = stale_pump_events,
      .dispositions = &stale_execution_disposition,
      .dispositions_count = 1,
    },
  }, &stale_execution_decision);
  AssertAlways(session->scheduler.phase == RVS_SchedulerPhase_Running);
  AssertAlways(session->scheduler.stop_transaction.owner == 0);
  rvs_scheduler_decision_release(&stale_execution_decision);
  scratch_end(stale_pump_scratch);
  RVS_SchedulerAdmission reducer_interrupt_admission = {0};
  AssertAlways(rvs_scheduler_admit_locked(&session->scheduler, session->engine->request_pool,
                                           &session->engine->next_request_id, interrupt_key, &run_programs[0], 1, 0,
                                          &reducer_interrupt_admission) == RVS_Result_Ok);
  rvs_test_activate_interrupt_locked(session, &reducer_interrupt_admission);
  AssertAlways(session->scheduler.stop_transaction.execution_request_id == reducer_run_request_id);
  RVS_ScheduledOperation *reducer_interrupt_operation = reducer_interrupt_admission.operation;
  RVS_Request *reducer_interrupt_request = reducer_interrupt_operation->request;
  RVS_SchedulerCommandToken reducer_interrupt_token = reducer_interrupt_operation->pending_command.token;
  RVS_SchedulerEventDisposition processless_halt_disposition = RVS_SchedulerEventDisposition_Forward;
  RVS_SchedulerDecision processless_halt_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = {
      .request_id = reducer_run_request_id,
      .events = { .first = &(DMN_EventNode){ .v = { .kind = DMN_EventKind_Halt } } },
      .dispositions = &processless_halt_disposition,
      .dispositions_count = 1,
    },
  }, &processless_halt_decision);
  AssertAlways(processless_halt_decision.command.kind == RVS_SchedulerCommand_Null);
  AssertAlways(session->scheduler.stop_transaction.phase == RVS_ControlTransactionPhase_CollectingBatch);
  AssertAlways(processless_halt_disposition == RVS_SchedulerEventDisposition_Suppress);
  rvs_scheduler_decision_release(&processless_halt_decision);

  RVS_ReducerEventTest reducer_event_tests[] = {
    { RVS_SchedulerEvent_DemonEventBatch, reducer_run_request_id + 1, 0, RVS_SchedulerEmission_PublishEvent, RVS_SchedulerCommand_Null,               RVS_Result_Ok,         0 },
    { RVS_SchedulerEvent_DemonEventBatch, reducer_run_request_id,     0, RVS_SchedulerEmission_Null, RVS_SchedulerCommand_Null,               RVS_Result_Ok,         0 },
    { RVS_SchedulerEvent_DemonEventBatch, reducer_run_request_id,     0, RVS_SchedulerEmission_Null, RVS_SchedulerCommand_Null,               RVS_Result_Ok,         0 },
    { RVS_SchedulerEvent_DemonEventBatch, reducer_run_request_id,     1, RVS_SchedulerEmission_Null, RVS_SchedulerCommand_Null,               RVS_Result_Ok,         0 },
    { RVS_SchedulerEvent_CommandOutcome,  reducer_interrupt_request->request_id, 1, RVS_SchedulerEmission_Null, RVS_SchedulerCommand_Null, RVS_Result_StaleState, 0 },
    { RVS_SchedulerEvent_CommandOutcome,  reducer_interrupt_request->request_id, 0, RVS_SchedulerEmission_PublishEvent, RVS_SchedulerCommand_ResumeTargetSubset, RVS_Result_Ok, 1 },
    { RVS_SchedulerEvent_CommandOutcome,  reducer_interrupt_request->request_id, 0, RVS_SchedulerEmission_Null, RVS_SchedulerCommand_Null, RVS_Result_StaleState, 0 },
    { RVS_SchedulerEvent_CommandOutcome,  reducer_interrupt_request->request_id, 1, RVS_SchedulerEmission_Null, RVS_SchedulerCommand_Null, RVS_Result_StaleState, 0 },
    { RVS_SchedulerEvent_CommandOutcome,  reducer_interrupt_request->request_id, 0, RVS_SchedulerEmission_CompleteRequest, RVS_SchedulerCommand_Null, RVS_Result_Ok, 0 },
    { RVS_SchedulerEvent_CommandOutcome,  reducer_interrupt_request->request_id, 0, RVS_SchedulerEmission_Null, RVS_SchedulerCommand_Null, RVS_Result_StaleState, 0 },
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
      *dmn_event_list_push(event_scratch.arena, &events) = (DMN_Event){ .kind = DMN_EventKind_Halt, .process = run_processes[test->target_index] };
      event.demon_events.request_id = test->source_request_id;
      event.demon_events.events = events;
      event.demon_events.dispositions = dispositions;
      event.demon_events.dispositions_count = 1;
    } else if (test->kind == RVS_SchedulerEvent_CommandOutcome) {
      B32 is_interrupt_completion = test_idx <= 6;
      event.command_outcome.command_kind = is_interrupt_completion ? RVS_SchedulerCommand_InterruptExecution :
                                                                     RVS_SchedulerCommand_ResumeTargetSubset;
      event.command_outcome.command = is_interrupt_completion ? reducer_interrupt_token : reducer_resume_token;
      event.command_outcome.command.command_id += test->target_index;
      event.command_outcome.result = RVS_Result_Ok;
    }
    rvs_scheduler_apply_locked(&session->scheduler, event, &decision);
    AssertAlways((decision.emissions.first ? decision.emissions.first->kind : RVS_SchedulerEmission_Null) == test->expected_emission);
    AssertAlways(decision.command.kind == test->expected_command && decision.result == test->expected_result);
    if (decision.command.kind == RVS_SchedulerCommand_ResumeTargetSubset) {
      AssertAlways(decision.command.resume_target_subset.targets_count == test->expected_targets_count);
      reducer_resume_token = decision.command.token;
      AssertAlways(decision.emissions.first->event.kind == RVS_EventKind_Stopped &&
                   decision.emissions.first->event.stopped.primary_cause == RVS_StopCause_UserInterrupt);
    }
    if (test_idx == 0) {
      AssertAlways(session->scheduler.phase == RVS_SchedulerPhase_Running);
      for EachIndex(target_idx, reducer_interrupt_operation->targets_count) {
        AssertAlways( ! reducer_interrupt_operation->targets[target_idx].stop_observed);
      }
    } else if (test_idx == 5 || test_idx == 6) {
      AssertAlways(session->scheduler.stop_transaction.stable_stop_generation != 0);
      for EachIndex(target_idx, reducer_interrupt_operation->targets_count) {
        RVS_TargetSnapshot *target = &reducer_interrupt_operation->targets[target_idx];
        RVS_TargetControl *entry = rvs_scheduler_target_from_id_locked(&session->scheduler, target->target);
        AssertAlways(target->stable_stop_generation == session->scheduler.stop_transaction.stable_stop_generation);
        AssertAlways(entry && entry->state == RVS_TargetExecutionState_InterruptPending);
      }
    }
    rvs_scheduler_decision_release(&decision);
    scratch_end(event_scratch);
  }
  AssertAlways(rvs_scheduler_finish_run_locked(&session->scheduler, reducer_run_request_id));
  rvs_request_release(reducer_interrupt_request); // drop the unreturned submit ownership

  RVS_MessageID failed_resume_run_request_id = 0x2800;
  AssertAlways(rvs_scheduler_reserve_execution_locked(&session->scheduler, reducer_run_targets,
                                                      ArrayCount(reducer_run_targets), failed_resume_run_request_id));
  AssertAlways(rvs_scheduler_mark_run_in_flight_locked(&session->scheduler, failed_resume_run_request_id));
  RVS_SchedulerAdmission failed_resume_admission = {0};
  AssertAlways(rvs_scheduler_admit_locked(&session->scheduler, session->engine->request_pool,
                                           &session->engine->next_request_id, interrupt_key, &run_programs[0], 1, 0,
                                          &failed_resume_admission) == RVS_Result_Ok);
  rvs_test_activate_interrupt_locked(session, &failed_resume_admission);
  RVS_SchedulerCommandToken failed_interrupt_stop_token = failed_resume_admission.operation->pending_command.token;
  Temp failed_resume_scratch = scratch_begin(0, 0);
  DMN_EventList failed_resume_events = {0};
  *dmn_event_list_push(failed_resume_scratch.arena, &failed_resume_events) = (DMN_Event){ .kind = DMN_EventKind_Halt, .process = run_processes[0] };
  *dmn_event_list_push(failed_resume_scratch.arena, &failed_resume_events) = (DMN_Event){ .kind = DMN_EventKind_Halt, .process = run_processes[1] };
  RVS_SchedulerEventDisposition failed_resume_dispositions[2] = {0};
  RVS_SchedulerDecision failed_resume_stop_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = {
      .request_id = failed_resume_run_request_id,
      .events = failed_resume_events,
      .dispositions = failed_resume_dispositions,
      .dispositions_count = ArrayCount(failed_resume_dispositions),
    },
  }, &failed_resume_stop_decision);
  AssertAlways(failed_resume_stop_decision.command.kind == RVS_SchedulerCommand_Null);
  rvs_scheduler_decision_release(&failed_resume_stop_decision);
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_CommandOutcome,
    .command_outcome = {
      .command_kind = RVS_SchedulerCommand_InterruptExecution,
      .command = failed_interrupt_stop_token,
      .result = RVS_Result_Ok,
    },
  }, &failed_resume_stop_decision);
  AssertAlways(failed_resume_stop_decision.command.kind == RVS_SchedulerCommand_ResumeTargetSubset);
  RVS_SchedulerCommandToken failed_resume_token = failed_resume_stop_decision.command.token;
  rvs_scheduler_decision_release(&failed_resume_stop_decision);
  scratch_end(failed_resume_scratch);
  RVS_Request *failed_resume_request = failed_resume_admission.request;
  RVS_SchedulerDecision failed_resume_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_CommandOutcome,
    .command_outcome = {
      .command_kind = RVS_SchedulerCommand_ResumeTargetSubset,
      .command = failed_resume_token,
      .result = RVS_Result_Error,
    },
  }, &failed_resume_decision);
  AssertAlways(failed_resume_decision.result == RVS_Result_Ok);
  AssertAlways(session->scheduler.active_execution_request_id == 0);
  AssertAlways(session->scheduler.stop_transaction.owner == 0);
  for EachIndex(target_idx, ArrayCount(run_programs)) {
    RVS_TargetControl *entry = rvs_scheduler_target_from_id_locked(&session->scheduler, run_programs[target_idx]);
    AssertAlways(entry->state == RVS_TargetExecutionState_Idle && entry->execution_owner == 0);
  }
  rvs_scheduler_decision_release(&failed_resume_decision);
  rvs_request_release(failed_resume_request);

  // A valid resume acknowledgment with locally inconsistent target state fails stopped and clears the transaction.
  RVS_ProgramID invalid_resume_targets[] = {
    { .u64 = { 0x2901 } },
    { .u64 = { 0x2902 } },
  };
  for EachIndex(target_idx, ArrayCount(invalid_resume_targets)) {
    rvs_scheduler_target_add_locked(&session->scheduler, invalid_resume_targets[target_idx]);
  }
  RVS_MessageID invalid_resume_run_request_id = 0x2900;
  RVS_TargetSnapshot invalid_resume_run_targets[] = {
    { .target = invalid_resume_targets[0] },
    { .target = invalid_resume_targets[1] },
  };
  AssertAlways(rvs_scheduler_reserve_execution_locked(&session->scheduler, invalid_resume_run_targets,
                                                       ArrayCount(invalid_resume_run_targets), invalid_resume_run_request_id));
  AssertAlways(rvs_scheduler_mark_run_in_flight_locked(&session->scheduler, invalid_resume_run_request_id));
  session->scheduler.active_execution_request_id = invalid_resume_run_request_id;
  session->scheduler.phase = RVS_SchedulerPhase_Running;
  session->scheduler.run_intent = (RVS_RunIntent){
    .kind = RVS_RunIntentKind_Continue,
    .state = RVS_RunIntentState_Active,
  };
  RVS_SchedulerAdmission invalid_resume_admission = {0};
  AssertAlways(rvs_scheduler_admit_locked(&session->scheduler, session->engine->request_pool,
                                           &session->engine->next_request_id, interrupt_key, &invalid_resume_targets[0], 1, 0,
                                          &invalid_resume_admission) == RVS_Result_Ok);
  rvs_test_activate_interrupt_locked(session, &invalid_resume_admission);
  RVS_SchedulerCommandToken invalid_interrupt_token = invalid_resume_admission.operation->pending_command.token;
  RVS_SchedulerDecision invalid_stop_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_CommandOutcome,
    .command_outcome = {
      .command_kind = RVS_SchedulerCommand_InterruptExecution,
      .command = invalid_interrupt_token,
      .result = RVS_Result_Ok,
    },
  }, &invalid_stop_decision);
  AssertAlways(invalid_stop_decision.command.kind == RVS_SchedulerCommand_ResumeTargetSubset);
  RVS_SchedulerCommandToken invalid_resume_token = invalid_stop_decision.command.token;
  rvs_scheduler_decision_release(&invalid_stop_decision);
  RVS_TargetControl *invalid_unselected_entry = rvs_scheduler_target_from_id_locked(&session->scheduler,
                                                                                          invalid_resume_targets[1]);
  AssertAlways(rvs_scheduler_commit_target_locked(invalid_unselected_entry, 0, RVS_TargetState_Removed,
                                                   RVS_TargetTransition_Retired));
  invalid_unselected_entry->execution_owner = 0;
  RVS_Request *invalid_resume_request = invalid_resume_admission.request;
  RVS_SchedulerDecision invalid_resume_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_CommandOutcome,
    .command_outcome = {
      .command_kind = RVS_SchedulerCommand_ResumeTargetSubset,
      .command = invalid_resume_token,
      .result = RVS_Result_Ok,
    },
  }, &invalid_resume_decision);
  AssertAlways(invalid_resume_decision.result == RVS_Result_Ok);
  AssertAlways(invalid_resume_decision.emissions.first && invalid_resume_decision.emissions.first->reply.result == RVS_Result_Error);
  AssertAlways(session->scheduler.stop_transaction.owner == 0);
  AssertAlways(session->scheduler.active_execution_request_id == 0);
  AssertAlways(rvs_scheduler_target_from_id_locked(&session->scheduler, invalid_resume_targets[0])->state ==
               RVS_TargetExecutionState_Idle);
  rvs_scheduler_decision_release(&invalid_resume_decision);
  rvs_request_release(invalid_resume_request);

  RVS_MessageID reducer_exit_run_request_id = reducer_run_request_id + 1;
  AssertAlways(rvs_scheduler_reserve_execution_locked(&session->scheduler, reducer_run_targets, ArrayCount(reducer_run_targets), reducer_exit_run_request_id));
  AssertAlways(rvs_scheduler_mark_run_in_flight_locked(&session->scheduler, reducer_exit_run_request_id));
  session->scheduler.phase = RVS_SchedulerPhase_Running;
  session->scheduler.run_intent = (RVS_RunIntent){
    .kind = RVS_RunIntentKind_Continue,
    .state = RVS_RunIntentState_Active,
  };
  RVS_SchedulerAdmission exit_interrupt_admission = {0};
  AssertAlways(rvs_scheduler_admit_locked(&session->scheduler, session->engine->request_pool,
                                           &session->engine->next_request_id, interrupt_key, &run_programs[0], 1, 0,
                                          &exit_interrupt_admission) == RVS_Result_Ok);
  rvs_test_activate_interrupt_locked(session, &exit_interrupt_admission);
  RVS_ScheduledOperation *exit_interrupt_operation = exit_interrupt_admission.operation;
  RVS_Request *exit_interrupt_request = exit_interrupt_operation->request;
  RVS_SchedulerCommandToken exit_interrupt_token = exit_interrupt_operation->pending_command.token;
  AssertAlways(session->scheduler.stop_transaction.execution_request_id == reducer_exit_run_request_id);
  RVS_ReducerEventTest exit_event_tests[] = {
    { RVS_SchedulerEvent_DemonEventBatch, reducer_exit_run_request_id, 1, RVS_SchedulerEmission_PublishEvent, RVS_SchedulerCommand_Null, RVS_Result_Ok, 0 },
    { RVS_SchedulerEvent_DemonEventBatch, reducer_exit_run_request_id, 0, RVS_SchedulerEmission_PublishEvent, RVS_SchedulerCommand_Null, RVS_Result_Ok, 0 },
    { RVS_SchedulerEvent_CommandOutcome,  exit_interrupt_request->request_id, 0, RVS_SchedulerEmission_PublishEvent, RVS_SchedulerCommand_ResumeTargetSubset, RVS_Result_Ok, 1 },
    { RVS_SchedulerEvent_CommandOutcome,  exit_interrupt_request->request_id, 0, RVS_SchedulerEmission_CompleteRequest, RVS_SchedulerCommand_Null, RVS_Result_Ok, 0 },
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
        .process = run_processes[test->target_index],
      };
      event.demon_events.request_id = test->source_request_id;
      event.demon_events.events = events;
      event.demon_events.dispositions = dispositions;
      event.demon_events.dispositions_count = 1;
    } else if (test->kind == RVS_SchedulerEvent_CommandOutcome) {
      B32 is_interrupt_completion = test_idx == 2;
      event.command_outcome.command_kind = is_interrupt_completion ? RVS_SchedulerCommand_InterruptExecution :
                                                                     RVS_SchedulerCommand_ResumeTargetSubset;
      event.command_outcome.command = is_interrupt_completion ? exit_interrupt_token : exit_resume_token;
      event.command_outcome.result = RVS_Result_Ok;
    }
    rvs_scheduler_apply_locked(&session->scheduler, event, &decision);
    AssertAlways((decision.emissions.first ? decision.emissions.first->kind : RVS_SchedulerEmission_Null) == test->expected_emission);
    AssertAlways(decision.command.kind == test->expected_command && decision.result == test->expected_result);
    if (test_idx == 0) { AssertAlways(decision.emissions.first->event.kind == RVS_EventKind_ProgramDestroyed); }
    if (test_idx == 1) {
      AssertAlways(rvs_scheduler_target_from_id_locked(&session->scheduler, run_programs[0])->state == RVS_TargetState_Removed);
    }
    if (test_idx == 2) {
      AssertAlways(decision.command.resume_target_subset.targets_count == test->expected_targets_count);
      AssertAlways(decision.emissions.first->event.kind == RVS_EventKind_Stopped &&
                   decision.emissions.first->event.stopped.primary_cause == RVS_StopCause_ProcessExit);
      exit_resume_token = decision.command.token;
      RVS_TargetControl *unselected_entry = rvs_scheduler_target_from_id_locked(&session->scheduler, run_programs[1]);
      AssertAlways(unselected_entry && unselected_entry->state == RVS_TargetExecutionState_InterruptPending);
    }
    if (test_idx == 3) { AssertAlways(decision.emissions.first->reply.result == RVS_Result_StaleState); }
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
  DMN_Handle batch_processes[] = {
    { .u64 = { 0x3001 } },
    { .u64 = { 0x3002 } },
  };
  for EachIndex(target_idx, ArrayCount(batch_targets)) {
    rvs_scheduler_target_add_locked(&session->scheduler, batch_targets[target_idx]);
    rvs_entity_program_create_locked(&session->entities, batch_targets[target_idx], 0);
    rvs_entity_process_create_locked(&session->entities, batch_targets[target_idx], rvs_process_id_from_handle(batch_processes[target_idx]),
                                     rvs_process_id_zero(), 0);
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
                                           &session->engine->next_request_id, (RVS_SchedulerKey){ .op = RVS_SchedulerOp_Interrupt, .identity = 2 },
                                          &batch_targets[0], 1, 0, &batch_interrupt_admission) == RVS_Result_Ok);
  rvs_test_activate_interrupt_locked(session, &batch_interrupt_admission);
  RVS_SchedulerCommandToken batch_interrupt_stop_token = batch_interrupt_admission.operation->pending_command.token;
  Temp batch_scratch = scratch_begin(0, 0);
  DMN_EventList batch_events = {0};
  *dmn_event_list_push(batch_scratch.arena, &batch_events) = (DMN_Event){ .kind = DMN_EventKind_Halt,        .process = batch_processes[0] };
  *dmn_event_list_push(batch_scratch.arena, &batch_events) = (DMN_Event){ .kind = DMN_EventKind_Halt,        .process = batch_processes[1] };
  *dmn_event_list_push(batch_scratch.arena, &batch_events) = (DMN_Event){ .kind = DMN_EventKind_ExitProcess, .process = batch_processes[1] };
  *dmn_event_list_push(batch_scratch.arena, &batch_events) = (DMN_Event){ .kind = DMN_EventKind_ExitProcess, .process = batch_processes[0] };
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
  AssertAlways(rvs_scheduler_target_from_id_locked(&session->scheduler, batch_targets[0])->state == RVS_TargetState_Removed);
  AssertAlways(rvs_scheduler_target_from_id_locked(&session->scheduler, batch_targets[1])->state == RVS_TargetState_Removed);
  AssertAlways(batch_decision.command.kind == RVS_SchedulerCommand_Null);
  for EachIndex(target_idx, batch_interrupt_admission.operation->targets_count) {
    AssertAlways(batch_interrupt_admission.operation->targets[target_idx].stop_observed);
    AssertAlways(batch_interrupt_admission.operation->targets[target_idx].exit_observed);
  }
  rvs_scheduler_decision_release(&batch_decision);
  scratch_end(batch_scratch);
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_CommandOutcome,
    .command_outcome = {
      .command_kind = RVS_SchedulerCommand_InterruptExecution,
      .command = batch_interrupt_stop_token,
      .result = RVS_Result_Ok,
    },
  }, &batch_decision);
  AssertAlways(batch_decision.command.kind == RVS_SchedulerCommand_Null);
  AssertAlways(batch_decision.emissions.first &&
               batch_decision.emissions.first->kind == RVS_SchedulerEmission_PublishEvent &&
               batch_decision.emissions.first->event.kind == RVS_EventKind_Stopped &&
               batch_decision.emissions.first->event.stopped.primary_cause == RVS_StopCause_ProcessExit &&
               batch_decision.emissions.first->next &&
               batch_decision.emissions.first->next->reply.result == RVS_Result_StaleState);
  for EachIndex(target_idx, batch_interrupt_admission.operation->targets_count) {
    AssertAlways(batch_interrupt_admission.operation->targets[target_idx].stable_stop_generation ==
                 session->scheduler.last_dispatched_stable_stop_generation);
  }
  RVS_Request *batch_interrupt_request = batch_interrupt_admission.request;
  AssertAlways(session->scheduler.last_stable_stop_used_fallback &&
               session->scheduler.last_stable_stop_command == RVS_SchedulerCommand_Null);
  rvs_scheduler_decision_release(&batch_decision);
  AssertAlways(session->scheduler.run_intent.state == RVS_RunIntentState_Completed);
  AssertAlways(session->scheduler.stop_transaction.owner == 0);
  rvs_request_release(batch_interrupt_request);
  mutex_drop(session->control->mutex);

  // Retired programs are terminal and cannot be revived by a non-advancing queue transition.
  mutex_take(session->control->mutex);
  RVS_TargetControl *terminal_program = rvs_scheduler_target_from_id_locked(&session->scheduler, reply.launch.program_id);
  AssertAlways(terminal_program && terminal_program->state == RVS_TargetState_Removed);
  AssertAlways(!rvs_scheduler_commit_target_locked(terminal_program, 0, RVS_TargetState_Queued, RVS_TargetTransition_Queue));
  mutex_drop(session->control->mutex);

  RVS_ProgramID queued_test_program = { .u64 = { 0x4100 } };
  RVS_TargetSnapshot queued_test_target = { .target = queued_test_program };
  mutex_take(session->control->mutex);
  rvs_scheduler_target_add_locked(&session->scheduler, queued_test_program);
  rvs_entity_program_create_locked(&session->entities, queued_test_program, 0);
  rvs_entity_process_create_locked(&session->entities, queued_test_program, (RVS_ProcessID){ .u64 = { 0x4100 } }, rvs_process_id_zero(), 0);
  // The original launch target exited in the preceding batch; use a live target for the remaining scheduler tests.
  reply.launch.program_id = queued_test_program;
  test_target = queued_test_target;
  AssertAlways(rvs_scheduler_reserve_execution_locked(&session->scheduler, &queued_test_target, 1, test_run_request_id));
  AssertAlways(rvs_scheduler_clear_queued_execution_locked(&session->scheduler, test_run_request_id));
  AssertAlways(rvs_scheduler_reserve_execution_locked(&session->scheduler, &queued_test_target, 1, test_run_request_id));
  mutex_drop(session->control->mutex);
  rvs_engine_process_demon_reply(engine, &(RVS_DemonReply){
    .kind = RVS_DemonReplyKind_ActionResult,
    .request_id = test_run_request_id,
    .action_result = { .action = RVS_DemonAction_Run, .result = RVS_Result_Error },
  });
  rvs_test_wait_until_execution_state(session, queued_test_program, RVS_TargetExecutionState_Idle);

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
  RVS_RequestControl *join_control = rvs_request_control_alloc(session, join_operation, 1);
  mutex_drop(session->control->mutex);
  rvs_request_control_release(join_control);
  mutex_take(session->control->mutex);
  AssertAlways(!join_operation->is_keyed);
  rvs_scheduler_operation_remove_locked(&session->scheduler, join_operation);
  mutex_drop(session->control->mutex);
  rvs_request_release(join_request); // drop caller ownership
  rvs_scheduler_operation_release(join_operation); // drop active registration ownership

  RVS_RequestPool *foreign_pool = rvs_request_pool_alloc();
  mutex_take(session->control->mutex);
  RVS_ScheduledOperation *foreign_operation = rvs_scheduler_operation_alloc_locked(&session->scheduler,
                                                                                      foreign_pool,
                                                                                      ins_atomic_u64_inc_eval(&session->engine->next_request_id),
                                                                                      join_key, 0, 0, 0);
  RVS_Request *foreign_request = foreign_operation->request;
  AssertAlways(foreign_operation == join_operation); // final releases recycle operations across request pools
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
  RVS_SchedulerEmission *recycled_emission = rvs_scheduler_emit_locked(&session->scheduler, &recycled_decision, RVS_SchedulerEmission_Null, 0);
  rvs_scheduler_decision_release(&recycled_decision);
  recycled_decision.result = RVS_Result_Ok;
  rvs_scheduler_emit_locked(&session->scheduler, &recycled_decision, RVS_SchedulerEmission_Null, 0);
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

  mutex_take(session->control->mutex);
  RVS_ScheduledOperation *duplicate_ack_operation = rvs_scheduler_operation_alloc_locked(&session->scheduler,
                                                                                           session->engine->request_pool,
                                                                                           ins_atomic_u64_inc_eval(&session->engine->next_request_id),
                                                                                           (RVS_SchedulerKey){ .op = RVS_SchedulerOp_Run, .identity = RVS_EngineCommandKind_Run },
                                                                                           &run_programs[1], 1, 0);
  duplicate_ack_operation->state = RVS_ScheduledOperationState_Active;
  RVS_Request *duplicate_ack_request = duplicate_ack_operation->request;
  RVS_EngineReply duplicate_ack_reply = {
    .request_id = duplicate_ack_request->request_id,
    .result = RVS_Result_Ok,
    .kind = RVS_EngineReplyKind_Run,
  };
  RVS_SchedulerDecision first_ack_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_BackendCompleted,
    .completed = { .reply = duplicate_ack_reply },
  }, &first_ack_decision);
  AssertAlways(first_ack_decision.result == RVS_Result_Ok &&
               first_ack_decision.emissions.first && duplicate_ack_operation->request_completed);
  rvs_scheduler_decision_release(&first_ack_decision);
  RVS_SchedulerDecision duplicate_ack_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_BackendCompleted,
    .completed = { .reply = duplicate_ack_reply },
  }, &duplicate_ack_decision);
  AssertAlways(duplicate_ack_decision.result == RVS_Result_StaleState &&
               duplicate_ack_decision.emissions.first == 0);
  rvs_scheduler_decision_release(&duplicate_ack_decision);
  RVS_SchedulerDecision stale_run_failure_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_OperationFailed,
    .failed = { .request_id = duplicate_ack_request->request_id, .result = RVS_Result_Error },
  }, &stale_run_failure_decision);
  AssertAlways(stale_run_failure_decision.result == RVS_Result_StaleState &&
               stale_run_failure_decision.emissions.first == 0);
  rvs_scheduler_decision_release(&stale_run_failure_decision);
  rvs_scheduler_operation_remove_locked(&session->scheduler, duplicate_ack_operation);
  mutex_drop(session->control->mutex);
  rvs_request_release(duplicate_ack_request);
  rvs_scheduler_operation_release(duplicate_ack_operation);

  mutex_take(session->control->mutex);
  RVS_ProgramID all_exit_target = { .u64 = { 0x5100 } };
  DMN_Handle all_exit_process = { .u64 = { 0x5100 } };
  rvs_scheduler_target_add_locked(&session->scheduler, all_exit_target);
  rvs_entity_program_create_locked(&session->entities, all_exit_target, 0);
  rvs_entity_process_create_locked(&session->entities, all_exit_target, rvs_process_id_from_handle(all_exit_process),
                                   rvs_process_id_zero(), 0);
  RVS_ScheduledOperation *all_exit_operation = rvs_scheduler_operation_alloc_locked(&session->scheduler,
                                                                                     session->engine->request_pool,
                                                                                     ins_atomic_u64_inc_eval(&session->engine->next_request_id),
                                                                                     (RVS_SchedulerKey){ .op = RVS_SchedulerOp_Run, .identity = RVS_EngineCommandKind_Run },
                                                                                     &all_exit_target, 1, 0);
  RVS_Request *all_exit_request = all_exit_operation->request;
  AssertAlways(rvs_scheduler_reserve_execution_locked(&session->scheduler, all_exit_operation->targets, 1,
                                                      all_exit_request->request_id));
  AssertAlways(rvs_scheduler_mark_run_in_flight_locked(&session->scheduler, all_exit_request->request_id));
  all_exit_operation->state = RVS_ScheduledOperationState_Active;
  all_exit_operation->request_completed = 1;
  session->scheduler.active_execution_request_id = all_exit_request->request_id;
  session->scheduler.phase = RVS_SchedulerPhase_Running;
  session->scheduler.run_intent = (RVS_RunIntent){
    .kind = RVS_RunIntentKind_Continue,
    .state = RVS_RunIntentState_Active,
  };
  RVS_SchedulerEventDisposition all_exit_disposition = RVS_SchedulerEventDisposition_Forward;
  RVS_SchedulerDecision all_exit_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = {
      .request_id = all_exit_request->request_id,
       .events = { .first = &(DMN_EventNode){ .v = { .kind = DMN_EventKind_ExitProcess, .process = all_exit_process } } },
      .dispositions = &all_exit_disposition,
      .dispositions_count = 1,
    },
  }, &all_exit_decision);
  AssertAlways(rvs_scheduler_operation_from_request_id_locked(&session->scheduler, all_exit_request->request_id) == 0);
  AssertAlways(session->scheduler.active_execution_request_id == 0);
  AssertAlways(rvs_scheduler_target_from_id_locked(&session->scheduler, all_exit_target)->state == RVS_TargetState_Removed);
  rvs_scheduler_decision_release(&all_exit_decision);
  mutex_drop(session->control->mutex);
  rvs_request_release(all_exit_request);

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

  // RunToAddress is a child plan whose one-shot trap survives both initial and resumed run preparation.
  mutex_take(session->control->mutex);
  RVS_ScheduledOperation *run_to_operation = rvs_scheduler_operation_alloc_locked(
    &session->scheduler, session->engine->request_pool,
    ins_atomic_u64_inc_eval(&session->engine->next_request_id),
    (RVS_SchedulerKey){ .op = RVS_SchedulerOp_Run, .identity = 0x6f00 }, &reply.launch.program_id, 1, 0);
  RVS_Request *run_to_request = run_to_operation->request;
  RVS_PlanID run_to_root_id = run_to_operation->root_plan_id;
  RVS_Plan *run_to_plan = rvs_scheduler_attach_run_to_address_locked(&session->scheduler, run_to_operation,
                                                                     reply.launch.program_id, 0x12345678);
  RVS_PlanID run_to_plan_id = run_to_plan->header.id;
  AssertAlways(run_to_plan->header.parent == run_to_root_id && run_to_operation->active_plan_id == run_to_plan_id);
  RVS_TargetControl *run_to_program = rvs_scheduler_target_from_id_locked(&session->scheduler, reply.launch.program_id);
  RVS_ProcessSnapshot run_to_process = {0};
  AssertAlways(rvs_entity_live_process_for_program_locked(&session->entities, run_to_program->id, &run_to_process));
  Temp run_to_scratch = scratch_begin(0, 0);
  RVS_ProcessSnapshot *run_to_processes = 0;
  U64 run_to_processes_count = 0;
  rvs_entity_copy_live_processes_locked(&session->entities, run_to_scratch.arena,
                                        &run_to_processes, &run_to_processes_count);
  RVS_ProgramID resume_run_to_target = reply.launch.program_id;
  RVS_SchedulerDecision run_to_resume_source = {
    .command = {
      .kind = RVS_SchedulerCommand_ResumeTargetSubset,
      .operation = run_to_operation,
      .resume_target_subset = {
        .targets = &resume_run_to_target,
        .targets_count = 1,
        .execution_request_id = run_to_request->request_id,
      },
    },
  };
  rvs_scheduler_prepare_decision_locked(&session->scheduler, &run_to_resume_source,
                                        run_to_processes, run_to_processes_count, run_to_scratch.arena);
  AssertAlways(run_to_resume_source.command.resume_target_subset.prepare_result == RVS_Result_Ok &&
               run_to_resume_source.command.resume_target_subset.traps.trap_count == 1 &&
               run_to_resume_source.command.resume_target_subset.traps.first->v[0].vaddr == 0x12345678 &&
               run_to_resume_source.command.resume_target_subset.traps.first->v[0].id == run_to_plan_id &&
               run_to_resume_source.command.resume_target_subset.traps.first->v[0].flags == 0 &&
               dmn_handle_match(run_to_resume_source.command.resume_target_subset.traps.first->v[0].process,
                                rvs_process_id_handle(run_to_process.process)));
  run_to_resume_source.command.resume_target_subset.targets_count = 0;
  rvs_scheduler_prepare_decision_locked(&session->scheduler, &run_to_resume_source,
                                        run_to_processes, run_to_processes_count, run_to_scratch.arena);
  AssertAlways(run_to_resume_source.command.resume_target_subset.traps.trap_count == 0);
  scratch_end(run_to_scratch);
  rvs_scheduler_operation_remove_locked(&session->scheduler, run_to_operation);
  AssertAlways(rvs_scheduler_plan_from_id_locked(&session->scheduler, run_to_root_id) == 0 &&
               rvs_scheduler_plan_from_id_locked(&session->scheduler, run_to_plan_id) == 0);
  rvs_scheduler_operation_release(run_to_operation);
  mutex_drop(session->control->mutex);
  rvs_request_release(run_to_request);

  // Route detached stable stops through leaf plans, parents, handlers, and fallback provenance.
  mutex_take(session->control->mutex);
  AssertAlways(session->scheduler.stop_transaction.owner == 0 && rvs_scheduler_reducer_active == 0);
  RVS_ScheduledOperation *routing_operation = rvs_scheduler_operation_alloc_locked(
    &session->scheduler, session->engine->request_pool,
    ins_atomic_u64_inc_eval(&session->engine->next_request_id),
    (RVS_SchedulerKey){ .op = RVS_SchedulerOp_Run, .identity = 0x7000 }, 0, 0, 0);
  RVS_Request *routing_request = routing_operation->request;
  RVS_Plan *routing_root = rvs_scheduler_plan_from_id_locked(&session->scheduler, routing_operation->root_plan_id);
  AssertAlways(routing_root && routing_root->header.kind == RVS_PlanKind_Run &&
               routing_operation->active_plan_id == routing_root->header.id);
  RVS_Plan *routing_leaf = rvs_scheduler_plan_alloc_locked(&session->scheduler, routing_operation,
                                                            RVS_PlanKind_Test, routing_root->header.id);
  routing_operation->active_plan_id = routing_leaf->header.id;
  RVS_SchedulerDecision routing_decision = {0};
  RVS_StableStop routing_stop = { .execution_request_id = routing_request->request_id };
  rvs_scheduler_reducer_active = &session->scheduler;

  // Leaf Unhandled bubbles to the root RunPlan, whose Continue prevents handler dispatch.
  routing_leaf->test_disposition = (RVS_StableStopDisposition){ .kind = RVS_StableStopDisposition_Unhandled };
  routing_stop.stable_stop_generation = ++session->scheduler.next_stable_stop_generation;
  session->scheduler.stop_transaction = (RVS_StopTransaction){
    .owner = routing_operation,
    .execution_request_id = routing_request->request_id,
    .phase = RVS_ControlTransactionPhase_Stable,
    .stable_stop_generation = routing_stop.stable_stop_generation,
  };
  RVS_StableStopDisposition routing_disposition = rvs_scheduler_dispatch_stable_stop_locked(
    &session->scheduler, routing_operation, &routing_stop, &routing_decision);
  AssertAlways(routing_disposition.kind == RVS_StableStopDisposition_Continue);
  AssertAlways(session->scheduler.test_plan_dispatch_count == 2 && session->scheduler.test_handler_dispatch_count == 0);
  AssertAlways(session->scheduler.test_first_plan_visited == routing_leaf->header.id &&
               session->scheduler.test_second_plan_visited == routing_root->header.id);
  AssertAlways(session->scheduler.last_stop_policy_source == RVS_StopPolicySource_Plan &&
               session->scheduler.decisive_plan_id == routing_root->header.id &&
               !session->scheduler.last_stable_stop_used_fallback);
  AssertAlways(!rvs_scheduler_stable_stop_dispatch_is_fresh_locked(&session->scheduler, &routing_stop));

  // Leaf Continue is decisive; neither its parent nor handlers run.
  routing_leaf->test_disposition = (RVS_StableStopDisposition){ .kind = RVS_StableStopDisposition_Continue };
  routing_stop.stable_stop_generation = ++session->scheduler.next_stable_stop_generation;
  session->scheduler.stop_transaction.stable_stop_generation = routing_stop.stable_stop_generation;
  routing_disposition = rvs_scheduler_dispatch_stable_stop_locked(&session->scheduler, routing_operation,
                                                                  &routing_stop, &routing_decision);
  AssertAlways(routing_disposition.kind == RVS_StableStopDisposition_Continue);
  AssertAlways(session->scheduler.test_plan_dispatch_count == 1 && session->scheduler.test_handler_dispatch_count == 0);
  AssertAlways(session->scheduler.decisive_plan_id == routing_leaf->header.id);

  // All plans Unhandled reaches the handler, whose Continue becomes decisive.
  routing_leaf->test_disposition = (RVS_StableStopDisposition){ .kind = RVS_StableStopDisposition_Unhandled };
  routing_root->header.kind = RVS_PlanKind_Test;
  routing_root->test_disposition = (RVS_StableStopDisposition){ .kind = RVS_StableStopDisposition_Unhandled };
  session->scheduler.test_handler_disposition = (RVS_StableStopDisposition){ .kind = RVS_StableStopDisposition_Continue };
  session->scheduler.test_handler_id = 0x71;
  routing_stop.stable_stop_generation = ++session->scheduler.next_stable_stop_generation;
  session->scheduler.stop_transaction.stable_stop_generation = routing_stop.stable_stop_generation;
  routing_disposition = rvs_scheduler_dispatch_stable_stop_locked(&session->scheduler, routing_operation,
                                                                  &routing_stop, &routing_decision);
  AssertAlways(routing_disposition.kind == RVS_StableStopDisposition_Continue);
  AssertAlways(session->scheduler.test_plan_dispatch_count == 2 && session->scheduler.test_handler_dispatch_count == 1);
  AssertAlways(session->scheduler.last_stop_policy_source == RVS_StopPolicySource_Handler &&
               session->scheduler.decisive_handler_id == session->scheduler.test_handler_id);

  // With every layer Unhandled, no semantic source is selected until scheduler fallback runs.
  session->scheduler.test_handler_disposition = (RVS_StableStopDisposition){ .kind = RVS_StableStopDisposition_Unhandled };
  routing_stop.stable_stop_generation = ++session->scheduler.next_stable_stop_generation;
  session->scheduler.stop_transaction.stable_stop_generation = routing_stop.stable_stop_generation;
  routing_disposition = rvs_scheduler_dispatch_stable_stop_locked(&session->scheduler, routing_operation,
                                                                  &routing_stop, &routing_decision);
  AssertAlways(routing_disposition.kind == RVS_StableStopDisposition_Unhandled &&
               session->scheduler.last_stop_policy_source == RVS_StopPolicySource_Null);

  rvs_scheduler_reducer_active = 0;
  MemoryZeroStruct(&session->scheduler.stop_transaction);
  RVS_PlanID routing_root_id = routing_root->header.id;
  RVS_PlanID routing_leaf_id = routing_leaf->header.id;
  rvs_scheduler_operation_remove_locked(&session->scheduler, routing_operation);
  AssertAlways(rvs_scheduler_plan_from_id_locked(&session->scheduler, routing_root_id) == 0 &&
               rvs_scheduler_plan_from_id_locked(&session->scheduler, routing_leaf_id) == 0);
  rvs_scheduler_operation_release(routing_operation);

  // Drive a real root RunPlan through fence establishment; Continue selects continuation, not fallback provenance.
  RVS_ProgramID run_plan_target = { .u64 = { 0x7201 } };
  rvs_scheduler_target_add_locked(&session->scheduler, run_plan_target);
  rvs_entity_program_create_locked(&session->entities, run_plan_target, 0);
  rvs_entity_process_create_locked(&session->entities, run_plan_target,
                                   rvs_process_id_from_handle((DMN_Handle){ .u64 = { 0x7201 } }), rvs_process_id_zero(), 0);
  RVS_ScheduledOperation *run_plan_operation = rvs_scheduler_operation_alloc_locked(
    &session->scheduler, session->engine->request_pool,
    ins_atomic_u64_inc_eval(&session->engine->next_request_id),
    (RVS_SchedulerKey){ .op = RVS_SchedulerOp_Run, .identity = 0x7200 }, &run_plan_target, 1, 0);
  RVS_Request *run_plan_request = run_plan_operation->request;
  AssertAlways(rvs_scheduler_reserve_execution_locked(&session->scheduler, run_plan_operation->targets, 1,
                                                       run_plan_request->request_id));
  AssertAlways(rvs_scheduler_mark_run_in_flight_locked(&session->scheduler, run_plan_request->request_id));
  run_plan_operation->state = RVS_ScheduledOperationState_Active;
  run_plan_operation->request_completed = 1;
  session->scheduler.active_execution_request_id = run_plan_request->request_id;
  session->scheduler.phase = RVS_SchedulerPhase_Running;
  session->scheduler.run_intent = (RVS_RunIntent){
    .kind = RVS_RunIntentKind_Continue,
    .state = RVS_RunIntentState_Active,
  };
  RVS_SchedulerAdmission run_plan_interrupt_admission = {0};
  AssertAlways(rvs_scheduler_admit_locked(&session->scheduler, session->engine->request_pool,
                                           &session->engine->next_request_id, (RVS_SchedulerKey){ .op = RVS_SchedulerOp_Interrupt, .identity = 0x7200 },
                                          &run_plan_target, 1, 0, &run_plan_interrupt_admission) == RVS_Result_Ok);
  rvs_test_activate_interrupt_locked(session, &run_plan_interrupt_admission);
  RVS_SchedulerCommandToken run_plan_interrupt_token = run_plan_interrupt_admission.operation->pending_command.token;
  RVS_Request *run_plan_interrupt_request = run_plan_interrupt_admission.request;
  RVS_SchedulerDecision run_plan_stop_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_CommandOutcome,
    .command_outcome = {
      .command_kind = RVS_SchedulerCommand_InterruptExecution,
      .command = run_plan_interrupt_token,
      .result = RVS_Result_Ok,
    },
  }, &run_plan_stop_decision);
  RVS_PlanID run_plan_root_id = run_plan_operation->root_plan_id;
  AssertAlways(run_plan_stop_decision.command.kind == RVS_SchedulerCommand_ResumeTargetSubset);
  AssertAlways(session->scheduler.last_plan_stop_disposition == RVS_StableStopDisposition_Continue &&
               session->scheduler.last_stop_policy_source == RVS_StopPolicySource_Plan &&
               session->scheduler.decisive_plan_id == run_plan_root_id &&
               !session->scheduler.last_stable_stop_used_fallback);
  RVS_SchedulerCommandToken run_plan_resume_token = run_plan_stop_decision.command.token;
  rvs_scheduler_decision_release(&run_plan_stop_decision);
  RVS_SchedulerDecision run_plan_resume_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_CommandOutcome,
    .command_outcome = {
      .command_kind = RVS_SchedulerCommand_ResumeTargetSubset,
      .command = run_plan_resume_token,
      .result = RVS_Result_Ok,
    },
  }, &run_plan_resume_decision);
  AssertAlways(run_plan_resume_decision.emissions.first &&
               run_plan_resume_decision.emissions.first->reply.result == RVS_Result_Ok);
  rvs_scheduler_decision_release(&run_plan_resume_decision);
  AssertAlways(run_plan_operation->state == RVS_ScheduledOperationState_Suspended);
  rvs_scheduler_operation_remove_locked(&session->scheduler, run_plan_operation);
  AssertAlways(rvs_scheduler_plan_from_id_locked(&session->scheduler, run_plan_root_id) == 0);
  rvs_scheduler_operation_release(run_plan_operation);
  mutex_drop(session->control->mutex);
  rvs_request_release(routing_request);
  rvs_request_release(run_plan_interrupt_request);
  rvs_request_release(run_plan_request);

  RVS_EventWaitTest event_waiter = { .session = session };
  Thread event_thread = thread_launch(rvs_event_wait_test_thread, &event_waiter);
  AssertAlways( ! MemoryIsZeroStruct(&event_thread));
  rvs_test_wait_until_event_waiting(session);
  ins_atomic_u32_eval_assign(&engine->test_hold_after_dispatch_prepare, 1);
  RVS_SubmitInfo shutdown_held_run_submit = {0};
  AssertAlways(rvs_session_run(session, run_programs[1], &shutdown_held_run_submit) == RVS_Result_Ok);
  rvs_test_wait_until_dispatch_prepared(engine);
  mutex_take(session->control->mutex);
  RVS_ScheduledOperation *shutdown_held_run_operation = rvs_scheduler_operation_from_request_id_locked(&session->scheduler,
                                                                                                         shutdown_held_run_submit.control->request_id);
  AssertAlways(shutdown_held_run_operation && shutdown_held_run_operation->state == RVS_ScheduledOperationState_Active);
  AssertAlways(session->scheduler.active_execution_request_id == shutdown_held_run_submit.control->request_id);
  mutex_drop(session->control->mutex);
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
      .command_kind = RVS_SchedulerCommand_PumpLaunch,
      .command = shutdown_pending_token,
      .result = RVS_Result_Error,
    },
  }, &late_command_decision);
  AssertAlways(late_command_decision.result == RVS_Result_StaleState);
  rvs_scheduler_decision_release(&late_command_decision);
  RVS_SchedulerDecision late_batch_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DemonEventBatch,
    .demon_events = {
      .request_id = test_run_request_id,
      .events = {
         .first = &(DMN_EventNode){ .v = { .kind = DMN_EventKind_ExitProcess, .process = run_processes[1] } },
      },
    },
  }, &late_batch_decision);
  AssertAlways(late_batch_decision.result == RVS_Result_StaleState);
  AssertAlways(late_batch_decision.emissions.first == 0 &&
               late_batch_decision.command.kind == RVS_SchedulerCommand_Null);
  AssertAlways(session->scheduler.phase == RVS_SchedulerPhase_Exited && session->scheduler.active_execution_request_id == 0);
  rvs_scheduler_decision_release(&late_batch_decision);
  mutex_drop(session->control->mutex);
  mutex_take(session->control->mutex);
  AssertAlways(rvs_scheduler_operation_from_request_id_locked(&session->scheduler, shutdown_held_run_submit.control->request_id) == 0);
  mutex_drop(session->control->mutex);
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
rvs_test_wait_until_dispatch_prepared(RVS_Engine *engine)
{
  while ( ! ins_atomic_u32_eval(&engine->test_is_held_after_dispatch_prepare)) {
    sleep_ms(1);
  }
}

internal void
rvs_test_wait_until_execution_state(RVS_Session *session, RVS_ProgramID target, RVS_TargetExecutionState state)
{
  for (;;) {
    mutex_take(session->control->mutex);
    RVS_TargetControl *entry = rvs_scheduler_target_from_id_locked(&session->scheduler, target);
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
  rvs_scheduler_decision_release(&decision);
  return accepted;
}

internal RVS_SchedulerCommandToken
rvs_test_launch_pump_token(RVS_Engine *engine, RVS_MessageID request_id)
{
  mutex_take(engine->control->mutex);
  RVS_ScheduledOperation *operation = rvs_scheduler_operation_from_request_id_locked(&engine->session->scheduler, request_id);
  AssertAlways(operation && operation->pending_command.kind == RVS_SchedulerCommand_PumpLaunch);
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
  B32 accepted = decision.result == RVS_Result_Ok;
  rvs_test_execute_scheduler_decision(engine, &decision);
  scratch_end(scratch);
  return accepted;
}

internal void
rvs_test_activate_interrupt_locked(RVS_Session *session, RVS_SchedulerAdmission *admission)
{
  RVS_ScheduledOperation *operation = admission->operation;
  AssertAlways(operation && operation->key.op == RVS_SchedulerOp_Interrupt && operation->targets_count != 0);
  if (session->scheduler.active_execution_request_id == 0) {
    RVS_TargetControl *entry = rvs_scheduler_target_from_id_locked(&session->scheduler, operation->targets[0].target);
    AssertAlways(entry && entry->execution_owner != 0);
    session->scheduler.active_execution_request_id = entry->execution_owner;
  }
  session->scheduler.phase = RVS_SchedulerPhase_Running;
  session->scheduler.run_intent = (RVS_RunIntent){
    .kind = RVS_RunIntentKind_Continue,
    .state = RVS_RunIntentState_Active,
  };
  RVS_SchedulerDecision dispatch_decision = {0};
  rvs_scheduler_apply_locked(&session->scheduler, (RVS_SchedulerEvent){
    .kind = RVS_SchedulerEvent_DispatchStarted,
    .request = { .request_id = operation->request->request_id },
  }, &dispatch_decision);
  AssertAlways(dispatch_decision.result == RVS_Result_Ok);
  AssertAlways(dispatch_decision.command.kind == RVS_SchedulerCommand_InterruptExecution);
  AssertAlways(dispatch_decision.command.interrupt_execution.execution_request_id == session->scheduler.stop_transaction.execution_request_id);
  AssertAlways(dispatch_decision.command.token.command_id != 0);
  rvs_scheduler_decision_release(&dispatch_decision);
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
