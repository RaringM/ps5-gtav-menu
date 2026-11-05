#pragma once

/* Fill the render/input native table from the target's generated native-address header
 * (GTAV_NATIVE_ADDR_*, named by GTAV_MENU_NATIVE_ADDRESSES_HEADER) -- the same reviewed table the
 * gameplay features bind from. Used by targets without a loader-supplied table (the PS4 one-shot
 * worker). DRAW_SPRITE and the streamed-texture natives stay unbound until a target opts in with
 * GTAV_MENU_BIND_SPRITE_NATIVES=1 after reviewing their argument shapes. */

#include <string.h>

#include "gtavmenu/native_bridge.h"
#include GTAV_MENU_NATIVE_ADDRESSES_HEADER

#ifndef GTAV_MENU_BIND_SPRITE_NATIVES
#define GTAV_MENU_BIND_SPRITE_NATIVES 0
#endif

static inline void gtav_native_table_from_header(GtavNativeAddressTable* table) {
  memset(table, 0, sizeof(*table));
  table->abi_version = GTAV_NATIVE_BRIDGE_ABI_VERSION;
  table->get_frame_count = GTAV_NATIVE_ADDR_GET_FRAME_COUNT;
  table->get_game_timer = GTAV_NATIVE_ADDR_GET_GAME_TIMER;
  table->is_control_pressed = GTAV_NATIVE_ADDR_IS_CONTROL_PRESSED;
  table->is_control_just_pressed = GTAV_NATIVE_ADDR_IS_CONTROL_JUST_PRESSED;
  table->is_disabled_control_pressed = GTAV_NATIVE_ADDR_IS_DISABLED_CONTROL_PRESSED;
  table->is_disabled_control_just_pressed = GTAV_NATIVE_ADDR_IS_DISABLED_CONTROL_JUST_PRESSED;
  table->is_disabled_control_just_released = GTAV_NATIVE_ADDR_IS_DISABLED_CONTROL_JUST_RELEASED;
  table->disable_control_action = GTAV_NATIVE_ADDR_DISABLE_CONTROL_ACTION;
  table->set_input_exclusive = GTAV_NATIVE_ADDR_SET_INPUT_EXCLUSIVE;
  table->draw_rect = GTAV_NATIVE_ADDR_DRAW_RECT;
  table->begin_text_command_display_text = GTAV_NATIVE_ADDR_BEGIN_TEXT_COMMAND_DISPLAY_TEXT;
  table->add_text_component_substring_player_name =
      GTAV_NATIVE_ADDR_ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME;
  table->end_text_command_display_text = GTAV_NATIVE_ADDR_END_TEXT_COMMAND_DISPLAY_TEXT;
  table->set_text_scale = GTAV_NATIVE_ADDR_SET_TEXT_SCALE;
  table->set_text_colour = GTAV_NATIVE_ADDR_SET_TEXT_COLOUR;
  table->set_text_font = GTAV_NATIVE_ADDR_SET_TEXT_FONT;
  table->set_text_centre = GTAV_NATIVE_ADDR_SET_TEXT_CENTRE;
  table->set_text_wrap = GTAV_NATIVE_ADDR_SET_TEXT_WRAP;
  table->set_text_justification = GTAV_NATIVE_ADDR_SET_TEXT_JUSTIFICATION;
  table->set_text_drop_shadow = GTAV_NATIVE_ADDR_SET_TEXT_DROP_SHADOW;
  table->set_text_dropshadow = GTAV_NATIVE_ADDR_SET_TEXT_DROP_SHADOW;
  table->set_text_outline = GTAV_NATIVE_ADDR_SET_TEXT_OUTLINE;
#if GTAV_MENU_BIND_SPRITE_NATIVES
  table->draw_sprite = GTAV_NATIVE_ADDR_DRAW_SPRITE;
  table->has_streamed_texture_dict_loaded = GTAV_NATIVE_ADDR_HAS_STREAMED_TEXTURE_DICT_LOADED;
  table->request_streamed_texture_dict = GTAV_NATIVE_ADDR_REQUEST_STREAMED_TEXTURE_DICT;
#endif
  table->player_id = GTAV_NATIVE_ADDR_PLAYER_ID;
  table->player_ped_id = GTAV_NATIVE_ADDR_PLAYER_PED_ID;
  table->set_player_invincible = GTAV_NATIVE_ADDR_SET_PLAYER_INVINCIBLE;
  table->set_entity_invincible = GTAV_NATIVE_ADDR_SET_ENTITY_INVINCIBLE;
  table->set_entity_health = GTAV_NATIVE_ADDR_SET_ENTITY_HEALTH;
  table->set_ped_armour = GTAV_NATIVE_ADDR_SET_PED_ARMOUR;
  table->set_player_wanted_level = GTAV_NATIVE_ADDR_SET_PLAYER_WANTED_LEVEL;
  table->set_player_wanted_level_now = GTAV_NATIVE_ADDR_SET_PLAYER_WANTED_LEVEL_NOW;
}
