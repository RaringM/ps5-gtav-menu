#include "gtavmenu/custom_pack_checks.h"

#include <stdio.h>
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
      if (length > GTAV_CUSTOM_PACK_XML_NAME_KEEP) return UINT32_MAX;  // as the stream refuses
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
    if (length > GTAV_CUSTOM_PACK_XML_NAME_KEEP) return UINT32_MAX;  // as the stream refuses
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

// xml_bound as a byte-at-a-time state machine, for files read in pieces. Every rule above maps to
// a state: the regions end at their first terminator ("-->", "]]>", "?>"); a comment's content is
// what precedes the terminator's two dashes (so it ends in '-' when 3+ dashes precede the '>', and
// holds "--" when an earlier run of 2+ dashes was not the terminator); a reference is checked
// against the following characters while they are also read as text.
enum {
  XML_S_TEXT = 0,
  XML_S_LT,    // after '<'
  XML_S_BANG,  // after "<!": "--" or "[CDATA[" must follow
  XML_S_COMMENT,
  XML_S_CDATA,
  XML_S_PI,
  XML_S_END_NAME,    // after "</"
  XML_S_END_SPACE,   // after the end tag's name
  XML_S_START_NAME,  // in a start tag's name
  XML_S_TAG,         // in a start tag, outside quotes
  XML_S_TAG_SLASH,   // after '/' in a start tag: '>' must follow
  XML_S_TAG_QUOTE,   // in a quoted attribute value
};
#define XML_BANG_COMMENT 0x100u  // bang: matching "<!--"
#define XML_BANG_CDATA 0x200u    // bang: matching "<![CDATA["
static const char* const kXmlReferences[4] = {"amp;", "gt;", "quot;", "apos;"};

static void xml_name_start_at(GtavCustomPackXmlName* name) {
  memset(name, 0, sizeof(*name));
  name->hash = 0xcbf29ce484222325ull;  // FNV-1a 64
}

static void xml_name_add(GtavCustomPackXmlStream* s, unsigned char c) {
  GtavCustomPackXmlName* name = &s->name;
  // Names longer than the kept head are refused (as the one-buffer walk does): the end-tag match
  // then compares every byte, never only a hash (a crafted collision could otherwise undercount).
  if (name->length >= GTAV_CUSTOM_PACK_XML_NAME_KEEP) {
    s->refused = 1;
    return;
  }
  if (name->length < GTAV_CUSTOM_PACK_XML_NAME_KEEP) name->head[name->length] = (char)c;
  name->hash = (name->hash ^ c) * 0x100000001b3ull;
  s->tail[name->length % GTAV_CUSTOM_PACK_XML_NAME_KEEP] = (char)c;
  if (name->length < UINT32_MAX) name->length++;
}

// The name just read is `list`, alone or after a namespace prefix (xml_name_is), from its tail.
static int xml_stream_name_is_list(const GtavCustomPackXmlStream* s) {
  const uint32_t length = s->name.length, want = s->list_length;
  if (length < want) return 0;
  const uint32_t keep = GTAV_CUSTOM_PACK_XML_NAME_KEEP;
  if (length > want && s->tail[(length - want - 1u) % keep] != ':') return 0;
  for (uint32_t i = 0; i < want; ++i)
    if (xml_lower((unsigned char)s->tail[(length - want + i) % keep]) !=
        xml_lower((unsigned char)s->list[i]))
      return 0;
  return 1;
}

static int xml_names_equal(const GtavCustomPackXmlName* a, const GtavCustomPackXmlName* b) {
  const uint32_t head =
      a->length < GTAV_CUSTOM_PACK_XML_NAME_KEEP ? a->length : GTAV_CUSTOM_PACK_XML_NAME_KEEP;
  return a->length == b->length && a->hash == b->hash && memcmp(a->head, b->head, head) == 0;
}

static void xml_stream_start_tag_done(GtavCustomPackXmlStream* s, int self_closing) {
  if (s->depth >= s->generation && s->open[s->depth - s->generation].list) ++s->count;
  if (!self_closing) {
    if (s->depth >= GTAV_CUSTOM_PACK_XML_DEPTH_MAX) {
      s->refused = 1;
      return;
    }
    s->open[s->depth] = s->name;
    s->open[s->depth].list = (uint8_t)xml_stream_name_is_list(s);
    s->depth++;
  }
  s->state = XML_S_TEXT;
}

static void xml_stream_end_tag_done(GtavCustomPackXmlStream* s) {
  if (!s->depth) {
    s->refused = 1;
    return;
  }
  s->depth--;
  if (!xml_names_equal(&s->open[s->depth], &s->name)) s->refused = 1;
  s->state = XML_S_TEXT;
}

// Region content character (comment, CDATA, processing instruction): "<name" markup and the
// characters a parser reading the region differently could take as markup.
static void xml_stream_region_char(GtavCustomPackXmlStream* s, unsigned char c) {
  if (s->prev_lt && xml_name_start(c)) s->markup++;
  s->prev_lt = c == '<';
  if (c == '<' || c == '>' || c == '&') s->region_ambiguous = 1;
}

static void xml_stream_region_open(GtavCustomPackXmlStream* s, int state) {
  s->state = state;
  s->run = 0;
  s->markup = 0;
  s->prev_lt = 0;
  s->prev_question = 0;
  s->double_dash = 0;
  s->region_ambiguous = 0;
}

static void xml_stream_step(GtavCustomPackXmlStream* s, unsigned char c) {
  if (c == '\0') {
    s->refused = 1;
    return;
  }
  // Every "<item" in the file (xml_flat_items).
  static const char kItem[] = "<item";
  if (xml_lower(c) == (unsigned char)kItem[s->item_at]) {
    if (++s->item_at == sizeof(kItem) - 1u) {
      s->flat++;
      s->item_at = 0;
    }
  } else {
    s->item_at = c == '<';
  }
  // A pending reference: the characters after its '&' must spell one of kXmlReferences.
  if (s->ref_mask) {
    uint32_t still = 0;
    for (uint32_t k = 0; k < 4u; ++k) {
      if (!(s->ref_mask & (1u << k)) || (unsigned char)kXmlReferences[k][s->ref_at] != c) continue;
      if (kXmlReferences[k][s->ref_at + 1u] == '\0') {
        still = 0;
        s->ref_mask = 0;
        goto reference_done;
      }
      still |= 1u << k;
    }
    if (!still) {
      s->refused = 1;
      return;
    }
    s->ref_mask = still;
    s->ref_at++;
  }
reference_done:
  switch (s->state) {
    case XML_S_TEXT:
      if (c == '<') {
        s->state = XML_S_LT;
        return;
      }
      if (c == '&') {
        s->ref_mask = 0xfu;
        s->ref_at = 0;
      }
      if (!xml_space(c) && c != 0xefu && c != 0xbbu && c != 0xbfu) s->prolog = 0;
      return;
    case XML_S_LT:
      if (c == '!') {
        s->state = XML_S_BANG;
        s->bang = 0;
        return;
      }
      if (c == '?') {
        xml_stream_region_open(s, XML_S_PI);
        return;
      }
      s->prolog = 0;
      xml_name_start_at(&s->name);
      if (c == '/') {
        s->state = XML_S_END_NAME;
        return;
      }
      if (!xml_name_start(c)) {
        s->refused = 1;
        return;
      }
      s->state = XML_S_START_NAME;
      xml_name_add(s, c);
      return;
    case XML_S_BANG: {
      // bang: the matched characters after "<!", tagged with the candidate once the first is known.
      const uint32_t at = s->bang & 0xffu;
      uint32_t kind = s->bang & ~0xffu;
      if (!kind) kind = c == '-' ? XML_BANG_COMMENT : c == '[' ? XML_BANG_CDATA : 0u;
      const char* want = kind == XML_BANG_COMMENT ? "--" : kind == XML_BANG_CDATA ? "[CDATA[" : "";
      if (!kind || (unsigned char)want[at] != c) {
        s->refused = 1;  // DOCTYPE, ENTITY, ...: entities could add markup
        return;
      }
      s->bang = kind | (at + 1u);
      if (want[at + 1u] == '\0')
        xml_stream_region_open(s, kind == XML_BANG_COMMENT ? XML_S_COMMENT : XML_S_CDATA);
      return;
    }
    case XML_S_COMMENT:
      if (c == '-') {
        s->run++;
        s->prev_lt = 0;
        return;
      }
      if (c == '>' && s->run >= 2u) {
        // XML forbids "--" inside a comment and a comment ending in '-'.
        if (s->double_dash || s->run >= 3u) {
          s->refused = 1;
          return;
        }
        s->count += s->markup;
        s->ambiguous |= s->region_ambiguous;
        s->state = XML_S_TEXT;
        return;
      }
      if (s->run >= 2u) s->double_dash = 1;
      s->run = 0;
      xml_stream_region_char(s, c);
      return;
    case XML_S_CDATA:
      if (c == ']') {
        s->run++;
        s->prev_lt = 0;
        return;
      }
      if (c == '>' && s->run >= 2u) {
        // A parser without CDATA support would read it as markup: always ambiguous.
        s->count += s->markup;
        s->ambiguous = 1;
        s->state = XML_S_TEXT;
        return;
      }
      s->run = 0;
      xml_stream_region_char(s, c);
      return;
    case XML_S_PI:
      if (c == '>' && s->prev_question) {
        s->count += s->markup;
        s->ambiguous |= !s->prolog || s->region_ambiguous;
        s->prolog = 0;
        s->state = XML_S_TEXT;
        return;
      }
      s->prev_question = c == '?';
      xml_stream_region_char(s, c);
      return;
    case XML_S_END_NAME:
      if (xml_name_char(c)) {
        xml_name_add(s, c);
        return;
      }
      if (!s->name.length) {
        s->refused = 1;
        return;
      }
      if (xml_space(c)) {
        s->state = XML_S_END_SPACE;
        return;
      }
      if (c == '>') {
        xml_stream_end_tag_done(s);
        return;
      }
      s->refused = 1;
      return;
    case XML_S_END_SPACE:
      if (xml_space(c)) return;
      if (c == '>') {
        xml_stream_end_tag_done(s);
        return;
      }
      s->refused = 1;
      return;
    case XML_S_START_NAME:
      if (xml_name_char(c)) {
        xml_name_add(s, c);
        return;
      }
      // The character after the name: whitespace, '>' or "/>".
      if (c == '>') {
        xml_stream_start_tag_done(s, 0);
      } else if (c == '/') {
        s->state = XML_S_TAG_SLASH;
      } else if (xml_space(c)) {
        s->state = XML_S_TAG;
      } else {
        s->refused = 1;
      }
      return;
    case XML_S_TAG:
      if (c == '>') {
        xml_stream_start_tag_done(s, 0);
        return;
      }
      if (c == '/') {
        s->state = XML_S_TAG_SLASH;
        return;
      }
      if (c == '<') {
        s->refused = 1;
        return;
      }
      if (c == '&') {
        s->ref_mask = 0xfu;
        s->ref_at = 0;
        return;
      }
      if (c == '"' || c == '\'') {
        s->quote = (char)c;
        s->state = XML_S_TAG_QUOTE;
      }
      return;
    case XML_S_TAG_SLASH:
      if (c != '>') {
        s->refused = 1;
        return;
      }
      xml_stream_start_tag_done(s, 1);
      return;
    case XML_S_TAG_QUOTE:
      if (c == (unsigned char)s->quote) {
        s->state = XML_S_TAG;
        return;
      }
      if (c == '<' || c == '>') {
        s->refused = 1;
        return;
      }
      if (c == '&') {
        s->ref_mask = 0xfu;
        s->ref_at = 0;
      }
      return;
    default:
      s->refused = 1;
      return;
  }
}

void gtav_custom_pack_xml_stream_init(GtavCustomPackXmlStream* stream, const char* list,
                                      uint32_t generation) {
  if (!stream) return;
  memset(stream, 0, sizeof(*stream));
  stream->list = list;
  stream->generation = generation;
  stream->prolog = 1;
  stream->state = XML_S_TEXT;
  const size_t length = list ? strlen(list) : 0;
  // The tail keeps the list name and the ':' before it.
  if (!list || !length || length + 1u > GTAV_CUSTOM_PACK_XML_NAME_KEEP || generation < 1u ||
      generation > 2u)
    stream->refused = 1;
  stream->list_length = (uint32_t)length;
}

void gtav_custom_pack_xml_stream_feed(GtavCustomPackXmlStream* stream, const char* text,
                                      size_t size) {
  if (!stream || stream->refused) return;
  if (!text) {
    if (size) stream->refused = 1;
    return;
  }
  for (size_t i = 0; i < size && !stream->refused; ++i)
    xml_stream_step(stream, (unsigned char)text[i]);
}

uint32_t gtav_custom_pack_xml_stream_finish(GtavCustomPackXmlStream* stream) {
  if (!stream || stream->refused) return UINT32_MAX;
  // Inside a tag, region or reference, or unclosed elements: a truncated file.
  if (stream->state != XML_S_TEXT || stream->ref_mask || stream->depth) return UINT32_MAX;
  uint64_t count = stream->count;
  if (stream->ambiguous && stream->flat > count) count = stream->flat;
  return count < UINT32_MAX ? (uint32_t)count : UINT32_MAX;
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
    {"pack data iface mismatch", 0, "Wrong game version (data loader)"},
    {"pack data iface unreadable", 0, "Wrong game version (data loader)"},
    {"pack data vtable mismatch", 0, "Wrong game version (data loader)"},
    {"pack data load mismatch", 0, "Wrong game version (data loader)"},
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
    {"pack data allocation failed", 0, "Out of memory; load fewer packs"},
    {"pack archive capacity gate", 0, "Game slots full; load fewer packs"},
    // Store-headroom refusals name their store (gtav_custom_pack_store_reason); a malformed one:
    {"store headroom gate refused", 0, "Game slots full; load fewer packs"},
    // Pack files on the console.
    {"pack archive size gate", 0, "Pack files damaged; reinstall it"},
    {"pack archive size/hash gate", 0, "Pack files damaged; reinstall it"},
    {"pack buffer hash gate", 0, "Pack files damaged; reinstall it"},
    {"pack buffer too small", 0, "Pack files damaged; reinstall it"},
    {"pack archive toc layout", 0, "Pack files damaged; reinstall it"},
    {"pack keyed toc count failed", 0, "Pack files damaged; reinstall it"},
    {"pack data file size/hash gate", 0, "Pack files damaged; reinstall it"},
    {"pack data read size", 0, "Pack files damaged; reinstall it"},
    {"pack archive open refused", 0, "Pack file missing; reinstall it"},
    {"pack archive bulk open refused", 0, "Game could not open archive"},
    {"pack archive path too long", 0, "Pack path too long; shorten it"},
    {"pack cache path too long", 0, "Pack path too long; shorten it"},
    {"pack data path too long", 0, "Pack path too long; shorten it"},
    {"pack registered path too long", 0, "Pack path too long; shorten it"},
    {"pack keyed archive copy failed", 0, "Could not prepare archive; retry"},
    {"pack keyed archive rename failed", 0, "Could not prepare archive; retry"},
    {"pack /data archive route gate", 0, "Wrong game version (file device)"},
    {"pack data /data device gate", 0, "Wrong game version (file device)"},
    {"pack archive header gate", 0, "Pack archive is not a valid RPF"},
    {"plain toc", 0, "Pack archive format not supported"},
    {"pack has no registered members", 0, "Pack archive has no usable files"},
    {"pack descriptor missing or invalid", 0, "A pack.cfg is invalid; reinstall"},
    {"pack active file missing or invalid", 0, "Pack selection invalid; reselect"},
    {"active packs overflow or collide", 0, "Selected packs clash or too big"},
    {"pack descriptors busy", 0, "Menu busy; press Load again"},
    // Data files (meta XML).
    {"pack data type has no pinned mounter", 0, "Data file type not supported"},
    {"pack data type is not supported on PS4", 0, "Data type not supported on PS4"},
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
    {"Carcols kits unsupported on PS4", 1, "Carcols kits unavailable on PS4"},
    {"apparel file needs one pedName", 0, "Clothing file lacks ped/dlc names"},
    {"fullDlcName must be", 0, "fullDlcName must be ped_dlcName"},
    {"apparel file already loaded", 0, "Clothing already added"},
    {"pmt is already on the ped", 0, "Clothing already added"},
    {"apparel pmt is not registered", 0, "Clothing archive not registered"},
    {"pedName is not a known model", 0, "Clothing ped model not found"},
    {"ped model is in use", 0, "Ped in use; restart and load first"},
    {"pack labels added=", 0, "Game refused some text labels"},
    {"carcols wheels are not supported", 0, "Custom wheels are not supported"},
    {"custom wheels need the PS4", 0, "Custom wheels unsupported on PS4"},
    {"pack has no labels", 1, "Pack has no labels"},
    // Stock overrides.
    {"pack overrides are not supported on PS4", 0, "Overrides not supported on PS4"},
    {"maps and typs are not supported on PS4", 0, "Maps and typs unsupported on PS4"},
    {"has no PS4 data loader", 0, "Data type not supported on PS4"},
    {"holds no PS4 resources", 0, "Not a PS4 pack (no .o* files)"},
    {"already in this game", 0, "Model already in game; restart GTA"},
    {"already registered in this game", 0, "Packs already loaded; restart GTA"},
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

// The store-headroom gates' stores: the technical name in refusals and pack-notes, the player's
// name and the selected packs that need it.
static const struct {
  const char* name;
  const char* label;  // "<label> slots full (...)"
  const char* text;   // the short menu text
  int scope;
} kStores[] = {
    {"frag", "Fragment", "Fragment slots full", GTAV_CUSTOM_PACK_STORE_ANY},
    {"drawable", "Drawable", "Drawable slots full", GTAV_CUSTOM_PACK_STORE_ANY},
    {"txd", "Texture", "Texture slots full", GTAV_CUSTOM_PACK_STORE_ANY},
    {"typ", "Archetype", "Archetype slots full", GTAV_CUSTOM_PACK_STORE_ANY},
    {"mapdata", "Map data", "Map data slots full", GTAV_CUSTOM_PACK_STORE_ANY},
    {"bounds", "Collision", "Collision slots full", GTAV_CUSTOM_PACK_STORE_ANY},
    {"metadata", "Ped metadata", "Ped metadata slots full", GTAV_CUSTOM_PACK_STORE_PED},
    {"dwd", "Ped drawable", "Ped drawable slots full", GTAV_CUSTOM_PACK_STORE_PED},
    {"ptfx", "Particle", "Particle slots full", GTAV_CUSTOM_PACK_STORE_PTFX},
    // Model and file tables the data rows add to (checked before Load, custom_pack_select.inc).
    {"vehicle", "Vehicle", "Vehicle slots full", GTAV_CUSTOM_PACK_STORE_VEHICLE},
    {"pedmodel", "Ped model", "Ped model slots full", GTAV_CUSTOM_PACK_STORE_PED},
    {"wmodel", "Weapon model", "Weapon model slots full", GTAV_CUSTOM_PACK_STORE_WEAPON},
    {"wcomp", "Component", "Component slots full", GTAV_CUSTOM_PACK_STORE_WEAPON},
    {"winfo", "Weapon file", "Weapon file slots full", GTAV_CUSTOM_PACK_STORE_WEAPON},
};
static const char kStorePrefix[] = "pack ";
static const char kStoreGate[] = " store headroom gate refused free=";
static const char kStoreNeed[] = " need=";

void gtav_custom_pack_store_refusal(char* out, size_t size, const char* store, uint32_t free_slots,
                                    uint32_t need) {
  if (!out || !size) return;
  snprintf(out, size, "%s%.8s%s%u%s%u", kStorePrefix, store ? store : "", kStoreGate,
           (unsigned)free_slots, kStoreNeed, (unsigned)need);
}

// Decimal u32 at *at (1..10 digits, no sign); advances *at. 0 when there is none or it overflows.
static int store_number(const char** at, uint32_t* value) {
  uint64_t n = 0;
  const char* p = *at;
  while (*p >= '0' && *p <= '9' && p - *at < 10) n = n * 10u + (uint64_t)(*p++ - '0');
  if (p == *at || n > UINT32_MAX || (*p >= '0' && *p <= '9')) return 0;
  *at = p;
  *value = (uint32_t)n;
  return 1;
}

const char* gtav_custom_pack_store_reason(const char* technical, char* detail, size_t size,
                                          int* scope) {
  if (!technical || strncmp(technical, kStorePrefix, sizeof(kStorePrefix) - 1u) != 0) return NULL;
  const char* name = technical + sizeof(kStorePrefix) - 1u;
  const char* gate = strstr(name, kStoreGate);
  if (!gate) return NULL;
  const size_t name_length = (size_t)(gate - name);
  for (size_t i = 0; i < sizeof(kStores) / sizeof(kStores[0]); ++i) {
    if (strlen(kStores[i].name) != name_length || strncmp(kStores[i].name, name, name_length) != 0)
      continue;
    const char* at = gate + sizeof(kStoreGate) - 1u;
    uint32_t free_slots = 0, need = 0;
    if (!store_number(&at, &free_slots) || strncmp(at, kStoreNeed, sizeof(kStoreNeed) - 1u) != 0)
      return NULL;
    at += sizeof(kStoreNeed) - 1u;
    if (!store_number(&at, &need) || *at != '\0') return NULL;
    if (detail && size)
      snprintf(detail, size, "%s slots full (%u free, need %u)", kStores[i].label,
               (unsigned)free_slots, (unsigned)need);
    if (scope) *scope = kStores[i].scope;
    return kStores[i].text;
  }
  return NULL;
}

const char* gtav_custom_pack_user_reason(const char* technical) {
  if (!technical) return "";
  const char* store = gtav_custom_pack_store_reason(technical, NULL, 0, NULL);
  if (store) return store;
  for (size_t i = 0; i < sizeof(kUserReasons) / sizeof(kUserReasons[0]); ++i) {
    const int hit = kUserReasons[i].exact ? strcmp(technical, kUserReasons[i].match) == 0
                                          : strstr(technical, kUserReasons[i].match) != NULL;
    if (hit) return kUserReasons[i].text;
  }
  return technical;
}

// Member extensions by GTAV_CUSTOM_PACK_MEMBER_* store (tools/gtavmenu_tools/rpf7.py mirrors it).
static const char* const kMemberExtensions[GTAV_CUSTOM_PACK_MEMBER_KINDS] = {
#if defined(__ORBIS__)
    "oft", "odr", "otd", "otyp", "omap", "obn", "omt", "odd", "opt",
#else
    "pft", "pdr", "ptd", "ptyp", "pmap", "pbn", "pmt", "pdd", "ppt",
#endif
};
// Free slots kept beyond a load's own adds: the game adds to these stores itself meanwhile.
static const uint32_t kMemberMargins[GTAV_CUSTOM_PACK_MEMBER_KINDS] = {16, 16, 16, 8, 8,
                                                                       8,  1,  16, 2};

int gtav_custom_pack_member_store(const char* name, size_t length) {
  if (!name) return -1;
  size_t dot = length;
  for (size_t i = length; i > 0; --i)
    if (name[i - 1] == '.') {
      dot = i - 1;
      break;
    }
  if (dot == length) return -1;
  const char* extension = name + dot + 1;
  const size_t extension_length = length - dot - 1;
  for (int kind = 0; kind < GTAV_CUSTOM_PACK_MEMBER_KINDS; ++kind) {
    const char* want = kMemberExtensions[kind];
    if (strlen(want) != extension_length) continue;
    size_t i = 0;
    while (i < extension_length &&
           (extension[i] == want[i] ||
            (extension[i] >= 'A' && extension[i] <= 'Z' && extension[i] - 'A' + 'a' == want[i])))
      ++i;
    if (i == extension_length) return kind;
  }
  return -1;
}

int gtav_custom_pack_toc_counts(const uint8_t* table, size_t bytes, uint32_t entries,
                                GtavCustomPackTocCounts* out) {
  if (!table || !out || entries < 2u || entries > bytes / 16u) return -1;
  const size_t pool_offset = (size_t)entries * 16u;
  const size_t pool_size = bytes - pool_offset;
  if (pool_size == 0) return -1;
  const uint8_t* pool = table + pool_offset;
  GtavCustomPackTocCounts counts;
  memset(&counts, 0, sizeof(counts));
  uint64_t children_end = 1;  // the earlier directories' children end here (ascending, disjoint)
  for (uint32_t i = 0; i < entries; ++i) {
    const uint8_t* entry = table + (size_t)i * 16u;
    uint64_t q = 0;
    for (int b = 7; b >= 0; --b) q = q << 8 | entry[b];
    const size_t name_offset = (size_t)(q & 0xffffu);
    if (name_offset >= pool_size) return -1;
    const uint8_t* name = pool + name_offset;
    const void* end = memchr(name, 0, pool_size - name_offset);
    if (!end) return -1;
    const int directory = (q >> 32) == 0x7fffff00u;
    if (i == 0 && !directory) return -1;
    if (directory) {
      const uint32_t first = (uint32_t)entry[8] | (uint32_t)entry[9] << 8 |
                             (uint32_t)entry[10] << 16 | (uint32_t)entry[11] << 24;
      const uint32_t children = (uint32_t)entry[12] | (uint32_t)entry[13] << 8 |
                                (uint32_t)entry[14] << 16 | (uint32_t)entry[15] << 24;
      if (children) {
        if (first <= i || first < children_end || (uint64_t)first + children > entries) return -1;
        children_end = (uint64_t)first + children;
      }
      continue;
    }
    counts.files++;
    const int kind =
        gtav_custom_pack_member_store((const char*)name, (size_t)((const uint8_t*)end - name));
    if (kind >= 0)
      counts.members[kind]++;
    else
      counts.other++;
  }
  *out = counts;
  return 0;
}

uint32_t gtav_custom_pack_store_need(int kind, uint32_t adds) {
  if (adds == 0 || kind < 0 || kind >= GTAV_CUSTOM_PACK_MEMBER_KINDS) return adds;
  const uint32_t margin = kMemberMargins[kind];
  return adds > UINT32_MAX - margin ? UINT32_MAX : adds + margin;
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
