#include "gtavmenu/nexhen_plugin.h"

typedef int (*GtavNexhenModuleEntry)(size_t, const void*);

__attribute__((used, aligned(8), section(".sce_module_param")))
const uint64_t gtav_nexhen_module_parameters[] = {0x20, 0x33c13f4bf, 0x0200000908050001, 1};

int gtav_nexhen_module_init(size_t size, const void* args, GtavNexhenModuleEntry override) {
  return override ? override(size, args) : gtav_nexhen_module_start(size, args);
}

int gtav_nexhen_module_fini(size_t size, const void* args, GtavNexhenModuleEntry override) {
  return override ? override(size, args) : gtav_nexhen_module_stop(size, args);
}
