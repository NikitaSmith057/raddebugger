// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#define BUILD_CONSOLE_INTERFACE 1
#define BUILD_TITLE "Debugger CLI"

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

////////////////////////////////

#define RVS_CLI_CMD_XLIST    \
  X(Help,      "HELP")       \
  X(Launch,    "LAUNCH")     \
  X(Start,     "START")      \
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
  g_rci.engine       = engine;
  g_rci.output_mutex = mutex_alloc();

  for (;;) {
    Temp scratch = scratch_begin(0,0);

    // read and parse input
    rci_fprintf(stdout, "dbg> ");
    char line_buffer[4096] = {0};
    if (fgets(line_buffer, sizeof(line_buffer), stdin) == 0) {
      break;
    }
    String8     input       = str8_skip_chop_whitespace(str8_cstring_capped(line_buffer, line_buffer+sizeof(line_buffer)));
    String8List input_split = str8_split_by_string_chars(scratch.arena, input, str8_lit(" "), 0);
    String8     cmd         = str8_list_first(&input_split);

    // dispatch command
    if (str8_matchi(cmd, str8_lit("launch"))) {
      if (input_split.node_count != 2) {
        rci_fprintf(stdout, "launch: invalid number of arguments\n");
        continue;
      }

      String8       exe_path        = input_split.first->next->string;
      RVS_MessageID launch_reply_id = 0;
      RVS_Result    launch_result   = rvs_engine_launch_async(engine, exe_path, str8_zero(), &launch_reply_id);

      if (launch_result != RVS_Result_Ok) {
        rci_fprintf(stdout, "launch: failed to launch program %S, error code %u\n", exe_path, launch_result);
        continue;
      }


      RVS_Reply reply = {0};
      RVS_Result reply_result = rvs_engine_wait_for_reply(scratch.arena, engine, launch_reply_id, max_U64, &reply);
      if (reply_result != RVS_Result_Ok) {
        rci_fprintf(stdout, "launch: request failed, error code %u\n", reply_result);
        continue;
      }
      if (reply.reply_id != launch_reply_id || reply.kind != RVS_ReplyKind_LaunchAck) {
        rci_fprintf(stdout, "launch: received an invalid completion\n");
        continue;
      }

      rci_fprintf(stdout, "launch: program %llu started with pid %u\n", reply.launch_ack.program_id, reply.launch_ack.pid);
    } else {
      rci_fprintf(stdout, "unknown command: %S", cmd);
    }

    scratch_end(scratch);
  }
}
