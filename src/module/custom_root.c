// Custom content requires the HEN-owned /data view. No alternate root or mount.
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gtavmenu/custom_assets.h"
#include "gtavmenu/log.h"

// Host tests use temporary trees instead of console paths.
#ifndef CUSTOM_ROOT_DATA
#define CUSTOM_ROOT_DATA GTAV_CUSTOM_GAME_ROOT
#endif
#ifndef CUSTOM_DATA_DIRECTORY
#define CUSTOM_DATA_DIRECTORY "/data"
#endif

extern void gtav_worker_klog(const char* message) __attribute__((weak));
enum { ROOT_UNDECIDED = 0, ROOT_AVAILABLE = 1, ROOT_UNAVAILABLE = 2 };
static int g_custom_root_state;

static int custom_data_readable(void) {
  // Some HEN views return empty directory listings while direct file reads work.
  static const char* const names[] = {"active", "installed"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    char path[512];
    const int n = snprintf(path, sizeof(path), "%s/packs/%s", CUSTOM_ROOT_DATA, names[i]);
    if (n <= 0 || (size_t)n >= sizeof(path)) continue;
    int control = open(path, O_RDONLY | O_NONBLOCK);
    if (control >= 0) {
      struct stat st;
      const int regular = fstat(control, &st) == 0 && S_ISREG(st.st_mode);
      close(control);
      if (regular) return 1;
    }
  }
  // A readable but empty /data is supported: the pack uploader may not have run yet.
  // Open the existing directory, never create it or enumerate its contents.
  int fd = open(CUSTOM_DATA_DIRECTORY, O_RDONLY | O_DIRECTORY);
  if (fd < 0) return 0;
  close(fd);
  return 1;
}

void gtav_custom_game_root_init(void) {
  if (__atomic_load_n(&g_custom_root_state, __ATOMIC_ACQUIRE) != ROOT_UNDECIDED) return;
  const int state = custom_data_readable() ? ROOT_AVAILABLE : ROOT_UNAVAILABLE;
  __atomic_store_n(&g_custom_root_state, state, __ATOMIC_RELEASE);
  if (gtav_worker_klog)
    gtav_worker_klog(state == ROOT_AVAILABLE ? "GTAVMenu pack root " CUSTOM_ROOT_DATA
                                             : "GTAVMenu pack unavailable: shared /data missing");
}

const char* gtav_custom_game_root(void) {
  return CUSTOM_ROOT_DATA;
}

int gtav_custom_game_root_available(void) {
  return __atomic_load_n(&g_custom_root_state, __ATOMIC_ACQUIRE) == ROOT_AVAILABLE;
}
