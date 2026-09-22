// Script-globals subsystem implementation. See include/gtavmenu/script_globals.h
// and docs/script-globals-design.md.
//
// The pure core (joaat / pattern scan / index extract / decompose) is always
// compiled and host-tested. The runtime state machine that walks the RAGE script
// VM and writes the despawn-control global is behind GTAV_MENU_ENABLE_SCRIPT_GLOBALS.

#include "gtavmenu/script_globals.h"

// Enhanced-build despawn-global signature (ref/PC-MenyooSP, g_isEnhanced branch).
// The trailing 0x62 opcode's 24-bit operand (at +OPERAND_OFFSET) is the global id.
const uint8_t gtav_despawn_global_pattern[GTAV_DESPAWN_GLOBAL_PATTERN_LEN] = {
    0x2D, 0x00, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00,
    0x56, 0x00, 0x00, 0x71, 0x2E, 0x00, 0x00, 0x62,
};
const char gtav_despawn_global_mask[GTAV_DESPAWN_GLOBAL_PATTERN_LEN + 1] = "x????x???x??xx??x";

uint32_t gtav_joaat(const char* s) {
  uint32_t h = 0;
  if (s == NULL) return 0;
  for (; *s; ++s) {
    unsigned char c = (unsigned char)*s;
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
    h += c;
    h += h << 10;
    h ^= h >> 6;
  }
  h += h << 3;
  h ^= h >> 11;
  h += h << 15;
  return h;
}

uint32_t gtav_read_u24le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

bool gtav_pattern_find(const uint8_t* hay, size_t hay_len, const uint8_t* pat, const char* mask,
                       size_t pat_len, size_t* out_index) {
  if (hay == NULL || pat == NULL || mask == NULL || pat_len == 0) return false;
  if (hay_len < pat_len) return false;
  for (size_t start = 0; start + pat_len <= hay_len; ++start) {
    size_t i = 0;
    for (; i < pat_len; ++i) {
      if (mask[i] == 'x' && hay[start + i] != pat[i]) break;
    }
    if (i == pat_len) {
      if (out_index) *out_index = start;
      return true;
    }
  }
  return false;
}

bool gtav_script_extract_global_index(const uint8_t* page, size_t page_len, const uint8_t* pattern,
                                      const char* mask, size_t pat_len, size_t operand_offset,
                                      int addend_offset, uint32_t* out_global_id) {
  size_t match = 0;
  if (!gtav_pattern_find(page, page_len, pattern, mask, pat_len, &match)) return false;
  // The 24-bit operand must lie fully inside this page (3 bytes for the index).
  size_t operand = match + operand_offset;
  if (operand + 3 > page_len) return false;
  uint32_t id = gtav_read_u24le(page + operand) & 0xFFFFFFu;
  // Optional struct-field addend byte (the "base global + field" accessor shape), also fully
  // in-page. Masked back to 24 bits in case the add carries out of the operand width.
  if (addend_offset >= 0) {
    size_t addend = match + (size_t)addend_offset;
    if (addend + 1 > page_len) return false;
    id = (id + page[addend]) & 0xFFFFFFu;
  }
  if (out_global_id) *out_global_id = id;
  return true;
}

bool gtav_script_find_despawn_global(const uint8_t* page, size_t page_len,
                                     uint32_t* out_global_id) {
  return gtav_script_extract_global_index(page, page_len, gtav_despawn_global_pattern,
                                          gtav_despawn_global_mask, GTAV_DESPAWN_GLOBAL_PATTERN_LEN,
                                          GTAV_DESPAWN_GLOBAL_OPERAND_OFFSET, -1, out_global_id);
}

GtavGlobalAddr gtav_global_decompose(uint32_t id) {
  GtavGlobalAddr a;
  a.block = (id >> 18) & 0x3Fu;
  a.offset = id & 0x3FFFFu;
  return a;
}

// ---------------------------------------------------------------------------
#if defined(GTAV_MENU_ENABLE_SCRIPT_GLOBALS) && GTAV_MENU_ENABLE_SCRIPT_GLOBALS

// The despawn-control flag value: 1 disables the SP sweep that ejects + despawns
// MP/DLC vehicles (see docs/script-globals-design.md).
#define GTAV_DESPAWN_GLOBAL_VALUE 1

// The capability is built/default-on, but the write is demand-armed. The menu
// worker may publish the flag when a spawn row is selected; only the game-thread
// tick below consumes it and touches RAGE VM memory.
static uint32_t g_script_globals_armed;

void gtav_script_globals_arm(void) {
  __atomic_store_n(&g_script_globals_armed, 1u, __ATOMIC_RELEASE);
}

int gtav_script_globals_is_armed(void) {
  return __atomic_load_n(&g_script_globals_armed, __ATOMIC_ACQUIRE) != 0u;
}

int gtav_script_write_global(uintptr_t globals_base, uint32_t id, int32_t value) {
  volatile int32_t* slot = gtav_global_ptr(globals_base, id);
  if (slot == NULL) return -1;  // base/block not up yet -- caller retries
  *slot = value;
  return 0;
}

int gtav_script_read_global(uintptr_t globals_base, uint32_t id, int32_t* out_value) {
  const volatile int32_t* slot = gtav_global_ptr(globals_base, id);
  if (slot == NULL) return -1;  // base/block not up yet -- value unknowable
  if (out_value) *out_value = *slot;
  return 0;
}

int gtav_script_ensure_bool_global(uintptr_t globals_base, uint32_t id, int32_t value,
                                   int32_t* out_before, int32_t* out_after, uint32_t* out_wrote) {
  int32_t before = 0;
  int32_t after = 0;
  if (out_wrote) *out_wrote = 0;
  if (value != 0 && value != 1) return GTAV_SCRIPT_GLOBAL_ENSURE_INVALID_VALUE;
  if (gtav_script_read_global(globals_base, id, &before) != 0)
    return GTAV_SCRIPT_GLOBAL_ENSURE_UNAVAILABLE;
  if (out_before) *out_before = before;
  if (before != 0 && before != 1) return GTAV_SCRIPT_GLOBAL_ENSURE_INVALID_VALUE;
  if (before != value) {
    if (gtav_script_write_global(globals_base, id, value) != 0)
      return GTAV_SCRIPT_GLOBAL_ENSURE_UNAVAILABLE;
    if (out_wrote) *out_wrote = 1;
  }
  if (gtav_script_read_global(globals_base, id, &after) != 0)
    return GTAV_SCRIPT_GLOBAL_ENSURE_UNAVAILABLE;
  if (out_after) *out_after = after;
  return after == value ? GTAV_SCRIPT_GLOBAL_ENSURE_OK : GTAV_SCRIPT_GLOBAL_ENSURE_VERIFY_FAILED;
}

// One-shot, game-thread state machine. Persistent across ticks via statics; the
// terminal states (DONE/FAILED) latch so repeated ticks are cheap no-ops.
//
// Two paths, chosen at IDLE:
//   * DIRECT (GTAV_DESPAWN_GLOBAL_INDEX baked, version-robust value already known):
//     write the global straight away -- needs only GTAV_GLOBALS_BASE_ADDR, so it is
//     decoupled from the script-table anchor. Retries until the block pool is up.
//   * DERIVE (index == 0): walk the script table to find shop_controller, scan its
//     bytecode for the index, then write -- needs both anchors.
int gtav_script_globals_tick(GtavScriptGlobalsStatus* out) {
  static GtavScriptGlobalsState state = GTAV_SCRIPT_GLOBALS_IDLE;
  static uint32_t resolved_id = 0;
  static int32_t attempts = 0;
  static uint32_t checks = 0;
  static uint32_t writes = 0;
  static uint32_t repairs = 0;
  static int32_t last_value = 0;
  static int32_t last_result = GTAV_SCRIPT_GLOBAL_ENSURE_UNAVAILABLE;

  if (!gtav_script_globals_is_armed()) {
    if (out) {
      out->state = GTAV_SCRIPT_GLOBALS_IDLE;
      out->global_id = 0;
      out->attempts = 0;
      out->checks = 0;
      out->writes = 0;
      out->repairs = 0;
      out->last_value = 0;
      out->last_result = GTAV_SCRIPT_GLOBAL_ENSURE_UNAVAILABLE;
    }
    return 0;
  }

  switch (state) {
    case GTAV_SCRIPT_GLOBALS_IDLE:
      // The globals base is required either way; without it nothing can be written.
      if (GTAV_GLOBALS_BASE_ADDR == 0ull) {
        state = GTAV_SCRIPT_GLOBALS_FAILED;
        break;
      }
      if (GTAV_DESPAWN_GLOBAL_INDEX != 0u) {
        resolved_id = (uint32_t)GTAV_DESPAWN_GLOBAL_INDEX;  // baked: skip the table walk
        state = GTAV_SCRIPT_GLOBALS_WRITE_DIRECT;
        break;
      }
      if (GTAV_SCRIPT_TABLE_ADDR == 0ull) {
        state = GTAV_SCRIPT_GLOBALS_FAILED;  // need the table to derive the index
        break;
      }
      state = GTAV_SCRIPT_GLOBALS_WAIT_SCRIPT;
      break;

    case GTAV_SCRIPT_GLOBALS_WRITE_DIRECT:
      ++attempts;
      // Block pool may not be allocated on the very first ticks; retry until it is. Refuse a
      // non-boolean slot and require readback before accepting the installation.
      {
        uint32_t wrote = 0;
        last_result = gtav_script_ensure_bool_global((uintptr_t)GTAV_GLOBALS_BASE_ADDR, resolved_id,
                                                     GTAV_DESPAWN_GLOBAL_VALUE, &last_value,
                                                     &last_value, &wrote);
        writes += wrote;
      }
      if (last_result == GTAV_SCRIPT_GLOBAL_ENSURE_OK) {
        state = GTAV_SCRIPT_GLOBALS_DONE;
      } else if (last_result != GTAV_SCRIPT_GLOBAL_ENSURE_UNAVAILABLE) {
        state = GTAV_SCRIPT_GLOBALS_FAILED;
      }
      break;

    case GTAV_SCRIPT_GLOBALS_WAIT_SCRIPT: {
      ++attempts;
      const GtavScriptTable* table = (const GtavScriptTable*)(uintptr_t)GTAV_SCRIPT_TABLE_ADDR;
      if (table->items == NULL || table->count <= 0) break;  // VM not ready; retry next tick
      const GtavScriptHeader* hdr = NULL;
      for (int32_t i = 0; i < table->count; ++i) {
        if ((uint32_t)table->items[i].hash == GTAV_SHOP_CONTROLLER_JOAAT) {
          hdr = table->items[i].header;
          break;
        }
      }
      if (hdr == NULL || hdr->code_length <= 0) break;  // not loaded yet; retry
      // shop_controller is resident -- scan its code pages this tick.
      int32_t pages = gtav_script_code_page_count(hdr);
      for (int32_t p = 0; p < pages; ++p) {
        int32_t sz = gtav_script_code_page_size(hdr, p);
        if (sz <= 0) continue;
        uint32_t id = 0;
        if (gtav_script_find_despawn_global(hdr->code_blocks[p], (size_t)sz, &id)) {
          resolved_id = id;
          uint32_t wrote = 0;
          last_result = gtav_script_ensure_bool_global((uintptr_t)GTAV_GLOBALS_BASE_ADDR, id,
                                                       GTAV_DESPAWN_GLOBAL_VALUE, &last_value,
                                                       &last_value, &wrote);
          writes += wrote;
          if (last_result == GTAV_SCRIPT_GLOBAL_ENSURE_OK) {
            state = GTAV_SCRIPT_GLOBALS_DONE;
          } else if (last_result != GTAV_SCRIPT_GLOBAL_ENSURE_UNAVAILABLE) {
            state = GTAV_SCRIPT_GLOBALS_FAILED;
          }
          break;
        }
      }
      if (state != GTAV_SCRIPT_GLOBALS_DONE) state = GTAV_SCRIPT_GLOBALS_FAILED;
      break;
    }

    case GTAV_SCRIPT_GLOBALS_DONE: {
      // A persistent worker can survive a Story reload even though the VM's transient globals
      // are reset. Keep the verified flag true without blind periodic stores: read every game
      // tick, write only after observing the valid boolean reset value 0, then verify readback.
      uint32_t wrote = 0;
      ++checks;
      last_result = gtav_script_ensure_bool_global((uintptr_t)GTAV_GLOBALS_BASE_ADDR, resolved_id,
                                                   GTAV_DESPAWN_GLOBAL_VALUE, &last_value,
                                                   &last_value, &wrote);
      writes += wrote;
      repairs += wrote;
      if (last_result == GTAV_SCRIPT_GLOBAL_ENSURE_UNAVAILABLE) {
        state = GTAV_SCRIPT_GLOBALS_WRITE_DIRECT;
      } else if (last_result != GTAV_SCRIPT_GLOBAL_ENSURE_OK) {
        state = GTAV_SCRIPT_GLOBALS_FAILED;
      }
      break;
    }

    case GTAV_SCRIPT_GLOBALS_SCANNING:  // folded into WAIT_SCRIPT; kept for ABI clarity
    case GTAV_SCRIPT_GLOBALS_FAILED:
      break;
  }

  if (out) {
    out->state = state;
    out->global_id = resolved_id;
    out->attempts = attempts;
    out->checks = checks;
    out->writes = writes;
    out->repairs = repairs;
    out->last_value = last_value;
    out->last_result = last_result;
  }
  return (state == GTAV_SCRIPT_GLOBALS_DONE || state == GTAV_SCRIPT_GLOBALS_FAILED) ? 1 : 0;
}

#endif  // GTAV_MENU_ENABLE_SCRIPT_GLOBALS
