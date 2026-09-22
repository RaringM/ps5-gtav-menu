#pragma once

#include <stdint.h>

// OnionHEN plugin descriptor ABI v1. It is an ELF marker consumed before the
// plugin starts, not a host function table. Keep this small local definition in
// sync with the public onionHEN plugin SDK so the normal GTAV build does not
// acquire a link-time dependency on the SDK runtime.
#define GTAV_ONION_PLUGIN_ABI_VERSION 1u
#define GTAV_ONION_PLUGIN_ID_MAX 32u
#define GTAV_ONION_PLUGIN_VERSION_MAX 16u
#define GTAV_ONION_PLUGIN_NAME_MAX 64u

#define GTAV_ONION_CAP_NOTIFY (1u << 0)
#define GTAV_ONION_CAP_PROCESS (1u << 2)
#define GTAV_ONION_CAP_INJECT (1u << 3)
#define GTAV_ONION_CAP_KERNEL (1u << 4)

#define GTAV_ONION_FLAG_LONG_RUNNING (1u << 1)
#define GTAV_ONION_FLAG_STOP_SUPPORTED (1u << 2)

typedef struct {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t capabilities;
  uint32_t flags;
  char plugin_id[GTAV_ONION_PLUGIN_ID_MAX];
  char version[GTAV_ONION_PLUGIN_VERSION_MAX];
  char name[GTAV_ONION_PLUGIN_NAME_MAX];
} GtavOnionPluginDescriptor;

_Static_assert(sizeof(GtavOnionPluginDescriptor) == 128, "OnionHEN descriptor ABI v1");
