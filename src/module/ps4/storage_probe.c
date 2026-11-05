#include "gtavmenu/ps4_slice.h"
#include "gtavmenu/ps4_syscall.h"

#include <string.h>

static void probe_path(const char* path, int32_t* error, uint32_t* mode) {
  uint8_t info[128];
  uint16_t st_mode = 0;
  memset(info, 0, sizeof(info));
  const int64_t result =
      gtav_ps4_syscall3(GTAV_PS4_SYS_STAT, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)info, 0);
  if (result < 0) {
    *error = (int32_t)result;
    *mode = 0;
    return;
  }
  memcpy(&st_mode, info + GTAV_PS4_STAT_MODE_OFFSET, sizeof(st_mode));
  *error = 0;
  *mode = st_mode;
}

void gtav_ps4_storage_probe(GtavPs4Control* control) {
  if (!control) return;
  probe_path("/app0", &control->probe_app0_error, &control->probe_app0_mode);
  probe_path("/data", &control->probe_data_error, &control->probe_data_mode);
  probe_path("/data/GTAVMenu", &control->probe_menu_error, &control->probe_menu_mode);
  __atomic_store_n(&control->storage_probe, GTAV_PS4_STORAGE_PROBE_DONE, __ATOMIC_RELEASE);
}
