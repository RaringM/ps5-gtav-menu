// Lifecycle-only nexHEN feasibility probe (make nexhen NEXHEN_PROBE=1).
//
// Stock nexHEN ptrace-attaches to the running game to load a native plugin, and an attach on a
// steady-state 01.010.002 process permanently jams the streaming I/O completion ring
// (research/docs/streaming-re-01.010.002.md, "LADDER RESULT"). This module answers whether an
// autoload attach at title start is early enough to be survivable. It installs no hooks and never
// writes game memory: it records how long after process start plugin_load ran, then samples the
// ring's head/tail/in-flight counters from a read-only thread and reports the same HEALTHY/JAMMED
// verdict as research/tools/live/probe_streaming_state.py.

#include "gtavmenu/nexhen_plugin.h"
#include "gtavmenu/notify.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef GTAV_NEXHEN_TITLE_ID
#define GTAV_NEXHEN_TITLE_ID "unsupported"
#endif
#ifndef GTAV_NEXHEN_CONTENT_VERSION
#define GTAV_NEXHEN_CONTENT_VERSION "unsupported"
#endif

// Streaming I/O ring (32 slots) of PPSA04264 01.010.002, runtime addresses (eboot maps at its fixed
// link base). Pinned with probe_streaming_state.py; data segment, never .text.
#define NX_PROBE_RING_HEAD 0x6257D10ull
#define NX_PROBE_RING_TAIL 0x6257D14ull
#define NX_PROBE_RING_INFLIGHT 0x6257D18ull
#define NX_PROBE_SAMPLE_USEC 1000000u
#define NX_PROBE_WINDOW 15u
#define NX_PROBE_LOG_PRIMARY "/data/GTAVMenu/nexhen-probe.log"
#define NX_PROBE_LOG_FALLBACK "/user/data/GTAVMenu/nexhen-probe.log"
// The game runs in its sandbox at plugin_load (measured on stock v1.00), so only sandbox paths
// resolve. app0 carries no param.json; nexHEN already binds the plugin to the title section.
#define NX_PROBE_EBOOT "/app0/eboot.bin"

enum { NX_VERDICT_INDETERMINATE, NX_VERDICT_HEALTHY, NX_VERDICT_JAMMED };

typedef struct NxRingSample {
  uint32_t head;
  uint32_t tail;
  uint32_t inflight;
} NxRingSample;

uint64_t sceKernelGetProcessTime(void);
int sceKernelQueryMemoryProtection(void* addr, void** start, void** end, int* prot);

GTAV_NEXHEN_PUBLIC const char* g_pluginName = "GTAV Menu nexHEN probe";
GTAV_NEXHEN_PUBLIC const char* g_pluginDesc = "Read-only streaming health probe; installs no hooks";
GTAV_NEXHEN_PUBLIC const char* g_pluginAuth = "Miles Richey and contributors";
GTAV_NEXHEN_PUBLIC uint32_t g_pluginVersion = 0x00000001u;

static pthread_t g_sampler;
static atomic_int g_sampler_running;
static atomic_int g_sampler_stop;
static int g_log_fd = -1;

static void probe_log(const char* fmt, ...) {
  char line[512];
  va_list args;
  int prefix =
      snprintf(line, sizeof(line), "[%llu us] ", (unsigned long long)sceKernelGetProcessTime());
  if (prefix < 0) return;
  va_start(args, fmt);
  int body = vsnprintf(line + prefix, sizeof(line) - (size_t)prefix - 1u, fmt, args);
  va_end(args);
  if (body < 0) return;
  size_t length = strnlen(line, sizeof(line) - 1u);
  line[length++] = '\n';
  if (g_log_fd >= 0) (void)write(g_log_fd, line, length);
}

static void probe_log_open(void) {
  mkdir("/data", 0777);
  mkdir("/data/GTAVMenu", 0777);
  g_log_fd = open(NX_PROBE_LOG_PRIMARY, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (g_log_fd >= 0) return;
  mkdir("/user/data/GTAVMenu", 0777);
  g_log_fd = open(NX_PROBE_LOG_FALLBACK, O_WRONLY | O_CREAT | O_APPEND, 0666);
}

// Confirms the game image is mounted and reports its eboot size. The title comes from nexHEN's
// [TITLEID] binding; the size lets the log distinguish builds after the fact.
static int read_title(char* title, size_t title_size, char* version, size_t version_size) {
  struct stat st;
  if (stat(NX_PROBE_EBOOT, &st) != 0) return -1;
  snprintf(title, title_size, "%s", GTAV_NEXHEN_TITLE_ID);
  snprintf(version, version_size, "eboot-%lld", (long long)st.st_size);
  return 1;
}

static int readable(uint64_t address, size_t size) {
  void* start = NULL;
  void* end = NULL;
  int prot = 0;
  if (sceKernelQueryMemoryProtection((void*)(uintptr_t)address, &start, &end, &prot) != 0) return 0;
  return (prot & 1) != 0 && (uintptr_t)end >= address + size;
}

static int sample_ring(NxRingSample* sample) {
  if (!readable(NX_PROBE_RING_HEAD, 12u)) return -1;
  sample->head = *(volatile const uint32_t*)(uintptr_t)NX_PROBE_RING_HEAD;
  sample->tail = *(volatile const uint32_t*)(uintptr_t)NX_PROBE_RING_TAIL;
  sample->inflight = *(volatile const uint32_t*)(uintptr_t)NX_PROBE_RING_INFLIGHT;
  return 0;
}

// Same scoring as probe_streaming_state.verdict_for: the discriminator is tail, not head.
static int window_verdict(const NxRingSample* samples, uint32_t count) {
  int tail_advanced = 0;
  int drained = 0;
  if (count < 2u) return NX_VERDICT_INDETERMINATE;
  for (uint32_t i = 0; i < count; ++i) {
    if (samples[i].tail != samples[0].tail) tail_advanced = 1;
    if (samples[i].inflight == 0u) drained = 1;
  }
  if (tail_advanced && drained) return NX_VERDICT_HEALTHY;
  if (!tail_advanced && !drained) return NX_VERDICT_JAMMED;
  return NX_VERDICT_INDETERMINATE;
}

static const char* verdict_name(int verdict) {
  switch (verdict) {
    case NX_VERDICT_HEALTHY:
      return "HEALTHY";
    case NX_VERDICT_JAMMED:
      return "JAMMED";
    default:
      return "INDETERMINATE";
  }
}

// nexHEN rewrites the game's rootdir/jaildir when it elevates it. Logs whether the game's own
// sandbox paths still resolve, which separates "elevation left the game unable to reach /app0 or
// its save mount" from a streaming stall.
static void log_sandbox_view(const char* when) {
  struct stat st;
  int app0 = stat("/app0/eboot.bin", &st) == 0;
  int savedata = stat("/savedata0", &st) == 0;
  int data = stat("/data", &st) == 0;
  int real_root = stat("/user/appmeta", &st) == 0 || stat("/mnt/sandbox", &st) == 0;
  probe_log("sandbox view %s: /app0=%d /savedata0=%d /data=%d real-root=%d", when, app0, savedata,
            data, real_root);
}

static void* sampler_main(void* argument) {
  NxRingSample window[NX_PROBE_WINDOW];
  uint32_t count = 0;
  uint32_t windows = 0;
  int last_reported = -1;
  uint32_t ticks = 0;
  (void)argument;

  while (!atomic_load_explicit(&g_sampler_stop, memory_order_acquire)) {
    NxRingSample sample;
    if (ticks == 1u || ticks == 5u || ticks % 30u == 0u) log_sandbox_view("sampler");
    ++ticks;
    if (sample_ring(&sample) == 0) {
      probe_log("ring head=%u tail=%u inflight=%u", sample.head, sample.tail, sample.inflight);
      window[count++] = sample;
      if (count == NX_PROBE_WINDOW) {
        int verdict = window_verdict(window, count);
        ++windows;
        probe_log("window %u verdict %s", windows, verdict_name(verdict));
        // Notify on the first decisive verdict and on every change between decisive verdicts,
        // so the operator sees a JAMMED transition without polling the log.
        if (verdict != NX_VERDICT_INDETERMINATE && verdict != last_reported) {
          char message[128];
          snprintf(message, sizeof(message), "GTAV nexHEN probe: streaming %s (window %u)",
                   verdict_name(verdict), windows);
          gtav_notify(message);
          last_reported = verdict;
        }
        count = 0;
      }
    } else {
      probe_log("ring unreadable");
    }
    for (uint32_t waited = 0; waited < NX_PROBE_SAMPLE_USEC &&
                              !atomic_load_explicit(&g_sampler_stop, memory_order_acquire);
         waited += 50000u) {
      usleep(50000u);
    }
  }
  return NULL;
}

int gtav_nexhen_plugin_load(int32_t argc, const char* argv[]) {
  char title[32];
  char version[32];
  NxRingSample at_load;
  uint64_t uptime = sceKernelGetProcessTime();
  (void)argc;
  (void)argv;

  if (atomic_load_explicit(&g_sampler_running, memory_order_acquire)) return 0;

  probe_log_open();
  log_sandbox_view("plugin_load");
  memset(title, 0, sizeof(title));
  memset(version, 0, sizeof(version));
  int title_result = read_title(title, sizeof(title), version, sizeof(version));
  probe_log("plugin_load pid=%d uptime_us=%llu title=%s version=%s (result %d) expect=%s %s",
            (int)getpid(), (unsigned long long)uptime, title, version, title_result,
            GTAV_NEXHEN_TITLE_ID, GTAV_NEXHEN_CONTENT_VERSION);
  if (title_result < 0 || strcmp(title, GTAV_NEXHEN_TITLE_ID) != 0 ||
      (title_result == 0 && strcmp(version, GTAV_NEXHEN_CONTENT_VERSION) != 0)) {
    probe_log("title mismatch; probe idle");
    gtav_notify("GTAV nexHEN probe: wrong title, idle");
    return -1;
  }
  if (sample_ring(&at_load) == 0) {
    probe_log("ring at load head=%u tail=%u inflight=%u", at_load.head, at_load.tail,
              at_load.inflight);
  } else {
    probe_log("ring at load unreadable");
  }

  atomic_store_explicit(&g_sampler_stop, 0, memory_order_release);
  if (pthread_create(&g_sampler, NULL, sampler_main, NULL) != 0) {
    probe_log("sampler thread create failed");
    return -1;
  }
  atomic_store_explicit(&g_sampler_running, 1, memory_order_release);

  char message[128];
  snprintf(message, sizeof(message), "GTAV nexHEN probe loaded %llu ms after launch",
           (unsigned long long)(uptime / 1000u));
  gtav_notify(message);
  return 0;
}

int gtav_nexhen_plugin_unload(int32_t argc, const char* argv[]) {
  (void)argc;
  (void)argv;
  if (atomic_exchange_explicit(&g_sampler_running, 0, memory_order_acq_rel)) {
    atomic_store_explicit(&g_sampler_stop, 1, memory_order_release);
    pthread_join(g_sampler, NULL);
  }
  probe_log("plugin_unload");
  if (g_log_fd >= 0) {
    close(g_log_fd);
    g_log_fd = -1;
  }
  return 0;
}

int gtav_nexhen_module_start(size_t size, const void* args) {
  (void)size;
  (void)args;
  return 0;
}

int gtav_nexhen_module_stop(size_t size, const void* args) {
  (void)size;
  (void)args;
  return gtav_nexhen_plugin_unload(0, NULL);
}
