// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#include "radvs/radvs_symbol_service.h"
#include "radvs/radvs_tasker.h"
#include "rdi/rdi_local.h"

struct RADVS_SymbolTicket
{
  RADVS_SymbolTicket  *next;
  RADVS_SymbolService *service;
  Arena               *arena;
  RADVS_SymbolRequest  request;
  RADVS_Task           task;
  String8              rdi_data;
  RDI_Parsed           rdi;
  RADVS_SymbolTicketState state;
  RADVS_Result            submission_result;
  U64                     ref_count;
};

struct RADVS_SymbolService
{
  Arena              *arena;
  Mutex               mutex;
  RADVS_SymbolTicket *all_tickets;
  B32                 shutdown_requested;
};

internal B32
radvs_symbol_path_has_extension(String8 path, String8 extension)
{
  return str8_matchi(str8_skip_last_dot(str8_skip_last_slash(path)), extension);
}

internal String8
radvs_symbol_rdi_path_from_ticket(Arena *arena, RADVS_SymbolTicket *ticket)
{
  String8 candidates[6] = {0};
  U64 candidate_count = 0;
  String8 module_path = ticket->request.module_path;
  String8 debug_info_path = ticket->request.debug_info_path;
  if (radvs_symbol_path_has_extension(debug_info_path, str8_lit("rdi"))) {
    if (path_style_from_str8(debug_info_path) == PathStyle_Relative) {
      candidates[candidate_count++] = push_str8f(arena, "%S/%S", str8_chop_last_slash(module_path), debug_info_path);
    }
    candidates[candidate_count++] = debug_info_path;
  }
  if (radvs_symbol_path_has_extension(debug_info_path, str8_lit("pdb"))) {
    String8 converted_path = push_str8f(arena, "%S.rdi", str8_chop_last_dot(debug_info_path));
    if (path_style_from_str8(converted_path) == PathStyle_Relative) {
      candidates[candidate_count++] = push_str8f(arena, "%S/%S", str8_chop_last_slash(module_path), converted_path);
    }
    candidates[candidate_count++] = converted_path;
  }
  candidates[candidate_count++] = push_str8f(arena, "%S.rdi", str8_chop_last_dot(module_path));
  candidates[candidate_count++] = push_str8f(arena, "%S.rdi", module_path);

  for EachIndex(candidate_idx, candidate_count) {
    String8 candidate = candidates[candidate_idx];
    FileProperties properties = properties_from_file_path(candidate);
    if (properties.modified != 0 && properties.size != 0) {
      return path_normalized_from_string(arena, candidate);
    }
  }
  return str8_zero();
}

internal RADVS_Result
radvs_symbol_ticket_load_rdi(RADVS_SymbolTicket *ticket, String8 rdi_path)
{
  ticket->rdi_data = data_from_file_path(ticket->arena, rdi_path);
  RDI_ParseStatus parse_status = rdi_parse(ticket->rdi_data.str, ticket->rdi_data.size, &ticket->rdi);
  if (parse_status == RDI_ParseStatus_Good) {
    U64 decompressed_size = rdi_decompressed_size_from_parsed(&ticket->rdi);
    if (decompressed_size > ticket->rdi.raw_data_size) {
      U8 *decompressed_data = push_array_no_zero(ticket->arena, U8, decompressed_size);
      rdi_decompress_parsed(decompressed_data, decompressed_size, &ticket->rdi);
      parse_status = rdi_parse(decompressed_data, decompressed_size, &ticket->rdi);
    }
  }
  return parse_status == RDI_ParseStatus_Good ? RADVS_Result_Ok : RADVS_Result_InvalidArgument;
}

internal RADVS_Result
radvs_symbol_ticket_start_pdb_conversion(RADVS_SymbolTicket *ticket)
{
  (void)ticket;
  // TODO(clanker): Start the PDB-to-RDI converter and retain its completion token on this symbol ticket.
  return RADVS_Result_NotImplemented;
}

internal RADVS_Result
radvs_symbol_ticket_task_proc(void *user_data)
{
  RADVS_SymbolTicket *ticket = user_data;
  MutexScope (ticket->service->mutex) {
    ticket->state = RADVS_SymbolTicketState_Loading;
  }

  Temp scratch = scratch_begin(0, 0);
  String8 rdi_path = radvs_symbol_rdi_path_from_ticket(scratch.arena, ticket);
  RADVS_Result result = RADVS_Result_InvalidArgument;
  if (rdi_path.size != 0) {
    result = radvs_symbol_ticket_load_rdi(ticket, rdi_path);
  } else if (radvs_symbol_path_has_extension(ticket->request.debug_info_path, str8_lit("pdb"))) {
    result = radvs_symbol_ticket_start_pdb_conversion(ticket);
  }
  scratch_end(scratch);
  return result;
}

internal RADVS_SymbolTicket *
radvs_symbol_ticket_from_token_locked(RADVS_SymbolService *service, U64 token)
{
  for EachNode(ticket, RADVS_SymbolTicket, service->all_tickets) {
    if (radvs_tasker_token(&ticket->task) == token) {
      return ticket;
    }
  }
  return 0;
}

internal RADVS_SymbolTicketStatus
radvs_symbol_ticket_status_from_task(RADVS_SymbolTicket *ticket)
{
  RADVS_SymbolTicketStatus status = {0};
  if (ticket == 0 || ticket->service == 0) {
    status.result = RADVS_Result_InvalidArgument;
    return status;
  }
  if (ticket->submission_result != RADVS_Result_Ok) {
    status.state = RADVS_SymbolTicketState_Failed;
    status.result = ticket->submission_result;
    return status;
  }

  RADVS_TaskStatus task_status = radvs_tasker_status(&ticket->task);
  if (!task_status.completed) {
    MutexScope (ticket->service->mutex) {
      status.state = ticket->state;
    }
    status.result = task_status.result;
    return status;
  }

  status.result = task_status.result;
  if (task_status.result == RADVS_Result_Ok) {
    status.state = RADVS_SymbolTicketState_Ready;
  } else if (task_status.result == RADVS_Result_Timeout) {
    status.state = RADVS_SymbolTicketState_TimedOut;
  } else if (task_status.result == RADVS_Result_Busy) {
    status.state = RADVS_SymbolTicketState_Cancelled;
  } else {
    status.state = RADVS_SymbolTicketState_Failed;
  }
  return status;
}

RADVS_Result
radvs_symbol_service_init(RADVS_SymbolService **out_service)
{
  if (out_service == 0) {
    return RADVS_Result_InvalidArgument;
  }
  *out_service = 0;
  Arena *arena = arena_alloc(.reserve_size = MB(1), .commit_size = KB(64));
  RADVS_SymbolService *service = push_array(arena, RADVS_SymbolService, 1);
  service->arena = arena;
  service->mutex = mutex_alloc();
  *out_service = service;
  return RADVS_Result_Ok;
}

void
radvs_symbol_service_release(RADVS_SymbolService *service)
{
  if (service != 0) {
    for (RADVS_SymbolTicket *ticket = service->all_tickets; ticket != 0;) {
      RADVS_SymbolTicket *next = ticket->next;
      radvs_task_release(&ticket->task);
      arena_release(ticket->arena);
      ticket = next;
    }
    mutex_release(service->mutex);
    arena_release(service->arena);
  }
}

void
radvs_symbol_service_prepare_release(RADVS_SymbolService *service)
{
  if (service != 0) {
    MutexScope (service->mutex) {
      service->shutdown_requested = 1;
    }
    for EachNode(ticket, RADVS_SymbolTicket, service->all_tickets) {
      radvs_tasker_cancel(&ticket->task);
    }
  }
}

RADVS_Result
radvs_symbol_service_configure(RADVS_SymbolService *service, RADVS_SymbolServiceSettings settings)
{
  if (service == 0) {
    return RADVS_Result_InvalidArgument;
  }
  (void)settings;
  // TODO(clanker): Configure persistent RDI caching, symbol-server lookup, and download policy.
  NotImplemented;
  return RADVS_Result_NotImplemented;
}

RADVS_Result
radvs_symbol_service_collect_cache(RADVS_SymbolService *service, U64 target_size)
{
  if (service == 0) {
    return RADVS_Result_InvalidArgument;
  }
  (void)target_size;
  // TODO(clanker): Evict unused symbol artifacts from the persistent and in-memory caches.
  NotImplemented;
  return RADVS_Result_NotImplemented;
}

RADVS_Result
radvs_symbol_ticket_submit(RADVS_SymbolService *service, RADVS_SymbolRequest request, RADVS_SymbolTicket **out_ticket)
{
  if (service == 0 || out_ticket == 0 || request.module_path.size == 0) {
    return RADVS_Result_InvalidArgument;
  }

  RADVS_SymbolTicket *ticket = 0;
  RADVS_Result result = RADVS_Result_Busy;
  MutexScope (service->mutex) {
    if (!service->shutdown_requested) {
      ticket = push_array(service->arena, RADVS_SymbolTicket, 1);
      ticket->service = service;
      ticket->arena = arena_alloc(.reserve_size = MB(1), .commit_size = KB(64));
      ticket->request = request;
      ticket->request.module_path = push_str8_copy(ticket->arena, request.module_path);
      ticket->request.debug_info_path = push_str8_copy(ticket->arena, request.debug_info_path);
      ticket->request.endt_us = request.endt_us ? request.endt_us : now_time_us() + 30000000;
      ticket->state = RADVS_SymbolTicketState_Queued;
      ticket->submission_result = RADVS_Result_Busy;
      ticket->ref_count = 1;
      radvs_task_init(&ticket->task, radvs_symbol_ticket_task_proc, ticket, ticket->request.endt_us);
      ticket->next = service->all_tickets;
      service->all_tickets = ticket;
      ticket->submission_result = radvs_tasker_submit(&ticket->task);
      if (ticket->submission_result != RADVS_Result_Ok) {
        ticket->state = RADVS_SymbolTicketState_Failed;
      }
      result = ticket->submission_result;
    }
  }
  *out_ticket = ticket;
  return result;
}

void
radvs_symbol_ticket_cancel(RADVS_SymbolTicket *ticket)
{
  if (ticket == 0 || ticket->service == 0) {
    return;
  }
  MutexScope (ticket->service->mutex) {
    ticket->state = RADVS_SymbolTicketState_Cancelled;
  }
  radvs_tasker_cancel(&ticket->task);
}

void
radvs_symbol_ticket_release(RADVS_SymbolTicket *ticket)
{
  if (ticket == 0 || ticket->service == 0) {
    return;
  }
  MutexScope (ticket->service->mutex) {
    if (ticket->ref_count > 0) {
      ticket->ref_count -= 1;
    }
  }
}

U64
radvs_symbol_ticket_token(RADVS_SymbolTicket *ticket)
{
  return ticket != 0 ? radvs_tasker_token(&ticket->task) : 0;
}

RADVS_Result
radvs_symbol_ticket_wait(RADVS_SymbolTicket *ticket)
{
  if (ticket == 0 || ticket->service == 0) {
    return RADVS_Result_InvalidArgument;
  }
  if (ticket->submission_result != RADVS_Result_Ok) {
    return ticket->submission_result;
  }
  return radvs_tasker_wait(&ticket->task);
}

RADVS_SymbolTicketStatus
radvs_symbol_ticket_status(RADVS_SymbolTicket *ticket)
{
  return radvs_symbol_ticket_status_from_task(ticket);
}

RADVS_Result
radvs_symbol_ticket_location_from_voff(RADVS_SymbolTicket *ticket, U64 voff, RADVS_SymbolLocation *out_location)
{
  if (ticket == 0 || ticket->service == 0 || out_location == 0) {
    return RADVS_Result_InvalidArgument;
  }
  MemoryZeroStruct(out_location);
  RADVS_SymbolTicketStatus status = radvs_symbol_ticket_status_from_task(ticket);
  if (status.state != RADVS_SymbolTicketState_Ready) {
    return status.result;
  }

  RADVS_Result result = RADVS_Result_Ok;
  MutexScope (ticket->service->mutex) {
    RDI_Line line = rdi_line_from_voff(&ticket->rdi, voff);
    RDI_SourceFile *source_file = rdi_source_file_from_line(&ticket->rdi, &line);
    U64 source_path_size = 0;
    U8 *source_path = rdi_normal_path_from_source_file(&ticket->rdi, source_file, &source_path_size);
    if (line.line_num != 0 && source_path_size != 0) {
      out_location->voff_first = voff;
      out_location->voff_opl = voff + 1;
      out_location->line = line.line_num;
      out_location->column = 1;
      out_location->source_path = str8(source_path, source_path_size);
    }
  }
  return result;
}

RADVS_Result
radvs_symbol_ticket_function_voff_from_name(RADVS_SymbolTicket *ticket, String8 name, U64 *out_voff)
{
  (void)ticket;
  (void)name;
  (void)out_voff;
  // TODO(clanker): Resolve RDI procedure names for function breakpoint binding.
  NotImplemented;
  return RADVS_Result_NotImplemented;
}

RADVS_Result
radvs_symbol_ticket_voff_from_source_line(RADVS_SymbolTicket *ticket, String8 source_path, U32 line, U64 *out_voff)
{
  (void)ticket;
  (void)source_path;
  (void)line;
  (void)out_voff;
  // TODO(clanker): Resolve RDI source file and line records for source breakpoint binding.
  NotImplemented;
  return RADVS_Result_NotImplemented;
}

RADVS_Result
radvs_symbol_service_signal_converter_complete(RADVS_SymbolService *service, U64 ticket_token, String8 rdi_path, RADVS_Result converter_result)
{
  if (service == 0 || ticket_token == 0) {
    return RADVS_Result_InvalidArgument;
  }

  RADVS_SymbolTicket *ticket = 0;
  RADVS_Result result = RADVS_Result_Ok;
  MutexScope (service->mutex) {
    if (service->shutdown_requested) {
      result = RADVS_Result_Busy;
    } else {
      ticket = radvs_symbol_ticket_from_token_locked(service, ticket_token);
    }
  }
  if (result != RADVS_Result_Ok) {
    return result;
  }
  if (ticket == 0) {
    return RADVS_Result_InvalidArgument;
  }

  result = converter_result;
  if (result == RADVS_Result_Ok) {
    result = radvs_symbol_ticket_load_rdi(ticket, rdi_path);
  }
  radvs_tasker_complete(&ticket->task, result);
  return result;
}

RADVS_Result
radvs_symbol_service_signal_converter_timeout(RADVS_SymbolService *service, U64 ticket_token)
{
  if (service == 0 || ticket_token == 0) {
    return RADVS_Result_InvalidArgument;
  }

  RADVS_Result result = RADVS_Result_InvalidArgument;
  MutexScope (service->mutex) {
    RADVS_SymbolTicket *ticket = !service->shutdown_requested ? radvs_symbol_ticket_from_token_locked(service, ticket_token) : 0;
    if (ticket != 0) {
      result = radvs_tasker_complete(&ticket->task, RADVS_Result_Timeout);
    } else if (service->shutdown_requested) {
      result = RADVS_Result_Busy;
    }
  }
  return result;
}
