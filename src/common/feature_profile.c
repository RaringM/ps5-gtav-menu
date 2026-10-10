#include "gtavmenu/feature_profile.h"

#include "gtavmenu/ini_parse.h"
#include "gtavmenu/rootdir.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Write a fixed-capacity hash list as `<prefix><i>=0x...` lines (parallel-array idiom,
 * like the keybind slots). Empty (0) slots are written too so the array round-trips by
 * index. */
static void save_hash_array(FILE* fp, const char* prefix, const uint32_t* arr, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    fprintf(fp, "%s%u=0x%08X\n", prefix, i, arr[i]);
  }
}

/* Match a `<prefix><i>` key and store its value into arr[i]. Returns 1 if the key matched
 * this array (so the caller can stop), 0 otherwise. */
static int load_hash_array(const char* key, uint32_t value, const char* prefix, uint32_t* arr,
                           uint32_t n) {
  char name[32];
  for (uint32_t i = 0; i < n; ++i) {
    snprintf(name, sizeof(name), "%s%u", prefix, i);
    if (!strcmp(key, name)) {
      arr[i] = value;
      return 1;
    }
  }
  return 0;
}

int gtav_feature_profile_save(const char* path, const GtavFeatureProfile* profile) {
  GtavRootdirGuard rootdir;
  FILE* fp;
  int rooted;

  if (!profile) return -1;

  // Loader processes can enter the console root; injected workers use the mounted path already
  // present in the game sandbox. Always leave the guard before touching the FILE.
  rooted = gtav_rootdir_enter(&rootdir) == 0;
  fp = fopen(path ? path : GTAV_FEATURE_PROFILE_DEFAULT_PATH, "w");
  if (rooted) {
    gtav_rootdir_leave(&rootdir);
  }
  if (!fp) return -1;

  fprintf(fp, "version=%u\n", profile->version);
  fprintf(fp, "toggle_mask=0x%016llX\n", (unsigned long long)profile->toggle_mask);
  fprintf(fp, "weather_index=%u\n", profile->weather_index);
  fprintf(fp, "time_index=%u\n", profile->time_index);
  fprintf(fp, "wanted_index=%u\n", profile->wanted_index);
  fprintf(fp, "nitro_power_index=%u\n", profile->nitro_power_index);
  fprintf(fp, "timescale_index=%u\n", profile->timescale_index);
  fprintf(fp, "gravity_index=%u\n", profile->gravity_index);
  fprintf(fp, "clock_hour=%u\n", profile->clock_hour);
  fprintf(fp, "move_rate_index=%u\n", profile->move_rate_index);
  fprintf(fp, "theme_index=%u\n", profile->theme_index);
  fprintf(fp, "region_index=%u\n", profile->region_index);
  for (uint32_t i = 0; i < GTAV_KEYBIND_SLOTS; ++i) {
    fprintf(fp, "keybind%u_mask=0x%08X\n", i, profile->keybind_mask[i]);
    fprintf(fp, "keybind%u_action=%u\n", i, profile->keybind_action[i]);
  }
  fprintf(fp, "bodyguard_health_index=%u\n", profile->bodyguard_health_index);
  fprintf(fp, "bodyguard_armor_index=%u\n", profile->bodyguard_armor_index);
  fprintf(fp, "bodyguard_formation_index=%u\n", profile->bodyguard_formation_index);
  fprintf(fp, "bodyguard_aggression_index=%u\n", profile->bodyguard_aggression_index);
  fprintf(fp, "bodyguard_weapon_index=%u\n", profile->bodyguard_weapon_index);
  fprintf(fp, "bodyguard_accuracy_index=%u\n", profile->bodyguard_accuracy_index);
  fprintf(fp, "cruise_speed_index=%u\n", profile->cruise_speed_index);
  save_hash_array(fp, "favorite_vehicle", profile->favorite_vehicles, GTAV_FAVORITE_VEHICLES_MAX);
  save_hash_array(fp, "favorite_ped", profile->favorite_peds, GTAV_FAVORITE_PEDS_MAX);
  save_hash_array(fp, "recent_vehicle", profile->recent_vehicles, GTAV_RECENT_VEHICLES_MAX);
  save_hash_array(fp, "recent_ped", profile->recent_peds, GTAV_RECENT_PEDS_MAX);
  save_hash_array(fp, "wardrobe_drawable", profile->wardrobe_drawable, GTAV_WARDROBE_SLOTS);
  save_hash_array(fp, "wardrobe_texture", profile->wardrobe_texture, GTAV_WARDROBE_SLOTS);
  /* Saved vehicles/outfits are multi-slot (v16): the 2D mod/outfit arrays decay to a contiguous
   * flat block, written as <prefix><flat-index>. Loaded back the same way (slot = index / PER). */
  save_hash_array(fp, "saved_vehicle_model", profile->saved_vehicle_model,
                  GTAV_SAVED_VEHICLE_SLOTS);
  save_hash_array(fp, "saved_vehicle_mod", &profile->saved_vehicle_mods[0][0],
                  GTAV_SAVED_VEHICLE_SLOTS * GTAV_SAVED_VEHICLE_MOD_SLOTS);
  save_hash_array(fp, "saved_outfit_drawable", &profile->saved_outfit_drawable[0][0],
                  GTAV_SAVED_OUTFIT_SLOTS * GTAV_WARDROBE_SLOTS);
  save_hash_array(fp, "saved_outfit_texture", &profile->saved_outfit_texture[0][0],
                  GTAV_SAVED_OUTFIT_SLOTS * GTAV_WARDROBE_SLOTS);
  save_hash_array(fp, "quick_action", profile->quick_action, GTAV_QUICK_PINS_MAX);
  save_hash_array(fp, "quick_param", profile->quick_param, GTAV_QUICK_PINS_MAX);
  fprintf(fp, "worker_hz_index=%u\n", profile->worker_hz_index);
  fprintf(fp, "toast_time_index=%u\n", profile->toast_time_index);
  save_hash_array(fp, "custom_theme", profile->custom_theme, GTAV_PROFILE_CUSTOM_THEME_CHANNELS);
  fprintf(fp, "reduce_motion=%u\n", profile->reduce_motion);
  fprintf(fp, "nav_delay_index=%u\n", profile->nav_delay_index);
  fprintf(fp, "nav_speed_index=%u\n", profile->nav_speed_index);
  fprintf(fp, "touchpad_enabled=%u\n", profile->touchpad_enabled);
  fprintf(fp, "panel_width_index=%u\n", profile->panel_width_index);
  fprintf(fp, "spawn_preserve_speed=%u\n", profile->spawn_preserve_speed);
  fprintf(fp, "spawn_replace_previous=%u\n", profile->spawn_replace_previous);
  fprintf(fp, "spawn_aircraft_in_flight=%u\n", profile->spawn_aircraft_in_flight);
  fprintf(fp, "population_density_index=%u\n", profile->population_density_index);
  fprintf(fp, "speedometer_layout=%u\n", profile->speedometer_layout);
  fprintf(fp, "language_id=%u\n", profile->language_id);

  fclose(fp);
  return 0;
}

int gtav_feature_profile_load(const char* path, GtavFeatureProfile* profile) {
  char line[256];
  GtavRootdirGuard rootdir;
  FILE* fp;
  int rooted;

  if (!profile) return -1;
  memset(profile, 0, sizeof(*profile));
  profile->version = GTAV_FEATURE_PROFILE_VERSION;
  profile->spawn_preserve_speed = 1;
  profile->spawn_replace_previous = 1;
  profile->spawn_aircraft_in_flight = 1;
  profile->population_density_index = 1;

  rooted = gtav_rootdir_enter(&rootdir) == 0;
  fp = fopen(path ? path : GTAV_FEATURE_PROFILE_DEFAULT_PATH, "r");
  if (rooted) {
    gtav_rootdir_leave(&rootdir);
  }
  if (!fp) return -1;

  while (fgets(line, sizeof(line), fp)) {
    char* key;
    char* text;
    if (gtav_ini_split(line, &key, &text) != 0) continue;
    uint32_t value = gtav_ini_parse_u32(text);

    if (!strcmp(key, "version")) {
      profile->version = value;
    } else if (!strcmp(key, "toggle_mask")) {
      profile->toggle_mask = gtav_ini_parse_u64(text);
    } else if (!strcmp(key, "weather_index")) {
      profile->weather_index = value;
    } else if (!strcmp(key, "time_index")) {
      profile->time_index = value;
    } else if (!strcmp(key, "wanted_index")) {
      profile->wanted_index = value;
    } else if (!strcmp(key, "nitro_power_index")) {
      profile->nitro_power_index = value;
    } else if (!strcmp(key, "timescale_index")) {
      profile->timescale_index = value;
    } else if (!strcmp(key, "gravity_index")) {
      profile->gravity_index = value;
    } else if (!strcmp(key, "clock_hour")) {
      profile->clock_hour = value;
    } else if (!strcmp(key, "move_rate_index")) {
      profile->move_rate_index = value;
    } else if (!strcmp(key, "theme_index")) {
      profile->theme_index = value;
    } else if (!strcmp(key, "region_index")) {
      profile->region_index = value;
    } else if (!strcmp(key, "bodyguard_health_index")) {
      profile->bodyguard_health_index = value;
    } else if (!strcmp(key, "bodyguard_armor_index")) {
      profile->bodyguard_armor_index = value;
    } else if (!strcmp(key, "bodyguard_formation_index")) {
      profile->bodyguard_formation_index = value;
    } else if (!strcmp(key, "bodyguard_aggression_index")) {
      profile->bodyguard_aggression_index = value;
    } else if (!strcmp(key, "bodyguard_weapon_index")) {
      profile->bodyguard_weapon_index = value;
    } else if (!strcmp(key, "bodyguard_accuracy_index")) {
      profile->bodyguard_accuracy_index = value;
    } else if (!strcmp(key, "cruise_speed_index")) {
      profile->cruise_speed_index = value;
    } else if (!strcmp(key, "saved_vehicle_model")) {
      /* Pre-v16 migration: the bare scalar key (no slot digit) loads into garage slot 0. New files
       * write saved_vehicle_model0..9 (handled by the load_hash_array chain below). */
      profile->saved_vehicle_model[0] = value;
    } else if (!strcmp(key, "spawn_preserve_speed")) {
      profile->spawn_preserve_speed = value;
    } else if (!strcmp(key, "spawn_replace_previous")) {
      profile->spawn_replace_previous = value;
    } else if (!strcmp(key, "spawn_aircraft_in_flight")) {
      profile->spawn_aircraft_in_flight = value;
    } else if (!strcmp(key, "population_density_index")) {
      profile->population_density_index = value;
    } else if (!strcmp(key, "speedometer_layout")) {
      profile->speedometer_layout = value;
    } else if (!strcmp(key, "language_id")) {
      profile->language_id = value;
    } else if (!strcmp(key, "panel_width_index")) {
      profile->panel_width_index = value;
    } else if (!strcmp(key, "worker_hz_index")) {
      profile->worker_hz_index = value;
    } else if (!strcmp(key, "toast_time_index")) {
      profile->toast_time_index = value;
    } else if (!strcmp(key, "reduce_motion")) {
      profile->reduce_motion = value;
    } else if (!strcmp(key, "nav_delay_index")) {
      profile->nav_delay_index = value;
    } else if (!strcmp(key, "nav_speed_index")) {
      profile->nav_speed_index = value;
    } else if (!strcmp(key, "touchpad_enabled")) {
      profile->touchpad_enabled = value;
    } else if (load_hash_array(key, value, "favorite_vehicle", profile->favorite_vehicles,
                               GTAV_FAVORITE_VEHICLES_MAX) ||
               load_hash_array(key, value, "favorite_ped", profile->favorite_peds,
                               GTAV_FAVORITE_PEDS_MAX) ||
               load_hash_array(key, value, "recent_vehicle", profile->recent_vehicles,
                               GTAV_RECENT_VEHICLES_MAX) ||
               load_hash_array(key, value, "recent_ped", profile->recent_peds,
                               GTAV_RECENT_PEDS_MAX) ||
               load_hash_array(key, value, "wardrobe_drawable", profile->wardrobe_drawable,
                               GTAV_WARDROBE_SLOTS) ||
               load_hash_array(key, value, "wardrobe_texture", profile->wardrobe_texture,
                               GTAV_WARDROBE_SLOTS) ||
               load_hash_array(key, value, "saved_vehicle_model", profile->saved_vehicle_model,
                               GTAV_SAVED_VEHICLE_SLOTS) ||
               load_hash_array(key, value, "saved_vehicle_mod", &profile->saved_vehicle_mods[0][0],
                               GTAV_SAVED_VEHICLE_SLOTS * GTAV_SAVED_VEHICLE_MOD_SLOTS) ||
               load_hash_array(key, value, "saved_outfit_drawable",
                               &profile->saved_outfit_drawable[0][0],
                               GTAV_SAVED_OUTFIT_SLOTS * GTAV_WARDROBE_SLOTS) ||
               load_hash_array(key, value, "saved_outfit_texture",
                               &profile->saved_outfit_texture[0][0],
                               GTAV_SAVED_OUTFIT_SLOTS * GTAV_WARDROBE_SLOTS) ||
               load_hash_array(key, value, "quick_action", profile->quick_action,
                               GTAV_QUICK_PINS_MAX) ||
               load_hash_array(key, value, "quick_param", profile->quick_param,
                               GTAV_QUICK_PINS_MAX) ||
               load_hash_array(key, value, "custom_theme", profile->custom_theme,
                               GTAV_PROFILE_CUSTOM_THEME_CHANNELS)) {
      /* matched a favorites/recents/wardrobe/saved-vehicle/quick/custom-theme slot */
    } else {
      /* keybind{N}_mask / keybind{N}_action -- match the dynamic key names. */
      char name[24];
      uint32_t i;
      for (i = 0; i < GTAV_KEYBIND_SLOTS; ++i) {
        snprintf(name, sizeof(name), "keybind%u_mask", i);
        if (!strcmp(key, name)) {
          profile->keybind_mask[i] = value;
          break;
        }
        snprintf(name, sizeof(name), "keybind%u_action", i);
        if (!strcmp(key, name)) {
          profile->keybind_action[i] = value;
          break;
        }
      }
    }
  }

  fclose(fp);
  return 0;
}
