// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)
//
// The RADVS bridge is an in-process Visual Studio component, not a standalone
// RAD executable. This translation unit assembles only the layers it embeds.
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
#include "radvs/engine/radvs_request.h"
#include "radvs/engine/radvs_demon.h"
#include "radvs/engine/radvs_tasker.h"
#include "radvs/engine/radvs_symbol_service.h"

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
#include "radvs/engine/radvs.h"

#include "radvs/engine/radvs.c"
#include "radvs/engine/radvs_request.c"
#include "radvs/engine/radvs_demon.c"
#include "radvs/engine/radvs_tasker.c"
#include "radvs/engine/radvs_symbol_service.c"
#include "radvs/engine/radvs_format.c"
#include "radvs/engine/radvs_engine.c"
#include "radvs/engine/radvs_bridge.c"

