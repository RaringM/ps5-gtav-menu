// Pure, host-testable half of the process-control backend.
//
// Everything here is free of PS5 SDK dependencies (no ps5/kernel.h, no ptrace),
// so it compiles with the host cc and is unit-tested in tests/. It holds the
// constants and lookup tables the SDK glue (proc_backend_sdk.c) consumes: the
// ucred elevation plan and the symbol-name mapping.

#include "gtavmenu/proc_backend.h"

#include <string.h>

// Proven etaHEN loader values (src/etahen_loader/utils.cpp). Applied through the
// SDK's kernel_set_ucred_* helpers rather than raw offset writes.
#define GTAV_PROC_ELEVATION_AUTHID 0x4801000000000013ULL
// sce attribute bitmap is a fixed-width kernel field (kernel_set_ucred_attrs takes
// uint8_t[32]); the unsandbox flag is bit 0x80 of byte 0, the rest cleared.
#define GTAV_PROC_ELEVATION_ATTRS_BYTE0 0x80

int gtav_proc_elevation_plan(GtavProcElevation* out) {
  if (out == NULL) {
    return -1;
  }
  out->authid = GTAV_PROC_ELEVATION_AUTHID;
  memset(out->caps, 0xff, sizeof(out->caps));
  memset(out->attrs, 0, sizeof(out->attrs));
  out->attrs[0] = GTAV_PROC_ELEVATION_ATTRS_BYTE0;
  out->relocate_root = 1;
  return 0;
}

int gtav_proc_symbol_name(GtavProcSym sym, const char** lib_out, const char** sym_out) {
  const char* lib;
  const char* name;
  // Lib basenames carry the on-console module suffix: kernel_dynlib_handle() matches
  // the trailing path component exactly (the char before it must be '/'), so a bare
  // "libkernel" never matches "/.../libkernel.sprx". The SDK backend also tries the
  // libkernel_web/_sys variants at resolve time.
  switch (sym) {
    case GTAV_PROC_SYM_SCE_PAD_READ_STATE:
      lib = "libScePad.sprx";
      name = "scePadReadState";
      break;
    case GTAV_PROC_SYM_LOAD_START_MODULE:
      lib = "libkernel.sprx";
      name = "sceKernelLoadStartModule";
      break;
    case GTAV_PROC_SYM_DLSYM:
      lib = "libkernel.sprx";
      name = "sceKernelDlsym";
      break;
    case GTAV_PROC_SYM_MMAP:
      lib = "libkernel.sprx";
      name = "mmap";
      break;
    case GTAV_PROC_SYM_PTHREAD_CREATE:
      lib = "libkernel.sprx";
      name = "scePthreadCreate";
      break;
    default:
      return -1;
  }
  if (lib_out != NULL) {
    *lib_out = lib;
  }
  if (sym_out != NULL) {
    *sym_out = name;
  }
  return 0;
}
