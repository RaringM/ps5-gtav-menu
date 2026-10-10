#include "gtavmenu/custom_pack_checks.h"

#include <string.h>

// Parser differential (custom_pack_data.inc): the model factories append one record per element of
// the InitDatas array with no capacity check, while the worker's name scanner matches a single
// spelling of the name element. This walk counts the array's children structurally instead, so a
// spelling the scanner misses (single quotes, whitespace in tags, self-closing items, a child not
// named Item) still counts. Over-counting is safe (the gate refuses); under-counting is not, so:
//  - every construct the walk cannot model with certainty refuses the whole file (UINT32_MAX);
//  - markup inside a skipped region (comment, CDATA, processing instruction) counts as if parsed;
//  - when a skipped region could be read differently by a parser that does not skip it the same
//    way (any CDATA section, a comment or processing instruction holding '<', '>' or '&', a
//    processing instruction after the prolog), the result is at least the flat count of every
//    "<Item" in the file (ASCII case-insensitive), wherever it is.

#define GTAV_PACK_XML_DEPTH_MAX 64u

static int xml_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int xml_name_start(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == ':' || c >= 0x80u;
}

static int xml_name_char(unsigned char c) {
  return xml_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

static unsigned char xml_lower(unsigned char c) {
  return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

static int xml_starts(const char* text, size_t size, size_t at, const char* literal) {
  const size_t length = strlen(literal);
  return at + length <= size && memcmp(text + at, literal, length) == 0;
}

// Offset of `literal` at or after `from`, or SIZE_MAX.
static size_t xml_find(const char* text, size_t size, size_t from, const char* literal) {
  for (size_t at = from; at < size; ++at)
    if (xml_starts(text, size, at, literal)) return at;
  return SIZE_MAX;
}

// "<name" sequences in [from, to): what a parser that did not skip the region could open.
static uint32_t xml_markup_in(const char* text, size_t from, size_t to) {
  uint32_t count = 0;
  for (size_t at = from; at + 1u < to; ++at)
    if (text[at] == '<' && xml_name_start((unsigned char)text[at + 1u]) && count < UINT32_MAX)
      ++count;
  return count;
}

// 1 when [from, to) holds a character that could be markup to a parser reading it differently.
static int xml_region_ambiguous(const char* text, size_t from, size_t to) {
  for (size_t at = from; at < to; ++at)
    if (text[at] == '<' || text[at] == '>' || text[at] == '&') return 1;
  return 0;
}

// Every "<Item" (ASCII case-insensitive) in the file, inside or outside any region.
static uint64_t xml_flat_items(const char* text, size_t size) {
  static const char kItem[] = "<item";
  uint64_t count = 0;
  for (size_t at = 0; at + sizeof(kItem) - 1u <= size; ++at) {
    size_t k = 0;
    while (k < sizeof(kItem) - 1u &&
           xml_lower((unsigned char)text[at + k]) == (unsigned char)kItem[k])
      ++k;
    count += k == sizeof(kItem) - 1u;
  }
  return count;
}

// `name` is `want` (ASCII case-insensitive), alone or after a namespace prefix ("x:InitDatas").
static int xml_name_is(const char* name, size_t length, const char* want) {
  const size_t want_length = strlen(want);
  if (length < want_length) return 0;
  if (length > want_length && name[length - want_length - 1u] != ':') return 0;
  const char* tail = name + (length - want_length);
  for (size_t i = 0; i < want_length; ++i)
    if (xml_lower((unsigned char)tail[i]) != xml_lower((unsigned char)want[i])) return 0;
  return 1;
}

// Character data and attribute values may use only the predefined entities that cannot produce
// markup; anything else (&lt;, numeric references, undeclared names) refuses the file. A '&' in a
// comment is plain text there (retail carcols have "COVER_&_PLATED"); it only marks the region
// ambiguous.
static int xml_reference_ok(const char* text, size_t size, size_t at) {
  return xml_starts(text, size, at, "&amp;") || xml_starts(text, size, at, "&gt;") ||
         xml_starts(text, size, at, "&quot;") || xml_starts(text, size, at, "&apos;");
}

// Element children (generation 1) or grandchildren (generation 2) of every `list` element.
static uint32_t xml_bound(const char* text, size_t size, const char* list, size_t generation) {
  struct {
    size_t at, length;
    int list;
  } open[GTAV_PACK_XML_DEPTH_MAX];
  size_t depth = 0;
  uint64_t count = 0;
  int ambiguous = 0, prolog = 1;  // prolog: nothing but whitespace or a BOM seen yet
  if (!text || !list || !list[0]) return UINT32_MAX;
  for (size_t i = 0; i < size; ++i) {
    if (text[i] == '\0') return UINT32_MAX;  // a C-string reader would stop here: refuse
  }
  for (size_t i = 0; i < size;) {
    if (text[i] != '<') {
      const unsigned char c = (unsigned char)text[i];
      if (c == '&' && !xml_reference_ok(text, size, i)) return UINT32_MAX;
      if (!xml_space(c) && c != 0xefu && c != 0xbbu && c != 0xbfu) prolog = 0;
      ++i;
      continue;
    }
    if (xml_starts(text, size, i, "<!--")) {
      // XML forbids "--" inside a comment and a comment ending in '-'; a parser could end it there.
      const size_t from = i + 4u;
      const size_t end = xml_find(text, size, from, "-->");
      if (end == SIZE_MAX) return UINT32_MAX;
      if (xml_find(text, end, from, "--") != SIZE_MAX || (end > from && text[end - 1u] == '-'))
        return UINT32_MAX;
      count += xml_markup_in(text, from, end);
      ambiguous |= xml_region_ambiguous(text, from, end);
      i = end + 3u;
      continue;
    }
    if (xml_starts(text, size, i, "<![CDATA[")) {
      // A parser without CDATA support would read it as markup: always ambiguous.
      const size_t from = i + 9u;
      const size_t end = xml_find(text, size, from, "]]>");
      if (end == SIZE_MAX) return UINT32_MAX;
      count += xml_markup_in(text, from, end);
      ambiguous = 1;
      i = end + 3u;
      continue;
    }
    if (xml_starts(text, size, i, "<?")) {
      const size_t from = i + 2u;
      const size_t end = xml_find(text, size, from, "?>");
      if (end == SIZE_MAX) return UINT32_MAX;
      count += xml_markup_in(text, from, end);
      ambiguous |= !prolog || xml_region_ambiguous(text, from, end);
      prolog = 0;
      i = end + 2u;
      continue;
    }
    prolog = 0;
    if (i + 1u >= size) return UINT32_MAX;
    if (text[i + 1u] == '!') return UINT32_MAX;  // DOCTYPE, ENTITY, ...: entities could add markup
    if (text[i + 1u] == '/') {
      size_t at = i + 2u;
      const size_t name = at;
      while (at < size && xml_name_char((unsigned char)text[at])) ++at;
      const size_t length = at - name;
      while (at < size && xml_space((unsigned char)text[at])) ++at;
      if (!length || !depth || at >= size || text[at] != '>') return UINT32_MAX;
      --depth;
      if (open[depth].length != length || memcmp(text + open[depth].at, text + name, length) != 0)
        return UINT32_MAX;
      i = at + 1u;
      continue;
    }
    size_t at = i + 1u;
    const size_t name = at;
    if (!xml_name_start((unsigned char)text[at])) return UINT32_MAX;
    while (at < size && xml_name_char((unsigned char)text[at])) ++at;
    const size_t length = at - name;
    if (at >= size) return UINT32_MAX;
    if (!xml_space((unsigned char)text[at]) && text[at] != '>' && text[at] != '/')
      return UINT32_MAX;
    int self_closing = 0;
    for (;;) {
      if (at >= size) return UINT32_MAX;
      const char c = text[at];
      if (c == '>') {
        ++at;
        break;
      }
      if (c == '/') {
        if (at + 1u >= size || text[at + 1u] != '>') return UINT32_MAX;
        self_closing = 1;
        at += 2u;
        break;
      }
      if (c == '<') return UINT32_MAX;
      if (c == '&' && !xml_reference_ok(text, size, at)) return UINT32_MAX;
      if (c == '"' || c == '\'') {
        // A quote-unaware parser would end the tag at a quoted '>' (or see "/>" in it): refuse
        // either bracket inside a value.
        for (++at; at < size && text[at] != c; ++at)
          if (text[at] == '<' || text[at] == '>' ||
              (text[at] == '&' && !xml_reference_ok(text, size, at)))
            return UINT32_MAX;
        if (at >= size) return UINT32_MAX;
      }
      ++at;
    }
    if (depth >= generation && open[depth - generation].list) ++count;
    if (!self_closing) {
      if (depth >= GTAV_PACK_XML_DEPTH_MAX) return UINT32_MAX;
      open[depth].at = name;
      open[depth].length = length;
      open[depth].list = xml_name_is(text + name, length, list);
      ++depth;
    }
    i = at;
  }
  if (depth) return UINT32_MAX;  // unclosed elements (a truncated file)
  if (ambiguous) {
    const uint64_t flat = xml_flat_items(text, size);
    if (flat > count) count = flat;
  }
  return count < UINT32_MAX ? (uint32_t)count : UINT32_MAX;
}

uint32_t gtav_custom_pack_xml_list_bound(const char* text, size_t size, const char* list) {
  return xml_bound(text, size, list, 1u);
}

uint32_t gtav_custom_pack_xml_grandchild_bound(const char* text, size_t size, const char* list) {
  return xml_bound(text, size, list, 2u);
}

static uint32_t audio_u32(const uint8_t* rel, size_t at) {
  return (uint32_t)rel[at] | (uint32_t)rel[at + 1] << 8 | (uint32_t)rel[at + 2] << 16 |
         (uint32_t)rel[at + 3] << 24;
}

const char* gtav_custom_pack_audio_rel_check(const uint8_t* rel, size_t size,
                                             GtavAudioRelInfo* info) {
  if (!rel || size < 8 || size > UINT32_MAX) return "audio chunk truncated";
  if (audio_u32(rel, 0) != GTAV_CUSTOM_PACK_AUDIO_TYPE) return "audio chunk is not game data (151)";
  const uint32_t data = audio_u32(rel, 4);
  if (data < 4 || data > size - 8) return "audio chunk data outside the file";
  size_t at = 8u + data;
  // Name table (wave bank names): length counts the count word, the offsets and the strings.
  if (size - at < 8) return "audio chunk truncated";
  const uint32_t table = audio_u32(rel, at), names = audio_u32(rel, at + 4);
  if (table < 4 || (table - 4u) / 4u < names || table > size - at - 4u)
    return "audio chunk name table outside the file";
  const size_t strings = at + 8u + 4u * (size_t)names, strings_end = at + 4u + table;
  for (uint32_t i = 0; i < names; ++i) {
    const uint32_t offset = audio_u32(rel, at + 8u + 4u * i);
    if (offset >= strings_end - strings ||
        !memchr(rel + strings + offset, 0, strings_end - strings - offset))
      return "audio chunk name unterminated";
  }
  at = strings_end;
  if (size - at < 4) return "audio chunk truncated";
  const uint32_t objects = audio_u32(rel, at);
  if (!objects || objects > GTAV_CUSTOM_PACK_AUDIO_OBJECTS_MAX)
    return "audio chunk has no objects or too many";
  if ((size - at - 4u) / 12u < objects) return "audio chunk index outside the file";
  const size_t index = at + 4u;
  uint64_t previous = 0;
  for (uint32_t i = 0; i < objects; ++i) {
    const size_t e = index + 12u * i;
    const uint32_t hash = audio_u32(rel, e), offset = audio_u32(rel, e + 4),
                   length = audio_u32(rel, e + 8);
    if (offset < 4 || length < 4 || offset > data || length > data - offset)
      return "audio object outside the data";
    const uint64_t key = (uint64_t)(hash & 0xffu) << 32 | hash;
    if (i && key <= previous) return "audio index not sorted or a name repeats";
    previous = key;
    if (hash == GTAV_CUSTOM_PACK_AUDIO_REINIT_HASH) return "audio chunk defines a reserved object";
    if (rel[8u + offset] == 0) return "audio object has class 0";
  }
  at = index + 12u * (size_t)objects;
  uint32_t refs[2] = {0, 0};
  for (int t = 0; t < 2; ++t) {
    if (size - at < 4) return "audio chunk truncated";
    const uint32_t n = audio_u32(rel, at);
    if ((size - at - 4u) / 4u < n) return "audio chunk reference table outside the file";
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t offset = audio_u32(rel, at + 4u + 4u * i);
      if (offset < 8 || offset - 8u > data - 4u) return "audio reference outside the data";
    }
    refs[t] = n;
    at += 4u + 4u * (size_t)n;
  }
  if (at != size) return "audio chunk has bytes after its tables";
  if (info) {
    info->data_size = data;
    info->index_offset = (uint32_t)index;
    info->objects = objects;
    info->hash_refs = refs[0];
    info->pack_refs = refs[1];
  }
  return NULL;
}

// Load refusals as the Custom Packs row ("Load failed: <text>") and the toast show them. The pack
// lane's own strings name the gate that refused (kept in the klog and `menu-ctl.sh pack-notes`);
// this table turns them into what the player can do about it. First match wins: `exact` entries
// match the whole text (stage names that are substrings of other refusals), the rest any substring,
// so specific entries come before the generic tail. tests/test_custom_pack_checks.py keeps every
// refusal string in src/module/features/custom_*.inc on the load path covered.
static const struct {
  const char* match;
  int exact;
  const char* text;
} kUserReasons[] = {
    // The game build is not the pinned one (or its code was patched).
    {"pack data mounter anchors", 0, "Wrong game version (data loader)"},
    {"pack data pump anchors", 0, "Wrong game version (data loader)"},
    {"loose collection anchors", 0, "Wrong game version (data loader)"},
    {"memory device gate", 0, "Wrong game version (file device)"},
    {"memory route gate", 0, "Wrong game version (file device)"},
    {"pack code anchors", 0, "Wrong game version (archives)"},
    {"pack store anchors", 0, "Wrong game version (archives)"},
    {"toc key table pin", 0, "Wrong game version (archives)"},
    {"override store anchors", 0, "Wrong game version (overrides)"},
    {"override remover anchors", 0, "Wrong game version (overrides)"},
    {"typ store anchors", 0, "Wrong game version (maps)"},
    {"typ mounter anchors", 0, "Wrong game version (maps)"},
    {"map store anchors", 0, "Wrong game version (maps)"},
    {"bounds store anchors", 0, "Wrong game version (collision)"},
    {"interior bounds binder anchors", 0, "Wrong game version (interiors)"},
    {"interior box streamer anchors", 0, "Wrong game version (interiors)"},
    {"text anchors", 0, "Wrong game version (labels)"},
    {"apparel anchors", 0, "Wrong game version (clothing)"},
    {"apparel post-parse functor", 0, "Wrong game version (clothing)"},
    {"component array anchors", 0, "Wrong game version (components)"},
    // Capacity: too much loaded at once.
    {"vehicle model store headroom", 0, "Too many custom vehicles loaded"},
    {"ped model store headroom", 0, "Too many custom peds loaded"},
    {"ped metadata store headroom", 0, "Too many custom peds loaded"},
    {"weapon model store headroom", 0, "Too many custom weapon models"},
    {"component info array is full", 0, "Too many weapon components"},
    {"component file items exceed", 0, "Too many weapon components"},
    {"weapon info file list is full", 0, "Too many weapon files loaded"},
    {"archetype pool headroom", 0, "Too many models; load fewer packs"},
    {"interior proxy pool full", 0, "Too many interiors; restart GTA"},
    {"pack dmem headroom", 0, "Out of memory; load fewer packs"},
    {"pack pool memory query", 0, "Out of memory; load fewer packs"},
    {"pack buffer map failed", 0, "Out of memory; load fewer packs"},
    {"pack buffer needs PS5 direct memory", 0, "Out of memory; load fewer packs"},
    {"pack archive capacity gate", 0, "Game slots full; load fewer packs"},
    {"store headroom gate refused", 0, "Game slots full; load fewer packs"},
    // Pack files on the console.
    {"pack archive size gate", 0, "Pack files damaged; reinstall it"},
    {"pack buffer hash gate", 0, "Pack files damaged; reinstall it"},
    {"pack buffer too small", 0, "Pack files damaged; reinstall it"},
    {"pack data file size/hash gate", 0, "Pack files damaged; reinstall it"},
    {"pack data read size", 0, "Pack files damaged; reinstall it"},
    {"pack archive open refused", 0, "Pack file missing; reinstall it"},
    {"pack archive header gate", 0, "Pack archive is not a PS5 RPF"},
    {"plain toc", 0, "Pack archive format not supported"},
    {"pack has no registered members", 0, "Pack archive has no usable files"},
    {"pack descriptor missing or invalid", 0, "A pack.cfg is invalid; reinstall"},
    {"pack active file missing or invalid", 0, "Pack selection invalid; reselect"},
    {"active packs overflow or collide", 0, "Selected packs clash or too big"},
    {"pack descriptors busy", 0, "Menu busy; press Load again"},
    // Data files (meta XML).
    {"pack data type has no pinned mounter", 0, "Data file type not supported"},
    {"pack data memory name too long", 0, "Data file name too long"},
    {"timecycle file names no modifier", 0, "Screen effects: none or over 64"},
    // Audio game data (AUDIO_GAMEDATA .rel chunks).
    {"audio anchors", 0, "Wrong game version (audio)"},
    {"audio metadata format", 0, "Wrong game version (audio)"},
    {"audio chunk name already loaded", 0, "Audio name in use; rename it"},
    {"audio chunk is being removed", 0, "Game busy loading; retry later"},
    {"audio chunk defines a reserved", 0, "Audio file has a reserved object"},
    {"audio chunk", 0, "Audio file damaged or wrong format"},
    {"audio object", 0, "Audio file damaged or wrong format"},
    {"audio index", 0, "Audio file damaged or wrong format"},
    {"audio reference", 0, "Audio file damaged or wrong format"},
    {"override names no stock member", 0, "Override target not found"},
    {"names no", 0, "Data file empty or not XML"},
    {"is not plain XML", 0, "Data file is not valid XML"},
    {"outside <InitDatas>", 0, "Data file layout not supported"},
    {"loose entry already filled", 0, "Data file in use; restart GTA"},
    {"loose registration refused", 0, "Game refused the data file"},
    {"loose entry does not hold", 0, "Game refused the data file"},
    {" sync returned ", 0, "Data file did not load fully"},
    {" queued=", 0, "Game did not queue the data file"},
    {" apparel slots ", 0, "Clothing did not attach to the ped"},
    {" components ", 0, "Components did not register"},
    {" weapons ", 0, "Weapons did not register"},
    {"model name already exists", 0, "Model name already used"},
    {"weapon name already exists", 0, "Weapon name already used"},
    {"component name already exists", 0, "Component name already used"},
    {"component name repeated", 0, "Component name repeated in file"},
    {"modifier name already exists", 0, "Screen effect name already used"},
    {"modifier name repeated", 0, "Screen effect name repeated"},
    {"every <modifier needs", 0, "Each screen effect needs a name"},
    {"timecycle vars not initialised", 0, "Load in Story mode, then retry"},
    {"timecycle name map out of step", 0, "Game state unexpected; restart GTA"},
    // Carcols growth gate: the one-press load waits on it (custom_pack_autoload.inc), then fails.
    {"carcols would free mod parts", 0, "Tuned car nearby (parked ones too)"},
    {"carcols gate wait timed out", 1, "Tuned car nearby (parked ones too)"},
    {"apparel file needs one pedName", 0, "Clothing file lacks ped/dlc names"},
    {"fullDlcName must be", 0, "fullDlcName must be ped_dlcName"},
    {"apparel file already loaded", 0, "Clothing already added"},
    {"pmt is already on the ped", 0, "Clothing already added"},
    {"apparel pmt is not registered", 0, "Clothing archive not registered"},
    {"pedName is not a known model", 0, "Clothing ped model not found"},
    {"ped model is in use", 0, "Ped in use; restart and load first"},
    {"pack labels added=", 0, "Game refused some text labels"},
    {"carcols wheels are not supported", 0, "Custom wheels are not supported"},
    {"pack has no labels", 1, "Pack has no labels"},
    // Stock overrides.
    {"loaded or requested", 0, "Stock asset in use; restart GTA"},
    {"override member has no extension", 0, "Override target not found"},
    {"override type has no store", 0, "Override target not found"},
    {"override owner is not an archive", 0, "Override target not supported"},
    {"overlaid by a non-DLC archive", 0, "Stock file overlaid by another mod"},
    {"overlay archive holds more than", 0, "Override archive has extra files"},
    {"override did not take", 0, "Override did not apply"},
    {"could not remove the DLC overlay", 0, "Override did not apply"},
    {"overlay permit not set", 0, "Override did not apply"},
    // Maps, typs, collision, interiors.
    {"stock typ not in the typ store", 0, "Stock typ not found in the game"},
    // Typ->typ / map->stock dependency binding (typdep rows).
    {"typ dependency not in the typ store", 0, "Stock typ not found in the game"},
    {"typ dependency has no streaming file", 0, "Stock typ not found in the game"},
    {"map dependency typ has no streaming file", 0, "Stock typ not found in the game"},
    {"typ already has dependencies", 0, "Typ already bound; restart GTA"},
    {"typ def layout unexpected", 0, "Wrong game version (maps)"},
    // Manage Packs "Revert overrides".
    {"no stock overrides are loaded", 0, "No overrides loaded"},
    {"overrides already reverted", 0, "Overrides already reverted"},
    {"override revert did not take", 0, "Revert failed; see pack-notes"},
    {"stock typ waits", 0, "Game busy loading; retry later"},
    {"map dependency typ slot not found", 0, "Map archetype file not in pack"},
    {"pack map slot not found", 0, "Map file not found in pack"},
    {"map slot is not a fresh archive slot", 0, "Map file already in use"},
    {"map slot already initialised", 0, "Map file already in use"},
    {"map already has dependencies", 0, "Map file already in use"},
    {"map slot has other requirement flags", 0, "Map file not supported"},
    {"map bounds not filled", 0, "Game refused the map file"},
    {"map bounds load could not be removed", 0, "Map did not activate"},
    {"bounds slot not found", 0, "Collision file not found in pack"},
    {"bounds slot is not a fresh archive slot", 0, "Collision file already in use"},
    {" loaded type=", 0, "Collision file did not load"},
    {"interior typ meta freed", 0, "Interior data freed; restart GTA"},
    {"no interior proxy", 0, "Interior map lacks its proxy"},
    {"interior bounds not resident", 0, "Interior collision not loaded yet"},
    {"binder attached nothing", 0, "Interior collision did not attach"},
    {"alias box empty", 0, "Interior collision did not attach"},
    // Game state and sequencing.
    {"phase/thread gate", 0, "Game busy; retry in free roam"},
    {"text allocator unavailable", 0, "Game busy; retry in free roam"},
    {"parser is not idle", 0, "Game busy loading; retry later"},
    {"parser does not hold exactly our request", 0, "Game busy loading; retry later"},
    {"startup header cache still active", 0, "Game still starting; retry later"},
    {"pack typs not resident", 0, "Game busy loading; retry later"},
    {"already attempted", 0, "Packs already loaded; restart GTA"},
    {"already loaded", 0, "Packs already loaded; restart GTA"},
    {"already requested", 0, "Packs already loaded; restart GTA"},
    {"already finished", 0, "Packs already loaded; restart GTA"},
    {"labels already added", 0, "Packs already loaded; restart GTA"},
    {"pack is not registered", 0, "Packs not registered; restart GTA"},
    {"descriptor not loaded", 0, "Packs not registered; restart GTA"},
    {"row not requested", 0, "Pack rows out of step; restart GTA"},
    {"out of range", 0, "Pack rows out of step; restart GTA"},
    {"archive is not resident", 0, "Pack archive did not load"},
    {"archive streamable handle mismatch", 0, "Game refused the pack archive"},
    {"AddImageToList returned", 0, "Game refused the pack archive"},
    {"LoadImage preconditions", 0, "Game refused the pack archive"},
    // Stages that stopped without a refusal of their own (custom_pack_autoload.inc pack_al_fail).
    {"registration refused", 1, "Packs did not register; see notes"},
    {"data row refused", 1, "A data file was refused; see notes"},
    {"stock typ", 1, "Stock typ failed; see pack-notes"},
    {"map dependency", 1, "Map dependency failed; see notes"},
    {"map row refused", 1, "Map was refused; see pack-notes"},
    {"map activation", 1, "Map did not activate"},
    {"collision bounds", 1, "Collision did not load; see notes"},
    {"timed out in waiting for archetypes", 1, "Timed out waiting for archetypes"},
    // Custom Packs content rows, as game-thread jobs (features.cpp gtav_features_run_job).
    {"of the loaded packs", 0, "Pack rows out of step; restart GTA"},
    {" natives", 0, "Needs a newer menu build"},
    {"pack screen effect is not loaded", 0, "Screen effect did not load; see notes"},
    {" on (index ", 0, "Screen effect did not apply"},
    {"effect dictionary has no store slot", 0, "Effect file not found in pack"},
    {"pack effect did not load", 0, "Effect did not load; see notes"},
    {"start returned 0", 0, "Effect did not start; retry"},
    {"player ped unavailable", 0, "Player not ready; retry in free roam"},
    {"no valid player ped", 0, "Player not ready; retry in free roam"},
    {"component could not be removed", 0, "Attachment did not come off"},
    {"weapon refused the component", 0, "Weapon refused the attachment"},
    {"pack map has no place", 0, "Map has no teleport point"},
    // Generic tail.
    {"anchors mismatch", 0, "Wrong game version (game code)"},
    {"pin mismatch", 0, "Wrong game version (game code)"},
    {"functor mismatch", 0, "Wrong game version (game code)"},
    {"unreadable", 0, "Game state unreadable; restart GTA"},
    {"streaming info unavailable", 0, "Game state unreadable; restart GTA"},
};

const char* gtav_custom_pack_user_reason(const char* technical) {
  if (!technical) return "";
  for (size_t i = 0; i < sizeof(kUserReasons) / sizeof(kUserReasons[0]); ++i) {
    const int hit = kUserReasons[i].exact ? strcmp(technical, kUserReasons[i].match) == 0
                                          : strstr(technical, kUserReasons[i].match) != NULL;
    if (hit) return kUserReasons[i].text;
  }
  return technical;
}

GtavCustomPackGateWait gtav_custom_pack_gate_wait(int waiting, int refused, int in_use,
                                                  uint64_t waited_ms, uint64_t asked_ms) {
  if (in_use) {
    if (waited_ms >= GTAV_CUSTOM_PACK_GATE_WAIT_MS) return GTAV_CUSTOM_PACK_GATE_GIVE_UP;
    return asked_ms >= GTAV_CUSTOM_PACK_GATE_RETRY_MS ? GTAV_CUSTOM_PACK_GATE_ASK
                                                      : GTAV_CUSTOM_PACK_GATE_HOLD;
  }
  // Not waiting, or another refusal (the step fails as before); else the last ask is unanswered.
  return waiting && !refused ? GTAV_CUSTOM_PACK_GATE_HOLD : GTAV_CUSTOM_PACK_GATE_NONE;
}
