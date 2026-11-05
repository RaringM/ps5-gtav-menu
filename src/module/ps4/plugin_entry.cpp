#include "gtavmenu/ps4_plugin.h"

#include "gtavmenu/pack_platform.h"
#include "gtavmenu/plugin_lifecycle.h"
#include "gtavmenu/ps4_plugin_boot_pools.h"
#include "gtavmenu/ps4_plugin_log.h"
#include "gtavmenu/ps4_slice.h"
#include GTAV_PS4_TARGET_HEADER
#include GTAV_PS4_PLUGIN_HEADER

#include <orbis/libkernel.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The build's boot-pool mode (make/ps4-plugin.mk PS4_BOOT_POOLS_MODE): GTAV_BOOT_POOL_DATA_MODE_*
// as a number, so the preprocessor can name it. Anything but full is a probe build.
#ifndef GTAV_PS4_BOOT_POOLS_MODE
#error \
    "GTAV_PS4_BOOT_POOLS_MODE must be set (make/ps4-plugin.mk PS4_BOOT_POOLS_MODE: off, observe, caps, full)"
#endif
#if GTAV_PS4_BOOT_POOLS_MODE == 0
#define GTAV_PS4_BOOT_POOLS_MODE_NAME "off"
#elif GTAV_PS4_BOOT_POOLS_MODE == 1
#define GTAV_PS4_BOOT_POOLS_MODE_NAME "observe"
#elif GTAV_PS4_BOOT_POOLS_MODE == 2
#define GTAV_PS4_BOOT_POOLS_MODE_NAME "caps"
#elif GTAV_PS4_BOOT_POOLS_MODE == 3
#define GTAV_PS4_BOOT_POOLS_MODE_NAME "full"
#else
#error "GTAV_PS4_BOOT_POOLS_MODE must be 0 (off), 1 (observe), 2 (caps) or 3 (full)"
#endif
#if GTAV_PS4_BOOT_POOLS_MODE == 3
#define GTAV_PS4_BOOT_POOLS_PROBE 0
#define GTAV_PS4_PLUGIN_DESC_SUFFIX ""
#else
#define GTAV_PS4_BOOT_POOLS_PROBE 1
#define GTAV_PS4_PLUGIN_DESC_SUFFIX " (boot-pool probe: " GTAV_PS4_BOOT_POOLS_MODE_NAME ")"
#endif
static_assert(GTAV_BOOT_POOL_DATA_MODE_OFF == 0 && GTAV_BOOT_POOL_DATA_MODE_OBSERVE == 1 &&
                  GTAV_BOOT_POOL_DATA_MODE_CAPS == 2 && GTAV_BOOT_POOL_DATA_MODE_FULL == 3,
              "GTAV_PS4_BOOT_POOLS_MODE numbers are the core's modes");

namespace {

constexpr uint32_t kPageMask = 0x3fff;  // 16 KiB pages
// FreeBSD's MAP_PRIVATE | MAP_ANON and open(2) flags: OpenOrbis's libkernel.h names none of them.
constexpr uint32_t kMapPrivateAnonymous = 0x0002 | 0x1000;
constexpr int32_t kOpenRead = 0x0000;
constexpr int32_t kOpenWriteNew = 0x0001 | 0x0200 | 0x0400;  // O_WRONLY | O_CREAT | O_TRUNC
constexpr uint32_t kPluginVersion = 0x00000100;
constexpr uint32_t kStartPollUsec = 100000;
constexpr uint32_t kActivePollUsec = 50000;
constexpr uint64_t kActivationHeartbeatUs = 10000000;
// A stopper that lost the race waits for the winner's join: longer than the worst-case stop
// (60 s for a shutdown behind a busy frame, 3 s quiesce, 60 s for a thread still inside the frame
// hook, plus one supervisor poll).
constexpr uint32_t kLifecycleWaitUsec = 10000;
constexpr uint32_t kLifecycleWaitPolls = 15000;

GtavPluginLifecycle g_lifecycle;
GtavPs4PluginRuntime g_runtime;
OrbisPthread g_thread;
uint32_t g_unload_requested;
int32_t g_thread_result;
GtavPs4BootPools g_boot_pools;
uint64_t g_load_us;

// tools/inspect_ps4_plugin.py reads the build's mode from this string (validation.json
// bootPoolsMode); plugin_load logs it.
const char kBootPoolsMarker[] = "gtavmenu-boot-pools-mode=" GTAV_PS4_BOOT_POOLS_MODE_NAME;

int query_map(void*, uint64_t address, GtavPs4PluginMapInfo* out) {
  void* start = NULL;
  void* end = NULL;
  int32_t prot = 0;
  if (sceKernelQueryMemoryProtection((void*)(uintptr_t)address, &start, &end, &prot) < 0) return -1;
  out->start = (uint64_t)(uintptr_t)start;
  out->end = (uint64_t)(uintptr_t)end;
  out->prot = (uint32_t)prot & 7u;
  out->name[0] = 0;
  return 0;
}

int name_map(void* context, uint64_t address, GtavPs4PluginMapInfo* out) {
  if (query_map(context, address, out) != 0) return -1;
  OrbisKernelVirtualQueryInfo info;
  memset(&info, 0, sizeof(info));
  if (sceKernelVirtualQuery((const void*)(uintptr_t)address, 0, &info, sizeof(info)) >= 0) {
    memcpy(out->name, info.name, sizeof(out->name));
    out->name[sizeof(out->name) - 1] = 0;
  }
  return 0;
}

// The pack engine's readability checks (pack_platform.c) use the kernel's protection query.
int protection_query(uintptr_t address, uintptr_t* start, uintptr_t* end, uint32_t* prot) {
  void* from = NULL;
  void* to = NULL;
  int32_t bits = 0;
  if (sceKernelQueryMemoryProtection((void*)address, &from, &to, &bits) < 0) return -1;
  *start = (uintptr_t)from;
  *end = (uintptr_t)to;
  *prot = (uint32_t)bits & 7u;
  return 0;
}

// An aligned word another thread may be writing (a boot state, a pointer being published) is read
// with one load, never byte by byte.
int read_memory(void*, uint64_t address, void* out, uint32_t size) {
  const uintptr_t at = (uintptr_t)address;
  if (size == 8 && (at & 7u) == 0) {
    const uint64_t value = __atomic_load_n((const uint64_t*)at, __ATOMIC_RELAXED);
    memcpy(out, &value, sizeof(value));
  } else if (size == 4 && (at & 3u) == 0) {
    const uint32_t value = __atomic_load_n((const uint32_t*)at, __ATOMIC_RELAXED);
    memcpy(out, &value, sizeof(value));
  } else if (size == 2 && (at & 1u) == 0) {
    const uint16_t value = __atomic_load_n((const uint16_t*)at, __ATOMIC_RELAXED);
    memcpy(out, &value, sizeof(value));
  } else {
    memcpy(out, (const void*)at, size);
  }
  return 0;
}

// Plain copy for the gateway page before it is published; live words go through cas64/cas32.
int write_memory(void*, uint64_t address, const void* data, uint32_t size) {
  memcpy((void*)(uintptr_t)address, data, size);
  return 0;
}

// Pages this plugin made writable with a successful sceKernelMprotect and has not protected again.
// On hardware the protection query keeps reporting a game code page as RX right after mprotect
// made it RWX (seen on hardware), so for pages the plugin unprotected itself the mprotect result is
// the authority. Only the supervisor thread protects pages and exchanges words.
struct WritableSpan {
  uint64_t start;
  uint64_t end;
};
constexpr uint32_t kWritableSpans = 8;
WritableSpan g_writable[kWritableSpans];

void note_protection(uint64_t start, uint64_t end, uint32_t prot) {
  for (WritableSpan& span : g_writable) {
    if (span.end > start && span.start < end) span = WritableSpan{0, 0};
  }
  if ((prot & GTAV_PS4_PLUGIN_PROT_WRITE) == 0) return;
  for (WritableSpan& span : g_writable) {
    if (span.end == 0) {
      span = WritableSpan{start, end};
      return;
    }
  }
}

bool in_writable_span(uint64_t address, uint32_t size) {
  for (const WritableSpan& span : g_writable) {
    if (span.end != 0 && span.start <= address && address + size <= span.end) return true;
  }
  return false;
}

// A compare-and-exchange must never fault on the game's memory: refuse a misaligned word, or one
// whose page is neither unprotected by this plugin nor reported writable by the kernel.
int writable_word(uint64_t address, uint32_t size) {
  if (!address || (address & (uint64_t)(size - 1u)) != 0) return 0;
  if (in_writable_span(address, size)) return 1;
  void* start = NULL;
  void* end = NULL;
  int32_t prot = 0;
  if (sceKernelQueryMemoryProtection((void*)(uintptr_t)address, &start, &end, &prot) < 0) return 0;
  return (prot & ORBIS_KERNEL_PROT_CPU_WRITE) != 0 && (uint64_t)(uintptr_t)start <= address &&
         address + size <= (uint64_t)(uintptr_t)end;
}

// Inline lock cmpxchg (aligned, seq_cst): a thread executing or loading the word sees either the
// old or the new value, never a mix.
int cas64(void*, uint64_t address, uint64_t expected, uint64_t desired, uint64_t* observed) {
  if (!writable_word(address, sizeof(uint64_t))) return GTAV_PS4_PLUGIN_CAS_REFUSED;
  uint64_t current = expected;
  const bool swapped = __atomic_compare_exchange_n((uint64_t*)(uintptr_t)address, &current, desired,
                                                   false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  if (observed) *observed = current;
  return swapped ? GTAV_PS4_PLUGIN_CAS_SWAPPED : GTAV_PS4_PLUGIN_CAS_DIFFERS;
}

int cas32(void*, uint64_t address, uint32_t expected, uint32_t desired, uint32_t* observed) {
  if (!writable_word(address, sizeof(uint32_t))) return GTAV_PS4_PLUGIN_CAS_REFUSED;
  uint32_t current = expected;
  const bool swapped = __atomic_compare_exchange_n((uint32_t*)(uintptr_t)address, &current, desired,
                                                   false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  if (observed) *observed = current;
  return swapped ? GTAV_PS4_PLUGIN_CAS_SWAPPED : GTAV_PS4_PLUGIN_CAS_DIFFERS;
}

int protect_memory(void*, uint64_t address, uint32_t size, uint32_t prot) {
  const uint64_t start = address & ~(uint64_t)kPageMask;
  const uint64_t end = (address + size + kPageMask) & ~(uint64_t)kPageMask;
  if (sceKernelMprotect((const void*)(uintptr_t)start, end - start, (int)prot) < 0) return -1;
  note_protection(start, end, prot);
  return 0;
}

int map_gateway(void*, uint64_t hint, uint32_t size, uint64_t* out) {
  void* mapped = NULL;
  const int result = sceKernelMmap(
      (void*)(uintptr_t)hint, size,
      ORBIS_KERNEL_PROT_CPU_READ | ORBIS_KERNEL_PROT_CPU_WRITE | ORBIS_KERNEL_PROT_CPU_EXEC,
      kMapPrivateAnonymous, -1, 0, &mapped);
  if (result < 0) return -1;
  if ((uint64_t)(uintptr_t)mapped != hint) {
    if (mapped) (void)sceKernelMunmap(mapped, size);
    return -1;
  }
  *out = (uint64_t)(uintptr_t)mapped;
  return 0;
}

int unmap_gateway(void*, uint64_t address, uint32_t size) {
  return sceKernelMunmap((void*)(uintptr_t)address, size) < 0 ? -1 : 0;
}

uint32_t process_id(void*) {
  return (uint32_t)getpid();
}

uint64_t now_us(void*) {
  return sceKernelGetProcessTime();
}

// Only the supervisor and unloading threads sleep: each poll writes the queued log lines first.
void sleep_us(void*, uint32_t usec) {
  (void)gtav_ps4_plugin_log_flush();
  (void)sceKernelUsleep(usec);
}

// Queued, never written here: any thread may log (see gtav_ps4_plugin_log_line).
void log_message(void*, const char* message) {
  gtav_ps4_plugin_log_line(message);
}

void notify(const char* message) {
  OrbisNotificationRequest request;
  memset(&request, 0, sizeof(request));
  request.type = NotificationRequest;
  request.targetId = -1;
  request.useIconImageUri = 1;
  snprintf(request.message, sizeof(request.message), "%s", message);
  snprintf(request.iconUri, sizeof(request.iconUri), "cxml://psnotification/tex_icon_system");
  (void)sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

const GtavPs4PluginOps kOrbisOps = {
    .context = NULL,
    .query = query_map,
    .map_name = name_map,
    .read = read_memory,
    .write = write_memory,
    .cas64 = cas64,
    .cas32 = cas32,
    .protect = protect_memory,
    .map = map_gateway,
    .unmap = unmap_gateway,
    .pid = process_id,
    .now_us = now_us,
    .sleep_us = sleep_us,
    .log = log_message,
};

int unload_requested() {
  return __atomic_load_n(&g_unload_requested, __ATOMIC_ACQUIRE) != 0;
}

int stop_requested(void*) {
  return unload_requested();
}

int story_mode_ready(void*) {
  return gtav_ps4_plugin_runtime_story_mode(&g_runtime);
}

// S0 and S1, before Story Mode. S0 writes the run header, the loader host and plugin_load to the
// log and reads the kill switch, which can only turn boot pools off. S1 (boot pools, data writes
// only) holds every log flush until it decides, then writes its lines.
void grow_boot_pools() {
  (void)gtav_ps4_plugin_log_flush();
  GtavPs4BootPoolsConfig config;
  memset(&config, 0, sizeof(config));
  config.mode = GTAV_PS4_BOOT_POOLS_MODE;
  config.off_reason = config.mode == GTAV_BOOT_POOL_DATA_MODE_OFF ? "build" : NULL;
  OrbisKernelStat st;
  memset(&st, 0, sizeof(st));
  if (config.mode != GTAV_BOOT_POOL_DATA_MODE_OFF &&
      sceKernelStat(GTAV_PS4_BOOT_POOLS_KILL_SWITCH, &st) == 0) {
    config.mode = GTAV_BOOT_POOL_DATA_MODE_OFF;
    config.off_reason = "kill-switch";
  }
  config.load_us = g_load_us;
  config.stop = stop_requested;
  config.story_mode = story_mode_ready;
  // A probe build notifies every result itself (below).
  config.notify = GTAV_PS4_BOOT_POOLS_PROBE ? NULL : notify;
  gtav_ps4_plugin_log_set_defer(1);
  const int status = gtav_ps4_boot_pools_run(&g_boot_pools, &g_runtime, &config);
  gtav_ps4_plugin_log_set_defer(0);
  (void)gtav_ps4_plugin_log_flush();
  if (GTAV_PS4_BOOT_POOLS_PROBE) {
    char message[128];
    snprintf(message, sizeof(message), "GTAV Menu probe build (boot pools %s): %s",
             GTAV_PS4_BOOT_POOLS_MODE_NAME, gtav_boot_pool_data_status_name(status));
    notify(message);
  }
}

// S2: one supervisor poll, with the one-shot boot-pool checks (store sizes, factory capacities).
void supervisor_poll(uint32_t usec) {
  (void)gtav_ps4_boot_pools_poll(&g_boot_pools);
  sleep_us(NULL, usec);
}

// "pid=<pid> gateway=0x<hex> shutdown=<0|1>\n" (GTAV_PS4_PLUGIN_UNCLEAN_STOP_PATH). 0 = parsed.
// strtoul/strtoull only: the PRX's sscanf has no 64-bit conversions.
int parse_unclean_marker(const char* text, uint32_t* pid, uint64_t* gateway, int* shutdown) {
  static const char kPid[] = "pid=";
  static const char kGateway[] = " gateway=0x";
  static const char kShutdown[] = " shutdown=";
  if (strncmp(text, kPid, sizeof(kPid) - 1) != 0) return -1;
  const char* cursor = text + sizeof(kPid) - 1;
  char* end = NULL;
  const unsigned long parsed_pid = strtoul(cursor, &end, 10);
  if (end == cursor || parsed_pid == 0 || parsed_pid > UINT32_MAX ||
      strncmp(end, kGateway, sizeof(kGateway) - 1) != 0) {
    return -1;
  }
  cursor = end + sizeof(kGateway) - 1;
  const unsigned long long parsed_gateway = strtoull(cursor, &end, 16);
  if (end == cursor || strncmp(end, kShutdown, sizeof(kShutdown) - 1) != 0) return -1;
  cursor = end + sizeof(kShutdown) - 1;
  if ((cursor[0] != '0' && cursor[0] != '1') || (cursor[1] != '\n' && cursor[1] != 0)) return -1;
  *pid = (uint32_t)parsed_pid;
  *gateway = (uint64_t)parsed_gateway;
  *shutdown = cursor[0] == '1';
  return 0;
}

// Before the start (outside the boot window): an earlier instance in this game process that
// stopped uncleanly left a marker. Its features get reset on the first active frame and its kept
// gateway is reclaimed; a marker from another process is stale. Read once, then removed.
void take_unclean_marker() {
  char text[96];
  const int fd = sceKernelOpen(GTAV_PS4_PLUGIN_UNCLEAN_STOP_PATH, kOpenRead, 0);
  if (fd < 0) return;
  const ssize_t length = sceKernelRead(fd, text, sizeof(text) - 1);
  (void)sceKernelClose(fd);
  (void)sceKernelUnlink(GTAV_PS4_PLUGIN_UNCLEAN_STOP_PATH);
  text[length > 0 ? length : 0] = 0;
  uint32_t pid = 0;
  uint64_t gateway = 0;
  int shutdown = 0;
  char message[160];
  if (parse_unclean_marker(text, &pid, &gateway, &shutdown) != 0) {
    log_message(NULL, "GTAV Menu plugin: removed a malformed unclean-stop marker");
    return;
  }
  if (pid != process_id(NULL)) {
    snprintf(message, sizeof(message),
             "GTAV Menu plugin: removed the unclean-stop marker of game process %u", (unsigned)pid);
    log_message(NULL, message);
    return;
  }
  g_runtime.reset_features = shutdown;
  g_runtime.stale_gateway = gateway;
  snprintf(message, sizeof(message),
           "GTAV Menu plugin: an earlier instance stopped uncleanly (reset features %s, gateway "
           "0x%llx)",
           shutdown ? "yes" : "no", (unsigned long long)gateway);
  log_message(NULL, message);
}

// After the stop: carry what the next instance in this process must fix, or remove the marker
// when the stop left nothing behind. A run that never started hands an earlier marker on.
void leave_unclean_marker() {
  const bool started = g_runtime.game_initialized != 0;
  const int shutdown = started ? g_runtime.shutdown_unfinished : g_runtime.reset_features;
  const uint64_t gateway =
      started ? (g_runtime.gateway_mapped ? g_runtime.gateway : 0u) : g_runtime.stale_gateway;
  if (!shutdown && !gateway) {
    (void)sceKernelUnlink(GTAV_PS4_PLUGIN_UNCLEAN_STOP_PATH);
    return;
  }
  char text[96];
  const int length = snprintf(text, sizeof(text), "pid=%u gateway=0x%llx shutdown=%d\n",
                              (unsigned)process_id(NULL), (unsigned long long)gateway, shutdown);
  (void)sceKernelMkdir(GTAV_PS4_PLUGIN_LOG_DIR, 0777);
  (void)sceKernelMkdir(GTAV_PS4_PLUGIN_LOG_DIR "/state", 0777);
  const int fd = sceKernelOpen(GTAV_PS4_PLUGIN_UNCLEAN_STOP_PATH, kOpenWriteNew, 0666);
  if (fd < 0) {
    log_message(NULL, "GTAV Menu plugin: could not write the unclean-stop marker");
    return;
  }
  const bool written = length > 0 && sceKernelWrite(fd, text, (size_t)length) == (size_t)length;
  (void)sceKernelClose(fd);
  char message[160];
  snprintf(message, sizeof(message), "GTAV Menu plugin: unclean-stop marker %s: %.*s",
           written ? "written" : "write failed", length > 1 ? length - 1 : 0, text);
  log_message(NULL, message);
}

// 0 once the hook is in, 1 when an unload came first, negative on a fatal start error (the start
// already cleaned up after itself).
int wait_story_mode() {
  while (!unload_requested()) {
    const int started = gtav_ps4_plugin_runtime_try_start(&g_runtime);
    if (started != 1) return started;
    supervisor_poll(kStartPollUsec);
  }
  return 1;
}

// 1 once the menu passed its activation gates, 0 when an unload came first, negative when it
// failed. No deadline while no hooked frame has run (the hook only chains the original then; a
// loading screen can last minutes); after the first, GTAV_PS4_PLUGIN_ACTIVATION_FRAMES of them.
int wait_activation() {
  uint64_t heartbeat = now_us(NULL);
  while (!unload_requested()) {
    const int live = gtav_ps4_plugin_runtime_activated(&g_runtime);
    if (live != 0) return live;
    const uint64_t frames = gtav_ps4_plugin_runtime_hooked_frames(&g_runtime);
    if (frames >= GTAV_PS4_PLUGIN_ACTIVATION_FRAMES) return GTAV_PS4_PLUGIN_ERROR_ACTIVATION;
    const uint64_t now = now_us(NULL);
    if (now - heartbeat >= kActivationHeartbeatUs) {
      heartbeat = now;
      char message[96];
      snprintf(message, sizeof(message),
               "GTAV Menu plugin: waiting for native activation (%llu hooked frames)",
               (unsigned long long)frames);
      log_message(NULL, message);
    }
    supervisor_poll(kActivePollUsec);
  }
  return 0;
}

// Active: /data goes to the game thread now (the profile import), then poll until a stop.
void run_until_stop() {
  log_message(NULL, "GTAV Menu plugin: active (R1 + D-pad Left)");
  notify(GTAV_PS4_BOOT_POOLS_PROBE ? "GTAV Menu PROBE build loaded (R1 + D-pad Left)"
                                   : "GTAV Menu loaded (R1 + D-pad Left)");
  if (gtav_ps4_plugin_runtime_grant_storage(&g_runtime)) {
    log_message(NULL, "GTAV Menu plugin: /data granted to the menu");
  }
  while (!unload_requested() && !gtav_ps4_plugin_runtime_menu_stop_requested(&g_runtime)) {
    supervisor_poll(kActivePollUsec);
  }
}

// The only thread besides the game's: it grows the boot pools when it is early enough, waits for
// Story Mode, installs the hook, watches for a stop and cleans up. It never runs a GTA native
// (those run only on the hooked game thread); whatever boot pools decide, the menu still starts.
void* supervisor_main(void*) {
  grow_boot_pools();
  take_unclean_marker();
  int result = wait_story_mode();
  int menu_stopped = 0;  // the menu's Stop Runtime ended the session, not the host's unload
  if (result >= 0) {
    if (result == 0) {
      const int live = wait_activation();
      if (live > 0) run_until_stop();
      menu_stopped = live > 0 && !unload_requested();
      result = live < 0 ? live : 0;
    } else {
      result = 0;
    }
    const int cleanup = gtav_ps4_plugin_runtime_cleanup(&g_runtime);
    if (result == 0) result = cleanup;
  }
  leave_unclean_marker();
  // A host unload notifies once it is joined; GoldHEN cannot unload, so its stop says so here.
  if (menu_stopped && result == 0 && !g_runtime.unload_unsafe) notify("GTAV Menu stopped");

  if (g_runtime.unload_unsafe) {
    log_message(NULL, "GTAV Menu plugin: UNSAFE: do not unload the plugin; restart the game");
    notify("GTAV Menu UNSAFE: do not unload; restart GTA V");
  }
  if (result < 0) {
    char message[160];
    snprintf(message, sizeof(message), "GTAV Menu plugin failed: %s",
             gtav_ps4_plugin_error_string(result));
    log_message(NULL, message);
    notify(message);
  }
  {
    char message[96];
    snprintf(message, sizeof(message), "GTAV Menu plugin: supervisor done (%d)", result);
    log_message(NULL, message);
  }
  (void)gtav_ps4_plugin_log_flush();
  __atomic_store_n(&g_thread_result, result, __ATOMIC_RELEASE);
  return NULL;
}

int32_t start_plugin() {
  (void)gtav_plugin_lifecycle_rearm(&g_lifecycle);
  if (!gtav_plugin_lifecycle_claim(&g_lifecycle, GTAV_PLUGIN_IDLE, GTAV_PLUGIN_STARTING)) {
    // Already starting, running or stopping; a stop whose cleanup failed never starts again.
    return gtav_plugin_lifecycle_state(&g_lifecycle) == GTAV_PLUGIN_STOPPED
               ? gtav_plugin_lifecycle_result(&g_lifecycle)
               : 0;
  }
  g_load_us = now_us(NULL);
  gtav_ps4_plugin_log_begin(kPluginVersion, GTAV_PS4_TARGET_ID);
  // The pack breadcrumbs (gtav_worker_klog) join the log; the ring never blocks the game thread.
  gtav_ps4_set_klog_sink(gtav_ps4_plugin_log_line);
  log_message(NULL, "GTAV Menu plugin: plugin_load");
  {
    char message[96];
    snprintf(message, sizeof(message), "GTAV Menu plugin: %s%s", kBootPoolsMarker,
             GTAV_PS4_BOOT_POOLS_PROBE ? " (probe build, not for release)" : "");
    log_message(NULL, message);
  }
  gtav_ps4_plugin_runtime_init(&g_runtime, &kOrbisOps);
  gtav_platform_set_protection_query(protection_query);
  __atomic_store_n(&g_unload_requested, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_thread_result, 0, __ATOMIC_RELEASE);
  if (scePthreadCreate(&g_thread, NULL, supervisor_main, NULL, "gtav_menu_plugin") < 0) {
    g_runtime.state = GTAV_PS4_PLUGIN_FAILED;
    g_runtime.last_error = GTAV_PS4_PLUGIN_ERROR_STATE;
    log_message(NULL, "GTAV Menu plugin: could not create supervisor thread");
    (void)gtav_ps4_plugin_log_flush();
    gtav_plugin_lifecycle_finish(&g_lifecycle, GTAV_PLUGIN_START_FAILED, -1, 1);
    return -1;
  }
  gtav_plugin_lifecycle_finish(&g_lifecycle, GTAV_PLUGIN_RUNNING, 0, 0);
  return 0;
}

// Only the caller that moved RUNNING to STOPPING gets here: the one join.
int32_t join_supervisor() {
  __atomic_store_n(&g_unload_requested, 1u, __ATOMIC_RELEASE);
  (void)scePthreadJoin(g_thread, NULL);
  const int32_t result = __atomic_load_n(&g_thread_result, __ATOMIC_ACQUIRE);
  {
    char message[96];
    snprintf(message, sizeof(message), "GTAV Menu plugin: unloaded (%d)", result);
    log_message(NULL, message);
  }
  // A failed or unsafe stop already notified its own message.
  if (result == 0 && !g_runtime.unload_unsafe) notify("GTAV Menu unloaded");
  (void)gtav_ps4_plugin_log_flush();
  gtav_plugin_lifecycle_finish(&g_lifecycle, GTAV_PLUGIN_STOPPED, result,
                               g_runtime.cleanup_result == 0);
  return result;
}

int32_t stop_plugin() {
  for (uint32_t i = 0; i < kLifecycleWaitPolls; ++i) {
    switch (gtav_plugin_lifecycle_state(&g_lifecycle)) {
      case GTAV_PLUGIN_IDLE:
      case GTAV_PLUGIN_START_FAILED:
        return 0;
      case GTAV_PLUGIN_STOPPED:
        return gtav_plugin_lifecycle_result(&g_lifecycle);
      case GTAV_PLUGIN_RUNNING:
        if (gtav_plugin_lifecycle_claim(&g_lifecycle, GTAV_PLUGIN_RUNNING, GTAV_PLUGIN_STOPPING)) {
          return join_supervisor();
        }
        break;
      default:  // STARTING, or another caller's STOPPING
        break;
    }
    sleep_us(NULL, kLifecycleWaitUsec);
  }
  log_message(NULL, "GTAV Menu plugin: gave up waiting for another unload");
  return GTAV_PS4_PLUGIN_ERROR_STATE;
}

}  // namespace

__attribute__((used, visibility("default"))) const char* g_pluginName = "gtav_menu";
__attribute__((used, visibility("default"))) const char* g_pluginDesc =
    "GTA V Legacy menu for " GTAV_PS4_PLUGIN_TITLE_ID
    " " GTAV_PS4_PLUGIN_CONTENT_VERSION GTAV_PS4_PLUGIN_DESC_SUFFIX;
__attribute__((used, visibility("default"))) const char* g_pluginAuth = "GTAV Menu contributors";
__attribute__((used, visibility("default"))) uint32_t g_pluginVersion = kPluginVersion;

extern "C" __attribute__((used, visibility("default"))) int32_t plugin_load(int32_t argc,
                                                                            const char* argv[]) {
  (void)argc;
  (void)argv;
  return start_plugin();
}

extern "C" __attribute__((used, visibility("default"))) int32_t plugin_unload(int32_t argc,
                                                                              const char* argv[]) {
  (void)argc;
  (void)argv;
  return stop_plugin();
}

extern "C" __attribute__((weak, visibility("hidden"))) int32_t module_start(size_t argc,
                                                                            const void* args) {
  (void)argc;
  (void)args;
  return 0;
}

extern "C" __attribute__((weak, visibility("hidden"))) int32_t module_stop(size_t argc,
                                                                           const void* args) {
  (void)argc;
  (void)args;
  return plugin_unload(0, NULL);
}
