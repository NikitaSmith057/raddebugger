// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////

#define BUILD_CONSOLE_INTERFACE 1
#define BUILD_TITLE "DEBUG ENGINE CLI"
#define DMN_INIT_MANUAL 1

#include "third_party/radsort/radsort.h"

#include "base/base_inc.h"
#include "x64/x64.h"
#include "win32/win32_inc.h"
#include "coff/coff.h"
#include "coff/coff_parse.h"
#include "pe/pe.h"
#include "elf/elf.h"
#include "elf/elf_parse.h"
#include "stap/stap_parse.h"
#include "gnu/gnu.h"
#include "gnu/gnu_parse.h"
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
#include "elf/elf.c"
#include "elf/elf_parse.c"
#include "stap/stap_parse.c"
#include "gnu/gnu.c"
#include "gnu/gnu_parse.c"
#include "linker/hash_table.c"
#include "linker/lnk_cmd_line.c"
#include "rdi/rdi_local.c"
#include "arch/arch_inc.c"
#include "demon/demon_inc.c"

#include "radvs/rvs_async.c"
#include "radvs/rvs_demon.c"
#include "radvs/rvs_engine.c"

////////////////////////////////

#define RCI_CMD_XLIST                                                                         \
  X(Launch,       "{EXE-PATH}",             "Create a process from an input file (EXE, ELF)") \
  X(Run,          "{PROGRAM-ID}",           "Runs the specified program")                     \
  X(RunAddr,      "{PROGRAM-ID} {ADDRESS}", "Run program to the specified address")           \
  X(Teardown,     "{PROGRAM-ID}",           "Kick off program shutdown sequence ")            \
  X(LsProg,       "",                       "List programs under the debug engine")           \
  X(Exit,         "",                       "Exit the debugger")                              \
  X(Help,         "",                       "Prints the help menu")

typedef enum
{
  RCI_CmdKind_Null,
#define X(id, ...) RCI_CmdKind_##id,
  RCI_CMD_XLIST
#undef X
  RCI_CmdKind_Count,
} RCI_CmdKind;

typedef struct
{
  RVS_Engine *engine;
  Mutex       output_mutex;
} RCI_Context;

////////////////////////////////

global RCI_Context g_rci;

internal RCI_CmdKind
rci_cmd_kind_from_string(String8 string)
{
#define X(id, ...) if (str8_matchi(string, str8_lit(Stringify(id)))) return RCI_CmdKind_##id;
  RCI_CMD_XLIST
#undef X
  return RCI_CmdKind_Null;
}

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
#define rci_printf(fmt, ...) rci_fprintf(stdout, fmt, ## __VA_ARGS__)

internal void
rci_print_help(void)
{
  rci_fprintf(stdout, "--- Help ----------------------------------------------------------------------\n");

#define X(id, arg, desc, ...) rci_fprintf(stdout, "  %-8s %-16s %s\n", Stringify(id), arg, desc);
  RCI_CMD_XLIST
#undef X
}

internal void
entry_point(CmdLine *cmdline)
{
  // init debug engine
  RVS_Engine *engine = 0;
  RVS_Result  engine_init_result = rvs_engine_init(&engine);
  if (engine_init_result != RVS_Result_Ok) {
    rci_fprintf(stderr, "ERROR: failed to initialize the debug engine; error code %u\n", engine_init_result);
    goto engine_exit;
  }

  // TODO: allocate engine session (currently only one session is allowed per engine, for final version hide it behind rvs_engine_* api)
  RVS_Session *session = 0;
  RVS_Result   session_create_result = rvs_engine_create_session(engine, &session);
  if (session_create_result != RVS_Result_Ok) {
    rci_fprintf(stderr, "ERROR: failed to create a session; error code %u\n", session_create_result);
    goto session_exit;
  }

  g_rci.engine       = engine;
  g_rci.output_mutex = mutex_alloc();

  Temp scratch = scratch_begin(0,0);
  U64   line_buffer_size = KB(1);
  char *line_buffer      = push_array(scratch.arena, char, line_buffer_size);
  for (B32 keep_running = 1; keep_running;) {
    Temp temp = temp_begin(scratch.arena);

    // read and parse a command for the debugger
    rci_printf("DBG <: ");
    if (fgets(line_buffer, line_buffer_size, stdin) == 0) {
      break;
    }

    // parse command and options
    String8     input       = str8_cstring_capped(line_buffer, line_buffer + line_buffer_size);
    String8List cmd_raw     = str8_split_by_string_chars(scratch.arena, input, str8_lit(" "), 0);
    String8     cmd_string  = str8_skip_chop_whitespace(str8_list_first(&cmd_raw));
    RCI_CmdKind cmd_kind    = rci_cmd_kind_from_string(cmd_string);
    LNK_CmdLine cmd_options = lnk_cmd_line_from_string_windows_rules(scratch.arena, cmd_string);

    // dispatch the input command
    switch (cmd_kind) {
    case RCI_CmdKind_Launch: {
      if (cmd_raw.node_count != 2) {
        rci_printf("Launch: invalid number of arguments\n");
        continue;
      }

      RVS_SubmitInfo submit;
      String8        exe_path       = str8_skip_chop_whitespace(cmd_raw.first->next->string);
      RVS_Result     launch_result  = rvs_session_launch(session, exe_path, str8_zero(), &submit);

      if (launch_result != RVS_Result_Ok) {
        rci_printf("Launch: failed to launch program %S, error code %u\n", exe_path, launch_result);
        continue;
      }

      RVS_EngineReply reply;
      RVS_Result      reply_result = rvs_request_wait(submit.request, max_U64, &reply);
      rvs_request_release(submit.request);
      rvs_request_control_release(submit.control);

      if (reply_result != RVS_Result_Ok) {
        rci_printf("Launch: request failed, error code %u\n", reply_result);
        continue;
      }
      if (reply.result != RVS_Result_Ok) {
        rci_printf("Launch: request failed, error code %u\n", reply.result);
        continue;
      }
      if (reply.kind != RVS_EngineReplyKind_Launch) {
        rci_printf("Launch: received an invalid completion\n");
        continue;
      }

      rci_printf("Launch: program 0x%llx (%S) started with pid %u\n", reply.launch.program_id.u64[0], exe_path, reply.launch.pid);
    } break;

    case RCI_CmdKind_Run: {
      if (cmd_raw.node_count != 2) {
        rci_printf("Run: invalid number of arguments\n");
        continue;
      }

      RVS_ProgramID program_id;
      if ( ! try_u64_from_str8_c_rules(cmd_raw.last->string, &program_id.u64[0])) {
        rci_printf("Run: failed to parse program ID string %S\n", cmd_raw.last->string);
        continue;
      }

      RVS_SubmitInfo submit;
      RVS_Result     run_result = rvs_session_run(session, program_id, &submit);
      if (run_result != RVS_Result_Ok) {
        rci_printf("Run: failed to submit program 0x%llx, error code %u\n", program_id.u64[0], run_result);
        continue;
      }

      RVS_EngineReply reply;
      RVS_Result      reply_result = rvs_request_wait(submit.request, max_U64, &reply);
      rvs_request_release(submit.request);
      rvs_request_control_release(submit.control);
      if (reply_result != RVS_Result_Ok || reply.result != RVS_Result_Ok || reply.kind != RVS_EngineReplyKind_Run) {
        rci_printf("Run: request failed, error code %u\n", reply_result != RVS_Result_Ok ? reply_result : reply.result);
        continue;
      }
      rci_printf("Run: program 0x%llx resumed\n", program_id.u64[0]);
    } break;

    case RCI_CmdKind_RunAddr: {
      if (cmd_raw.node_count != 3) {
        rci_printf("RunAddr: expected <program-id> <absolute-address>\n");
        continue;
      }

      String8 program_string = cmd_raw.first->next->string;
      String8 address_string = cmd_raw.last->string;
      U64     program_id_u64 = 0;
      U64     address        = 0;
      if (!try_u64_from_str8_c_rules(program_string, &program_id_u64)) {
        rci_printf("RunAddr: failed to parse program ID string %S\n", program_string);
        continue;
      }
      if (!try_u64_from_str8_c_rules(address_string, &address) || address == 0) {
        rci_printf("RunAddr: failed to parse non-zero address string %S\n", address_string);
        continue;
      }

      RVS_ProgramID program_id = { .u64 = { program_id_u64 } };
      RVS_SubmitInfo submit;
      RVS_Result run_result = rvs_session_run_to_address(session, program_id, address, &submit);
      if (run_result != RVS_Result_Ok) {
        rci_printf("RunAddr: failed to submit program 0x%llx, error code %u\n",
                    program_id.u64[0], run_result);
        continue;
      }

      RVS_EngineReply reply;
      RVS_Result      reply_result = rvs_request_wait(submit.request, max_U64, &reply);
      rvs_request_release(submit.request);
      rvs_request_control_release(submit.control);
      if (reply_result != RVS_Result_Ok || reply.result != RVS_Result_Ok || reply.kind != RVS_EngineReplyKind_Run) {
        rci_printf("RunAddr: request failed, error code %u\n",
                    reply_result != RVS_Result_Ok ? reply_result : reply.result);
        continue;
      }
      rci_printf("RunAddr: program 0x%llx resumed toward 0x%llx\n", program_id.u64[0], address);
    } break;

    case RCI_CmdKind_Teardown: {
      NotImplemented;
    } break;

    case RCI_CmdKind_LsProg: {
      rci_printf("--- Programs -------------------------------------------------------------------\n");
      rci_printf("  %-3s %-10s %-8s %-9s %s\n", "No.", "PROGRAM-ID", "PID", "LIFECYCLE", "PATH");
      rci_printf("  --- ---------- -------- --------- ----\n");
      U64 program_idx = 0; 
      for EachNode(prog, RVS_Program, engine->session->first_program) {
        rci_printf("  %-3u %-10llx %-8u %-8S\n", program_idx, prog->id.u64[0], prog->pid, rvs_string_from_live_lifecycle(prog->lifecycle));
      }
    } break;

    case RCI_CmdKind_Help: {
      rci_print_help();
    } break;

    case RCI_CmdKind_Exit: {
      keep_running = 0;
    } break;

    default: {
      rci_printf("unknown command: %S; use \"help\" to see the commands\n", cmd_string);
    } break;
    }

    temp_end(temp);
  }
  scratch_end(scratch);

  session_exit:;
  // shutdown the debug engine
  rvs_session_release(session);
  engine_exit:;
  RVS_Result shutdown_result = rvs_engine_shutdown(engine);
  rci_printf("debug engine exited with code %u%s\n", shutdown_result, shutdown_result == RVS_Result_Ok ? " (Ok)" : "");

  mutex_release(g_rci.output_mutex);
}
