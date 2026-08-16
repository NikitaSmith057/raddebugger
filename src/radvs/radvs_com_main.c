// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)
//
// Native runtime shared by the AD7 bridge DLL and the command-line client.
//

#define BUILD_TITLE "RAD VS Debugger"
#define DMN_INIT_MANUAL     1
#define RADDBG_MARKUP_STUBS 1

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
#include "radvs/radvs_request.h"
#include "radvs/radvs_demon.h"
#include "radvs/radvs_tasker.h"
#include "radvs/radvs_symbol_service.h"

#include "base/base_inc.c"
#include "x64/x64.c"
#include "win32/win32_inc.c"
#include "coff/coff.c"
#include "coff/coff_parse.c"
#include "pe/pe.c"
#include "arch/arch_inc.c"
#include "demon/demon_inc.c"
#include "linker/hash_table.c"
#include "rdi/rdi_local.c"
#include "radvs/radvs.h"

#include "radvs/radvs.c"
#include "radvs/radvs_request.c"
#include "radvs/radvs_demon.c"
#include "radvs/radvs_tasker.c"
#include "radvs/radvs_symbol_service.c"
#include "radvs/radvs_format.c"
#include "radvs/radvs_engine.c"
#include "radvs/radvs_com_core.c"

#if BUILD_DLL_INTERFACE
# include "radvs/radvs_ad7_bridge.c"
#endif
