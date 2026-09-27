// Standalone GTAV-Menu payload loader.
//
// Launched as a self-contained payload ELF by a generic PS5 payload loader (the
// ps5-payload-sdk PS5_DEPLOY server on port 9021) -- no etaHEN daemon, no
// .plugin, no libhijacker. The SDK CRT binds kernel R/W before main(); from
// there this uses the enabler-agnostic process-control backend (proc_backend.h)
// to reach the running game. Production waits for the configured player-world
// readiness anchor, maps the menu worker ELF into GTA, and lets the first frame-hook
// fire start its worker thread. The explicit smoke profile performs no injection.

#include "gtavmenu/abi.h"
#include "gtavmenu/build_pin.h"
#include "gtavmenu/cave_bootstrap.h"
#include "gtavmenu/command_mailbox.h"
#include "gtavmenu/daemon_control.h"
#include "gtavmenu/daemon_lifecycle.h"
#include "gtavmenu/elf_inject.h"
#include "gtavmenu/inject_lock.h"
#include "gtavmenu/loader_pins_generated.h"
#include "gtavmenu/log.h"
#include "gtavmenu/notify.h"
#include "gtavmenu/patch_broker.h"
#include "gtavmenu/proc_backend.h"
#include "gtavmenu/render_phase_discovery.h"
#include "gtavmenu/runtime_config.h"
#include "gtavmenu/supervisor_lifecycle.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef GTAV_PAYLOAD_TARGET_TITLE_ID
#define GTAV_PAYLOAD_TARGET_TITLE_ID "PPSA04264"
#endif

// Production 01.010 renderer bootstrap. The loader performs read-only exact task discovery, then
// preconfigures the mapped worker's in-process CAS installer before committing the existing frame
// hook. Default off for inert/legacy builds; menu-live/watch/OnionHEN enable it with the matching
// phase-render worker gates.
#ifndef GTAV_LOADER_INSTALL_RENDER_PHASE
#define GTAV_LOADER_INSTALL_RENDER_PHASE 0
#endif

// Phase 1 wait-for-game (build-gated): instead of giving up on the first miss,
// poll until GTA becomes the foreground big app, then inject. This lets the loader
// be deployed BEFORE GTA launches -- it parks on the console and injects itself
// once the separately configured load-readiness gate passes. 0 = off (classic one-shot loader:
// find once, bail if absent).
#ifndef GTAV_PAYLOAD_WAIT_FOR_GAME
#define GTAV_PAYLOAD_WAIT_FOR_GAME 0
#endif

// Seconds to wait before giving up (0 = wait indefinitely). Discovery is the cheap
// foreground big-app query (sceSystemService, no ptrace), so a long wait costs
// almost nothing; the cap only exists so a loader deployed when GTA never launches
// eventually exits instead of lingering forever with kernel R/W bound.
#ifndef GTAV_PAYLOAD_WAIT_TIMEOUT_SEC
#define GTAV_PAYLOAD_WAIT_TIMEOUT_SEC 0
#endif

// Poll cadence for the wait loop, in microseconds.
#ifndef GTAV_PAYLOAD_WAIT_POLL_USEC
#define GTAV_PAYLOAD_WAIT_POLL_USEC 1000000
#endif

// Persistent (dormant daemon) injection: instead of injecting once and exiting,
// stay resident and re-inject every time GTA relaunches, so one deploy covers every
// future GTA instance the operator launches. Re-arming keys on the game INSTANCE (pid + app-id
// token, gtav_daemon_same_instance): after a successful inject the loader idles until
// a DIFFERENT instance appears -- a new pid, OR a recycled pid now running a fresh game
// (a pid-only check would miss the latter and never re-inject) -- then injects into it.
// Implies wait-for-game (a daemon that gives up the moment GTA isn't foreground would
// be pointless). The daemon holds a single-instance lock and honours a stop sentinel
// (daemon_lifecycle.h) so `menu-ctl daemon-stop` can retire it. 0 = off (one-shot:
// inject the first time GTA is found, then exit -- the default).
#ifndef GTAV_PAYLOAD_PERSISTENT
#define GTAV_PAYLOAD_PERSISTENT 0
#endif

// Managed delivery adapters run this loader as a separately launched helper and require a live
// supervisor lease; losing it feeds the same cooperative retirement path as daemon.stop or SIGTERM.
#ifndef GTAV_MANAGED_RUNTIME
#define GTAV_MANAGED_RUNTIME 0
#endif
#if GTAV_MANAGED_RUNTIME && (!GTAV_MENU_PAYLOAD_INJECT || !GTAV_PAYLOAD_PERSISTENT)
#error "GTAV_MANAGED_RUNTIME requires the persistent injection runtime"
#endif

// Backoff before retrying an inject that failed (e.g. injected too early), in usec.
#ifndef GTAV_PAYLOAD_RETRY_BACKOFF_USEC
#define GTAV_PAYLOAD_RETRY_BACKOFF_USEC 5000000
#endif

// Player-world/load readiness gate (build-gated; legacy macro name SP_READY). The foreground
// big-app query cannot tell GTA's boot/update/story-select screens from a live world, and a full
// inject at the boot screen is destructive (hardware-confirmed: it crashes the game). When this
// gate is on, the loader polls a known game-memory anchor before injecting. The canonical non-null
// player-ped predicate means that a playable world exists. Game-mode selection is deliberately
// outside this read-only probe. Supply its target-specific values via the PAYLOAD_LOADER_SP_READY_*
// make vars (menu-ctl 'watch' threads them through). 0 = off (no gate).
#ifndef GTAV_PAYLOAD_SP_READY
#define GTAV_PAYLOAD_SP_READY 0
#endif
// Absolute address of the readiness anchor in the game process.
#ifndef GTAV_PAYLOAD_SP_READY_ADDR
#define GTAV_PAYLOAD_SP_READY_ADDR 0
#endif
// Bytes to read at the anchor (1/2/4/8).
#ifndef GTAV_PAYLOAD_SP_READY_SIZE
#define GTAV_PAYLOAD_SP_READY_SIZE 8
#endif
#if GTAV_PAYLOAD_SP_READY_SIZE != 1 && GTAV_PAYLOAD_SP_READY_SIZE != 2 && \
    GTAV_PAYLOAD_SP_READY_SIZE != 4 && GTAV_PAYLOAD_SP_READY_SIZE != 8
#error "GTAV_PAYLOAD_SP_READY_SIZE must be 1, 2, 4, or 8"
#endif
// Mask applied to the read value before comparing.
#ifndef GTAV_PAYLOAD_SP_READY_MASK
#define GTAV_PAYLOAD_SP_READY_MASK 0xFFFFFFFFFFFFFFFFULL
#endif
// Expected value: ready when (read & MASK) compares equal (MODE 0) / not-equal
// (MODE 1) to (VALUE & MASK). MODE 1 with VALUE 0 means "ready when the anchor is
// non-zero" (e.g. a pointer that is null until the player world is live).
#ifndef GTAV_PAYLOAD_SP_READY_VALUE
#define GTAV_PAYLOAD_SP_READY_VALUE 0
#endif
#ifndef GTAV_PAYLOAD_SP_READY_MODE
#define GTAV_PAYLOAD_SP_READY_MODE 0
#endif
// Give up after this many seconds of polling (0 = wait indefinitely).
#ifndef GTAV_PAYLOAD_SP_READY_TIMEOUT_SEC
#define GTAV_PAYLOAD_SP_READY_TIMEOUT_SEC 0
#endif
// Extra settle delay after the anchor first reports ready, before injecting (usec).
// Lets the scene finish coming up so the inject lands on a fully-settled process.
#ifndef GTAV_PAYLOAD_SP_READY_SETTLE_USEC
#define GTAV_PAYLOAD_SP_READY_SETTLE_USEC 0
#endif
// One-level pointer dereference. When 1, the compared value is read from
// *(uint64_t*)SP_READY_ADDR + SP_READY_DEREF_OFFSET instead of directly at the anchor
// -- so the gate can follow a pointer chain. The canonical use is the local player ped:
// PLAYER_PED_ID reads ped = *(*(0x51cf258) + 8), and ped != 0 means the player exists
// in the world (player control). A null base pointer reads as "not ready" (the manager singleton
// is not up yet).
#ifndef GTAV_PAYLOAD_SP_READY_DEREF
#define GTAV_PAYLOAD_SP_READY_DEREF 0
#endif
#ifndef GTAV_PAYLOAD_SP_READY_DEREF_OFFSET
#define GTAV_PAYLOAD_SP_READY_DEREF_OFFSET 0
#endif
// Debounce: number of CONSECUTIVE ready anchor reads required before injecting. A single-shot
// transient (a half-initialised / stale pointer flickering non-null during a load transition)
// must not fire an inject -- injecting mid-load crashes GTA, which is the exact failure this gate
// exists to prevent. At genuine player control the anchor is stably ready, so a few extra
// confirmations cost only a couple of seconds. 1 = legacy single-read behaviour.
#ifndef GTAV_PAYLOAD_SP_READY_CONFIRMATIONS
#define GTAV_PAYLOAD_SP_READY_CONFIRMATIONS 1
#endif

// Gentle SP-ready polling (build-gated, default off). Each poll PT_ATTACH-stops the WHOLE game
// for the read; the proven 1s cadence freezes GTA ~once/second for the entire boot+load window.
// When on, the sp-ready poll uses a slower base interval that backs off toward a cap while the
// game is not yet ready (far fewer freezes during a long load), then snaps back to the fast
// WAIT_POLL cadence the instant the player ped appears, so the confirmation debounce still resolves
// in a couple of seconds. This changes ONLY the not-ready polling cadence, never the readiness
// predicate. Opt in with `menu-ctl watch --gentle-poll` to A/B it on hardware. 0 = off (1s
// cadence).
#ifndef GTAV_PAYLOAD_SP_READY_GENTLE
#define GTAV_PAYLOAD_SP_READY_GENTLE 0
#endif
// Base sp-ready poll interval when gentle is on (usec). Falls back to the plain WAIT cadence off.
#ifndef GTAV_PAYLOAD_SP_READY_POLL_USEC
#define GTAV_PAYLOAD_SP_READY_POLL_USEC 2000000
#endif
// Backoff cap for the gentle sp-ready poll (usec): the interval grows by the base each not-ready
// poll until it reaches this ceiling.
#ifndef GTAV_PAYLOAD_SP_READY_POLL_MAX_USEC
#define GTAV_PAYLOAD_SP_READY_POLL_MAX_USEC 5000000
#endif

// Double-inject guard. Before mapping the worker, claim a per-pid lock file so two
// loaders (e.g. a stray parked instance racing the same launch, or a re-run while the
// menu is already live in that pid) cannot stack two workers + two hooks in one GTA
// process. Keyed by pid, so a relaunched GTA (new pid) always injects fresh. 1 = on
// (default); set 0 (menu-ctl --force) to deliberately re-inject the same running pid.
#ifndef GTAV_PAYLOAD_INJECT_GUARD
#define GTAV_PAYLOAD_INJECT_GUARD 1
#endif

// Executable (.text) live-address window of the target, from the target manifest's
// liveMapping. Every privileged .text protect/write (version check, frame-hook broker
// install, drain-slot pre-protect) is bounds-checked against this range: a kernel R/W to a
// wildly-wrong address (e.g. a stale pin after a game update) can fault in a kernel context
// and take the whole console down, so an out-of-range target fails closed instead. 0/0 = no
// range configured -> the guard is skipped (backward-compatible with a bare build).
#ifndef GTAV_LOADER_TEXT_LIVE_START
#define GTAV_LOADER_TEXT_LIVE_START 0
#endif
#ifndef GTAV_LOADER_TEXT_LIVE_END
#define GTAV_LOADER_TEXT_LIVE_END 0
#endif

// Build-gated real menu injection: map the feature-menu ELF into GTA and start its
// worker thread via the SDK-native injector. 0 = off (inert loader / dry-run hook).
#ifndef GTAV_MENU_PAYLOAD_INJECT
#define GTAV_MENU_PAYLOAD_INJECT 0
#endif

// Ptrace-free bootstrap used by the production inject route.
#ifndef GTAV_LOADER_CAVE_BOOTSTRAP
#define GTAV_LOADER_CAVE_BOOTSTRAP 0
#endif
#ifndef GTAV_LOADER_CAVE_ADDR
#define GTAV_LOADER_CAVE_ADDR 0
#endif
#ifndef GTAV_LOADER_CAVE_ALLOC
#define GTAV_LOADER_CAVE_ALLOC 0x200000
#endif
#ifndef GTAV_LOADER_CAVE_TIMEOUT_MS
#define GTAV_LOADER_CAVE_TIMEOUT_MS 5000
#endif
#if GTAV_LOADER_CAVE_BOOTSTRAP && !GTAV_LOADER_CAVE_ADDR
#error "PAYLOAD_LOADER_CAVE_BOOTSTRAP needs PAYLOAD_LOADER_CAVE_ADDR (the code cave address)"
#endif

#define GTAV_LOADER_TARGET_TRANSACTION GTAV_MENU_PAYLOAD_INJECT

// The real injection over the ptrace-free route: the cave bootstrap supplies the memory the worker
// is mapped into (replacing alloc_exec's remote mmap), and the worker starts its own thread from
// the first frame-hook fire (replacing start_thread's injected call). Those were the only two
// remaining PT_ATTACH sites in an inject, so with this on the whole lane never stops the game.
//
// Both safety gates are mandatory here rather than advisory. Without NOSTOP_STRICT a transfer the
// kernel path cannot serve falls back to ptrace, which is precisely the damage this lane exists to
// avoid, and the retries hide it. Without VERIFY_WRITES an image write to memory with no backing
// reports success while doing nothing, and the hook would then jump into a region of zeros.
#ifndef GTAV_LOADER_CAVE_INJECT
#define GTAV_LOADER_CAVE_INJECT 0
#endif
#if GTAV_LOADER_CAVE_INJECT
#if !GTAV_LOADER_CAVE_BOOTSTRAP
#error "PAYLOAD_LOADER_CAVE_INJECT needs PAYLOAD_LOADER_CAVE_BOOTSTRAP=1 (it supplies the memory)"
#endif
#if !GTAV_PROC_NOSTOP_STRICT
#error "PAYLOAD_LOADER_CAVE_INJECT needs PAYLOAD_LOADER_NOSTOP_STRICT=1 (no silent ptrace fallback)"
#endif
#if !GTAV_ELF_INJECT_VERIFY_WRITES
#error "PAYLOAD_LOADER_CAVE_INJECT needs PAYLOAD_LOADER_VERIFY_WRITES=1 (a write must be read back)"
#endif
#endif

// Read the build-pin signature without stopping the game. The ptrace read path PT_ATTACHes (and
// resumes) every thread in GTA per call; the stage ladder showed a handful of those cycles is
// enough to jam the streaming I/O ring for good, while the same reads over the debug-memory path
// disturb nothing (docs/ptrace-free-injection.md). mdbg also ignores page protections, so the
// probe's .text protect becomes unnecessary too. 0 = the original ptrace+protect probe.
#ifndef GTAV_LOADER_PROBE_NOSTOP
#define GTAV_LOADER_PROBE_NOSTOP 0
#endif

// Production never rewrites GTA's credentials or root/jail directories. The loader's kernel
// binding performs copies/protection changes and the bootstrap executes mmap inside GTA.
#if GTAV_LOADER_TARGET_TRANSACTION &&                                                           \
    (!GTAV_LOADER_CAVE_INJECT || !GTAV_LOADER_PROBE_NOSTOP || !GTAV_PROC_NOSTOP_IO ||           \
     !GTAV_PROC_NOSTOP_STRICT || !GTAV_ELF_INJECT_VERIFY_WRITES || !GTAV_MENU_PAYLOAD_INJECT || \
     !GTAV_LOADER_VERIFY_VERSION)
#error "production injection requires strict verified cave injection and exact build binding"
#endif

// Where menu-ctl uploads the feature-menu ELF the loader injects.
#ifndef GTAV_MENU_INJECT_ELF_PATH
#define GTAV_MENU_INJECT_ELF_PATH "/data/GTAVMenu/gtav-menu-feature-menu.elf"
#endif

#ifndef GTAV_MENU_EMBEDDED_WORKER
#define GTAV_MENU_EMBEDDED_WORKER 0
#endif

#if GTAV_MENU_EMBEDDED_WORKER
extern const uint8_t gtav_embedded_worker_start[];
extern const uint8_t gtav_embedded_worker_end[];
#endif

// Install the frame-hook patch broker the injected menu publishes (EXTERNAL_INSTALL
// build). The loader reads the broker from the injected image, makes the target
// native's .text page writable via kernel R/W, and writes the prologue jump -- so the
// drain fires on every RUNTIME call of that native. This catches the cached-pointer
// native call path that the registration-table slot swap cannot (scripts resolve a
// native handler once at load and call the cached pointer, bypassing the table). The
// in-process write would fault on PS5 execute-only .text; doing it from the loader via
// kernel R/W is what makes it safe. 0 = off.
#ifndef GTAV_MENU_INSTALL_PATCH_BROKER
#define GTAV_MENU_INSTALL_PATCH_BROKER 0
#endif

#if GTAV_LOADER_INSTALL_RENDER_PHASE && \
    (!GTAV_MENU_PAYLOAD_INJECT || !GTAV_MENU_INSTALL_PATCH_BROKER || !GTAV_LOADER_VERIFY_VERSION)
#error "render-phase auto-install requires injected worker, broker, and exact build verification"
#endif

#if GTAV_MENU_PAYLOAD_INJECT && GTAV_MENU_INSTALL_PATCH_BROKER && !GTAV_PAYLOAD_INJECT_GUARD
#error "production broker injection requires GTAV_PAYLOAD_INJECT_GUARD=1"
#endif

// Loader-owned broker contract. A staged worker may publish the request, but it cannot choose what
// privileged eboot address/bytes the loader will patch. Production make targets override every
// value below with the pinned PLAYER_PED_ID tuple; zero/default values fail validation.
#ifndef GTAV_LOADER_BROKER_TARGET
#define GTAV_LOADER_BROKER_TARGET 0ull
#endif
#ifndef GTAV_LOADER_BROKER_CONTINUATION
#define GTAV_LOADER_BROKER_CONTINUATION 0ull
#endif
#ifndef GTAV_LOADER_BROKER_PATCH_LEN
#define GTAV_LOADER_BROKER_PATCH_LEN 0u
#endif
#ifndef GTAV_LOADER_BROKER_STOLEN_LEN
#define GTAV_LOADER_BROKER_STOLEN_LEN 0u
#endif
#ifndef GTAV_LOADER_BROKER_EXPECTED_BYTES
#define GTAV_LOADER_BROKER_EXPECTED_BYTES 0x00
#endif

#if GTAV_MENU_PAYLOAD_INJECT && GTAV_PAYLOAD_PERSISTENT
// Defined with the daemon lifecycle helpers below. Forward declarations let long-running inject
// handshakes use the same bounded, stop-aware wait as the outer persistent loop.
static int daemon_should_stop(void);
static void daemon_lock_refresh(void);
static int daemon_wait_interruptible(unsigned long wait_us);
#endif

#if GTAV_LOADER_TARGET_TRANSACTION && (GTAV_MENU_INSTALL_PATCH_BROKER || GTAV_PAYLOAD_SP_READY)
// Cheap, non-ptrace liveness probe: is `pid` still the foreground game instance? 1 = live (the
// title is running and still owns this pid), 0 = the pid is gone or recycled to a different app.
// Uses the foreground big-app query (gtav_proc_find_game), NOT ptrace, so it is safe to call
// against a target that may be dying or already gone -- unlike the kernel R/W it guards. The SP
// gate and the broker .text write both consult it so the loader never ptraces / kernel-writes a
// process that is no longer our game.
static int loader_target_is_live(int pid) {
  int cur = -1;
  if (gtav_proc_find_game(GTAV_PAYLOAD_TARGET_TITLE_ID, &cur) != 0) {
    return 0;  // the title is not running at all -> the prior pid is gone
  }
  return cur == pid ? 1 : 0;  // same pid still the title -> live; different -> recycled
}
#endif

#if GTAV_LOADER_TARGET_TRANSACTION
// Bind every multi-stage inject transaction to the app-id captured for this exact launch. A raw
// pid can be recycled while readiness polling or mapping is in progress; app-id changes on every
// launch. Unknown identity is fail-closed because privileged writes cannot safely target a pid
// whose instance cannot be proven.
static int loader_instance_token_matches(int pid, uint64_t expected_token, const char* stage) {
  uint64_t current_token = gtav_proc_app_id(pid);
  if (expected_token == 0 || current_token == 0 || current_token != expected_token) {
    gtav_logf("inject: instance changed/unknown at %s pid=%d token=0x%llx->0x%llx", stage, pid,
              (unsigned long long)expected_token, (unsigned long long)current_token);
    return 0;
  }
  return 1;
}
#endif

#if GTAV_LOADER_TARGET_TRANSACTION && (GTAV_LOADER_VERIFY_VERSION || GTAV_MENU_INSTALL_PATCH_BROKER)
// Refuse a privileged .text protect/write whose [addr, addr+len) is not fully inside the
// target's known executable live range. The bounds come from the build pin selected for the
// running target (pin->text_live_*, from the manifest liveMapping); when no pin is bound this
// falls back to the compile-time GTAV_LOADER_TEXT_LIVE_* defaults (bare/research builds).
// A kernel R/W to a wildly-wrong address -- e.g. a stale pin after a silent game update -- can
// fault in a kernel context and take the whole console down, so bounds-check first and fail
// closed. Returns 1 if the operation is in range (or no range was configured, i.e. a bare build),
// 0 if it must be refused. Read-only probes are NOT gated -- only the kernel-write path.
static int loader_text_write_ok(const GtavBuildPin* pin, uintptr_t addr, size_t len) {
  const uintptr_t lo =
      pin ? (uintptr_t)pin->text_live_start : (uintptr_t)(GTAV_LOADER_TEXT_LIVE_START);
  const uintptr_t hi = pin ? (uintptr_t)pin->text_live_end : (uintptr_t)(GTAV_LOADER_TEXT_LIVE_END);
  if (lo == 0 && hi == 0) {
    return 1;  // no range configured -> guard disabled (backward-compatible bare build)
  }
  if (hi <= lo || len == 0 || len > (size_t)(hi - lo)) {
    return 0;
  }
  return (addr >= lo && addr <= hi - len) ? 1 : 0;  // [addr, addr+len) subset of [lo, hi)
}
#endif

#if GTAV_MENU_PAYLOAD_INJECT && GTAV_MENU_INSTALL_PATCH_BROKER
static int loader_image_contains(uintptr_t base, size_t image_size, uintptr_t addr, size_t len) {
  if (image_size == 0 || len == 0 || base > UINTPTR_MAX - image_size) return 0;
  if (addr < base || addr > base + image_size) return 0;
  return len <= (size_t)((base + image_size) - addr);
}

// Publish the detail fields first and ERROR state last, so a reader that observes ERROR also sees
// its matching code/note. These writes are best-effort when the target itself is disappearing.
static int broker_publish_error(int pid, uint64_t expected_token, uintptr_t broker_addr,
                                uint32_t error, const char* note) {
  uint32_t state = GTAV_PATCH_BROKER_STATE_ERROR;
  char note_buf[GTAV_PATCH_BROKER_NOTE_LEN];
  int rc = 0;

  if (!loader_instance_token_matches(pid, expected_token, "broker error publish")) {
    return -1;
  }
  memset(note_buf, 0, sizeof(note_buf));
  if (note) {
    strncpy(note_buf, note, sizeof(note_buf) - 1u);
  }
  if (gtav_proc_write(pid, broker_addr + offsetof(GtavPatchBrokerState, error), &error,
                      sizeof(error)) != 0) {
    rc = -1;
  }
  if (gtav_proc_write(pid, broker_addr + offsetof(GtavPatchBrokerState, note), note_buf,
                      sizeof(note_buf)) != 0) {
    rc = -1;
  }
  if (gtav_proc_write(pid, broker_addr + offsetof(GtavPatchBrokerState, state), &state,
                      sizeof(state)) != 0) {
    rc = -1;
  }
  if (rc != 0) {
    gtav_logf("broker: could not publish coherent error state: %s", gtav_proc_last_error());
  }
  return rc;
}

// A jump write is the transaction's commit boundary. Any later verification/publication failure
// must restore and verify the broker-provided original prologue before returning. Cancellation is
// deliberately ignored here: once target .text may have changed, commit/rollback must finish.
static int broker_fail_after_commit(int pid, uint64_t expected_token, uintptr_t broker_addr,
                                    uintptr_t target, uintptr_t page, size_t span,
                                    const GtavPatchBrokerState* st, const char* reason) {
  uint8_t check[GTAV_PATCH_BROKER_MAX_BYTES];
  char note[GTAV_PATCH_BROKER_NOTE_LEN];
  int attempt;
  int restored = 0;

  for (attempt = 0; attempt < 6 && !restored; ++attempt) {
    if (attempt) {
      usleep(150000);
    }
#if GTAV_PAYLOAD_PERSISTENT
    daemon_lock_refresh();  // heartbeat only; do not observe cancellation mid-rollback
#endif
    if (!loader_instance_token_matches(pid, expected_token, "broker rollback")) {
      gtav_logf("broker: %s; instance changed/unknown, refusing rollback into recycled pid",
                reason);
      return -1;
    }
    gtav_proc_protect(pid, page, span,
                      GTAV_PROC_PROT_READ | GTAV_PROC_PROT_WRITE | GTAV_PROC_PROT_EXEC);
    if (gtav_proc_write(pid, target, st->original, st->restore_len) == 0 &&
        gtav_proc_protect(pid, page, span, GTAV_PROC_PROT_READ | GTAV_PROC_PROT_EXEC) == 0 &&
        gtav_proc_read(pid, target, check, st->restore_len) == 0 &&
        memcmp(check, st->original, st->restore_len) == 0) {
      restored = 1;
    }
  }

  if (restored) {
    snprintf(note, sizeof(note), "%s; rollback verified", reason);
    gtav_logf("broker: %s; restored %u original bytes", reason, st->restore_len);
    broker_publish_error(pid, expected_token, broker_addr, GTAV_PATCH_BROKER_ERROR_INSTALL_FAILED,
                         note);
  } else {
    snprintf(note, sizeof(note), "%s; ROLLBACK FAILED", reason);
    gtav_logf("broker: %s; FAILED to restore/verify %u original bytes", reason, st->restore_len);
    broker_publish_error(pid, expected_token, broker_addr, GTAV_PATCH_BROKER_ERROR_RESTORE_FAILED,
                         note);
  }
  return -1;
}

// Consume the frame-hook patch broker the injected menu published and land the
// prologue jump on the target native's .text. The injected ELF has already built the
// thunk + gateway in its own RWX image and filled the broker with the target address
// and the ready-to-write jump bytes; all that's left is the privileged write the menu
// cannot do to its own host's execute-only .text. Returns 0 on success.
// `pin` is the build pin bound for the running target: the broker request must match
// it exactly (the loader owns the frame-hook contract; the worker may not choose the
// privileged eboot address/bytes it installs).
static int run_install_broker(int pid, uint64_t expected_token, const GtavBuildPin* pin,
                              uintptr_t base, const uint8_t* elf, size_t elf_len, size_t image_size,
                              int worker_free) {
  static const uint8_t jump_prefix[] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00};
  const uint8_t* pinned_expected = pin ? pin->broker_expected : NULL;
  const uint32_t pinned_stolen = pin ? pin->broker_stolen_len : 0u;
  const uint32_t pinned_patch = pin ? pin->broker_patch_len : 0u;
  uint64_t off = 0;
  uint64_t jump_destination = 0;
  GtavPatchBrokerState st;
  uintptr_t broker_addr, target, page, end;
  size_t span, access_len;
  uint32_t installed = GTAV_PATCH_BROKER_STATE_INSTALLED;
  uint8_t check[GTAV_PATCH_BROKER_MAX_BYTES];
  int attempt;

  if (gtav_elf_symbol_value(elf, elf_len, "gtav_patch_broker_state", &off) != 0) {
    gtav_logf("broker: gtav_patch_broker_state not found (ELF built without EXTERNAL_INSTALL?)");
    return -1;
  }
  if (pin == NULL || pinned_expected == NULL || pinned_stolen < 14u ||
      pinned_stolen > GTAV_PATCH_BROKER_MAX_BYTES || pinned_patch < 14u ||
      pinned_patch > pinned_stolen || pin->broker_target == 0ull ||
      pin->broker_continuation == 0ull || image_size < sizeof(st) ||
      off > (uint64_t)(image_size - sizeof(st)) || off > (uint64_t)(UINTPTR_MAX - base)) {
    gtav_logf("broker: loader-owned patch contract/image bounds are invalid");
    return -1;
  }
  broker_addr = base + (uintptr_t)off;

  if (worker_free) {
    // The injected image is mapped and relocated but no worker thread exists yet. Derive the
    // broker contract entirely from the loader's own ELF symbol table and the build pin, so a
    // mismatch refuses before anything resident is created.
    uint64_t thunk_sym = 0;
    uint64_t gateway_sym = 0;
    memset(&st, 0, sizeof(st));
    if (gtav_elf_symbol_value(elf, elf_len, "gtav_frame_hook_thunk_value", &thunk_sym) != 0 ||
        gtav_elf_symbol_value(elf, elf_len, "gtav_frame_hook_gateway", &gateway_sym) != 0) {
      gtav_logf("broker: worker-free path missing thunk/gateway ELF symbols");
      return -1;
    }
    if (gtav_proc_read(pid, base + (uintptr_t)thunk_sym, &st.thunk_addr, sizeof(st.thunk_addr)) !=
        0) {
      gtav_logf("broker: worker-free path cannot read relocated thunk value: %s",
                gtav_proc_last_error());
      return -1;
    }
    st.magic = GTAV_PATCH_BROKER_MAGIC;
    st.abi_version = GTAV_PATCH_BROKER_ABI_VERSION;
    st.struct_size = (uint32_t)sizeof(GtavPatchBrokerState);
    st.state = GTAV_PATCH_BROKER_STATE_READY;
    st.request_kind = GTAV_PATCH_BROKER_REQUEST_FRAME_HOOK;
    st.flags = GTAV_PATCH_BROKER_FLAG_RESTORE_REQUIRED | GTAV_PATCH_BROKER_FLAG_EXTERNAL_WRITE_ONLY;
    st.error = GTAV_PATCH_BROKER_ERROR_NONE;
    st.target_addr = (uint64_t)pin->broker_target;
    st.continuation_addr = (uint64_t)pin->broker_continuation;
    st.gateway_addr = (uint64_t)(base + (uintptr_t)gateway_sym);
    st.patch_len = pinned_patch;
    st.stolen_len = pinned_stolen;
    st.expected_len = pinned_stolen;
    st.jump_len = pinned_patch;
    st.restore_len = pinned_stolen;
    memcpy(st.expected, pinned_expected, pinned_stolen);
    memcpy(st.original, pinned_expected, pinned_stolen);
    memcpy(st.jump, jump_prefix, sizeof(jump_prefix));
    memcpy(st.jump + sizeof(jump_prefix), &st.thunk_addr, sizeof(st.thunk_addr));

    // Pre-load the gateway continuation slot (worker data symbol) so that if the worker
    // thread never starts (or starts late), the first native call still chains through
    // the original handler.  This must write to the payload's own data slot, not to the
    // game continuation address stored in st.continuation_addr.
    {
      uint64_t cont_sym = 0;
      uint64_t chain = (uint64_t)(pin->broker_target + (uint64_t)pinned_stolen);
      if (gtav_elf_symbol_value(elf, elf_len, "gtav_frame_hook_gateway_continuation_value",
                                &cont_sym) != 0) {
        gtav_logf(
            "broker: worker-free path missing gtav_frame_hook_gateway_continuation_value "
            "ELF symbol");
        return -1;
      }
      if (gtav_proc_write(pid, base + (uintptr_t)cont_sym, &chain, sizeof(chain)) != 0) {
        gtav_logf("broker: worker-free path cannot pre-load continuation slot: %s",
                  gtav_proc_last_error());
        return -1;
      }
    }

    // Pre-load the thunk's chain pointer too. The .text patch lands on frame_hook_thunk,
    // which runs BEFORE the worker thread has reached install_external(); without g_gateway
    // set it would return without chaining and the native call would never complete.
    {
      uint64_t gateway_chain_sym = 0;
      if (gtav_elf_symbol_value(elf, elf_len, "g_gateway", &gateway_chain_sym) != 0) {
        gtav_logf("broker: worker-free path missing g_gateway ELF symbol");
        return -1;
      }
      if (gtav_proc_write(pid, base + (uintptr_t)gateway_chain_sym, &st.gateway_addr,
                          sizeof(st.gateway_addr)) != 0) {
        gtav_logf("broker: worker-free path cannot pre-load g_gateway: %s", gtav_proc_last_error());
        return -1;
      }
    }
  } else {
    // The menu publishes the broker during features-init on the worker thread; poll a
    // few times in case init hasn't reached READY yet (the inject probe already waited).
    for (attempt = 0; attempt < 20; ++attempt) {
      if (!loader_instance_token_matches(pid, expected_token, "broker handshake")) {
        return -1;
      }
      memset(&st, 0, sizeof(st));
      if (gtav_proc_read(pid, broker_addr, &st, sizeof(st)) != 0) {
        gtav_logf("broker: read @0x%lx failed: %s", (unsigned long)broker_addr,
                  gtav_proc_last_error());
        return -1;
      }
      if (st.magic == GTAV_PATCH_BROKER_MAGIC && st.state == GTAV_PATCH_BROKER_STATE_READY) break;
#if GTAV_PAYLOAD_PERSISTENT
      if (daemon_wait_interruptible(100000UL) != 0) {
        gtav_logf("broker: stop requested while waiting for worker handshake");
        return -1;
      }
#else
      usleep(100000);
#endif
    }
  }
  if (st.magic != GTAV_PATCH_BROKER_MAGIC || st.abi_version != GTAV_PATCH_BROKER_ABI_VERSION) {
    gtav_logf("broker: bad magic=0x%llx abi=%u", (unsigned long long)st.magic, st.abi_version);
    return -1;
  }
  if (st.struct_size != sizeof(GtavPatchBrokerState)) {
    gtav_logf("broker: bad struct_size=%u expected=%lu", st.struct_size,
              (unsigned long)sizeof(GtavPatchBrokerState));
    return -1;  // layout is not trustworthy enough to publish fields through local offsets
  }
  if (st.state != GTAV_PATCH_BROKER_STATE_READY ||
      st.request_kind != GTAV_PATCH_BROKER_REQUEST_FRAME_HOOK) {
    gtav_logf("broker: not ready (state=%u request=%u error=%u)", st.state, st.request_kind,
              st.error);
    return -1;
  }
  memcpy(&jump_destination, st.jump + sizeof(jump_prefix), sizeof(jump_destination));
  if (st.target_addr != (uint64_t)pin->broker_target ||
      st.continuation_addr != (uint64_t)pin->broker_continuation || st.patch_len != pinned_patch ||
      st.jump_len != pinned_patch || st.stolen_len != pinned_stolen ||
      st.expected_len != pinned_stolen || st.restore_len != pinned_stolen ||
      memcmp(st.expected, pinned_expected, (size_t)pinned_stolen) != 0 ||
      memcmp(st.original, pinned_expected, (size_t)pinned_stolen) != 0 ||
      memcmp(st.jump, jump_prefix, sizeof(jump_prefix)) != 0 || jump_destination != st.thunk_addr ||
      (st.flags & GTAV_PATCH_BROKER_FLAG_EXTERNAL_WRITE_ONLY) == 0u ||
      !loader_image_contains(base, image_size, (uintptr_t)st.thunk_addr, 1u) ||
      !loader_image_contains(base, image_size, (uintptr_t)st.gateway_addr, 1u)) {
    gtav_logf("broker: request does not match loader-owned PLAYER_PED_ID patch contract");
    broker_publish_error(pid, expected_token, broker_addr,
                         GTAV_PATCH_BROKER_ERROR_VALIDATION_FAILED,
                         "broker request differs from loader-owned patch contract");
    return -1;
  }
  // The current production gateway overwrites a 14-byte absolute jump while stealing/restoring a
  // 16-byte instruction-aligned prologue (the frame-hook source default uses another longer-than-
  // patch window). patch_len need only cover the jump; stolen_len is the exact expected/rollback
  // extent and may legitimately be larger.
  if (st.target_addr == 0u || st.thunk_addr == 0u || st.jump_len == 0u ||
      st.jump_len > GTAV_PATCH_BROKER_MAX_BYTES || st.patch_len == 0u ||
      st.patch_len > GTAV_PATCH_BROKER_MAX_BYTES || st.patch_len < st.jump_len ||
      st.stolen_len < st.patch_len || st.stolen_len > GTAV_PATCH_BROKER_MAX_BYTES ||
      st.expected_len != st.stolen_len || st.restore_len != st.stolen_len ||
      (st.flags & GTAV_PATCH_BROKER_FLAG_RESTORE_REQUIRED) == 0u ||
      memcmp(st.original, st.expected, st.restore_len) != 0) {
    gtav_logf(
        "broker: invalid lengths/restore data patch=%u stolen=%u expected=%u jump=%u restore=%u",
        st.patch_len, st.stolen_len, st.expected_len, st.jump_len, st.restore_len);
    broker_publish_error(pid, expected_token, broker_addr,
                         GTAV_PATCH_BROKER_ERROR_VALIDATION_FAILED,
                         "loader rejected broker lengths/restore data");
    return -1;
  }

  target = (uintptr_t)st.target_addr;
  access_len = st.expected_len;

  // Liveness gate immediately before the privileged .text write. If the target died, or its pid
  // was recycled to a different app, between the broker handshake above and now, a kernel write
  // here would plant a jump into the wrong / teardown-state process -- the highest-blast-radius
  // mistake the loader can make (a kernel R/W against an unstable target can fault in a kernel
  // context and take the whole console down). Bail instead; a re-run / persistent re-arm injects
  // cleanly into the next instance.
  if (!loader_target_is_live(pid) ||
      !loader_instance_token_matches(pid, expected_token, "broker preflight")) {
    gtav_logf("broker: target pid=%d is no longer the live game before .text write; aborting", pid);
    return -1;
  }

  // Bounds gate: never kernel-write a target outside the known .text live range. A stale pin
  // (game updated) could otherwise point the frame-hook jump at an arbitrary mapping.
  if (!loader_text_write_ok(pin, target, access_len)) {
    gtav_logf(
        "broker: target 0x%llx (+%lu) outside .text live range [0x%llx,0x%llx); refusing write",
        (unsigned long long)target, (unsigned long)access_len,
        (unsigned long long)(pin ? (uintptr_t)pin->text_live_start
                                 : (uintptr_t)(GTAV_LOADER_TEXT_LIVE_START)),
        (unsigned long long)(pin ? (uintptr_t)pin->text_live_end
                                 : (uintptr_t)(GTAV_LOADER_TEXT_LIVE_END)));
    broker_publish_error(pid, expected_token, broker_addr,
                         GTAV_PATCH_BROKER_ERROR_VALIDATION_FAILED,
                         "broker target outside pinned text range");
    return -1;
  }
  page = target & ~(uintptr_t)0x3fffu;
  end = (target + access_len + 0x3fffu) & ~(uintptr_t)0x3fffu;
  span = (size_t)(end - page);

  // Make the execute-only page readable and verify the exact untouched prologue before the first
  // write. This both rejects stale/already-patched targets and proves st.original is valid rollback
  // material. All cancellation checks happen before the first target write.
  {
    int read_ok = 0;
    for (attempt = 0; attempt < 6 && !read_ok; ++attempt) {
      if (attempt) {
#if GTAV_PAYLOAD_PERSISTENT
        if (daemon_wait_interruptible(150000UL) != 0) {
          broker_publish_error(pid, expected_token, broker_addr,
                               GTAV_PATCH_BROKER_ERROR_INSTALL_FAILED,
                               "broker install cancelled before commit");
          return -1;
        }
#else
        usleep(150000);
#endif
      }
      if (!loader_instance_token_matches(pid, expected_token, "broker prologue read")) {
        return -1;
      }
      gtav_proc_read(pid, target, check, st.expected_len); /* fault the page in */
      read_ok =
          (gtav_proc_protect(pid, page, span, GTAV_PROC_PROT_READ | GTAV_PROC_PROT_EXEC) == 0 &&
           gtav_proc_read(pid, target, check, st.expected_len) == 0);
    }
    if (!read_ok) {
      gtav_logf("broker: cannot read live prologue before commit: %s", gtav_proc_last_error());
      broker_publish_error(pid, expected_token, broker_addr,
                           GTAV_PATCH_BROKER_ERROR_VALIDATION_FAILED,
                           "live prologue unreadable before commit");
      return -1;
    }
    if (memcmp(check, st.expected, st.expected_len) != 0) {
      gtav_logf("broker: live prologue mismatch before commit; refusing target write");
      broker_publish_error(pid, expected_token, broker_addr,
                           GTAV_PATCH_BROKER_ERROR_VALIDATION_FAILED,
                           "live prologue mismatch before commit");
      return -1;
    }
#if GTAV_PAYLOAD_PERSISTENT
    if (daemon_wait_interruptible(0) != 0) {
      broker_publish_error(pid, expected_token, broker_addr, GTAV_PATCH_BROKER_ERROR_INSTALL_FAILED,
                           "broker install cancelled before commit");
      return -1;
    }
#endif
    if (!loader_instance_token_matches(pid, expected_token, "broker commit")) {
      broker_publish_error(pid, expected_token, broker_addr,
                           GTAV_PATCH_BROKER_ERROR_VALIDATION_FAILED,
                           "game instance changed before broker commit");
      return -1;
    }
  }

  // Commit the already-validated jump. From the first write attempt onward this transaction is
  // non-interruptible: a failed PT_IO may still have changed part of the prologue, so every failure
  // completes through the verified rollback path below.
  {
    int wrote_ok = 0, attempt2;
    for (attempt2 = 0; attempt2 < 6 && !wrote_ok; ++attempt2) {
      uint8_t fault[GTAV_PATCH_BROKER_MAX_BYTES];
      if (attempt2) {
        usleep(150000);
      }
#if GTAV_PAYLOAD_PERSISTENT
      daemon_lock_refresh();  // heartbeat only; cancellation is deferred until commit/rollback ends
#endif
      if (!loader_instance_token_matches(pid, expected_token, "broker jump write")) {
        return broker_fail_after_commit(pid, expected_token, broker_addr, target, page, span, &st,
                                        "game instance changed after commit");
      }
      gtav_proc_read(pid, target, fault, st.jump_len); /* fault the page in */
      gtav_proc_protect(pid, page, span,
                        GTAV_PROC_PROT_READ | GTAV_PROC_PROT_WRITE | GTAV_PROC_PROT_EXEC);
      if (gtav_proc_write(pid, target, st.jump, st.jump_len) == 0) wrote_ok = 1;
    }
    if (!wrote_ok) {
      gtav_logf("broker: write jump @0x%lx failed after retries: %s", (unsigned long)target,
                gtav_proc_last_error());
      return broker_fail_after_commit(pid, expected_token, broker_addr, target, page, span, &st,
                                      "jump write failed");
    }
  }
  if (!loader_instance_token_matches(pid, expected_token, "broker jump readback")) {
    return broker_fail_after_commit(pid, expected_token, broker_addr, target, page, span, &st,
                                    "game instance changed after commit");
  }
  if (gtav_proc_read(pid, target, check, st.jump_len) != 0) {
    gtav_logf("broker: jump readback @0x%lx failed: %s", (unsigned long)target,
              gtav_proc_last_error());
    return broker_fail_after_commit(pid, expected_token, broker_addr, target, page, span, &st,
                                    "jump readback failed");
  }
  if (memcmp(check, st.jump, st.jump_len) != 0) {
    gtav_logf("broker: jump readback mismatch @0x%lx; refusing to report install success",
              (unsigned long)target);
    return broker_fail_after_commit(pid, expected_token, broker_addr, target, page, span, &st,
                                    "jump readback mismatch");
  }
  if (!loader_instance_token_matches(pid, expected_token, "broker RX restore")) {
    return broker_fail_after_commit(pid, expected_token, broker_addr, target, page, span, &st,
                                    "game instance changed after commit");
  }
  if (gtav_proc_protect(pid, page, span, GTAV_PROC_PROT_READ | GTAV_PROC_PROT_EXEC) != 0) {
    gtav_logf("broker: could not restore target page RX after jump commit: %s",
              gtav_proc_last_error());
    return broker_fail_after_commit(pid, expected_token, broker_addr, target, page, span, &st,
                                    "post-commit RX protection failed");
  }
  if (!worker_free) {
    // Confirm the detour actually fires. The worker copies gtav_frame_hook_call_count()
    // into the broker each tick, so a climbing thunk_call_count means the target native is
    // being called and our thunk runs on the game thread -- which the registration-slot
    // swap never achieved.
    {
      uint64_t c0 = 0, c1 = 0;
      uintptr_t cnt = broker_addr + offsetof(GtavPatchBrokerState, thunk_call_count);
      int poll;
      if (!loader_instance_token_matches(pid, expected_token, "broker heartbeat")) {
        return broker_fail_after_commit(pid, expected_token, broker_addr, target, page, span, &st,
                                        "game instance changed after commit");
      }
      if (gtav_proc_read(pid, cnt, &c0, sizeof(c0)) != 0) {
        gtav_logf("broker: cannot read initial thunk activity: %s", gtav_proc_last_error());
        return broker_fail_after_commit(pid, expected_token, broker_addr, target, page, span, &st,
                                        "initial thunk heartbeat read failed");
      }
      // Poll up to ~12s for the count to climb instead of two fixed samples -- a target
      // native may be quiet for a second or two right after inject (loading/idle) yet
      // fire heavily once gameplay is active.
      for (poll = 0; poll < 24; ++poll) {
#if GTAV_PAYLOAD_PERSISTENT
        daemon_lock_refresh();  // keep lease alive, but finish commit/rollback before observing
                                // stop
#endif
        usleep(500000);
        if (!loader_instance_token_matches(pid, expected_token, "broker heartbeat")) {
          return broker_fail_after_commit(pid, expected_token, broker_addr, target, page, span, &st,
                                          "game instance changed after commit");
        }
        if (gtav_proc_read(pid, cnt, &c1, sizeof(c1)) != 0) {
          gtav_logf("broker: cannot read thunk activity: %s", gtav_proc_last_error());
          return broker_fail_after_commit(pid, expected_token, broker_addr, target, page, span, &st,
                                          "thunk heartbeat read failed");
        }
        if (c1 > c0) break;
      }
      if (c1 <= c0) {
        gtav_logf("broker: thunk activity stayed flat at %llu for ~%dms; refusing healthy install",
                  (unsigned long long)c0, (poll + 1) * 500);
        return broker_fail_after_commit(pid, expected_token, broker_addr, target, page, span, &st,
                                        "thunk heartbeat stayed flat");
      }
      gtav_logf("broker: thunk fires=%llu->%llu after ~%dms (DETOUR LIVE on game thread)",
                (unsigned long long)c0, (unsigned long long)c1, (poll + 1) * 500);
    }
  }

  // Publish INSTALLED after jump readback verifies; in worker-free mode the heartbeat is skipped
  // because the worker thread has not started yet.
  // A mapped worker without a live game-thread executor is a partial install, never healthy.
  if (!loader_instance_token_matches(pid, expected_token, "broker installed publish")) {
    return broker_fail_after_commit(pid, expected_token, broker_addr, target, page, span, &st,
                                    "game instance changed after commit");
  }
  if (gtav_proc_write(pid, broker_addr + offsetof(GtavPatchBrokerState, state), &installed,
                      sizeof(installed)) != 0) {
    gtav_logf("broker: installed-state publish failed: %s", gtav_proc_last_error());
    return broker_fail_after_commit(pid, expected_token, broker_addr, target, page, span, &st,
                                    "installed-state publish failed");
  }
  gtav_logf("broker: frame-hook installed target=0x%llx thunk=0x%llx jump_len=%u verified=1",
            (unsigned long long)st.target_addr, (unsigned long long)st.thunk_addr, st.jump_len);
  return 0;
}
#endif

#if GTAV_MENU_PAYLOAD_INJECT
// A remote image cannot currently be unmapped safely. Distinguish failure before allocation,
// failure after a partial/full image mapping, and broker failure after the worker starts so the
// persistent daemon never retries by stacking another image in the same game instance or reports a
// partial install as success.
#define GTAV_INJECT_ALREADY_LOADED 1
#define GTAV_INJECT_BUSY 2
// A partial mapping stays resident and the lock remains held. Relaunch GTA before another run.
#define GTAV_INJECT_PARTIAL_BROKER_FAILED (-2)
#define GTAV_INJECT_PARTIAL_MAP_FAILED (-3)

// Loader-owned handles for the one resident worker installed in a persistent daemon's current
// game instance. The mapped image deliberately remains resident after retirement; these addresses
// exist only so the privileged loader can stop the worker, verify every in-process callback is
// restored, and remove its own eboot .text detour exactly. Nothing here authorizes a second mapping
// in the same process -- the inject lock remains until relaunch.
typedef struct LoaderResidentInstall {
  int valid;
  int pid;
  uint64_t token;
  const GtavBuildPin* pin;
  uintptr_t base;
  size_t image_size;
  uintptr_t broker;
  uintptr_t mailbox;
  uintptr_t status;
  uintptr_t render_phase_state;
  uintptr_t render_phase_slot;
  uintptr_t render_phase_wrapper;
} LoaderResidentInstall;

// Read the feature-menu ELF from disk and inject it into the target, starting its
// worker thread. Returns 0 on success.
#if GTAV_PAYLOAD_INJECT_GUARD
#define GTAV_INJECT_CLAIM_ERROR (-1)
#define GTAV_INJECT_CLAIMED 0
#define GTAV_INJECT_CLAIM_ALREADY 1
#define GTAV_INJECT_CLAIM_BUSY 2

// Seconds since the lock's last heartbeat (mtime), or -1 if it cannot be stat'd.
static long inject_lock_age_seconds(const char* path) {
  struct stat st;
  time_t now;
  if (stat(path, &st) != 0) {
    return -1;
  }
  now = time(NULL);
  return now > st.st_mtime ? (long)(now - st.st_mtime) : 0;  // clamp clock skew to fresh
}

// Read the route-neutral process-start token only while the production app-id identity still
// matches the transaction. Sampling app id on both sides of the uncached allproc walk prevents a
// same-pid process replacement from binding T2's start token to a T1 transaction. The app id
// remains the identity used by every process-control stage; this token exists only for cross-route
// lock ownership and relaunch reclamation.
static int read_bound_process_start_token(int pid, uint64_t expected_app_id, const char* phase,
                                          uint64_t* start_token_out) {
  uint64_t app_before;
  uint64_t start_token;
  uint64_t app_after;
  if (start_token_out == NULL || expected_app_id == 0) {
    return -1;
  }
  *start_token_out = 0;
  app_before = gtav_proc_app_id(pid);
  start_token = gtav_proc_start_token(pid);
  app_after = gtav_proc_app_id(pid);
  if (app_before == 0 || app_before != expected_app_id || app_after != expected_app_id ||
      start_token == 0 || (start_token & GTAV_INJECT_PROCESS_START_TOKEN_MARKER) == 0) {
    gtav_logf(
        "inject: %s identity changed/unknown pid=%d app=0x%llx->0x%llx expected=0x%llx "
        "start=0x%llx",
        phase != NULL ? phase : "lock token", pid, (unsigned long long)app_before,
        (unsigned long long)app_after, (unsigned long long)expected_app_id,
        (unsigned long long)start_token);
    return -1;
  }
  *start_token_out = start_token;
  return 0;
}

// Atomically claim the right to inject into `pid`. The first loader to reach a given
// pid creates the lock (O_CREAT|O_EXCL) and proceeds; a later one sees it and skips, so the worker
// is never double-loaded. An existing lock is reclaimed only when a FINALIZED transaction has a
// definite marker-bearing process-start mismatch (the same pid now belongs to a new launch).
// Legacy/untyped finalized records and all QUARANTINED
// partial/in-flight transactions, age, and foreground loss never authorize reclaim. Returns
// CLAIMED for a newly acquired lock, ALREADY for an unreconciled existing/racing claim, and ERROR
// for identity/filesystem failures.
static int claim_inject_under_guard(int pid, uint64_t expected_token, uint64_t* lock_token_out) {
  char path[GTAV_INJECT_LOCK_PATH_MAX];
  int fd;
  uint64_t lock_token = 0;
  uint64_t verified_lock_token = 0;
  if (lock_token_out == NULL) {
    return GTAV_INJECT_CLAIM_ERROR;
  }
  *lock_token_out = 0;
  if (read_bound_process_start_token(pid, expected_token, "lock claim", &lock_token) != 0) {
    return GTAV_INJECT_CLAIM_ERROR;
  }
  gtav_inject_lock_path(path, sizeof(path), pid);
  fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
  if (fd < 0) {
    if (errno != EEXIST) {
      gtav_logf("inject: cannot create lock %s: errno=%d", path, errno);
      return GTAV_INJECT_CLAIM_ERROR;
    }
    // Foreground loss is not proof that this pid/instance died. Reclaim authority comes only from
    // a definite typed process-start mismatch; all same/unknown identity cases remain quarantined.
    int live = -1;
    long age = inject_lock_age_seconds(path);
    int age_errno = age < 0 ? errno : 0;
    if (age < 0) {
      if (age_errno != ENOENT) {
        gtav_logf("inject: cannot stat existing lock %s: errno=%d", path, age_errno);
        return GTAV_INJECT_CLAIM_ERROR;
      }
      // The lock vanished between the failed O_EXCL create and stat. Retry exactly once: success
      // means we own it; EEXIST means another loader won the race; anything else is a real error.
      fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
      if (fd >= 0) {
        goto claimed;
      }
      if (errno == EEXIST) {
        return GTAV_INJECT_CLAIM_ALREADY;
      }
      gtav_logf("inject: cannot claim vanished lock %s: errno=%d", path, errno);
      return GTAV_INJECT_CLAIM_ERROR;
    }
    unsigned char lock_state = 0;
    uint64_t prev = 0;
    int record_ok = gtav_inject_lock_read_record(path, &lock_state, &prev) == 0;
    int record_finalized = record_ok &&
                           (lock_state == GTAV_INJECT_LOCK_STATE_FINALIZED ||
                            lock_state == GTAV_INJECT_LOCK_STATE_MANUAL_FINALIZED) &&
                           (prev & GTAV_INJECT_PROCESS_START_TOKEN_MARKER) != 0;
    // Unknown/incomplete/quarantined records fail closed like a token match. A definite nonzero
    // typed process-start difference authorizes reclaim only after the prior transaction published
    // I/M. An in-flight or partial map must remain quarantined across a same-pid transition.
    int token_matches = (!record_finalized || prev == 0 || prev == lock_token) ? 1 : 0;
    if (!record_ok) {
      gtav_logf("inject: lock %s has no complete recognized record; retaining fail-closed", path);
    } else if ((lock_state == GTAV_INJECT_LOCK_STATE_FINALIZED ||
                lock_state == GTAV_INJECT_LOCK_STATE_MANUAL_FINALIZED ||
                lock_state == GTAV_INJECT_LOCK_STATE_LEGACY) &&
               !record_finalized) {
      gtav_logf("inject: lock %s uses a legacy/untyped finalized record; retaining fail-closed",
                path);
    }
    if (!gtav_inject_lock_is_stale(live, age, GTAV_INJECT_LOCK_STALE_SECONDS, token_matches)) {
      return GTAV_INJECT_CLAIM_ALREADY;  // live worker owns this instance; health not reconciled
    }
    gtav_logf("reclaiming stale inject lock %s (live=%d age=%lds start=0x%llx->0x%llx)", path, live,
              age, (unsigned long long)prev, (unsigned long long)lock_token);
    if (unlink(path) != 0 && errno != ENOENT) {
      gtav_logf("inject: cannot remove stale lock %s: errno=%d", path, errno);
      return GTAV_INJECT_CLAIM_ERROR;
    }
    fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) {
      if (errno == EEXIST) {
        return GTAV_INJECT_CLAIM_ALREADY;  // lost the race; never treat it as our success
      }
      gtav_logf("inject: cannot reclaim lock %s: errno=%d", path, errno);
      return GTAV_INJECT_CLAIM_ERROR;
    }
  }
claimed:
  close(fd);
  if (read_bound_process_start_token(pid, expected_token, "lock publish", &verified_lock_token) !=
          0 ||
      verified_lock_token != lock_token) {
    // Keep the O_EXCL main lock as a fail-closed quarantine. It is not safe to publish either
    // process generation after the identity crossed while stale-lock reclamation was in progress.
    gtav_logf("inject: process-start identity changed before quarantine publish; refusing map");
    return GTAV_INJECT_CLAIM_ERROR;
  }
  if (gtav_inject_lock_write_token(pid, lock_token) != 0) {
    // The O_EXCL file remains in place. Treat its unknown/partial contents as a permanent
    // quarantine rather than mapping without a durable guard.
    gtav_logf("inject: cannot initialize verified quarantine record %s; refusing map", path);
    return GTAV_INJECT_CLAIM_ERROR;
  }
  *lock_token_out = lock_token;
  return GTAV_INJECT_CLAIMED;
}

// Serialize the entire claim/map/finalize transaction for one pid. Unlike the main worker lock,
// this mutex is never lease-reclaimed: if a loader dies while ownership is ambiguous, leaving the
// guard behind is safer than permitting a second loader to ABA-unlink a newly-created quarantine.
// CLAIMED deliberately keeps the guard; run_inject releases it only after a durable Q/I/M record
// (or a proven pre-allocation failure) represents the outcome.
static int claim_inject(int pid, uint64_t expected_token, uint64_t* lock_token_out) {
  char guard_path[GTAV_INJECT_LOCK_PATH_MAX];
  int fd;
  int rc;
  gtav_inject_claim_guard_path(guard_path, sizeof(guard_path), pid);
  fd = open(guard_path, O_CREAT | O_EXCL | O_WRONLY, 0644);
  if (fd < 0) {
    if (errno == EEXIST) {
      // The owner can still abort a pre-inject Q or transition Q->I/M, so no main-lock snapshot is
      // terminal while this mutex exists. Always report BUSY and retry after it disappears; only
      // then may claim_inject_under_guard classify the canonical record as ALREADY or reclaim it.
      gtav_logf("inject: transaction guard active for pid=%d; retry after owner publishes outcome",
                pid);
      return GTAV_INJECT_CLAIM_BUSY;
    }
    gtav_logf("inject: cannot create transaction guard %s: errno=%d", guard_path, errno);
    return GTAV_INJECT_CLAIM_ERROR;
  }
  close(fd);
  if (lock_token_out == NULL) {
    unlink(guard_path);
    return GTAV_INJECT_CLAIM_ERROR;
  }
  *lock_token_out = 0;
  rc = claim_inject_under_guard(pid, expected_token, lock_token_out);
  if (rc != GTAV_INJECT_CLAIMED) {
    unlink(guard_path);  // no mapping began; no ownership ambiguity remains
  }
  return rc;
}

static void release_claim_guard(int pid) {
  char path[GTAV_INJECT_LOCK_PATH_MAX];
  gtav_inject_claim_guard_path(path, sizeof(path), pid);
  unlink(path);
}

// Release the transaction mutex only if the canonical main lock still carries a complete state that
// blocks same-instance retries. If the file vanished or was replaced, retain the mutex as the final
// PID-wide quarantine; a healthy mapped worker without any guard must never be reported as safely
// re-injectable.
static void release_claim_guard_if_protected(int pid, uint64_t expected_lock_token) {
  char lock_path[GTAV_INJECT_LOCK_PATH_MAX];
  char guard_path[GTAV_INJECT_LOCK_PATH_MAX];
  unsigned char state = 0;
  uint64_t token = 0;
  gtav_inject_lock_path(lock_path, sizeof(lock_path), pid);
  if (gtav_inject_lock_read_record(lock_path, &state, &token) == 0 &&
      (state == GTAV_INJECT_LOCK_STATE_QUARANTINED || state == GTAV_INJECT_LOCK_STATE_FINALIZED ||
       state == GTAV_INJECT_LOCK_STATE_MANUAL_FINALIZED) &&
      (token & GTAV_INJECT_PROCESS_START_TOKEN_MARKER) != 0 && token == expected_lock_token) {
    release_claim_guard(pid);
    return;
  }
  gtav_inject_claim_guard_path(guard_path, sizeof(guard_path), pid);
  gtav_logf("inject: retaining transaction guard %s; canonical lock is missing/foreign",
            guard_path);
}

// Drop our claim only after a proven pre-allocation failure. Once an image may exist, or in the
// external frame-hook lane whose eboot jump outlives worker shutdown, the lock is retained until a
// different process-start token proves game relaunch. App id is still checked independently so a
// same-pid replacement can never make us delete the prior transaction's quarantine.
static void release_inject_if_owned(int pid, uint64_t expected_token,
                                    uint64_t expected_lock_token) {
  char path[GTAV_INJECT_LOCK_PATH_MAX];
  unsigned char state = 0;
  uint64_t stored_token = 0;
  uint64_t current_lock_token = 0;
  gtav_inject_lock_path(path, sizeof(path), pid);
  if (read_bound_process_start_token(pid, expected_token, "lock release", &current_lock_token) !=
          0 ||
      current_lock_token != expected_lock_token ||
      gtav_inject_lock_read_record(path, &state, &stored_token) != 0 ||
      state != GTAV_INJECT_LOCK_STATE_QUARANTINED || stored_token != expected_lock_token) {
    gtav_logf(
        "inject: retaining lock %s; ownership changed/unknown app=0x%llx "
        "start=0x%llx->0x%llx",
        path, (unsigned long long)expected_token, (unsigned long long)expected_lock_token,
        (unsigned long long)current_lock_token);
    return;
  }
  unlink(path);
}
#endif

#endif  // GTAV_MENU_PAYLOAD_INJECT

#if GTAV_LOADER_TARGET_TRANSACTION && GTAV_LOADER_VERIFY_VERSION
// Build-pin detection: bind the running game to one row of the loader's compiled
// build-pin table (loader_pins_generated.h / include/gtavmenu/build_pin.h). The
// loader auto-detects which supported GTA V build is live by probing each row's
// version signature -- the GET_FRAME_COUNT prologue, real game code the loader never
// patches (unlike the frame-hook target, whose prologue it rewrites) -- and runs with
// the first matching build's pins (frame-hook contract, .text guard range, SP-ready
// anchor). This is how one loader deploy covers every supported build and how an
// unsupported build is rejected cleanly.
//
// GET_FRAME_COUNT lives in execute-only .text: a raw PT_IO read of a no-read page
// comes back short (there is no kernel copyout for a *process* VA -- ps5debug reads
// the same bytes fine that way). So mirror run_install_broker's .text access: fault
// the page in, gtav_proc_protect it to add READ (keep EXEC so the running game keeps
// executing GET_FRAME_COUNT), then read. The .text vm-entry path EFAULTs
// intermittently (residency / entry split), so retry a few times.
//
// Result codes:
//   GTAV_BUILD_BIND_OK          matched a build pin; *pin_out points into the table
//   GTAV_BUILD_BIND_UNSUPPORTED no compiled pin matched (unsupported/undumped build)
//   GTAV_BUILD_BIND_ERROR       unrecoverable (no kernel R/W, instance changed mid-probe)
#define GTAV_BUILD_BIND_OK 0
#define GTAV_BUILD_BIND_ERROR (-1)
#define GTAV_BUILD_BIND_UNSUPPORTED (-2)
// Probe one build pin's version signature. Returns 1 on an exact byte match, 0 when the
// signature does not match (try the next build), and -1 on a hard probe error that makes
// further detection unreliable (token/ABI change or unreadable target) -- the caller
// aborts the whole ballot rather than mislabel the build as unsupported. Never performs
// a write that survives -- .text is protect-touched to READ|EXEC exactly as the broker
// does, and no bytes are modified.
static int probe_version_signature(int pid, uint64_t expected_token, const GtavBuildPin* pin) {
  uint8_t got[GTAV_BUILD_PIN_VERSION_BYTES_N];
  const uintptr_t addr = (uintptr_t)pin->version_signature_addr;
  const uintptr_t page = addr & ~(uintptr_t)0x3fffu;
  int rd = -1;
  int attempt;
  // Never gtav_proc_protect a probe address outside the pinned .text live range: a
  // stale pin would otherwise re-protect an unrelated mapping via kernel R/W before
  // we even compare bytes.
  if (!loader_text_write_ok(pin, addr, sizeof(got))) {
    gtav_logf("build detect: probe 0x%llx outside %s .text live range [0x%llx,0x%llx)",
              (unsigned long long)addr, pin->target_id,
              (unsigned long long)(uintptr_t)pin->text_live_start,
              (unsigned long long)(uintptr_t)pin->text_live_end);
    return -1;
  }
  for (attempt = 0; attempt < 6 && rd != 0; ++attempt) {
    if (attempt) {
      usleep(150000);
    }
    if (!loader_instance_token_matches(pid, expected_token, "build detect")) {
      return -1;
    }
#if GTAV_LOADER_PROBE_NOSTOP
    // No PT_ATTACH and no protection change: mdbg copies the prologue straight out of the
    // running process. `page` is unused on this path.
    (void)page;
    rd = gtav_proc_read_nostop(pid, addr, got, sizeof(got));
#else
    gtav_proc_read(pid, addr, got, sizeof(got)); /* fault the page in */
    if (!loader_instance_token_matches(pid, expected_token, "build detect protect")) {
      return -1;
    }
    gtav_proc_protect(pid, page, 0x4000, GTAV_PROC_PROT_READ | GTAV_PROC_PROT_EXEC);
    rd = gtav_proc_read(pid, addr, got, sizeof(got));
#endif
  }
  if (rd != 0) {
    gtav_logf("build detect: cannot read %s prologue at 0x%llx (%s)", pin->target_id,
              (unsigned long long)addr, gtav_proc_last_error());
    return -1;
  }
  if (!loader_instance_token_matches(pid, expected_token, "build detect accept")) {
    return -1;
  }
  if (memcmp(got, pin->version_signature_expected, sizeof(got)) != 0) {
    gtav_logf("build detect: %s signature mismatch at 0x%llx; checking next build", pin->target_id,
              (unsigned long long)addr);
    return 0;
  }
  gtav_logf("build detect: target confirmed %s", pin->target_id);
  return 1;
}

// Probe every compiled row and fail closed:
// an unsupported build returns GTAV_BUILD_BIND_UNSUPPORTED *before* any worker map or
// privileged write is attempted, so the game is left untouched and the caller can
// notify the user.
static int bind_build(int pid, uint64_t expected_token, const GtavBuildPin** pin_out) {
  size_t i;
  if (pin_out != NULL) {
    *pin_out = NULL;
  }
  gtav_logf("build detect: target credential/root elevation is absent from production");
  if (!loader_instance_token_matches(pid, expected_token, "build detect ballot")) {
    return GTAV_BUILD_BIND_ERROR;
  }
#if GTAV_LOADER_VERIFY_VERSION && GTAV_BUILD_PIN_COUNT == 0
  gtav_logf(
      "build detect: no compiled build pins (loader_pins_generated.h empty); "
      "refusing inject");
  return GTAV_BUILD_BIND_UNSUPPORTED;
#else
  for (i = 0; i < GTAV_BUILD_PIN_COUNT; i++) {
    int prc = probe_version_signature(pid, expected_token, &gtav_build_pins[i]);
    if (prc == 1) {
#ifdef GTAV_LOADER_EXPECT_TARGET_ID
      if (strcmp(gtav_build_pins[i].target_id, GTAV_LOADER_EXPECT_TARGET_ID) != 0) {
        gtav_logf("build detect: live target %s does not match embedded worker target %s; refusing",
                  gtav_build_pins[i].target_id, GTAV_LOADER_EXPECT_TARGET_ID);
        return GTAV_BUILD_BIND_UNSUPPORTED;
      }
#endif
      if (pin_out != NULL) {
        *pin_out = &gtav_build_pins[i];
      }
      return GTAV_BUILD_BIND_OK;
    }
    if (prc == -1) {
      gtav_logf("build detect: probing %s failed; aborting detection (stale pins?)",
                gtav_build_pins[i].target_id);
      return GTAV_BUILD_BIND_ERROR;
    }
  }
  return GTAV_BUILD_BIND_UNSUPPORTED;
#endif
}
#endif  // GTAV_LOADER_TARGET_TRANSACTION && GTAV_LOADER_VERIFY_VERSION

#if GTAV_MENU_PAYLOAD_INJECT
static int load_worker_elf(uint8_t** out, long* len_out) {
  uint8_t* buf = NULL;
  long len = 0;
#if GTAV_MENU_EMBEDDED_WORKER
  size_t embedded_len = (size_t)(gtav_embedded_worker_end - gtav_embedded_worker_start);
  if (embedded_len == 0 || embedded_len > LONG_MAX) {
    gtav_logf("worker: embedded ELF has invalid size %llu", (unsigned long long)embedded_len);
    return -1;
  }
  len = (long)embedded_len;
  buf = (uint8_t*)malloc(embedded_len);
  if (buf == NULL) {
    gtav_logf("worker: cannot allocate %ld bytes for embedded ELF", len);
    return -1;
  }
  memcpy(buf, gtav_embedded_worker_start, embedded_len);
#else
  const char* path = GTAV_MENU_INJECT_ELF_PATH;
  FILE* file = fopen(path, "rb");
  if (file == NULL) {
    gtav_logf("worker: cannot open %s", path);
    return -1;
  }
  if (fseek(file, 0, SEEK_END) != 0 || (len = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) != 0 ||
      (buf = (uint8_t*)malloc((size_t)len)) == NULL ||
      fread(buf, 1, (size_t)len, file) != (size_t)len) {
    gtav_logf("worker: read failed for %s (len=%ld)", path, len);
    fclose(file);
    free(buf);
    return -1;
  }
  fclose(file);
#endif
  *out = buf;
  *len_out = len;
  return 0;
}

#if GTAV_LOADER_CAVE_BOOTSTRAP
// The bootstrap's region comes back from mmap as RW, so the loader makes it executable before the
// worker image goes in. W^X stops the game doing this to itself, but kernel_set_vmem_protection
// writes the vm_map entry directly and does not go through vm_map_protect.
static int cave_region_make_rwx(int pid, uint64_t base, uint64_t size) {
  // Retried, not fire-and-forget: this exact call failed once with errno=14 (EFAULT) on a freshly
  // mmap'd region in an otherwise healthy inject, aborting the whole injection. Protection changes
  // are transiently unreliable on this firmware, which is why the bootstrap's own protects have
  // always retried; this one was the odd path out.
  if (gtav_cave_protect_retry(pid, (uintptr_t)base, (size_t)size,
                              GTAV_PROC_PROT_READ | GTAV_PROC_PROT_WRITE | GTAV_PROC_PROT_EXEC,
                              "cave region->rwx") != 0) {
    gtav_logf("cave: could not make 0x%llx RWX: %s", (unsigned long long)base,
              gtav_proc_last_error());
    return -1;
  }
  return 0;
}
#endif

#if GTAV_LOADER_INSTALL_RENDER_PHASE
#define LOADER_RENDER_PHASE_MAGIC 0x3148505249545447ull
#define LOADER_RENDER_PHASE_UNCONFIGURED 0ull
#define LOADER_RENDER_PHASE_INSTALL_PENDING 2ull
#define LOADER_RENDER_PHASE_INSTALLED 3ull
#define LOADER_RENDER_PHASE_RESTORED 5ull
#define LOADER_RENDER_PHASE_FAILED 6ull

typedef struct LoaderRenderPhaseState {
  uint64_t magic, abi, size, phase;
  uint64_t slot, object, original, wrapper;
  uint64_t services, installs, restores, wrapper_calls;
  uint64_t draw_attempts, draw_completed, duplicate_epochs, gate_rejections;
  uint64_t first_epoch, last_epoch, last_flags, last_object, error;
} LoaderRenderPhaseState;

typedef struct LoaderRenderPhaseInstall {
  uintptr_t state;
  uintptr_t object;
  uintptr_t slot;
  uintptr_t wrapper;
} LoaderRenderPhaseInstall;

static int loader_render_phase_read(void* context, uintptr_t address, void* output, size_t size) {
  const int pid = *(const int*)context;
  return gtav_proc_read(pid, address, output, size);
}

static int prepare_render_phase(int pid, uint64_t expected_token, const GtavBuildPin* pin,
                                uintptr_t base, const uint8_t* elf, size_t elf_len,
                                size_t image_size, LoaderRenderPhaseInstall* install) {
  GtavRenderPhaseDiscovery first, second;
  LoaderRenderPhaseState state, check;
  uint64_t state_off = 0, wrapper_off = 0;
  uint64_t phase = LOADER_RENDER_PHASE_INSTALL_PENDING;
  uint8_t object[40];
  memset(install, 0, sizeof(*install));
  if (!pin || strcmp(pin->target_id, "PPSA04264_01.010.002_DISC") != 0 ||
      !loader_instance_token_matches(pid, expected_token, "render phase discovery")) {
    gtav_logf("render-phase: build/instance prerequisite failed");
    return -1;
  }
  if (gtav_render_phase_discover(loader_render_phase_read, &pid, &first) != 0 ||
      gtav_render_phase_discover(loader_render_phase_read, &pid, &second) != 0 ||
      first.object != second.object || first.slot != second.slot || first.groups != second.groups ||
      first.nodes != second.nodes || first.matches != 1u || second.matches != 1u) {
    gtav_logf(
        "render-phase: exact task discovery failed/changed (first err=%u matches=%u, second "
        "err=%u matches=%u)",
        first.error, first.matches, second.error, second.matches);
    return -1;
  }
  if (gtav_elf_symbol_value(elf, elf_len, "gtav_render_phase_state", &state_off) != 0 ||
      gtav_elf_symbol_value(elf, elf_len, "gtav_render_phase_wrapper", &wrapper_off) != 0 ||
      image_size < sizeof(state) || state_off > image_size - sizeof(state) ||
      wrapper_off >= image_size) {
    gtav_logf("render-phase: worker lacks bounded phase symbols");
    return -1;
  }
  install->state = base + (uintptr_t)state_off;
  install->object = first.object;
  install->slot = first.slot;
  install->wrapper = base + (uintptr_t)wrapper_off;
  if (gtav_proc_read(pid, install->state, &state, sizeof(state)) != 0 ||
      gtav_proc_read(pid, install->object, object, sizeof(object)) != 0) {
    gtav_logf("render-phase: state/object read failed: %s", gtav_proc_last_error());
    return -1;
  }
  uint64_t object_vtable = 0, object_original = 0;
  uint32_t object_task = 0;
  memcpy(&object_vtable, object, sizeof(object_vtable));
  memcpy(&object_task, object + 16, sizeof(object_task));
  memcpy(&object_original, object + 32, sizeof(object_original));
  if (state.magic != LOADER_RENDER_PHASE_MAGIC || state.abi != 1u || state.size != sizeof(state) ||
      state.phase != LOADER_RENDER_PHASE_UNCONFIGURED || state.slot || state.object ||
      state.original != GTAV_RENDER_PHASE_DISCOVERY_ORIGINAL || state.wrapper != install->wrapper ||
      state.error || object_vtable != GTAV_RENDER_PHASE_DISCOVERY_LEAF_VTABLE ||
      object_task != GTAV_RENDER_PHASE_DISCOVERY_TASK_ID ||
      object_original != GTAV_RENDER_PHASE_DISCOVERY_ORIGINAL) {
    gtav_logf("render-phase: pristine worker/live object identity failed");
    return -1;
  }
  if (gtav_proc_write(pid, install->state + offsetof(LoaderRenderPhaseState, slot), &install->slot,
                      sizeof(install->slot)) != 0 ||
      gtav_proc_write(pid, install->state + offsetof(LoaderRenderPhaseState, object),
                      &install->object, sizeof(install->object)) != 0 ||
      gtav_proc_write(pid, install->state + offsetof(LoaderRenderPhaseState, phase), &phase,
                      sizeof(phase)) != 0 ||
      gtav_proc_read(pid, install->state, &check, sizeof(check)) != 0 ||
      check.phase != LOADER_RENDER_PHASE_INSTALL_PENDING || check.slot != install->slot ||
      check.object != install->object || check.wrapper != install->wrapper || check.error) {
    gtav_logf("render-phase: worker configuration write/readback failed: %s",
              gtav_proc_last_error());
    return -1;
  }
  gtav_logf("render-phase: armed unique group-2 object=0x%lx slot=0x%lx wrapper=0x%lx",
            (unsigned long)install->object, (unsigned long)install->slot,
            (unsigned long)install->wrapper);
  return 0;
}

static int verify_render_phase(int pid, uint64_t expected_token,
                               const LoaderRenderPhaseInstall* install) {
  LoaderRenderPhaseState state;
  uint64_t slot = 0;
  uint32_t waited_ms;
  for (waited_ms = 0; waited_ms <= 3000u; waited_ms += 100u) {
    memset(&state, 0, sizeof(state));
    if (!loader_instance_token_matches(pid, expected_token, "render phase verification") ||
        gtav_proc_read(pid, install->state, &state, sizeof(state)) != 0 ||
        gtav_proc_read(pid, install->slot, &slot, sizeof(slot)) != 0) {
      gtav_logf("render-phase: installation verification read failed after %ums: %s", waited_ms,
                gtav_proc_last_error());
      return -1;
    }
    if (state.magic != LOADER_RENDER_PHASE_MAGIC || state.abi != 1u ||
        state.size != sizeof(state) || state.slot != install->slot ||
        state.object != install->object || state.original != GTAV_RENDER_PHASE_DISCOVERY_ORIGINAL ||
        state.wrapper != install->wrapper ||
        (slot != GTAV_RENDER_PHASE_DISCOVERY_ORIGINAL && slot != install->wrapper)) {
      gtav_logf(
          "render-phase: installation contract changed (phase=%llu installs=%llu error=%llu "
          "slot=0x%llx expected=0x%lx)",
          (unsigned long long)state.phase, (unsigned long long)state.installs,
          (unsigned long long)state.error, (unsigned long long)slot,
          (unsigned long)install->wrapper);
      return -1;
    }
    if (state.phase == LOADER_RENDER_PHASE_INSTALLED && state.installs == 1u && !state.error &&
        slot == install->wrapper) {
      gtav_logf("render-phase: installed and verified slot=0x%lx services=%llu after %ums",
                (unsigned long)install->slot, (unsigned long long)state.services, waited_ms);
      return 0;
    }
    if (state.phase == LOADER_RENDER_PHASE_FAILED || state.error ||
        state.phase != LOADER_RENDER_PHASE_INSTALL_PENDING || state.installs > 1u) {
      break;
    }
#if GTAV_PAYLOAD_PERSISTENT
    // The broker is already committed. Keep lifecycle ownership alive while allowing the next
    // frame-hook service to perform the worker's in-process CAS; a daemon-stop is handled after
    // run_inject returns and must not strand this installation mid-handshake.
    daemon_lock_refresh();
#endif
    if (waited_ms < 3000u) usleep(100000);
  }
  {
    gtav_logf(
        "render-phase: installation verification failed after %ums (phase=%llu installs=%llu "
        "error=%llu slot=0x%llx expected=0x%lx)",
        waited_ms > 3000u ? 3000u : waited_ms, (unsigned long long)state.phase,
        (unsigned long long)state.installs, (unsigned long long)state.error,
        (unsigned long long)slot, (unsigned long)install->wrapper);
    return -1;
  }
}
#endif

// `preallocated`/`reserved` describe memory the caller already owns in the target -- the cave
// bootstrap's mapping on the ptrace-free lane. 0 allocates as before.
#if GTAV_LOADER_CAVE_INJECT
// Obtain the worker's memory from inside the game, once per injection. The mapping belongs to the
// game process, not to the loader, so the persistent daemon has to re-acquire it for every instance
// it serves -- there is nothing to carry across a relaunch.
static int cave_acquire_region(int pid, const GtavBuildPin* pin, uint64_t* base_out,
                               uint64_t* reserved_out) {
  GtavCaveBootstrapResult cave;
  *base_out = 0;
  *reserved_out = 0;
  if (gtav_cave_bootstrap_probe(pid, pin, (uint64_t)(GTAV_LOADER_CAVE_ADDR),
                                (uint32_t)(GTAV_LOADER_CAVE_ALLOC),
                                (uint32_t)(GTAV_LOADER_CAVE_TIMEOUT_MS), &cave) != 0) {
    gtav_logf("cave: bootstrap FAILED (ran=%d alloc=0x%llx restored=%d); not injecting",
              cave.stub_ran, (unsigned long long)cave.allocation, cave.restored);
    return -1;
  }
  *base_out = cave.allocation;
  *reserved_out = (uint64_t)(GTAV_LOADER_CAVE_ALLOC);
  gtav_logf("cave: 0x%llx (%llu bytes) acquired with no ptrace; mapping the worker into it",
            (unsigned long long)*base_out, (unsigned long long)*reserved_out);
  return 0;
}
#endif

static int run_inject(int pid, uint64_t expected_token, const GtavBuildPin* pin,
                      uint64_t preallocated, uint64_t reserved, uint64_t* quarantine_token_out,
                      LoaderResidentInstall* resident_out) {
  GtavElfInjectResult res;
  LoaderResidentInstall resident;
  uint8_t* buf = NULL;
  long len = 0;
  int rc;
#if GTAV_LOADER_INSTALL_RENDER_PHASE
  LoaderRenderPhaseInstall render_phase_install;
#endif

  memset(&resident, 0, sizeof(resident));
  if (resident_out != NULL) memset(resident_out, 0, sizeof(*resident_out));
#if GTAV_PAYLOAD_INJECT_GUARD
  uint64_t lock_token = 0;
#endif

  if (quarantine_token_out != NULL) {
    *quarantine_token_out = expected_token;
  }

#if GTAV_PAYLOAD_PERSISTENT
  if (daemon_should_stop()) {
    gtav_logf("inject: stop requested before target transaction; aborting");
    return -1;
  }
#endif

  if (!loader_instance_token_matches(pid, expected_token, "inject start")) {
    return -1;
  }

#if GTAV_LOADER_VERIFY_VERSION
  // The running build was already bound to a build pin by bind_build(). The production cave lane
  // reached this point without changing target credentials or filesystem roots. `pin` is never
  // NULL on the supported-inject path, and the frame-hook contract is enforced against it.
#endif

  if (load_worker_elf(&buf, &len) != 0) {
    return -1;
  }
  gtav_logf("inject: loaded %s worker (%ld bytes); mapping into pid=%d",
            GTAV_MENU_EMBEDDED_WORKER ? "embedded" : "staged", len, pid);

#if GTAV_PAYLOAD_INJECT_GUARD
  {
    int claim_rc = claim_inject(pid, expected_token, &lock_token);
    if (claim_rc == GTAV_INJECT_CLAIM_BUSY) {
      gtav_logf("inject: another transaction is still preparing pid=%d; reporting BUSY", pid);
      free(buf);
      return GTAV_INJECT_BUSY;
    }
    if (claim_rc == GTAV_INJECT_CLAIM_ALREADY) {
      gtav_logf(
          "inject: worker lock already held in pid=%d; health was not reconciled, refusing success",
          pid);
      free(buf);
      return GTAV_INJECT_ALREADY_LOADED;
    }
    if (claim_rc != GTAV_INJECT_CLAIMED) {
      gtav_logf("inject: failed to claim worker lock for pid=%d", pid);
      free(buf);
      return -1;
    }
  }
#endif

#if GTAV_PAYLOAD_PERSISTENT
  if (daemon_should_stop()) {
    gtav_logf("inject: stop requested before worker mapping; aborting");
#if GTAV_PAYLOAD_INJECT_GUARD
    release_inject_if_owned(pid, expected_token, lock_token);
    release_claim_guard(pid);
#endif
    free(buf);
    return -1;
  }
#endif

  if (!loader_instance_token_matches(pid, expected_token, "worker map")) {
#if GTAV_PAYLOAD_INJECT_GUARD
    release_inject_if_owned(pid, expected_token, lock_token);
    release_claim_guard(pid);
#endif
    free(buf);
    return -1;
  }

  if (preallocated != 0) {
    // Ptrace-free lane: the memory already exists and its pages are resident (the bootstrap stub
    // faulted every one of them in), which is what the kernel write path needs. Nothing here
    // allocates, so there is no ambiguous allocation to quarantine on failure either.
    rc = cave_region_make_rwx(pid, preallocated, reserved);
    if (rc == 0) {
      rc = gtav_elf_map_and_relocate_at(pid, buf, (size_t)len, expected_token,
                                        (uintptr_t)preallocated, reserved, &res);
    } else {
      memset(&res, 0, sizeof(res));
      res.status = -1;
      snprintf(res.error, sizeof(res.error), "cave region 0x%llx could not be made RWX",
               (unsigned long long)preallocated);
    }
  } else {
    rc = gtav_elf_map_and_relocate(pid, buf, (size_t)len, expected_token, &res);
  }
  {
    uint64_t post_token = gtav_proc_app_id(pid);
    if (post_token == 0 || post_token != expected_token) {
      if (quarantine_token_out != NULL) {
        *quarantine_token_out = post_token;
      }
      gtav_logf(
          "inject: instance changed/unknown during ELF map pid=%d token=0x%llx->0x%llx "
          "base=0x%lx allocation_may_exist=%u",
          pid, (unsigned long long)expected_token, (unsigned long long)post_token,
          (unsigned long)res.base, res.allocation_may_exist);
      if (res.allocation_may_exist != 0) {
#if GTAV_PAYLOAD_INJECT_GUARD
        release_claim_guard_if_protected(pid, lock_token);
#endif
        free(buf);
        return GTAV_INJECT_PARTIAL_MAP_FAILED;
      }
#if GTAV_PAYLOAD_INJECT_GUARD
      release_inject_if_owned(pid, expected_token, lock_token);
      release_claim_guard(pid);
#endif
      free(buf);
      return -1;
    }
  }
  if (rc != 0) {
    gtav_logf("inject: map/relocate FAILED (stage %d): %s", res.status, res.error);
    if (res.allocation_may_exist != 0) {
      gtav_logf("inject: remote allocation may remain (base=0x%lx observed=%s); retaining lock",
                (unsigned long)res.base, res.base != 0 ? "yes" : "no");
#if GTAV_PAYLOAD_INJECT_GUARD
      release_claim_guard_if_protected(pid, lock_token);
#endif
      free(buf);
      return GTAV_INJECT_PARTIAL_MAP_FAILED;
    }
#if GTAV_PAYLOAD_INJECT_GUARD
    release_inject_if_owned(pid, expected_token, lock_token);
    release_claim_guard(pid);
#endif
    free(buf);
    return -1;
  }
  gtav_logf("inject: mapped base=0x%lx relocs=%u imports=%u", (unsigned long)res.base,
            res.relocs_applied, res.imports_resolved);

#if GTAV_PAYLOAD_PERSISTENT && GTAV_MENU_INSTALL_PATCH_BROKER
  // A persistent production install is accepted only if the daemon can later address every block
  // needed for exact retirement. Resolve them from the exact ELF bytes that were just mapped, not
  // from a mutable host-side artifact.
  {
    uint64_t broker_off = 0, mailbox_off = 0, status_off = 0;
    if (gtav_elf_symbol_value(buf, (size_t)len, "gtav_patch_broker_state", &broker_off) != 0 ||
        gtav_elf_symbol_value(buf, (size_t)len, "gtav_menu_command_mailbox", &mailbox_off) != 0 ||
        gtav_elf_symbol_value(buf, (size_t)len, "gtav_menu_status", &status_off) != 0 ||
        res.image_size < sizeof(GtavPatchBrokerState) ||
        res.image_size < sizeof(GtavMenuCommandMailbox) ||
        res.image_size < sizeof(GtavMenuStatus) ||
        broker_off > (uint64_t)(res.image_size - sizeof(GtavPatchBrokerState)) ||
        mailbox_off > (uint64_t)(res.image_size - sizeof(GtavMenuCommandMailbox)) ||
        status_off > (uint64_t)(res.image_size - sizeof(GtavMenuStatus))) {
      gtav_logf("inject: persistent worker lacks bounded lifecycle symbols; retaining partial map");
#if GTAV_PAYLOAD_INJECT_GUARD
      release_claim_guard_if_protected(pid, lock_token);
#endif
      free(buf);
      return GTAV_INJECT_PARTIAL_MAP_FAILED;
    }
    resident.pid = pid;
    resident.token = expected_token;
    resident.pin = pin;
    resident.base = res.base;
    resident.image_size = res.image_size;
    resident.broker = res.base + (uintptr_t)broker_off;
    resident.mailbox = res.base + (uintptr_t)mailbox_off;
    resident.status = res.base + (uintptr_t)status_off;
  }
#endif

  // Publish the worker's control blocks as ABSOLUTE addresses. The host tooling used to recompute
  // these from `base` plus offsets read out of its LOCAL copy of the worker ELF, which is wrong
  // whenever that copy is not byte-identical to the staged one: a rebuild with different flags can
  // produce the same file SIZE with symbols 0x4000 apart, so the size check cannot catch it and the
  // host reads the wrong memory and reports a healthy worker as missing. The loader already knows
  // the answers, so it states them.
  {
    static const char* const kBlocks[] = {"gtav_menu_status", "gtav_menu_command_mailbox",
                                          "gtav_menu_log_ring"};
    static const char* const kNames[] = {"status", "mailbox", "logring"};
    char line[192];
    size_t used = 0;
    unsigned i;
    line[0] = '\0';
    for (i = 0; i < sizeof(kBlocks) / sizeof(kBlocks[0]); ++i) {
      uint64_t off = 0;
      int n;
      if (gtav_elf_symbol_value(buf, (size_t)len, kBlocks[i], &off) != 0) {
        continue;
      }
      n = snprintf(line + used, sizeof(line) - used, "%s%s=0x%llx", used ? " " : "", kNames[i],
                   (unsigned long long)(res.base + off));
      if (n <= 0 || (size_t)n >= sizeof(line) - used) {
        break;
      }
      used += (size_t)n;
    }
    if (used != 0) {
      gtav_logf("inject: blocks %s", line);
    }
  }

#if GTAV_LOADER_INSTALL_RENDER_PHASE
  // Configure only worker-owned data here. The live callback slot remains untouched until the
  // existing frame hook fires in-process and render_phase_service performs its exact CAS.
  if (prepare_render_phase(pid, expected_token, pin, res.base, buf, (size_t)len, res.image_size,
                           &render_phase_install) != 0) {
    gtav_logf("render-phase: refusing broker commit because phase preparation failed");
#if GTAV_PAYLOAD_INJECT_GUARD
    release_claim_guard_if_protected(pid, lock_token);
#endif
    free(buf);
    return GTAV_INJECT_PARTIAL_MAP_FAILED;
  }
#if GTAV_PAYLOAD_PERSISTENT
  resident.render_phase_state = render_phase_install.state;
  resident.render_phase_slot = render_phase_install.slot;
  resident.render_phase_wrapper = render_phase_install.wrapper;
#endif
#endif

#if GTAV_MENU_INSTALL_PATCH_BROKER
  // Broker-first: commit the frame-hook jump before any worker thread exists. A mismatch
  // here refuses with nothing resident, so a stale worker binary can never crash a relaunched
  // process of a different game version.
  if (run_install_broker(pid, expected_token, pin, res.base, buf, (size_t)len, res.image_size,
                         1 /* worker_free */) != 0) {
    gtav_logf(
        "broker: frame-hook install failed before worker start; no resident worker to retire; "
        "injection is partial and will not report success");
    if (quarantine_token_out != NULL) {
      *quarantine_token_out = gtav_proc_app_id(pid);
    }
#if GTAV_PAYLOAD_INJECT_GUARD
    release_claim_guard_if_protected(pid, lock_token);
#endif
    free(buf);
    return GTAV_INJECT_PARTIAL_BROKER_FAILED;
  }
#endif

#if GTAV_LOADER_CAVE_INJECT
  // The last ptrace site. On this lane the worker starts its own thread from the first frame-hook
  // fire (GTAV_FRAME_HOOK_SELF_START_WORKER), which the broker install above has just made live, so
  // there is nothing for the loader to do here.
  rc = -1;
  gtav_logf("inject: worker self-starts from the frame hook (no ptrace thread start)");
  // pthread_create mid-frame, on the game thread, inside a chained native is the one step of this
  // route that has never run on hardware, so read its result rather than inferring it from ticks:
  // 0xffffffff means the hook never reached the self-start at all, which is a different failure
  // from pthread_create refusing.
  {
    uint64_t rcval = 0;
    if (gtav_elf_symbol_value(buf, (size_t)len, "gtav_frame_hook_self_start_rc", &rcval) == 0) {
      uint32_t self_rc = 0xFFFFFFFFu;
      uint32_t waited_ms;
      for (waited_ms = 0; waited_ms < 3000u; waited_ms += 100u) {
        usleep(100000);
        if (gtav_proc_read(pid, (uintptr_t)(res.base + rcval), &self_rc, sizeof(self_rc)) != 0) {
          break;
        }
        if (self_rc != 0xFFFFFFFFu) {
          break;
        }
      }
      if (self_rc == 0) {
        rc = 0;
        gtav_logf("inject: self-start OK after %ums (pthread_create on the game thread)",
                  waited_ms);
      } else if (self_rc == 0xFFFFFFFFu) {
        gtav_logf("inject: self-start NEVER RAN after %ums -- the hook is not reaching the worker",
                  waited_ms);
      } else {
        gtav_logf("inject: self-start FAILED rc=%u after %ums", self_rc, waited_ms);
      }
    } else {
      gtav_logf(
          "inject: worker has no gtav_frame_hook_self_start_rc symbol; build it with "
          "FRAME_HOOK_SELF_START_WORKER=1");
    }
  }
#else
  rc = gtav_elf_start_thread(pid, buf, (size_t)len, "gtav_menu_start_thread", 0, expected_token,
                             &res);
#endif
  {
    uint64_t post_token = gtav_proc_app_id(pid);
    if (post_token == 0 || post_token != expected_token) {
      if (quarantine_token_out != NULL) {
        *quarantine_token_out = post_token;
      }
      gtav_logf(
          "inject: instance changed/unknown before worker thread start pid=%d token=0x%llx->0x%llx",
          pid, (unsigned long long)expected_token, (unsigned long long)post_token);
      if (res.allocation_may_exist != 0) {
#if GTAV_PAYLOAD_INJECT_GUARD
        release_claim_guard_if_protected(pid, lock_token);
#endif
        free(buf);
        return GTAV_INJECT_PARTIAL_MAP_FAILED;
      }
#if GTAV_PAYLOAD_INJECT_GUARD
      release_inject_if_owned(pid, expected_token, lock_token);
      release_claim_guard(pid);
#endif
      free(buf);
      return -1;
    }
  }
  if (rc != 0) {
    gtav_logf("inject: start worker thread FAILED (stage %d): %s", res.status, res.error);
    // The broker jump is already committed, but the gateway continuation was pre-loaded
    // to chain to the original handler, so the target native remains safe. Quarantine.
    if (quarantine_token_out != NULL) {
      *quarantine_token_out = gtav_proc_app_id(pid);
    }
#if GTAV_PAYLOAD_INJECT_GUARD
    release_claim_guard_if_protected(pid, lock_token);
#endif
    free(buf);
    return GTAV_INJECT_PARTIAL_BROKER_FAILED;
  }
#if GTAV_PAYLOAD_PERSISTENT && GTAV_MENU_INSTALL_PATCH_BROKER
  // From this point the worker can service STOP and exact restoration. Publish the handles before
  // later certification/probes so a partial result cannot leave a live frame hook ownerless.
  resident.valid = 1;
  if (resident_out != NULL) *resident_out = resident;
#endif
#if GTAV_LOADER_INSTALL_RENDER_PHASE
  if (verify_render_phase(pid, expected_token, &render_phase_install) != 0) {
    gtav_logf("render-phase: worker is resident but the default renderer was not certified live");
    if (quarantine_token_out != NULL) *quarantine_token_out = gtav_proc_app_id(pid);
#if GTAV_PAYLOAD_INJECT_GUARD
    release_claim_guard_if_protected(pid, lock_token);
#endif
    free(buf);
    return GTAV_INJECT_PARTIAL_BROKER_FAILED;
  }
#endif
#if GTAV_LOADER_CAVE_INJECT
  // No thread was started here, so res.entry/res.tid are 0 and printing them reads like a failure.
  gtav_logf("inject: worker mapped at base=0x%lx; its thread is its own", (unsigned long)res.base);
#else
  gtav_logf("inject: started worker thread base=0x%lx entry=0x%lx tid=%d", (unsigned long)res.base,
            (unsigned long)res.entry, res.tid);
#endif

  // Diagnostic readback of gtav_menu_status (layout from abi.h):
  //   magic@0 (u64): present => the image mapped.
  //   state@16 (u32): nonzero => the worker thread executed past its early setup.
  //   ticks@24 (u64): climbs every worker loop => the menu loop is alive.
  // Reading twice ~1s apart separates "didn't map" / "crashed early" / "running".
  {
    uint64_t stval = 0;
    if (gtav_elf_symbol_value(buf, (size_t)len, "gtav_menu_status", &stval) == 0) {
      uint8_t a[32];
      uint8_t b[32];
      uintptr_t addr = res.base + stval;
      uint64_t magic = 0, ticks0 = 0, ticks1 = 0;
      uint32_t state = 0;
      memset(a, 0, sizeof(a));
      memset(b, 0, sizeof(b));
      if (!loader_instance_token_matches(pid, expected_token, "worker probe initial")) {
        if (quarantine_token_out != NULL) {
          *quarantine_token_out = gtav_proc_app_id(pid);
        }
#if GTAV_PAYLOAD_INJECT_GUARD
        release_claim_guard_if_protected(pid, lock_token);
#endif
        free(buf);
        return GTAV_INJECT_PARTIAL_MAP_FAILED;
      }
      gtav_proc_read(pid, addr, a, sizeof(a));
#if GTAV_PAYLOAD_PERSISTENT
      // The broker is committed and the worker is running. A daemon-stop arriving here must wait
      // until run_inject publishes the resident retirement handles; treating cancellation as a
      // partial install would abandon a live .text jump with no exact-restoration authority.
      daemon_lock_refresh();
      usleep(1000000);
#else
      usleep(1000000);
#endif
      if (!loader_instance_token_matches(pid, expected_token, "worker probe final")) {
        if (quarantine_token_out != NULL) {
          *quarantine_token_out = gtav_proc_app_id(pid);
        }
#if GTAV_PAYLOAD_INJECT_GUARD
        release_claim_guard_if_protected(pid, lock_token);
#endif
        free(buf);
        return GTAV_INJECT_PARTIAL_MAP_FAILED;
      }
      gtav_proc_read(pid, addr, b, sizeof(b));
      memcpy(&magic, a, sizeof(magic));
      memcpy(&state, a + 16, sizeof(state));
      memcpy(&ticks0, a + 24, sizeof(ticks0));
      memcpy(&ticks1, b + 24, sizeof(ticks1));
      gtav_logf("inject: probe@0x%lx magic=0x%llx state=%u ticks=%llu->%llu", (unsigned long)addr,
                (unsigned long long)magic, state, (unsigned long long)ticks0,
                (unsigned long long)ticks1);
    } else {
      gtav_logf("inject: probe symbol gtav_menu_status not found");
    }
  }

  free(buf);

#if GTAV_PAYLOAD_INJECT_GUARD
  // Publish a reclaimable process-start record only after every mapping/broker/module stage
  // completed and both the app id and process-start token still identify the launch we claimed.
  // Until this exact point the lock remains QUARANTINED, so a partial map can never be retried
  // merely because the pid was recycled.
  {
    uint64_t final_token = gtav_proc_app_id(pid);
    uint64_t final_lock_token = 0;
    if (quarantine_token_out != NULL) {
      *quarantine_token_out = final_token;
    }
    if (final_token == 0 || final_token != expected_token) {
      gtav_logf(
          "inject: instance changed/unknown before lock finalization pid=%d token=0x%llx->0x%llx; "
          "retaining quarantine",
          pid, (unsigned long long)expected_token, (unsigned long long)final_token);
      release_claim_guard_if_protected(pid, lock_token);
      return GTAV_INJECT_PARTIAL_MAP_FAILED;
    }
    if (read_bound_process_start_token(pid, expected_token, "lock finalization",
                                       &final_lock_token) != 0 ||
        final_lock_token != lock_token) {
      gtav_logf(
          "inject: process-start identity changed/unknown before lock finalization "
          "pid=%d start=0x%llx->0x%llx; retaining quarantine",
          pid, (unsigned long long)lock_token, (unsigned long long)final_lock_token);
      release_claim_guard_if_protected(pid, lock_token);
      return GTAV_INJECT_PARTIAL_MAP_FAILED;
    }
    if (gtav_inject_lock_finalize(pid, lock_token) != 0) {
      // The installed worker is healthy; only lifecycle re-arm is degraded. The transaction state
      // remains fail-closed (or finalization landed but readback failed), so never claim that a
      // later loader may safely stack another image.
      gtav_logf(
          "inject: worker installed but lock finalization was not verified; retaining quarantine");
    }
    release_claim_guard_if_protected(pid, lock_token);
  }
#else
  if (!loader_instance_token_matches(pid, expected_token, "inject complete")) {
    if (quarantine_token_out != NULL) {
      *quarantine_token_out = gtav_proc_app_id(pid);
    }
    return GTAV_INJECT_PARTIAL_MAP_FAILED;
  }
#endif

  resident.valid = 1;
  if (resident_out != NULL) *resident_out = resident;
  return 0;
}
#endif

#if GTAV_MENU_PAYLOAD_INJECT && GTAV_PAYLOAD_PERSISTENT
// ---- Persistent-daemon lifecycle: stop sentinel + single-instance lock. Side-effecting file ops;
// the pure decisions (same-instance, staleness) live in daemon_lifecycle.h so they are host-tested.
// Declared before find_game_wait / main so both can heartbeat the lock and honour a stop request.
// ----

static GtavDaemonControl g_daemon_control = {.owner_fd = -1};
static volatile sig_atomic_t g_daemon_stop_requested;
#if GTAV_MANAGED_RUNTIME
static GtavSupervisorRuntimeLease g_managed_runtime = {.supervisor_fd = -1};

static void managed_runtime_cleanup(void) {
  gtav_supervisor_runtime_release(&g_managed_runtime);
}
#endif

static void daemon_signal_stop(int signal_number) {
  (void)signal_number;
  g_daemon_stop_requested = 1;
}

// True if the host has dropped the stop sentinel (menu-ctl daemon-stop). Best effort; NOT deleted
// here -- a fresh daemon clears it once at startup so a leftover sentinel can't wedge the next run.
static int daemon_should_stop(void) {
  if (g_daemon_stop_requested || gtav_daemon_control_should_stop(&g_daemon_control)) return 1;
#if GTAV_MANAGED_RUNTIME
  if (!gtav_supervisor_runtime_alive(&g_managed_runtime)) return 1;
#endif
  return 0;
}

// Bump the daemon lock's mtime as a liveness heartbeat (existing lock only; best effort). Called on
// every poll tick -- idle, relaunch-wait, and SP-ready wait -- so a live daemon's lease never
// lapses.
static void daemon_lock_refresh(void) {
  gtav_daemon_control_refresh(&g_daemon_control);
}

// Sleep in short slices so a stop sentinel is honoured promptly even during a multi-second
// readiness interval or retry backoff. Keeping this here centralises both cancellation and daemon
// lock heartbeats; callers treat a non-zero return as "leave persistent mode now".
static int daemon_wait_interruptible(unsigned long wait_us) {
  const unsigned long slice_max_us = 100000UL;
  unsigned long remaining_us = wait_us;

  for (;;) {
    daemon_lock_refresh();
    if (daemon_should_stop()) {
      return -1;
    }
    if (remaining_us == 0) {
      return 0;
    }
    {
      unsigned long slice_us = remaining_us < slice_max_us ? remaining_us : slice_max_us;
      usleep((useconds_t)slice_us);
      remaining_us -= slice_us;
    }
  }
}

#if GTAV_MENU_INSTALL_PATCH_BROKER
#define LOADER_RETIRE_OK 0
#define LOADER_RETIRE_NOT_STOPPED 1
#define LOADER_RETIRE_TARGET_GONE 2
#define LOADER_RETIRE_RETRY (-1)

static int resident_instance_state(const LoaderResidentInstall* resident) {
  uint64_t current;
  if (resident == NULL || !resident->valid || resident->pid <= 0 || resident->token == 0)
    return LOADER_RETIRE_TARGET_GONE;
  current = gtav_proc_app_id(resident->pid);
  if (current == 0 || current != resident->token) return LOADER_RETIRE_TARGET_GONE;
  return loader_target_is_live(resident->pid) ? LOADER_RETIRE_OK : LOADER_RETIRE_RETRY;
}

static int resident_read_status(const LoaderResidentInstall* resident, GtavMenuStatus* status) {
  if (!loader_image_contains(resident->base, resident->image_size, resident->status,
                             sizeof(*status)) ||
      gtav_proc_read_nostop(resident->pid, resident->status, status, sizeof(*status)) != 0 ||
      status->magic != GTAV_MENU_STATUS_MAGIC ||
      status->abi_version != GTAV_MENU_STATUS_ABI_VERSION ||
      status->struct_size != sizeof(*status)) {
    gtav_logf("retire: worker status identity/read failed: %s", gtav_proc_last_error());
    return -1;
  }
  return 0;
}

static void resident_retire_wait(void) {
  daemon_lock_refresh();
  usleep(100000);
}

// Ask the worker to run its normal shutdown path. The command fields are written and read back
// before request_sequence is published, matching the host mailbox protocol. A STOPPED status is
// stronger than a mailbox acknowledgement: it proves phase/input/native cleanup finished.
static int resident_ensure_stopped(LoaderResidentInstall* resident, int request_stop) {
  GtavMenuStatus status;
  GtavMenuCommandMailbox mailbox;
  uint64_t sequence;
  int poll;

  if (resident_read_status(resident, &status) != 0) return LOADER_RETIRE_RETRY;
  if (status.state == GTAV_MENU_STATE_STOPPED) return LOADER_RETIRE_OK;
  if (!request_stop) return LOADER_RETIRE_NOT_STOPPED;
  if (status.state == GTAV_MENU_STATE_ERROR || status.state == GTAV_MENU_STATE_ZERO) {
    gtav_logf("retire: refusing STOP request from invalid worker state=%u error=%u", status.state,
              status.last_error);
    return LOADER_RETIRE_RETRY;
  }

  if (!loader_image_contains(resident->base, resident->image_size, resident->mailbox,
                             sizeof(mailbox))) {
    gtav_logf("retire: mailbox address is outside the mapped worker");
    return LOADER_RETIRE_RETRY;
  }
  for (poll = 0; poll < 20; ++poll) {
    int instance = resident_instance_state(resident);
    if (instance != LOADER_RETIRE_OK) return instance;
    if (gtav_proc_read_nostop(resident->pid, resident->mailbox, &mailbox, sizeof(mailbox)) != 0 ||
        mailbox.magic != GTAV_MENU_COMMAND_MAILBOX_MAGIC ||
        mailbox.abi_version != GTAV_MENU_COMMAND_MAILBOX_ABI_VERSION ||
        mailbox.struct_size != sizeof(mailbox)) {
      gtav_logf("retire: mailbox identity/read failed: %s", gtav_proc_last_error());
      return LOADER_RETIRE_RETRY;
    }
    if (mailbox.request_sequence == mailbox.ack_sequence) break;
    resident_retire_wait();
  }
  if (mailbox.request_sequence != mailbox.ack_sequence) {
    gtav_logf("retire: mailbox remained busy at sequence=%llu",
              (unsigned long long)mailbox.request_sequence);
    return LOADER_RETIRE_RETRY;
  }

  sequence = mailbox.request_sequence > mailbox.ack_sequence ? mailbox.request_sequence
                                                             : mailbox.ack_sequence;
  if (++sequence == 0) sequence = 1;
  {
    struct {
      uint32_t command;
      uint32_t status;
      uint64_t argument;
    } fields = {GTAV_MENU_COMMAND_STOP, GTAV_MENU_COMMAND_STATUS_PENDING, 0};
    if (gtav_proc_write_nostop(resident->pid,
                               resident->mailbox + offsetof(GtavMenuCommandMailbox, command),
                               &fields, sizeof(fields)) != 0 ||
        gtav_proc_read_nostop(resident->pid, resident->mailbox, &mailbox, sizeof(mailbox)) != 0 ||
        mailbox.command != GTAV_MENU_COMMAND_STOP ||
        mailbox.status != GTAV_MENU_COMMAND_STATUS_PENDING || mailbox.argument != 0 ||
        gtav_proc_write_nostop(
            resident->pid, resident->mailbox + offsetof(GtavMenuCommandMailbox, request_sequence),
            &sequence, sizeof(sequence)) != 0 ||
        gtav_proc_read_nostop(resident->pid, resident->mailbox, &mailbox, sizeof(mailbox)) != 0 ||
        mailbox.request_sequence != sequence) {
      gtav_logf("retire: STOP publication/readback failed: %s", gtav_proc_last_error());
      return LOADER_RETIRE_RETRY;
    }
  }
  gtav_logf("retire: STOP published sequence=%llu; waiting for worker STOPPED",
            (unsigned long long)sequence);

  for (poll = 0; poll < 100; ++poll) {
    int instance = resident_instance_state(resident);
    if (instance != LOADER_RETIRE_OK) return instance;
    if (resident_read_status(resident, &status) != 0) return LOADER_RETIRE_RETRY;
    if (status.state == GTAV_MENU_STATE_STOPPED) {
      gtav_logf("retire: worker STOPPED ticks=%llu hook_status=%u",
                (unsigned long long)status.ticks, status.hook_status);
      return LOADER_RETIRE_OK;
    }
    if (status.state == GTAV_MENU_STATE_ERROR) {
      gtav_logf("retire: worker entered ERROR=%u while stopping", status.last_error);
      return LOADER_RETIRE_RETRY;
    }
    resident_retire_wait();
  }
  gtav_logf("retire: timed out waiting for worker STOPPED (state=%u)", status.state);
  return LOADER_RETIRE_RETRY;
}

#if GTAV_LOADER_INSTALL_RENDER_PHASE
static int resident_verify_phase_restored(const LoaderResidentInstall* resident) {
  LoaderRenderPhaseState phase;
  uint64_t slot = 0;
  memset(&phase, 0, sizeof(phase));
  if (!loader_image_contains(resident->base, resident->image_size, resident->render_phase_state,
                             sizeof(phase)) ||
      resident->render_phase_slot == 0 || resident->render_phase_wrapper == 0 ||
      gtav_proc_read_nostop(resident->pid, resident->render_phase_state, &phase, sizeof(phase)) !=
          0 ||
      gtav_proc_read_nostop(resident->pid, resident->render_phase_slot, &slot, sizeof(slot)) != 0 ||
      phase.magic != LOADER_RENDER_PHASE_MAGIC || phase.abi != 1u || phase.size != sizeof(phase) ||
      phase.phase != LOADER_RENDER_PHASE_RESTORED || phase.error ||
      phase.slot != resident->render_phase_slot ||
      phase.wrapper != resident->render_phase_wrapper || slot != phase.original ||
      phase.original != GTAV_RENDER_PHASE_DISCOVERY_ORIGINAL) {
    gtav_logf(
        "retire: render phase is not exactly restored (phase=%llu error=%llu slot=0x%llx "
        "original=0x%llx)",
        (unsigned long long)phase.phase, (unsigned long long)phase.error, (unsigned long long)slot,
        (unsigned long long)phase.original);
    return -1;
  }
  gtav_logf("retire: render phase exact-restored slot=0x%lx -> 0x%llx",
            (unsigned long)resident->render_phase_slot, (unsigned long long)slot);
  return 0;
}
#endif

// Restore the loader-owned PLAYER_PED_ID prologue only after the worker is STOPPED and the phase
// callback is exact-original. The no-stop kernel copy path ignores execute-only protection, so
// retirement never mutates the target mapping's protection and cannot fall back to PT_ATTACH.
static int resident_restore_broker(LoaderResidentInstall* resident) {
  GtavPatchBrokerState broker;
  uint8_t live[GTAV_PATCH_BROKER_MAX_BYTES];
  uint8_t installed[GTAV_PATCH_BROKER_MAX_BYTES];
  uint64_t jump_destination = 0;
  uint32_t restored = GTAV_PATCH_BROKER_STATE_RESTORED;
  uint32_t no_error = GTAV_PATCH_BROKER_ERROR_NONE;
  char note[GTAV_PATCH_BROKER_NOTE_LEN];
  int attempt;
  int exact = 0;

  if (resident->pin == NULL ||
      !loader_image_contains(resident->base, resident->image_size, resident->broker,
                             sizeof(broker)) ||
      gtav_proc_read_nostop(resident->pid, resident->broker, &broker, sizeof(broker)) != 0) {
    gtav_logf("retire: broker identity/read failed: %s", gtav_proc_last_error());
    return LOADER_RETIRE_RETRY;
  }
  memcpy(&jump_destination, broker.jump + 6, sizeof(jump_destination));
  if (broker.magic != GTAV_PATCH_BROKER_MAGIC ||
      broker.abi_version != GTAV_PATCH_BROKER_ABI_VERSION || broker.struct_size != sizeof(broker) ||
      (broker.state != GTAV_PATCH_BROKER_STATE_READY &&
       broker.state != GTAV_PATCH_BROKER_STATE_INSTALLED &&
       broker.state != GTAV_PATCH_BROKER_STATE_RESTORED) ||
      broker.request_kind != GTAV_PATCH_BROKER_REQUEST_FRAME_HOOK ||
      broker.target_addr != resident->pin->broker_target ||
      broker.continuation_addr != resident->pin->broker_continuation ||
      broker.patch_len != resident->pin->broker_patch_len ||
      broker.stolen_len != resident->pin->broker_stolen_len ||
      broker.expected_len != broker.stolen_len || broker.restore_len != broker.stolen_len ||
      broker.jump_len != broker.patch_len || broker.restore_len < 14u ||
      broker.restore_len > GTAV_PATCH_BROKER_MAX_BYTES ||
      memcmp(broker.expected, resident->pin->broker_expected, broker.restore_len) != 0 ||
      memcmp(broker.original, resident->pin->broker_expected, broker.restore_len) != 0 ||
      memcmp(broker.jump, "\xff\x25\x00\x00\x00\x00", 6) != 0 ||
      jump_destination != broker.thunk_addr ||
      !loader_image_contains(resident->base, resident->image_size, (uintptr_t)broker.thunk_addr,
                             1u) ||
      !loader_text_write_ok(resident->pin, (uintptr_t)broker.target_addr, broker.restore_len)) {
    gtav_logf(
        "retire: broker differs from the loader-owned install contract; refusing .text write");
    return LOADER_RETIRE_RETRY;
  }
#if GTAV_LOADER_INSTALL_RENDER_PHASE
  if (resident_verify_phase_restored(resident) != 0) return LOADER_RETIRE_RETRY;
#endif
  {
    int instance = resident_instance_state(resident);
    if (instance != LOADER_RETIRE_OK) return instance;
  }

  memset(installed, 0, sizeof(installed));
  memcpy(installed, broker.original, broker.restore_len);
  memcpy(installed, broker.jump, broker.jump_len);
  if (gtav_proc_read_nostop(resident->pid, (uintptr_t)broker.target_addr, live,
                            broker.restore_len) != 0) {
    gtav_logf("retire: live prologue read failed: %s", gtav_proc_last_error());
    return LOADER_RETIRE_RETRY;
  }
  if (memcmp(live, broker.original, broker.restore_len) == 0) {
    exact = 1;
  } else if (memcmp(live, installed, broker.restore_len) != 0) {
    gtav_logf(
        "retire: live prologue is neither exact installed nor exact original; refusing write");
    return LOADER_RETIRE_RETRY;
  }

  // Once an original-byte write is attempted, finish or retry the same restoration regardless of
  // the daemon stop flag. Only a proven process-instance change stops further writes.
  for (attempt = 0; attempt < 6 && !exact; ++attempt) {
    if (attempt) resident_retire_wait();
    if (!loader_instance_token_matches(resident->pid, resident->token, "broker retirement"))
      return LOADER_RETIRE_TARGET_GONE;
    if (gtav_proc_write_nostop(resident->pid, (uintptr_t)broker.target_addr, broker.original,
                               broker.restore_len) == 0 &&
        gtav_proc_read_nostop(resident->pid, (uintptr_t)broker.target_addr, live,
                              broker.restore_len) == 0 &&
        memcmp(live, broker.original, broker.restore_len) == 0)
      exact = 1;
  }
  if (!exact) {
    gtav_logf("retire: FAILED to restore/verify %u original prologue bytes", broker.restore_len);
    return LOADER_RETIRE_RETRY;
  }

  memset(note, 0, sizeof(note));
  strncpy(note, "worker stopped; phase and frame hook exact-restored", sizeof(note) - 1u);
  if (gtav_proc_write_nostop(resident->pid,
                             resident->broker + offsetof(GtavPatchBrokerState, error), &no_error,
                             sizeof(no_error)) != 0 ||
      gtav_proc_write_nostop(resident->pid, resident->broker + offsetof(GtavPatchBrokerState, note),
                             note, sizeof(note)) != 0 ||
      gtav_proc_write_nostop(resident->pid,
                             resident->broker + offsetof(GtavPatchBrokerState, state), &restored,
                             sizeof(restored)) != 0) {
    gtav_logf("retire: prologue restored but broker publication failed: %s",
              gtav_proc_last_error());
    return LOADER_RETIRE_RETRY;
  }
  gtav_logf(
      "retire: frame-hook exact-restored target=0x%llx bytes=%u protections=untouched ptrace=0",
      (unsigned long long)broker.target_addr, broker.restore_len);
  resident->valid = 0;
  return LOADER_RETIRE_OK;
}

static int resident_retire(LoaderResidentInstall* resident, int request_stop) {
  int instance;
  int stopped;
  if (resident == NULL || !resident->valid) return LOADER_RETIRE_OK;
  instance = resident_instance_state(resident);
  if (instance != LOADER_RETIRE_OK) return instance;
  stopped = resident_ensure_stopped(resident, request_stop);
  if (stopped != LOADER_RETIRE_OK) return stopped;
  return resident_restore_broker(resident);
}
#endif  // GTAV_MENU_INSTALL_PATCH_BROKER

// Request the resident daemon's cooperative exit and wait up to 15 seconds for ownership.
// If it is still finishing work, keep its ownership intact and leave the stop request pending.
static int daemon_claim(void) {
  return gtav_daemon_control_claim(&g_daemon_control, GTAV_MENU_DEFAULT_DIR, 15000);
}

// Drop the daemon lock on exit so the next deploy can claim immediately.
static void daemon_release(void) {
  gtav_daemon_control_release(&g_daemon_control);
}
#endif  // GTAV_MENU_PAYLOAD_INJECT && GTAV_PAYLOAD_PERSISTENT

// Find the game, optionally waiting for it to appear (Phase 1 wait-for-game).
// Returns 0 with *pid_out set once found. With waiting off this is a single probe
// (classic one-shot behaviour); with waiting on it polls every
// GTAV_PAYLOAD_WAIT_POLL_USEC until the game is foreground or the timeout elapses,
// returning -1 only after giving up. Polling uses the foreground big-app query, so
// it never ptraces the target while waiting. In the persistent daemon build it also
// heartbeats the single-instance lock and honours a stop request during the wait, so
// a daemon-stop lands even while GTA is closed (the relaunch-wait window).
static int find_game_wait(const char* title_id, int* pid_out) {
#if GTAV_PAYLOAD_WAIT_FOR_GAME || GTAV_PAYLOAD_PERSISTENT
  const unsigned long timeout_us = (unsigned long)GTAV_PAYLOAD_WAIT_TIMEOUT_SEC * 1000000UL;
  unsigned long waited_us = 0;
  unsigned long next_log_us = 0;  // log a heartbeat on the first miss, then every 30s

  for (;;) {
    if (gtav_proc_find_game(title_id, pid_out) == 0) {
      if (waited_us > 0) {
        gtav_logf("game appeared after %lus of waiting", waited_us / 1000000UL);
      }
      return 0;
    }
    if (waited_us >= next_log_us) {
      gtav_logf("waiting for %s (foreground); waited %lus: %s", title_id, waited_us / 1000000UL,
                gtav_proc_last_error());
      if (next_log_us == 0) {
        gtav_notify("GTAVMenu payload: waiting for GTA V");
      }
      next_log_us = waited_us + 30000000UL;
    }
    if (timeout_us != 0 && waited_us >= timeout_us) {
      gtav_logf("gave up waiting for %s after %lus", title_id, waited_us / 1000000UL);
      return -1;
    }
#if GTAV_MENU_PAYLOAD_INJECT && GTAV_PAYLOAD_PERSISTENT
    // Persistent daemon: keep the single-instance lease fresh while GTA is closed, and let a
    // daemon-stop interrupt this relaunch-wait sleep instead of waiting for the next whole poll.
    if (daemon_wait_interruptible(GTAV_PAYLOAD_WAIT_POLL_USEC) != 0) {
      gtav_logf("persistent: stop requested while waiting for %s; leaving daemon", title_id);
      return -1;
    }
#else
    usleep(GTAV_PAYLOAD_WAIT_POLL_USEC);
#endif
    waited_us += GTAV_PAYLOAD_WAIT_POLL_USEC;
  }
#else
  return gtav_proc_find_game(title_id, pid_out);
#endif
}

#if GTAV_LOADER_TARGET_TRANSACTION
// wait_for_sp_ready() outcomes (negative = do not inject). Defined unconditionally (not under the
// SP_READY gate) so the persistent re-arm in main() can name GTAV_SP_READY_TARGET_GONE even in a
// build without the gate compiled in, where wait_for_sp_ready() simply returns OK.
#define GTAV_SP_READY_OK 0
#define GTAV_SP_READY_GAVE_UP (-1)
#define GTAV_SP_READY_TARGET_GONE (-2)
#define GTAV_SP_READY_STOP_REQUESTED (-3)

#ifndef GTAV_PAYLOAD_SP_DEAD_FAILS
#define GTAV_PAYLOAD_SP_DEAD_FAILS 3u  // consecutive anchor-read failures before a liveness probe
#endif

// Block until the configured player-world/load readiness predicate is stable (see
// GTAV_PAYLOAD_SP_READY), then return GTAV_SP_READY_OK so the caller may inject. Returns OK
// immediately when the gate is off. Returns GTAV_SP_READY_GAVE_UP only if a finite timeout elapses
// without the anchor satisfying, or GTAV_SP_READY_TARGET_GONE if the target pid dies/recycles while
// we wait. A persistent read failure is checked against the cheap non-ptrace liveness probe, so a
// vanished target is never injected.
static int wait_for_sp_ready(int pid, const GtavBuildPin* pin) {
#if GTAV_PAYLOAD_SP_READY
  // The ready anchor comes from the bound build pin when one is selected (per-version
  // player-ped anchor), falling back to the compile-time default for bare/research builds.
  const uintptr_t ready_addr = (pin && pin->sp_ready_addr)
                                   ? (uintptr_t)pin->sp_ready_addr
                                   : (uintptr_t)(GTAV_PAYLOAD_SP_READY_ADDR);
  const size_t ready_deref_off = (pin && pin->sp_ready_addr)
                                     ? (size_t)pin->sp_ready_deref_offset
                                     : (size_t)(GTAV_PAYLOAD_SP_READY_DEREF_OFFSET);
  const uint64_t mask = (uint64_t)(GTAV_PAYLOAD_SP_READY_MASK);
  const uint64_t want = (uint64_t)(GTAV_PAYLOAD_SP_READY_VALUE)&mask;
  const size_t ready_size = (size_t)GTAV_PAYLOAD_SP_READY_SIZE;  // compile-time limited to <= 8
  const unsigned long timeout_us = (unsigned long)GTAV_PAYLOAD_SP_READY_TIMEOUT_SEC * 1000000UL;
  unsigned long waited_us = 0;
  unsigned long next_log_us = 0;
  unsigned consecutive_fail = 0u;
  unsigned consecutive_ready = 0u;
#if GTAV_PAYLOAD_SP_READY_GENTLE
  unsigned long poll_us = (unsigned long)GTAV_PAYLOAD_SP_READY_POLL_USEC;  // adaptive; backs off
#else
  const unsigned long poll_us = (unsigned long)GTAV_PAYLOAD_WAIT_POLL_USEC;  // fixed proven cadence
#endif

#if GTAV_PAYLOAD_SP_READY_DEREF
  gtav_logf("sp-ready: gating inject on [*0x%llx + 0x%llx]&0x%llx %s 0x%llx (read %d bytes)",
            (unsigned long long)ready_addr, (unsigned long long)ready_deref_off,
            (unsigned long long)mask,
            GTAV_PAYLOAD_SP_READY_MODE ? "!=" : "==", (unsigned long long)want, (int)ready_size);
#else
  gtav_logf("sp-ready: gating inject on [0x%llx]&0x%llx %s 0x%llx (read %d bytes)",
            (unsigned long long)ready_addr, (unsigned long long)mask,
            GTAV_PAYLOAD_SP_READY_MODE ? "!=" : "==", (unsigned long long)want, (int)ready_size);
#endif
#if GTAV_PAYLOAD_SP_READY_GENTLE
  gtav_logf("sp-ready: gentle poll on (base %luus, backoff cap %luus; %luus once ready)",
            (unsigned long)GTAV_PAYLOAD_SP_READY_POLL_USEC,
            (unsigned long)GTAV_PAYLOAD_SP_READY_POLL_MAX_USEC,
            (unsigned long)GTAV_PAYLOAD_WAIT_POLL_USEC);
#endif

  for (;;) {
    uint64_t raw = 0;
    uintptr_t value_addr = ready_addr;
    int reachable;       // the anchor/base read reached the target (vs. a ptrace failure)
    int have_value = 0;  // we obtained a value to compare this poll
#if GTAV_PAYLOAD_PERSISTENT
    // Refresh the lease and notice daemon-stop before every ptrace read. The subsequent sleep is
    // interruptible too, so even the gentle 5s cadence never delays cancellation by a full poll.
    if (daemon_wait_interruptible(0) != 0) {
      gtav_logf("sp-ready: stop requested; abandoning readiness wait for pid=%d", pid);
      return GTAV_SP_READY_STOP_REQUESTED;
    }
#endif
#if GTAV_PAYLOAD_SP_READY_DEREF
    // One pointer hop: base = *(anchor); compare *(base + offset). A null base means the
    // player-manager singleton is not up yet -- the target is ALIVE, just not in-world yet, so
    // it counts as a successful (reachable) read, NOT a failure. Conflating "null base" with
    // "ptrace read failed" made the entire early-boot window trip the dead-target liveness probe
    // (a full pid scan) every few seconds and mislabel a booting game as "unreadable".
    uint64_t base = 0;
#if GTAV_PAYLOAD_SP_READY_GENTLE
    // Single-attach chained read: fold base=*(anchor) and value=*(base+off) into ONE whole-game
    // stop instead of two, halving the per-poll freeze on top of the backoff cadence. Semantics
    // (reachable / null-base / value-read) match the two-read path below.
    {
      int value_read = 0;
      reachable = (gtav_proc_read_chain(pid, value_addr, ready_deref_off, ready_size, &base, &raw,
                                        &value_read) == 0);
      have_value = value_read ? 1 : 0;
    }
#else
    reachable = (gtav_proc_read(pid, value_addr, &base, sizeof(base)) == 0);
    if (reachable && base != 0) {
      value_addr = (uintptr_t)base + ready_deref_off;
      have_value = (gtav_proc_read(pid, value_addr, &raw, ready_size) == 0);
    }
#endif
#else
    reachable = (gtav_proc_read(pid, value_addr, &raw, ready_size) == 0);
    have_value = reachable;
#endif

    // Liveness keys on whether the TARGET was reachable, never on readiness: a reachable-but-not-
    // ready poll (null base / value below threshold) resets the dead-target counter. Only genuine
    // consecutive ptrace failures -- what a dead/recycled pid looks like (ESRCH) -- escalate to the
    // cheap non-ptrace liveness probe, so a vanished target is abandoned instead of polled forever
    // (the watch timeout defaults to 0 = indefinite) while a booting game is left in peace.
    if (reachable) {
      consecutive_fail = 0u;
    } else if (++consecutive_fail >= GTAV_PAYLOAD_SP_DEAD_FAILS) {
      if (!loader_target_is_live(pid)) {
        gtav_logf("sp-ready: target pid=%d vanished after %lus (%s); abandoning the wait", pid,
                  waited_us / 1000000UL, gtav_proc_last_error());
        return GTAV_SP_READY_TARGET_GONE;
      }
      consecutive_fail = 0u;  // alive but transiently unreadable -> keep waiting
    }

    // Readiness with debounce: require GTAV_PAYLOAD_SP_READY_CONFIRMATIONS consecutive ready polls
    // before injecting so a one-shot transient can't trigger a load-crashing early inject.
    if (have_value) {
      uint64_t v = raw & mask;
      int ready = GTAV_PAYLOAD_SP_READY_MODE ? (v != want) : (v == want);
      if (ready) {
        if (++consecutive_ready >= (unsigned)GTAV_PAYLOAD_SP_READY_CONFIRMATIONS) {
          gtav_logf(
              "sp-ready: satisfied after %lus (read=0x%llx, %u confirmations); settling %luus",
              waited_us / 1000000UL, (unsigned long long)v,
              (unsigned)GTAV_PAYLOAD_SP_READY_CONFIRMATIONS,
              (unsigned long)GTAV_PAYLOAD_SP_READY_SETTLE_USEC);
          if (GTAV_PAYLOAD_SP_READY_SETTLE_USEC) {
#if GTAV_PAYLOAD_PERSISTENT
            if (daemon_wait_interruptible(GTAV_PAYLOAD_SP_READY_SETTLE_USEC) != 0) {
              gtav_logf("sp-ready: stop requested during settle delay for pid=%d", pid);
              return GTAV_SP_READY_STOP_REQUESTED;
            }
#else
            usleep(GTAV_PAYLOAD_SP_READY_SETTLE_USEC);
#endif
          }
          return GTAV_SP_READY_OK;
        }
      } else {
        consecutive_ready = 0u;  // fell back below the threshold -> restart the streak
      }
    } else {
      consecutive_ready = 0u;  // could not compare this poll -> restart the streak
    }

    if (waited_us >= next_log_us) {
      if (have_value) {
        gtav_logf("sp-ready: not yet (waited %lus; anchor=0x%llx, ready streak %u/%u)",
                  waited_us / 1000000UL, (unsigned long long)(raw & mask), consecutive_ready,
                  (unsigned)GTAV_PAYLOAD_SP_READY_CONFIRMATIONS);
      } else if (reachable) {
        gtav_logf("sp-ready: not yet (waited %lus; player-manager singleton not up)",
                  waited_us / 1000000UL);
      } else {
        gtav_logf("sp-ready: not yet (waited %lus; anchor unreadable: %s)", waited_us / 1000000UL,
                  gtav_proc_last_error());
      }
      next_log_us = waited_us + 30000000UL;
    }
    if (timeout_us != 0 && waited_us >= timeout_us) {
      gtav_logf("sp-ready: timed out after %lus; not injecting", waited_us / 1000000UL);
      return GTAV_SP_READY_GAVE_UP;
    }
#if GTAV_PAYLOAD_PERSISTENT
    if (daemon_wait_interruptible(poll_us) != 0) {
      gtav_logf("sp-ready: stop requested during readiness backoff for pid=%d", pid);
      return GTAV_SP_READY_STOP_REQUESTED;
    }
#else
    usleep((useconds_t)poll_us);
#endif
    waited_us += poll_us;
#if GTAV_PAYLOAD_SP_READY_GENTLE
    // Adapt the NEXT interval: once the ped has appeared (a ready streak is building) poll fast so
    // the remaining confirmations resolve in ~1s each; otherwise keep backing off toward the cap.
    if (consecutive_ready > 0u) {
      poll_us = (unsigned long)GTAV_PAYLOAD_WAIT_POLL_USEC;
    } else if (poll_us < (unsigned long)GTAV_PAYLOAD_SP_READY_POLL_MAX_USEC) {
      poll_us += (unsigned long)GTAV_PAYLOAD_SP_READY_POLL_USEC;
      if (poll_us > (unsigned long)GTAV_PAYLOAD_SP_READY_POLL_MAX_USEC) {
        poll_us = (unsigned long)GTAV_PAYLOAD_SP_READY_POLL_MAX_USEC;
      }
    }
#endif
  }
#else
  (void)pid;
  (void)pin;
  return GTAV_SP_READY_OK;  // no load-readiness gate compiled in: caller injects directly
#endif
}
#endif  // GTAV_LOADER_TARGET_TRANSACTION

#ifdef GTAV_PAYLOAD_ENTRY
int GTAV_PAYLOAD_ENTRY(void) {
#else
int main(void) {
#endif
  int pid = -1;

  // FreeBSD's special tid -1 changes the process name shown by the payload host.
  syscall(SYS_thr_set_name, -1, "gtav-menu.elf");
#if GTAV_MENU_PAYLOAD_INJECT && GTAV_PAYLOAD_PERSISTENT
  signal(SIGTERM, daemon_signal_stop);
  signal(SIGINT, daemon_signal_stop);
#endif

  mkdir("/data", 0777);
  mkdir(GTAV_MENU_DEFAULT_DIR, 0777);
  gtav_log_open(GTAV_MENU_DEFAULT_LOG);
  gtav_notify("GTAVMenu payload loader started");
  gtav_logf("payload loader starting (sdk-native process backend)");

#if GTAV_MANAGED_RUNTIME
  if (gtav_supervisor_runtime_attach(&g_managed_runtime, GTAV_MENU_DEFAULT_DIR) != 0) {
    gtav_logf("managed runtime: no live Toolbox supervisor lease: %s", strerror(errno));
    gtav_log_close();
    return 1;
  }
  if (atexit(managed_runtime_cleanup) != 0) {
    gtav_logf("managed runtime: could not register lifecycle cleanup");
    gtav_supervisor_runtime_release(&g_managed_runtime);
    gtav_log_close();
    return 1;
  }
  gtav_logf("managed runtime: attached to supervisor session=%016llx",
            (unsigned long long)g_managed_runtime.token);
#endif

  if (gtav_proc_backend_init() != 0) {
    gtav_logf("backend init failed: %s", gtav_proc_last_error());
    gtav_notify("GTAVMenu payload: no kernel R/W");
    gtav_log_close();
    return 1;
  }
  gtav_logf("backend init ok");

#if GTAV_MENU_PAYLOAD_INJECT && GTAV_PAYLOAD_PERSISTENT
  // Ownership and stale-sentinel clearing are serialised with other deployments.
  gtav_logf("persistent: claiming ownership; requesting any previous daemon to stop");
  if (daemon_claim() != 0) {
    gtav_logf("persistent: replacement failed (%s); previous ownership left intact",
              strerror(errno));
    gtav_notify("GTAVMenu daemon replacement failed");
    gtav_log_close();
    return 1;
  }
  gtav_logf("persistent: ownership acquired pid=%d", getpid());
#if GTAV_MANAGED_RUNTIME
  if (gtav_supervisor_runtime_publish_ack(&g_managed_runtime, getpid()) != 0) {
    gtav_logf("managed runtime: supervisor disappeared before ready acknowledgement");
    daemon_release();
    gtav_log_close();
    return 1;
  }
  gtav_logf("managed runtime: ready acknowledgement published");
#endif
#endif

  if (find_game_wait(GTAV_PAYLOAD_TARGET_TITLE_ID, &pid) != 0) {
    gtav_logf("game not found: %s", gtav_proc_last_error());
    gtav_notify("GTAVMenu payload: game not running");
#if GTAV_MENU_PAYLOAD_INJECT && GTAV_PAYLOAD_PERSISTENT
    daemon_release();  // stop requested (or wait timed out) before GTA ever appeared
    gtav_logf("persistent: daemon exited");
#endif
    gtav_log_close();
    return 0;
  }
  gtav_logf("found game %s pid=%d", GTAV_PAYLOAD_TARGET_TITLE_ID, pid);

#if GTAV_LOADER_TARGET_TRANSACTION
#if GTAV_PAYLOAD_PERSISTENT
  {
    // Dormant daemon: wait for SP-ready, inject, then stay resident and re-arm on each relaunch.
    // Re-arm keys on the game INSTANCE (pid + app-id token, gtav_daemon_same_instance), NOT the raw
    // pid: if the console recycles the pid for the relaunched game, a pid-only check would idle
    // forever and never re-inject. `pid` holds the first foreground GTA from find_game_wait above.
    int served_pid = -1;
    uint64_t served_token = 0;
    LoaderResidentInstall resident;
    memset(&resident, 0, sizeof(resident));
    for (;;) {
      uint64_t cur_token = gtav_proc_app_id(pid);
      daemon_lock_refresh();  // heartbeat the single-instance lease each iteration (idle cadence)
      if (daemon_should_stop()) {
#if GTAV_MENU_INSTALL_PATCH_BROKER
        int retire_rc = resident_retire(&resident, 1);
        if (retire_rc == LOADER_RETIRE_OK || retire_rc == LOADER_RETIRE_TARGET_GONE) {
          gtav_logf("persistent: stop requested; resident retirement complete");
          break;
        }
        gtav_logf("persistent: stop requested; exact retirement pending rc=%d", retire_rc);
        resident_retire_wait();
        continue;
#else
        gtav_logf("persistent: stop requested; exiting daemon");
        break;
#endif
      }
#if GTAV_MENU_INSTALL_PATCH_BROKER
      if (resident.valid &&
          !gtav_daemon_same_instance(pid, cur_token, resident.pid, resident.token)) {
        int old_state = resident_instance_state(&resident);
        if (old_state == LOADER_RETIRE_TARGET_GONE) {
          gtav_logf("persistent: prior resident instance is gone; dropping lifecycle handles");
          memset(&resident, 0, sizeof(resident));
        } else {
          gtav_logf("persistent: prior resident still exists; refusing a second mapped instance");
          resident_retire_wait();
          pid = resident.pid;
          continue;
        }
      }
#endif
      if (!gtav_daemon_same_instance(pid, cur_token, served_pid, served_token)) {
        // A fresh instance (new pid, or a recycled pid now running a new game): bind the build
        // pin first (auto-detect which supported GTA V build is live, reject unsupported builds
        // before any worker map/privileged write), then gate on stable player-world readiness
        // using that build's anchor, then inject. This does not prove Story/offline mode.
        int sp;
        const GtavBuildPin* pin = NULL;
        int injectable = 1;
        int unsupported = 0;
        gtav_logf("persistent: serving new game pid=%d token=0x%llx", pid,
                  (unsigned long long)cur_token);
#if GTAV_LOADER_VERIFY_VERSION
        {
          int bind_rc = bind_build(pid, cur_token, &pin);
          if (bind_rc == GTAV_BUILD_BIND_ERROR) {
            gtav_logf("persistent: build detect failed for pid=%d (%s); idling until relaunch", pid,
                      gtav_proc_last_error());
            injectable = 0;
          } else if (bind_rc == GTAV_BUILD_BIND_UNSUPPORTED) {
            gtav_logf("persistent: pid=%d is an unsupported GTA V build; not injecting", pid);
            unsupported = 1;
            injectable = 0;
          }
        }
#endif
        if (!injectable) {
          // Leave the game untouched. Serve this instance so the daemon idles until a NEW
          // instance/relaunch instead of re-probing a known-bad build every loop.
          gtav_notify(unsupported ? "GTAVMenu: unsupported GTA V build"
                                  : "GTAVMenu payload: build detect failed");
          served_pid = pid;
          served_token = cur_token;
          goto persistent_wait_instance;
        }
        sp = wait_for_sp_ready(pid, pin);
        if (sp == GTAV_SP_READY_OK) {
          uint64_t inject_token = gtav_proc_app_id(pid);
          uint64_t quarantine_token = inject_token;
          int ir;
          int retry_inject = 0;
          if (cur_token == 0 || inject_token == 0 || inject_token != cur_token) {
            gtav_logf(
                "persistent: game instance changed/unknown across readiness pid=%d "
                "token=0x%llx->0x%llx; restarting gate",
                pid, (unsigned long long)cur_token, (unsigned long long)inject_token);
            if (daemon_wait_interruptible(GTAV_PAYLOAD_RETRY_BACKOFF_USEC) != 0) {
              gtav_logf("persistent: stop requested during instance-change backoff");
              break;
            }
            continue;
          }
          {
            uint64_t cave_base = 0;
            uint64_t cave_reserved = 0;
#if GTAV_LOADER_CAVE_INJECT
            // Per instance: the previous game's mapping died with its process.
            if (cave_acquire_region(pid, pin, &cave_base, &cave_reserved) != 0) {
              // Serve this instance so the daemon idles until the next relaunch rather than
              // re-installing the stub every loop against a game that cannot take it.
              gtav_notify("GTAVMenu cave bootstrap failed");
              gtav_logf("persistent: cave bootstrap failed for pid=%d; idling until relaunch", pid);
              served_pid = pid;
              served_token = cur_token;
              goto persistent_wait_instance;
            }
#endif
            ir = run_inject(pid, inject_token, pin, cave_base, cave_reserved, &quarantine_token,
                            &resident);
          }
          if (ir == 0) {
            gtav_notify("GTAVMenu injected");
            gtav_logf("inject: overall OK (pid=%d)", pid);
            served_pid = pid;  // re-arm: idle until a DIFFERENT instance appears
            served_token = quarantine_token;
          } else if (ir == GTAV_INJECT_PARTIAL_BROKER_FAILED) {
            // The worker is already mapped, so retrying cannot safely repair this instance without
            // remote unmap/reconciliation support. Quarantine it until the next game launch while
            // keeping the result an explicit failure (never a false "overall OK").
            gtav_notify("GTAVMenu inject partial; broker failed");
            gtav_logf("inject: overall PARTIAL_BROKER_FAILED (pid=%d)", pid);
            gtav_logf("persistent: partial broker install for pid=%d; awaiting a new game instance",
                      pid);
            served_pid = pid;
            served_token = quarantine_token;
          } else if (ir == GTAV_INJECT_PARTIAL_MAP_FAILED) {
            // The injector reserved remote memory before a later stage failed. Without a verified
            // remote unmap, retrying could stack images or start against corrupted partial state.
            gtav_notify("GTAVMenu inject partial; mapping retained");
            gtav_logf("inject: overall PARTIAL_MAP_FAILED (pid=%d)", pid);
            gtav_logf("persistent: partial map for pid=%d; awaiting a new game instance", pid);
            served_pid = pid;
            served_token = quarantine_token;
          } else if (ir == GTAV_INJECT_ALREADY_LOADED) {
            // A live lock proves only that some worker owns this instance, not that its broker,
            // build, or heartbeat is healthy. Avoid a retry loop, but never promote it to success
            // without a future reconciliation path.
            gtav_notify("GTAVMenu worker present; health unknown");
            gtav_logf("inject: overall ALREADY_UNRECONCILED (pid=%d)", pid);
            gtav_logf(
                "persistent: existing worker for pid=%d was not reconciled; awaiting new instance",
                pid);
            served_pid = pid;
            served_token = quarantine_token;
          } else if (ir == GTAV_INJECT_BUSY) {
            // A competing loader/manual route has not yet published a durable Q/I/M outcome.
            // Retry this same instance after backoff; BUSY is not proof that a worker exists.
            gtav_logf("inject: overall BUSY (pid=%d)", pid);
            retry_inject = 1;
          } else {
            gtav_notify("GTAVMenu inject failed");
            gtav_logf("inject: overall FAIL (pid=%d)", pid);
            retry_inject = 1;
          }

          // run_inject defers cancellation across a committed broker transaction. Always publish
          // and classify its completed result before honouring the pending daemon-stop request, so
          // an installed or rolled-back hook is never left without an overall outcome in the log.
          if (daemon_should_stop()) {
            gtav_logf(
                "persistent: stop requested during inject; result recorded; entering retirement");
            continue;
          }
          if (retry_inject) {
            if (daemon_wait_interruptible(GTAV_PAYLOAD_RETRY_BACKOFF_USEC) != 0) {
              gtav_logf("persistent: stop requested during inject retry backoff");
              break;
            }
          }
        } else if (sp == GTAV_SP_READY_STOP_REQUESTED) {
          gtav_logf("persistent: stop requested during SP-ready wait; exiting daemon");
          break;
        } else if (sp == GTAV_SP_READY_TARGET_GONE) {
          gtav_logf("persistent: target pid=%d vanished before SP-ready; awaiting relaunch", pid);
          served_pid = pid;  // mark served so we wait for a DIFFERENT instance, not this dead pid
          served_token = cur_token;
        } else {
          // Finite SP timeout while the game is still up: do NOT abandon a live game (the old code
          // marked it served and never retried). Back off and re-evaluate the same instance.
          gtav_logf("persistent: sp-ready gave up for pid=%d; will retry", pid);
          if (daemon_wait_interruptible(GTAV_PAYLOAD_RETRY_BACKOFF_USEC) != 0) {
            gtav_logf("persistent: stop requested during SP-ready retry backoff");
            break;
          }
        }
      } else {
#if GTAV_MENU_INSTALL_PATCH_BROKER
        // A host `stop` or in-game Stop Runtime reaches STOPPED independently of daemon-stop.
        // Reconcile it here and remove the loader-owned .text detour while the exact instance is
        // still foreground. A running worker is the ordinary idle case.
        if (resident.valid) {
          int retire_rc = resident_retire(&resident, 0);
          if (retire_rc == LOADER_RETIRE_OK) {
            gtav_logf("persistent: stopped worker exact-retired; awaiting relaunch");
          } else if (retire_rc == LOADER_RETIRE_TARGET_GONE) {
            memset(&resident, 0, sizeof(resident));
          }
        }
#endif
        // Still the instance we served: idle, but let daemon-stop break the poll immediately.
        if (daemon_wait_interruptible(GTAV_PAYLOAD_WAIT_POLL_USEC) != 0) {
          gtav_logf("persistent: stop requested while idling; entering exact retirement");
          continue;
        }
      }
    persistent_wait_instance:;
      // Wait for the foreground GTA for the next iteration. In the persistent build find_game_wait
      // also heartbeats the lock and honours a stop request internally, so a quit/relaunch or a
      // daemon-stop both resolve here; it returns non-zero only on stop or the (default-off) wait
      // timeout -> leave the daemon.
      if (find_game_wait(GTAV_PAYLOAD_TARGET_TITLE_ID, &pid) != 0) {
#if GTAV_MENU_INSTALL_PATCH_BROKER
        if (daemon_should_stop() && resident.valid) {
          int retire_rc = resident_retire(&resident, 1);
          if (retire_rc != LOADER_RETIRE_OK && retire_rc != LOADER_RETIRE_TARGET_GONE) {
            gtav_logf(
                "persistent: target is not foreground; retaining daemon until retirement or exit");
            resident_retire_wait();
            pid = resident.pid;
            continue;
          }
        }
#endif
        gtav_logf("persistent: wait ended (stop/timeout); exiting daemon");
        break;
      }
    }
    daemon_release();
    gtav_logf("persistent: daemon exited");
    gtav_log_close();
    return 0;
  }
#else
  {
    // One-shot: bind the build pin (auto-detect supported build, reject unsupported cleanly),
    // gate on SP-ready with that build's anchor, then inject and exit.
    const GtavBuildPin* pin = NULL;
#if GTAV_LOADER_VERIFY_VERSION
    uint64_t bind_token = gtav_proc_app_id(pid);
#endif
#if GTAV_LOADER_VERIFY_VERSION
    {
      int bind_rc = bind_build(pid, bind_token, &pin);
      if (bind_rc == GTAV_BUILD_BIND_ERROR) {
        gtav_logf("build detect failed for pid=%d (%s); not injecting", pid,
                  gtav_proc_last_error());
        gtav_notify("GTAVMenu payload: injection not possible");
        gtav_log_close();
        return 1;
      }
      if (bind_rc == GTAV_BUILD_BIND_UNSUPPORTED) {
        gtav_logf("pid=%d is an unsupported GTA V build; not injecting", pid);
        gtav_notify("GTAVMenu: unsupported GTA V build");
        gtav_log_close();
        return 0;
      }
    }
#endif

#if GTAV_MENU_PAYLOAD_INJECT
    if (wait_for_sp_ready(pid, pin) != GTAV_SP_READY_OK) {
      gtav_logf("sp-ready gave up; not injecting");
      gtav_notify("GTAVMenu payload: player world not ready");
      gtav_log_close();
      return 0;
    }
    {
      uint64_t inject_token = gtav_proc_app_id(pid);
      uint64_t quarantine_token = inject_token;
      uint64_t cave_base = 0;
      uint64_t cave_reserved = 0;
      int ir;
#if GTAV_LOADER_CAVE_INJECT
      // Obtain the worker's memory from inside the game instead of by remote mmap. This runs after
      // sp-ready on purpose: the stub only executes when the game's scripts call the hooked native.
      if (cave_acquire_region(pid, pin, &cave_base, &cave_reserved) != 0) {
        gtav_notify("GTAVMenu cave bootstrap failed");
        gtav_log_close();
        return 1;
      }
#endif
      ir = run_inject(pid, inject_token, pin, cave_base, cave_reserved, &quarantine_token, NULL);
      if (ir == 0) {
        gtav_notify("GTAVMenu injected");
        gtav_logf("inject: overall OK");
      } else if (ir == GTAV_INJECT_ALREADY_LOADED) {
        gtav_notify("GTAVMenu worker present; health unknown");
        gtav_logf("inject: overall ALREADY_UNRECONCILED");
      } else if (ir == GTAV_INJECT_BUSY) {
        gtav_notify("GTAVMenu inject busy; retry later");
        gtav_logf("inject: overall BUSY");
      } else if (ir == GTAV_INJECT_PARTIAL_BROKER_FAILED) {
        gtav_notify("GTAVMenu inject partial; broker failed");
        gtav_logf("inject: overall PARTIAL_BROKER_FAILED");
      } else if (ir == GTAV_INJECT_PARTIAL_MAP_FAILED) {
        gtav_notify("GTAVMenu inject partial; mapping retained");
        gtav_logf("inject: overall PARTIAL_MAP_FAILED");
      } else {
        gtav_notify("GTAVMenu inject failed");
        gtav_logf("inject: overall FAIL");
      }
      gtav_log_close();
      return ir == 0 ? 0 : 1;
    }
#endif
  }
#endif
#endif

  gtav_logf("smoke profile: target discovered; injection disabled");
  gtav_notify("GTAVMenu smoke payload: injection disabled");
  gtav_log_close();
  return 0;
}
