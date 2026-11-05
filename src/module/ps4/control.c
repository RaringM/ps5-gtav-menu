#include "gtavmenu/ps4_slice.h"

#include <string.h>

GtavPs4Control gtav_ps4_control __attribute__((used, visibility("default"))) = {
    .magic = GTAV_PS4_CONTROL_MAGIC,
    .abi_version = GTAV_PS4_CONTROL_ABI,
    .struct_size = GTAV_PS4_CONTROL_SIZE,
    .state = GTAV_PS4_STATE_LOADED,
};

/* Runs before any hook exists and before the host writes a field: the dev worker's loader entry on
   a fresh image, or the plugin's start on an image a clean stop re-armed. Either way the control
   block starts from its load state, never from the last run's worker flags, native readiness,
   frame counters or pad hook state. */
void gtav_ps4_runtime_load(void) {
  memset(&gtav_ps4_control, 0, sizeof(gtav_ps4_control));
  gtav_ps4_control.magic = GTAV_PS4_CONTROL_MAGIC;
  gtav_ps4_control.abi_version = GTAV_PS4_CONTROL_ABI;
  gtav_ps4_control.struct_size = GTAV_PS4_CONTROL_SIZE;
  gtav_ps4_control.state = GTAV_PS4_STATE_LOADED;
  gtav_ps4_storage_probe(&gtav_ps4_control);
  gtav_ps4_game_init();
}
