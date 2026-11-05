/* PS4 platform hooks of the custom-pack engine (gtavmenu/pack_platform.h). */

#include "gtavmenu/pack_platform.h"

#include "gtavmenu/frame_hook.h"
#include "gtavmenu/ps4_syscall.h"

/* The whole menu runs in the game-thread frame hook: engine steps may run while it is live. */
int gtav_platform_on_game_thread(void) {
  return gtav_frame_hook_is_active();
}

static GtavPlatformProtectionQuery g_query;

void gtav_platform_set_protection_query(GtavPlatformProtectionQuery query) {
  __atomic_store_n(&g_query, query, __ATOMIC_RELEASE);
}

static GtavPlatformProtectionQuery protection_query(void) {
  return __atomic_load_n(&g_query, __ATOMIC_ACQUIRE);
}

/* Every byte of [address, end) in mappings the query shows readable (at most 8 pieces). */
static int readable_by_query(GtavPlatformProtectionQuery query, uintptr_t address, uintptr_t end) {
  uintptr_t cursor = address;
  for (int piece = 0; piece < 8 && cursor < end; ++piece) {
    uintptr_t start = 0, stop = 0;
    uint32_t prot = 0;
    if (query(cursor, &start, &stop, &prot) != 0 || start > cursor || stop <= cursor ||
        (prot & 1u) == 0) {
      return 0;
    }
    cursor = stop;
  }
  return cursor >= end;
}

/* Pack preflight reads many small store/factory fields on the game thread. Probing the following
   8 MiB for each field multiplies those reads into thousands of needless queries, and also rejects
   valid fields near the end of a mapping. Validate only the bytes the caller will use. */
int gtav_platform_pack_range_readable(uintptr_t address, size_t bytes) {
  if (!address || !bytes || bytes - 1u > UINTPTR_MAX - address) return 0;
  const GtavPlatformProtectionQuery query = protection_query();
  if (query) return readable_by_query(query, address, address + bytes);
  const uintptr_t first = address & ~(uintptr_t)0x3fff;
  const uintptr_t last = (address + bytes - 1u) & ~(uintptr_t)0x3fff;
  /* Preserve the discovery query's bounded work, including the offset into the first page. */
  if (last - first >= 0x800000u) return 0;
  uint8_t vec[64];
  uintptr_t at = first;
  for (;;) {
    const uintptr_t left = last - at + 0x4000u;
    const uintptr_t chunk = left < 0x40000u ? left : 0x40000u;
    if (gtav_ps4_syscall3(GTAV_PS4_SYS_MINCORE, at, chunk, (uintptr_t)vec) != 0) return 0;
    if (left == chunk) return 1;
    at += chunk;
  }
}

/* The query's readable mapping holding `address`, bounded to 8 MiB from its page. Without the
   query: FreeBSD mincore (no libkernel import), up to 8 MiB (32 chunks of 256 KiB) of contiguous
   mapped pages from the 16 KiB page holding `address`. */
int gtav_platform_mapped_range(uintptr_t address, uintptr_t* start, uintptr_t* end) {
  *start = 0;
  *end = 0;
  if (!address) return -1;
  const uintptr_t first = address & ~(uintptr_t)0x3fff;
  const GtavPlatformProtectionQuery query = protection_query();
  if (query) {
    uintptr_t from = 0, to = 0;
    uint32_t prot = 0;
    if (query(address, &from, &to, &prot) != 0 || from > address || to <= address ||
        (prot & 1u) == 0) {
      return -1;
    }
    *start = from;
    *end = to - first > 0x800000u ? first + 0x800000u : to;
    return 0;
  }
  uintptr_t at = first;
  uint8_t vec[64];
  for (int chunk = 0; chunk < 32; ++chunk, at += 0x40000u) {
    if (gtav_ps4_syscall3(GTAV_PS4_SYS_MINCORE, at, 0x40000u, (uintptr_t)vec) != 0) break;
    *end = at + 0x40000u;
  }
  if (!*end) return -1;
  *start = first;
  return 0;
}
