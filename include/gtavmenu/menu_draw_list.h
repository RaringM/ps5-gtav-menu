#pragma once

#include <stdint.h>

#ifndef GTAV_MENU_PHASE_DRAW_LIST
#define GTAV_MENU_PHASE_DRAW_LIST 0
#endif

#if GTAV_MENU_PHASE_DRAW_LIST
#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_MENU_DRAW_LIST_MAGIC 0x315453494c444d47ull
#define GTAV_MENU_DRAW_LIST_SLOTS 2u
#define GTAV_MENU_DRAW_LIST_CAPACITY 192u
#define GTAV_MENU_DRAW_TEXT_CAPACITY 96u
#define GTAV_MENU_DRAW_SPRITE_DICT_CAPACITY 32u
#define GTAV_MENU_DRAW_SPRITE_TEXTURE_CAPACITY 48u

enum {
  GTAV_MENU_DRAW_NONE = 0,
  GTAV_MENU_DRAW_RECT = 1,
  GTAV_MENU_DRAW_TEXT = 2,
  GTAV_MENU_DRAW_SPRITE = 3,
};

enum {
  GTAV_MENU_DRAW_LIST_OVERFLOW = 1u,
};

typedef struct GtavMenuDrawRect {
  float x, y, width, height;
  int32_t r, g, b, a;
} GtavMenuDrawRect;

typedef struct GtavMenuDrawText {
  float x, y, scale_x, scale_y, wrap_start, wrap_end;
  int32_t font, r, g, b, a;
  int32_t centre, justification, drop_shadow, outline;
  char text[GTAV_MENU_DRAW_TEXT_CAPACITY];
} GtavMenuDrawText;

typedef struct GtavMenuDrawSprite {
  float x, y, width, height, heading;
  int32_t r, g, b, a, p11, p12;
  char dict[GTAV_MENU_DRAW_SPRITE_DICT_CAPACITY];
  char texture[GTAV_MENU_DRAW_SPRITE_TEXTURE_CAPACITY];
} GtavMenuDrawSprite;

typedef struct GtavMenuDrawCommand {
  uint32_t kind;
  uint32_t reserved;
  union {
    GtavMenuDrawRect rect;
    GtavMenuDrawText text;
    GtavMenuDrawSprite sprite;
  } payload;
} GtavMenuDrawCommand;

typedef struct GtavMenuDrawList {
  uint64_t generation;
  uint32_t count;
  uint32_t flags;
  GtavMenuDrawCommand commands[GTAV_MENU_DRAW_LIST_CAPACITY];
} GtavMenuDrawList;

typedef struct GtavMenuDrawListState {
  uint64_t magic, abi, size;
  uint64_t published_token;
  uint64_t readers[GTAV_MENU_DRAW_LIST_SLOTS];
  uint64_t publications, acquisitions, releases, retries;
  uint64_t misses, writer_busy, overflows, rejected_commands;
  uint64_t last_published_generation, last_acquired_generation;
  GtavMenuDrawList slots[GTAV_MENU_DRAW_LIST_SLOTS];
} GtavMenuDrawListState;

extern GtavMenuDrawListState gtav_menu_draw_lists;

// Single-worker builder. Returns NULL instead of waiting when the inactive slot is still owned by
// the phase reader. A failed/overflowed list is published empty so stale menu contents cannot
// remain visible.
GtavMenuDrawList* gtav_menu_draw_list_begin(uint64_t generation);
int gtav_menu_draw_list_add_rect(GtavMenuDrawList* list, float x, float y, float width,
                                 float height, int32_t r, int32_t g, int32_t b, int32_t a);
int gtav_menu_draw_list_add_text(GtavMenuDrawList* list, const char* text, float x, float y,
                                 float scale_x, float scale_y, int32_t font, int32_t r, int32_t g,
                                 int32_t b, int32_t a, int32_t centre, float wrap_start,
                                 float wrap_end, int32_t justification, int32_t drop_shadow,
                                 int32_t outline);
int gtav_menu_draw_list_add_sprite(GtavMenuDrawList* list, const char* dict, const char* texture,
                                   float x, float y, float width, float height, float heading,
                                   int32_t r, int32_t g, int32_t b, int32_t a, int32_t p11,
                                   int32_t p12);
int gtav_menu_draw_list_publish(GtavMenuDrawList* list);

// Phase-side bounded claim. The caller must release a non-NULL list exactly once. Acquisition
// verifies the publication token only after claiming the slot, so a writer may never overwrite a
// list while native submission is reading it.
const GtavMenuDrawList* gtav_menu_draw_list_acquire(void);
void gtav_menu_draw_list_release(const GtavMenuDrawList* list);

// True only after `generation` or a newer list has replaced every earlier publication and no
// phase reader still owns an older slot. Resource managers use this nonblocking fence before
// releasing texture dictionaries referenced by superseded sprite commands.
int gtav_menu_draw_list_before_generation_drained(uint64_t generation);

#ifdef __cplusplus
}
#endif
#endif
