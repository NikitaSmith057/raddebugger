// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

#include "radvs/radvs_format.h"

typedef struct RADVS_SymbolTicket RADVS_SymbolTicket;
typedef struct RADVS_SymbolService RADVS_SymbolService;

typedef enum
{
  RADVS_SymbolTicketState_Null,
  RADVS_SymbolTicketState_Queued,
  RADVS_SymbolTicketState_Loading,
  RADVS_SymbolTicketState_Ready,
  RADVS_SymbolTicketState_Failed,
  RADVS_SymbolTicketState_TimedOut,
  RADVS_SymbolTicketState_Cancelled,
} RADVS_SymbolTicketState;

typedef struct
{
  String8  module_path;
  String8  debug_info_path;
  Guid     debug_info_guid;
  U64      debug_info_timestamp;
  U64      debug_info_age;
  U64      endt_us;
} RADVS_SymbolRequest;

typedef struct
{
  RADVS_SymbolTicketState state;
  RADVS_Result            result;
} RADVS_SymbolTicketStatus;

typedef struct
{
  U64      voff_first;
  U64      voff_opl;
  U32      line;
  U32      column;
  String8  source_path;
} RADVS_SymbolLocation;

typedef struct
{
  String8  rdi_cache_path;
  String8  symbol_server_path;
  U32      allow_downloads;
} RADVS_SymbolServiceSettings;

RADVS_Result radvs_symbol_service_init(RADVS_SymbolService **out_service);
// Rejects new work and external completion before the bridge stops the Tasker.
void         radvs_symbol_service_prepare_release(RADVS_SymbolService *service);
// The bridge stops the Tasker before releasing symbol tickets.
void         radvs_symbol_service_release(RADVS_SymbolService *service);

// Future persistent-cache and symbol-server configuration surface.
RADVS_Result radvs_symbol_service_configure(RADVS_SymbolService *service, RADVS_SymbolServiceSettings settings);
RADVS_Result radvs_symbol_service_collect_cache(RADVS_SymbolService *service, U64 target_size);

// Module symbol loading and completion lifecycle.
RADVS_Result radvs_symbol_ticket_submit(RADVS_SymbolService *service, RADVS_SymbolRequest request, RADVS_SymbolTicket **out_ticket);
void         radvs_symbol_ticket_cancel(RADVS_SymbolTicket *ticket);
void         radvs_symbol_ticket_release(RADVS_SymbolTicket *ticket);
U64          radvs_symbol_ticket_token(RADVS_SymbolTicket *ticket);
RADVS_Result radvs_symbol_ticket_wait(RADVS_SymbolTicket *ticket);
RADVS_SymbolTicketStatus radvs_symbol_ticket_status(RADVS_SymbolTicket *ticket);

// Immutable RDI artifact queries. Returned strings remain valid while the ticket
// is retained by an engine module.
RADVS_Result radvs_symbol_ticket_location_from_voff(RADVS_SymbolTicket *ticket, U64 voff, RADVS_SymbolLocation *out_location);

// Future breakpoint-binding query surface.
RADVS_Result radvs_symbol_ticket_function_voff_from_name(RADVS_SymbolTicket *ticket, String8 name, U64 *out_voff);
RADVS_Result radvs_symbol_ticket_voff_from_source_line(RADVS_SymbolTicket *ticket, String8 source_path, U32 line, U64 *out_voff);

// Completion ingress for a future external PDB-to-RDI converter. The token is
// process-neutral and can be passed to a converter process or signal transport.
RADVS_Result radvs_symbol_service_signal_converter_complete(RADVS_SymbolService *service, U64 ticket_token, String8 rdi_path, RADVS_Result converter_result);
RADVS_Result radvs_symbol_service_signal_converter_timeout(RADVS_SymbolService *service, U64 ticket_token);
