#include "gtavmenu/menu_draw_list.h"

#if GTAV_MENU_PHASE_DRAW_LIST
#include <stddef.h>
#include <string.h>

_Static_assert(__atomic_always_lock_free(8, 0), "draw-list publication requires lock-free qwords");
_Static_assert(sizeof(GtavMenuDrawCommand) == 164u, "draw command ABI changed");
_Static_assert(sizeof(GtavMenuDrawList) == 31504u, "draw list ABI changed");
_Static_assert(offsetof(GtavMenuDrawListState, slots) == 128u, "draw-list header ABI changed");
_Static_assert(sizeof(GtavMenuDrawListState) == 63136u, "draw-list state ABI changed");

#ifdef __cplusplus
#define GTAV_DRAW_LIST_ZERO_INIT \
  {                              \
  }
#else
#define GTAV_DRAW_LIST_ZERO_INIT {0}
#endif

GtavMenuDrawListState gtav_menu_draw_lists __attribute__((used, retain, visibility("default"))) = {
    .magic = GTAV_MENU_DRAW_LIST_MAGIC,
    .abi = 1,
    .size = sizeof(GtavMenuDrawListState),
    .published_token = 0,
    .readers = GTAV_DRAW_LIST_ZERO_INIT,
    .publications = 0,
    .acquisitions = 0,
    .releases = 0,
    .retries = 0,
    .misses = 0,
    .writer_busy = 0,
    .overflows = 0,
    .rejected_commands = 0,
    .last_published_generation = 0,
    .last_acquired_generation = 0,
    .slots = GTAV_DRAW_LIST_ZERO_INIT,
};

#undef GTAV_DRAW_LIST_ZERO_INIT

static uint32_t g_building_slot = UINT32_MAX;

static size_t bounded_string_length(const char* value, size_t capacity) {
  size_t length = 0;
  while (length < capacity && value[length]) ++length;
  return length;
}

static uint32_t published_index(uint64_t token) {
  const uint32_t encoded = (uint32_t)(token & 0xffu);
  return encoded >= 1u && encoded <= GTAV_MENU_DRAW_LIST_SLOTS ? encoded - 1u : UINT32_MAX;
}

static uint32_t list_index(const GtavMenuDrawList* list) {
  for (uint32_t i = 0; i < GTAV_MENU_DRAW_LIST_SLOTS; ++i) {
    if (list == &gtav_menu_draw_lists.slots[i]) return i;
  }
  return UINT32_MAX;
}

GtavMenuDrawList* gtav_menu_draw_list_begin(uint64_t generation) {
  const uint64_t token = __atomic_load_n(&gtav_menu_draw_lists.published_token, __ATOMIC_ACQUIRE);
  const uint32_t current = published_index(token);
  const uint32_t candidate = current == UINT32_MAX ? 0u : current ^ 1u;
  if (__atomic_load_n(&gtav_menu_draw_lists.readers[candidate], __ATOMIC_ACQUIRE) != 0) {
    __atomic_fetch_add(&gtav_menu_draw_lists.writer_busy, 1u, __ATOMIC_RELAXED);
    return NULL;
  }
  GtavMenuDrawList* list = &gtav_menu_draw_lists.slots[candidate];
  // The generation is part of the publication token, so it must never repeat while a stale
  // reader could still be validating an older token for the same slot (ABA avoidance). Honor a
  // newer caller generation, but advance internally when callers repeat or regress it.
  const uint64_t previous =
      __atomic_load_n(&gtav_menu_draw_lists.last_published_generation, __ATOMIC_RELAXED);
  list->generation = generation > previous ? generation : previous + 1u;
  if (!list->generation) list->generation = 1u;
  list->count = 0;
  list->flags = 0;
  g_building_slot = candidate;
  return list;
}

static GtavMenuDrawCommand* reserve_command(GtavMenuDrawList* list, uint32_t kind) {
  const uint32_t index = list_index(list);
  if (index == UINT32_MAX || index != g_building_slot) return NULL;
  if (list->count >= GTAV_MENU_DRAW_LIST_CAPACITY) {
    list->flags |= GTAV_MENU_DRAW_LIST_OVERFLOW;
    __atomic_fetch_add(&gtav_menu_draw_lists.rejected_commands, 1u, __ATOMIC_RELAXED);
    return NULL;
  }
  GtavMenuDrawCommand* command = &list->commands[list->count++];
  memset(command, 0, sizeof(*command));
  command->kind = kind;
  return command;
}

int gtav_menu_draw_list_add_rect(GtavMenuDrawList* list, float x, float y, float width,
                                 float height, int32_t r, int32_t g, int32_t b, int32_t a) {
  GtavMenuDrawCommand* command = reserve_command(list, GTAV_MENU_DRAW_RECT);
  if (!command) return -1;
  command->payload.rect = (GtavMenuDrawRect){x, y, width, height, r, g, b, a};
  return 0;
}

int gtav_menu_draw_list_add_text(GtavMenuDrawList* list, const char* text, float x, float y,
                                 float scale_x, float scale_y, int32_t font, int32_t r, int32_t g,
                                 int32_t b, int32_t a, int32_t centre, float wrap_start,
                                 float wrap_end, int32_t justification, int32_t drop_shadow,
                                 int32_t outline) {
  if (!text || !text[0]) return -1;
  GtavMenuDrawCommand* command = reserve_command(list, GTAV_MENU_DRAW_TEXT);
  if (!command) return -1;
  GtavMenuDrawText* out = &command->payload.text;
  out->x = x;
  out->y = y;
  out->scale_x = scale_x;
  out->scale_y = scale_y;
  out->wrap_start = wrap_start;
  out->wrap_end = wrap_end;
  out->font = font;
  out->r = r;
  out->g = g;
  out->b = b;
  out->a = a;
  out->centre = centre;
  out->justification = justification;
  out->drop_shadow = drop_shadow;
  out->outline = outline;
  size_t length = bounded_string_length(text, GTAV_MENU_DRAW_TEXT_CAPACITY);
  if (length >= GTAV_MENU_DRAW_TEXT_CAPACITY) {
    length = GTAV_MENU_DRAW_TEXT_CAPACITY - 1u;
    list->flags |= GTAV_MENU_DRAW_LIST_OVERFLOW;
    __atomic_fetch_add(&gtav_menu_draw_lists.rejected_commands, 1u, __ATOMIC_RELAXED);
  }
  memcpy(out->text, text, length);
  out->text[length] = '\0';
  return (list->flags & GTAV_MENU_DRAW_LIST_OVERFLOW) ? -1 : 0;
}

static int copy_owned_string(GtavMenuDrawList* list, char* out, size_t capacity,
                             const char* value) {
  if (!value || !value[0]) return -1;
  size_t length = bounded_string_length(value, capacity);
  if (length >= capacity) {
    length = capacity - 1u;
    list->flags |= GTAV_MENU_DRAW_LIST_OVERFLOW;
    __atomic_fetch_add(&gtav_menu_draw_lists.rejected_commands, 1u, __ATOMIC_RELAXED);
  }
  memcpy(out, value, length);
  out[length] = '\0';
  return (list->flags & GTAV_MENU_DRAW_LIST_OVERFLOW) ? -1 : 0;
}

int gtav_menu_draw_list_add_sprite(GtavMenuDrawList* list, const char* dict, const char* texture,
                                   float x, float y, float width, float height, float heading,
                                   int32_t r, int32_t g, int32_t b, int32_t a, int32_t p11,
                                   int32_t p12) {
  if (!dict || !dict[0] || !texture || !texture[0]) return -1;
  GtavMenuDrawCommand* command = reserve_command(list, GTAV_MENU_DRAW_SPRITE);
  if (!command) return -1;
  GtavMenuDrawSprite* out = &command->payload.sprite;
  out->x = x;
  out->y = y;
  out->width = width;
  out->height = height;
  out->heading = heading;
  out->r = r;
  out->g = g;
  out->b = b;
  out->a = a;
  out->p11 = p11;
  out->p12 = p12;
  int rc = copy_owned_string(list, out->dict, sizeof(out->dict), dict);
  rc |= copy_owned_string(list, out->texture, sizeof(out->texture), texture);
  return rc;
}

int gtav_menu_draw_list_publish(GtavMenuDrawList* list) {
  const uint32_t index = list_index(list);
  if (index == UINT32_MAX || index != g_building_slot) return -1;
  const int overflow = (list->flags & GTAV_MENU_DRAW_LIST_OVERFLOW) != 0;
  if (overflow) {
    list->count = 0;
    __atomic_fetch_add(&gtav_menu_draw_lists.overflows, 1u, __ATOMIC_RELAXED);
  }
  const uint64_t token = (list->generation << 8u) | (uint64_t)(index + 1u);
  __atomic_store_n(&gtav_menu_draw_lists.last_published_generation, list->generation,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&gtav_menu_draw_lists.published_token, token, __ATOMIC_RELEASE);
  __atomic_fetch_add(&gtav_menu_draw_lists.publications, 1u, __ATOMIC_RELAXED);
  g_building_slot = UINT32_MAX;
  return overflow ? -1 : 0;
}

const GtavMenuDrawList* gtav_menu_draw_list_acquire(void) {
  for (uint32_t attempt = 0; attempt < 2u; ++attempt) {
    const uint64_t token = __atomic_load_n(&gtav_menu_draw_lists.published_token, __ATOMIC_ACQUIRE);
    const uint32_t index = published_index(token);
    if (index == UINT32_MAX) break;
    __atomic_fetch_add(&gtav_menu_draw_lists.readers[index], 1u, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&gtav_menu_draw_lists.published_token, __ATOMIC_ACQUIRE) == token) {
      const GtavMenuDrawList* list = &gtav_menu_draw_lists.slots[index];
      if (!(list->flags & GTAV_MENU_DRAW_LIST_OVERFLOW)) {
        __atomic_fetch_add(&gtav_menu_draw_lists.acquisitions, 1u, __ATOMIC_RELAXED);
        __atomic_store_n(&gtav_menu_draw_lists.last_acquired_generation, list->generation,
                         __ATOMIC_RELAXED);
        return list;
      }
      __atomic_fetch_sub(&gtav_menu_draw_lists.readers[index], 1u, __ATOMIC_RELEASE);
      break;
    }
    __atomic_fetch_sub(&gtav_menu_draw_lists.readers[index], 1u, __ATOMIC_RELEASE);
    __atomic_fetch_add(&gtav_menu_draw_lists.retries, 1u, __ATOMIC_RELAXED);
  }
  __atomic_fetch_add(&gtav_menu_draw_lists.misses, 1u, __ATOMIC_RELAXED);
  return NULL;
}

void gtav_menu_draw_list_release(const GtavMenuDrawList* list) {
  const uint32_t index = list_index(list);
  if (index == UINT32_MAX) return;
  __atomic_fetch_sub(&gtav_menu_draw_lists.readers[index], 1u, __ATOMIC_RELEASE);
  __atomic_fetch_add(&gtav_menu_draw_lists.releases, 1u, __ATOMIC_RELAXED);
}

int gtav_menu_draw_list_before_generation_drained(uint64_t generation) {
  if (!generation) return 0;
  const uint64_t token = __atomic_load_n(&gtav_menu_draw_lists.published_token, __ATOMIC_ACQUIRE);
  if ((token >> 8u) < generation) return 0;
  for (uint32_t i = 0; i < GTAV_MENU_DRAW_LIST_SLOTS; ++i) {
    if (__atomic_load_n(&gtav_menu_draw_lists.readers[i], __ATOMIC_ACQUIRE) == 0) continue;
    const uint64_t slot_generation =
        __atomic_load_n(&gtav_menu_draw_lists.slots[i].generation, __ATOMIC_ACQUIRE);
    if (slot_generation < generation) return 0;
  }
  return 1;
}
#endif
