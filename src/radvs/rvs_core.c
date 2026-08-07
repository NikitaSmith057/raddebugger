// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#include "radvs/rvs_core.h"

internal void
rvs_run_copy(Arena *arena, RVS_RunInfo *dst, RVS_RunInfo *src)
{
  dst->programs       = push_array(arena, RVS_ProgramID, src->programs_count);
  dst->programs_count = src->programs_count;
  MemoryCopyTyped(dst->programs, src->programs, src->programs_count);
}

internal void
rvs_command_copy(Arena *arena, RVS_Command *dst, RVS_Command *src)
{
  *dst = *src;
  switch (src->kind) {
  case RVS_CommandKind_Launch: { dst->launch_params = *process_launch_params_copy(arena, &src->launch_params); } break;
  case RVS_CommandKind_Run:    { rvs_run_copy(arena, &dst->run, &src->run);     } break;
  case RVS_CommandKind_Pause:  { rvs_run_copy(arena, &dst->pause, &src->pause); } break;
  case RVS_CommandKind_Null:  break;
  default: InvalidPath;
  }
}

internal String8
rvs_string_from_command_kind(RVS_CommandKind v)
{
  switch (v) {
#define X(id, ...) case RVS_CommandKind_##id: return str8_lit(Stringify(id));
  RVS_COMMAND_XLIST
#undef X
  default: break;
  }
  return str8_zero();
}

internal String8
rvs_help_from_command_kind(RVS_CommandKind v)
{
  switch (v) {
  case RVS_CommandKind_Null: break;
#define X(id, help, ...) case RVS_CommandKind_##id: return str8_lit(help);
  RVS_COMMAND_XLIST
#undef X
  }
  return str8_zero();
}

internal RVS_CommandKind
rvs_command_kind_from_string(String8 v)
{
#define X(id, ...) if (str8_matchi(str8_lit(Stringify(id)), v)) return RVS_CommandKind_##id;
  RVS_COMMAND_XLIST
#undef X
  return RVS_CommandKind_Null;
}

