// Copyright (c) 2024 Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: Handle Type Functions (Helpers, Implemented Once)

internal OS_Handle
os_handle_zero(void)
{
  OS_Handle handle = {0};
  return handle;
}

internal B32
os_handle_match(OS_Handle a, OS_Handle b)
{
  return a.u64[0] == b.u64[0];
}

internal void
os_handle_list_push(Arena *arena, OS_HandleList *handles, OS_Handle handle)
{
  OS_HandleNode *n = push_array(arena, OS_HandleNode, 1);
  n->v = handle;
  SLLQueuePush(handles->first, handles->last, n);
  handles->count += 1;
}

internal OS_HandleArray
os_handle_array_from_list(Arena *arena, OS_HandleList *list)
{
  OS_HandleArray result = {0};
  result.count = list->count;
  result.v = push_array_no_zero(arena, OS_Handle, result.count);
  U64 idx = 0;
  for(OS_HandleNode *n = list->first; n != 0; n = n->next, idx += 1)
  {
    result.v[idx] = n->v;
  }
  return result;
}

////////////////////////////////
//~ rjf: Command Line Argc/Argv Helper (Helper, Implemented Once)

internal String8List
os_string_list_from_argcv(Arena *arena, int argc, char **argv)
{
  String8List result = {0};
  for(int i = 0; i < argc; i += 1)
  {
    String8 str = str8_cstring(argv[i]);
    str8_list_push(arena, &result, str);
  }
  return result;
}

////////////////////////////////
//~ rjf: Filesystem Helpers (Helpers, Implemented Once)

internal String8
os_data_from_file_path(Arena *arena, String8 path)
{
  OS_Handle file = os_file_open(OS_AccessFlag_Read|OS_AccessFlag_ShareRead, path);
  FileProperties props = os_properties_from_file(file);
  String8 data = os_string_from_file_range(arena, file, r1u64(0, props.size));
  os_file_close(file);
  return data;
}

internal B32
os_write_data_to_file_path(String8 path, String8 data)
{
  B32 good = 0;
  OS_Handle file = os_file_open(OS_AccessFlag_Write, path);
  if(!os_handle_match(file, os_handle_zero()))
  {
    good = 1;
    os_file_write(file, r1u64(0, data.size), data.str);
    os_file_close(file);
  }
  return good;
}

internal B32
os_write_data_list_to_file_path(String8 path, String8List list)
{
  B32 good = 0;
  OS_Handle file = os_file_open(OS_AccessFlag_Write, path);
  if(!os_handle_match(file, os_handle_zero()))
  {
    good = 1;
    U64 off = 0;
    for(String8Node *n = list.first; n != 0; n = n->next)
    {
      os_file_write(file, r1u64(off, off+n->string.size), n->string.str);
      off += n->string.size;
    }
    os_file_close(file);
  }
  return good;
}

internal B32
os_append_data_to_file_path(String8 path, String8 data)
{
  B32 good = 0;
  if(data.size != 0)
  {
    OS_Handle file = os_file_open(OS_AccessFlag_Write|OS_AccessFlag_Append, path);
    if(!os_handle_match(file, os_handle_zero()))
    {
      good = 1;
      U64 pos = os_properties_from_file(file).size;
      os_file_write(file, r1u64(pos, pos+data.size), data.str);
      os_file_close(file);
    }
  }
  return good;
}

internal OS_FileID
os_id_from_file_path(String8 path)
{
  OS_Handle file = os_file_open(OS_AccessFlag_Read|OS_AccessFlag_ShareRead, path);
  OS_FileID id = os_id_from_file(file);
  os_file_close(file);
  return id;
}

internal S64
os_file_id_compare(OS_FileID a, OS_FileID b)
{
  S64 cmp = MemoryCompare((void*)&a.v[0], (void*)&b.v[0], sizeof(a.v));
  return cmp;
}

internal String8
os_string_from_file_range(Arena *arena, OS_Handle file, Rng1U64 range)
{
  U64 pre_pos = arena_pos(arena);
  String8 result;
  result.size = dim_1u64(range);
  result.str = push_array_no_zero(arena, U8, result.size);
  U64 actual_read_size = os_file_read(file, range, result.str);
  if(actual_read_size < result.size)
  {
    arena_pop_to(arena, pre_pos + actual_read_size);
    result.size = actual_read_size;
  }
  return result;
}

////////////////////////////////
//~ rjf: GUID Helpers (Helpers, Implemented Once)

internal String8
os_string_from_guid(Arena *arena, OS_Guid guid)
{
  String8 result = push_str8f(arena, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                              guid.data1,
                              guid.data2,
                              guid.data3,
                              guid.data4[0],
                              guid.data4[1],
                              guid.data4[2],
                              guid.data4[3],
                              guid.data4[4],
                              guid.data4[5],
                              guid.data4[6],
                              guid.data4[7]);
  return result;
}

internal B32
os_try_guid_from_string(String8 string, OS_Guid *guid_out)
{
  Temp scratch = scratch_begin(0,0);
  B32 is_parsed = 0;
  String8List list = str8_split_by_string_chars(scratch.arena, string, str8_lit("-"), StringSplitFlag_KeepEmpties);
  if (list.node_count == 5) {
    String8 data1_str    = list.first->string;
    String8 data2_str    = list.first->next->string;
    String8 data3_str    = list.first->next->next->string;
    String8 data4_hi_str = list.first->next->next->next->string;
    String8 data4_lo_str = list.first->next->next->next->next->string;
    if (str8_is_integer(data1_str, 16) && 
        str8_is_integer(data2_str, 16) &&
        str8_is_integer(data3_str, 16) &&
        str8_is_integer(data4_hi_str, 16) &&
        str8_is_integer(data4_lo_str, 16)) {
      U64 data1    = u64_from_str8(data1_str, 16);
      U64 data2    = u64_from_str8(data2_str, 16);
      U64 data3    = u64_from_str8(data3_str, 16);
      U64 data4_hi = u64_from_str8(data4_hi_str, 16);
      U64 data4_lo = u64_from_str8(data4_lo_str, 16);
      if (data1 <= max_U32 &&
          data2 <= max_U16 &&
          data3 <= max_U16 &&
          data4_hi <= max_U16 &&
          data4_lo <= 0xffffffffffff) {
        guid_out->data1 = (U32)data1;
        guid_out->data2 = (U16)data2;
        guid_out->data3 = (U16)data3;
        U64 data4 = (data4_hi << 48) | data4_lo;
        MemoryCopy(&guid_out->data4[0], &data4, sizeof(data4));
        is_parsed = 1;
      }
    }
  }
  scratch_end(scratch);
  return is_parsed;
}

internal OS_Guid
os_guid_from_string(String8 string)
{
  OS_Guid guid = {0};
  os_try_guid_from_string(string, &guid);
  return guid;
}
