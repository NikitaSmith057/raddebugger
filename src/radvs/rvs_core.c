// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////

#include "radvs/rvs_core.h"

////////////////////////////////

internal RVS_EventNode *
rvs_event_list_push(Arena *arena, RVS_EventList *list, RVS_Event v)
{
  RVS_EventNode *n = push_array(arena, RVS_EventNode, 1);
  n->v = v;
  SLLQueuePush(list->first, list->last, n);
  list->count += 1;
  return n;
}

internal void
rvs_run_copy(Arena *arena, RVS_RunInfo *dst, RVS_RunInfo *src)
{
  switch (src->target_kind) {
  case RVS_RunTargetKind_All: break;
  case RVS_RunTargetKind_Programs: {
    dst->programs.v     = push_array(arena, RVS_ProgramID, src->programs.count);
    dst->programs.count = src->programs.count;
    MemoryCopyTyped(dst->programs.v, src->programs.v, src->programs.count);
  } break;
  default: InvalidPath;
  }
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
  default: break;
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

