
BUILD_DIR ?= build/ps5
BUILD_PROFILE_DIR := $(BUILD_DIR)/$(GTAV_TARGET)/$(GTAV_BUILD_PROFILE)/$(GTAV_DELIVERY)
BUILD_CONFIG_STAMP := $(BUILD_PROFILE_DIR)/build-config.json
# Standalone payload bundle (etaHEN-free): loader + worker + target metadata.
PAYLOAD_PACKAGE_DIR ?= build/pkg/$(GTAV_TARGET)/standalone
INCLUDES := -Iinclude
WARNINGS := -Wall -Wextra -Werror -Wno-unused-parameter
# Keep debug information and any __FILE__ strings independent of the checkout
# location. Besides making release hashes reproducible, this prevents a packaged
# ELF from disclosing a maintainer's local path.
REPRODUCIBLE_PATH_CFLAGS := -ffile-prefix-map=$(CURDIR)=. -fdebug-prefix-map=$(CURDIR)=. -fdebug-compilation-dir=.
CFLAGS := $(WARNINGS) -g -O2 -ffunction-sections -fdata-sections $(REPRODUCIBLE_PATH_CFLAGS) $(INCLUDES)

# Production build flags. Research-only render, TLS, and streaming experiments live under
# research/legacy and are fixed off in current builds.

# Complete production menu. Address values come from the exact 01.010.002 target
# configuration and its generated native header; drift tests keep both in sync.
FEATURE_MENU_CFLAGS := $(CFLAGS) \
		-DGTAV_MENU_RENDER_DIAGNOSTICS=0 \
		-DGTAV_RENDER_DIAG_INITIAL_MODE=0 \
		-DGTAV_RENDER_DIAG_ALPHA=255 \
		-DGTAV_RENDER_DIAG_BUILD_ID=\"production\" \
		-DGTAV_RENDER_CYCLE_PROBE=0 \
		-DGTAV_RENDER_PATH_PROBE=0 \
		-DGTAV_RENDER_BANK_PROBE=0 \
		-DGTAV_RENDER_PHASE_INTERCEPT=$(RENDER_PHASE_INTERCEPT) \
		-DGTAV_MENU_PHASE_DRAW_LIST=$(GTAV_MENU_PHASE_DRAW_LIST) \
		-DGTAV_MENU_DEFAULT_TARGET_ID=\"$(FEATURE_MENU_TARGET_ID)\" \
		-DGTAV_MENU_DEFAULT_INSTALL_HOOK=0 \
		-DGTAV_MENU_DEFAULT_DRY_RUN=1 \
		-DGTAV_MENU_ENABLE_HOOK_DRY_RUN=0 \
		-DGTAV_MENU_ENABLE_LIVE_HOOK=0 \
		-DGTAV_MENU_ENABLE_NATIVE_FEATURES=$(FEATURE_MENU_ENABLE_NATIVE_FEATURES) \
		-DGTAV_MENU_ENABLE_VEHICLE_PRELOAD=0 \
		-DGTAV_MENU_ENABLE_MODEL_STREAM_OWNER_CENSUS=0 \
		-DGTAV_MENU_ENABLE_MODEL_STREAM_OWNER_FILTER=0 \
		-DGTAV_MENU_ENABLE_MENU_PED_STREAM_REQUEST=0 \
		-DGTAV_MENU_ENABLE_STREAM_FORCE_LOAD=0 \
		-DGTAV_MENU_STREAM_RESIDENT_ONLY=0 \
		-DGTAV_FRAME_HOOK_CAPTURE_SCRIPT_HANDLER=0 \
		-DGTAV_MENU_GATE_MAINTHREAD_ACTIONS=$(FEATURE_MENU_GATE_MAINTHREAD) \
		-DGTAV_MENU_UNLOCK_ALL_ACTIONS=0 \
		-DGTAV_MENU_ENABLE_WORKER_TEXT_OVERLAY=$(GTAV_MENU_ENABLE_WORKER_TEXT_OVERLAY) \
		-DGTAV_MENU_ENABLE_TOASTS=$(GTAV_MENU_ENABLE_TOASTS) \
		-DGTAV_MENU_WORKER_TEXT_OVERLAY_RENDER_LIST=1 \
		-DGTAV_MENU_WORKER_TEXT_OVERLAY_INTERVAL=$(FEATURE_MENU_INTERVAL) \
		-DGTAV_MENU_DEFAULT_NATIVE_CANARY=0 \
		-DGTAV_MENU_DEFAULT_NATIVE_CANARY_MAX_FRAMES=0 \
		-DGTAV_MENU_SMOKE_MAX_TICKS=$(FEATURE_MENU_MAX_TICKS) \
		-DGTAV_MENU_LOG_TICK_INTERVAL=60 \
		-DGTAV_MENU_SKIP_RUNTIME_CONFIG=1 \
		-DGTAV_MENU_DISABLE_NOTIFICATIONS=1 \
		-DGTAV_MENU_DISABLE_LOG_IO=1 \
		-DGTAV_MENU_NO_ROOTDIR=1 \
		-DGTAV_FRAME_HOOK_TLS_SCAN_DIAGNOSTIC=0 \
		-DGTAV_FRAME_HOOK_USE_REAL_FS_BASE=0

# Exact production gameplay-native address header.
GTAV_MENU_NATIVE_ADDRESSES_HEADER ?= "$(GTAV_TARGET_NATIVE_HEADER)"
FEATURE_MENU_CFLAGS += -DGTAV_MENU_NATIVE_ADDRESSES_HEADER=\"$(subst ",,$(GTAV_MENU_NATIVE_ADDRESSES_HEADER))\"
GTAV_MENU_SCRIPT_GLOBALS_HEADER ?= "$(GTAV_TARGET_SCRIPT_HEADER)"
FEATURE_MENU_CFLAGS += -DGTAV_MENU_SCRIPT_GLOBALS_HEADER=\"$(subst ",,$(GTAV_MENU_SCRIPT_GLOBALS_HEADER))\"

# --- Feature opt-ins that depend on the gameplay/native feature layer ----------
# Vehicle-preview image, inline button glyphs, and native Scaleform instructional
# bar all need GTAV_MENU_ENABLE_NATIVE_FEATURES. If that base gate is off, force
# these off fail-closed so the worker can't call feature-layer functions that
# were compiled out.
GTAV_MENU_ENABLE_VEHICLE_PREVIEW ?= 0
# Experimental engine device mount of the custom root as gtavmenu:/ (features/custom_device.inc).
# Hardware-unverified, so off by default; enable with CUSTOM_DEVICE=1 ./menu-ctl.sh cave-inject.
GTAV_MENU_ENABLE_CUSTOM_DEVICE ?= 0
FEATURE_MENU_CFLAGS += -DGTAV_MENU_ENABLE_CUSTOM_DEVICE=$(GTAV_MENU_ENABLE_CUSTOM_DEVICE)
GTAV_MENU_ENABLE_BUTTON_GLYPHS ?= 0
GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM ?= 0

ifeq ($(FEATURE_MENU_ENABLE_NATIVE_FEATURES),0)
GTAV_MENU_ENABLE_VEHICLE_PREVIEW := 0
GTAV_MENU_ENABLE_BUTTON_GLYPHS := 0
GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM := 0
endif

# Vehicle-spawner image preview (bare default OFF; menu-live/menu-ctl cave-inject enable it).
# When on, the highlighted
# vehicle's in-game website thumbnail (scaleform_web.rpf streamed texture dict, keyed by
# model name) is streamed on the game-thread frame-hook drain and drawn in the menu via
# DRAW_SPRITE. The legacy renderer submits it directly; the 01.010 phase renderer copies the
# owned dict/texture names into its immutable list and submits it at the verified callback. Bakes
# the DRAW_SPRITE handler into the bridge
# native table; the streaming natives (REQUEST/HAS/SET_..._NO_LONGER_NEEDED) come from the
# generated header into the feature table.
ifeq ($(GTAV_MENU_ENABLE_VEHICLE_PREVIEW),1)
FEATURE_MENU_CFLAGS += -DGTAV_MENU_ENABLE_VEHICLE_PREVIEW=1
endif

# Real PlayStation button glyphs in the WORKER-drawn fallback instructional bar (bare default OFF).
# With this on, each entry draws the controller's real button glyph inline via GTA's `~INPUT_*~`
# text-token substitution (no chip rect) instead of the coloured ASCII chip; a verified (txd, tex)
# in kHintGlyphInfo, if ever filled, would draw a DRAW_SPRITE glyph ahead of the token. This is the
# FALLBACK tier -- when GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM is also on and its movie is loaded,
# the native Scaleform bar (below) supersedes this worker bar entirely. Bakes DRAW_SPRITE + the
# streamed-texture handshake into the bridge native table (for the optional sprite tier; identical
# to the preview's redefine, so the flags compose). The `~INPUT_*~` glyph rendering still needs
# hardware verification on this firmware.
ifeq ($(GTAV_MENU_ENABLE_BUTTON_GLYPHS),1)
FEATURE_MENU_CFLAGS += -DGTAV_MENU_ENABLE_BUTTON_GLYPHS=1
endif

# Native instructional-button bar: GTA's own "instructional_buttons" Scaleform (bare default OFF).
# This is the EXACT widget the pause menu draws along the bottom, so the menu's controls strip shows
# real DualSense glyphs in the native format (auto X/O swap, native chrome). REQUEST/load/data-slot
# construction runs on the game-thread frame-hook tick. The legacy renderer also submits the
# fullscreen draw there; the 01.010 phase renderer submits only that final draw at the verified
# callback, with a nonblocking reader claim preventing overlap with a slot rebuild. It needs
# GTAV_MENU_ENABLE_NATIVE_FEATURES and GTAV_MENU_ENABLE_FRAME_HOOK. When loaded, the worker-drawn
# fallback bar stands down. menu-live / menu-ctl cave-inject enable it.
ifeq ($(GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM),1)
FEATURE_MENU_CFLAGS += -DGTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM=1
endif

# Worker -> PS5 kernel-log sink (crash capture). The worker's in-memory log ring lives in
# GTA's process memory and is lost when the process dies; the kernel log is kernel-side and
# survives, so route worker log lines there too. Off by default: it issues the raw debug-out
# syscall (601) from inside GTA's sandbox -- proven for SDK payloads but not yet hardware-
# validated from the injected worker. menu-ctl `start --klog` opts in; stream the result with
# `./menu-ctl.sh logs --kernel`.
GTAV_MENU_ENABLE_WORKER_KLOG ?= 0
ifeq ($(GTAV_MENU_ENABLE_WORKER_KLOG),1)
FEATURE_MENU_CFLAGS += -DGTAV_MENU_ENABLE_WORKER_KLOG=1
endif

# Teardown/suspend guard (src/module/quit_guard.c). The worker watches the frame-hook heartbeat
# and parks -- defensively restores the scePad GOT swap + stands the live input path down -- before
# a force-close or rest-mode suspend tears GTA down, the teardown gtav_menu_shutdown() never sees.
# The decision logic is the pure, host-tested teardown_watchdog.h. The production profile enables
# it; the explicit smoke profile disables it.
#
# QUIT_GUARD_APPSTATE additionally polls the PS5 system-service background flag
# (sceSystemServiceGetStatus) as a fast, explicit teardown/suspend signal on top of the heartbeat
# fallback, and links the worker against -lSceSystemService. The symbol + status-struct layout are
# firmware-specific and need in-game verification (the guard logs the first few raw reads), so it is
# SEPARATELY gated -- disable with GTAV_MENU_QUIT_GUARD_APPSTATE=0 if the start build won't link or
# the flag reads wrong on hardware.
GTAV_MENU_ENABLE_QUIT_GUARD ?= 0
GTAV_MENU_QUIT_GUARD_APPSTATE ?= 0
QUIT_GUARD_CFLAGS :=
QUIT_GUARD_LDLIBS :=
ifeq ($(GTAV_MENU_ENABLE_QUIT_GUARD),1)
QUIT_GUARD_CFLAGS += -DGTAV_MENU_ENABLE_QUIT_GUARD=1
ifeq ($(GTAV_MENU_QUIT_GUARD_APPSTATE),1)
QUIT_GUARD_CFLAGS += -DGTAV_MENU_QUIT_GUARD_APPSTATE=1
QUIT_GUARD_LDLIBS += -lSceSystemService
endif
endif

LDFLAGS := -Wl,--gc-sections

# Opt-in direct DualSense input via scePad (src/module/pad_input.c). Off by
# default: PAD_INPUT_CFLAGS/PAD_INPUT_LDLIBS expand to nothing, so default
# builds are unchanged and pad_input.c compiles as a no-op. Build a controller-
# navigable payload with ENABLE_PAD_INPUT=1, which compiles the scePad path and
# links it against the ScePad / SceUserService modules already loaded in GTA.
# PAD_HOOK=1 additionally installs the scePadReadState GOT-swap so the menu
# suppresses the game's own controller input while visible (no phone-on-DpadUp).
# It implies ENABLE_PAD_INPUT and self-restores on shutdown.
ENABLE_PAD_INPUT ?= 0
PAD_HOOK ?= 0
ifeq ($(PAD_HOOK),1)
ENABLE_PAD_INPUT := 1
endif
ifeq ($(ENABLE_PAD_INPUT),1)
PAD_INPUT_CFLAGS := -DGTAV_MENU_ENABLE_PAD_INPUT=1
ifeq ($(PAD_HOOK),1)
PAD_INPUT_CFLAGS += -DGTAV_MENU_ENABLE_PAD_HOOK=1
# The GOT-swap flips RELRO pages writable in-process; that needs Sony's authorized
# sceKernelMprotect (libkernel origin), not libc mprotect (a raw syscall the PS5 kernel
# rejects as "directly issued" -> fatal SYSTEM_ILLEGAL_FUNCTION_CALL). Same call detour.c
# uses to patch .text (GTAV_DETOUR_SCE_MPROTECT).
PAD_INPUT_CFLAGS += -DGTAV_MENU_PAD_HOOK_SCE_MPROTECT=1
endif
PAD_INPUT_LDLIBS := -lScePad -lSceUserService
else
PAD_INPUT_CFLAGS :=
PAD_INPUT_LDLIBS :=
endif

FEATURE_MENU_FRAME_HOOK_PLAYERPED_ELF := $(BUILD_PROFILE_DIR)/gtav-menu-feature-menu.elf
FEATURE_MENU_TARGET_CFLAGS := $(shell $(PYTHON) tools/feature_menu_target_cflags.py \
	--target $(GTAV_TARGET) \
	$(if $(filter 1,$(GTAV_MENU_ENABLE_VEHICLE_PREVIEW)),--include-preview) \
	$(if $(filter 1,$(GTAV_MENU_ENABLE_BUTTON_GLYPHS)),--include-glyphs))
# Drain spawn jobs only when the frame-hook's context probe confirms a real script-VM
# thread (capture_probe_context). 1 = gated (entity natives like CREATE_VEHICLE need
# script context; without this they crash when the hooked native fires from an engine
# thread, e.g. the [GTA] ControlMgr Update Thread). 0 = drain on every fire (diagnostic
# only -- read g_probe_calls_with_context without spawning).
FRAME_HOOK_REQUIRE_CONTEXT ?= 1
# The worker starts its own thread from the first frame-hook fire instead of the loader injecting one
# by ptrace -- the last PT_ATTACH site in an inject. Pair with PAYLOAD_LOADER_CAVE_INJECT=1, which
# stops the loader starting a thread; with both off or both on exactly one starter exists, and with
# only this one on the worker would be double-loaded. See GTAV_FRAME_HOOK_SELF_START_WORKER.
FRAME_HOOK_SELF_START_WORKER ?= 0

COMMON_SRCS := \
	src/common/detour.c \
	src/common/feature_profile.c \
	src/common/hex.c \
	src/common/ini_parse.c \
	src/common/log.c \
	src/common/notify.c \
	src/common/rootdir.c \
	src/common/runtime_config.c \
	src/common/strutil.c

# Standalone, etaHEN-free payload loader. Launched by a generic PS5 payload loader
# (PS5_DEPLOY, port 9021); the SDK CRT binds kernel R/W before main(), then this
# uses the SDK-native process-control backend (proc_backend_sdk.c) to reach GTA.
# Read the build-pin signature via the debug-memory path (mdbg) instead of ptrace+protect.
# 1 = no PT_ATTACH and no .text protection change during build detection. See GTAV_LOADER_PROBE_NOSTOP
# in src/payload_loader/main.c and docs/ptrace-free-injection.md.
PAYLOAD_LOADER_PROBE_NOSTOP ?= 0
# Route ALL plain target reads/writes through the debug-memory path (mdbg) instead of ptrace, so an
# inject stops the game only where it must (remote mmap, remote thread start) instead of ~840 times.
# Falls back to ptrace (with a log line) if mdbg cannot serve a transfer. See GTAV_PROC_NOSTOP_IO.
PAYLOAD_LOADER_NOSTOP_IO ?= 0
# Never let a failed kernel transfer fall back to ptrace: fail the transfer instead. Requires
# PAYLOAD_LOADER_NOSTOP_IO=1. On a lane that exists to keep the game's streaming I/O intact, the
# fallback is worse than the failure -- it performs the one stop that jams the ring for good, and
# inject_write's retries then make it look like a success. See GTAV_PROC_NOSTOP_STRICT.
PAYLOAD_LOADER_NOSTOP_STRICT ?= 0
# Read every image/relocation write back and compare it before believing it. The kernel write path
# reaches RESIDENT pages only and the debug-memory path reports success on memory with no backing,
# so on the cave lane a write is only believed after a readback. See GTAV_ELF_INJECT_VERIFY_WRITES.
PAYLOAD_LOADER_VERIFY_WRITES ?= 0
# Address of the streaming in-flight counter in the target, polled over mdbg immediately before
# every PT_ATTACH so the loader only ever stops the game while no file read is outstanding.
# Build-specific: 0x6257d18 on ppsa04264-01.010.002. 0 = no gate (classic behaviour).
PAYLOAD_LOADER_QUIESCE_ADDR ?= 0
PAYLOAD_LOADER_QUIESCE_TIMEOUT_MS ?= 15000
# Ptrace-free bootstrap: install the bounded cave stub, obtain/prefault worker memory from the game
# thread, map the worker through verified kernel-copy writes, then restore the temporary bytes.
# CAVE_ADDR is the target-specific executable tail padding address.
PAYLOAD_LOADER_CAVE_BOOTSTRAP ?= 0
PAYLOAD_LOADER_CAVE_ADDR ?= 0
PAYLOAD_LOADER_CAVE_ALLOC ?= 0x200000
PAYLOAD_LOADER_CAVE_TIMEOUT_MS ?= 5000
# The real inject over the ptrace-free route: the bootstrap's memory replaces alloc_exec's remote
# mmap, and the worker starts its own thread from the frame hook instead of the loader injecting one,
# so no step of an inject stops the game. Requires PAYLOAD_LOADER_CAVE_BOOTSTRAP=1,
# PAYLOAD_LOADER_NOSTOP_STRICT=1, PAYLOAD_LOADER_VERIFY_WRITES=1 (all enforced with #error), and a
# worker built with FRAME_HOOK_SELF_START_WORKER=1.
PAYLOAD_LOADER_CAVE_INJECT ?= 0
# Explicit legacy allocation/thread-start lane. This is a selected target contract, never a
# fallback if cave injection fails.
PAYLOAD_LOADER_CLASSIC_INJECT ?= 0
# Real menu injection: map the feature-menu ELF into GTA and start its worker.
PAYLOAD_LOADER_INJECT ?= 0
# Frame-hook (function-prologue) drain: after inject, the loader reads the patch broker
# the injected EXTERNAL_INSTALL menu published and writes the prologue jump on the
# target native via kernel R/W -- catching runtime native calls the slot swap misses.
# 1 = install the broker (pair with a feature-menu-frame-hook-*-build ELF). 0 = off.
PAYLOAD_LOADER_INSTALL_BROKER ?= 0
# Exact 01.010.002 group-2 renderer discovery + mapped-worker preconfiguration. Production
# 01.010 lanes opt in alongside RENDER_PHASE_INTERCEPT/GTAV_MENU_PHASE_DRAW_LIST; default off keeps
# inert and legacy builds unchanged.
PAYLOAD_LOADER_INSTALL_RENDER_PHASE ?= 0
# Phase 1 wait-for-game: poll for GTA to become the foreground app instead of
# bailing on the first miss, so the loader can be deployed BEFORE GTA launches and
# continue to the separate player-world readiness gate. 1 = wait (parks on console),
# 0 = one-shot.
PAYLOAD_LOADER_WAIT_FOR_GAME ?= 0
# Seconds to wait before giving up (0 = indefinitely). Discovery is the cheap
# foreground big-app query (no ptrace), so a long wait is harmless.
PAYLOAD_LOADER_WAIT_TIMEOUT_SEC ?= 0
# Persistent dormant daemon: stay resident and re-inject every time GTA relaunches
# (re-arm keyed on game instance). Implies wait-for-game.
# 1 = daemon, 0 = one-shot inject-then-exit (default).
PAYLOAD_LOADER_PERSISTENT ?= 0
# Player-world/load readiness gate (legacy SP_READY name): once GTA is foreground,
# poll a known read-only game-memory anchor and inject only after it is stable. This
# prevents destructive boot/loading injection. Game-mode selection is outside this predicate.
# The anchor is RE'd per game build. 1 = gate on (requires the ADDR below).
# 0 = off (manual lane).
PAYLOAD_LOADER_SP_READY ?= 0
PAYLOAD_LOADER_SP_READY_ADDR ?= 0          # absolute anchor address in the game
PAYLOAD_LOADER_SP_READY_SIZE ?= 8          # bytes to read (1/2/4/8)
PAYLOAD_LOADER_SP_READY_MASK ?= 0xFFFFFFFFFFFFFFFF
PAYLOAD_LOADER_SP_READY_VALUE ?= 0         # ready when (read & MASK) cmp (VALUE & MASK)
PAYLOAD_LOADER_SP_READY_MODE ?= 0          # 0 = ready when ==, 1 = ready when != (e.g. non-null)
PAYLOAD_LOADER_SP_READY_TIMEOUT_SEC ?= 0   # 0 = wait indefinitely
PAYLOAD_LOADER_SP_READY_SETTLE_USEC ?= 0   # extra delay after first ready, before inject
PAYLOAD_LOADER_SP_READY_DEREF ?= 0         # 1 = compare *(*(ADDR)+OFFSET) (follow one pointer)
PAYLOAD_LOADER_SP_READY_DEREF_OFFSET ?= 0  # offset into the dereferenced base pointer
PAYLOAD_LOADER_SP_READY_CONFIRMATIONS ?= 3 # consecutive ready reads required before inject (debounce)
PAYLOAD_LOADER_SP_READY_GENTLE ?= 0        # 1 = gentler not-ready poll cadence (fewer PT_ATTACH freezes)
PAYLOAD_LOADER_SP_READY_POLL_USEC ?= 2000000     # gentle base poll interval (usec)
PAYLOAD_LOADER_SP_READY_POLL_MAX_USEC ?= 5000000 # gentle backoff cap (usec)
# Double-inject transaction guard: per-pid lock + claim mutex so two loaders / a re-run cannot
# stack workers or race stale-lock replacement. Production/menu-ctl requires 1. Value 0 exists only
# for isolated host/research builds and is unsafe for deployment because remote unmap is unavailable.
PAYLOAD_LOADER_INJECT_GUARD ?= 1
# Read-only nullfs mount of /data/GTAVMenu/custom into GTA's sandbox, visible to the game as
# /gtavmenu (include/gtavmenu/custom_mount.h). Hardware-unverified, so off by default; enable with
# CUSTOM_MOUNT=1 ./menu-ctl.sh cave-inject (or watch).
PAYLOAD_LOADER_CUSTOM_MOUNT ?= 0
# Target-version guard: before injecting, the loader reads a stable native handler's prologue
# (GET_FRAME_COUNT -- real game code we never patch) and refuses to inject if it does not match
# the bytes recorded for PPSA04264 01.010.002, so the pinned native/hook addresses can never be
# fired into a wrong build. 1 = on (default); 0 = skip (deliberate different-build bring-up).
PAYLOAD_LOADER_VERIFY_VERSION ?= 1
# Foreground big-app discovery (gtav_proc_find_game): app id + title id from
# libSceSystemService, app-id->pid scan via _sceApplicationGetAppId in libSceSysCore.
PAYLOAD_LOADER_LDLIBS := -lSceSystemService -lSceSysCore
PAYLOAD_LOADER_ELF := $(BUILD_PROFILE_DIR)/gtav-menu-payload-loader.elf
ONIONHEN_PLUGIN_ID := GTAV00001
ONIONHEN_OUTPUT_DIR := $(BUILD_DIR)/$(GTAV_TARGET)/production/onionhen
ONIONHEN_PLUGIN_ELF := $(ONIONHEN_OUTPUT_DIR)/$(ONIONHEN_PLUGIN_ID).elf
ONIONHEN_DAEMON_ELF := $(ONIONHEN_OUTPUT_DIR)/gtav-menu-onion-daemon.elf
ETAHEN_PLUGIN_ID := GTAV00001
ETAHEN_RUNTIME_ID := GTAV00002
ETAHEN_OUTPUT_DIR := $(BUILD_DIR)/$(GTAV_TARGET)/production/etahen
ETAHEN_RUNTIME_ELF := $(ETAHEN_OUTPUT_DIR)/gtav-menu-etahen-runtime.elf
ETAHEN_RUNTIME_PLUGIN := $(ETAHEN_OUTPUT_DIR)/$(ETAHEN_RUNTIME_ID).plugin
ETAHEN_SUPERVISOR_ELF := $(ETAHEN_OUTPUT_DIR)/gtav-menu-etahen-supervisor.elf
ETAHEN_PLUGIN := $(ETAHEN_OUTPUT_DIR)/$(ETAHEN_PLUGIN_ID).plugin
PAYLOAD_LOADER_SRCS := \
	src/payload_loader/main.c \
	src/common/daemon_control.c \
	src/payload_loader/cave_bootstrap.c \
	src/common/proc_backend.c \
	src/common/proc_backend_sdk.c \
	src/common/elf_inject.c \
	src/common/render_phase_discovery.c \
	src/common/detour.c \
	src/common/ini_parse.c \
	src/common/log.c \
	src/common/notify.c \
	src/common/rootdir.c \
	src/common/runtime_config.c \
	src/common/strutil.c \
	src/common/hex.c \
	src/common/custom_mount_pick.c \
	src/payload_loader/custom_mount.c
