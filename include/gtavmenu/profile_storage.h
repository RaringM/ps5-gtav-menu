#pragma once

/*
 * The injected worker runs inside GTA's filesystem sandbox and deliberately cannot use the
 * payload SDK's kernel root-switch helpers: its ELF CRT entry point never runs, so the SDK syscall
 * dispatcher is not initialized. The loader therefore exposes this small host-side directory at
 * the same path inside the live title sandbox before mapping the worker.
 */
#define GTAV_PROFILE_STORAGE_ROOT "/data/GTAVMenu/state"
#define GTAV_PROFILE_STORAGE_PATH GTAV_PROFILE_STORAGE_ROOT "/profile.cfg"
/* v0.1.5 and earlier addressed the console-global path directly. If a file exists there from a
 * build whose worker root switch was available, the loader moves it into the mounted directory. */
#define GTAV_PROFILE_STORAGE_LEGACY_PATH "/data/GTAVMenu/profile.cfg"

#ifdef __cplusplus
extern "C" {
#endif

/* Loader-side only. Mounts GTAV_PROFILE_STORAGE_ROOT read-write at the same path inside the live
 * title sandbox. Stale mounts belonging to older instances of the title are retired first. */
int gtav_profile_storage_mount(const char* title);

#ifdef __cplusplus
}
#endif
