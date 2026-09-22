#pragma once

#include <stdint.h>

// Retrievable worker-side log sink. The worker (injected into GTA) compiles file I/O out,
// so its log lines land in this in-memory ring instead. Like GtavMenuStatus it is exported
// with a fixed magic + a byte marker the PC scans for, then read whole out-of-process over
// ps5debug. Single-writer, lock-free: ONLY the worker thread writes it (never the game-thread
// frame hook), and the remote reader tolerates the occasional torn read.
//
// Distinct from the 16-slot status event ring (structured liveness/state): this carries the
// free-text leveled log lines and is larger so verbose (Telemetry) tracing isn't lost to
// fast wrap. level/category are stored as raw uint32 (GtavLogLevel / GtavLogCategory values)
// to keep the layout independent of log.h for tooling/decoders.

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_MENU_LOG_RING_MAGIC 0x52474F4C56415447ull /* "GTAVLOGR" little-endian */
#define GTAV_MENU_LOG_RING_ABI_VERSION 1u
#define GTAV_MENU_LOG_RING_ENTRY_COUNT 64u
#define GTAV_MENU_LOG_RING_MSG_LEN 128u
#define GTAV_MENU_LOG_RING_ENTRY_SIZE 144u
#define GTAV_MENU_LOG_RING_SIZE 9240u

typedef struct GtavMenuLogEntry {
  uint64_t seq;  // 0-based monotonic id (= write_index at write time); orders entries past wrap
  uint32_t level;
  uint32_t category;
  char message[GTAV_MENU_LOG_RING_MSG_LEN];
} GtavMenuLogEntry;

typedef struct GtavMenuLogRing {
  uint64_t magic;
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t write_index;  // total writes; newest slot is (write_index - 1) % ENTRY_COUNT
  uint32_t count;        // valid slots, saturates at ENTRY_COUNT
  GtavMenuLogEntry entries[GTAV_MENU_LOG_RING_ENTRY_COUNT];
} GtavMenuLogRing;

// The live ring, and the byte marker the PC scans for to locate it in process memory.
extern GtavMenuLogRing gtav_menu_log_ring;
extern const char gtav_menu_log_ring_marker[];

// Append one line to the ring. level/category are GtavLogLevel/GtavLogCategory values.
// Messages longer than the slot are truncated (NUL-terminated). Worker thread only.
void gtav_log_ring_write(uint32_t level, uint32_t category, const char* message);

#ifdef __cplusplus
}
#endif
