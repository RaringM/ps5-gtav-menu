#pragma once

#include "gtavmenu/abi.h"

// Runtime configuration: seed a GtavMenuInit from compile-time defaults, then optionally
// load/save the on-disk runtime.cfg (an INI under /data/GTAVMenu). NOTE: the live
// feature-menu build is compiled with GTAV_MENU_SKIP_RUNTIME_CONFIG, so these knobs are
// inert there -- tune that build via compile-time constants or the mailbox instead.

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_MENU_DEFAULT_DIR "/data/GTAVMenu"
#define GTAV_MENU_DEFAULT_LOG "/data/GTAVMenu/gtav-menu.log"
#define GTAV_MENU_DEFAULT_CONFIG "/data/GTAVMenu/runtime.cfg"

// Populate `init` with the built-in defaults (native addresses, cadences, flags).
void gtav_runtime_init_defaults(GtavMenuInit* init);
// Merge the runtime.cfg at `path` into `init`. Returns 0 on success, -1 if `init` is NULL
// or the file cannot be opened (a missing file simply leaves `init` at its defaults).
int gtav_runtime_config_load(const char* path, GtavMenuInit* init);
// Write `init` out to the runtime.cfg at `path`. Returns 0 on success, -1 on a NULL
// argument or an unwritable path.
int gtav_runtime_config_write(const char* path, const GtavMenuInit* init);

#ifdef __cplusplus
}
#endif
