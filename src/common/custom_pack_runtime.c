#include "gtavmenu/custom_pack_runtime.h"

#include <stdio.h>
#include <string.h>

static const struct {
  const char* name;
  uint32_t type;
} kDataTypes[] = {
    {"HANDLING_FILE", GTAV_CUSTOM_PACK_DATA_HANDLING},
    {"CARCOLS_FILE", GTAV_CUSTOM_PACK_DATA_CARCOLS},
    {"VEHICLE_METADATA_FILE", GTAV_CUSTOM_PACK_DATA_VEHICLE_METADATA},
    {"VEHICLE_VARIATION_FILE", GTAV_CUSTOM_PACK_DATA_VEHICLE_VARIATION},
    {"VEHICLE_LAYOUTS_FILE", GTAV_CUSTOM_PACK_DATA_VEHICLE_LAYOUTS},
    {"PED_METADATA_FILE", GTAV_CUSTOM_PACK_DATA_PED_METADATA},
    {"WEAPON_METADATA_FILE", GTAV_CUSTOM_PACK_DATA_WEAPON_METADATA},
    {"WEAPONINFO_FILE", GTAV_CUSTOM_PACK_DATA_WEAPONINFO},
    {"WEAPONCOMPONENTSINFO_FILE", GTAV_CUSTOM_PACK_DATA_WEAPONCOMPONENTSINFO},
    {"WEAPON_ANIMATIONS_FILE", GTAV_CUSTOM_PACK_DATA_WEAPON_ANIMATIONS},
    {"SHOP_PED_APPAREL_META_FILE", GTAV_CUSTOM_PACK_DATA_SHOP_PED_APPAREL},
    {"TIMECYCLEMOD_FILE", GTAV_CUSTOM_PACK_DATA_TIMECYCLEMOD},
    {"AUDIO_GAMEDATA", GTAV_CUSTOM_PACK_DATA_AUDIO_GAMEDATA},
};

const char* gtav_custom_pack_data_type_name(uint32_t type) {
  for (size_t i = 0; i < sizeof(kDataTypes) / sizeof(kDataTypes[0]); ++i)
    if (kDataTypes[i].type == type) return kDataTypes[i].name;
  return NULL;
}

static int id_valid(const char* value, size_t length) {
  if (!length || length >= GTAV_CUSTOM_PACK_ID_MAX) return 0;
  for (size_t i = 0; i < length; ++i) {
    char c = value[i];
    int alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!alnum && c != '-') return 0;
    if ((i == 0 || i + 1 == length) && !alnum) return 0;
  }
  return 1;
}

// Lowercase engine name: [a-z0-9_]+, optionally followed by one ".<ext>" of [a-z0-9]+.
static int name_valid(const char* value, const char* extension) {
  size_t length = strlen(value);
  const char* dot = strchr(value, '.');

  if (!length || length >= GTAV_CUSTOM_PACK_NAME_MAX) return 0;
  if (extension) {
    if (!dot || strcmp(dot + 1, extension) != 0 || dot == value) return 0;
  } else if (dot) {
    return 0;
  }
  for (const char* p = value; p < (dot ? dot : value + length); ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return 0;
  return 1;
}

// `spawn ptfx` model <asset>:<effect> and `spawn component` model <weapon>:<component>: two
// lowercase engine names, the whole under the name limit.
static int pair_spawn_valid(const char* value) {
  char asset[GTAV_CUSTOM_PACK_NAME_MAX];
  const char* colon = strchr(value, ':');
  const size_t length = strlen(value);
  if (!colon || colon == value || length >= GTAV_CUSTOM_PACK_NAME_MAX) return 0;
  memcpy(asset, value, (size_t)(colon - value));
  asset[colon - value] = '\0';
  return name_valid(asset, NULL) && name_valid(colon + 1, NULL);
}

// Stock member an override row may replace: [<folder>/]<stem>.<ptd|pft|pdr|pdd>, folder [a-z0-9_]
// (one level: the member's folder inside its stock archive, e.g. player_one/), stem [a-z0-9_+]
// (`+hi`).
static int override_member_valid(const char* value) {
  static const char* const kExtensions[] = {"ptd", "pft", "pdr", "pdd"};
  const size_t length = strlen(value);
  const char* slash = strchr(value, '/');
  const char* stem = slash ? slash + 1 : value;
  const char* dot = strchr(stem, '.');
  int extension_ok = 0;
  if (!length || length >= GTAV_CUSTOM_PACK_NAME_MAX || !dot || dot == stem || slash == value)
    return 0;
  for (size_t i = 0; i < sizeof(kExtensions) / sizeof(kExtensions[0]); ++i)
    extension_ok |= strcmp(dot + 1, kExtensions[i]) == 0;
  if (!extension_ok) return 0;
  for (const char* p = value; slash && p < slash; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return 0;
  for (const char* p = stem; p < dot; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '+')) return 0;
  return 1;
}

// `archive` row tail: 4 fields, or 5 with "overlay". Returns the overlay flag, or -1.
static int archive_overlay_field(char** fields, int count) {
  if (count == 4) return 0;
  return count == 5 && strcmp(fields[4], "overlay") == 0 ? 1 : -1;
}

// A data row's file: <name>.meta, or for an audio game-data chunk <chunk>_game.rel. The worker
// stages the chunk as "audio/<file>"; the mounter names it after the basename up to its first '_'.
static int file_valid(const char* value, uint32_t type) {
  const char* dot = strrchr(value, '.');
  if (!dot || strchr(value, '.') != dot || !name_valid(value, dot + 1)) return 0;
  if (type != GTAV_CUSTOM_PACK_DATA_AUDIO_GAMEDATA) return strcmp(dot, ".meta") == 0;
  const char* underscore = strchr(value, '_');
  const size_t chunk = underscore ? (size_t)(underscore - value) : 0;
  return chunk >= 1 && chunk <= GTAV_CUSTOM_PACK_AUDIO_CHUNK_MAX &&
         strcmp(underscore, "_game.rel") == 0;
}

static int label_key_valid(const char* value) {
  size_t length = strlen(value);
  if (!length || length >= GTAV_CUSTOM_PACK_NAME_MAX) return 0;
  for (const char* p = value; *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
          *p == '_'))
      return 0;
  return 1;
}

// Label, spawn, map and place texts: printable ASCII without '~' (a format token to the game's text
// renderer; label texts can come from a mod's own text table).
static int label_text_valid(const char* value) {
  size_t length = strlen(value);
  if (!length || length >= GTAV_CUSTOM_PACK_NAME_MAX) return 0;
  for (const char* p = value; *p; ++p)
    if (*p < 0x20 || *p > 0x7e || *p == '~') return 0;
  return 1;
}

// Optional descriptor text (description, author, version): 1..max-1 printable ASCII characters
// without '~' (a format token to the game's text renderer); a version is [A-Za-z0-9._+-].
static int about_text_valid(const char* value, size_t max, int version) {
  const size_t length = strlen(value);
  if (!length || length >= max) return 0;
  for (const char* p = value; *p; ++p) {
    const char c = *p;
    if (c < 0x20 || c > 0x7e || c == '~') return 0;
    if (version && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '.' || c == '_' || c == '+' || c == '-'))
      return 0;
  }
  return 1;
}

// Signed whole number without leading zeros, |value| <= GTAV_CUSTOM_PACK_PLACE_COORD_MAX.
static int parse_coord(const char* value, int32_t* result) {
  int negative = *value == '-';
  int32_t number = 0;
  const char* p = value + negative;
  if (!*p || (*p == '0' && p[1])) return -1;
  for (; *p; ++p) {
    if (*p < '0' || *p > '9') return -1;
    number = number * 10 + (*p - '0');
    if (number > GTAV_CUSTOM_PACK_PLACE_COORD_MAX) return -1;
  }
  *result = negative ? -number : number;
  return 0;
}

static int parse_size(const char* value, uint64_t limit, uint64_t* result) {
  uint64_t number = 0;

  if (!*value || *value == '0') return -1;
  for (const char* p = value; *p; ++p) {
    uint64_t digit;
    if (*p < '0' || *p > '9') return -1;
    digit = (uint64_t)(*p - '0');
    if (number > (limit - digit) / 10u) return -1;
    number = number * 10u + digit;
  }
  *result = number;
  return 0;
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static int parse_sha256(const char* value, uint8_t digest[32]) {
  if (strlen(value) != 64) return -1;
  for (size_t i = 0; i < 32; ++i) {
    int high = hex_value(value[i * 2u]);
    int low = hex_value(value[i * 2u + 1u]);
    if (high < 0 || low < 0) return -1;
    digest[i] = (uint8_t)((high << 4) | low);
  }
  return 0;
}

// Split a line into exactly `count` tab-separated fields.
static int split(char* line, char** fields, int count) {
  int n = 0;
  fields[n++] = line;
  for (char* p = line; *p; ++p) {
    if (*p != '\t') continue;
    if (n == count) return -1;
    *p = '\0';
    fields[n++] = p + 1;
  }
  return n == count ? 0 : -1;
}

// Split a line into at most `max` tab-separated fields; returns the count, or -1 when there are
// more.
static int split_upto(char* line, char** fields, int max) {
  int n = 0;
  fields[n++] = line;
  for (char* p = line; *p; ++p) {
    if (*p != '\t') continue;
    if (n == max) return -1;
    *p = '\0';
    fields[n++] = p + 1;
  }
  return n;
}

static char* next_line(char** cursor) {
  char* line = *cursor;
  char* newline;

  if (!*line) return NULL;
  newline = strchr(line, '\n');
  if (!newline) return NULL;
  *newline = '\0';
  *cursor = newline + 1;
  return line;
}

int gtav_custom_pack_parse(char* buffer, size_t size, GtavCustomPack* pack) {
  char* cursor = buffer;
  char* line;
  char* f[5];
  int typdeps = 0;  // `typdep` rows seen: they follow every typ row
  int tints = 0;    // `tints` rows seen: they follow every spawn row

  if (!buffer || !pack || !size || size > GTAV_CUSTOM_PACK_DESCRIPTOR_MAX ||
      buffer[size - 1] != '\n' || memchr(buffer, '\0', size) || memchr(buffer, '\r', size))
    return -1;
  buffer[size] = '\0';
  memset(pack, 0, sizeof(*pack));
  pack->source_count = 1;
  line = next_line(&cursor);
  if (!line || strcmp(line, GTAV_CUSTOM_PACK_MAGIC) != 0) return -1;

  line = next_line(&cursor);
  if (!line || split(line, f, 2) != 0 || strcmp(f[0], "pack") != 0 || !id_valid(f[1], strlen(f[1])))
    return -1;
  snprintf(pack->pack_id, sizeof(pack->pack_id), "%s", f[1]);
  snprintf(pack->source_ids[0], sizeof(pack->source_ids[0]), "%s", f[1]);

  // Optional menu texts, in this order, before the first archive row; checked, not stored
  // (gtav_custom_pack_about reads them).
  line = next_line(&cursor);
  if (line && strncmp(line, "description\t", 12) == 0) {
    if (!about_text_valid(line + 12, GTAV_CUSTOM_PACK_DESCRIPTION_MAX, 0)) return -1;
    line = next_line(&cursor);
  }
  if (line && strncmp(line, "author\t", 7) == 0) {
    if (!about_text_valid(line + 7, GTAV_CUSTOM_PACK_AUTHOR_MAX, 0)) return -1;
    line = next_line(&cursor);
  }
  if (line && strncmp(line, "version\t", 8) == 0) {
    if (!about_text_valid(line + 8, GTAV_CUSTOM_PACK_VERSION_MAX, 1)) return -1;
    line = next_line(&cursor);
  }
  int overlay = line ? archive_overlay_field(f, split_upto(line, f, 5)) : -1;
  if (overlay < 0 || strcmp(f[0], "archive") != 0 ||
      parse_size(f[1], GTAV_CUSTOM_PACK_ARCHIVE_BYTES_MAX, &pack->archive_size) != 0 ||
      parse_sha256(f[2], pack->archive_sha256) != 0 || !name_valid(f[3], "rpf"))
    return -1;
  snprintf(pack->archive, sizeof(pack->archive), "%s", f[3]);
  pack->archive_overlay[0] = (uint8_t)overlay;

  while ((line = next_line(&cursor)) != NULL) {
    // `place` rows follow every other row; `hide` rows come last.
    if ((pack->place_count && strncmp(line, "place\t", 6) != 0 &&
         strncmp(line, "hide\t", 5) != 0) ||
        (pack->hide_count && strncmp(line, "hide\t", 5) != 0))
      return -1;
    if (strncmp(line, "archive\t", 8) == 0) {
      // Further archives directly follow the first, before any other row.
      int n = pack->extra_archive_count;
      overlay = archive_overlay_field(f, split_upto(line, f, 5));
      if (pack->override_count || pack->card_count || pack->typ_count || pack->map_count ||
          pack->bounds_count || pack->data_count || pack->label_count || pack->spawn_count ||
          n >= GTAV_CUSTOM_PACK_ARCHIVE_MAX - 1 || overlay < 0 ||
          parse_size(f[1], GTAV_CUSTOM_PACK_ARCHIVE_BYTES_MAX, &pack->extra_archives[n].size) !=
              0 ||
          parse_sha256(f[2], pack->extra_archives[n].sha256) != 0 || !name_valid(f[3], "rpf") ||
          strcmp(f[3], pack->archive) == 0)
        return -1;
      for (int i = 0; i < n; ++i)
        if (strcmp(pack->extra_archives[i].name, f[3]) == 0) return -1;
      snprintf(pack->extra_archives[n].name, sizeof(pack->extra_archives[n].name), "%s", f[3]);
      pack->archive_overlay[n + 1] = (uint8_t)overlay;
      pack->extra_archive_count++;
    } else if (strncmp(line, "override\t", 9) == 0) {
      // Directly after the archive rows; names an overlay archive of this descriptor.
      int archive = -1;
      if (pack->card_count || pack->typ_count || pack->map_count || pack->bounds_count ||
          pack->data_count || pack->label_count || pack->spawn_count ||
          pack->override_count >= GTAV_CUSTOM_PACK_OVERRIDE_MAX || split(line, f, 3) != 0 ||
          !override_member_valid(f[2]))
        return -1;
      if (strcmp(f[1], pack->archive) == 0) archive = 0;
      for (int i = 0; i < pack->extra_archive_count; ++i)
        if (strcmp(f[1], pack->extra_archives[i].name) == 0) archive = i + 1;
      if (archive < 0 || !pack->archive_overlay[archive]) return -1;
      for (int i = 0; i < pack->override_count; ++i)
        if (strcmp(pack->overrides[i].member, f[2]) == 0) return -1;
      pack->overrides[pack->override_count].archive = (uint8_t)archive;
      snprintf(pack->overrides[pack->override_count].member, sizeof(pack->overrides[0].member),
               "%s", f[2]);
      pack->override_count++;
    } else if (strncmp(line, "card\t", 5) == 0) {
      GtavCustomPackCard* card;
      if (pack->data_count || pack->typ_count || pack->map_count || pack->bounds_count ||
          pack->label_count || pack->spawn_count || pack->card_count >= GTAV_CUSTOM_PACK_CARD_MAX ||
          split(line, f, 3) != 0 || !name_valid(f[1], NULL) || !name_valid(f[2], NULL))
        return -1;
      card = &pack->cards[pack->card_count];
      for (int i = 0; i < pack->card_count; ++i)
        if (strcmp(pack->cards[i].dict, f[1]) == 0 && strcmp(pack->cards[i].texture, f[2]) == 0)
          return -1;
      snprintf(card->dict, sizeof(card->dict), "%s", f[1]);
      snprintf(card->texture, sizeof(card->texture), "%s", f[2]);
      pack->card_count++;
    } else if (strncmp(line, "typ\t", 4) == 0) {
      char* g[3];
      const int fields = split_upto(line, g, 3);
      if (typdeps || pack->data_count || pack->map_count || pack->bounds_count ||
          pack->label_count || pack->spawn_count || pack->typ_count >= GTAV_CUSTOM_PACK_TYP_MAX ||
          (fields != 2 && fields != 3) || (fields == 3 && strcmp(g[2], "retail") != 0) ||
          !name_valid(g[1], "ptyp"))
        return -1;
      for (int i = 0; i < pack->typ_count; ++i)
        if (strcmp(pack->typs[i], g[1]) == 0) return -1;
      snprintf(pack->typs[pack->typ_count], sizeof(pack->typs[0]), "%s", g[1]);
      pack->typ_retail[pack->typ_count] = (uint8_t)(fields == 3);
      pack->typ_count++;
    } else if (strncmp(line, "typdep\t", 7) == 0) {
      // After the typ rows, before the maps: one stock typ dependency per pack typ row, bound
      // typ->typ by the worker (a retail _manifest itypDependencies entry). The dependency is a
      // stock typ, never one of this descriptor's own members.
      int typ = -1;
      if (pack->map_count || pack->bounds_count || pack->data_count || pack->label_count ||
          pack->spawn_count || split(line, f, 3) != 0 || !name_valid(f[2], "ptyp") ||
          strcmp(f[1], f[2]) == 0)
        return -1;
      for (int i = 0; i < pack->typ_count; ++i) {
        if (strcmp(pack->typs[i], f[1]) == 0) typ = i;
        if (strcmp(pack->typs[i], f[2]) == 0 && !pack->typ_retail[i]) return -1;
      }
      if (typ < 0 || pack->typ_retail[typ] || pack->typ_deps[typ][0]) return -1;
      snprintf(pack->typ_deps[typ], sizeof(pack->typ_deps[0]), "%s", f[2]);
      typdeps++;
    } else if (strncmp(line, "map\t", 4) == 0) {
      char* g[6];
      const int fields = split_upto(line, g, 6);
      int bound_maps = 0;
      for (int i = 0; i < pack->map_count; ++i) bound_maps |= pack->map_dep_name[i][0] != '\0';
      if (bound_maps || pack->bounds_count || pack->data_count || pack->label_count ||
          pack->spawn_count || pack->map_count >= GTAV_CUSTOM_PACK_MAP_MAX ||
          (fields != 2 && fields != 6) || !name_valid(g[1], "pmap"))
        return -1;
      for (int i = 0; i < pack->map_count; ++i)
        if (strcmp(pack->maps[i], g[1]) == 0) return -1;
      if (fields == 6) {
        if (parse_coord(g[2], &pack->map_places[pack->map_count].x) != 0 ||
            parse_coord(g[3], &pack->map_places[pack->map_count].y) != 0 ||
            parse_coord(g[4], &pack->map_places[pack->map_count].z) != 0 ||
            !label_text_valid(g[5]) || strlen(g[5]) >= GTAV_CUSTOM_PACK_SPAWN_TEXT_MAX)
          return -1;
        snprintf(pack->map_places[pack->map_count].text, sizeof(pack->map_places[0].text), "%s",
                 g[5]);
      }
      f[1] = g[1];
      snprintf(pack->maps[pack->map_count], sizeof(pack->maps[0]), "%s", f[1]);
      pack->map_count++;
    } else if (strncmp(line, "mapdep\t", 7) == 0) {
      // After the map rows: one typ dependency per map, both rows of this descriptor, or (`retail`)
      // a stock typ named by the map row alone.
      char* g[4];
      const int fields = split_upto(line, g, 4);
      const int stock = fields == 4 && strcmp(g[3], "retail") == 0;
      int map = -1, typ = -1;
      if (pack->bounds_count || pack->data_count || pack->label_count || pack->spawn_count ||
          (fields != 3 && fields != 4) || (fields == 4 && !stock && strcmp(g[3], "interior") != 0))
        return -1;
      for (int i = 0; i < pack->map_count; ++i)
        if (strcmp(pack->maps[i], g[1]) == 0) map = i;
      for (int i = 0; i < pack->typ_count; ++i)
        if (strcmp(pack->typs[i], g[2]) == 0) typ = i;
      // Precedence: a typ row of this descriptor, then (`retail`) a stock typ by name, then another
      // pack's own typ (left unbound here, bound by gtav_custom_pack_merge). `typ ... retail` rows
      // are loaded, never bound.
      if (map < 0 || !name_valid(g[2], "ptyp") || pack->map_dep_name[map][0]) return -1;
      if (stock) {
        // Never an interior (the keep bit is for the pack's own MLO typ) nor a pack member.
        if (typ >= 0 && !pack->typ_retail[typ]) return -1;
        pack->map_dep_retail[map] = 1u;
      } else {
        if (typ >= 0 && pack->typ_retail[typ]) return -1;
        pack->map_dep_typ[map] = (uint8_t)(typ + 1);
        pack->map_interior[map] = (uint8_t)(fields == 4);
      }
      snprintf(pack->map_dep_name[map], sizeof(pack->map_dep_name[0]), "%s", g[2]);
    } else if (strncmp(line, "bounds\t", 7) == 0) {
      // After the map rows, before data.
      if (pack->data_count || pack->label_count || pack->spawn_count ||
          pack->bounds_count >= GTAV_CUSTOM_PACK_BOUNDS_MAX || split(line, f, 2) != 0 ||
          !name_valid(f[1], "pbn"))
        return -1;
      for (int i = 0; i < pack->bounds_count; ++i)
        if (strcmp(pack->bounds[i], f[1]) == 0) return -1;
      snprintf(pack->bounds[pack->bounds_count], sizeof(pack->bounds[0]), "%s", f[1]);
      pack->bounds_count++;
    } else if (strncmp(line, "data\t", 5) == 0) {
      GtavCustomPackData* data;
      uint32_t type = 0;
      if (pack->label_count || pack->spawn_count || pack->wicon_count ||
          pack->data_count >= GTAV_CUSTOM_PACK_DATA_MAX || split(line, f, 5) != 0)
        return -1;
      for (size_t i = 0; i < sizeof(kDataTypes) / sizeof(kDataTypes[0]); ++i)
        if (strcmp(kDataTypes[i].name, f[1]) == 0) type = kDataTypes[i].type;
      data = &pack->data[pack->data_count];
      if (!type || parse_size(f[2], GTAV_CUSTOM_PACK_DATA_BYTES_MAX, &data->size) != 0 ||
          parse_sha256(f[3], data->sha256) != 0 || !file_valid(f[4], type) ||
          strcmp(f[4], pack->archive) == 0)
        return -1;
      for (int i = 0; i < pack->data_count; ++i)
        if (strcmp(pack->data[i].file, f[4]) == 0) return -1;
      data->type = type;
      snprintf(data->file, sizeof(data->file), "%s", f[4]);
      pack->data_count++;
    } else if (strncmp(line, "wicon\t", 6) == 0) {
      // After the data rows, before labels: pack weapon -> retail donor whose wheel icon it shows.
      const int n = pack->wicon_count;
      if (pack->label_count || pack->spawn_count || n >= GTAV_CUSTOM_PACK_WICON_MAX ||
          split(line, f, 3) != 0 || !name_valid(f[1], NULL) || !name_valid(f[2], NULL) ||
          strcmp(f[1], f[2]) == 0)
        return -1;
      for (int i = 0; i < n; ++i)
        if (strcmp(pack->wicons[i].weapon, f[1]) == 0) return -1;
      snprintf(pack->wicons[n].weapon, sizeof(pack->wicons[0].weapon), "%s", f[1]);
      snprintf(pack->wicons[n].donor, sizeof(pack->wicons[0].donor), "%s", f[2]);
      pack->wicon_count++;
    } else if (strncmp(line, "spawn\t", 6) == 0) {
      uint32_t kind = 0;
      if (tints || pack->spawn_count >= GTAV_CUSTOM_PACK_SPAWN_MAX || split(line, f, 4) != 0 ||
          !label_text_valid(f[3]) || strlen(f[3]) >= GTAV_CUSTOM_PACK_SPAWN_TEXT_MAX)
        return -1;
      if (strcmp(f[1], "vehicle") == 0) kind = GTAV_CUSTOM_PACK_SPAWN_VEHICLE;
      if (strcmp(f[1], "object") == 0) kind = GTAV_CUSTOM_PACK_SPAWN_OBJECT;
      if (strcmp(f[1], "ped") == 0) kind = GTAV_CUSTOM_PACK_SPAWN_PED;
      if (strcmp(f[1], "weapon") == 0) kind = GTAV_CUSTOM_PACK_SPAWN_WEAPON;
      if (strcmp(f[1], "timecycle") == 0) kind = GTAV_CUSTOM_PACK_SPAWN_TIMECYCLE;
      if (strcmp(f[1], "ptfx") == 0) kind = GTAV_CUSTOM_PACK_SPAWN_PTFX;
      if (strcmp(f[1], "component") == 0) kind = GTAV_CUSTOM_PACK_SPAWN_COMPONENT;
      if (!kind) return -1;
      if (kind == GTAV_CUSTOM_PACK_SPAWN_PTFX || kind == GTAV_CUSTOM_PACK_SPAWN_COMPONENT
              ? !pair_spawn_valid(f[2])
              : !name_valid(f[2], NULL))
        return -1;
      for (int i = 0; i < pack->spawn_count; ++i)
        if (strcmp(pack->spawns[i].model, f[2]) == 0) return -1;
      pack->spawns[pack->spawn_count].kind = kind;
      snprintf(pack->spawns[pack->spawn_count].model, sizeof(pack->spawns[0].model), "%s", f[2]);
      snprintf(pack->spawns[pack->spawn_count].text, sizeof(pack->spawns[0].text), "%s", f[3]);
      pack->spawn_count++;
    } else if (strncmp(line, "tints\t", 6) == 0) {
      // After the spawn rows: whether a `spawn weapon` row's model reads a tint palette (once).
      int spawn = -1;
      uint8_t value = 0;
      if (split(line, f, 3) != 0) return -1;
      if (strcmp(f[2], "palette") == 0) value = GTAV_CUSTOM_PACK_TINTS_PALETTE;
      if (strcmp(f[2], "none") == 0) value = GTAV_CUSTOM_PACK_TINTS_NONE;
      for (int i = 0; i < pack->spawn_count; ++i)
        if (pack->spawns[i].kind == GTAV_CUSTOM_PACK_SPAWN_WEAPON &&
            strcmp(pack->spawns[i].model, f[1]) == 0)
          spawn = i;
      if (!value || spawn < 0 || pack->spawn_tints[spawn]) return -1;
      pack->spawn_tints[spawn] = value;
      tints++;
    } else if (strncmp(line, "label\t", 6) == 0) {
      if (pack->spawn_count || pack->label_count >= GTAV_CUSTOM_PACK_LABEL_MAX ||
          split(line, f, 3) != 0 || !label_key_valid(f[1]) || !label_text_valid(f[2]))
        return -1;
      for (int i = 0; i < pack->label_count; ++i)
        if (strcmp(pack->label_keys[i], f[1]) == 0) return -1;
      snprintf(pack->label_keys[pack->label_count], sizeof(pack->label_keys[0]), "%s", f[1]);
      snprintf(pack->label_texts[pack->label_count], sizeof(pack->label_texts[0]), "%s", f[2]);
      pack->label_count++;
    } else if (strncmp(line, "place\t", 6) == 0) {
      // A Custom Packs teleport without a map row: x, y, z (whole metres) and its menu text.
      char* g[5];
      const int n = pack->place_count;
      if (n >= GTAV_CUSTOM_PACK_PLACE_MAX || split(line, g, 5) != 0 ||
          parse_coord(g[1], &pack->places[n].x) != 0 ||
          parse_coord(g[2], &pack->places[n].y) != 0 ||
          parse_coord(g[3], &pack->places[n].z) != 0 || !label_text_valid(g[4]) ||
          strlen(g[4]) >= GTAV_CUSTOM_PACK_SPAWN_TEXT_MAX)
        return -1;
      snprintf(pack->places[n].text, sizeof(pack->places[0].text), "%s", g[4]);
      pack->place_count++;
    } else if (strncmp(line, "hide\t", 5) == 0) {
      // Stock map entities of one model inside a sphere: model name, x, y, z, radius (metres).
      char* g[6];
      int32_t radius = 0;
      const int n = pack->hide_count;
      if (n >= GTAV_CUSTOM_PACK_HIDE_MAX || split(line, g, 6) != 0 || !name_valid(g[1], NULL) ||
          parse_coord(g[2], &pack->hides[n].x) != 0 || parse_coord(g[3], &pack->hides[n].y) != 0 ||
          parse_coord(g[4], &pack->hides[n].z) != 0 || parse_coord(g[5], &radius) != 0 ||
          radius < 1 || radius > GTAV_CUSTOM_PACK_HIDE_RADIUS_MAX)
        return -1;
      for (int i = 0; i < n; ++i)
        if (strcmp(pack->hides[i].model, g[1]) == 0 && pack->hides[i].x == pack->hides[n].x &&
            pack->hides[i].y == pack->hides[n].y && pack->hides[i].z == pack->hides[n].z)
          return -1;
      snprintf(pack->hides[n].model, sizeof(pack->hides[0].model), "%s", g[1]);
      pack->hides[n].radius = (uint32_t)radius;
      pack->hide_count++;
    } else {
      return -1;
    }
  }
  if (*cursor) return -1;
  // Every overlay archive replaces at least one stock member.
  for (int a = 0; a <= pack->extra_archive_count; ++a) {
    int rows = 0;
    for (int i = 0; i < pack->override_count; ++i) rows += pack->overrides[i].archive == a;
    if (pack->archive_overlay[a] && !rows) return -1;
  }
  return 0;
}

int gtav_custom_pack_parse_active(const char* buffer, size_t size,
                                  char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX],
                                  int* count) {
  size_t at = 0;
  int n = 0;

  if (!buffer || !ids || !count || size < 2 || buffer[size - 1] != '\n') return -1;
  while (at < size) {
    const char* line = buffer + at;
    const char* newline = (const char*)memchr(line, '\n', size - at);
    size_t length = (size_t)(newline - line);
    if (n >= GTAV_CUSTOM_PACK_ACTIVE_MAX || !id_valid(line, length)) return -1;
    memcpy(ids[n], line, length);
    ids[n][length] = '\0';
    for (int i = 0; i < n; ++i)
      if (strcmp(ids[i], ids[n]) == 0) return -1;
    n++;
    at += length + 1u;
  }
  *count = n;
  return 0;
}

// Length of `value` scanning at most GTAV_CUSTOM_PACK_ID_MAX bytes (that bound when unterminated).
static size_t id_length(const char* value) {
  const char* end = (const char*)memchr(value, '\0', GTAV_CUSTOM_PACK_ID_MAX);
  return end ? (size_t)(end - value) : GTAV_CUSTOM_PACK_ID_MAX;
}

int gtav_custom_pack_id_valid(const char* id) {
  return id && id_valid(id, id_length(id));
}

int gtav_custom_pack_active_toggle(char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX],
                                   int* count, const char* id) {
  if (!ids || !count || *count < 0 || *count > GTAV_CUSTOM_PACK_ACTIVE_MAX ||
      !gtav_custom_pack_id_valid(id))
    return -1;
  for (int i = 0; i < *count; ++i) {
    if (strcmp(ids[i], id) != 0) continue;
    for (int j = i; j + 1 < *count; ++j) memcpy(ids[j], ids[j + 1], GTAV_CUSTOM_PACK_ID_MAX);
    --*count;
    memset(ids[*count], 0, GTAV_CUSTOM_PACK_ID_MAX);
    return 0;
  }
  if (*count == GTAV_CUSTOM_PACK_ACTIVE_MAX) return -1;
  snprintf(ids[*count], GTAV_CUSTOM_PACK_ID_MAX, "%s", id);
  ++*count;
  return 1;
}

int gtav_custom_pack_render_active(
    const char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX], int count, char* out,
    size_t size) {
  size_t at = 0;
  if (!ids || !out || size == 0 || count < 0 || count > GTAV_CUSTOM_PACK_ACTIVE_MAX) return -1;
  out[0] = '\0';
  for (int i = 0; i < count; ++i) {
    const size_t length = id_length(ids[i]);
    if (!id_valid(ids[i], length) || at + length + 2u > size) return -1;
    for (int j = 0; j < i; ++j)
      if (strcmp(ids[j], ids[i]) == 0) return -1;
    memcpy(out + at, ids[i], length);
    at += length;
    out[at++] = '\n';
    out[at] = '\0';
  }
  return (int)at;
}

static int set_has_archive(const GtavCustomPack* set, const char* name) {
  if (strcmp(set->archive, name) == 0) return 1;
  for (int i = 0; i < set->extra_archive_count; ++i)
    if (strcmp(set->extra_archives[i].name, name) == 0) return 1;
  return 0;
}

// Bytes of a descriptor's (or merged set's) archives and of its data files (not inlined: four
// vectorised copies would cost the worker image ~2 KiB).
__attribute__((noinline)) static uint64_t pack_archive_bytes(const GtavCustomPack* pack) {
  uint64_t bytes = pack->archive_size;
  for (int i = 0; i < pack->extra_archive_count; ++i) bytes += pack->extra_archives[i].size;
  return bytes;
}

__attribute__((noinline)) static uint64_t pack_data_bytes(const GtavCustomPack* pack) {
  uint64_t bytes = 0;
  for (int i = 0; i < pack->data_count; ++i) bytes += pack->data[i].size;
  return bytes;
}

static int clash_name(char* why, size_t size, const char* kind, const char* name) {
  if (why) snprintf(why, size, "%s %s", kind, name);
  return GTAV_CUSTOM_PACK_CLASH_NAME;
}

static int clash_cap(char* why, size_t size, const char* text) {
  if (why) snprintf(why, size, "%s", text);
  return GTAV_CUSTOM_PACK_CLASH_CAP;
}

// Menu word for a spawn row's kind (what the player calls the thing whose name repeats).
static const char* spawn_kind_word(uint32_t kind) {
  switch (kind) {
    case GTAV_CUSTOM_PACK_SPAWN_VEHICLE:
      return "vehicle";
    case GTAV_CUSTOM_PACK_SPAWN_PED:
      return "ped";
    case GTAV_CUSTOM_PACK_SPAWN_WEAPON:
      return "weapon";
    case GTAV_CUSTOM_PACK_SPAWN_TIMECYCLE:
    case GTAV_CUSTOM_PACK_SPAWN_PTFX:
      return "effect";
    case GTAV_CUSTOM_PACK_SPAWN_COMPONENT:
      return "attachment";
    default:
      return "prop";
  }
}

// First of `count` names of `names` (each `stride` bytes after the last) that is also one of the
// `set_count` names of `set_names` (same stride), or NULL. One loop for every name row kind keeps
// the worker image small.
__attribute__((noinline)) static const char* first_repeat(const char* names, int count,
                                                          const char* set_names, int set_count,
                                                          size_t stride) {
  for (int i = 0; i < count; ++i)
    for (int j = 0; j < set_count; ++j)
      if (strcmp(names + (size_t)i * stride, set_names + (size_t)j * stride) == 0)
        return names + (size_t)i * stride;
  return NULL;
}

int gtav_custom_pack_clash(const GtavCustomPack* set, const GtavCustomPack* pack, char* why,
                           size_t size) {
  if (why && size) why[0] = '\0';
  if (!set || !pack || pack->source_count != 1) return -1;
  if (set->source_count == 0) return GTAV_CUSTOM_PACK_CLASH_NONE;
  // Names first: a repeat is the likelier cause and the one the player fixes by deselecting.
  for (int i = 0; i < set->source_count; ++i)
    if (strcmp(set->source_ids[i], pack->pack_id) == 0)
      return clash_name(why, size, "pack", pack->pack_id);
  if (set_has_archive(set, pack->archive)) return clash_name(why, size, "archive", pack->archive);
  for (int i = 0; i < pack->extra_archive_count; ++i)
    if (set_has_archive(set, pack->extra_archives[i].name))
      return clash_name(why, size, "archive", pack->extra_archives[i].name);
  const struct {
    const char* kind;
    const char* names;
    int count;
    const char* set_names;
    int set_count;
    size_t stride;
  } kRows[] = {
      {"map", pack->maps[0], pack->map_count, set->maps[0], set->map_count, sizeof(set->maps[0])},
      {NULL, pack->spawns[0].model, pack->spawn_count, set->spawns[0].model, set->spawn_count,
       sizeof(set->spawns[0])},
      {"override", pack->overrides[0].member, pack->override_count, set->overrides[0].member,
       set->override_count, sizeof(set->overrides[0])},
      {"data file", pack->data[0].file, pack->data_count, set->data[0].file, set->data_count,
       sizeof(set->data[0])},
      {"collision", pack->bounds[0], pack->bounds_count, set->bounds[0], set->bounds_count,
       sizeof(set->bounds[0])},
      {"label", pack->label_keys[0], pack->label_count, set->label_keys[0], set->label_count,
       sizeof(set->label_keys[0])},
      {"wheel icon", pack->wicons[0].weapon, pack->wicon_count, set->wicons[0].weapon,
       set->wicon_count, sizeof(set->wicons[0])},
  };
  for (size_t k = 0; k < sizeof(kRows) / sizeof(kRows[0]); ++k) {
    const char* name = first_repeat(kRows[k].names, kRows[k].count, kRows[k].set_names,
                                    kRows[k].set_count, kRows[k].stride);
    if (!name) continue;
    // Spawn rows: the player's word for the model's kind.
    const char* kind = kRows[k].kind;
    if (!kind)
      kind = spawn_kind_word(
          pack->spawns[(name - pack->spawns[0].model) / sizeof(pack->spawns[0])].kind);
    return clash_name(why, size, kind, name);
  }
  // A typ may repeat only when both rows are `retail` (requesting a stock typ twice is harmless).
  for (int i = 0; i < pack->typ_count; ++i)
    for (int j = 0; j < set->typ_count; ++j)
      if (strcmp(pack->typs[i], set->typs[j]) == 0 && !(pack->typ_retail[i] && set->typ_retail[j]))
        return clash_name(why, size, "typ", pack->typs[i]);
  for (int i = 0; i < pack->card_count; ++i)
    for (int j = 0; j < set->card_count; ++j)
      if (strcmp(pack->cards[i].dict, set->cards[j].dict) == 0 &&
          strcmp(pack->cards[i].texture, set->cards[j].texture) == 0)
        return clash_name(why, size, "card", pack->cards[i].texture);
  const struct {
    int have, add, cap;
    const char* text;
  } kCaps[] = {
      {set->source_count, 1, GTAV_CUSTOM_PACK_ACTIVE_MAX, "too many packs"},
      {set->extra_archive_count + 1, pack->extra_archive_count + 1, GTAV_CUSTOM_PACK_ARCHIVE_CAP,
       "too many archives"},
      {set->card_count, pack->card_count, GTAV_CUSTOM_PACK_CARD_CAP, "too many cards"},
      {set->typ_count, pack->typ_count, GTAV_CUSTOM_PACK_TYP_CAP, "too many typs"},
      {set->map_count, pack->map_count, GTAV_CUSTOM_PACK_MAP_CAP, "too many maps"},
      {set->label_count, pack->label_count, GTAV_CUSTOM_PACK_LABEL_CAP, "too many labels"},
      {set->spawn_count, pack->spawn_count, GTAV_CUSTOM_PACK_SPAWN_CAP, "too many menu rows"},
      {set->data_count, pack->data_count, GTAV_CUSTOM_PACK_DATA_CAP, "too many data files"},
      {set->override_count, pack->override_count, GTAV_CUSTOM_PACK_OVERRIDE_CAP,
       "too many overrides"},
      {set->bounds_count, pack->bounds_count, GTAV_CUSTOM_PACK_BOUNDS_CAP,
       "too many collision files"},
      {set->place_count, pack->place_count, GTAV_CUSTOM_PACK_PLACE_CAP, "too many teleports"},
      {set->hide_count, pack->hide_count, GTAV_CUSTOM_PACK_HIDE_CAP, "too many hides"},
      {set->wicon_count, pack->wicon_count, GTAV_CUSTOM_PACK_WICON_CAP, "too many wheel icons"},
  };
  for (size_t k = 0; k < sizeof(kCaps) / sizeof(kCaps[0]); ++k)
    if (kCaps[k].have + kCaps[k].add > kCaps[k].cap) return clash_cap(why, size, kCaps[k].text);
  if (pack_archive_bytes(set) + pack_archive_bytes(pack) > GTAV_CUSTOM_PACK_SET_ARCHIVE_BYTES_MAX)
    return clash_cap(why, size, "archives too big");
  if (pack_data_bytes(set) + pack_data_bytes(pack) > GTAV_CUSTOM_PACK_SET_DATA_BYTES_MAX)
    return clash_cap(why, size, "data files too big");
  return GTAV_CUSTOM_PACK_CLASH_NONE;
}

int gtav_custom_pack_merge(GtavCustomPack* set, const GtavCustomPack* pack) {
  if (gtav_custom_pack_clash(set, pack, NULL, 0) != GTAV_CUSTOM_PACK_CLASH_NONE) return -1;
  if (set->source_count == 0) {
    *set = *pack;
    return 0;
  }
  const int source = set->source_count;
  int n = set->extra_archive_count;
  // The pack's archive k becomes merged archive first + k.
  const int first = n + 1;
  for (int k = 0; k <= pack->extra_archive_count; ++k)
    set->archive_overlay[first + k] = pack->archive_overlay[k];
  for (int i = 0; i < pack->override_count; ++i, ++set->override_count) {
    set->overrides[set->override_count] = pack->overrides[i];
    set->overrides[set->override_count].archive = (uint8_t)(first + pack->overrides[i].archive);
  }
  snprintf(set->extra_archives[n].name, sizeof(set->extra_archives[n].name), "%s", pack->archive);
  set->extra_archives[n].size = pack->archive_size;
  memcpy(set->extra_archives[n].sha256, pack->archive_sha256, 32);
  set->extra_archive_source[n++] = (uint8_t)source;
  for (int i = 0; i < pack->extra_archive_count; ++i, ++n) {
    set->extra_archives[n] = pack->extra_archives[i];
    set->extra_archive_source[n] = (uint8_t)source;
  }
  set->extra_archive_count = n;
  for (int i = 0; i < pack->card_count; ++i) set->cards[set->card_count++] = pack->cards[i];
  for (int i = 0; i < pack->typ_count; ++i, ++set->typ_count) {
    set->typ_source[set->typ_count] = (uint8_t)source;
    memcpy(set->typs[set->typ_count], pack->typs[i], sizeof(set->typs[0]));
    set->typ_retail[set->typ_count] = pack->typ_retail[i];
    memcpy(set->typ_deps[set->typ_count], pack->typ_deps[i], sizeof(set->typ_deps[0]));
  }
  // Typ rows are appended before maps, so a map's dependency moves by the set's old typ count.
  const int typ_base = set->typ_count - pack->typ_count;
  for (int i = 0; i < pack->map_count; ++i, ++set->map_count) {
    memcpy(set->maps[set->map_count], pack->maps[i], sizeof(set->maps[0]));
    set->map_places[set->map_count] = pack->map_places[i];
    set->map_source[set->map_count] = (uint8_t)source;
    set->map_dep_typ[set->map_count] =
        pack->map_dep_typ[i] ? (uint8_t)(pack->map_dep_typ[i] + typ_base) : 0u;
    set->map_interior[set->map_count] = pack->map_interior[i];
    memcpy(set->map_dep_name[set->map_count], pack->map_dep_name[i], sizeof(set->map_dep_name[0]));
    set->map_dep_retail[set->map_count] = pack->map_dep_retail[i];
  }
  // Cross-pack map dependencies: a mapdep naming another pack's typ binds to that typ's row as soon
  // as both packs are merged (typ rows only append, so a bound index never moves). A stock
  // (`retail`) mapdep is bound by name by the worker, never to a pack's row.
  for (int m = 0; m < set->map_count; ++m)
    for (int t = 0; t < set->typ_count && set->map_dep_name[m][0] && !set->map_dep_typ[m] &&
                    !set->map_dep_retail[m];
         ++t)
      if (!set->typ_retail[t] && strcmp(set->typs[t], set->map_dep_name[m]) == 0)
        set->map_dep_typ[m] = (uint8_t)(t + 1);
  for (int i = 0; i < pack->bounds_count; ++i, ++set->bounds_count) {
    set->bounds_source[set->bounds_count] = (uint8_t)source;
    memcpy(set->bounds[set->bounds_count], pack->bounds[i], sizeof(set->bounds[0]));
  }
  for (int i = 0; i < pack->label_count; ++i, ++set->label_count) {
    set->label_source[set->label_count] = (uint8_t)source;
    memcpy(set->label_keys[set->label_count], pack->label_keys[i], sizeof(set->label_keys[0]));
    memcpy(set->label_texts[set->label_count], pack->label_texts[i], sizeof(set->label_texts[0]));
  }
  for (int i = 0; i < pack->spawn_count; ++i, ++set->spawn_count) {
    set->spawns[set->spawn_count] = pack->spawns[i];
    set->spawn_tints[set->spawn_count] = pack->spawn_tints[i];
    set->spawn_source[set->spawn_count] = (uint8_t)source;
  }
  for (int i = 0; i < pack->data_count; ++i, ++set->data_count) {
    set->data[set->data_count] = pack->data[i];
    set->data_source[set->data_count] = (uint8_t)source;
  }
  for (int i = 0; i < pack->place_count; ++i, ++set->place_count) {
    set->places[set->place_count] = pack->places[i];
    set->place_source[set->place_count] = (uint8_t)source;
  }
  for (int i = 0; i < pack->hide_count; ++i, ++set->hide_count) {
    set->hide_source[set->hide_count] = (uint8_t)source;
    set->hides[set->hide_count] = pack->hides[i];
  }
  for (int i = 0; i < pack->wicon_count; ++i) set->wicons[set->wicon_count++] = pack->wicons[i];
  snprintf(set->source_ids[source], sizeof(set->source_ids[0]), "%s", pack->pack_id);
  set->source_count = source + 1;
  return 0;
}

// ---- Vehicle menu names (gtav_custom_pack_vehicle_name) ----

static char ascii_upper(char c) {
  return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}

// 1 when `text` starts with `prefix` (ASCII case-insensitive) followed by the end or a space.
static int starts_with_word(const char* text, const char* prefix) {
  size_t i = 0;
  for (; prefix[i]; ++i)
    if (ascii_upper(text[i]) != ascii_upper(prefix[i])) return 0;
  return text[i] == '\0' || text[i] == ' ';
}

// 1 when `key` contains `word` (ASCII case-insensitive).
__attribute__((noinline)) static int contains_word(const char* key, const char* word) {
  const size_t n = strlen(word);
  if (!n) return 0;
  for (const char* p = key; *p; ++p) {
    size_t i = 0;
    while (i < n && p[i] && ascii_upper(p[i]) == ascii_upper(word[i])) ++i;
    if (i == n) return 1;
  }
  return 0;
}

int gtav_custom_pack_vehicle_name(const GtavCustomPack* set, int spawn, char* out, size_t size) {
  if (!out || !size) return -1;
  out[0] = '\0';
  if (!set || spawn < 0 || spawn >= set->spawn_count) return -1;
  const char* text = set->spawns[spawn].text;
  const char* only = NULL;   // the pack's first make label text
  const char* named = NULL;  // a make label whose key names the model
  int makes = 0;
  for (int i = 0; i < set->label_count; ++i) {
    const char* key = set->label_keys[i];
    const size_t n = strlen(key);
    if (set->label_source[i] != set->spawn_source[spawn] || n < 5 || !set->label_texts[i][0] ||
        !starts_with_word(key + n - 5, "_MAKE"))
      continue;
    if (!makes++) only = set->label_texts[i];
    if (!named && contains_word(key, set->spawns[spawn].model)) named = set->label_texts[i];
  }
  // The pack's only make, or (several makes) the one whose key names the model; never a guess.
  const char* make = makes == 1 ? only : named;
  if (!make || starts_with_word(text, make)) {
    snprintf(out, size, "%s", text);
    return 0;
  }
  snprintf(out, size, "%s %s", make, text);
  return 1;
}

// ---- Weapon menu (gtav_custom_pack_weapon_meta, component labels) ----

// Offset of `literal` in [from, size), or SIZE_MAX.
__attribute__((noinline)) static size_t meta_find(const char* text, size_t size, size_t from,
                                                  const char* literal) {
  const size_t n = strlen(literal);
  for (size_t at = from; at + n <= size; ++at)
    if (memcmp(text + at, literal, n) == 0) return at;
  return SIZE_MAX;
}

// The character data at `at` up to the next '<' (spaces trimmed, cut to the buffer) into `out`.
__attribute__((noinline)) static void meta_value(const char* text, size_t size, size_t at,
                                                 char* out, size_t out_size) {
  size_t n = 0;
  while (at < size && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n'))
    ++at;
  while (at < size && text[at] != '<' && n + 1u < out_size) out[n++] = text[at++];
  while (n && (out[n - 1] == ' ' || out[n - 1] == '\t' || out[n - 1] == '\r' || out[n - 1] == '\n'))
    --n;
  out[n] = '\0';
}

static int ascii_equal(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b)
    if (ascii_upper(*a) != ascii_upper(*b)) return 0;
  return *a == *b;
}

int gtav_custom_pack_weapon_meta(const char* text, size_t size, const char* weapon,
                                 GtavCustomPackWeaponMeta* out) {
  static const char kName[] = "<Name>";
  if (!out) return -1;
  memset(out, 0, sizeof(*out));
  if (!text || !weapon || !weapon[0]) return -1;
  for (size_t at = meta_find(text, size, 0, kName); at != SIZE_MAX;
       at = meta_find(text, size, at + 1u, kName)) {
    char value[GTAV_CUSTOM_PACK_WEAPON_TEXT_MAX];
    meta_value(text, size, at + sizeof(kName) - 1u, value, sizeof(value));
    if (!ascii_equal(value, weapon)) continue;
    // The weapon's own fields lie before the next weapon item.
    size_t end = meta_find(text, size, at, "\"CWeaponInfo\"");
    if (end == SIZE_MAX) end = size;
    size_t field = meta_find(text, end, at, "<HumanNameHash>");
    if (field != SIZE_MAX)
      meta_value(text, end, field + 15u, out->human_name, sizeof(out->human_name));
    field = meta_find(text, end, at, "<WheelSlot>");
    if (field != SIZE_MAX)
      meta_value(text, end, field + 11u, out->wheel_slot, sizeof(out->wheel_slot));
    const size_t points = meta_find(text, end, at, "<AttachPoints>");
    if (points != SIZE_MAX) {
      size_t close = meta_find(text, end, points, "</AttachPoints>");
      if (close == SIZE_MAX) close = end;
      for (size_t name = meta_find(text, close, points, kName), next; name != SIZE_MAX;
           name = next) {
        next = meta_find(text, close, name + 1u, kName);
        const int i = out->component_count++;
        if (i >= GTAV_CUSTOM_PACK_WEAPON_COMPONENTS_MAX) continue;
        meta_value(text, close, name + sizeof(kName) - 1u, out->components[i],
                   sizeof(out->components[i]));
        // The item's own fields lie before the next component's <Name>.
        if (meta_find(text, next == SIZE_MAX ? close : next, name, "<Default value=\"true\"") !=
            SIZE_MAX)
          out->default_mask |= 1u << i;
      }
    }
    return 0;
  }
  return -1;
}

// 1 when `text` starts with `prefix` (ASCII case-insensitive).
__attribute__((noinline)) static int has_prefix(const char* text, const char* prefix) {
  for (; *prefix; ++text, ++prefix)
    if (ascii_upper(*text) != ascii_upper(*prefix)) return 0;
  return 1;
}

// `n` characters of `text` into `out` (cut to the buffer, NUL-terminated).
__attribute__((noinline)) static void copy_text(char* out, size_t size, const char* text,
                                                size_t n) {
  if (n >= size) n = size - 1u;
  memcpy(out, text, n);
  out[n] = '\0';
}

void gtav_custom_pack_component_label(const char* name, const char* row, char* out, size_t size) {
  if (!out || !size) return;
  out[0] = '\0';
  if (row && row[0]) {
    const char* text = row;
    for (const char* p = row; (p = strstr(p, ": ")) != NULL; p += 2) text = p + 2;
    size_t n = strlen(text);
    if (n > 7 && ascii_equal(text + n - 7, " on/off")) n -= 7;
    copy_text(out, size, text, n);
    out[0] = ascii_upper(out[0]);
    if (out[0]) return;
  }
  if (!name) return;
  const char* part = has_prefix(name, "COMPONENT_") && name[10] ? name + 10 : name;
  static const struct {
    const char* key;
    const char* label;
  } kKinds[] = {
      {"CLIP_01", "Standard Clip"},
      {"CLIP_02", "Extended Clip"},
      {"CLIP_03", "Clip 3"},
      {"CLIP_", "Clip"},
      {"SUPP", "Suppressor"},
      {"SCOPE", "Scope"},
      {"FLSH", "Flashlight"},
      {"GRIP", "Grip"},
      {"MUZZLE", "Muzzle Brake"},
      {"COMP", "Compensator"},
      {"BARREL", "Barrel"},
      {"VARMOD", "Finish"},
      {"CAMO", "Camo"},
      {"RAILCOVER", "Rail Cover"},
  };
#pragma clang loop unroll(disable)
  for (size_t k = 0; k < sizeof(kKinds) / sizeof(kKinds[0]); ++k)
    if (contains_word(part, kKinds[k].key)) {
      copy_text(out, size, kKinds[k].label, strlen(kKinds[k].label));
      return;
    }
  copy_text(out, size, part, strlen(part));
}

// 1 when `label` is `base` alone or `base` + " <digits>" (a suffix the dedupe gave it).
static int label_has_base(const char* label, const char* base) {
  const size_t n = strlen(base);
  if (!has_prefix(label, base)) return 0;
  if (!label[n]) return 1;
  if (label[n] != ' ' || !label[n + 1]) return 0;
  for (const char* d = label + n + 1; *d; ++d)
    if (*d < '0' || *d > '9') return 0;
  return 1;
}

void gtav_custom_pack_label_dedupe(char* labels, size_t stride, int count) {
  if (!labels || stride < 4u) return;
  for (int i = 1; i < count; ++i) {
    char* label = labels + (size_t)i * stride;
    int same = 0;
    for (int j = 0; j < i; ++j) same += label_has_base(labels + (size_t)j * stride, label);
    if (!same) continue;
    char suffix[8];
    const int n = snprintf(suffix, sizeof(suffix), " %d", same + 1);
    size_t keep = strlen(label);
    if (keep + (size_t)n + 1u > stride) keep = stride - (size_t)n - 1u;
    memcpy(label + keep, suffix, (size_t)n + 1u);
  }
}

// ---- Manage Packs line (gtav_custom_pack_about) ----

// What a pack adds, in menu order: descriptor row (and spawn kind) -> singular / plural word.
enum {
  ABOUT_VEHICLES,
  ABOUT_WEAPONS,
  ABOUT_ATTACHMENTS,
  ABOUT_PEDS,
  ABOUT_PROPS,
  ABOUT_EFFECTS,
  ABOUT_MAPS,
  ABOUT_PLACES,
  ABOUT_COLLISION,
  ABOUT_HIDES,
  ABOUT_OVERRIDES,
  ABOUT_LABELS,
  ABOUT_DATA,
  ABOUT_ARCHIVES,
  ABOUT_KINDS,
};

static const char* const kAboutWords[ABOUT_KINDS][2] = {
    {"vehicle", "vehicles"},
    {"weapon", "weapons"},
    {"attachment", "attachments"},
    {"ped", "peds"},
    {"prop", "props"},
    {"effect", "effects"},
    {"map", "maps"},
    {"teleport", "teleports"},
    {"collision file", "collision files"},
    {"hidden model", "hidden models"},
    {"override", "overrides"},
    {"label", "labels"},
    {"data file", "data files"},
    {"archive", "archives"},
};

// `line` (`length` bytes, no newline) starts with "<key>\t".
static int about_key(const char* line, size_t length, const char* key) {
  const size_t n = strlen(key);
  return length > n && memcmp(line, key, n) == 0 && line[n] == '\t';
}

// Copy a text field (up to the next tab or the line end) of printable characters only.
static void about_copy(char* out, size_t size, const char* value, size_t length) {
  size_t n = 0;
  for (size_t i = 0; i < length && value[i] != '\t' && n + 1u < size; ++i)
    if (value[i] >= 0x20 && value[i] <= 0x7e && value[i] != '~') out[n++] = value[i];
  out[n] = '\0';
}

int gtav_custom_pack_about(const char* text, size_t size, char* out, size_t out_size) {
  static const char kMagic[] = GTAV_CUSTOM_PACK_MAGIC "\n";
  // Row keyword -> what it adds; from kFirstSpawnKind on, a `spawn` row's kind.
  static const struct {
    const char* key;
    int kind;
  } kRows[] = {
      {"map", ABOUT_MAPS},       {"place", ABOUT_PLACES},          {"bounds", ABOUT_COLLISION},
      {"hide", ABOUT_HIDES},     {"override", ABOUT_OVERRIDES},    {"label", ABOUT_LABELS},
      {"data", ABOUT_DATA},      {"archive", ABOUT_ARCHIVES},      {"vehicle", ABOUT_VEHICLES},
      {"weapon", ABOUT_WEAPONS}, {"component", ABOUT_ATTACHMENTS}, {"ped", ABOUT_PEDS},
      {"object", ABOUT_PROPS},   {"timecycle", ABOUT_EFFECTS},     {"ptfx", ABOUT_EFFECTS}};
  static const int kFirstSpawnKind = 8;
  // The optional texts: description, author, version.
  static const char* const kTexts[3] = {"description", "author", "version"};
  static const size_t kTextSizes[3] = {GTAV_CUSTOM_PACK_DESCRIPTION_MAX,
                                       GTAV_CUSTOM_PACK_AUTHOR_MAX, GTAV_CUSTOM_PACK_VERSION_MAX};
  char texts[3][GTAV_CUSTOM_PACK_DESCRIPTION_MAX] = {"", "", ""};
  unsigned counts[ABOUT_KINDS] = {0};
  if (!out || !out_size) return -1;
  out[0] = '\0';
  if (!text || size < sizeof(kMagic) - 1u || memcmp(text, kMagic, sizeof(kMagic) - 1u) != 0)
    return -1;
  for (size_t at = 0; at < size;) {
    const char* line = text + at;
    const char* newline = (const char*)memchr(line, '\n', size - at);
    size_t length = newline ? (size_t)(newline - line) : size - at;
    at += length + 1u;
    for (int t = 0; t < 3; ++t)
      if (about_key(line, length, kTexts[t])) {
        const size_t skip = strlen(kTexts[t]) + 1u;
        about_copy(texts[t], kTextSizes[t], line + skip, length - skip);
      }
    int first = 0, last = kFirstSpawnKind;
    if (about_key(line, length, "spawn")) {
      line += 6;
      length -= 6u;
      first = kFirstSpawnKind;
      last = (int)(sizeof(kRows) / sizeof(kRows[0]));
    }
    for (int k = first; k < last; ++k)
      if (about_key(line, length, kRows[k].key)) {
        counts[kRows[k].kind]++;
        // A map row with x, y, z and a text is also a Locations teleport.
        if (kRows[k].kind == ABOUT_MAPS && memchr(line + 4, '\t', length - 4u))
          counts[ABOUT_PLACES]++;
        break;
      }
  }
  const char* description = texts[0];
  const char* author = texts[1];
  const char* version = texts[2];
  // Data files and archives only count when nothing a player sees is listed.
  int shown = 0;
  for (int k = 0; k < ABOUT_DATA; ++k) shown |= counts[k] != 0;
  if (shown || counts[ABOUT_DATA]) counts[ABOUT_ARCHIVES] = 0;
  if (shown) counts[ABOUT_DATA] = 0;

  // "<description> (v<version>, by <author>). Adds <n> <kind>, ..." with the missing parts left
  // out.
  const int meta = version[0] || author[0];
  size_t n = (size_t)snprintf(
      out, out_size, "%s%s%s%s%s%s%s%s%sAdds", description, description[0] && meta ? " (" : "",
      version[0] ? "v" : "", version, version[0] && author[0] ? ", " : "", author[0] ? "by " : "",
      author, description[0] && meta ? ")" : "", description[0] || meta ? ". " : "");
  int listed = 0;
  for (int k = 0; k < ABOUT_KINDS && n < out_size; ++k) {
    if (!counts[k]) continue;
    n += (size_t)snprintf(out + n, out_size - n, "%s%u %s", listed++ ? ", " : " ", counts[k],
                          kAboutWords[k][counts[k] != 1u]);
  }
  if (!listed && n < out_size) snprintf(out + n, out_size - n, " nothing");
  return 0;
}
