#pragma once

/* The PS4 plugin log, /data/GTAVMenu/ps4-plugin.log.
 *
 * Any thread may add a line, the hooked game thread included: gtav_ps4_plugin_log_line stamps it
 * with the process time and copies it into a small static ring; it never blocks, allocates or
 * touches a file. gtav_ps4_plugin_log_flush (the supervisor thread, and unload) writes the queued
 * lines in one batch, each as
 *
 *   [2026-11-04T18:22:03.412Z +2.131877s pid=91 host=goldhen(module)] message
 *
 * (UTC from sceKernelGettimeofday, process time from sceKernelGetProcessTime, the loader host and
 * the evidence it was detected by). The first flush of a run rotates a log above 256 KiB to .log.1
 * and writes the run header "=== gtav_menu.prx <version> target=<stem> load=+X.XXXs ===". */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_PS4_PLUGIN_LOG_DIR "/data/GTAVMenu"
#define GTAV_PS4_PLUGIN_LOG_PATH GTAV_PS4_PLUGIN_LOG_DIR "/ps4-plugin.log"
#define GTAV_PS4_PLUGIN_LOG_ROTATED_PATH GTAV_PS4_PLUGIN_LOG_PATH ".1"
#define GTAV_PS4_PLUGIN_LOG_ROTATE_BYTES (256u * 1024u)
#define GTAV_PS4_PLUGIN_LOG_SLOTS 128u
/* Room for the longest pack breadcrumb (224 bytes); a slot is 256 bytes. */
#define GTAV_PS4_PLUGIN_LOG_TEXT_BYTES 240u

/* A new run: the header carries `version` (g_pluginVersion), `target` and the load time. */
void gtav_ps4_plugin_log_begin(uint32_t version, const char* target);
/* Stamp and queue one line (truncated to fit). Non-blocking; when writers lap the flusher the
   oldest unwritten lines are lost and the next flush says how many. */
void gtav_ps4_plugin_log_line(const char* message);
/* While set, flushes leave every line queued (for a timing-critical window). */
void gtav_ps4_plugin_log_set_defer(int defer);
/* Write the queued lines: 0 = written, nothing pending, deferred or another flush is running;
   -1 = the log could not be opened or written (unwritten lines stay queued). */
int gtav_ps4_plugin_log_flush(void);
/* "goldhen(module)", "nexhen(plugins.ini)", "goldhen(dir)", ... or "unknown"; detected on the
   first flush. */
const char* gtav_ps4_plugin_log_host(void);
/* "YYYY-MM-DDTHH:MM:SS.mmmZ" for a Unix time in milliseconds; 0, or -1 when `size` is too small. */
int gtav_ps4_plugin_log_format_utc(int64_t unix_ms, char* out, size_t size);

#ifdef __cplusplus
}
#endif
