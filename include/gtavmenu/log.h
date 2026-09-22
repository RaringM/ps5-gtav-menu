#pragma once

#include <stdarg.h>

// Leveled, tagged line logger for the worker/loader. One global threshold; the emit path
// itself is intentionally NOT thread-safe (callers are the single worker/loader thread).
//
// Two sinks share the same API:
//   * the loader writes the formatted line to a file/stdout (this translation unit);
//   * the worker -- where file I/O is compiled out (GTAV_MENU_DISABLE_LOG_IO) -- attaches an
//     in-memory ring via the weak `gtav_log_sink` hook (see src/module/log_ring.c), so the
//     same GTAV_LOG* calls remain retrievable over ps5debug.
//
// Verbosity is light by default (INFO). The menu's Telemetry toggle raises the threshold to
// DEBUG at runtime via gtav_log_set_level(); messages below the threshold are dropped before
// any formatting. Each line carries a severity level and a subsystem category; the file sink
// renders them as "[ts] [LEVEL] [cat] msg".

#ifdef __cplusplus
extern "C" {
#endif

// Severity, most-severe first. The threshold admits levels with a value <= the current level,
// so raising the threshold to DEBUG admits ERROR..DEBUG and drops TRACE.
typedef enum GtavLogLevel {
  GTAV_LOG_ERROR = 0,
  GTAV_LOG_WARN = 1,
  GTAV_LOG_INFO = 2,
  GTAV_LOG_DEBUG = 3,
  GTAV_LOG_TRACE = 4,
} GtavLogLevel;

// Subsystem tag, so logs can be filtered/traced by area. GENERAL is the default for untagged
// call sites (and the legacy gtav_logf).
typedef enum GtavLogCategory {
  GTAV_LOG_CAT_GENERAL = 0,
  GTAV_LOG_CAT_INJECT = 1,
  GTAV_LOG_CAT_HOOK = 2,
  GTAV_LOG_CAT_PAD = 3,
  GTAV_LOG_CAT_FEATURE = 4,
  GTAV_LOG_CAT_NATIVE = 5,
  GTAV_LOG_CAT_MENU = 6,
  GTAV_LOG_CAT_CONFIG = 7,
} GtavLogCategory;

// Open the log sink at `path`. Returns 0 when `path` was opened, or 1 when it fell back
// to stdout (still a usable sink, e.g. when log I/O is compiled out or the path is
// unwritable). NOTE: a deliberate exception to the project's 0/-1 convention -- 1 is a
// non-fatal fallback, not an error, and there is no -1 result.
int gtav_log_open(const char* path);
// Close the log sink. No-op when it fell back to stdout.
void gtav_log_close(void);

// Verbosity threshold. Default GTAV_LOG_INFO. Always live, even when file I/O is compiled
// out, so the worker can raise it when Telemetry is enabled.
void gtav_log_set_level(GtavLogLevel level);
GtavLogLevel gtav_log_level(void);

// Human-readable names for the level/category (for sinks and tooling).
const char* gtav_log_level_name(GtavLogLevel level);
const char* gtav_log_category_name(GtavLogCategory category);

// Leveled, tagged log. Dropped (no formatting) when `level` is below the current threshold.
void gtav_logf_cat(GtavLogLevel level, GtavLogCategory category, const char* fmt, ...);
void gtav_vlogf(GtavLogLevel level, GtavLogCategory category, const char* fmt, va_list ap);
// printf-style line log at INFO/GENERAL (newline appended). Back-compat alias.
void gtav_logf(const char* fmt, ...);

#ifdef __cplusplus
}
#endif

// Per-file default category: a source file may `#define GTAV_LOG_DEFAULT_CATEGORY
// GTAV_LOG_CAT_INJECT` before including this header to tag all of its GTAV_LOG* lines.
#ifndef GTAV_LOG_DEFAULT_CATEGORY
#define GTAV_LOG_DEFAULT_CATEGORY GTAV_LOG_CAT_GENERAL
#endif

#define GTAV_LOGE(...) gtav_logf_cat(GTAV_LOG_ERROR, GTAV_LOG_DEFAULT_CATEGORY, __VA_ARGS__)
#define GTAV_LOGW(...) gtav_logf_cat(GTAV_LOG_WARN, GTAV_LOG_DEFAULT_CATEGORY, __VA_ARGS__)
#define GTAV_LOGI(...) gtav_logf_cat(GTAV_LOG_INFO, GTAV_LOG_DEFAULT_CATEGORY, __VA_ARGS__)
#define GTAV_LOGD(...) gtav_logf_cat(GTAV_LOG_DEBUG, GTAV_LOG_DEFAULT_CATEGORY, __VA_ARGS__)
#define GTAV_LOGT(...) gtav_logf_cat(GTAV_LOG_TRACE, GTAV_LOG_DEFAULT_CATEGORY, __VA_ARGS__)
