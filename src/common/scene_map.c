#include "gtavmenu/scene_map.h"

#include <stdio.h>
#include <string.h>

uint32_t gtav_scene_map_joaat(const char* text) {
  uint32_t hash = 0;
  for (const char* p = text; p && *p; ++p) {
    const char c = *p >= 'A' && *p <= 'Z' ? (char)(*p - 'A' + 'a') : *p;
    hash += (uint8_t)c;
    hash += hash << 10;
    hash ^= hash >> 6;
  }
  hash += hash << 3;
  hash ^= hash >> 11;
  hash += hash << 15;
  return hash;
}

void gtav_scene_map_init(GtavSceneMap* map) {
  if (map) memset(map, 0, sizeof(*map));
}

// A pack id as the descriptor's `pack` row: [a-z0-9-], alphanumeric at both ends.
static int scene_id_valid(const char* value, size_t length) {
  if (!length || length >= GTAV_SCENE_MAP_PACK_ID_MAX) return 0;
  for (size_t i = 0; i < length; ++i) {
    const char c = value[i];
    const int alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!alnum && c != '-') return 0;
    if ((i == 0 || i + 1 == length) && !alnum) return 0;
  }
  return 1;
}

// A model name as a pack's spawn row: [a-z0-9_]+.
static int scene_name_valid(const char* value, size_t length) {
  if (!length || length >= GTAV_SCENE_MAP_NAME_MAX) return 0;
  for (size_t i = 0; i < length; ++i) {
    const char c = value[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return 0;
  }
  return 1;
}

int gtav_scene_map_entry_valid(const GtavSceneMapEntry* e, int mode) {
  if (!e || e->kind < GTAV_SCENE_MAP_VEHICLE || e->kind > GTAV_SCENE_MAP_OBJECT || e->model == 0 ||
      (e->flags & ~(uint32_t)GTAV_SCENE_MAP_FLAG_MASK))
    return 0;
  const float values[6] = {e->x, e->y, e->z, e->rx, e->ry, e->rz};
  const float position_limit = mode == GTAV_SCENE_MAP_RELATIVE ? 10000.0f : 100000.0f;
  for (int i = 0; i < 6; ++i) {
    if (!__builtin_isfinite(values[i])) return 0;
    if (__builtin_fabsf(values[i]) > (i < 3 ? position_limit : 100000.0f)) return 0;
  }
  return 1;
}

static int scene_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int scene_header(GtavSceneMap* map, const char* text) {
  static const char* const kHeaders[] = {"GTAVMAP,1,absolute", "GTAVMAP,1,relative",
                                         "GTAVMAP,2,absolute", "GTAVMAP,2,relative"};
  for (int i = 0; i < 4; ++i) {
    if (strcmp(text, kHeaders[i]) != 0) continue;
    map->version = i < 2 ? 1 : 2;
    map->mode = (i & 1) ? GTAV_SCENE_MAP_RELATIVE : GTAV_SCENE_MAP_ABSOLUTE;
    return 1;
  }
  return 0;
}

// The pack tail ",PACK,NAME" of a version 2 row (`tail` after the flags field). Returns the 1-based
// pack index for the entry, 0 for no tail, -1 when invalid.
static int scene_pack_tail(GtavSceneMap* map, const char* tail, uint32_t model) {
  if (*tail == '\0') return 0;
  if (*tail != ',' || map->version < 2) return -1;
  const char* id = tail + 1;
  const char* comma = strchr(id, ',');
  if (!comma) return -1;
  const size_t id_length = (size_t)(comma - id);
  const char* name = comma + 1;
  const size_t name_length = strlen(name);
  if (!scene_id_valid(id, id_length) || !scene_name_valid(name, name_length) ||
      gtav_scene_map_joaat(name) != model)
    return -1;
  int pack = 0;
  while (pack < map->pack_count && (strlen(map->pack_ids[pack]) != id_length ||
                                    memcmp(map->pack_ids[pack], id, id_length) != 0))
    ++pack;
  if (pack == map->pack_count) {
    if (map->pack_count >= GTAV_SCENE_MAP_PACK_MAX) return -1;
    memcpy(map->pack_ids[pack], id, id_length);
    map->pack_ids[pack][id_length] = '\0';
    memcpy(map->pack_models[pack], name, name_length + 1);
    ++map->pack_count;
  }
  ++map->pack_rows[pack];
  return pack + 1;
}

int gtav_scene_map_feed(GtavSceneMap* map, const char* line) {
  if (!map || !line) return -1;
  const size_t raw = strlen(line);
  if (raw >= GTAV_SCENE_MAP_LINE_MAX) return -1;
  char buffer[GTAV_SCENE_MAP_LINE_MAX];
  memcpy(buffer, line, raw + 1);
  char* text = buffer;
  while (scene_space(*text)) ++text;
  size_t length = strlen(text);
  while (length && scene_space(text[length - 1])) text[--length] = '\0';
  if (!*text || *text == '#') return 0;
  if (!map->started && scene_header(map, text)) {
    map->started = 1;
    return 0;
  }
  map->started = 1;
  if (!strncmp(text, "GTAVMAP,", 8) || map->count >= GTAV_SCENE_MAP_MAX) return -1;
  GtavSceneMapEntry e;
  memset(&e, 0, sizeof(e));
  int kind = -1;
  unsigned model = 0, flags = 0;
  int consumed = 0;
  const int fields = map->version ? sscanf(text, "%d,0x%x,%f,%f,%f,%f,%f,%f,%u%n", &kind, &model,
                                           &e.x, &e.y, &e.z, &e.rx, &e.ry, &e.rz, &flags, &consumed)
                                  : sscanf(text, "%d,0x%x,%f,%f,%f,%f,%f,%f%n", &kind, &model, &e.x,
                                           &e.y, &e.z, &e.rx, &e.ry, &e.rz, &consumed);
  if (fields != (map->version ? 9 : 8) || consumed <= 0) return -1;
  e.kind = kind;
  e.model = model;
  e.flags = flags;
  if (!gtav_scene_map_entry_valid(&e, map->mode)) return -1;
  if (map->kind_counts[kind] >= GTAV_SCENE_MAP_KIND_MAX) return -1;
  const int pack = scene_pack_tail(map, text + consumed, e.model);
  if (pack < 0) return -1;
  e.pack = (uint8_t)pack;
  ++map->kind_counts[kind];
  map->entries[map->count++] = e;
  return 0;
}

int gtav_scene_map_format_row(char* out, size_t size, const GtavSceneMapEntry* e,
                              const char* pack_id, const char* name) {
  if (!out || !size || !gtav_scene_map_entry_valid(e, GTAV_SCENE_MAP_ABSOLUTE)) return -1;
  const int has_pack = pack_id && *pack_id;
  const int has_name = name && *name;
  if (has_pack != has_name) return -1;
  if (has_pack && (!scene_id_valid(pack_id, strlen(pack_id)) ||
                   !scene_name_valid(name, strlen(name)) || gtav_scene_map_joaat(name) != e->model))
    return -1;
  const int n = snprintf(out, size, "%d,0x%08X,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u%s%s%s%s\n",
                         (int)e->kind, (unsigned)e->model, e->x, e->y, e->z, e->rx, e->ry, e->rz,
                         (unsigned)e->flags, has_pack ? "," : "", has_pack ? pack_id : "",
                         has_pack ? "," : "", has_pack ? name : "");
  if (n < 0 || (size_t)n >= size) return -1;
  return n;
}
