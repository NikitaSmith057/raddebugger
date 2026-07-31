// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

internal int
radvs_fprintf(FILE *f, char *fmt, ...)
{
  static U64 is_inited;
  static FILE *debug_file = 0;
  U64 was_inited = ins_atomic_u64_eval_cond_assign(&is_inited, 1, 0);
  if (was_inited == 0) {
    debug_file = fopen("e:/devel/raddebugger/build/radvs_log.txt", "w");
    is_inited = 1;
  } else if (was_inited == 1) {
    for (; is_inited != 1 ;) { sleep_ms(0); }
  }
  f = debug_file;

  va_list args;
  va_start(args, fmt);
  Temp scratch = scratch_begin(0,0);
  String8 string = str8fv(scratch.arena, fmt, args);
  int result = fprintf(f, "%.*s\n", str8_varg(string));
  fflush(f);
  _commit(_fileno(f));
  scratch_end(scratch);
  va_end(args);
  return result;
}


