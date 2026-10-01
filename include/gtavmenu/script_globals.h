// Script-globals subsystem: port of the version-robust "enable MP/DLC vehicles
// in SP" technique (drp4lyf / Chiheb-Bacha lineage, ref/PC-MenyooSP) to the PS5.
//
// The fix sets a single RAGE script global -- the despawn-control flag that the
// SP background sweep reads -- to 1, which stops the sweep that both ejects the
// player from and deletes MP/DLC vehicles. See docs/script-globals-design.md.
//
// This header splits into two layers:
//
//   * The PURE CORE (joaat, the bytecode pattern scan, the global-index extract,
//     and the block/offset decomposition) is platform-independent and always
//     compiled. It touches no game memory, so it is exercised host-side by
//     tests/test_script_globals_static.py. The version-ROBUST part of the fix --
//     the global index -- comes out of this layer, because RAGE script bytecode
//     is compiled once per game edition and shipped identically across platforms.
//
//   * The RUNTIME (struct walk + resolve + write) is behind
//     GTAV_MENU_ENABLE_SCRIPT_GLOBALS (default off). It dereferences two
//     version-SPECIFIC anchor addresses (the script table and the globals base)
//     that must be RE'd out of the PS5 eboot once per game build -- exactly like
//     the native handler table in native_addresses_generated.h. Until those
//     anchors are filled and hardware-verified, this layer compiles out.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Pure core -- always compiled, no game-memory access, host-testable.
// ---------------------------------------------------------------------------

// joaat("shop_controller"): the script that owns the despawn-control bytecode.
#define GTAV_SHOP_CONTROLLER_JOAAT 0x39DA738Bu

// Enhanced-build (Gen9 / PS5) bytecode signature for the instruction sequence
// that touches the despawn-control global, plus where the 24-bit global index
// operand sits relative to the match. Sourced from ref/PC-MenyooSP
// (GeneralGlobalHax::EnableBlockedMpVehiclesInSp, g_isEnhanced branch). The PS5
// runs the Enhanced edition, so this pattern + offset apply unchanged.
#define GTAV_DESPAWN_GLOBAL_PATTERN_LEN 17u
#define GTAV_DESPAWN_GLOBAL_OPERAND_OFFSET 17u
extern const uint8_t gtav_despawn_global_pattern[GTAV_DESPAWN_GLOBAL_PATTERN_LEN];
// One char per pattern byte: 'x' = must match, '?' = wildcard. NUL-terminated.
extern const char gtav_despawn_global_mask[GTAV_DESPAWN_GLOBAL_PATTERN_LEN + 1];

// Jenkins one-at-a-time hash, GTA V flavor (input lower-cased). Mirrors the
// reference impl in devtools/generators/generate_vehicle_registry.py so precomputed catalog
// hashes and a runtime hash of the same string agree.
uint32_t gtav_joaat(const char* s);

// Read a little-endian 24-bit value (the RRAGE global-index operand width).
// PS5 is little-endian, but this is explicit so the host test is endian-safe.
uint32_t gtav_read_u24le(const uint8_t* p);

// Masked pattern search over [hay, hay+hay_len). On a match, writes the match
// offset to *out_index and returns true; otherwise returns false. mask is one
// char per pattern byte ('x' literal, anything else wildcard); pat_len bytes are
// compared, so the last candidate start is hay_len - pat_len.
bool gtav_pattern_find(const uint8_t* hay, size_t hay_len, const uint8_t* pat, const char* mask,
                       size_t pat_len, size_t* out_index);

// Generic global-index extractor: find `pattern`/`mask` (pat_len bytes) in one script
// code page, then read the 24-bit little-endian operand at match+operand_offset (masked to
// 24 bits) as a base global index. If addend_offset >= 0, the unsigned byte at
// match+addend_offset is ADDED to that base (the "base global + struct-field" accessor
// shape some scripts use); pass addend_offset < 0 for the plain operand shape (the despawn
// global). Every byte read is bounds-checked fully within the page, so a match too close to
// the page tail to read the operand (or the addend byte) is rejected rather than reading
// past the end. Returns true and sets *out_global_id on a hit. This is the reusable core the
// despawn finder below is a thin wrapper over.
bool gtav_script_extract_global_index(const uint8_t* page, size_t page_len, const uint8_t* pattern,
                                      const char* mask, size_t pat_len, size_t operand_offset,
                                      int addend_offset, uint32_t* out_global_id);

// Scan one script code page for the despawn-global access pattern and, on a hit, extract its
// 24-bit global index. Thin wrapper over gtav_script_extract_global_index (no addend).
bool gtav_script_find_despawn_global(const uint8_t* page, size_t page_len, uint32_t* out_global_id);

// Decomposition of a 24-bit global id into (block, slot-within-block). The top 6
// bits select one of <=64 block base pointers; the low 18 bits index 8-byte slots
// within that block. Pure arithmetic, so it is host-testable; the actual deref
// lives in gtav_global_ptr() below (runtime layer).
typedef struct {
  uint32_t block;   // (id >> 18) & 0x3F
  uint32_t offset;  // id & 0x3FFFF  (in 8-byte slots)
} GtavGlobalAddr;

GtavGlobalAddr gtav_global_decompose(uint32_t id);

// ---------------------------------------------------------------------------
// Runtime layer -- in-process game-memory access, behind the build gate.
// ---------------------------------------------------------------------------
#if defined(GTAV_MENU_ENABLE_SCRIPT_GLOBALS) && GTAV_MENU_ENABLE_SCRIPT_GLOBALS

// Version-specific anchors, RE'd out of the PS5 eboot once per game build (like
// native_addresses_generated.h). The generated header bakes the RE'd values via
// #ifndef guards, so a -D override wins; an unset anchor stays 0. The fallbacks
// below cover a build where the generated header is absent. Anchors of 0 make the
// runtime latch FAILED rather than dereference a null base.
#ifndef GTAV_MENU_SCRIPT_GLOBALS_HEADER
#define GTAV_MENU_SCRIPT_GLOBALS_HEADER "gtavmenu/script_globals_addresses_generated.h"
#endif
#if defined(__has_include)
#if __has_include(GTAV_MENU_SCRIPT_GLOBALS_HEADER)
#include GTAV_MENU_SCRIPT_GLOBALS_HEADER
#endif
#endif
#ifndef GTAV_SCRIPT_TABLE_ADDR
#define GTAV_SCRIPT_TABLE_ADDR 0ull
#endif
#ifndef GTAV_GLOBALS_BASE_ADDR
#define GTAV_GLOBALS_BASE_ADDR 0ull
#endif
// Despawn global index. The version-robust value re-derived from shop_controller
// bytecode (block 17, offset 0x1493B = 4540731). When non-zero, the runtime uses it
// DIRECTLY -- writing the global needs only GTAV_GLOBALS_BASE_ADDR, NOT the script
// table -- so the fix works the moment the globals base is RE'd, decoupled from the
// harder script-table anchor. When 0, the runtime re-derives it at load by walking
// the table and scanning the bytecode (needs GTAV_SCRIPT_TABLE_ADDR).
#ifndef GTAV_DESPAWN_GLOBAL_INDEX
#define GTAV_DESPAWN_GLOBAL_INDEX 0u
#endif

// Skip-Prologue global index: Global.f_9092.f_330[53]=1 (flow mission 53 = prologue1
// mission-complete). The story flow-controller skips launching a mission whose completion
// flag is set, so writing 1 here skips the prologue -- a plain in-process global write
// (no profile setting, no cloud sync, no crash). 0 = feature off. The value is a CANDIDATE
// until confirmed live on the target build (see data/script_globals/anchors.json).
#ifndef GTAV_PROLOGUE_GLOBAL_INDEX
#define GTAV_PROLOGUE_GLOBAL_INDEX 0u
#endif

// Skip-Prologue candidate set: the flow-completion flags the prologue sets on
// completion (diff-derived live; see anchors.json). The gate f_330[53] is one of
// these; the armed worker stamps ALL of them, replicating the post-prologue flag
// state. {0u}/0 = none baked (the stamp loop is then a no-op).
#ifndef GTAV_PROLOGUE_GLOBAL_CANDIDATES
#define GTAV_PROLOGUE_GLOBAL_CANDIDATES {0u}
#endif
#ifndef GTAV_PROLOGUE_GLOBAL_CANDIDATE_COUNT
#define GTAV_PROLOGUE_GLOBAL_CANDIDATE_COUNT 0u
#endif

// RAGE script-VM structs. Offsets mirror ref/PC-MenyooSP (GTAmemory.h, Enhanced
// layout); each is a hardware-verification point on PS5. Code is paged at 0x4000.
typedef struct GtavScriptHeader {
  char pad0[0x10];
  uint8_t** code_blocks;   // 0x10: array of code-page base pointers
  char pad1[0x04];         // 0x18
  int32_t code_length;     // 0x1C: total bytecode length (bytes)
  char pad2[0x04];         // 0x20
  int32_t local_count;     // 0x24
  char pad3[0x04];         // 0x28
  int32_t native_count;    // 0x2C
  int64_t* local_offset;   // 0x30
  char pad4[0x08];         // 0x38
  int64_t* native_offset;  // 0x40
  char pad5[0x10];         // 0x48
  int32_t name_hash;       // 0x58: joaat of the script name
  char pad6[0x04];         // 0x5C
  char* name;              // 0x60
  char** strings_offset;   // 0x68
  int32_t string_size;     // 0x70
  char pad7[0x0C];         // 0x74
} GtavScriptHeader;

typedef struct GtavScriptTableItem {
  GtavScriptHeader* header;
  char pad[0x04];
  int32_t hash;
} GtavScriptTableItem;

typedef struct GtavScriptTable {
  GtavScriptTableItem* items;
  char pad[0x10];
  int32_t count;
} GtavScriptTable;

// Code-page geometry helpers (page size 0x4000), mirroring ScriptHeader.
static inline int32_t gtav_script_code_page_count(const GtavScriptHeader* h) {
  return (h->code_length + 0x3FFF) >> 14;
}
static inline int32_t gtav_script_code_page_size(const GtavScriptHeader* h, int32_t page) {
  int32_t pages = gtav_script_code_page_count(h);
  if (page < 0 || page >= pages) return 0;
  return (page == pages - 1) ? (h->code_length & 0x3FFF) : 0x4000;
}

// Resolve a global id to its live int32 slot via the globals base anchor:
//   block_base = ((uint64_t*)globals_base)[block];  slot = block_base + 8*offset
// Returns NULL if globals_base is null or the selected block pointer is null (the
// global block pool is not up yet) -- callers must retry rather than write, so a
// not-yet-allocated block never turns into a wild ~674 KB-offset store.
static inline int32_t* gtav_global_ptr(uintptr_t globals_base, uint32_t id) {
  GtavGlobalAddr a = gtav_global_decompose(id);
  const uint64_t* blocks;
  uintptr_t block_base;
  if (globals_base == 0) return NULL;
  blocks = (const uint64_t*)globals_base;
  block_base = (uintptr_t)blocks[a.block];
  if (block_base == 0) return NULL;
  return (int32_t*)(block_base + 8ull * a.offset);
}

// Write `value` to script global `id` through the globals base. Returns 0 on a
// successful store, -1 if the slot is not resolvable yet (null base / block) so the
// caller retries next tick. Takes the base as a parameter (not the compile-time
// anchor), so it is exercised host-side against a mock block array.
int gtav_script_write_global(uintptr_t globals_base, uint32_t id, int32_t value);

// Read script global `id` through the globals base into *out_value. Returns 0 on a
// successful load, -1 if the slot is not resolvable yet (null base / block). The read
// mirror of gtav_script_write_global -- it takes the base as a parameter (not the
// compile-time anchor) so it is exercised host-side against a mock g_globals array, and it
// backs the read-only "global watch" debug overlay (gtav_features_debug_stats). A plain slot
// read is safe from either thread (it is just memory; values race the script engine
// harmlessly), so the overlay can sample it from the render path.
int gtav_script_read_global(uintptr_t globals_base, uint32_t id, int32_t* out_value);

/* Ensure a boolean script global has `value` (0 or 1), with an immediate readback.
 * This is the guarded primitive used by the despawn fix: it refuses to overwrite a
 * non-boolean current value, so a plausible-but-wrong index fails closed instead of
 * silently corrupting an unrelated scalar. `out_wrote` is 1 only when a store was
 * needed; the other outputs are optional. */
enum {
  GTAV_SCRIPT_GLOBAL_ENSURE_OK = 0,
  GTAV_SCRIPT_GLOBAL_ENSURE_UNAVAILABLE = -1,
  GTAV_SCRIPT_GLOBAL_ENSURE_INVALID_VALUE = -2,
  GTAV_SCRIPT_GLOBAL_ENSURE_VERIFY_FAILED = -3,
};
int gtav_script_ensure_bool_global(uintptr_t globals_base, uint32_t id, int32_t value,
                                   int32_t* out_before, int32_t* out_after, uint32_t* out_wrote);

typedef enum {
  GTAV_SCRIPT_GLOBALS_IDLE = 0,
  GTAV_SCRIPT_GLOBALS_WAIT_SCRIPT,   // polling the table for shop_controller
  GTAV_SCRIPT_GLOBALS_SCANNING,      // walking shop_controller's code pages
  GTAV_SCRIPT_GLOBALS_WRITE_DIRECT,  // baked index: retry the write until the block is up
  GTAV_SCRIPT_GLOBALS_DONE,          // global resolved + written (one-shot)
  GTAV_SCRIPT_GLOBALS_FAILED,        // anchors missing or pattern not found
} GtavScriptGlobalsState;

typedef struct {
  GtavScriptGlobalsState state;
  uint32_t global_id;   // resolved 24-bit index (valid once DONE)
  int32_t attempts;     // resolution/initial-write attempts so far
  uint32_t checks;      // post-install health checks
  uint32_t writes;      // verified stores, including repairs
  uint32_t repairs;     // stores after the first successful installation
  int32_t last_value;   // most recent readable value
  int32_t last_result;  // GTAV_SCRIPT_GLOBAL_ENSURE_* result
} GtavScriptGlobalsStatus;

// Arm the MP/DLC-vehicle despawn fix after an explicit vehicle-spawn request. This
// only publishes a process-local flag and may be called from the menu worker; it
// never reads or writes GTA memory. Until armed, gtav_script_globals_tick() remains
// IDLE and performs no script-global access. This keeps the default-on capability
// out of story/save initialization while preserving it for the feature that needs it.
void gtav_script_globals_arm(void);
int gtav_script_globals_is_armed(void);

// Drive the guarded state machine. Call once per GAME-THREAD tick (script
// context) -- it walks VM structures the script engine actively mutates, so it
// must not run on the worker thread. Before gtav_script_globals_arm(), it is a
// no-op that reports IDLE. Once armed, it returns 1 in a terminal state (DONE or
// FAILED), 0 while still working. DONE performs one plain read per tick and repairs
// a 0->1 reset with verified readback; an unavailable block returns to WRITE_DIRECT
// and retries, while a non-boolean value fails closed. Fills *out (may be NULL).
int gtav_script_globals_tick(GtavScriptGlobalsStatus* out);

#endif  // GTAV_MENU_ENABLE_SCRIPT_GLOBALS

#ifdef __cplusplus
}  // extern "C"
#endif
