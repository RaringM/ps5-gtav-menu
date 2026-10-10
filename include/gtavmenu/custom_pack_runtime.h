#pragma once

#include <stddef.h>
#include <stdint.h>

// Runtime pack descriptor (resources/pack.cfg): one engine archive registered through the memory
// device, the texture dictionaries the operator may request and show, and the data files the
// worker may load. Canonical ASCII, LF line endings, tab-separated fields, fixed line order:
//
//   GTAVPACK,1
//   pack\t<id>
//   description\t<text>                          (optional, 0..1; Manage Packs shows it with the
//                                                 author, the version and what the pack adds)
//   author\t<text>                               (optional, 0..1)
//   version\t<text>                              (optional, 0..1; [A-Za-z0-9._+-])
//   archive\t<size>\t<sha256>\t<name>.rpf[\toverlay]     (1..GTAV_CUSTOM_PACK_ARCHIVE_MAX)
//   override\t<archive>.rpf\t[<folder>/]<stem>.<ptd|pft|pdr|pdd>
//                                                 (0..GTAV_CUSTOM_PACK_OVERRIDE_MAX)
//   card\t<dict>\t<texture>                        (0..GTAV_CUSTOM_PACK_CARD_MAX)
//   typ\t<name>.ptyp[\tretail]                     (0..GTAV_CUSTOM_PACK_TYP_MAX; `retail`
//                                                  names a stock typ the pack's archetypes
//                                                  need resident, e.g. an interior's
//                                                  itypDependencies; not an archive member)
//   typdep\t<typ>.ptyp\t<stock>.ptyp                (one per pack typ row; binds the typ's
//                                                  ITYP dependency on a stock typ, as a retail
//                                                  _manifest itypDependencies entry does, so
//                                                  the engine streams the stock typ with it)
//   map\t<name>.pmap[\t<x>\t<y>\t<z>\t<menu text>]  (0..GTAV_CUSTOM_PACK_MAP_MAX; optional
//                                                  Custom Packs teleport, whole metres)
//   mapdep\t<map>.pmap\t<typ>.ptyp[\tinterior|\tretail]
//                                                 (one per map; binds the map's ITYP
//                                                  dependency, `interior` places an MLO. The
//                                                  typ is, in this order: a typ row of this
//                                                  descriptor; with `retail`, a stock typ by
//                                                  name that no row lists (never an interior);
//                                                  else another active pack's own typ, bound by
//                                                  the merge)
//   bounds\t<name>.pbn                            (0..GTAV_CUSTOM_PACK_BOUNDS_MAX; static
//                                                  collision, composite root, world space)
//   data\t<type>\t<size>\t<sha256>\t<file>          (0..GTAV_CUSTOM_PACK_DATA_MAX; <file> is
//                                                 <name>.meta, or for AUDIO_GAMEDATA
//                                                 <chunk>_game.rel: an audio game-data chunk
//                                                 named <chunk> ([a-z0-9], 1..31 characters))
//   wicon\t<weapon>\t<donor weapon>                 (0..GTAV_CUSTOM_PACK_WICON_MAX; after the
//                                                 data rows: the weapon wheel shows the
//                                                 donor's icon for a pack weapon whose name has
//                                                 none, by aliasing the hud.gfx frame label;
//                                                 the donor must share the weapon's WheelSlot)
//   label\t<KEY>\t<text>                          (0..GTAV_CUSTOM_PACK_LABEL_MAX)
//   spawn\t<vehicle|object|ped|weapon|timecycle|ptfx|component>\t<model>\t<menu text>
//                                                 (0..GTAV_CUSTOM_PACK_SPAWN_MAX; a timecycle
//                                                 row names a pack screen-effect modifier; a
//                                                 ptfx row's model is <asset>:<effect>, a
//                                                 particle dictionary (<asset>.ppt archive
//                                                 member) and one of its effects; a component
//                                                 row's model is <weapon>:<component>, a
//                                                 weapon component toggled on that weapon)
//   tints\t<weapon>\t<palette|none>               (0..1 per `spawn weapon` row, after the spawn
//                                                 rows: whether the weapon's model reads a tint
//                                                 palette; `none` makes Weapon Tint skip the
//                                                 setter with "no tint palette", and the Weapon
//                                                 Browser footer shows "Tints: 8" or "Tints: none")
//   place\t<x>\t<y>\t<z>\t<menu text>              (0..GTAV_CUSTOM_PACK_PLACE_MAX; a Custom Packs
//                                                 teleport not tied to a map row, whole metres)
//   hide\t<model>\t<x>\t<y>\t<z>\t<radius>          (0..GTAV_CUSTOM_PACK_HIDE_MAX; hides the stock
//                                                 map entities of <model> (joaat of the name)
//                                                 within <radius> metres of the point for the
//                                                 session, once the pack's maps are loaded;
//                                                 whole metres, radius 1..HIDE_RADIUS_MAX)
//
// `place` rows follow every other row and `hide` rows come last. The description, author and
// version texts are printable ASCII without '~' (the game's text renderer reads it as a format
// token), under GTAV_CUSTOM_PACK_DESCRIPTION_MAX / _AUTHOR_MAX / _VERSION_MAX characters; the
// parser checks and skips them (gtav_custom_pack_about reads them for the menu).
//
// Files live next to pack.cfg in <pack root>/<id>/resources/. Names are lowercase.
//
// Stock overrides: an `overlay` archive is registered
// with the engine's override permit and replaces the stock members its `override` rows name (stem
// [a-z0-9_+]); it holds nothing else. A member filed in a folder of its stock archive keeps that
// one folder level ([a-z0-9_], e.g. player_one/uppr_014_u.pdd in streamedpeds_players.rpf): the
// engine names the store slot after the member's path inside its archive. Every overlay archive has
// at least one override row and every override row names an overlay archive of the same descriptor.
//
// <pack root>/active names 1..GTAV_CUSTOM_PACK_ACTIVE_MAX pack ids, one per line. The worker
// merges their descriptors (gtav_custom_pack_merge) into one set whose arrays use the *_CAP
// capacities; each descriptor alone stays within the *_MAX limits.

#define GTAV_CUSTOM_PACK_DESCRIPTOR_MAX 8192u
#define GTAV_CUSTOM_PACK_MAGIC "GTAVPACK,1"
#define GTAV_CUSTOM_PACK_ID_MAX 65
#define GTAV_CUSTOM_PACK_NAME_MAX 64
// Optional descriptor texts (buffer sizes: at most one less character).
#define GTAV_CUSTOM_PACK_DESCRIPTION_MAX 81
#define GTAV_CUSTOM_PACK_AUTHOR_MAX 33
#define GTAV_CUSTOM_PACK_VERSION_MAX 17
// Longest audio chunk name (the mounter copies at most 31 characters before the first '_').
#define GTAV_CUSTOM_PACK_AUDIO_CHUNK_MAX 31
#define GTAV_CUSTOM_PACK_CARD_MAX 8
// Archives per pack: the first is `archive*` below, the rest `extra_archives`.
#define GTAV_CUSTOM_PACK_ARCHIVE_MAX 4
#define GTAV_CUSTOM_PACK_DATA_MAX 16
// Archetype definitions (.ptyp members of the archive, or stock `retail` typs) requested through
// DLC_ITYP_REQUEST.
#define GTAV_CUSTOM_PACK_TYP_MAX 16
// Map data (.pmap members of the archive) activated in the map-data store.
#define GTAV_CUSTOM_PACK_MAP_MAX 8
// Static collision bounds (.pbn members of the archive) requested keep-resident in the bounds
// store.
#define GTAV_CUSTOM_PACK_BOUNDS_MAX 8
// Text labels (e.g. a vehicle gameName): key [A-Za-z0-9_], printable ASCII text without tabs. One
// pack may hold up to 128 of the set's GTAV_CUSTOM_PACK_LABEL_CAP (a converted car with a large LSC
// kit: the Dominator GTX has 87 part labels); the per-pack limit costs no memory, the set cap does.
#define GTAV_CUSTOM_PACK_LABEL_MAX 128
// Menu entries for the pack's spawnable models (Custom Packs page).
#define GTAV_CUSTOM_PACK_SPAWN_MAX 24
#define GTAV_CUSTOM_PACK_SPAWN_TEXT_MAX 40
// `tints` row values per spawn row (spawn_tints): no row, `palette`, `none`.
enum {
  GTAV_CUSTOM_PACK_TINTS_UNKNOWN = 0,
  GTAV_CUSTOM_PACK_TINTS_PALETTE = 1,
  GTAV_CUSTOM_PACK_TINTS_NONE = 2,
};
// Spawn kinds: a vehicle or object to create, a ped to spawn, a weapon to give the player, a
// timecycle modifier (screen effect) to apply and clear, a particle effect to play once, or a
// weapon component to toggle on a weapon.
enum {
  GTAV_CUSTOM_PACK_SPAWN_VEHICLE = 1,
  GTAV_CUSTOM_PACK_SPAWN_OBJECT = 2,
  GTAV_CUSTOM_PACK_SPAWN_PED = 3,
  GTAV_CUSTOM_PACK_SPAWN_WEAPON = 4,
  GTAV_CUSTOM_PACK_SPAWN_TIMECYCLE = 5,
  GTAV_CUSTOM_PACK_SPAWN_PTFX = 6,
  GTAV_CUSTOM_PACK_SPAWN_COMPONENT = 7,
};
#define GTAV_CUSTOM_PACK_PLACE_COORD_MAX 16000
// Map-less teleport rows and stock-entity hide rows (CREATE_MODEL_HIDE) per pack.
#define GTAV_CUSTOM_PACK_PLACE_MAX 8
#define GTAV_CUSTOM_PACK_HIDE_MAX 32
#define GTAV_CUSTOM_PACK_HIDE_RADIUS_MAX 200
#define GTAV_CUSTOM_PACK_OVERRIDE_MAX 16
// Weapon wheel icon aliases (`wicon` rows).
#define GTAV_CUSTOM_PACK_WICON_MAX 8
// Merged set (up to GTAV_CUSTOM_PACK_ACTIVE_MAX packs loaded together). The worker tracks the
// per-row load state of the data, typ, map, bounds and hide rows in bitmasks: data, typ and hide
// rows in uint64_t, map, bounds and card rows in uint32_t, so those caps stay <= 64 and <= 32
// (static_asserts next to the masks). The worker holds two GtavCustomPack (the loaded set, which
// the menu's selection check also merges into before the load, and the shared descriptor parse
// buffer), so every row costs twice its size; spawn rows also size the menu's Custom Packs and
// browser pages, so SPAWN_CAP grows less (8 packs x 8 spawn rows) than the other caps, which
// doubled.
#define GTAV_CUSTOM_PACK_ACTIVE_MAX 8
#define GTAV_CUSTOM_PACK_ARCHIVE_CAP 16
#define GTAV_CUSTOM_PACK_CARD_CAP 16
#define GTAV_CUSTOM_PACK_DATA_CAP 64
#define GTAV_CUSTOM_PACK_TYP_CAP 48
#define GTAV_CUSTOM_PACK_MAP_CAP 16
#define GTAV_CUSTOM_PACK_BOUNDS_CAP 32
#define GTAV_CUSTOM_PACK_LABEL_CAP 192
#define GTAV_CUSTOM_PACK_SPAWN_CAP 64
#define GTAV_CUSTOM_PACK_OVERRIDE_CAP 64
#define GTAV_CUSTOM_PACK_PLACE_CAP 32
#define GTAV_CUSTOM_PACK_HIDE_CAP 64
#define GTAV_CUSTOM_PACK_WICON_CAP 32
#define GTAV_CUSTOM_PACK_ARCHIVE_BYTES_MAX (64ull * 1024ull * 1024ull)
#define GTAV_CUSTOM_PACK_DATA_BYTES_MAX (4ull * 1024ull * 1024ull)
// Bytes the whole merged set may hold in archives and in data files. Each archive and data file is
// copied into its own never-freed direct-memory buffer, so these keep the set's memory to what the
// earlier 4-pack caps allowed at most (8 archives x 64 MiB, 32 data files x 4 MiB) while more,
// smaller packs load together.
#define GTAV_CUSTOM_PACK_SET_ARCHIVE_BYTES_MAX (512ull * 1024ull * 1024ull)
#define GTAV_CUSTOM_PACK_SET_DATA_BYTES_MAX (128ull * 1024ull * 1024ull)

// Data-file types, by their engine enum value (parser enum table, 01.010.002).
enum {
  GTAV_CUSTOM_PACK_DATA_HANDLING = 6,
  GTAV_CUSTOM_PACK_DATA_CARCOLS = 10,
  GTAV_CUSTOM_PACK_DATA_VEHICLE_METADATA = 73,
  GTAV_CUSTOM_PACK_DATA_VEHICLE_VARIATION = 135,
  GTAV_CUSTOM_PACK_DATA_VEHICLE_LAYOUTS = 165,
  // Peds and weapons: peds.meta and weaponarchetypes.meta parse inside
  // LoadDataFile (sync); weapons, components and weapon animations queue a parse like the metas.
  GTAV_CUSTOM_PACK_DATA_PED_METADATA = 71,
  GTAV_CUSTOM_PACK_DATA_WEAPON_METADATA = 72,
  GTAV_CUSTOM_PACK_DATA_WEAPONINFO = 78,
  GTAV_CUSTOM_PACK_DATA_WEAPONCOMPONENTSINFO = 79,
  GTAV_CUSTOM_PACK_DATA_WEAPON_ANIMATIONS = 118,
  // Clothing for an existing ped: queued like weapons; the post-parse step only
  // looks the DLC pmt up by name, so the pack archive must be registered first.
  GTAV_CUSTOM_PACK_DATA_SHOP_PED_APPAREL = 137,
  // Timecycle modifiers: sync XML parse, appends modifiers.
  GTAV_CUSTOM_PACK_DATA_TIMECYCLEMOD = 28,
  // An audio game-data chunk (dat151 .rel, e.g. a CarAudioSettings a pack vehicle's
  // audioNameHash names), loaded sync by the audio metadata mounter.
  GTAV_CUSTOM_PACK_DATA_AUDIO_GAMEDATA = 140,
};

typedef struct {
  char dict[GTAV_CUSTOM_PACK_NAME_MAX];
  char texture[GTAV_CUSTOM_PACK_NAME_MAX];
} GtavCustomPackCard;

typedef struct {
  uint32_t type;
  uint64_t size;
  uint8_t sha256[32];
  char file[GTAV_CUSTOM_PACK_NAME_MAX];
} GtavCustomPackData;

// A stock member replaced by overlay archive `archive` (0 = `archive`, i = extra_archives[i - 1]).
typedef struct {
  uint8_t archive;
  char member[GTAV_CUSTOM_PACK_NAME_MAX];
} GtavCustomPackOverride;

typedef struct {
  char pack_id[GTAV_CUSTOM_PACK_ID_MAX];
  char archive[GTAV_CUSTOM_PACK_NAME_MAX];
  uint64_t archive_size;
  uint8_t archive_sha256[32];
  struct {
    char name[GTAV_CUSTOM_PACK_NAME_MAX];
    uint64_t size;
    uint8_t sha256[32];
  } extra_archives[GTAV_CUSTOM_PACK_ARCHIVE_CAP - 1];
  int extra_archive_count;
  uint8_t archive_overlay[GTAV_CUSTOM_PACK_ARCHIVE_CAP];  // by archive index, as `override.archive`
  GtavCustomPackOverride overrides[GTAV_CUSTOM_PACK_OVERRIDE_CAP];
  int override_count;
  GtavCustomPackCard cards[GTAV_CUSTOM_PACK_CARD_CAP];
  int card_count;
  char typs[GTAV_CUSTOM_PACK_TYP_CAP][GTAV_CUSTOM_PACK_NAME_MAX];
  uint8_t typ_retail[GTAV_CUSTOM_PACK_TYP_CAP];  // 1: a stock typ (`retail`), not a pack member
  // Per typ row: the stock typ it depends on (`typdep`; empty = none).
  char typ_deps[GTAV_CUSTOM_PACK_TYP_CAP][GTAV_CUSTOM_PACK_NAME_MAX];
  int typ_count;
  char maps[GTAV_CUSTOM_PACK_MAP_CAP][GTAV_CUSTOM_PACK_NAME_MAX];
  struct {
    int32_t x, y, z;
    char text[GTAV_CUSTOM_PACK_SPAWN_TEXT_MAX];  // empty: the map row has no teleport
  } map_places[GTAV_CUSTOM_PACK_MAP_CAP];
  int map_count;
  // Per map row: its bound typ row + 1 (0 = none) and whether it places an interior (MLO).
  uint8_t map_dep_typ[GTAV_CUSTOM_PACK_MAP_CAP];
  uint8_t map_interior[GTAV_CUSTOM_PACK_MAP_CAP];
  // Per map row: the mapdep row's typ name ("" = no mapdep). With map_dep_typ 0 it names a stock
  // typ (map_dep_retail 1, `mapdep ... retail`) or another pack's typ that no merged pack has
  // provided yet; the map row then refuses to load.
  char map_dep_name[GTAV_CUSTOM_PACK_MAP_CAP][GTAV_CUSTOM_PACK_NAME_MAX];
  uint8_t map_dep_retail[GTAV_CUSTOM_PACK_MAP_CAP];
  char bounds[GTAV_CUSTOM_PACK_BOUNDS_CAP][GTAV_CUSTOM_PACK_NAME_MAX];
  int bounds_count;
  char label_keys[GTAV_CUSTOM_PACK_LABEL_CAP][GTAV_CUSTOM_PACK_NAME_MAX];
  char label_texts[GTAV_CUSTOM_PACK_LABEL_CAP][GTAV_CUSTOM_PACK_NAME_MAX];
  int label_count;
  struct {
    uint32_t kind;
    char model[GTAV_CUSTOM_PACK_NAME_MAX];
    char text[GTAV_CUSTOM_PACK_SPAWN_TEXT_MAX];
  } spawns[GTAV_CUSTOM_PACK_SPAWN_CAP];
  int spawn_count;
  // Per spawn row: its `tints` row (GTAV_CUSTOM_PACK_TINTS_*; only `spawn weapon` rows have one).
  uint8_t spawn_tints[GTAV_CUSTOM_PACK_SPAWN_CAP];
  GtavCustomPackData data[GTAV_CUSTOM_PACK_DATA_CAP];
  int data_count;
  // Merged sets: the descriptor (0-based, in `active` order) each row came from. A single parsed
  // descriptor has source_count 1 and every source 0.
  int source_count;
  char source_ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX];
  uint8_t extra_archive_source[GTAV_CUSTOM_PACK_ARCHIVE_CAP - 1];
  uint8_t data_source[GTAV_CUSTOM_PACK_DATA_CAP];
  uint8_t spawn_source[GTAV_CUSTOM_PACK_SPAWN_CAP];
  // The pack each label row came from (the menu names a vehicle with its own pack's make label).
  uint8_t label_source[GTAV_CUSTOM_PACK_LABEL_CAP];
  uint8_t map_source[GTAV_CUSTOM_PACK_MAP_CAP];
  // The pack a failing typ, bounds or hide row of the one-press load belongs to (menu text).
  uint8_t typ_source[GTAV_CUSTOM_PACK_TYP_CAP];
  uint8_t bounds_source[GTAV_CUSTOM_PACK_BOUNDS_CAP];
  uint8_t hide_source[GTAV_CUSTOM_PACK_HIDE_CAP];
  struct {
    int32_t x, y, z;
    char text[GTAV_CUSTOM_PACK_SPAWN_TEXT_MAX];
  } places[GTAV_CUSTOM_PACK_PLACE_CAP];
  int place_count;
  uint8_t place_source[GTAV_CUSTOM_PACK_PLACE_CAP];
  struct {
    char model[GTAV_CUSTOM_PACK_NAME_MAX];
    int32_t x, y, z;
    uint32_t radius;
  } hides[GTAV_CUSTOM_PACK_HIDE_CAP];
  int hide_count;
  // Weapon wheel icon aliases: weapon and donor weapon names (lowercase, as spawn weapon rows).
  struct {
    char weapon[GTAV_CUSTOM_PACK_NAME_MAX];
    char donor[GTAV_CUSTOM_PACK_NAME_MAX];
  } wicons[GTAV_CUSTOM_PACK_WICON_CAP];
  int wicon_count;
} GtavCustomPack;

#ifdef __cplusplus
extern "C" {
#endif

// Parse the descriptor in place. `buffer` needs one writable byte beyond `size` for the
// terminating NUL. Returns 0 only for a complete descriptor within every bound.
int gtav_custom_pack_parse(char* buffer, size_t size, GtavCustomPack* pack);

// Validate an `active` file body (1..GTAV_CUSTOM_PACK_ACTIVE_MAX distinct "<id>\n" lines) and copy
// the ids. Returns 0 on success.
int gtav_custom_pack_parse_active(const char* buffer, size_t size,
                                  char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX],
                                  int* count);

// 1 when `id` is a valid pack id ([a-z0-9-], starting and ending alphanumeric, shorter than
// GTAV_CUSTOM_PACK_ID_MAX), else 0.
int gtav_custom_pack_id_valid(const char* id);

// Toggle `id` in an active list (the ids parse_active produced): remove it when listed, otherwise
// append it. Returns 1 when added, 0 when removed, -1 when refused (invalid id or a full list),
// leaving the list unchanged.
int gtav_custom_pack_active_toggle(char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX],
                                   int* count, const char* id);

// Render an active list as the `active` file body ("<id>\n" per id) into `out` (NUL-terminated).
// Returns the body length, or -1 when the list is invalid or does not fit. A rendered non-empty
// list always parses back through parse_active.
int gtav_custom_pack_render_active(
    const char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX], int count, char* out,
    size_t size);

// Append parsed descriptor `pack` to `set` (an empty set takes it whole). Refuses, leaving `set`
// unchanged, when a capacity or a set byte cap would overflow or an archive, data file, typ, map,
// label key, spawn model, card, bounds file, override member or wicon weapon would repeat across
// packs (a `retail` typ row may repeat: requesting a stock typ twice is harmless; place and hide
// rows may repeat). A mapdep naming another pack's typ binds to that pack's own (non-`retail`) typ
// row once both are merged, in either order. Returns 0 on success.
int gtav_custom_pack_merge(GtavCustomPack* set, const GtavCustomPack* pack);

// gtav_custom_pack_clash results.
enum {
  GTAV_CUSTOM_PACK_CLASH_NONE = 0,  // the pack merges
  GTAV_CUSTOM_PACK_CLASH_NAME = 1,  // a name of the pack is already in the set
  GTAV_CUSTOM_PACK_CLASH_CAP = 2,   // a merged capacity or byte total would overflow
};

// Why `pack` (a parsed descriptor) would not merge into `set`: the checks of
// gtav_custom_pack_merge, which calls it; neither argument changes. Returns a
// GTAV_CUSTOM_PACK_CLASH_* value, or -1 when `pack` is not a parsed descriptor. `why` (may be NULL)
// gets a short text naming the repeated row ("map gm_x.pmap", "vehicle car_a") or the cap ("too
// many maps"), "" when the pack merges. With a single parsed descriptor as `set`, a NAME result
// means that pack holds the name.
int gtav_custom_pack_clash(const GtavCustomPack* set, const GtavCustomPack* pack, char* why,
                           size_t size);

// One line for the menu about descriptor text `text` (`size` bytes, read only): its description,
// version and author, then what it adds per kind ("Malibu house (v3, by Ann). Adds 2 vehicles,
// 1 map, 4 labels"). A cheap line scan for Manage Packs, not a validation: the parse decides
// whether the pack loads. Returns 0, or -1 (out = "") when `text` does not start with the magic.
int gtav_custom_pack_about(const char* text, size_t size, char* out, size_t out_size);

// Menu name of spawn row `spawn` (a vehicle): "<Make> <text>", the make being the text of a
// `*_MAKE` label of the row's own pack (the only one, else the one whose key contains the model
// name); the row text alone when the pack has no such label or the text starts with the make
// ("Nissan GT-R" stays). Writes a NUL-terminated name into `out`; returns 1 when the make was
// prefixed, 0 when the text is used as is, -1 for a bad argument (out = "" when size > 0).
int gtav_custom_pack_vehicle_name(const GtavCustomPack* set, int spawn, char* out, size_t size);

// What the menu shows for a pack weapon (Weapon Browser, Weapons > Attachments "Component"), read
// from the pack's own weapons.meta (WEAPONINFO_FILE row) text.
#define GTAV_CUSTOM_PACK_WEAPON_COMPONENTS_MAX 16
#define GTAV_CUSTOM_PACK_WEAPON_TEXT_MAX 48  // longest retail component name: 40 characters
typedef struct {
  char human_name[GTAV_CUSTOM_PACK_WEAPON_TEXT_MAX];  // <HumanNameHash> text, the wheel's label key
  char wheel_slot[GTAV_CUSTOM_PACK_WEAPON_TEXT_MAX];  // <WheelSlot> text ("WHEEL_SMG")
  int component_count;  // components listed under <AttachPoints> (may exceed the array)
  char components[GTAV_CUSTOM_PACK_WEAPON_COMPONENTS_MAX][GTAV_CUSTOM_PACK_WEAPON_TEXT_MAX];
  uint32_t default_mask;  // bit i: components[i]'s item says <Default value="true" /> (fitted
                          // with the weapon; the engine does not remove it)
} GtavCustomPackWeaponMeta;

// Find the weapon item whose <Name> is `weapon` (ASCII case-insensitive) in weapons.meta text
// `text` (`size` bytes, not NUL-terminated) and fill `out` from that item (up to the next
// "CWeaponInfo" item): its HumanNameHash and WheelSlot texts and, in file order, the <Name> of
// every item under its <AttachPoints> element (each cut to GTAV_CUSTOM_PACK_WEAPON_TEXT_MAX - 1
// characters), and which of them are the weapon's default parts (default_mask).
// Returns 0, or -1 (out zeroed) when no such weapon item is in the text.
int gtav_custom_pack_weapon_meta(const char* text, size_t size, const char* weapon,
                                 GtavCustomPackWeaponMeta* out);

// Short menu name of weapon component `name` (COMPONENT_..., as an <AttachPoints> item names it).
// With `row` (a pack's `spawn component` row text for it, e.g. "GM Pistol: 40-round clip on/off"):
// that text after its last ": " without a trailing " on/off", first letter upper case. Else the
// part's kind from the name: Standard Clip (CLIP_01), Extended Clip (CLIP_02), Clip 3 (CLIP_03),
// Clip (other CLIP_), Suppressor, Scope, Flashlight, Grip, Muzzle Brake, Compensator, Barrel,
// Finish (VARMOD), Camo, Rail Cover; else the name without "COMPONENT_". Always NUL-terminates
// `out` (size > 0).
void gtav_custom_pack_component_label(const char* name, const char* row, char* out, size_t size);

// Make `count` menu labels (NUL-terminated, `stride` bytes apart) distinct: a label equal (ASCII
// case-insensitive) to an earlier one gets " 2", " 3", ... (its text cut so the suffix fits).
void gtav_custom_pack_label_dedupe(char* labels, size_t stride, int count);

// Engine type name for a data type id ("HANDLING_FILE", ...), or NULL when not allowed.
const char* gtav_custom_pack_data_type_name(uint32_t type);

#ifdef __cplusplus
}
#endif
