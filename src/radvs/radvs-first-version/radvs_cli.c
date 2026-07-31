// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#define BUILD_CONSOLE_INTERFACE 1
#define RADVS_REQUEST_DIAGNOSTICS 1

#include <stdio.h>
#include <stdarg.h>
#include "radvs/radvs_bridge_main.c"

typedef struct
{
  RADVS_Session  *session;
  Thread          event_thread;
  Mutex           state_mutex;
  Mutex           output_mutex;
  DMN_Handle      last_process_handle;
  DMN_Handle      last_stopped_thread_handle;
  String8         exe;
  String8         args;
  String8         wdir;
} RADVS_CLI;

internal String8
radvs_cli_result_string(RADVS_Result result)
{
#define X(name, string) if (result == RADVS_Result_##name) { return str8_lit(string); }
  RADVS_Result_XList
#undef X
  return str8_lit("unknown");
}

internal void
radvs_cli_fprintf(FILE *file, char *fmt, ...)
{
  Temp scratch = scratch_begin(0, 0);
  va_list args;
  va_start(args, fmt);
  String8 string = str8fv(scratch.arena, fmt, args);
  va_end(args);
  fwrite(string.str, 1, string.size, file);
  scratch_end(scratch);
}

internal void
radvs_cli_print_result(RADVS_CLI *cli, char *command, RADVS_Result result)
{
  MutexScope (cli->output_mutex) {
    radvs_cli_fprintf(stderr, "%S: %S\n", str8_cstring((U8 *)command), radvs_cli_result_string(result));
  }
}

internal B32
radvs_cli_is_stop_event(DMN_EventKind kind)
{
  switch (kind) {
  case DMN_EventKind_Breakpoint:
  case DMN_EventKind_Trap:
  case DMN_EventKind_SingleStep:
  case DMN_EventKind_Exception:
  case DMN_EventKind_Halt: return 1;
  default:                 return 0;
  }
}

internal void
radvs_cli_print_event(RADVS_CLI *cli, RADVS_Event *event)
{
  MutexScope (cli->output_mutex) {
    radvs_cli_fprintf(stdout, "event kind=%u process_handle=%I64u thread_handle=%I64u module_handle=%I64u breakpoint=%I64u ip=0x%I64x code=%u\n",
                      event->raw.kind,
                       event->process_handle.u64[0],
                       event->thread_handle.u64[0],
                       event->module_handle.u64[0],
                       event->breakpoint_id,
                      event->raw.instruction_pointer,
                      event->raw.code);
    if (event->raw.string.size != 0) {
      radvs_cli_fprintf(stdout, "  %S\n", event->raw.string);
    }
    fflush(stdout);
  }
}

internal void
radvs_cli_event_worker(void *user_data)
{
  RADVS_CLI *cli = user_data;
  RADVS_EngineSession *engine_session = cli->session->engine_session;
  for (;;) {
    if (radvs_engine_session_wait_event(engine_session, max_U64) != RADVS_Result_Ok) {
      break;
    }
    for (;;) {
      RADVS_Event event = {0};
      if (radvs_engine_session_poll_event(engine_session, &event) != RADVS_Result_Ok) {
        break;
      }
      MutexScope (cli->state_mutex) {
        if (!MemoryIsZeroStruct(&event.process_handle)) {
          cli->last_process_handle = event.process_handle;
        }
        if (!MemoryIsZeroStruct(&event.thread_handle) && radvs_cli_is_stop_event(event.raw.kind)) {
          cli->last_stopped_thread_handle = event.thread_handle;
        }
      }
      radvs_cli_print_event(cli, &event);
    }
  }
}

internal String8
radvs_cli_next_token(String8 *input)
{
  String8 string = str8_skip_chop_whitespace(*input);
  U64 index = 0;
  for (; index < string.size && !char_is_space(string.str[index]); index += 1) {}
  String8 token = str8(string.str, index);
  *input = str8_skip_chop_whitespace(str8(string.str + index, string.size - index));
  return token;
}

internal B32
radvs_cli_parse_u64(String8 string, U64 *out_value)
{
  return string.size != 0 && try_u64_from_str8_c_rules(string, out_value);
}

internal DMN_Handle
radvs_cli_last_stopped_thread_handle(RADVS_CLI *cli)
{
  DMN_Handle result = {0};
  MutexScope (cli->state_mutex) {
    result = cli->last_stopped_thread_handle;
  }
  return result;
}

internal B32
radvs_cli_parse_handle(String8 string, DMN_Handle *out_handle)
{
  U64 value = 0;
  if (!out_handle || !radvs_cli_parse_u64(string, &value)) {
    return 0;
  }
  *out_handle = (DMN_Handle){ .u64 = {value} };
  return 1;
}

internal void
radvs_cli_print_help(RADVS_CLI *cli)
{
  (void)cli;
  radvs_cli_fprintf(stdout,
                    "commands:\n"
                    "  launch [exe]       launch --exe target, or the supplied executable\n"
                    "  run                 run all session processes\n"
                    "  continue [thread]  continue the stopped thread\n"
                    "  step [thread]      single-step the stopped thread\n"
                    "  break               halt the active session\n"
                    "  bp <address>       create an enabled address breakpoint\n"
                    "  bp-enable <id>     enable a breakpoint\n"
                    "  bp-disable <id>    disable a breakpoint\n"
                    "  bp-delete <id>     remove a breakpoint\n"
                    "  threads             list tracked threads\n"
                    "  modules             list tracked modules\n"
                    "  terminate [process] terminate one process or all session processes\n"
                    "  wait <milliseconds> wait for asynchronous events\n"
                    "  help\n"
                    "  quit\n");
  fflush(stdout);
}

internal RADVS_Result
radvs_cli_launch(RADVS_CLI *cli, String8 exe)
{
  if (exe.size == 0) {
    return RADVS_Result_InvalidArgument;
  }
  U32 pid = 0;
  RADVS_Result result = radvs_engine_session_launch(cli->session->engine_session, exe, cli->args, cli->wdir, &pid);
  if (result == RADVS_Result_Ok) {
    MutexScope (cli->output_mutex) {
      radvs_cli_fprintf(stdout, "launched pid=%u\n", pid);
      fflush(stdout);
    }
  }
  return result;
}

internal void
radvs_cli_list_threads(RADVS_CLI *cli)
{
  U64 count = 0;
  RADVS_Result result = radvs_engine_session_copy_threads(cli->session->engine_session, 0, 0, &count);
  if (result != RADVS_Result_Ok && result != RADVS_Result_OutOfMemory) {
    radvs_cli_print_result(cli, "threads", result);
    return;
  }
  Temp scratch = scratch_begin(0, 0);
  RADVS_ThreadDesc *threads = push_array(scratch.arena, RADVS_ThreadDesc, count);
  result = radvs_engine_session_copy_threads(cli->session->engine_session, threads, count, &count);
  if (result == RADVS_Result_Ok) {
    MutexScope (cli->output_mutex) {
      for EachIndex(index, count) {
        RADVS_ThreadDesc *thread = &threads[index];
        radvs_cli_fprintf(stdout, "thread handle=%I64u process_handle=%I64u os_tid=%u state=%u epoch=%I64u\n",
                           thread->thread_handle.u64[0],
                           thread->process_handle.u64[0],
                          thread->system_thread_id,
                          thread->state,
                          thread->process_state_epoch);
      }
      fflush(stdout);
    }
  } else {
    radvs_cli_print_result(cli, "threads", result);
  }
  scratch_end(scratch);
}

internal void
radvs_cli_list_modules(RADVS_CLI *cli)
{
  U64 count = 0;
  RADVS_Result result = radvs_engine_session_copy_modules(cli->session->engine_session, 0, 0, &count);
  if (result != RADVS_Result_Ok && result != RADVS_Result_OutOfMemory) {
    radvs_cli_print_result(cli, "modules", result);
    return;
  }
  Temp scratch = scratch_begin(0, 0);
  RADVS_ModuleDesc *modules = push_array(scratch.arena, RADVS_ModuleDesc, count);
  result = radvs_engine_session_copy_modules(cli->session->engine_session, modules, count, &count);
  if (result == RADVS_Result_Ok) {
    MutexScope (cli->output_mutex) {
      for EachIndex(index, count) {
        RADVS_ModuleDesc *module = &modules[index];
        radvs_cli_fprintf(stdout, "module handle=%I64u process_handle=%I64u base=0x%I64x size=0x%I64x symbols=%u\n",
                           module->module_handle.u64[0],
                           module->process_handle.u64[0],
                          module->base_address,
                          module->size,
                          module->symbol_state);
      }
      fflush(stdout);
    }
  } else {
    radvs_cli_print_result(cli, "modules", result);
  }
  scratch_end(scratch);
}

internal B32
radvs_cli_dispatch_command(RADVS_CLI *cli, String8 command_line)
{
  String8 command = radvs_cli_next_token(&command_line);
  if (command.size == 0) {
    return 1;
  }

  RADVS_Result result = RADVS_Result_InvalidArgument;
  if (str8_matchi(command, str8_lit("help"))) {
    MutexScope (cli->output_mutex) {
      radvs_cli_print_help(cli);
    }
  } else if (str8_matchi(command, str8_lit("launch"))) {
    String8 exe = radvs_cli_next_token(&command_line);
    result = radvs_cli_launch(cli, exe.size != 0 ? exe : cli->exe);
    if (result != RADVS_Result_Ok) { radvs_cli_print_result(cli, "launch", result); }
  } else if (str8_matchi(command, str8_lit("run"))) {
    result = radvs_engine_session_run(cli->session->engine_session, 0, 0);
    if (result != RADVS_Result_Ok) { radvs_cli_print_result(cli, "run", result); }
  } else if (str8_matchi(command, str8_lit("continue"))) {
    DMN_Handle thread_handle = radvs_cli_last_stopped_thread_handle(cli);
    String8 argument = radvs_cli_next_token(&command_line);
    if (argument.size != 0 && !radvs_cli_parse_handle(argument, &thread_handle)) { thread_handle = dmn_handle_zero(); }
    result = radvs_engine_session_run_thread(cli->session->engine_session, thread_handle);
    if (result != RADVS_Result_Ok) { radvs_cli_print_result(cli, "continue", result); }
  } else if (str8_matchi(command, str8_lit("step"))) {
    DMN_Handle thread_handle = radvs_cli_last_stopped_thread_handle(cli);
    String8 argument = radvs_cli_next_token(&command_line);
    if (argument.size != 0 && !radvs_cli_parse_handle(argument, &thread_handle)) { thread_handle = dmn_handle_zero(); }
    result = radvs_engine_session_step_thread(cli->session->engine_session, thread_handle);
    if (result != RADVS_Result_Ok) { radvs_cli_print_result(cli, "step", result); }
  } else if (str8_matchi(command, str8_lit("break"))) {
    result = radvs_engine_session_break(cli->session->engine_session);
    if (result != RADVS_Result_Ok) { radvs_cli_print_result(cli, "break", result); }
  } else if (str8_matchi(command, str8_lit("bp"))) {
    U64 address = 0;
    if (radvs_cli_parse_u64(radvs_cli_next_token(&command_line), &address)) {
      RADVS_BreakpointID breakpoint_id = 0;
      RADVS_BreakpointSpec spec = { .address = address, .enabled = 1 };
      result = radvs_engine_session_alloc_breakpoint(cli->session->engine_session, &spec, &breakpoint_id);
      if (result == RADVS_Result_Ok) {
        MutexScope (cli->output_mutex) {
          radvs_cli_fprintf(stdout, "breakpoint id=%I64u address=0x%I64x\n", breakpoint_id, address);
          fflush(stdout);
        }
      } else {
        radvs_cli_print_result(cli, "bp", result);
      }
    } else {
      radvs_cli_print_result(cli, "bp", RADVS_Result_InvalidArgument);
    }
  } else if (str8_matchi(command, str8_lit("bp-enable")) ||
              str8_matchi(command, str8_lit("bp-disable"))) {
    U64 breakpoint_id = 0;
    if (radvs_cli_parse_u64(radvs_cli_next_token(&command_line), &breakpoint_id)) {
      B32 enabled = str8_matchi(command, str8_lit("bp-enable"));
      result = radvs_engine_session_set_breakpoint_enabled(cli->session->engine_session, breakpoint_id, enabled);
      if (result != RADVS_Result_Ok) { radvs_cli_print_result(cli, "breakpoint", result); }
    } else {
      radvs_cli_print_result(cli, "breakpoint", RADVS_Result_InvalidArgument);
    }
  } else if (str8_matchi(command, str8_lit("bp-delete"))) {
    U64 breakpoint_id = 0;
    if (radvs_cli_parse_u64(radvs_cli_next_token(&command_line), &breakpoint_id)) {
      result = radvs_engine_session_remove_breakpoint(cli->session->engine_session, breakpoint_id);
      if (result != RADVS_Result_Ok) { radvs_cli_print_result(cli, "bp-delete", result); }
    } else {
      radvs_cli_print_result(cli, "bp-delete", RADVS_Result_InvalidArgument);
    }
  } else if (str8_matchi(command, str8_lit("threads"))) {
    radvs_cli_list_threads(cli);
  } else if (str8_matchi(command, str8_lit("modules"))) {
    radvs_cli_list_modules(cli);
  } else if (str8_matchi(command, str8_lit("terminate"))) {
    DMN_Handle process_handle = {0};
    String8 argument = radvs_cli_next_token(&command_line);
    if (argument.size == 0) {
      result = radvs_engine_session_terminate(cli->session->engine_session, 0, 0);
    } else if (radvs_cli_parse_handle(argument, &process_handle)) {
      result = radvs_engine_session_terminate_process(cli->session->engine_session, process_handle);
    }
    if (result != RADVS_Result_Ok) { radvs_cli_print_result(cli, "terminate", result); }
  } else if (str8_matchi(command, str8_lit("wait"))) {
    U64 milliseconds = 0;
    if (radvs_cli_parse_u64(radvs_cli_next_token(&command_line), &milliseconds)) {
      Sleep((U32)Min(milliseconds, max_U32));
    } else {
      radvs_cli_print_result(cli, "wait", RADVS_Result_InvalidArgument);
    }
  } else if (str8_matchi(command, str8_lit("quit")) ||
              str8_matchi(command, str8_lit("exit"))) {
    return 0;
  } else {
    MutexScope (cli->output_mutex) {
      radvs_cli_fprintf(stderr, "unknown command: %S\n", command);
    }
  }
  return 1;
}

internal B32
radvs_cli_known_option(String8 option)
{
  return str8_matchi(option, str8_lit("exe")) ||
         str8_matchi(option, str8_lit("args")) ||
         str8_matchi(option, str8_lit("wdir")) ||
         str8_matchi(option, str8_lit("command")) ||
         str8_matchi(option, str8_lit("no-repl")) ||
         str8_matchi(option, str8_lit("help"));
}

internal String8
radvs_cli_option_value(String8 argument, String8 name)
{
  String8 option = argument;
  if (str8_match(str8_prefix(option, 2), str8_lit("--"), 0)) {
    option = str8_skip(option, 2);
  } else if (str8_match(str8_prefix(option, 1), str8_lit("-"), 0) ||
             str8_match(str8_prefix(option, 1), str8_lit("/"), 0)) {
    option = str8_skip(option, 1);
  } else {
    return (String8){0};
  }

  U64 colon = str8_find_needle(option, 0, str8_lit(":"), 0);
  U64 equals = str8_find_needle(option, 0, str8_lit("="), 0);
  U64 value_start = Min(colon, equals);
  if (value_start >= option.size || !str8_matchi(str8_prefix(option, value_start), name)) {
    return (String8){0};
  }
  return str8_skip(option, value_start + 1);
}

internal void
entry_point(CmdLine *cmdline)
{
  for EachNode(option, CmdLineOpt, cmdline->options.first) {
    if (!radvs_cli_known_option(option->string)) {
      radvs_cli_fprintf(stderr, "unknown option: %S\n", option->string);
      return;
    }
  }

  RADVS_CLI cli = {0};
  cli.exe = cmd_line_string(cmdline, str8_lit("exe"));
  cli.args = cmd_line_string(cmdline, str8_lit("args"));
  cli.wdir = cmd_line_string(cmdline, str8_lit("wdir"));
  if (cli.exe.size == 0 && cmdline->inputs.first != 0) {
    cli.exe = cmdline->inputs.first->string;
  }

  if (cmd_line_has_flag(cmdline, str8_lit("help"))) {
    radvs_cli_print_help(&cli);
    return;
  }

  RADVS_Result result = radvs_session_create(&cli.session);
  if (result != RADVS_Result_Ok) {
    radvs_cli_fprintf(stderr, "session: %S\n", radvs_cli_result_string(result));
    return;
  }
  cli.state_mutex = mutex_alloc();
  cli.output_mutex = mutex_alloc();
  cli.event_thread = thread_launch(radvs_cli_event_worker, &cli);
  if (cli.event_thread.u64[0] == 0) {
    mutex_release(cli.output_mutex);
    mutex_release(cli.state_mutex);
    radvs_session_destroy(cli.session);
    return;
  }

  B32 keep_running = 1;
  if (cli.exe.size != 0) {
    result = radvs_cli_launch(&cli, cli.exe);
    if (result != RADVS_Result_Ok) {
      radvs_cli_print_result(&cli, "launch", result);
      keep_running = 0;
    }
  }
  Temp scratch = scratch_begin(0, 0);
  for (U64 argument_index = 1; keep_running && argument_index < cmdline->argc; argument_index += 1) {
    String8 command_argument = radvs_cli_option_value(str8_cstring((U8 *)cmdline->argv[argument_index]), str8_lit("command"));
    if (command_argument.size != 0) {
      U8 splits[] = { ',' };
      String8List commands = str8_split(scratch.arena, command_argument, splits, ArrayCount(splits), 0);
      for EachNode(command, String8Node, commands.first) {
        if (!keep_running) {
          break;
        }
        keep_running = radvs_cli_dispatch_command(&cli, command->string);
      }
    }
  }
  scratch_end(scratch);

  if (keep_running && !cmd_line_has_flag(cmdline, str8_lit("no-repl"))) {
    char line_buffer[4096] = {0};
    MutexScope (cli.output_mutex) {
      radvs_cli_print_help(&cli);
    }
    for (;;) {
      MutexScope (cli.output_mutex) {
        radvs_cli_fprintf(stdout, "radvs> ");
        fflush(stdout);
      }
      if (fgets(line_buffer, sizeof(line_buffer), stdin) == 0 ||
          !radvs_cli_dispatch_command(&cli, str8((U8 *)line_buffer, cstring8_length((U8 *)line_buffer)))) {
        break;
      }
    }
  }

  radvs_engine_session_close_event_wait(cli.session->engine_session);
  thread_join(cli.event_thread, max_U64);
  mutex_release(cli.output_mutex);
  mutex_release(cli.state_mutex);
  radvs_session_destroy(cli.session);
}
