#pragma once

#include <stddef.h>
#include <stdint.h>

// Saved scene ("map") file of Spawn > Spawned Entities > Save Map / Load Map
// (features/spooner.inc). Plain ASCII lines, read back as untrusted input:
//
//   GTAVMAP,2,absolute | GTAVMAP,2,relative        header (version 1 is read as well)
//   # comment                                      blank lines and comments are skipped
//   kind,0xMODEL,x,y,z,rx,ry,rz,flags              kind 0 vehicle, 1 ped, 2 object; flag bit 0
//                                                  freezes, bit 1 grounds an object
//   kind,0xMODEL,x,y,z,rx,ry,rz,flags,PACK,NAME    version 2 only: the entity's model comes from
//                                                  runtime pack PACK (a pack id) and is named NAME
//                                                  ([a-z0-9_], joaat(NAME) == MODEL), so Load Map
//                                                  can name the pack to load first
//
// Headerless files of the first Save Map hold eight-field rows (no flags), read as absolute.
// Relative maps use player-local coordinates (x right, y forward, z up) and rotate with the
// player's heading. At most GTAV_SCENE_MAP_KIND_MAX rows per kind (the spawned-entity rosters) and
// GTAV_SCENE_MAP_PACK_MAX distinct packs per file.

#define GTAV_SCENE_MAP_KIND_MAX 32
#define GTAV_SCENE_MAP_MAX (3 * GTAV_SCENE_MAP_KIND_MAX)
#define GTAV_SCENE_MAP_PACK_MAX 8
#define GTAV_SCENE_MAP_PACK_ID_MAX 65  // with the NUL, as GTAV_CUSTOM_PACK_ID_MAX
#define GTAV_SCENE_MAP_NAME_MAX 64     // with the NUL, as GTAV_CUSTOM_PACK_NAME_MAX
// A longer line is refused (a version 2 row with the longest id and name fits).
#define GTAV_SCENE_MAP_LINE_MAX 256
#define GTAV_SCENE_MAP_HEADER_ABSOLUTE "GTAVMAP,2,absolute"

enum { GTAV_SCENE_MAP_VEHICLE = 0, GTAV_SCENE_MAP_PED = 1, GTAV_SCENE_MAP_OBJECT = 2 };
enum { GTAV_SCENE_MAP_ABSOLUTE = 0, GTAV_SCENE_MAP_RELATIVE = 1 };
enum { GTAV_SCENE_MAP_FROZEN = 1u << 0, GTAV_SCENE_MAP_GROUND = 1u << 1 };
#define GTAV_SCENE_MAP_FLAG_MASK (GTAV_SCENE_MAP_FROZEN | GTAV_SCENE_MAP_GROUND)

typedef struct {
  int32_t kind;
  uint32_t model;
  float x, y, z;
  float rx, ry, rz;  // GET/SET_ENTITY_ROTATION order 2; rz doubles as the creation heading
  uint32_t flags;
  uint8_t pack;  // 0: no pack; n: pack_ids[n - 1] of the map
} GtavSceneMapEntry;

typedef struct {
  int version;  // 0 headerless, else the header's version
  int mode;     // GTAV_SCENE_MAP_ABSOLUTE / _RELATIVE
  int started;  // a header or a row was read (a header after that is refused)
  int count;
  int kind_counts[3];
  int pack_count;
  char pack_ids[GTAV_SCENE_MAP_PACK_MAX][GTAV_SCENE_MAP_PACK_ID_MAX];
  // Per pack: the first row's model name and how many rows name the pack (Load Map's refusal).
  char pack_models[GTAV_SCENE_MAP_PACK_MAX][GTAV_SCENE_MAP_NAME_MAX];
  int pack_rows[GTAV_SCENE_MAP_PACK_MAX];
  GtavSceneMapEntry entries[GTAV_SCENE_MAP_MAX];
} GtavSceneMap;

#ifdef __cplusplus
extern "C" {
#endif

// Lowercase joaat (the engine's model name hash).
uint32_t gtav_scene_map_joaat(const char* text);

void gtav_scene_map_init(GtavSceneMap* map);

// Read one line (NUL-terminated; a trailing CR/LF and surrounding blanks are ignored). Returns 0
// when the line was a blank, a comment, the header or a valid row (appended), -1 when the file is
// invalid at this line (bad header, bad row, a cap exceeded); the map is then not to be used.
int gtav_scene_map_feed(GtavSceneMap* map, const char* line);

// 1 when every field of the entry is in range for `mode` (finite, bounded coordinates, known kind
// and flags, a model hash).
int gtav_scene_map_entry_valid(const GtavSceneMapEntry* entry, int mode);

// Format one version 2 row with its newline. `pack_id` and `name` are both null/empty (a stock
// model) or both valid with joaat(name) == entry->model. Returns the length, or -1 when the entry
// is invalid or the row does not fit `size`.
int gtav_scene_map_format_row(char* out, size_t size, const GtavSceneMapEntry* entry,
                              const char* pack_id, const char* name);

#ifdef __cplusplus
}
#endif
