// Worker-side kernel-log writer (crash-capture sink).
//
// History of two dead ends (both crashed the injected worker on the first log line):
//   1. Reusing the crt's klog_puts -- it snprintf()s through a pointer __klog_init() fills at
//      crt _start, which the injected worker (started as a bare thread at its ELF entry) never
//      runs, so the pointer is NULL.
//   2. A raw `syscall` instruction -- PS5 requires the syscall instruction to originate from
//      inside libkernel (the SDK's __crt_syscall literally jumps to libkernel's syscall site,
//      crt/syscall.c "jump directly to the syscall instruction"). A `syscall` in our injected
//      code is at a non-libkernel address, so the kernel kills the process.
//
// The blessed path for injected code is the libkernel export sceKernelDebugOutText(channel,
// text): it routes through libkernel (valid syscall origin) and is what injected game plugins
// use for debug output (see ref/etaHEN-Plugins Game_Plugin_Loader). It resolves from the same
// libkernel.so stub that gives notify.c its sceKernelSendNotificationRequest -- which already
// works from this worker -- so it links implicitly and is safe to call at init.
//
// Why the kernel log at all: the worker's other sink is the in-memory ring (log_ring.c), which
// lives in GTA's process memory and dies with the process -- it can never hold the breadcrumbs
// immediately before a fatal crash. The kernel log is kernel-side and survives. Stream it from
// a PC with `./menu-ctl.sh logs --kernel` (ps5debug klog forwarder, port 3232).

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gtavmenu/custom_assets.h"
#include "gtavmenu/log.h"

// Pack-note mirror. Hardware runs lost worker klog lines mid-run (the klog forwarder
// stopped delivering them while system lines kept coming), and the status event ring overflows,
// so every "GTAVMenu pack ..." line is also kept in a small in-memory ring that the worker thread
// writes to /data/gtavmenu/custom/GTAV_PACK_NOTES_NAME (temp + rename) when shared /data
// is available, about once a second; `menu-ctl.sh pack-notes`
// fetches it over FTP. Writers may be the game thread or the worker: a slot is claimed with an
// atomic counter and published with its sequence number, so the flusher skips a half-written slot.
#define GTAV_PACK_NOTE_SLOTS 128u
#define GTAV_PACK_NOTE_LEN 160u
static char g_pack_notes[GTAV_PACK_NOTE_SLOTS][GTAV_PACK_NOTE_LEN];
static uint32_t g_pack_note_seq[GTAV_PACK_NOTE_SLOTS];
static uint32_t g_pack_note_next;
static uint32_t g_pack_note_flushed;

static void pack_note_mirror(const char* message) {
  static const char kTag[] = "GTAVMenu pack";
  if (strncmp(message, kTag, sizeof(kTag) - 1u) != 0) return;
  const uint32_t n = __atomic_fetch_add(&g_pack_note_next, 1u, __ATOMIC_ACQ_REL);
  const uint32_t slot = n % GTAV_PACK_NOTE_SLOTS;
  __atomic_store_n(&g_pack_note_seq[slot], 0u, __ATOMIC_RELEASE);
  size_t i = 0;
  for (; message[i] != '\0' && i < GTAV_PACK_NOTE_LEN - 1u; ++i) g_pack_notes[slot][i] = message[i];
  g_pack_notes[slot][i] = '\0';
  __atomic_store_n(&g_pack_note_seq[slot], n + 1u, __ATOMIC_RELEASE);
}

int gtav_pack_notes_flush(void) {
  const uint32_t end = __atomic_load_n(&g_pack_note_next, __ATOMIC_ACQUIRE);
  if (end == g_pack_note_flushed) return 0;
  // Streamed line by line (no file-sized buffer: worker memory is tight,
  // tools/check_worker_span.py).
  const uint32_t start = end > GTAV_PACK_NOTE_SLOTS ? end - GTAV_PACK_NOTE_SLOTS : 0u;
  char path[128];
  char temp[132];
#ifdef GTAV_PACK_NOTES_PATH  // host tests: a fixed file
  snprintf(path, sizeof(path), "%s", GTAV_PACK_NOTES_PATH);
#else
  if (!gtav_custom_game_root_available()) return -1;
  snprintf(path, sizeof(path), "%s/" GTAV_PACK_NOTES_NAME, gtav_custom_game_root());
#endif
  snprintf(temp, sizeof(temp), "%s.tmp", path);
  FILE* fp = fopen(temp, "wb");
  if (!fp) return -1;
  int bad = fprintf(fp, "# pack notes %u..%u\n", (unsigned)start, (unsigned)end) < 0;
  for (uint32_t n = start; n < end && !bad; ++n) {
    const uint32_t slot = n % GTAV_PACK_NOTE_SLOTS;
    if (__atomic_load_n(&g_pack_note_seq[slot], __ATOMIC_ACQUIRE) != n + 1u) continue;
    char line[GTAV_PACK_NOTE_LEN];
    memcpy(line, g_pack_notes[slot], sizeof(line));
    line[sizeof(line) - 1u] = '\0';
    // A writer may have reclaimed the slot while it was copied: keep the line only if it did not.
    if (__atomic_load_n(&g_pack_note_seq[slot], __ATOMIC_ACQUIRE) != n + 1u) continue;
    bad = fprintf(fp, "%u %s\n", (unsigned)n, line) < 0;
  }
  if (fclose(fp) != 0) bad = 1;
  if (bad || rename(temp, path) != 0) {
    remove(temp);
    return -1;
  }
  g_pack_note_flushed = end;
  return 0;
}

#if defined(__PROSPERO__) || defined(__ORBIS__)

// libkernel debug output (NID 9JYNqN6jAKI). Declared locally like notify.c's libkernel import;
// resolves from libkernel.so, which the prospero toolchain links implicitly.
extern int sceKernelDebugOutText(int channel, const char* text);

void gtav_worker_klog(const char* message) {
  if (!message) {
    return;
  }
  pack_note_mirror(message);
  // Copy + append a newline so consecutive lines don't run together. Manual (no libc dep);
  // the message already carries the "GTAVMenu" tag that `logs --kernel` filters on.
  char line[512];
  size_t i = 0;
  for (; message[i] != '\0' && i < sizeof(line) - 2; ++i) {
    line[i] = message[i];
  }
  line[i++] = '\n';
  line[i] = '\0';
  (void)sceKernelDebugOutText(0, line);
}

#else  // Host build (static tests, Linux cc): no libkernel -- inert stub.

void gtav_worker_klog(const char* message) {
  if (message) pack_note_mirror(message);
}

#endif
