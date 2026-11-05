#pragma once

#include <stddef.h>
#include <stdint.h>

// Host-testable checks for the runtime pack lane (src/module/features/custom_pack_data.inc and
// custom_pack_autoload.inc): a structural upper bound on the entries the engine's XML parser can
// append from a data file, and the short menu text for a load refusal.

// Longest user-facing refusal text: "Load failed: " plus this fits the Custom Packs row label
// (GtavPackMenuRow.label[48]).
#define GTAV_CUSTOM_PACK_REASON_MAX 34u

#ifdef __cplusplus
extern "C" {
#endif

// Upper bound on the entries an engine data-file parser can append from the list element `list`
// (e.g. "InitDatas") of the XML text `text` (`size` bytes, not NUL-terminated): the number of
// element children of every element named `list` (ASCII case-insensitive, optionally prefixed
// "ns:", at any depth), whatever their own name or attribute spelling, plus every "<name" inside
// comments, CDATA sections and processing instructions. Self-closing children count; nested lists
// inside a child do not. When a skipped region could be read differently by another parser (any
// CDATA section, a comment or processing instruction holding '<', '>' or '&', a processing
// instruction after the prolog) the result is at least the flat count of every "<Item" in the
// file. Returns UINT32_MAX (refuse) when the text is not well-formed in a way the bound relies on:
// a NUL byte, an unterminated tag, comment, CDATA or processing instruction, "--" inside a comment
// or one ending in '-', a DOCTYPE or other declaration, a reference other than &amp; &gt; &quot;
// &apos; outside comments, '<' or '>' inside an attribute value, a tag name not followed by
// whitespace, '>' or "/>", a mismatched, stray or missing end tag (a truncated file), or nesting
// deeper than 64.
uint32_t gtav_custom_pack_xml_list_bound(const char* text, size_t size, const char* list);

// The same bound one generation deeper: the element grandchildren of every `list` element (e.g. the
// wheels of carcols <Wheels>, one array per wheel type whatever the type elements are named), with
// the same comment/CDATA/ambiguity rules and the same refusals.
uint32_t gtav_custom_pack_xml_grandchild_bound(const char* text, size_t size, const char* list);

// The same bounds over a file read in pieces (the worker reads data files larger than its buffer
// in chunks): init with the list name (not copied; it must outlive the stream) and generation 1
// (gtav_custom_pack_xml_list_bound) or 2 (..._grandchild_bound), feed every byte of the file in
// order in pieces of any size, then finish. The result equals the one-buffer function's for the
// same bytes (tests/test_custom_pack_xml_stream.py checks it over arbitrary splits). Both refuse an
// element name longer than GTAV_CUSTOM_PACK_XML_NAME_KEEP bytes, so the stream's end-tag match
// compares whole names.
#define GTAV_CUSTOM_PACK_XML_DEPTH_MAX 64u
#define GTAV_CUSTOM_PACK_XML_NAME_KEEP 48u
typedef struct {
  uint64_t hash;
  uint32_t length;
  uint8_t list;
  char head[GTAV_CUSTOM_PACK_XML_NAME_KEEP];
} GtavCustomPackXmlName;
typedef struct {
  const char* list;
  uint32_t list_length;
  uint32_t generation;
  int refused, state, prolog, ambiguous, self_closing, prev_lt, prev_question, double_dash;
  int region_ambiguous;
  char quote;
  uint32_t run;       // dashes (comment) or brackets (CDATA) just seen
  uint32_t bang;      // "<!" prefix characters matched
  uint32_t ref_mask;  // candidate references still matching after an '&' (0: none pending)
  uint32_t ref_at;    // characters of the reference matched
  uint32_t item_at;   // characters of "<item" matched
  uint64_t count, flat, markup;
  GtavCustomPackXmlName name;                 // the tag name being read
  char tail[GTAV_CUSTOM_PACK_XML_NAME_KEEP];  // its last characters (the list-name match)
  uint32_t depth;
  GtavCustomPackXmlName open[GTAV_CUSTOM_PACK_XML_DEPTH_MAX];
} GtavCustomPackXmlStream;
void gtav_custom_pack_xml_stream_init(GtavCustomPackXmlStream* stream, const char* list,
                                      uint32_t generation);
void gtav_custom_pack_xml_stream_feed(GtavCustomPackXmlStream* stream, const char* text,
                                      size_t size);
uint32_t gtav_custom_pack_xml_stream_finish(GtavCustomPackXmlStream* stream);

// Audio game-data chunk (AUDIO_GAMEDATA row, a .rel file) as the PS5 game-data loader reads it: u32
// type 151, data length, data, name table {length, count, offsets, NUL-terminated strings}, index
// {count, {hash, offset, length}}, hash table {count, file offsets}, pack table {count, file
// offsets}, nothing after it. The loader writes the resolved hash/pack references into the data and
// binary-searches the index per hash low byte, so this refuses: any table outside the file,
// trailing bytes, no objects or more than GTAV_CUSTOM_PACK_AUDIO_OBJECTS_MAX, an object outside the
// data (offset < 4, length < 4) or of class 0, an index not strictly sorted by (hash & 0xff, hash),
// a reference outside the data, an unterminated name, and the object the GAMEDATA mounter
// re-initialises (hash 0x953bd40d).
// tools/gtavmenu_tools/audio_rel.py (parse + check) applies the same rules.
#define GTAV_CUSTOM_PACK_AUDIO_TYPE 151u
#define GTAV_CUSTOM_PACK_AUDIO_OBJECTS_MAX 65536u
#define GTAV_CUSTOM_PACK_AUDIO_REINIT_HASH 0x953bd40du

typedef struct {
  uint32_t data_size;     // data block bytes (file offset 8)
  uint32_t index_offset;  // file offset of the first {hash, offset, length} entry
  uint32_t objects;       // index entries
  uint32_t hash_refs;     // hash-table (object reference) entries
  uint32_t pack_refs;     // pack-table (wave bank reference) entries
} GtavAudioRelInfo;

// NULL when `rel` (`size` bytes) is a chunk the loader can take, else the refusal text. `info` is
// filled on success (may be NULL).
const char* gtav_custom_pack_audio_rel_check(const uint8_t* rel, size_t size,
                                             GtavAudioRelInfo* info);

// Short menu text (at most GTAV_CUSTOM_PACK_REASON_MAX characters) for the technical refusal a
// pack-lane step returned, or `technical` itself when it is already plain (or unknown). The
// technical text stays in the klog / pack-notes line. Idempotent: a returned text maps to itself.
// A store-headroom refusal gets its store's text (gtav_custom_pack_store_reason).
const char* gtav_custom_pack_user_reason(const char* technical);

// Store-headroom gates (custom_rpf.inc kPackStoreGates): LoadImage adds one slot per archive member
// to its asset store, and a full slot pool traps the game, so a load first requires free slots.
// The scope says which selected packs need the store.
#define GTAV_CUSTOM_PACK_STORE_ANY 0      // every load (archive members of any pack)
#define GTAV_CUSTOM_PACK_STORE_PED 1      // packs with PED_METADATA_FILE / apparel rows
#define GTAV_CUSTOM_PACK_STORE_PTFX 2     // packs with ptfx spawn rows
#define GTAV_CUSTOM_PACK_STORE_VEHICLE 3  // packs with VEHICLE_METADATA_FILE rows (vehicle models)
#define GTAV_CUSTOM_PACK_STORE_WEAPON 4   // packs with weapon model, component or weapon-info rows

// Archive members by the asset store LoadImage's RegisterFile finds or adds their slot in (by base
// name, so a member whose name already has a slot adds none: the count is an upper bound), in the
// order of custom_rpf.inc kPackStoreGates. Other extensions (pld, pcd, ...) are not gated.
enum {
  GTAV_CUSTOM_PACK_MEMBER_FRAG = 0,  // .pft  FragmentStore
  GTAV_CUSTOM_PACK_MEMBER_DRAWABLE,  // .pdr  DrawableStore
  GTAV_CUSTOM_PACK_MEMBER_TXD,       // .ptd  TxdStore
  GTAV_CUSTOM_PACK_MEMBER_TYP,       // .ptyp map types store
  GTAV_CUSTOM_PACK_MEMBER_MAPDATA,   // .pmap map data store
  GTAV_CUSTOM_PACK_MEMBER_BOUNDS,    // .pbn  static bounds store
  GTAV_CUSTOM_PACK_MEMBER_METADATA,  // .pmt  MetaDataStore
  GTAV_CUSTOM_PACK_MEMBER_DWD,       // .pdd  DwdStore
  GTAV_CUSTOM_PACK_MEMBER_PTFX,      // .ppt  PtFxAssetStore
  GTAV_CUSTOM_PACK_MEMBER_KINDS
};

// The store of the archive member named `name` (`length` bytes, its extension compared
// case-insensitively), or -1 for an extension no gate counts.
int gtav_custom_pack_member_store(const char* name, size_t length);

typedef struct {
  uint32_t members[GTAV_CUSTOM_PACK_MEMBER_KINDS];  // file entries per store
  uint32_t files;                                   // every file entry
  uint32_t other;                                   // file entries of no gated store
} GtavCustomPackTocCounts;

// Count the file entries of a plaintext RPF7 table: `entries` 16-byte entries ({u64 q, u32, u32};
// a directory has q >> 32 == 0x7FFFFF00 and lists its children as {u32 first, u32 count}, the name
// is at name-pool offset q & 0xFFFF) followed by the name pool, `bytes` in all (entries * 16 + the
// header's name-pool size). Returns 0 (out filled) or -1 when the table cannot be the one LoadImage
// reads: fewer than 2 entries, entries past `bytes`, an empty name pool, entry 0 not a directory, a
// name offset outside the pool or unterminated in it, or a directory whose children are not after
// it, run past the table or overlap an earlier directory's (every RPF7 writer lays the tree out
// breadth-first, so the children ranges ascend in directory order and each entry is listed at most
// once: the count is an upper bound however LoadImage walks the tree).
int gtav_custom_pack_toc_counts(const uint8_t* table, size_t bytes, uint32_t entries,
                                GtavCustomPackTocCounts* out);

// Free slots a load needs in member store `kind` for `adds` slots (archive members plus by-name
// adds): adds plus the store's margin for what the game itself adds meanwhile (frag, drawable, txd,
// dwd 16; typ, mapdata, bounds 8; ptfx 2; metadata 1), or 0 when the load adds nothing there.
// Saturates at UINT32_MAX.
uint32_t gtav_custom_pack_store_need(int kind, uint32_t adds);
// Longest store-headroom refusal (technical) and detail (player) text, every store and count.
#define GTAV_CUSTOM_PACK_STORE_REFUSAL_MAX 73u
#define GTAV_CUSTOM_PACK_STORE_DETAIL_MAX 59u

// The technical refusal of store `store` (frag, drawable, txd, typ, mapdata, bounds, metadata, dwd,
// ptfx, or the model / file tables vehicle, pedmodel, wmodel, wcomp, winfo):
// "pack <store> store headroom gate refused free=<n> need=<m>", at most
// GTAV_CUSTOM_PACK_STORE_REFUSAL_MAX characters (it fits a feature message). It goes to the klog /
// pack-notes as it is; gtav_custom_pack_store_reason turns it into the menu text.
void gtav_custom_pack_store_refusal(char* out, size_t size, const char* store, uint32_t free_slots,
                                    uint32_t need);

// Menu text for a store-headroom refusal (exactly the gtav_custom_pack_store_refusal shape):
// returns the short text ("Ped metadata slots full", at most GTAV_CUSTOM_PACK_REASON_MAX), writes
// "<Store> slots full (<n> free, need <m>)" to `detail` (when non-NULL; at most
// GTAV_CUSTOM_PACK_STORE_DETAIL_MAX characters) and the store's GTAV_CUSTOM_PACK_STORE_* scope to
// `*scope` (when non-NULL). NULL (nothing written) for any other text or an unknown store.
const char* gtav_custom_pack_store_reason(const char* technical, char* detail, size_t size,
                                          int* scope);

// One-press load, carcols gate wait (custom_pack_autoload.inc pack_al_gate_wait): while tuned cars
// draw from the kit or wheel arrays a data row would grow, the row is asked again every RETRY_MS
// until its gate passes, for at most WAIT_MS.
#define GTAV_CUSTOM_PACK_GATE_RETRY_MS 2000u
#define GTAV_CUSTOM_PACK_GATE_WAIT_MS 180000u
typedef enum {
  GTAV_CUSTOM_PACK_GATE_NONE = 0,  // not waiting: the step's own refusal rules apply
  GTAV_CUSTOM_PACK_GATE_HOLD,      // waiting: for the last ask's answer or the next retry
  GTAV_CUSTOM_PACK_GATE_ASK,       // ask the row again
  GTAV_CUSTOM_PACK_GATE_GIVE_UP,   // waited WAIT_MS and the gate still refuses: fail
} GtavCustomPackGateWait;
// `waiting`: a wait has started; `refused`: the last ask answered with a refusal, `in_use` when it
// was the gate's in-use one (or, once waiting, the game was busy), so the wait starts or goes on;
// `waited_ms` since the wait started, `asked_ms` since the last ask. An unanswered ask is held even
// past WAIT_MS: it may be loading the row.
GtavCustomPackGateWait gtav_custom_pack_gate_wait(int waiting, int refused, int in_use,
                                                  uint64_t waited_ms, uint64_t asked_ms);

#ifdef __cplusplus
}
#endif
