// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#define BUILD_CONSOLE_INTERFACE 1
#define BUILD_TITLE "DBG CLI"
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

#include "radvs/rvs_async.c"
#include "radvs/rvs_demon.c"
#include "radvs/rvs_engine.c"

////////////////////////////////

#define RVS_CLI_CMD_XLIST    \
  X(Help,      "HELP")       \
  X(Launch,    "LAUNCH")     \
  X(Run,       "Run")        \
  X(Stop,      "STOP")       \
  X(Continue,  "CONTINUE")   \
  X(Step,      "STEP")       \
  X(Break,     "BREAK")      \
  X(Bp,        "BP")         \
  X(BpEnable,  "BP-ENABLE")  \
  X(BpDisable, "BP-DISABLE") \
  X(BpDelete,  "BP-DELETE")  \
  X(Threads,   "THREADS")    \
  X(Modules,   "MODULES")    \
  X(Exit,      "EXIT")

typedef enum
{
#define X(id, ...) RVS_CliCmdKind_##id,
  RVS_CLI_CMD_XLIST
#undef X
  RVS_CliCmdKind_Count,
} RVS_CliCmdKind;

internal String8
rvs_name_from_cli_cmd_kind(RVS_CliCmdKind k)
{
  switch (k) {
#define X(id, name) case RVS_CliCmdKind_##id: return str8_lit(name);
  RVS_CLI_CMD_XLIST
#undef X
  }
  return str8_zero();
}

typedef struct
{
  RVS_Engine *engine;
  Mutex       output_mutex;
} RCI_Context;

global RCI_Context g_rci;

internal void
rci_fprintf(FILE *f, char *fmt, ...)
{
  Temp scratch = scratch_begin(0, 0);
  va_list args;
  va_start(args, fmt);
  String8 string = str8fv(scratch.arena, fmt, args);
  va_end(args);
  mutex_take(g_rci.output_mutex);
  fwrite(string.str, 1, string.size, f);
  mutex_drop(g_rci.output_mutex);
  scratch_end(scratch);
}

internal void
entry_point(CmdLine *cmdline)
{
  RVS_Engine *engine = 0;
  if (rvs_engine_init(&engine) != RVS_Result_Ok) { InvalidPath; }

  RVS_Session *session = 0;
  if (rvs_engine_create_session(engine, &session) != RVS_Result_Ok) { InvalidPath; }

  g_rci.engine       = engine;
  g_rci.output_mutex = mutex_alloc();

  for (;;) {
    Temp scratch = scratch_begin(0,0);

    //
    // read and parse input
    //
    rci_fprintf(stdout, "DBG> ");
    char line_buffer[4096] = {0};
    if (fgets(line_buffer, sizeof(line_buffer), stdin) == 0) {
      break;
    }
    String8     input       = str8_skip_chop_whitespace(str8_cstring_capped(line_buffer, line_buffer+sizeof(line_buffer)));
    String8List input_split = str8_split_by_string_chars(scratch.arena, input, str8_lit(" "), 0);

    //
    // dispatch the input command
    //
    String8 command_string = str8_list_first(&input_split);
    if (str8_match_lit("launch", command_string, StringMatchFlag_CaseInsensitive)) {
      if (input_split.node_count != 2) {
        rci_fprintf(stdout, "launch: invalid number of arguments\n");
        continue;
      }

      RVS_SubmitInfo submit;
      String8        exe_path       = input_split.first->next->string;
      RVS_Result     launch_result  = rvs_session_launch(session, exe_path, str8_zero(), &submit);

      if (launch_result != RVS_Result_Ok) {
        rci_fprintf(stdout, "launch: failed to launch program %S, error code %u\n", exe_path, launch_result);
        continue;
      }

      RVS_EngineReply reply = {0};
      RVS_Result reply_result = rvs_request_wait(submit.request, max_U64, &reply);
      rvs_request_release(submit.request);
      rvs_request_control_release(submit.control);

      if (reply_result != RVS_Result_Ok) {
        rci_fprintf(stdout, "launch: request failed, error code %u\n", reply_result);
        continue;
      }
      if (reply.result != RVS_Result_Ok) {
        rci_fprintf(stdout, "launch: request failed, error code %u\n", reply.result);
        continue;
      }
      if (reply.kind != RVS_EngineReplyKind_Launch) {
        rci_fprintf(stdout, "launch: received an invalid completion\n");
        continue;
      }

      rci_fprintf(stdout, "launch: program %llx (%S) started with pid %u\n", reply.launch.program_id.u64[0], exe_path, reply.launch.pid);
    }

    else if (str8_match_lit("run", command_string, StringMatchFlag_CaseInsensitive)) {
      if (input_split.node_count != 2) {
        rci_fprintf(stdout, "run: invalid number of arguments\n");
        continue;
      }

      U64 program_id_u64;
      if ( ! try_u64_from_str8_c_rules(input_split.last->string, &program_id_u64)) {
        rci_fprintf(stdout, "run: failed to parse program ID string %S\n", input_split.last->string);
        continue;
      }
      RVS_ProgramID program_id = { .u64 = { program_id_u64 } };

      RVS_SubmitInfo submit = {0};
      RVS_Result run_result = rvs_session_run(session, program_id, &submit);
      if (run_result != RVS_Result_Ok) {
        rci_fprintf(stdout, "run: failed to submit program %llu, error code %u\n", program_id.u64[0], run_result);
        continue;
      }

      RVS_EngineReply reply = {0};
      RVS_Result reply_result = rvs_request_wait(submit.request, max_U64, &reply);
      rvs_request_release(submit.request);
      rvs_request_control_release(submit.control);
      if (reply_result != RVS_Result_Ok || reply.result != RVS_Result_Ok || reply.kind != RVS_EngineReplyKind_Run) {
        rci_fprintf(stdout, "run: request failed, error code %u\n", reply_result != RVS_Result_Ok ? reply_result : reply.result);
        continue;
      }
      rci_fprintf(stdout, "run: program %llu resumed\n", program_id.u64[0]);
    }

    else if (command_string.size != 0) {
      rci_fprintf(stdout, "unknown command: %S\n", command_string);
    }

    scratch_end(scratch);
  }

  rvs_session_release(session);
  rvs_engine_shutdown(engine);
  mutex_release(g_rci.output_mutex);
}
