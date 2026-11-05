#include "gtavmenu/ps4_plugin.h"

#include "gtavmenu/ps4_pad_hook.h"
#include "gtavmenu/ps4_slice.h"
#include "gtavmenu/ps4_syscall.h"
#include "gtavmenu/sha256.h"
#include GTAV_PS4_TARGET_HEADER
#ifndef GTAV_PS4_PLUGIN_HEADER
#error \
    "GTAV_PS4_PLUGIN_HEADER must name the target's generated PS4 plugin header (tools/ps4_build.py)"
#endif
#include GTAV_PS4_PLUGIN_HEADER

#include <stdio.h>
#include <string.h>

namespace {

constexpr uint32_t kPollUsec = 50000;
// Unload budget, in 50 ms polls. The cooperative shutdown gets 6 s of polls with nothing in
// flight: while a hooked frame is still running on the game thread (our own frame blocking it,
// a pack Load for example) the shutdown runs as soon as that frame returns, so those polls do not
// count, up to 60 s in all. A game that produced no frame for the last second of the budget is
// stalled (backgrounded): the waits that need frames are skipped. Quiesce 3 s; drain 3 s, or up to
// 60 s while a thread is still counted inside the frame hook. Blocking the unload is the lesser
// harm: the host unmaps the module when plugin_unload returns, whatever it returns.
constexpr uint32_t kShutdownPolls = 120;
constexpr uint32_t kBusyPolls = 1200;
constexpr uint32_t kQuiescePolls = 60;
constexpr uint32_t kFrameStallPolls = 20;
constexpr uint32_t kSettleSamples = 3;
// Drain: 250 ms of grace for a thread that already took a jump into PRX code but has not counted
// itself yet, then 250 ms of consecutive quiet samples.
constexpr uint32_t kDrainGraceSamples = 5;
constexpr uint32_t kDrainQuietSamples = 5;
constexpr uint32_t kDrainPolls = 60;
constexpr uint32_t kProgressPolls = 100;  // a long wait logs where it is every 5 s
constexpr uint32_t kPageBytes = 0x4000;
// The gateway's map, protect and unmap span: the manifest's size in whole 16 KiB pages.
constexpr uint32_t kGatewaySpan =
    (GTAV_PS4_PLUGIN_GATEWAY_SIZE + kPageBytes - 1u) & ~(kPageBytes - 1u);
constexpr uint32_t kMaxRegionEntries = 64;
constexpr uint32_t kSessionTag = 0x504c5547;  // "PLUG": the control block belongs to the PRX

// Every live word is exchanged with one aligned 8-byte compare-and-exchange: script threads call
// PLAYER_PED_ID and the pad import continuously, so a byte-wise copy could be executed
// half-written.
static_assert(GTAV_PS4_HOOK_ADDR % 8u == 0, "the hook patch must be one aligned qword");
static_assert(GTAV_PS4_PAD_READ_STATE_GOT % 8u == 0,
              "the pad import slot must be an aligned qword");
static_assert((GTAV_PS4_PLUGIN_GATEWAY_HINT + GTAV_PS4_PLUGIN_RELAY_OFFSET) % 8u == 0,
              "the relay head must be an aligned qword");
static_assert(GTAV_PS4_PLUGIN_HOOK_PATCH_BYTES == sizeof(uint64_t), "the hook patch is one qword");
static_assert((GTAV_PS4_PLUGIN_GATEWAY_HINT + GTAV_PS4_PLUGIN_PAD_TARGET_OFFSET) % 8u == 0,
              "the pad trampoline target must be an aligned qword");
static_assert(GTAV_PS4_PLUGIN_PAD_TRAMPOLINE_OFFSET + 6u == GTAV_PS4_PLUGIN_PAD_TARGET_OFFSET,
              "the pad trampoline reads its target right after the FF 25 00000000 head");
static_assert(GTAV_PS4_PLUGIN_PAD_TARGET_OFFSET + 8u <= GTAV_PS4_PLUGIN_GATEWAY_SIZE &&
                  GTAV_PS4_PLUGIN_RELAY_OFFSET + GTAV_PS4_PLUGIN_ABSOLUTE_JUMP_BYTES <=
                      GTAV_PS4_PLUGIN_PAD_TRAMPOLINE_OFFSET,
              "the relay and the pad trampoline must fit the gateway without overlapping");
static_assert(GTAV_PS4_PLUGIN_GATEWAY_HINT % kPageBytes == 0, "the gateway starts on a page");

uint64_t qword(const uint8_t* bytes) {
  uint64_t value = 0;
  memcpy(&value, bytes, sizeof(value));
  return value;
}

void absolute_jump(uint64_t destination, uint8_t out[GTAV_PS4_PLUGIN_ABSOLUTE_JUMP_BYTES]) {
  static const uint8_t kPrefix[] = {0xff, 0x25, 0, 0, 0, 0};
  memcpy(out, kPrefix, sizeof(kPrefix));
  memcpy(out + sizeof(kPrefix), &destination, sizeof(destination));
}

void log_line(GtavPs4PluginRuntime* runtime, const char* message) {
  if (runtime->ops.log) runtime->ops.log(runtime->ops.context, message);
}

void log_failure(GtavPs4PluginRuntime* runtime, int error) {
  char message[192];
  snprintf(message, sizeof(message), "GTAV Menu plugin: %s (%d)",
           gtav_ps4_plugin_error_string(error), error);
  log_line(runtime, message);
}

int read_u32(GtavPs4PluginRuntime* runtime, uint64_t address, uint32_t* out) {
  return runtime->ops.read(runtime->ops.context, address, out, sizeof(*out));
}

int read_u64(GtavPs4PluginRuntime* runtime, uint64_t address, uint64_t* out) {
  return runtime->ops.read(runtime->ops.context, address, out, sizeof(*out));
}

int write_memory(GtavPs4PluginRuntime* runtime, uint64_t address, const void* data, uint32_t size) {
  return runtime->ops.write(runtime->ops.context, address, data, size);
}

uint32_t inflight() {
  return __atomic_load_n(&gtav_ps4_control.inflight, __ATOMIC_ACQUIRE);
}

uint32_t worker_flags() {
  return __atomic_load_n(&gtav_ps4_control.worker_flags, __ATOMIC_ACQUIRE);
}

// The code page a live word sits on: the gateway (relay head, pad trampoline target) or the hook
// site's page in the game's text.
enum class CodePage { kGateway, kHookSite };

int protect_page(GtavPs4PluginRuntime* runtime, CodePage page, uint32_t prot) {
  return page == CodePage::kGateway
             ? runtime->ops.protect(runtime->ops.context, runtime->gateway, kGatewaySpan, prot)
             : runtime->ops.protect(runtime->ops.context, GTAV_PS4_HOOK_ADDR,
                                    sizeof(kGtavPs4PluginHookBytes), prot);
}

// One aligned 8-byte exchange of a word on a code page other threads may be executing: make the
// page writable, exchange, make it read+execute again.
struct LiveExchange {
  int cas;        // GTAV_PS4_PLUGIN_CAS_*; REFUSED when the page could not be made writable
  uint64_t seen;  // the word the exchange saw
  bool rx_again;  // the page is read+execute again
  // The word holds `desired` now: swapped, or it already held it.
  bool holds(uint64_t desired) const {
    return cas == GTAV_PS4_PLUGIN_CAS_SWAPPED ||
           (cas == GTAV_PS4_PLUGIN_CAS_DIFFERS && seen == desired);
  }
};

LiveExchange exchange_live_qword(GtavPs4PluginRuntime* runtime, CodePage page, uint64_t address,
                                 uint64_t expected, uint64_t desired) {
  LiveExchange result = {GTAV_PS4_PLUGIN_CAS_REFUSED, 0, false};
  if (protect_page(runtime, page,
                   GTAV_PS4_PLUGIN_PROT_READ | GTAV_PS4_PLUGIN_PROT_WRITE |
                       GTAV_PS4_PLUGIN_PROT_EXEC) != 0) {
    return result;
  }
  result.cas = runtime->ops.cas64(runtime->ops.context, address, expected, desired, &result.seen);
  result.rx_again =
      protect_page(runtime, page, GTAV_PS4_PLUGIN_PROT_READ | GTAV_PS4_PLUGIN_PROT_EXEC) == 0;
  return result;
}

// The hook site's first 16 bytes once the patch is in: the jump to the relay, then the original
// bytes after it. Also builds the gateway contents and the relay for `gateway`.
int encode_site(uint64_t gateway, uint8_t gateway_bytes[GTAV_PS4_PLUGIN_GATEWAY_BYTES],
                uint8_t relay[GTAV_PS4_PLUGIN_ABSOLUTE_JUMP_BYTES],
                uint8_t patch[GTAV_PS4_PLUGIN_HOOK_PATCH_BYTES],
                uint8_t patched_site[sizeof(kGtavPs4PluginHookBytes)]) {
  const uint64_t thunk = (uint64_t)(uintptr_t)&gtav_ps4_frame_hook;
  const int error = gtav_ps4_plugin_encode_hook(gateway, thunk, gateway_bytes, relay, patch);
  if (error != 0) return error;
  memcpy(patched_site, patch, GTAV_PS4_PLUGIN_HOOK_PATCH_BYTES);
  memcpy(patched_site + GTAV_PS4_PLUGIN_HOOK_PATCH_BYTES,
         kGtavPs4PluginHookBytes + GTAV_PS4_PLUGIN_HOOK_PATCH_BYTES,
         sizeof(kGtavPs4PluginHookBytes) - GTAV_PS4_PLUGIN_HOOK_PATCH_BYTES);
  return 0;
}

// A region may span several adjacent map entries: installing the hook changes the protection of one
// code page and writes it through copy-on-write, and the kernel never merges that page back after
// the restore. Accept the split only as an exact, gapless cover with the expected protection on
// every piece; the byte and fingerprint checks still prove the contents are original.
int mapping_matches(GtavPs4PluginRuntime* runtime, const GtavPs4PluginMapping& expected) {
  uint64_t cursor = expected.start;
  for (uint32_t i = 0; i < kMaxRegionEntries && cursor < expected.end; ++i) {
    GtavPs4PluginMapInfo info;
    if (runtime->ops.query(runtime->ops.context, cursor, &info) != 0) {
      return cursor == expected.start ? 1 : GTAV_PS4_PLUGIN_ERROR_MAPPING;
    }
    if (info.start != cursor || info.end <= cursor || info.end > expected.end ||
        info.prot != expected.prot) {
      return GTAV_PS4_PLUGIN_ERROR_MAPPING;
    }
    cursor = info.end;
  }
  return cursor == expected.end ? 0 : GTAV_PS4_PLUGIN_ERROR_MAPPING;
}

int compare_bytes(GtavPs4PluginRuntime* runtime, uint64_t address, const uint8_t* expected,
                  uint32_t size) {
  uint8_t observed[32];
  if (size > sizeof(observed)) return GTAV_PS4_PLUGIN_ERROR_STATE;
  if (runtime->ops.read(runtime->ops.context, address, observed, size) != 0) return 1;
  return memcmp(observed, expected, size) == 0 ? 0 : -1;
}

// A heap word the game publishes (the player ped behind the player global): read only once the
// protection query shows the whole word readable, never dereferenced blind.
int read_heap_u64(GtavPs4PluginRuntime* runtime, uint64_t address, uint64_t* out) {
  GtavPs4PluginMapInfo info;
  if ((address & 7u) != 0 || runtime->ops.query(runtime->ops.context, address, &info) != 0 ||
      (info.prot & GTAV_PS4_PLUGIN_PROT_READ) == 0 || info.start > address ||
      address + sizeof(*out) > info.end) {
    return -1;
  }
  return read_u64(runtime, address, out);
}

// The mapping, version, prologue and native checks: only eboot-image reads, so they can run before
// Story Mode (the boot-pool gate) as well as in the start preflight.
int identity(GtavPs4PluginRuntime* runtime) {
  int result = mapping_matches(runtime, kGtavPs4PluginTextMapping);
  if (result != 0) return result;
  for (size_t i = 0; i < GTAV_PS4_PLUGIN_DATA_SEGMENT_COUNT; ++i) {
    result = mapping_matches(runtime, kGtavPs4PluginDataSegments[i]);
    if (result != 0) return result;
  }

  result = compare_bytes(runtime, GTAV_PS4_VERSION_ADDR, kGtavPs4PluginVersionBytes,
                         sizeof(kGtavPs4PluginVersionBytes));
  if (result > 0) return result;
  if (result < 0) return GTAV_PS4_PLUGIN_ERROR_VERSION;
  result = compare_bytes(runtime, GTAV_PS4_HOOK_ADDR, kGtavPs4PluginHookBytes,
                         sizeof(kGtavPs4PluginHookBytes));
  if (result > 0) return result;
  if (result < 0) return GTAV_PS4_PLUGIN_ERROR_HOOK_IDENTITY;

  for (size_t i = 0; i < GTAV_PS4_PLUGIN_NATIVE_COUNT; ++i) {
    uint8_t bytes[32];
    uint8_t digest[32];
    const GtavPs4PluginNativeFingerprint& native = kGtavPs4PluginNativeFingerprints[i];
    if (runtime->ops.read(runtime->ops.context, native.address, bytes, sizeof(bytes)) != 0) {
      return GTAV_PS4_PLUGIN_ERROR_NATIVE;
    }
    GtavSha256 sha;
    gtav_sha256_init(&sha);
    gtav_sha256_update(&sha, bytes, sizeof(bytes));
    gtav_sha256_final(&sha, digest);
    if (memcmp(digest, native.digest, sizeof(digest)) != 0) return GTAV_PS4_PLUGIN_ERROR_NATIVE;
  }
  return 0;
}

// The player ped exists and the text flags the menu's draw path needs are up.
bool story_mode_ready(GtavPs4PluginRuntime* runtime) {
  uint64_t player_global = 0;
  uint64_t player_ped = 0;
  uint32_t text_flags = 0;
  return read_u64(runtime, GTAV_PS4_PLAYER_GLOBAL, &player_global) == 0 && player_global &&
         read_heap_u64(runtime, player_global + GTAV_PS4_PLAYER_PED_OFFSET, &player_ped) == 0 &&
         player_ped && read_u32(runtime, GTAV_PS4_TEXT_FLAGS, &text_flags) == 0 && text_flags;
}

// Story Mode is ready for the hook: the player ped exists, the text flags are up and the frame
// counter moves, so hooked frames reach activation without a wait.
int preflight(GtavPs4PluginRuntime* runtime) {
  const int result = identity(runtime);
  if (result != 0) return result;
  if (!story_mode_ready(runtime)) return 1;

  uint32_t first_frame = 0;
  uint32_t second_frame = 0;
  if (read_u32(runtime, GTAV_PS4_FRAME_COUNT, &first_frame) != 0) return 1;
  runtime->ops.sleep_us(runtime->ops.context, kPollUsec);
  if (read_u32(runtime, GTAV_PS4_FRAME_COUNT, &second_frame) != 0) return 1;
  if (first_frame == second_frame) return 1;

  runtime->pad_original = 0;
#if GTAV_PS4_PLUGIN_TOUCHPAD_ENABLED
  uint64_t pad = 0;
  GtavPs4PluginMapInfo pad_info;
  if (read_u64(runtime, GTAV_PS4_PAD_READ_STATE_GOT, &pad) == 0 && pad &&
      runtime->ops.map_name(runtime->ops.context, pad, &pad_info) == 0 &&
      (pad_info.prot & GTAV_PS4_PLUGIN_PROT_EXEC) != 0 &&
      strcmp(pad_info.name, GTAV_PS4_PLUGIN_PAD_IMPORT_MODULE) == 0) {
    runtime->pad_original = pad;
  }
#endif
  return 0;
}

int storage_ready() {
  return GTAV_PS4_PLUGIN_STORAGE_ENABLED &&
         gtav_ps4_control.storage_probe == GTAV_PS4_STORAGE_PROBE_DONE &&
         gtav_ps4_control.probe_app0_error == 0 && gtav_ps4_control.probe_data_error == 0 &&
         (gtav_ps4_control.probe_data_mode & GTAV_PS4_S_IFMT) == GTAV_PS4_S_IFDIR;
}

// /data is granted only after activation (gtav_ps4_plugin_runtime_grant_storage): the profile
// import re-applies features through natives, which must not run before the menu proved itself.
void publish_control(GtavPs4PluginRuntime* runtime, uint64_t thunk) {
  gtav_ps4_control.gateway_address = runtime->gateway;
  gtav_ps4_control.thunk_address = thunk;
  gtav_ps4_control.pid = runtime->pid;
  gtav_ps4_control.session_nonce = runtime->nonce;
  if (runtime->pad_original) gtav_ps4_control.pad_got_expected = runtime->pad_original;
  // The game thread swaps the pad slot to the gateway trampoline, never to PRX code directly.
  runtime->pad_entry = runtime->gateway + GTAV_PS4_PLUGIN_PAD_TRAMPOLINE_OFFSET;
  gtav_ps4_pad_hook_set_entry(runtime->pad_entry);
  uint32_t flags = GTAV_PS4_HOST_UI_ALLOWED | GTAV_PS4_HOST_HOOK_ENABLED;
  if (runtime->reset_features) flags |= GTAV_PS4_HOST_RESET_FEATURES;
  __atomic_store_n(&gtav_ps4_control.host_flags, flags, __ATOMIC_RELEASE);
  __atomic_store_n(&gtav_ps4_control.state, GTAV_PS4_STATE_RUNNING, __ATOMIC_RELEASE);
}

// An earlier instance in this process stopped while the game produced no frames and kept its
// gateway at the hint (its unclean-stop marker names it). The game has run frames since (the
// preflight saw the counter move), so no thread is still inside it. Unmap it only when it is
// exactly what that instance left: the relocated prologue at +0 and a relay that is a passthrough
// to it, with the hook site original (the identity check) and the pad slot not routed into it.
int reclaim_stale_gateway(GtavPs4PluginRuntime* runtime) {
  const uint64_t hint = GTAV_PS4_PLUGIN_GATEWAY_HINT;
  if (runtime->stale_gateway != hint) return 0;
  runtime->stale_gateway = 0;
  GtavPs4PluginMapInfo info;
  if (runtime->ops.query(runtime->ops.context, hint, &info) != 0 || info.start > hint) {
    log_line(runtime, "GTAV Menu plugin: the gateway an earlier stop kept is gone already");
    return 0;
  }
  uint8_t expected[GTAV_PS4_PLUGIN_GATEWAY_BYTES];
  uint8_t relay[GTAV_PS4_PLUGIN_ABSOLUTE_JUMP_BYTES];
  uint8_t patch[GTAV_PS4_PLUGIN_HOOK_PATCH_BYTES];
  uint8_t passthrough[GTAV_PS4_PLUGIN_RELAY_HEAD_BYTES];
  uint8_t observed[GTAV_PS4_PLUGIN_GATEWAY_BYTES];
  uint8_t head[GTAV_PS4_PLUGIN_RELAY_HEAD_BYTES];
  uint64_t pad = 0;
  const bool ours =
      (info.prot & GTAV_PS4_PLUGIN_PROT_READ) != 0 && info.end >= hint + kGatewaySpan &&
      gtav_ps4_plugin_encode_hook(hint, (uint64_t)(uintptr_t)&gtav_ps4_frame_hook, expected, relay,
                                  patch) == 0 &&
      gtav_ps4_plugin_encode_relay_passthrough(hint, relay, passthrough) == 0 &&
      runtime->ops.read(runtime->ops.context, hint, observed, sizeof(observed)) == 0 &&
      runtime->ops.read(runtime->ops.context, hint + GTAV_PS4_PLUGIN_RELAY_OFFSET, head,
                        sizeof(head)) == 0 &&
      read_u64(runtime, GTAV_PS4_PAD_READ_STATE_GOT, &pad) == 0 &&
      memcmp(observed, expected, sizeof(observed)) == 0 &&
      // E9 rel32 to +0; bytes 5..7 are whatever followed that instance's relay jump.
      memcmp(head, passthrough, 5) == 0 && (pad < hint || pad >= hint + kGatewaySpan);
  if (!ours) {
    log_line(runtime,
             "GTAV Menu plugin: the mapping at the gateway hint is not the passthrough "
             "an earlier stop kept; left alone");
    return 0;
  }
  if (runtime->ops.unmap(runtime->ops.context, hint, kGatewaySpan) != 0) {
    log_line(runtime, "GTAV Menu plugin: could not unmap the gateway an earlier stop kept");
    return 0;
  }
  log_line(runtime, "GTAV Menu plugin: reclaimed the gateway an earlier stalled stop kept");
  return 0;
}

// The gateway page: the relocated prologue at +0, the relay to the frame hook and the pad
// trampoline, written while nothing executes it, then made read+execute. 0 or an error.
int map_and_fill_gateway(GtavPs4PluginRuntime* runtime, uint8_t patch_out[8],
                         uint8_t patched_site[sizeof(kGtavPs4PluginHookBytes)]) {
  (void)reclaim_stale_gateway(runtime);
  uint64_t gateway = 0;
  if (runtime->ops.map(runtime->ops.context, GTAV_PS4_PLUGIN_GATEWAY_HINT, kGatewaySpan,
                       &gateway) != 0 ||
      gateway != GTAV_PS4_PLUGIN_GATEWAY_HINT) {
    return GTAV_PS4_PLUGIN_ERROR_GATEWAY;
  }
  runtime->gateway = gateway;
  runtime->gateway_mapped = 1;

  uint8_t gateway_bytes[GTAV_PS4_PLUGIN_GATEWAY_BYTES];
  uint8_t relay[GTAV_PS4_PLUGIN_ABSOLUTE_JUMP_BYTES];
  uint8_t trampoline[GTAV_PS4_PLUGIN_ABSOLUTE_JUMP_BYTES];
  int error = encode_site(gateway, gateway_bytes, relay, patch_out, patched_site);
  if (error == 0) {
    error = gtav_ps4_plugin_encode_pad_trampoline(
        (uint64_t)(uintptr_t)&gtav_ps4_pad_read_state_hook, trampoline);
  }
  if (error < 0) return error;
  if (write_memory(runtime, gateway, gateway_bytes, sizeof(gateway_bytes)) != 0 ||
      write_memory(runtime, gateway + GTAV_PS4_PLUGIN_RELAY_OFFSET, relay, sizeof(relay)) != 0 ||
      write_memory(runtime, gateway + GTAV_PS4_PLUGIN_PAD_TRAMPOLINE_OFFSET, trampoline,
                   sizeof(trampoline)) != 0 ||
      protect_page(runtime, CodePage::kGateway,
                   GTAV_PS4_PLUGIN_PROT_READ | GTAV_PS4_PLUGIN_PROT_EXEC) != 0) {
    return GTAV_PS4_PLUGIN_ERROR_GATEWAY;
  }
  return 0;
}

// The one write into the game's code: the hook site's first qword becomes a jump to the relay.
// Read back the original first, and the patched bytes after.
int install_hook(GtavPs4PluginRuntime* runtime, const uint8_t patch[8],
                 const uint8_t patched_site[sizeof(kGtavPs4PluginHookBytes)]) {
  uint8_t observed[sizeof(kGtavPs4PluginHookBytes)];
  if (runtime->ops.read(runtime->ops.context, GTAV_PS4_HOOK_ADDR, observed, sizeof(observed)) !=
          0 ||
      memcmp(observed, kGtavPs4PluginHookBytes, sizeof(observed)) != 0) {
    return GTAV_PS4_PLUGIN_ERROR_HOOK_IDENTITY;
  }
  const LiveExchange exchanged =
      exchange_live_qword(runtime, CodePage::kHookSite, GTAV_PS4_HOOK_ADDR,
                          qword(kGtavPs4PluginHookBytes), qword(patch));
  if (exchanged.cas != GTAV_PS4_PLUGIN_CAS_SWAPPED) {
    char detail[96];
    snprintf(detail, sizeof(detail),
             "GTAV Menu plugin: hook exchange returned %d (observed 0x%016llx)", exchanged.cas,
             (unsigned long long)exchanged.seen);
    log_line(runtime, detail);
    // Nothing was written. If the page did not go back to RX, cleanup retries the protection.
    if (!exchanged.rx_again) runtime->hook_attempted = 1;
    return GTAV_PS4_PLUGIN_ERROR_HOOK_INSTALL;
  }
  runtime->hook_attempted = 1;
  runtime->hook_installed = 1;
  if (!exchanged.rx_again ||
      runtime->ops.read(runtime->ops.context, GTAV_PS4_HOOK_ADDR, observed, sizeof(observed)) !=
          0 ||
      memcmp(observed, patched_site, sizeof(observed)) != 0) {
    return GTAV_PS4_PLUGIN_ERROR_HOOK_INSTALL;
  }
  return 0;
}

// First cleanup step once the menu is quiesced: the relay's absolute jump into PRX code becomes a
// tail jump to gateway+0, the relocated original prologue. The hook site then behaves like the
// unpatched function even if restoring it fails, and the gateway (kept mapped on any failure) is
// the only code it can reach.
int neutralize_relay(GtavPs4PluginRuntime* runtime) {
  if (!runtime->gateway_mapped || !runtime->hook_attempted || runtime->relay_neutral) return 0;
  const uint64_t head_address = runtime->gateway + GTAV_PS4_PLUGIN_RELAY_OFFSET;
  uint8_t relay[GTAV_PS4_PLUGIN_ABSOLUTE_JUMP_BYTES];
  uint8_t passthrough[GTAV_PS4_PLUGIN_RELAY_HEAD_BYTES];
  uint8_t head[GTAV_PS4_PLUGIN_RELAY_HEAD_BYTES];
  absolute_jump((uint64_t)(uintptr_t)&gtav_ps4_frame_hook, relay);
  if (gtav_ps4_plugin_encode_relay_passthrough(runtime->gateway, relay, passthrough) != 0) {
    return GTAV_PS4_PLUGIN_ERROR_STATE;
  }
  if (runtime->ops.read(runtime->ops.context, head_address, head, sizeof(head)) != 0) {
    return GTAV_PS4_PLUGIN_ERROR_RELAY;
  }
  if (memcmp(head, passthrough, sizeof(head)) == 0) {
    runtime->relay_neutral = 1;
    return 0;
  }
  if (memcmp(head, relay, sizeof(head)) != 0) return GTAV_PS4_PLUGIN_ERROR_RELAY;
  const LiveExchange exchanged = exchange_live_qword(runtime, CodePage::kGateway, head_address,
                                                     qword(relay), qword(passthrough));
  if (!exchanged.rx_again) {
    log_line(runtime, "GTAV Menu plugin: gateway page stays writable after the relay passthrough");
  }
  if (!exchanged.holds(qword(passthrough))) return GTAV_PS4_PLUGIN_ERROR_RELAY;
  runtime->relay_neutral = 1;
  return 0;
}

int restore_hook(GtavPs4PluginRuntime* runtime) {
  if (!runtime->hook_attempted) return 0;
  uint8_t gateway_bytes[GTAV_PS4_PLUGIN_GATEWAY_BYTES];
  uint8_t relay[GTAV_PS4_PLUGIN_ABSOLUTE_JUMP_BYTES];
  uint8_t patch[GTAV_PS4_PLUGIN_HOOK_PATCH_BYTES];
  uint8_t patched_site[sizeof(kGtavPs4PluginHookBytes)];
  uint8_t observed[sizeof(kGtavPs4PluginHookBytes)];
  if (encode_site(runtime->gateway, gateway_bytes, relay, patch, patched_site) != 0) {
    return GTAV_PS4_PLUGIN_ERROR_STATE;
  }
  if (runtime->ops.read(runtime->ops.context, GTAV_PS4_HOOK_ADDR, observed, sizeof(observed)) !=
      0) {
    return GTAV_PS4_PLUGIN_ERROR_HOOK_CHANGED;
  }
  if (memcmp(observed, kGtavPs4PluginHookBytes, sizeof(observed)) == 0) {
    if (protect_page(runtime, CodePage::kHookSite,
                     GTAV_PS4_PLUGIN_PROT_READ | GTAV_PS4_PLUGIN_PROT_EXEC) != 0) {
      return GTAV_PS4_PLUGIN_ERROR_HOOK_CHANGED;
    }
    runtime->hook_installed = 0;
    return 0;
  }
  if (memcmp(observed, patched_site, sizeof(observed)) != 0) {
    return GTAV_PS4_PLUGIN_ERROR_HOOK_CHANGED;
  }
  const uint64_t original = qword(kGtavPs4PluginHookBytes);
  // Our patch went back, or the word already holds the original: both leave the site unpatched.
  const LiveExchange exchanged =
      exchange_live_qword(runtime, CodePage::kHookSite, GTAV_PS4_HOOK_ADDR, qword(patch), original);
  if (!exchanged.holds(original) || !exchanged.rx_again) return GTAV_PS4_PLUGIN_ERROR_HOOK_CHANGED;
  if (runtime->ops.read(runtime->ops.context, GTAV_PS4_HOOK_ADDR, observed, sizeof(observed)) !=
          0 ||
      memcmp(observed, kGtavPs4PluginHookBytes, sizeof(observed)) != 0) {
    return GTAV_PS4_PLUGIN_ERROR_HOOK_CHANGED;
  }
  runtime->hook_installed = 0;
  return 0;
}

// Data only: the trampoline's target becomes the verified libScePad entry, so a caller that already
// loaded the slot, or a slot that cannot be put back, reaches the original instead of the PRX hook.
int neutralize_pad_trampoline(GtavPs4PluginRuntime* runtime) {
  if (!runtime->gateway_mapped || !runtime->pad_entry || !runtime->pad_original ||
      runtime->pad_neutral) {
    return 0;
  }
  const uint64_t target = runtime->gateway + GTAV_PS4_PLUGIN_PAD_TARGET_OFFSET;
  const uint64_t hook = (uint64_t)(uintptr_t)&gtav_ps4_pad_read_state_hook;
  const LiveExchange exchanged =
      exchange_live_qword(runtime, CodePage::kGateway, target, hook, runtime->pad_original);
  if (!exchanged.rx_again) {
    log_line(runtime, "GTAV Menu plugin: gateway page stays writable after the pad passthrough");
  }
  if (!exchanged.holds(runtime->pad_original)) return GTAV_PS4_PLUGIN_ERROR_PAD_CHANGED;
  runtime->pad_neutral = 1;
  return 0;
}

// The import slot back from the gateway trampoline to the original libScePad entry, when it holds
// the trampoline. 0 = the slot is not ours (or is back), or an error.
int put_pad_slot_back(GtavPs4PluginRuntime* runtime, uint64_t current) {
  if (current != runtime->pad_entry) return 0;
  if (!runtime->pad_original) return GTAV_PS4_PLUGIN_ERROR_PAD_CHANGED;
  uint64_t seen = 0;
  const int exchanged = runtime->ops.cas64(runtime->ops.context, GTAV_PS4_PAD_READ_STATE_GOT,
                                           runtime->pad_entry, runtime->pad_original, &seen);
  // The game thread may put the entry back itself between the read and the exchange.
  const int restored = exchanged == GTAV_PS4_PLUGIN_CAS_SWAPPED ||
                       (exchanged == GTAV_PS4_PLUGIN_CAS_DIFFERS && seen == runtime->pad_original);
  if (!restored || read_u64(runtime, GTAV_PS4_PAD_READ_STATE_GOT, &current) != 0 ||
      current != runtime->pad_original) {
    return GTAV_PS4_PLUGIN_ERROR_PAD_CHANGED;
  }
  return 0;
}

int restore_pad(GtavPs4PluginRuntime* runtime) {
#if GTAV_PS4_PLUGIN_TOUCHPAD_ENABLED
  // Never published: the game thread had no entry to swap the slot to.
  if (!runtime->pad_entry) return 0;
  if (neutralize_pad_trampoline(runtime) != 0) {
    log_line(runtime, "GTAV Menu plugin: pad trampoline target changed; left alone");
  }
  uint64_t current = 0;
  if (read_u64(runtime, GTAV_PS4_PAD_READ_STATE_GOT, &current) != 0) {
    return GTAV_PS4_PLUGIN_ERROR_PAD_CHANGED;
  }
  if (current == runtime->pad_entry) return put_pad_slot_back(runtime, current);
  if (current != runtime->pad_original &&
      (gtav_ps4_control.pad_hook_state == GTAV_PS4_PAD_HOOK_INSTALLED ||
       gtav_ps4_control.pad_hook_inflight != 0)) {
    return GTAV_PS4_PLUGIN_ERROR_PAD_CHANGED;
  }
#else
  (void)runtime;
#endif
  return 0;
}

// After a clean drain: a frame that was already inside the hook when the stop began may have
// swapped the slot to the trampoline after restore_pad looked (the touchpad lane switched on in
// that frame). Nothing is inside the PRX any more and the trampoline's target is the original, so
// put the slot back before the gateway the trampoline lives in goes away.
int restore_late_pad(GtavPs4PluginRuntime* runtime) {
#if GTAV_PS4_PLUGIN_TOUCHPAD_ENABLED
  if (!runtime->pad_entry) return 0;
  uint64_t current = 0;
  if (read_u64(runtime, GTAV_PS4_PAD_READ_STATE_GOT, &current) != 0) {
    return GTAV_PS4_PLUGIN_ERROR_PAD_CHANGED;
  }
  if (current == (uint64_t)(uintptr_t)&gtav_ps4_pad_read_state_hook) {
    return GTAV_PS4_PLUGIN_ERROR_PAD_CHANGED;
  }
  if (current != runtime->pad_entry) return 0;
  log_line(runtime, "GTAV Menu plugin: pad slot swapped in during the stop; putting it back");
  return put_pad_slot_back(runtime, current);
#else
  (void)runtime;
  return 0;
#endif
}

// 1 while the pad import slot still routes into PRX code.
int pad_references_prx(GtavPs4PluginRuntime* runtime) {
#if GTAV_PS4_PLUGIN_TOUCHPAD_ENABLED
  uint64_t current = 0;
  if (read_u64(runtime, GTAV_PS4_PAD_READ_STATE_GOT, &current) != 0) return 0;
  if (current == (uint64_t)(uintptr_t)&gtav_ps4_pad_read_state_hook) return 1;
  // A slot left on the trampoline is safe only once the trampoline's target is the original.
  return runtime->pad_entry && current == runtime->pad_entry && !runtime->pad_neutral;
#else
  (void)runtime;
  return 0;
#endif
}

// 1 while the pad import slot still holds the gateway trampoline (the gateway must stay mapped).
int pad_references_gateway(GtavPs4PluginRuntime* runtime) {
#if GTAV_PS4_PLUGIN_TOUCHPAD_ENABLED
  uint64_t current = 0;
  if (!runtime->pad_entry) return 0;
  return read_u64(runtime, GTAV_PS4_PAD_READ_STATE_GOT, &current) != 0 ||
         current == runtime->pad_entry;
#else
  (void)runtime;
  return 0;
#endif
}

void log_wait(GtavPs4PluginRuntime* runtime, const char* what, uint32_t polls) {
  uint32_t frame = 0;
  (void)read_u32(runtime, GTAV_PS4_FRAME_COUNT, &frame);
  char message[224];
  snprintf(message, sizeof(message),
           "GTAV Menu plugin: %s after %u ms: frame=%u hook_calls=%llu game_frames=%llu "
           "inflight=%u visible=%u",
           what, (unsigned)(polls * (kPollUsec / 1000u)), (unsigned)frame,
           (unsigned long long)__atomic_load_n(&gtav_ps4_control.hook_calls, __ATOMIC_ACQUIRE),
           (unsigned long long)gtav_ps4_control.game_frames, (unsigned)inflight(),
           (unsigned)__atomic_load_n(&gtav_ps4_control.visible, __ATOMIC_ACQUIRE));
  log_line(runtime, message);
}

// 0 once the game thread finished the shutdown, else GTAV_PS4_PLUGIN_ERROR_SHUTDOWN. The budget
// counts only polls with nothing in flight (see kShutdownPolls). *stalled is set when the budget
// ran out and the game produced no frame for its last second: nothing will run the shutdown until
// the game resumes, so the frame-driven waits after it are skipped. Frames that resume within the
// budget still finish the shutdown cooperatively.
int wait_for_shutdown(GtavPs4PluginRuntime* runtime, int* stalled) {
  *stalled = 0;
  if ((worker_flags() & GTAV_PS4_WORKER_SHUTDOWN_DONE) == 0) {
    __atomic_or_fetch(&gtav_ps4_control.host_flags, GTAV_PS4_HOST_SHUTDOWN_REQUEST,
                      __ATOMIC_ACQ_REL);
  }
  uint32_t last_frame = 0;
  (void)read_u32(runtime, GTAV_PS4_FRAME_COUNT, &last_frame);
  uint32_t idle = 0;
  uint32_t budget = 0;
  uint32_t polls = 0;
  for (; polls < kBusyPolls && budget < kShutdownPolls; ++polls) {
    if ((worker_flags() & GTAV_PS4_WORKER_SHUTDOWN_DONE) != 0) return 0;
    runtime->ops.sleep_us(runtime->ops.context, kPollUsec);
    // A hooked frame still running is our own frame blocking the game thread: not a stall, and
    // the shutdown runs as soon as it returns.
    const bool busy = inflight() != 0;
    uint32_t frame = 0;
    if (read_u32(runtime, GTAV_PS4_FRAME_COUNT, &frame) == 0 && frame != last_frame) {
      last_frame = frame;
      idle = 0;
    } else if (!busy) {
      ++idle;
    }
    if (!busy) ++budget;
    if ((polls + 1u) % kProgressPolls == 0) log_wait(runtime, "waiting for the shutdown", polls);
  }
  if ((worker_flags() & GTAV_PS4_WORKER_SHUTDOWN_DONE) != 0) return 0;
  *stalled = idle >= kFrameStallPolls;
  if (*stalled) log_wait(runtime, "game frames stalled", polls);
  return GTAV_PS4_PLUGIN_ERROR_SHUTDOWN;
}

// Once QUIESCE is set every new call only chains the original, so the menu is quiet when nothing
// is counted in flight for kSettleSamples samples. (The frame hook's `quiesced` field has several
// writers; it stays for the control ABI but is not a proof.)
int wait_quiesced(GtavPs4PluginRuntime* runtime) {
  uint32_t settled = 0;
  for (uint32_t i = 0; i < kQuiescePolls && settled < kSettleSamples; ++i) {
    settled = inflight() == 0 ? settled + 1 : 0;
    if (settled < kSettleSamples) runtime->ops.sleep_us(runtime->ops.context, kPollUsec);
  }
  return settled == kSettleSamples ? 0 : GTAV_PS4_PLUGIN_ERROR_INFLIGHT;
}

// After the passthrough and the restores no new caller can enter PRX code, but a thread that
// already took a jump into it may not have counted itself yet: the frame hook bumps inflight and
// hook_calls only on its first instructions, the pad hook its own pair. Wait out a grace period,
// then require consecutive samples with nobody inside and no call counted since the last sample.
// A thread still counted inside keeps the drain going (up to kBusyPolls): the host unmaps the
// module when the unload returns, so returning under live code is worse than blocking.
int drain_callbacks(GtavPs4PluginRuntime* runtime) {
  uint64_t hook_calls = __atomic_load_n(&gtav_ps4_control.hook_calls, __ATOMIC_ACQUIRE);
  uint64_t pad_calls = __atomic_load_n(&gtav_ps4_control.pad_hook_calls, __ATOMIC_ACQUIRE);
  uint32_t quiet = 0;
  for (uint32_t i = 0; i < kBusyPolls; ++i) {
    runtime->ops.sleep_us(runtime->ops.context, kPollUsec);
    const uint64_t hook_now = __atomic_load_n(&gtav_ps4_control.hook_calls, __ATOMIC_ACQUIRE);
    const uint64_t pad_now = __atomic_load_n(&gtav_ps4_control.pad_hook_calls, __ATOMIC_ACQUIRE);
    const bool inside = inflight() != 0 || !gtav_ps4_pad_hook_idle();
    const bool idle = !inside && hook_now == hook_calls && pad_now == pad_calls;
    hook_calls = hook_now;
    pad_calls = pad_now;
    if (i >= kDrainGraceSamples) {
      quiet = idle ? quiet + 1 : 0;
      if (quiet >= kDrainQuietSamples) return 0;
    }
    // Past the usual 3 s only a thread still counted inside, or a quiet run under way, keeps it
    // going.
    if (i + 1u >= kDrainPolls && !inside && quiet == 0) break;
    if ((i + 1u) % kProgressPolls == 0) log_wait(runtime, "waiting for the frame hook", i);
  }
  return GTAV_PS4_PLUGIN_ERROR_INFLIGHT;
}

int fail_start(GtavPs4PluginRuntime* runtime, int error) {
  const int cleanup_error = gtav_ps4_plugin_runtime_cleanup(runtime);
  runtime->last_error = cleanup_error < 0 ? cleanup_error : error;
  runtime->state = GTAV_PS4_PLUGIN_FAILED;
  log_failure(runtime, runtime->last_error);
  return runtime->last_error;
}

}  // namespace

int gtav_ps4_plugin_encode_hook(uint64_t gateway, uint64_t thunk,
                                uint8_t gateway_bytes[GTAV_PS4_PLUGIN_GATEWAY_BYTES],
                                uint8_t relay[GTAV_PS4_PLUGIN_ABSOLUTE_JUMP_BYTES],
                                uint8_t patch[GTAV_PS4_PLUGIN_HOOK_PATCH_BYTES]) {
  if (!gateway || !thunk || !gateway_bytes || !relay || !patch) return GTAV_PS4_PLUGIN_ERROR_STATE;
  memcpy(gateway_bytes, kGtavPs4PluginHookBytes, sizeof(kGtavPs4PluginHookBytes));
  const uint32_t lea = GTAV_PS4_PLUGIN_LEA_OFFSET;
  if (gateway_bytes[lea] != 0x48 || gateway_bytes[lea + 1] != 0x8d ||
      gateway_bytes[lea + 2] != 0x05) {
    return GTAV_PS4_PLUGIN_ERROR_HOOK_IDENTITY;
  }
  int32_t original_displacement = 0;
  memcpy(&original_displacement, gateway_bytes + lea + 3, sizeof(original_displacement));
  if ((uint64_t)(GTAV_PS4_CONTINUATION + original_displacement) != GTAV_PS4_PLAYER_GLOBAL) {
    return GTAV_PS4_PLUGIN_ERROR_HOOK_IDENTITY;
  }
  const int64_t displacement = (int64_t)GTAV_PS4_PLAYER_GLOBAL - (int64_t)(gateway + lea + 7);
  if (displacement < INT32_MIN || displacement > INT32_MAX) return GTAV_PS4_PLUGIN_ERROR_GATEWAY;
  const int32_t relocated = (int32_t)displacement;
  memcpy(gateway_bytes + lea + 3, &relocated, sizeof(relocated));
  absolute_jump(GTAV_PS4_CONTINUATION, gateway_bytes + sizeof(kGtavPs4PluginHookBytes));
  absolute_jump(thunk, relay);

  const int64_t hook_displacement =
      (int64_t)(gateway + GTAV_PS4_PLUGIN_RELAY_OFFSET) - (int64_t)(GTAV_PS4_HOOK_ADDR + 5);
  if (hook_displacement < INT32_MIN || hook_displacement > INT32_MAX) {
    return GTAV_PS4_PLUGIN_ERROR_GATEWAY;
  }
  patch[0] = 0xe9;
  const int32_t relative = (int32_t)hook_displacement;
  memcpy(patch + 1, &relative, sizeof(relative));
  memcpy(patch + 5, kGtavPs4PluginHookBytes + 5, 3);
  return 0;
}

int gtav_ps4_plugin_encode_relay_passthrough(
    uint64_t gateway, const uint8_t relay_head[GTAV_PS4_PLUGIN_RELAY_HEAD_BYTES],
    uint8_t out[GTAV_PS4_PLUGIN_RELAY_HEAD_BYTES]) {
  if (!gateway || !relay_head || !out) return GTAV_PS4_PLUGIN_ERROR_STATE;
  // One aligned exchange must cover the whole jump.
  if (((gateway + GTAV_PS4_PLUGIN_RELAY_OFFSET) & 7u) != 0) return GTAV_PS4_PLUGIN_ERROR_GATEWAY;
  const int64_t displacement =
      (int64_t)gateway - (int64_t)(gateway + GTAV_PS4_PLUGIN_RELAY_OFFSET + 5);
  const int32_t relative = (int32_t)displacement;
  uint8_t head[GTAV_PS4_PLUGIN_RELAY_HEAD_BYTES];
  head[0] = 0xe9;
  memcpy(head + 1, &relative, sizeof(relative));
  memcpy(head + 5, relay_head + 5, 3);
  memcpy(out, head, sizeof(head));
  return 0;
}

int gtav_ps4_plugin_encode_pad_trampoline(uint64_t target,
                                          uint8_t out[GTAV_PS4_PLUGIN_ABSOLUTE_JUMP_BYTES]) {
  if (!target || !out) return GTAV_PS4_PLUGIN_ERROR_STATE;
  absolute_jump(target, out);
  return 0;
}

void gtav_ps4_plugin_runtime_init(GtavPs4PluginRuntime* runtime, const GtavPs4PluginOps* ops) {
  memset(runtime, 0, sizeof(*runtime));
  runtime->ops = *ops;
  runtime->state = GTAV_PS4_PLUGIN_IDLE;
  runtime->nonce = kSessionTag;
}

int gtav_ps4_plugin_runtime_identity(GtavPs4PluginRuntime* runtime) {
  return identity(runtime);
}

int gtav_ps4_plugin_runtime_story_mode(GtavPs4PluginRuntime* runtime) {
  return story_mode_ready(runtime) ? 1 : 0;
}

int gtav_ps4_plugin_runtime_try_start(GtavPs4PluginRuntime* runtime) {
  if (runtime->state == GTAV_PS4_PLUGIN_IDLE) runtime->state = GTAV_PS4_PLUGIN_WAITING;
  if (runtime->state != GTAV_PS4_PLUGIN_WAITING) return GTAV_PS4_PLUGIN_ERROR_STATE;

  const int ready = preflight(runtime);
  if (ready != 0) return ready;

  // The control block starts from its load state (control.c), whatever an earlier run of this
  // image left in it.
  gtav_ps4_runtime_load();
  runtime->game_initialized = 1;
  runtime->pid = runtime->ops.pid(runtime->ops.context);

  uint8_t patch[GTAV_PS4_PLUGIN_HOOK_PATCH_BYTES];
  uint8_t patched_site[sizeof(kGtavPs4PluginHookBytes)];
  int error = map_and_fill_gateway(runtime, patch, patched_site);
  if (error != 0) return fail_start(runtime, error);
  publish_control(runtime, (uint64_t)(uintptr_t)&gtav_ps4_frame_hook);
  error = install_hook(runtime, patch, patched_site);
  if (error != 0) return fail_start(runtime, error);

  runtime->activation_frame = gtav_ps4_control.game_frames;
  runtime->state = GTAV_PS4_PLUGIN_ACTIVE;
  log_line(runtime, "GTAV Menu plugin: hook installed; waiting for native activation");
  return 0;
}

int gtav_ps4_plugin_runtime_activated(GtavPs4PluginRuntime* runtime) {
  if (runtime->state != GTAV_PS4_PLUGIN_ACTIVE) return GTAV_PS4_PLUGIN_ERROR_STATE;
  const uint32_t worker = worker_flags();
  if ((worker & GTAV_PS4_WORKER_SELF_QUIESCE) != 0 &&
      (worker & GTAV_PS4_WORKER_SHUTDOWN_DONE) == 0) {
    return GTAV_PS4_PLUGIN_ERROR_ACTIVATION;
  }
  if (gtav_ps4_control.native_ready != GTAV_PS4_NATIVE_ALL || !gtav_ps4_control.captured_ped ||
      gtav_ps4_control.getter_frame != gtav_ps4_control.direct_frame ||
      gtav_ps4_control.getter_timer != gtav_ps4_control.direct_timer) {
    return 0;
  }
  return gtav_ps4_control.game_frames > runtime->activation_frame + 1 &&
         gtav_ps4_control.draw_lists != 0;
}

uint64_t gtav_ps4_plugin_runtime_hooked_frames(const GtavPs4PluginRuntime* runtime) {
  if (runtime->state != GTAV_PS4_PLUGIN_ACTIVE) return 0;
  const uint64_t frames = gtav_ps4_control.game_frames;
  return frames > runtime->activation_frame ? frames - runtime->activation_frame : 0;
}

int gtav_ps4_plugin_runtime_grant_storage(GtavPs4PluginRuntime* runtime) {
  if (runtime->state != GTAV_PS4_PLUGIN_ACTIVE || !storage_ready()) return 0;
  __atomic_or_fetch(&gtav_ps4_control.host_flags, GTAV_PS4_HOST_STORAGE_READY, __ATOMIC_ACQ_REL);
  return 1;
}

int gtav_ps4_plugin_runtime_features_reset(const GtavPs4PluginRuntime* runtime) {
  (void)runtime;
  return (worker_flags() & GTAV_PS4_WORKER_FEATURES_RESET) != 0;
}

int gtav_ps4_plugin_runtime_menu_stop_requested(const GtavPs4PluginRuntime* runtime) {
  (void)runtime;
  return (worker_flags() & (GTAV_PS4_WORKER_MENU_STOP | GTAV_PS4_WORKER_SHUTDOWN_DONE)) != 0;
}

int gtav_ps4_plugin_runtime_cleanup(GtavPs4PluginRuntime* runtime) {
  if (runtime->state == GTAV_PS4_PLUGIN_STOPPED) return 0;
  if (runtime->state == GTAV_PS4_PLUGIN_FAILED) return runtime->last_error;

  // Order: cooperative shutdown, QUIESCE, quiesced, relay passthrough, hook, pad, drain, the pad
  // slot again, unmap.
  int result = 0;
  int stalled = 0;
  runtime->frames_stalled = 0;
  runtime->shutdown_unfinished = 0;
  const uint64_t started_us = runtime->ops.now_us(runtime->ops.context);
  if (runtime->game_initialized && runtime->hook_installed) {
    __atomic_store_n(&gtav_ps4_control.state, GTAV_PS4_STATE_STOPPING, __ATOMIC_RELEASE);
    result = wait_for_shutdown(runtime, &stalled);
    runtime->shutdown_unfinished = result != 0;
    __atomic_or_fetch(&gtav_ps4_control.host_flags, GTAV_PS4_HOST_QUIESCE, __ATOMIC_ACQ_REL);
    if (stalled) {
      log_line(runtime, "GTAV Menu plugin: game frames stalled; skipping the shutdown waits");
    } else {
      const int settle_result = wait_quiesced(runtime);
      if (result == 0) result = settle_result;
    }
  }

  const int relay_result = neutralize_relay(runtime);
  const int hook_result = restore_hook(runtime);
  int pad_result = restore_pad(runtime);
  const int drain_result =
      runtime->game_initialized && runtime->hook_attempted ? drain_callbacks(runtime) : 0;
  if (pad_result == 0 && drain_result == 0) pad_result = restore_late_pad(runtime);

  if (result == 0) result = relay_result;
  if (result == 0) result = hook_result;
  if (result == 0) result = pad_result;
  if (result == 0) result = drain_result;

  // Unsafe to unload: the hook site still reaches PRX code (neither the relay nor the site could
  // be put back), the pad slot still does, or a thread is still inside the PRX (drain failed).
  runtime->unload_unsafe =
      (relay_result != 0 && hook_result != 0) || pad_references_prx(runtime) || drain_result != 0;

  // Any failure keeps the gateway: a site, relay or pad slot that still reaches it must find code
  // there. So does a stall: a thread the stall froze inside the relocated prologue or the relay
  // was never counted, so the drain proves nothing; the next instance in this process reclaims it.
  runtime->frames_stalled = stalled;
  if (runtime->gateway_mapped && relay_result == 0 && hook_result == 0 && pad_result == 0 &&
      drain_result == 0 && !runtime->hook_installed && !stalled &&
      !pad_references_gateway(runtime)) {
    if (runtime->ops.unmap(runtime->ops.context, runtime->gateway, kGatewaySpan) != 0) {
      if (result == 0) result = GTAV_PS4_PLUGIN_ERROR_UNMAP;
    } else {
      runtime->gateway_mapped = 0;
    }
  } else if (runtime->gateway_mapped && stalled) {
    log_line(runtime,
             "GTAV Menu plugin: gateway kept mapped (frames stalled); the next load in this game "
             "reclaims it");
  }

  if (runtime->game_initialized) {
    // A failed stop leaves the frame hook chaining the original only (QUIESCE stays set).
    __atomic_store_n(&gtav_ps4_control.host_flags,
                     result < 0 ? (uint32_t)GTAV_PS4_HOST_QUIESCE : 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gtav_ps4_control.state,
                     result < 0 ? GTAV_PS4_STATE_ERROR : GTAV_PS4_STATE_STOPPING, __ATOMIC_RELEASE);
  }
  {
    char message[96];
    snprintf(message, sizeof(message), "GTAV Menu plugin: stop took %u ms",
             (unsigned)((runtime->ops.now_us(runtime->ops.context) - started_us) / 1000u));
    log_line(runtime, message);
  }
  runtime->last_error = result;
  runtime->cleanup_result = result;
  runtime->state = result < 0 ? GTAV_PS4_PLUGIN_FAILED : GTAV_PS4_PLUGIN_STOPPED;
  if (result < 0) {
    log_failure(runtime, result);
  } else {
    log_line(runtime, "GTAV Menu plugin: cleanup complete");
  }
  return result;
}

const char* gtav_ps4_plugin_error_string(int error) {
  switch (error) {
    case 0:
      return "ok";
    case GTAV_PS4_PLUGIN_ERROR_MAPPING:
      return "executable/data mapping mismatch";
    case GTAV_PS4_PLUGIN_ERROR_VERSION:
      return "game version fingerprint mismatch";
    case GTAV_PS4_PLUGIN_ERROR_HOOK_IDENTITY:
      return "PLAYER_PED_ID prologue mismatch";
    case GTAV_PS4_PLUGIN_ERROR_NATIVE:
      return "native fingerprint mismatch";
    case GTAV_PS4_PLUGIN_ERROR_GATEWAY:
      return "near gateway allocation/relocation failed";
    case GTAV_PS4_PLUGIN_ERROR_HOOK_INSTALL:
      return "hook installation/read-back failed";
    case GTAV_PS4_PLUGIN_ERROR_ACTIVATION:
      return "native activation failed";
    case GTAV_PS4_PLUGIN_ERROR_SHUTDOWN:
      return "cooperative shutdown timed out";
    case GTAV_PS4_PLUGIN_ERROR_HOOK_CHANGED:
      return "hook bytes changed; blind restore refused";
    case GTAV_PS4_PLUGIN_ERROR_PAD_CHANGED:
      return "pad import ownership changed; blind restore refused";
    case GTAV_PS4_PLUGIN_ERROR_INFLIGHT:
      return "callback did not become idle";
    case GTAV_PS4_PLUGIN_ERROR_UNMAP:
      return "gateway unmap failed";
    case GTAV_PS4_PLUGIN_ERROR_STATE:
      return "invalid lifecycle state";
    case GTAV_PS4_PLUGIN_ERROR_RELAY:
      return "relay changed; passthrough refused";
    default:
      return "unknown PS4 plugin runtime error";
  }
}
