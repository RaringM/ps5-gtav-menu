#pragma once

// Build pin table (see loader_pins_generated.h): one immutable row per supported
// GTA V build the loader can auto-detect. A row is only emitted from a target
// manifest whose per-build addresses have been derived from that build's decrypted
// eboot; pending manifests never produce a row. All addresses are absolute live
// virtual addresses (the eboot maps at its fixed link base, no ASLR slide).

#include <stddef.h>
#include <stdint.h>

#define GTAV_BUILD_PIN_VERSION_BYTES_N 16u
#define GTAV_BUILD_PIN_BROKER_EXPECTED_BYTES_N 16u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gtav_build_pin {
  // Immutable identity: the target manifest id (e.g. "PPSA04264_01.005.000_DISC").
  const char* target_id;
  // GET_FRAME_COUNT prologue signature: absolute live address + exact 16 bytes.
  // The loader probes this for every row and selects the first matching build.
  uintptr_t version_signature_addr;
  uint8_t version_signature_expected[GTAV_BUILD_PIN_VERSION_BYTES_N];
  // Loader-owned frame-hook contract at PLAYER_PED_ID: the loader writes a
  // 14-byte jump into the 16-byte prologue [target, target+stolen_len), whose
  // exact bytes must match broker_expected (also the rollback material).
  uintptr_t broker_target;
  uintptr_t broker_continuation;
  uint32_t broker_patch_len;
  uint32_t broker_stolen_len;
  uint8_t broker_expected[GTAV_BUILD_PIN_BROKER_EXPECTED_BYTES_N];
  // Privileged .text write-guard range (any loader protect/write outside it fails
  // closed). 0/0 disables the range (bare/research build).
  uintptr_t text_live_start;
  uintptr_t text_live_end;
  // Player-world/load readiness anchor: ped = *(*(anchor)+deref_offset) != 0.
  uintptr_t sp_ready_addr;
  uintptr_t sp_ready_deref_offset;
} GtavBuildPin;

#ifdef __cplusplus
}
#endif