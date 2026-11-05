#pragma once

/* Platform hooks of the custom-pack engine (src/module/features/custom_engine.inc). The PS5 build
 * implements both inline in the engine; every other platform links its own. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Nonzero when the caller runs where engine pack steps may touch the engine (the game thread). */
int gtav_platform_on_game_thread(void);
/* 0 and the readable run [*start, *end) holding `address`, or -1 when `address` is not mapped
 * readable. The run may be bounded (callers bound each read within it). */
int gtav_platform_mapped_range(uintptr_t address, uintptr_t* start, uintptr_t* end);
/* PS4: nonzero when every byte of [address, address + bytes) lies in readable memory. Unlike the
 * discovery query above, this checks only the requested range, never an extra 8 MiB per field. */
int gtav_platform_pack_range_readable(uintptr_t address, size_t bytes);

/* PS4: the kernel's protection query (sceKernelQueryMemoryProtection), which the plugin hands the
 * engine at load: 0 with the mapping [*start, *end) and its protection bits (1 = read) for
 * `address`, or -1. Without one (the freestanding dev worker, which links no libkernel) the checks
 * above fall back to mincore, which proves a page mapped but not readable. */
typedef int (*GtavPlatformProtectionQuery)(uintptr_t address, uintptr_t* start, uintptr_t* end,
                                           uint32_t* prot);
void gtav_platform_set_protection_query(GtavPlatformProtectionQuery query);

#ifdef __cplusplus
}
#endif
