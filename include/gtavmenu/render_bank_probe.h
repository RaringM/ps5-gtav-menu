#pragma once

#include "gtavmenu/render_path_probe.h"

#ifndef GTAV_RENDER_BANK_PROBE
#define GTAV_RENDER_BANK_PROBE 0
#endif
#if GTAV_RENDER_BANK_PROBE && !GTAV_RENDER_PATH_PROBE
#error "Bank lifetime observation requires the pinned isolated path-probe build"
#endif
#if GTAV_RENDER_BANK_PROBE && GTAV_RENDER_DIAG_INITIAL_MODE != 1
#error "Bank lifetime observation requires drawing disabled at startup"
#endif

#if GTAV_RENDER_BANK_PROBE
#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_BANK_MAGIC 0x314b4e4142545447ull
#define GTAV_BANK_CAPACITY 4096u
#define GTAV_BANK_LEDGER 64u
#define GTAV_BANK_SITE 0x1cbf920u
#define GTAV_BANK_RELAY 0x3797000u
#define GTAV_BANK_ORIGINAL 0x480000bb88c96948ull
#define GTAV_BANK_PATCH 0x48909001ad76dbe9ull
// No override: live text-page writability and cross-core instruction publication are unproven.
#define GTAV_BANK_INSTALL_VALIDATED 0

enum { GTAV_BANK_BEGIN = 1, GTAV_BANK_END = 2, GTAV_BANK_CALLBACK = 3 };
// Every word is a separate observation, not a coherent engine transaction.
typedef struct GtavBankWords {
  uint64_t epoch, reset, producer, consumer, count0, count1;
  uint64_t force_zero, alternate_mode, requested0, requested1, active0, active1;
  uint64_t requested_flag, active_flag;
} GtavBankWords;
typedef struct GtavBankRecord {
  uint64_t ready, ordinal, stamp, kind, thread, key, link, bank, caller;
  GtavBankWords words;
} GtavBankRecord;
typedef struct GtavBankState {
  uint64_t magic, abi, size, phase, installed, reserved, serial, active;
  uint64_t dropped, ledger_full, invalid_bank, missing_leave, entering;
  GtavBankRecord records[GTAV_BANK_CAPACITY];
} GtavBankState;
extern GtavBankState gtav_render_bank_probe;
extern uintptr_t gtav_render_bank_resume;
void gtav_render_bank_select_thunk(void);
void gtav_render_bank_return_thunk(void);

// Core: no game reads/writes. Successful begin owns a ledger slot until leave, even after stop.
int gtav_render_bank_begin(uint64_t thread, uintptr_t key, uintptr_t caller, uint64_t bank,
                           uint64_t stamp, const GtavBankWords* words);
uintptr_t gtav_render_bank_end(uint64_t thread, uintptr_t key, uint64_t stamp,
                               const GtavBankWords* words);
void gtav_render_bank_point(uint64_t thread, uint64_t path_sequence, uintptr_t context,
                            uint64_t stamp, const GtavBankWords* words);
// 1 = install once (host must prepare relay and writable code); 2 = arm once; 3 = permanent stop.
int gtav_render_bank_control(uint64_t operation);
void gtav_render_bank_callback(uint64_t thread, uint64_t path_sequence, uintptr_t context);

#ifdef __cplusplus
}
#endif
#endif
