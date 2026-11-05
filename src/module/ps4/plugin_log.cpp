// The PS4 plugin log (see gtavmenu/ps4_plugin_log.h): a lock-free line ring any thread may add
// to, written to /data/GTAVMenu/ps4-plugin.log by whoever flushes (the supervisor thread).

#include "gtavmenu/ps4_plugin_log.h"

#include <orbis/libkernel.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace {

constexpr int32_t kOpenRead = 0x0000;
constexpr int32_t kOpenAppend = 0x0001 | 0x0008 | 0x0200;  // O_WRONLY | O_APPEND | O_CREAT
constexpr uint32_t kSlotMask = GTAV_PS4_PLUGIN_LOG_SLOTS - 1u;
constexpr uint32_t kBatchBytes = 4096;
constexpr uint32_t kMaxModules = 256;
constexpr uint32_t kIniBytes = 4096;
constexpr uint32_t kStatDirectory = 0040000;
constexpr uint32_t kStatTypeMask = 0170000;
// A long run checks the size again after this many bytes, so one run cannot grow the log unbounded.
constexpr uint64_t kRecheckBytes = 64u * 1024u;

static_assert((GTAV_PS4_PLUGIN_LOG_SLOTS & kSlotMask) == 0, "the slot count must be a power of 2");

struct Slot {
  uint32_t seq;  // n + 1 once line n is complete, 0 while a writer fills the slot
  uint32_t length;
  uint64_t process_us;
  char text[GTAV_PS4_PLUGIN_LOG_TEXT_BYTES];
};

Slot g_slots[GTAV_PS4_PLUGIN_LOG_SLOTS];
uint32_t g_next;      // lines claimed by writers
uint32_t g_flushing;  // try-lock: one flusher at a time
uint32_t g_defer;
uint32_t g_header_pending;

// Owned by the flusher (or by gtav_ps4_plugin_log_begin, before any flush of the run).
uint32_t g_written;
uint32_t g_lost;
uint32_t g_version;
uint64_t g_load_us;
int g_rotation_pending;
int g_directory_ready;
uint64_t g_bytes_since_check;
char g_target[48];
char g_host[32];
char g_batch[kBatchBytes];
char g_ini[kIniBytes];

char lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

int contains_word(const char* text, const char* needle) {
  for (; *text; ++text) {
    size_t i = 0;
    while (needle[i] && lower(text[i]) == lower(needle[i])) ++i;
    if (!needle[i]) return 1;
  }
  return 0;
}

void set_host(const char* name, const char* evidence) {
  snprintf(g_host, sizeof(g_host), "%s(%s)", name, evidence);
}

// The loader's own module mapped into the game process names it best.
const char* host_from_modules() {
  static OrbisKernelModule modules[kMaxModules];
  size_t count = 0;
  if (sceKernelGetModuleList(modules, kMaxModules, &count) < 0) return NULL;
  if (count > kMaxModules) count = kMaxModules;
  for (size_t i = 0; i < count; ++i) {
    OrbisKernelModuleInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetModuleInfo(modules[i], &info) < 0) continue;
    info.name[sizeof(info.name) - 1] = 0;
    if (contains_word(info.name, "goldhen")) return "goldhen";
    if (contains_word(info.name, "nexhen")) return "nexhen";
  }
  return NULL;
}

// 1 when an uncommented line of the loader's plugins.ini names this plugin.
int ini_lists_plugin(const char* path) {
  const int fd = sceKernelOpen(path, kOpenRead, 0);
  if (fd < 0) return 0;
  size_t used = 0;
  while (used < sizeof(g_ini) - 1) {
    const int64_t n = (int64_t)sceKernelRead(fd, g_ini + used, sizeof(g_ini) - 1 - used);
    if (n <= 0) break;
    used += (size_t)n;
  }
  (void)sceKernelClose(fd);
  g_ini[used] = 0;
  for (char* line = g_ini; *line;) {
    char* end = line;
    while (*end && *end != '\n') ++end;
    const char saved = *end;
    *end = 0;
    const char* start = line;
    while (*start == ' ' || *start == '\t') ++start;
    const int listed = *start != ';' && *start != '#' && contains_word(start, "gtav_menu.prx");
    *end = saved;
    if (listed) return 1;
    line = saved ? end + 1 : end;
  }
  return 0;
}

int is_directory(const char* path) {
  OrbisKernelStat st;
  memset(&st, 0, sizeof(st));
  return sceKernelStat(path, &st) == 0 && ((uint32_t)st.st_mode & kStatTypeMask) == kStatDirectory;
}

// A console set up for both loaders lists the plugin in both plugins.ini files; neither file says
// which loader booted, so the label names both rather than guess.
void detect_host() {
  const char* module = host_from_modules();
  const int goldhen = module ? 0 : ini_lists_plugin("/data/GoldHEN/plugins.ini");
  const int nexhen = module ? 0 : ini_lists_plugin("/data/nexHEN/plugins.ini");
  if (module) {
    set_host(module, "module");
  } else if (goldhen && nexhen) {
    set_host("goldhen|nexhen", "plugins.ini");
  } else if (goldhen) {
    set_host("goldhen", "plugins.ini");
  } else if (nexhen) {
    set_host("nexhen", "plugins.ini");
  } else if (is_directory("/data/GoldHEN")) {
    set_host("goldhen", "dir");
  } else if (is_directory("/data/nexHEN")) {
    set_host("nexhen", "dir");
  } else {
    snprintf(g_host, sizeof(g_host), "unknown");
  }
}

// 1 when the log was over the limit and moved aside.
int rotate_if_large() {
  OrbisKernelStat st;
  memset(&st, 0, sizeof(st));
  return sceKernelStat(GTAV_PS4_PLUGIN_LOG_PATH, &st) == 0 &&
         (uint64_t)st.st_size > GTAV_PS4_PLUGIN_LOG_ROTATE_BYTES &&
         sceKernelRename(GTAV_PS4_PLUGIN_LOG_PATH, GTAV_PS4_PLUGIN_LOG_ROTATED_PATH) == 0;
}

// Days since 1970-01-01 to a proleptic Gregorian date (H. Hinnant's civil_from_days).
void civil_from_days(int64_t days, int64_t* year, uint32_t* month, uint32_t* day) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const uint32_t doe = (uint32_t)(days - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  *day = doy - (153 * mp + 2) / 5 + 1;
  *month = mp < 10 ? mp + 3 : mp - 9;
  *year = (int64_t)yoe + era * 400 + (*month <= 2 ? 1 : 0);
}

struct Batch {
  int fd;
  size_t used;
  int failed;
  uint64_t written;

  void drain() {
    if (used && !failed) {
      const int64_t n = (int64_t)sceKernelWrite(fd, g_batch, used);
      if (n != (int64_t)used) failed = 1;
      if (n > 0) written += (uint64_t)n;
    }
    used = 0;
  }

  void append(const char* text, size_t length) {
    if (used + length > sizeof(g_batch)) drain();
    if (length > sizeof(g_batch)) length = sizeof(g_batch);
    memcpy(g_batch + used, text, length);
    used += length;
  }
};

void append_line(Batch* batch, int64_t wall_ms, uint64_t process_us, const char* text) {
  char utc[32];
  if (gtav_ps4_plugin_log_format_utc(wall_ms, utc, sizeof(utc)) != 0) utc[0] = 0;
  char line[GTAV_PS4_PLUGIN_LOG_TEXT_BYTES + 128];
  const int n =
      snprintf(line, sizeof(line), "[%s +%llu.%06llus pid=%u host=%s] %s\n", utc,
               (unsigned long long)(process_us / 1000000u),
               (unsigned long long)(process_us % 1000000u), (unsigned)getpid(), g_host, text);
  if (n > 0) batch->append(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

int flush_locked() {
  const uint32_t end = __atomic_load_n(&g_next, __ATOMIC_ACQUIRE);
  if (end == g_written && !__atomic_load_n(&g_header_pending, __ATOMIC_ACQUIRE) && !g_lost) {
    return 0;
  }
  if (!g_host[0]) detect_host();
  if (!g_directory_ready) {
    (void)sceKernelMkdir(GTAV_PS4_PLUGIN_LOG_DIR, 0777);
    g_directory_ready = 1;
  }
  if (g_rotation_pending) {
    // Rotated mid-run: the new file starts with this run's header again.
    if (rotate_if_large() && g_bytes_since_check) {
      __atomic_store_n(&g_header_pending, 1u, __ATOMIC_RELEASE);
    }
    g_rotation_pending = 0;
    g_bytes_since_check = 0;
  }
  const int header = __atomic_load_n(&g_header_pending, __ATOMIC_ACQUIRE) != 0;
  const int fd = sceKernelOpen(GTAV_PS4_PLUGIN_LOG_PATH, kOpenAppend, 0666);
  if (fd < 0) return -1;

  // One clock pair per flush: each line's UTC is this anchor moved by its process-time offset.
  OrbisKernelTimeval now;
  memset(&now, 0, sizeof(now));
  (void)sceKernelGettimeofday(&now);
  const uint64_t anchor_us = sceKernelGetProcessTime();
  const int64_t anchor_ms = (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_usec / 1000;
  const auto wall_ms = [anchor_us, anchor_ms](uint64_t process_us) {
    return anchor_ms + ((int64_t)process_us - (int64_t)anchor_us) / 1000;
  };

  Batch batch = {fd, 0, 0, 0};
  if (header) {
    char line[160];
    const int n =
        snprintf(line, sizeof(line), "=== gtav_menu.prx 0x%08x target=%s load=+%llu.%03llus ===\n",
                 (unsigned)g_version, g_target, (unsigned long long)(g_load_us / 1000000u),
                 (unsigned long long)(g_load_us % 1000000u / 1000u));
    if (n > 0) batch.append(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    __atomic_store_n(&g_header_pending, 0u, __ATOMIC_RELEASE);
  }
  if (end - g_written > GTAV_PS4_PLUGIN_LOG_SLOTS) {
    g_lost += end - g_written - GTAV_PS4_PLUGIN_LOG_SLOTS;
    g_written = end - GTAV_PS4_PLUGIN_LOG_SLOTS;
  }
  uint32_t n = g_written;
  for (; n != end; ++n) {
    const Slot* slot = &g_slots[n & kSlotMask];
    const uint32_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
    if (seq != n + 1u) {
      // Still being written (the next flush takes it), or already overwritten by a later line.
      if (seq == 0u || (int32_t)(seq - (n + 1u)) < 0) break;
      ++g_lost;
      continue;
    }
    char text[GTAV_PS4_PLUGIN_LOG_TEXT_BYTES];
    uint32_t length = slot->length;
    if (length >= sizeof(text)) length = sizeof(text) - 1;
    memcpy(text, slot->text, length);
    text[length] = 0;
    const uint64_t stamp = slot->process_us;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&slot->seq, __ATOMIC_RELAXED) != n + 1u) {
      ++g_lost;
      continue;
    }
    if (g_lost) {
      char note[64];
      snprintf(note, sizeof(note), "[%u earlier lines lost]", (unsigned)g_lost);
      append_line(&batch, wall_ms(stamp), stamp, note);
      g_lost = 0;
    }
    append_line(&batch, wall_ms(stamp), stamp, text);
  }
  g_written = n;
  if (g_lost && n == end) {
    char note[64];
    snprintf(note, sizeof(note), "[%u earlier lines lost]", (unsigned)g_lost);
    append_line(&batch, anchor_ms, anchor_us, note);
    g_lost = 0;
  }
  batch.drain();
  (void)sceKernelClose(fd);
  g_bytes_since_check += batch.written;
  if (g_bytes_since_check >= kRecheckBytes) g_rotation_pending = 1;
  return batch.failed ? -1 : 0;
}

}  // namespace

void gtav_ps4_plugin_log_begin(uint32_t version, const char* target) {
  g_version = version;
  snprintf(g_target, sizeof(g_target), "%s", target ? target : "unknown");
  g_load_us = sceKernelGetProcessTime();
  g_rotation_pending = 1;
  __atomic_store_n(&g_header_pending, 1u, __ATOMIC_RELEASE);
}

void gtav_ps4_plugin_log_line(const char* message) {
  if (!message) return;
  const uint64_t now = sceKernelGetProcessTime();
  const uint32_t n = __atomic_fetch_add(&g_next, 1u, __ATOMIC_ACQ_REL);
  Slot* slot = &g_slots[n & kSlotMask];
  // Seqlock writer: mark the slot in progress, and fence so no text store moves ahead of the mark
  // (a release store alone orders only the stores before it).
  __atomic_store_n(&slot->seq, 0u, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  uint32_t length = 0;
  for (; message[length] && length < sizeof(slot->text) - 1; ++length) {
    const char c = message[length];
    slot->text[length] = (c == '\n' || c == '\r') ? ' ' : c;
  }
  slot->text[length] = 0;
  slot->length = length;
  slot->process_us = now;
  __atomic_store_n(&slot->seq, n + 1u, __ATOMIC_RELEASE);
}

void gtav_ps4_plugin_log_set_defer(int defer) {
  __atomic_store_n(&g_defer, defer ? 1u : 0u, __ATOMIC_RELEASE);
}

int gtav_ps4_plugin_log_flush(void) {
  if (__atomic_load_n(&g_defer, __ATOMIC_ACQUIRE)) return 0;
  uint32_t idle = 0;
  if (!__atomic_compare_exchange_n(&g_flushing, &idle, 1u, false, __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE)) {
    return 0;
  }
  const int result = flush_locked();
  __atomic_store_n(&g_flushing, 0u, __ATOMIC_RELEASE);
  return result;
}

const char* gtav_ps4_plugin_log_host(void) {
  return g_host[0] ? g_host : "unknown";
}

int gtav_ps4_plugin_log_format_utc(int64_t unix_ms, char* out, size_t size) {
  if (!out || size < 25) return -1;
  int64_t seconds = unix_ms / 1000;
  int64_t millis = unix_ms % 1000;
  if (millis < 0) {
    millis += 1000;
    --seconds;
  }
  int64_t days = seconds / 86400;
  int64_t rest = seconds % 86400;
  if (rest < 0) {
    rest += 86400;
    --days;
  }
  int64_t year = 0;
  uint32_t month = 0;
  uint32_t day = 0;
  civil_from_days(days, &year, &month, &day);
  snprintf(out, size, "%04lld-%02u-%02uT%02u:%02u:%02u.%03uZ", (long long)year, (unsigned)month,
           (unsigned)day, (unsigned)(rest / 3600), (unsigned)(rest / 60 % 60),
           (unsigned)(rest % 60), (unsigned)millis);
  return 0;
}
