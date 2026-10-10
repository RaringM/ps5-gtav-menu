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
const char* gtav_custom_pack_user_reason(const char* technical);

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
