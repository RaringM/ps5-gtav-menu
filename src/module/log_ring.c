#include "gtavmenu/log_ring.h"

#include "gtavmenu/log.h"
#include "gtavmenu/strutil.h"

_Static_assert(sizeof(GtavMenuLogEntry) == GTAV_MENU_LOG_RING_ENTRY_SIZE,
               "GtavMenuLogEntry size changed");
_Static_assert(sizeof(GtavMenuLogRing) == GTAV_MENU_LOG_RING_SIZE, "GtavMenuLogRing size changed");

GtavMenuLogRing gtav_menu_log_ring __attribute__((used, visibility("default"))) = {
    .magic = GTAV_MENU_LOG_RING_MAGIC,
    .abi_version = GTAV_MENU_LOG_RING_ABI_VERSION,
    .struct_size = sizeof(GtavMenuLogRing),
};

const char gtav_menu_log_ring_marker[] __attribute__((used, visibility("default"))) =
    "GTAVMENU_LOGRING_V1";

void gtav_log_ring_write(uint32_t level, uint32_t category, const char* message) {
  uint32_t index = gtav_menu_log_ring.write_index % GTAV_MENU_LOG_RING_ENTRY_COUNT;
  GtavMenuLogEntry* entry = &gtav_menu_log_ring.entries[index];

  entry->seq = gtav_menu_log_ring.write_index;
  entry->level = level;
  entry->category = category;
  gtav_copy_string(entry->message, sizeof(entry->message), message);

  gtav_menu_log_ring.write_index++;
  if (gtav_menu_log_ring.count < GTAV_MENU_LOG_RING_ENTRY_COUNT) {
    gtav_menu_log_ring.count++;
  }
}

// Strong override for the weak gtav_log_sink hook in src/common/log.c: every admitted
// GTAV_LOG* line (after the threshold check) is mirrored into the ring.
void gtav_log_sink(GtavLogLevel level, GtavLogCategory category, const char* message) {
  gtav_log_ring_write((uint32_t)level, (uint32_t)category, message);
}
