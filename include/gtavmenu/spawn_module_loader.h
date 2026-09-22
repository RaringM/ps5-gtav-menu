#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Privileged in-process spawn-module load via sceKernelLoadStartModule. This is
// retained only as a historical diagnostic -- the live spawn route drains
// game-thread actions through the PLAYER_PED_ID frame hook, not this path. On
// host builds without PS5 kernel R/W it is a no-op stub. Returns the command
// name ("load_module") for status reporting. See src/module/spawn_module_loader.c.
const char* gtav_menu_load_spawn_module(const char* source);

#ifdef __cplusplus
}
#endif
