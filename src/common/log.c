#include "gtavmenu/log.h"

#include "gtavmenu/rootdir.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static FILE* g_log_file;

// Each hook below is OPTIONAL: it must resolve to null, not a link error, when the
// translation unit that provides it is not in the link. A weak UNDEFINED symbol gives
// exactly that on ELF, which is what the PS5 target and CI use.
extern void klog_puts(const char* message) __attribute__((weak));
// Worker-only crash-capture sink (src/module/klog.c). Distinct from the crt's klog_puts above:
// that one needs crt _start init the injected worker never runs, so the worker uses this
// self-contained writer instead. Weak -- null (and skipped) wherever klog.c isn't linked.
extern void gtav_worker_klog(const char* message) __attribute__((weak));
// Optional secondary sink: the worker provides a strong definition (src/module/log_ring.c)
// that pushes each formatted line into an in-memory ring read over ps5debug. Null elsewhere.
extern void gtav_log_sink(GtavLogLevel level, GtavLogCategory category, const char* message)
    __attribute__((weak));

#if defined(__APPLE__)
// Mach-O has no weak-undefined symbol: weak_import only resolves an export of some dylib,
// so a hook that is absent from the link is a hard ld error rather than a null pointer.
// That makes the host static tests -- and therefore `make local-verify` -- unbuildable on a
// dev Mac while Linux CI passes. Supply weak NO-OP definitions here so the guards above see
// a valid (harmless) target; the real providers are strong definitions and override these
// wherever they are linked. Confined to __APPLE__, so the PS5 build is unchanged.
__attribute__((weak)) void klog_puts(const char* message) {
  (void)message;
}
__attribute__((weak)) void gtav_worker_klog(const char* message) {
  (void)message;
}
__attribute__((weak)) void gtav_log_sink(GtavLogLevel level, GtavLogCategory category,
                                         const char* message) {
  (void)level;
  (void)category;
  (void)message;
}
#endif

#ifndef GTAV_MENU_DISABLE_LOG_IO
#define GTAV_MENU_DISABLE_LOG_IO 0
#endif

// Worker crash-capture sink: when file I/O is compiled out (the worker), also mirror each
// admitted line to the PS5 kernel log via gtav_worker_klog (src/module/klog.c -- a self-
// contained writer; the crt's klog_puts faults in the injected worker, see that file). The
// kernel log outlives the GTA process, so it retains the lines before a fatal crash that the
// in-memory ring (gtav_log_sink) cannot. Off by default; menu-ctl `start --klog` opts in.
#ifndef GTAV_MENU_ENABLE_WORKER_KLOG
#define GTAV_MENU_ENABLE_WORKER_KLOG 0
#endif

// Verbosity threshold lives outside the file-I/O gate so it stays live in the worker build
// (where the file sink is compiled out) -- that is what lets Telemetry raise the level.
static GtavLogLevel g_log_level = GTAV_LOG_INFO;

void gtav_log_set_level(GtavLogLevel level) {
  g_log_level = level;
}

GtavLogLevel gtav_log_level(void) {
  return g_log_level;
}

const char* gtav_log_level_name(GtavLogLevel level) {
  switch (level) {
    case GTAV_LOG_ERROR:
      return "error";
    case GTAV_LOG_WARN:
      return "warn";
    case GTAV_LOG_INFO:
      return "info";
    case GTAV_LOG_DEBUG:
      return "debug";
    case GTAV_LOG_TRACE:
      return "trace";
  }
  return "?";
}

const char* gtav_log_category_name(GtavLogCategory category) {
  switch (category) {
    case GTAV_LOG_CAT_GENERAL:
      return "general";
    case GTAV_LOG_CAT_INJECT:
      return "inject";
    case GTAV_LOG_CAT_HOOK:
      return "hook";
    case GTAV_LOG_CAT_PAD:
      return "pad";
    case GTAV_LOG_CAT_FEATURE:
      return "feat";
    case GTAV_LOG_CAT_NATIVE:
      return "native";
    case GTAV_LOG_CAT_MENU:
      return "menu";
    case GTAV_LOG_CAT_CONFIG:
      return "config";
  }
  return "?";
}

int gtav_log_open(const char* path) {
#if GTAV_MENU_DISABLE_LOG_IO
  (void)path;
  g_log_file = stdout;
  return 1;
#else
  GtavRootdirGuard rootdir;
  int rooted;

  if (g_log_file && g_log_file != stdout) {
    fclose(g_log_file);
    g_log_file = NULL;
  }

  rooted = gtav_rootdir_enter(&rootdir) == 0;
  mkdir("/data", 0777);
  mkdir("/data/GTAVMenu", 0777);

  if (path && path[0]) {
    g_log_file = fopen(path, "a");
  }
  if (rooted) {
    gtav_rootdir_leave(&rootdir);
  }

  if (!g_log_file) {
    g_log_file = stdout;
  }

  gtav_logf("log opened");
  return g_log_file == stdout ? 1 : 0;
#endif
}

void gtav_log_close(void) {
  if (g_log_file && g_log_file != stdout) {
    gtav_logf("log closing");
    fclose(g_log_file);
  }
  g_log_file = NULL;
}

void gtav_vlogf(GtavLogLevel level, GtavLogCategory category, const char* fmt, va_list ap) {
  // Drop below-threshold lines before any formatting (cheap). Lower enum value = more severe,
  // so an admitted line has level <= the current threshold.
  if ((int)level > (int)g_log_level) {
    return;
  }

  char message[512];
  vsnprintf(message, sizeof(message), fmt, ap);

#if !GTAV_MENU_DISABLE_LOG_IO
  {
    FILE* out = g_log_file ? g_log_file : stdout;
    const char* lvl = gtav_log_level_name(level);
    const char* cat = gtav_log_category_name(category);
    time_t now = time(NULL);
    struct tm* tmv = localtime(&now);  // ISO C; the logger is single-threaded (see header)
    char stamp[32] = "unknown";

    if (tmv) {
      strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", tmv);
    }

    if (out == stdout && klog_puts) {
      char line[640];
      snprintf(line, sizeof(line), "[%s] [%s] [%s] %s", stamp, lvl, cat, message);
      klog_puts(line);
      fprintf(out, "%s\n", line);
    } else {
      fprintf(out, "[%s] [%s] [%s] %s\n", stamp, lvl, cat, message);
    }
    fflush(out);
  }
#elif GTAV_MENU_ENABLE_WORKER_KLOG
  // Worker build (file sink compiled out): mirror the line to the kernel log so it survives
  // the process dying. gtav_worker_klog is weak -- if klog.c wasn't linked it stays NULL and
  // we skip silently. Same worker-thread-only invariant as gtav_log_sink applies.
  if (gtav_worker_klog) {
    char kline[640];
    snprintf(kline, sizeof(kline), "GTAVMenu [%s] [%s] %s", gtav_log_level_name(level),
             gtav_log_category_name(category), message);
    gtav_worker_klog(kline);
  }
#endif

  if (gtav_log_sink) {
    gtav_log_sink(level, category, message);
  }
}

void gtav_logf_cat(GtavLogLevel level, GtavLogCategory category, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  gtav_vlogf(level, category, fmt, ap);
  va_end(ap);
}

void gtav_logf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  gtav_vlogf(GTAV_LOG_INFO, GTAV_LOG_CAT_GENERAL, fmt, ap);
  va_end(ap);
}
