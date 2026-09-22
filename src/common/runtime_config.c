#include "gtavmenu/runtime_config.h"

#include "gtavmenu/detour.h"
#include "gtavmenu/hex.h"
#include "gtavmenu/ini_parse.h"
#include "gtavmenu/rootdir.h"
#include "gtavmenu/strutil.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef GTAV_MENU_DEFAULT_TARGET_ID
#define GTAV_MENU_DEFAULT_TARGET_ID "GTAV_PS5_SAFE_PROBE"
#endif

#ifndef GTAV_MENU_DEFAULT_LOG_PATH
#define GTAV_MENU_DEFAULT_LOG_PATH GTAV_MENU_DEFAULT_LOG
#endif

#ifndef GTAV_MENU_DEFAULT_INSTALL_HOOK
#define GTAV_MENU_DEFAULT_INSTALL_HOOK 0
#endif

#ifndef GTAV_MENU_DEFAULT_DRY_RUN
#define GTAV_MENU_DEFAULT_DRY_RUN 1
#endif

#ifndef GTAV_MENU_DEFAULT_GAME_BASE
#define GTAV_MENU_DEFAULT_GAME_BASE 0ull
#endif

#ifndef GTAV_MENU_DEFAULT_TEXT_START
#define GTAV_MENU_DEFAULT_TEXT_START 0ull
#endif

#ifndef GTAV_MENU_DEFAULT_TEXT_END
#define GTAV_MENU_DEFAULT_TEXT_END 0ull
#endif

#ifndef GTAV_MENU_DEFAULT_DATA_START
#define GTAV_MENU_DEFAULT_DATA_START 0ull
#endif

#ifndef GTAV_MENU_DEFAULT_DATA_END
#define GTAV_MENU_DEFAULT_DATA_END 0ull
#endif

#ifndef GTAV_MENU_DEFAULT_HOOK_ADDR
#define GTAV_MENU_DEFAULT_HOOK_ADDR 0ull
#endif

#ifndef GTAV_MENU_DEFAULT_HOOK_LENGTH
#define GTAV_MENU_DEFAULT_HOOK_LENGTH GTAV_MENU_DETOUR_JUMP_LEN
#endif

#ifndef GTAV_MENU_DEFAULT_EXPECTED_HEX
#define GTAV_MENU_DEFAULT_EXPECTED_HEX ""
#endif

#ifndef GTAV_MENU_DEFAULT_NATIVE_GET_FRAME_COUNT
#define GTAV_MENU_DEFAULT_NATIVE_GET_FRAME_COUNT 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_IS_CONTROL_PRESSED
#define GTAV_MENU_DEFAULT_NATIVE_IS_CONTROL_PRESSED 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_IS_CONTROL_JUST_PRESSED
#define GTAV_MENU_DEFAULT_NATIVE_IS_CONTROL_JUST_PRESSED 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_DRAW_RECT
#define GTAV_MENU_DEFAULT_NATIVE_DRAW_RECT 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_BEGIN_TEXT_COMMAND_DISPLAY_TEXT
#define GTAV_MENU_DEFAULT_NATIVE_BEGIN_TEXT_COMMAND_DISPLAY_TEXT 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME
#define GTAV_MENU_DEFAULT_NATIVE_ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_END_TEXT_COMMAND_DISPLAY_TEXT
#define GTAV_MENU_DEFAULT_NATIVE_END_TEXT_COMMAND_DISPLAY_TEXT 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_SCALE
#define GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_SCALE 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_COLOUR
#define GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_COLOUR 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_FONT
#define GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_FONT 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_CENTRE
#define GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_CENTRE 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_WRAP
#define GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_WRAP 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_JUSTIFICATION
#define GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_JUSTIFICATION 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_DROP_SHADOW
#define GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_DROP_SHADOW 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_DROPSHADOW
#define GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_DROPSHADOW 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_OUTLINE
#define GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_OUTLINE 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_IS_DISABLED_CONTROL_PRESSED
#define GTAV_MENU_DEFAULT_NATIVE_IS_DISABLED_CONTROL_PRESSED 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_IS_DISABLED_CONTROL_JUST_PRESSED
#define GTAV_MENU_DEFAULT_NATIVE_IS_DISABLED_CONTROL_JUST_PRESSED 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_IS_DISABLED_CONTROL_JUST_RELEASED
#define GTAV_MENU_DEFAULT_NATIVE_IS_DISABLED_CONTROL_JUST_RELEASED 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_DISABLE_CONTROL_ACTION
#define GTAV_MENU_DEFAULT_NATIVE_DISABLE_CONTROL_ACTION 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_SET_INPUT_EXCLUSIVE
#define GTAV_MENU_DEFAULT_NATIVE_SET_INPUT_EXCLUSIVE 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_GET_GAME_TIMER
#define GTAV_MENU_DEFAULT_NATIVE_GET_GAME_TIMER 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_DRAW_SPRITE
#define GTAV_MENU_DEFAULT_NATIVE_DRAW_SPRITE 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_HAS_STREAMED_TEXTURE_DICT_LOADED
#define GTAV_MENU_DEFAULT_NATIVE_HAS_STREAMED_TEXTURE_DICT_LOADED 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_REQUEST_STREAMED_TEXTURE_DICT
#define GTAV_MENU_DEFAULT_NATIVE_REQUEST_STREAMED_TEXTURE_DICT 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_PLAYER_ID
#define GTAV_MENU_DEFAULT_NATIVE_PLAYER_ID 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_PLAYER_PED_ID
#define GTAV_MENU_DEFAULT_NATIVE_PLAYER_PED_ID 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_SET_PLAYER_INVINCIBLE
#define GTAV_MENU_DEFAULT_NATIVE_SET_PLAYER_INVINCIBLE 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_SET_ENTITY_INVINCIBLE
#define GTAV_MENU_DEFAULT_NATIVE_SET_ENTITY_INVINCIBLE 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_SET_ENTITY_HEALTH
#define GTAV_MENU_DEFAULT_NATIVE_SET_ENTITY_HEALTH 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_SET_PED_ARMOUR
#define GTAV_MENU_DEFAULT_NATIVE_SET_PED_ARMOUR 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_SET_PLAYER_WANTED_LEVEL
#define GTAV_MENU_DEFAULT_NATIVE_SET_PLAYER_WANTED_LEVEL 0ull
#endif
#ifndef GTAV_MENU_DEFAULT_NATIVE_SET_PLAYER_WANTED_LEVEL_NOW
#define GTAV_MENU_DEFAULT_NATIVE_SET_PLAYER_WANTED_LEVEL_NOW 0ull
#endif

#ifndef GTAV_MENU_DEFAULT_NATIVE_CANARY
#define GTAV_MENU_DEFAULT_NATIVE_CANARY 0
#endif

#ifndef GTAV_MENU_DEFAULT_NATIVE_TIMER_CANARY
#define GTAV_MENU_DEFAULT_NATIVE_TIMER_CANARY 0
#endif

#ifndef GTAV_MENU_DEFAULT_NATIVE_CANARY_INTERVAL
#define GTAV_MENU_DEFAULT_NATIVE_CANARY_INTERVAL 1u
#endif

#ifndef GTAV_MENU_DEFAULT_NATIVE_CANARY_MAX_FRAMES
#define GTAV_MENU_DEFAULT_NATIVE_CANARY_MAX_FRAMES 300u
#endif

/* Default on-screen toast dwell time in worker ticks (~3s at the ~90Hz worker). */
#ifndef GTAV_MENU_DEFAULT_TOAST_TICKS
#define GTAV_MENU_DEFAULT_TOAST_TICKS 270u
#endif

// Single source of truth for the runtime.cfg native-address slots. Each X entry
// pairs the "native_<config_key>" key written to / read from runtime.cfg with the
// matching GtavNativeAddressTable field. The load and write paths below both
// expand this list, so a new native address is added in exactly one place.
#define GTAV_NATIVE_ADDRESS_LIST(X)                                                       \
  X("get_frame_count", get_frame_count)                                                   \
  X("is_control_pressed", is_control_pressed)                                             \
  X("is_control_just_pressed", is_control_just_pressed)                                   \
  X("draw_rect", draw_rect)                                                               \
  X("begin_text_command_display_text", begin_text_command_display_text)                   \
  X("add_text_component_substring_player_name", add_text_component_substring_player_name) \
  X("end_text_command_display_text", end_text_command_display_text)                       \
  X("set_text_scale", set_text_scale)                                                     \
  X("set_text_colour", set_text_colour)                                                   \
  X("set_text_font", set_text_font)                                                       \
  X("set_text_centre", set_text_centre)                                                   \
  X("set_text_wrap", set_text_wrap)                                                       \
  X("set_text_justification", set_text_justification)                                     \
  X("set_text_drop_shadow", set_text_drop_shadow)                                         \
  X("set_text_dropshadow", set_text_dropshadow)                                           \
  X("set_text_outline", set_text_outline)                                                 \
  X("is_disabled_control_pressed", is_disabled_control_pressed)                           \
  X("is_disabled_control_just_pressed", is_disabled_control_just_pressed)                 \
  X("is_disabled_control_just_released", is_disabled_control_just_released)               \
  X("disable_control_action", disable_control_action)                                     \
  X("set_input_exclusive", set_input_exclusive)                                           \
  X("get_game_timer", get_game_timer)                                                     \
  X("draw_sprite", draw_sprite)                                                           \
  X("has_streamed_texture_dict_loaded", has_streamed_texture_dict_loaded)                 \
  X("request_streamed_texture_dict", request_streamed_texture_dict)                       \
  X("player_id", player_id)                                                               \
  X("player_ped_id", player_ped_id)                                                       \
  X("set_player_invincible", set_player_invincible)                                       \
  X("set_entity_invincible", set_entity_invincible)                                       \
  X("set_entity_health", set_entity_health)                                               \
  X("set_ped_armour", set_ped_armour)                                                     \
  X("set_player_wanted_level", set_player_wanted_level)                                   \
  X("set_player_wanted_level_now", set_player_wanted_level_now)

static int parse_native_address_key(GtavNativeAddressTable* table, const char* key,
                                    uint64_t value) {
  if (!table || !key) return 0;

#define GTAV_PARSE_NATIVE_ADDRESS(config_key, field_name) \
  if (!strcmp(key, "native_" config_key)) {               \
    table->field_name = value;                            \
    return 1;                                             \
  }

  GTAV_NATIVE_ADDRESS_LIST(GTAV_PARSE_NATIVE_ADDRESS)

#undef GTAV_PARSE_NATIVE_ADDRESS
  return 0;
}

static void set_native_canary_enabled(GtavMenuInit* init, int enabled) {
  if (!init) return;
  if (enabled) {
    init->flags |= GTAV_MENU_FLAG_NATIVE_DRAW_CANARY;
    init->native_canary_flags |= GTAV_NATIVE_CANARY_DRAW_RECT;
  } else {
    init->flags &= ~GTAV_MENU_FLAG_NATIVE_DRAW_CANARY;
    init->native_canary_flags &= ~GTAV_NATIVE_CANARY_DRAW_RECT;
  }
}

static void set_native_timer_canary_enabled(GtavMenuInit* init, int enabled) {
  if (!init) return;
  if (enabled) {
    init->native_canary_flags |= GTAV_NATIVE_CANARY_GET_GAME_TIMER;
  } else {
    init->native_canary_flags &= ~GTAV_NATIVE_CANARY_GET_GAME_TIMER;
  }
}

void gtav_runtime_init_defaults(GtavMenuInit* init) {
  size_t parsed = 0;

  if (!init) return;
  memset(init, 0, sizeof(*init));
  init->abi_version = GTAV_MENU_ABI_VERSION;
  if (GTAV_MENU_DEFAULT_INSTALL_HOOK) init->flags |= GTAV_MENU_FLAG_ALLOW_HOOK;
  if (GTAV_MENU_DEFAULT_DRY_RUN) init->flags |= GTAV_MENU_FLAG_DRY_RUN;
  init->game_base = GTAV_MENU_DEFAULT_GAME_BASE;
  init->text_start = GTAV_MENU_DEFAULT_TEXT_START;
  init->text_end = GTAV_MENU_DEFAULT_TEXT_END;
  init->data_start = GTAV_MENU_DEFAULT_DATA_START;
  init->data_end = GTAV_MENU_DEFAULT_DATA_END;
  init->hook_addr = GTAV_MENU_DEFAULT_HOOK_ADDR;
  init->hook_length = GTAV_MENU_DEFAULT_HOOK_LENGTH;
  init->native_table.abi_version = GTAV_NATIVE_BRIDGE_ABI_VERSION;
  init->native_table.get_frame_count = GTAV_MENU_DEFAULT_NATIVE_GET_FRAME_COUNT;
  init->native_table.is_control_pressed = GTAV_MENU_DEFAULT_NATIVE_IS_CONTROL_PRESSED;
  init->native_table.is_control_just_pressed = GTAV_MENU_DEFAULT_NATIVE_IS_CONTROL_JUST_PRESSED;
  init->native_table.draw_rect = GTAV_MENU_DEFAULT_NATIVE_DRAW_RECT;
  init->native_table.begin_text_command_display_text =
      GTAV_MENU_DEFAULT_NATIVE_BEGIN_TEXT_COMMAND_DISPLAY_TEXT;
  init->native_table.add_text_component_substring_player_name =
      GTAV_MENU_DEFAULT_NATIVE_ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME;
  init->native_table.end_text_command_display_text =
      GTAV_MENU_DEFAULT_NATIVE_END_TEXT_COMMAND_DISPLAY_TEXT;
  init->native_table.set_text_scale = GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_SCALE;
  init->native_table.set_text_colour = GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_COLOUR;
  init->native_table.set_text_font = GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_FONT;
  init->native_table.set_text_centre = GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_CENTRE;
  init->native_table.set_text_wrap = GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_WRAP;
  init->native_table.set_text_justification = GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_JUSTIFICATION;
  init->native_table.set_text_drop_shadow = GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_DROP_SHADOW;
  init->native_table.set_text_dropshadow = GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_DROPSHADOW;
  init->native_table.set_text_outline = GTAV_MENU_DEFAULT_NATIVE_SET_TEXT_OUTLINE;
  init->native_table.is_disabled_control_pressed =
      GTAV_MENU_DEFAULT_NATIVE_IS_DISABLED_CONTROL_PRESSED;
  init->native_table.is_disabled_control_just_pressed =
      GTAV_MENU_DEFAULT_NATIVE_IS_DISABLED_CONTROL_JUST_PRESSED;
  init->native_table.is_disabled_control_just_released =
      GTAV_MENU_DEFAULT_NATIVE_IS_DISABLED_CONTROL_JUST_RELEASED;
  init->native_table.disable_control_action = GTAV_MENU_DEFAULT_NATIVE_DISABLE_CONTROL_ACTION;
  init->native_table.set_input_exclusive = GTAV_MENU_DEFAULT_NATIVE_SET_INPUT_EXCLUSIVE;
  init->native_table.get_game_timer = GTAV_MENU_DEFAULT_NATIVE_GET_GAME_TIMER;
  init->native_table.draw_sprite = GTAV_MENU_DEFAULT_NATIVE_DRAW_SPRITE;
  init->native_table.has_streamed_texture_dict_loaded =
      GTAV_MENU_DEFAULT_NATIVE_HAS_STREAMED_TEXTURE_DICT_LOADED;
  init->native_table.request_streamed_texture_dict =
      GTAV_MENU_DEFAULT_NATIVE_REQUEST_STREAMED_TEXTURE_DICT;
  init->native_table.player_id = GTAV_MENU_DEFAULT_NATIVE_PLAYER_ID;
  init->native_table.player_ped_id = GTAV_MENU_DEFAULT_NATIVE_PLAYER_PED_ID;
  init->native_table.set_player_invincible = GTAV_MENU_DEFAULT_NATIVE_SET_PLAYER_INVINCIBLE;
  init->native_table.set_entity_invincible = GTAV_MENU_DEFAULT_NATIVE_SET_ENTITY_INVINCIBLE;
  init->native_table.set_entity_health = GTAV_MENU_DEFAULT_NATIVE_SET_ENTITY_HEALTH;
  init->native_table.set_ped_armour = GTAV_MENU_DEFAULT_NATIVE_SET_PED_ARMOUR;
  init->native_table.set_player_wanted_level = GTAV_MENU_DEFAULT_NATIVE_SET_PLAYER_WANTED_LEVEL;
  init->native_table.set_player_wanted_level_now =
      GTAV_MENU_DEFAULT_NATIVE_SET_PLAYER_WANTED_LEVEL_NOW;
  init->native_canary_interval = GTAV_MENU_DEFAULT_NATIVE_CANARY_INTERVAL;
  init->native_canary_max_frames = GTAV_MENU_DEFAULT_NATIVE_CANARY_MAX_FRAMES;
  init->toast_ticks = GTAV_MENU_DEFAULT_TOAST_TICKS;
  set_native_canary_enabled(init, GTAV_MENU_DEFAULT_NATIVE_CANARY);
  set_native_timer_canary_enabled(init, GTAV_MENU_DEFAULT_NATIVE_TIMER_CANARY);
  gtav_copy_string(init->target_id, sizeof(init->target_id), GTAV_MENU_DEFAULT_TARGET_ID);
  gtav_copy_string(init->log_path, sizeof(init->log_path), GTAV_MENU_DEFAULT_LOG_PATH);
  if (GTAV_MENU_DEFAULT_EXPECTED_HEX[0] &&
      !gtav_hex_parse_bytes(GTAV_MENU_DEFAULT_EXPECTED_HEX, init->expected, sizeof(init->expected),
                            &parsed)) {
    init->expected_len = (uint32_t)parsed;
  }
}

int gtav_runtime_config_load(const char* path, GtavMenuInit* init) {
  char line[512];
  GtavRootdirGuard rootdir;
  FILE* fp;
  int rooted;

  if (!init) return -1;
  gtav_runtime_init_defaults(init);

  rooted = gtav_rootdir_enter(&rootdir) == 0;
  fp = fopen(path ? path : GTAV_MENU_DEFAULT_CONFIG, "r");
  if (rooted) {
    gtav_rootdir_leave(&rootdir);
  }
  if (!fp) return -1;

  while (fgets(line, sizeof(line), fp)) {
    char* key;
    char* value;
    if (gtav_ini_split(line, &key, &value) != 0) continue;

    if (!strcmp(key, "target_id")) {
      gtav_copy_string(init->target_id, sizeof(init->target_id), value);
    } else if (!strcmp(key, "log_path")) {
      gtav_copy_string(init->log_path, sizeof(init->log_path), value);
    } else if (!strcmp(key, "flags")) {
      init->flags = (uint32_t)gtav_ini_parse_u64(value);
    } else if (!strcmp(key, "install_hook")) {
      if (gtav_ini_truthy(value))
        init->flags |= GTAV_MENU_FLAG_ALLOW_HOOK;
      else
        init->flags &= ~GTAV_MENU_FLAG_ALLOW_HOOK;
    } else if (!strcmp(key, "dry_run")) {
      if (gtav_ini_truthy(value))
        init->flags |= GTAV_MENU_FLAG_DRY_RUN;
      else
        init->flags &= ~GTAV_MENU_FLAG_DRY_RUN;
    } else if (!strcmp(key, "game_base")) {
      init->game_base = gtav_ini_parse_u64(value);
    } else if (!strcmp(key, "text_start")) {
      init->text_start = gtav_ini_parse_u64(value);
    } else if (!strcmp(key, "text_end")) {
      init->text_end = gtav_ini_parse_u64(value);
    } else if (!strcmp(key, "data_start")) {
      init->data_start = gtav_ini_parse_u64(value);
    } else if (!strcmp(key, "data_end")) {
      init->data_end = gtav_ini_parse_u64(value);
    } else if (!strcmp(key, "hook_addr")) {
      init->hook_addr = gtav_ini_parse_u64(value);
    } else if (!strcmp(key, "hook_length")) {
      init->hook_length = (uint32_t)gtav_ini_parse_u64(value);
    } else if (!strcmp(key, "expected")) {
      size_t parsed = 0;
      if (!gtav_hex_parse_bytes(value, init->expected, sizeof(init->expected), &parsed)) {
        init->expected_len = (uint32_t)parsed;
      }
    } else if (!strcmp(key, "native_canary") || !strcmp(key, "native_draw_canary")) {
      set_native_canary_enabled(init, gtav_ini_truthy(value));
    } else if (!strcmp(key, "native_timer_canary") ||
               !strcmp(key, "native_get_game_timer_canary")) {
      set_native_timer_canary_enabled(init, gtav_ini_truthy(value));
    } else if (!strcmp(key, "native_canary_flags")) {
      init->native_canary_flags = (uint32_t)gtav_ini_parse_u64(value);
    } else if (!strcmp(key, "native_canary_interval")) {
      init->native_canary_interval = (uint32_t)gtav_ini_parse_u64(value);
    } else if (!strcmp(key, "native_canary_max_frames")) {
      init->native_canary_max_frames = (uint32_t)gtav_ini_parse_u64(value);
    } else if (!strcmp(key, "toast_ticks")) {
      init->toast_ticks = (uint32_t)gtav_ini_parse_u64(value);
    } else {
      parse_native_address_key(&init->native_table, key, gtav_ini_parse_u64(value));
    }
  }

  fclose(fp);
  return 0;
}

int gtav_runtime_config_write(const char* path, const GtavMenuInit* init) {
  GtavRootdirGuard rootdir;
  FILE* fp;
  char expected[(GTAV_MENU_MAX_EXPECTED_BYTES * 3) + 1];
  int rooted;

  if (!init) return -1;

  rooted = gtav_rootdir_enter(&rootdir) == 0;
  fp = fopen(path ? path : GTAV_MENU_DEFAULT_CONFIG, "w");
  if (rooted) {
    gtav_rootdir_leave(&rootdir);
  }
  if (!fp) return -1;

  gtav_hex_format(init->expected, init->expected_len, expected, sizeof(expected));

  fprintf(fp, "target_id=%s\n", init->target_id);
  fprintf(fp, "log_path=%s\n", init->log_path);
  fprintf(fp, "flags=0x%08X\n", init->flags);
  fprintf(fp, "install_hook=%u\n", !!(init->flags & GTAV_MENU_FLAG_ALLOW_HOOK));
  fprintf(fp, "dry_run=%u\n", !!(init->flags & GTAV_MENU_FLAG_DRY_RUN));
  fprintf(fp, "game_base=0x%llX\n", (unsigned long long)init->game_base);
  fprintf(fp, "text_start=0x%llX\n", (unsigned long long)init->text_start);
  fprintf(fp, "text_end=0x%llX\n", (unsigned long long)init->text_end);
  fprintf(fp, "data_start=0x%llX\n", (unsigned long long)init->data_start);
  fprintf(fp, "data_end=0x%llX\n", (unsigned long long)init->data_end);
  fprintf(fp, "hook_addr=0x%llX\n", (unsigned long long)init->hook_addr);
  fprintf(fp, "hook_length=%u\n", init->hook_length);
  fprintf(fp, "expected=%s\n", expected);
  fprintf(fp, "native_canary=%u\n", !!(init->flags & GTAV_MENU_FLAG_NATIVE_DRAW_CANARY));
  fprintf(fp, "native_timer_canary=%u\n",
          !!(init->native_canary_flags & GTAV_NATIVE_CANARY_GET_GAME_TIMER));
  fprintf(fp, "native_canary_flags=0x%08X\n", init->native_canary_flags);
  fprintf(fp, "native_canary_interval=%u\n", init->native_canary_interval);
  fprintf(fp, "native_canary_max_frames=%u\n", init->native_canary_max_frames);
  fprintf(fp, "toast_ticks=%u\n", init->toast_ticks);

#define GTAV_WRITE_NATIVE_ADDRESS(config_key, field_name) \
  fprintf(fp, "native_" config_key "=0x%llX\n", (unsigned long long)init->native_table.field_name);

  GTAV_NATIVE_ADDRESS_LIST(GTAV_WRITE_NATIVE_ADDRESS)

#undef GTAV_WRITE_NATIVE_ADDRESS

  fclose(fp);
  return 0;
}
