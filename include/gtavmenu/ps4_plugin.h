#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_PS4_PLUGIN_GATEWAY_BYTES 30u
#define GTAV_PS4_PLUGIN_ABSOLUTE_JUMP_BYTES 14u
#define GTAV_PS4_PLUGIN_HOOK_PATCH_BYTES 8u
#define GTAV_PS4_PLUGIN_RELAY_HEAD_BYTES 8u
/* The pad import trampoline in the gateway page: FF 25 00000000 at +0x202, its 8-byte target
   at +0x208 (aligned). While the Touchpad swap is in, the import slot holds the trampoline, so
   a slot that cannot be put back still lands in the gateway, and cleanup points the target
   at the original libScePad entry with one aligned data exchange. */
#define GTAV_PS4_PLUGIN_PAD_TRAMPOLINE_OFFSET 0x202u
#define GTAV_PS4_PLUGIN_PAD_TARGET_OFFSET 0x208u
#define GTAV_PS4_PLUGIN_PROT_READ 1u
#define GTAV_PS4_PLUGIN_PROT_WRITE 2u
#define GTAV_PS4_PLUGIN_PROT_EXEC 4u
/* Hooked game frames (the frame hook ran the menu pass) the menu may take to pass its activation
   gates once the first one ran; while none has run there is no deadline (a loading screen can
   hold the hook for minutes). About 15 s at 30 fps. */
#ifndef GTAV_PS4_PLUGIN_ACTIVATION_FRAMES
#define GTAV_PS4_PLUGIN_ACTIVATION_FRAMES 450u
#endif
/* An earlier instance in the same game process that stopped without the cooperative shutdown, or
   kept its gateway mapped, leaves this marker ("pid=<pid> gateway=0x<hex> shutdown=<0|1>"); the
   next instance in that process resets the features and reclaims the gateway. */
#define GTAV_PS4_PLUGIN_UNCLEAN_STOP_PATH "/data/GTAVMenu/state/unclean-stop"

/* -1005 was the readiness error, never returned; the numbers are kept stable. */
enum {
  GTAV_PS4_PLUGIN_ERROR_MAPPING = -1001,
  GTAV_PS4_PLUGIN_ERROR_VERSION = -1002,
  GTAV_PS4_PLUGIN_ERROR_HOOK_IDENTITY = -1003,
  GTAV_PS4_PLUGIN_ERROR_NATIVE = -1004,
  GTAV_PS4_PLUGIN_ERROR_GATEWAY = -1006,
  GTAV_PS4_PLUGIN_ERROR_HOOK_INSTALL = -1007,
  GTAV_PS4_PLUGIN_ERROR_ACTIVATION = -1008,
  GTAV_PS4_PLUGIN_ERROR_SHUTDOWN = -1009,
  GTAV_PS4_PLUGIN_ERROR_HOOK_CHANGED = -1010,
  GTAV_PS4_PLUGIN_ERROR_PAD_CHANGED = -1011,
  GTAV_PS4_PLUGIN_ERROR_INFLIGHT = -1012,
  GTAV_PS4_PLUGIN_ERROR_UNMAP = -1013,
  GTAV_PS4_PLUGIN_ERROR_STATE = -1014,
  GTAV_PS4_PLUGIN_ERROR_RELAY = -1015,
};

typedef struct {
  uint64_t start;
  uint64_t end;
  uint32_t prot;
  char name[32];  // filled by the map_name op only; query leaves it empty
} GtavPs4PluginMapInfo;

/* Results of the compare-and-exchange ops. */
enum {
  GTAV_PS4_PLUGIN_CAS_SWAPPED = 0,
  GTAV_PS4_PLUGIN_CAS_DIFFERS = 1, /* the word held *observed instead; nothing was written */
  GTAV_PS4_PLUGIN_CAS_REFUSED = -1 /* misaligned, unmapped or not writable; nothing was written */
};

typedef struct GtavPs4PluginOps {
  void* context;
  /* The mapping holding `address` and its protection (the boot-pool samples ask this up to 1000
     times a second, so no name lookup here). */
  int (*query)(void* context, uint64_t address, GtavPs4PluginMapInfo* out);
  /* The mapping's name (out->name) as well: only where a module must be identified. */
  int (*map_name)(void* context, uint64_t address, GtavPs4PluginMapInfo* out);
  int (*read)(void* context, uint64_t address, void* out, uint32_t size);
  /* Plain byte copy: only for memory no other thread executes or reads yet (the unpublished
     gateway page). */
  int (*write)(void* context, uint64_t address, const void* data, uint32_t size);
  /* One aligned atomic exchange of a word other threads may be executing or loading right now (the
     hook site, the pad import slot, a published gateway word). GTAV_PS4_PLUGIN_CAS_*. */
  int (*cas64)(void* context, uint64_t address, uint64_t expected, uint64_t desired,
               uint64_t* observed);
  int (*cas32)(void* context, uint64_t address, uint32_t expected, uint32_t desired,
               uint32_t* observed);
  int (*protect)(void* context, uint64_t address, uint32_t size, uint32_t prot);
  int (*map)(void* context, uint64_t hint, uint32_t size, uint64_t* out);
  int (*unmap)(void* context, uint64_t address, uint32_t size);
  uint32_t (*pid)(void* context);
  /* Monotonic process time in microseconds. */
  uint64_t (*now_us)(void* context);
  void (*sleep_us)(void* context, uint32_t usec);
  void (*log)(void* context, const char* message);
} GtavPs4PluginOps;

typedef enum {
  GTAV_PS4_PLUGIN_IDLE = 0,
  GTAV_PS4_PLUGIN_WAITING,
  GTAV_PS4_PLUGIN_ACTIVE,
  GTAV_PS4_PLUGIN_STOPPED,
  GTAV_PS4_PLUGIN_FAILED,
} GtavPs4PluginState;

typedef struct {
  GtavPs4PluginOps ops;
  GtavPs4PluginState state;
  int last_error;
  uint64_t gateway;
  uint64_t activation_frame;
  uint64_t pad_original;
  uint32_t pid;
  uint32_t nonce;
  int game_initialized;
  int gateway_mapped;
  int hook_attempted;
  int hook_installed;
  /* The relay no longer jumps into PRX code (cleanup turned it into a passthrough). */
  int relay_neutral;
  /* The address the pad import slot is swapped to (the gateway trampoline; 0 = unpublished). */
  uint64_t pad_entry;
  /* The trampoline's target no longer points into PRX code. */
  int pad_neutral;
  /* Cleanup could not take every route into PRX code away: unloading would crash the game. */
  int unload_unsafe;
  /* What the last gtav_ps4_plugin_runtime_cleanup returned (0 before any). */
  int cleanup_result;
  /* The last cleanup: the game produced no frame for the whole shutdown budget (backgrounded), so
     the gateway was kept mapped for a thread the stall may have frozen inside it. */
  int frames_stalled;
  /* The last cleanup: the game thread never finished the cooperative shutdown, so features may
     still be on in the game. */
  int shutdown_unfinished;
  /* Inputs from an earlier instance's unclean-stop marker in this process, set before the start:
     reset the features on the first active frame, and reclaim the gateway it left at the hint. */
  int reset_features;
  uint64_t stale_gateway;
} GtavPs4PluginRuntime;

int gtav_ps4_plugin_encode_hook(uint64_t gateway, uint64_t thunk,
                                uint8_t gateway_bytes[GTAV_PS4_PLUGIN_GATEWAY_BYTES],
                                uint8_t relay[GTAV_PS4_PLUGIN_ABSOLUTE_JUMP_BYTES],
                                uint8_t patch[GTAV_PS4_PLUGIN_HOOK_PATCH_BYTES]);

/* The relay head with its absolute jump into PRX code replaced by E9 rel32 to gateway+0 (the
   relocated original prologue and its jump to the continuation); bytes 5..7 are kept, so one
   aligned 8-byte exchange flips it. */
int gtav_ps4_plugin_encode_relay_passthrough(
    uint64_t gateway, const uint8_t relay_head[GTAV_PS4_PLUGIN_RELAY_HEAD_BYTES],
    uint8_t out[GTAV_PS4_PLUGIN_RELAY_HEAD_BYTES]);

/* The pad trampoline: an absolute jump to `target` (the PRX pad hook, or the original entry
   once neutralized). */
int gtav_ps4_plugin_encode_pad_trampoline(uint64_t target,
                                          uint8_t out[GTAV_PS4_PLUGIN_ABSOLUTE_JUMP_BYTES]);

void gtav_ps4_plugin_runtime_init(GtavPs4PluginRuntime* runtime, const GtavPs4PluginOps* ops);
/* The start preflight's game identity checks, safe before Story Mode: the text and data mapping
   layout, the version bytes, the PLAYER_PED_ID prologue and the native fingerprints. Reads only
   inside the eboot image. 0 = this target, 1 = not readable yet, negative = a mismatch. */
int gtav_ps4_plugin_runtime_identity(GtavPs4PluginRuntime* runtime);
/* 0 = installed, 1 = game is not ready yet, negative = fatal. */
int gtav_ps4_plugin_runtime_try_start(GtavPs4PluginRuntime* runtime);
/* 1 when Story Mode looks ready (the player ped exists and the text flags are up), from two image
   words and one protection-checked heap read: cheap enough to ask during the boot-pool samples. */
int gtav_ps4_plugin_runtime_story_mode(GtavPs4PluginRuntime* runtime);
/* 1 = the full menu passed its live activation gates, 0 = still starting, negative = failed. */
int gtav_ps4_plugin_runtime_activated(GtavPs4PluginRuntime* runtime);
/* Hooked game frames since the hook went in (the activation deadline counts these). */
uint64_t gtav_ps4_plugin_runtime_hooked_frames(const GtavPs4PluginRuntime* runtime);
/* After activation: grant /data to the game thread (the profile import) when the storage probe
   passed. 1 = granted. */
int gtav_ps4_plugin_runtime_grant_storage(GtavPs4PluginRuntime* runtime);
/* 1 once the game thread reset the features an earlier instance left on (reset_features). */
int gtav_ps4_plugin_runtime_features_reset(const GtavPs4PluginRuntime* runtime);
int gtav_ps4_plugin_runtime_menu_stop_requested(const GtavPs4PluginRuntime* runtime);
int gtav_ps4_plugin_runtime_cleanup(GtavPs4PluginRuntime* runtime);
const char* gtav_ps4_plugin_error_string(int error);

#ifdef __cplusplus
}
#endif
