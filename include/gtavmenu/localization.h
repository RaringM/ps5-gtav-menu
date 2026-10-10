#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gtavmenu/localization_generated.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stable locale ids are persisted in profile.cfg. English must remain id 0. */
uint32_t gtav_locale_count(void);
uint32_t gtav_locale_id(void);
int gtav_locale_set(uint32_t id);
/* Step through the available catalog IDs in sorted catalog order. IDs are stable and may have
 * gaps, so callers must not infer the next ID with arithmetic. A negative direction steps back. */
uint32_t gtav_locale_step_id(uint32_t id, int direction);
const char* gtav_locale_code(uint32_t id);
const char* gtav_locale_display_name(uint32_t id);

/* Key lookup falls back to English. The compatibility lookup lets existing English
 * menu definitions participate while rows are progressively assigned symbolic keys. */
const char* gtav_locale_text(const char* key);
const char* gtav_locale_translate(const char* english);

#if defined(GTAV_LOCALE_TESTING) && GTAV_LOCALE_TESTING
typedef struct GtavLocaleTestEntry {
  const char* key;
  const char* text;
} GtavLocaleTestEntry;
int gtav_locale_install_test_catalog(uint32_t id, const char* code, const char* display_name,
                                     const GtavLocaleTestEntry* entries, uint32_t count);
void gtav_locale_clear_test_catalog(void);
#endif

#ifdef __cplusplus
}
#endif
