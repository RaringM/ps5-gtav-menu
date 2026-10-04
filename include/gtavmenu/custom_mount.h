#pragma once

// Make the custom-asset root visible inside GTA's sandbox (see custom_assets.h). The same
// approach BOx-GSC-Injector's maps runtime uses for Black Ops III.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 1 when `name` is a sandbox directory of `title` (<TITLE>_<digits>), storing the numeric suffix
// in *index_out when non-NULL; 0 otherwise. Pure.
int gtav_custom_sandbox_index(const char* name, const char* title, long* index_out);

// Of the sandbox directory names `names`, the live one for `title` (live[i] nonzero: it holds the
// title's app0) with the highest numeric suffix. A sandbox kept alive only by our mount is not
// live, and the system may hand its suffix's predecessor back to the next instance, so the suffix
// alone does not identify the running instance. Returns 0 and writes the name to `out`, or -1 when
// none matches or it does not fit. Pure.
int gtav_custom_pick_sandbox(const char* const* names, const int* live, int count,
                             const char* title, char* out, size_t out_size);

// PS5 loader only. Unmounts our mounts left in other sandboxes of `title` (and the legacy
// mountpoint everywhere), then mounts GTAV_CUSTOM_ASSET_ROOT read-only at
// <live sandbox>GTAV_CUSTOM_GAME_ROOT. Idempotent. Returns 0 when the game can open files under
// GTAV_CUSTOM_GAME_ROOT, -1 otherwise.
int gtav_custom_mount(const char* title);

#ifdef __cplusplus
}
#endif
