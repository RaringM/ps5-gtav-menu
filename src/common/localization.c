#include "gtavmenu/localization.h"

#include "gtavmenu/localization_generated.h"

#include <string.h>

typedef struct GtavLocaleMeta {
  uint32_t id;
  char code[12];
  char name[32];
} GtavLocaleMeta;

typedef struct GtavLocaleText {
  uint32_t locale_id;
  uint32_t key_offset;
  uint32_t text_offset;
} GtavLocaleText;

typedef struct GtavLocaleReverse {
  uint32_t text_offset;
  uint32_t key_offset;
} GtavLocaleReverse;

static const GtavLocaleMeta kLocales[] = {GTAV_LOCALE_GENERATED_META};
static const GtavLocaleText kTexts[] = {GTAV_LOCALE_GENERATED_TEXT};
static const GtavLocaleReverse kReverse[] = {GTAV_LOCALE_GENERATED_REVERSE};
static const char kTextBlob[] = GTAV_LOCALE_GENERATED_BLOB;
static uint32_t g_locale_id;

#if defined(GTAV_LOCALE_TESTING) && GTAV_LOCALE_TESTING
static uint32_t g_test_id;
static const char* g_test_code;
static const char* g_test_name;
static const GtavLocaleTestEntry* g_test_entries;
static uint32_t g_test_count;
#endif

static const GtavLocaleMeta* locale_meta(uint32_t id) {
  for (uint32_t i = 0; i < GTAV_LOCALE_GENERATED_META_COUNT; ++i) {
    if (kLocales[i].id == id) return &kLocales[i];
  }
  return NULL;
}

static const char* generated_text(uint32_t id, const char* key) {
  if (!key) return NULL;
  uint32_t lo = 0;
  uint32_t hi = GTAV_LOCALE_GENERATED_TEXT_COUNT;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2u;
    const GtavLocaleText* entry = &kTexts[mid];
    int cmp = 0;
    if (entry->locale_id < id)
      cmp = -1;
    else if (entry->locale_id > id)
      cmp = 1;
    else
      cmp = strcmp(kTextBlob + entry->key_offset, key);
    if (cmp < 0)
      lo = mid + 1u;
    else if (cmp > 0)
      hi = mid;
    else
      return kTextBlob + entry->text_offset;
  }
  return NULL;
}

uint32_t gtav_locale_count(void) {
#if defined(GTAV_LOCALE_TESTING) && GTAV_LOCALE_TESTING
  return GTAV_LOCALE_GENERATED_META_COUNT + (g_test_entries ? 1u : 0u);
#else
  return GTAV_LOCALE_GENERATED_META_COUNT;
#endif
}

uint32_t gtav_locale_id(void) {
  return g_locale_id;
}

int gtav_locale_set(uint32_t id) {
  if (locale_meta(id)) {
    g_locale_id = id;
    return 0;
  }
#if defined(GTAV_LOCALE_TESTING) && GTAV_LOCALE_TESTING
  if (g_test_entries && id == g_test_id) {
    g_locale_id = id;
    return 0;
  }
#endif
  g_locale_id = 0;
  return -1;
}

static uint32_t locale_id_at(uint32_t index) {
  if (index < GTAV_LOCALE_GENERATED_META_COUNT) return kLocales[index].id;
#if defined(GTAV_LOCALE_TESTING) && GTAV_LOCALE_TESTING
  if (g_test_entries && index == GTAV_LOCALE_GENERATED_META_COUNT) return g_test_id;
#endif
  return kLocales[0].id;
}

uint32_t gtav_locale_step_id(uint32_t id, int direction) {
  const uint32_t count = gtav_locale_count();
  if (count <= 1u) return locale_id_at(0);
  uint32_t index = 0;
  for (; index < count; ++index) {
    if (locale_id_at(index) == id) break;
  }
  if (index == count) index = 0;
  index = direction < 0 ? (index + count - 1u) % count : (index + 1u) % count;
  return locale_id_at(index);
}

const char* gtav_locale_code(uint32_t id) {
  const GtavLocaleMeta* meta = locale_meta(id);
  if (meta) return meta->code;
#if defined(GTAV_LOCALE_TESTING) && GTAV_LOCALE_TESTING
  if (g_test_entries && id == g_test_id) return g_test_code;
#endif
  return kLocales[0].code;
}

const char* gtav_locale_display_name(uint32_t id) {
  const GtavLocaleMeta* meta = locale_meta(id);
  if (meta) return meta->name;
#if defined(GTAV_LOCALE_TESTING) && GTAV_LOCALE_TESTING
  if (g_test_entries && id == g_test_id) return g_test_name;
#endif
  return kLocales[0].name;
}

const char* gtav_locale_text(const char* key) {
#if defined(GTAV_LOCALE_TESTING) && GTAV_LOCALE_TESTING
  if (g_test_entries && g_locale_id == g_test_id) {
    for (uint32_t i = 0; i < g_test_count; ++i) {
      if (!strcmp(g_test_entries[i].key, key)) return g_test_entries[i].text;
    }
  }
#endif
  const char* selected = generated_text(g_locale_id, key);
  if (selected) return selected;
  const char* english = generated_text(0, key);
  return english ? english : (key ? key : "");
}

const char* gtav_locale_translate(const char* english) {
  if (!english || g_locale_id == 0) return english ? english : "";
  uint32_t lo = 0;
  uint32_t hi = GTAV_LOCALE_GENERATED_REVERSE_COUNT;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2u;
    const GtavLocaleReverse* entry = &kReverse[mid];
    const int cmp = strcmp(kTextBlob + entry->text_offset, english);
    if (cmp < 0)
      lo = mid + 1u;
    else if (cmp > 0)
      hi = mid;
    else
      return gtav_locale_text(kTextBlob + entry->key_offset);
  }
  return english;
}

#if defined(GTAV_LOCALE_TESTING) && GTAV_LOCALE_TESTING
int gtav_locale_install_test_catalog(uint32_t id, const char* code, const char* display_name,
                                     const GtavLocaleTestEntry* entries, uint32_t count) {
  if (!id || !code || !display_name || !entries || !count || locale_meta(id)) return -1;
  g_test_id = id;
  g_test_code = code;
  g_test_name = display_name;
  g_test_entries = entries;
  g_test_count = count;
  return 0;
}

void gtav_locale_clear_test_catalog(void) {
  if (g_locale_id == g_test_id) g_locale_id = 0;
  g_test_id = 0;
  g_test_code = NULL;
  g_test_name = NULL;
  g_test_entries = NULL;
  g_test_count = 0;
}
#endif
