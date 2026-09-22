# Build targets for the GTAV-Menu PS5 payload.
#
# The live menu is one lane: an SDK-native payload loader injects the feature-menu
# worker ELF into GTA and writes the PLAYER_PED_ID frame-hook via kernel R/W, so
# queued game-thread actions (vehicle spawn, weapons, skin) drain on the game thread.
# Launch it with ./menu-ctl.sh cave-inject. See docs/ARCHITECTURE.md.

.DEFAULT_GOAL := all

.PHONY: all menu-live onionhen _onionhen onionhen-plugin-build package-onionhen clean help FORCE require-ps5-sdk \
	payload-loader-build deploy-payload-loader deploy-built-payload-loader package-payload \
	feature-menu-frame-hook-playerped-build

# Sources linked into the injected menu worker ELF (COMMON_SRCS comes from flags.mk,
# which the root Makefile includes before this file).
MODULE_SRCS := \
	src/module/command_mailbox.c \
	src/module/feature_catalog.c \
	src/module/features.cpp \
	src/module/frame_hook.c \
	src/module/klog.c \
	src/module/log_ring.c \
	src/module/main.c \
	src/module/menu.c \
	src/module/menu_draw_list.c \
	src/module/native_bridge.cpp \
	src/module/native_job_queue.c \
	src/module/pad_input.c \
	src/module/patch_broker.c \
	src/module/quit_guard.c \
	src/module/render_cycle_probe.c \
	src/module/render_phase_intercept.c \
	src/module/render_phase_thunk.S \
	src/module/script_globals.c \
	src/module/spawn_module_loader.c \
	src/module/status.c \
	$(COMMON_SRCS)

# Non-TU build inputs: the feature fragments (textually #included into features.cpp),
# the menu-definition header, and the public headers. They are NOT compiled directly, but
# the worker ELF must rebuild when any of them changes -- otherwise a fragment-only edit
# (the common case here, since features are split across features/*.inc) silently reuses a
# stale ELF. Listed as ordinary prerequisites so a change triggers a rebuild; the recipe
# compiles $(MODULE_SRCS) explicitly (not $^) so these are never passed to the compiler.
MODULE_DEPS := \
	$(wildcard src/module/features/*.inc) \
	$(wildcard src/module/*.hpp) \
	$(wildcard include/gtavmenu/*.h)

help:
	@echo "GTAV-Menu — common targets:"
	@echo "  make all                       Build the complete 01.010.002 production loader + worker"
	@echo "  make menu-live                 Compatibility alias for make all"
	@echo "  make onionhen                  Build single-file OnionHEN auto-inject plugin"
	@echo "  make package-onionhen          Stage plugin + auto-start marker for installation"
	@echo "  make payload-loader-build      Build the SDK payload loader (the injector)"
	@echo "  make feature-menu-frame-hook-playerped-build  Build the injected menu worker ELF"
	@echo "  make deploy-payload-loader     Deploy the loader to PS5 (prospero-deploy)"
	@echo "  make package-payload           Bundle the production loader, worker, manifest, and installer"
	@echo "Launch the default target with ./menu-ctl.sh cave-inject."

# Ordinary builds produce the complete supported menu. Building never deploys it.
all: payload-loader-build feature-menu-frame-hook-playerped-build

require-ps5-sdk:
	@test "$(PS5_TOOLCHAIN_AVAILABLE)" = "1" || { \
		echo "error: PS5 payload SDK not found at $(PS5_PAYLOAD_SDK); set PS5_PAYLOAD_SDK to a v0.43-compatible SDK" >&2; \
		exit 1; \
	}
	@llvm_version="$$($(PS5_PAYLOAD_SDK)/bin/prospero-llvm-config --version 2>/dev/null)"; \
	compiler_version="$$($(CC) --version 2>/dev/null | sed -n '1p')"; \
	llvm_major="$${llvm_version%%.*}"; \
	compiler_major="$$(printf '%s\n' "$$compiler_version" | sed -E 's/^.*clang version ([0-9]+).*$$/\1/')"; \
	case "$$llvm_major" in \
		''|*[!0-9]*) echo "error: could not identify llvm-config version: $$llvm_version" >&2; exit 1 ;; \
	esac; \
	case "$$compiler_major" in \
		''|*[!0-9]*) echo "error: could not identify compiler version: $$compiler_version" >&2; exit 1 ;; \
	esac; \
	if test "$$llvm_major" != "$(PS5_LLVM_MAJOR)" || test "$$compiler_major" != "$(PS5_LLVM_MAJOR)"; then \
		if test "$(GTAV_ALLOW_UNVALIDATED_TOOLCHAIN)" = "1"; then \
			echo "warning: unvalidated PS5 toolchain: $$compiler_version (llvm-config $$llvm_version); expected LLVM $(PS5_LLVM_MAJOR)" >&2; \
		else \
			echo "error: PS5 release builds require LLVM $(PS5_LLVM_MAJOR), got $$compiler_version (llvm-config $$llvm_version)" >&2; \
			echo "error: set LLVM_CONFIG to LLVM $(PS5_LLVM_MAJOR)'s absolute llvm-config path, or use the included Containerfile" >&2; \
			exit 1; \
		fi; \
	fi

menu-live: all

BUILD_LOGIC_SHA256 := $(shell shasum -a 256 Containerfile Makefile make/config.mk make/flags.mk make/targets.mk make/profiles/$(GTAV_BUILD_PROFILE).mk tools/target_loader_config.py | shasum -a 256 | cut -d' ' -f1)
TARGET_MANIFEST_SHA256 := $(shell shasum -a 256 $(GTAV_TARGET_MANIFEST) 2>/dev/null | cut -d' ' -f1)
LOADER_TARGET_CFLAGS := $(shell $(PYTHON) tools/target_loader_config.py \
	--target-manifest $(GTAV_TARGET_MANIFEST) --cflags)

FORCE:

$(BUILD_PROFILE_DIR):
	mkdir -p $@

$(BUILD_CONFIG_STAMP): FORCE tools/write_build_stamp.py tools/target_loader_config.py \
		$(GTAV_TARGET_MANIFEST) | $(BUILD_PROFILE_DIR) require-ps5-sdk
	$(PYTHON) tools/write_build_stamp.py --output $@ --compiler "$(CC)" \
		--set target=$(GTAV_TARGET) --set profile=$(GTAV_BUILD_PROFILE) \
		--set delivery=$(GTAV_DELIVERY) --set build_logic_sha256=$(BUILD_LOGIC_SHA256) \
		--set target_manifest_sha256=$(TARGET_MANIFEST_SHA256) \
		--set sdk=$(PS5_PAYLOAD_SDK) --set sdk_proc_copyio=$(GTAV_SDK_HAS_PROC_COPYIO) \
		--set release_llvm_major=$(PS5_LLVM_MAJOR) \
		--set unvalidated_toolchain=$(GTAV_ALLOW_UNVALIDATED_TOOLCHAIN) \
		--set worker_rootdir=0 \
		--set native_features=$(FEATURE_MENU_ENABLE_NATIVE_FEATURES) \
		--set frame_hook=$(FEATURE_MENU_ENABLE_FRAME_HOOK) \
		--set external_frame_hook=$(FRAME_HOOK_EXTERNAL_INSTALL) \
		--set capture_context=$(FRAME_HOOK_CAPTURE_CONTEXT) \
		--set require_context=$(FRAME_HOOK_REQUIRE_CONTEXT) \
		--set pad_input=$(ENABLE_PAD_INPUT) --set pad_hook=$(PAD_HOOK) \
		--set vehicle_preview=$(GTAV_MENU_ENABLE_VEHICLE_PREVIEW) \
		--set instructional_scaleform=$(GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM) \
		--set button_glyphs=$(GTAV_MENU_ENABLE_BUTTON_GLYPHS) \
		--set worker_text=$(GTAV_MENU_ENABLE_WORKER_TEXT_OVERLAY) \
		--set toasts=$(GTAV_MENU_ENABLE_TOASTS) \
		--set gate_mainthread=$(FEATURE_MENU_GATE_MAINTHREAD) \
		--set worker_interval=$(FEATURE_MENU_INTERVAL) --set max_ticks=$(FEATURE_MENU_MAX_TICKS) \
		--set worker_klog=$(GTAV_MENU_ENABLE_WORKER_KLOG) \
		--set quit_guard=$(GTAV_MENU_ENABLE_QUIT_GUARD) \
		--set quit_appstate=$(GTAV_MENU_QUIT_GUARD_APPSTATE) \
		--set script_globals=$(SCRIPT_GLOBALS) --set worker_context=$(WORKER_REQUIRE_CONTEXT) \
		--set self_start=$(FRAME_HOOK_SELF_START_WORKER) \
		--set phase_intercept=$(RENDER_PHASE_INTERCEPT) \
		--set phase_draw_list=$(GTAV_MENU_PHASE_DRAW_LIST) \
		--set loader_inject=$(PAYLOAD_LOADER_INJECT) \
		--set loader_wait=$(PAYLOAD_LOADER_WAIT_FOR_GAME) \
		--set loader_wait_timeout=$(PAYLOAD_LOADER_WAIT_TIMEOUT_SEC) \
		--set loader_persistent=$(PAYLOAD_LOADER_PERSISTENT) \
		--set loader_sp_ready=$(PAYLOAD_LOADER_SP_READY) \
		--set loader_sp_addr=$(PAYLOAD_LOADER_SP_READY_ADDR) \
		--set loader_sp_size=$(PAYLOAD_LOADER_SP_READY_SIZE) \
		--set loader_sp_mask=$(PAYLOAD_LOADER_SP_READY_MASK) \
		--set loader_sp_value=$(PAYLOAD_LOADER_SP_READY_VALUE) \
		--set loader_sp_mode=$(PAYLOAD_LOADER_SP_READY_MODE) \
		--set loader_sp_timeout=$(PAYLOAD_LOADER_SP_READY_TIMEOUT_SEC) \
		--set loader_sp_settle=$(PAYLOAD_LOADER_SP_READY_SETTLE_USEC) \
		--set loader_sp_deref=$(PAYLOAD_LOADER_SP_READY_DEREF) \
		--set loader_sp_deref_offset=$(PAYLOAD_LOADER_SP_READY_DEREF_OFFSET) \
		--set loader_sp_confirmations=$(PAYLOAD_LOADER_SP_READY_CONFIRMATIONS) \
		--set loader_sp_gentle=$(PAYLOAD_LOADER_SP_READY_GENTLE) \
		--set loader_sp_poll=$(PAYLOAD_LOADER_SP_READY_POLL_USEC) \
		--set loader_sp_poll_max=$(PAYLOAD_LOADER_SP_READY_POLL_MAX_USEC) \
		--set loader_probe_nostop=$(PAYLOAD_LOADER_PROBE_NOSTOP) \
		--set loader_nostop_io=$(PAYLOAD_LOADER_NOSTOP_IO) \
		--set loader_nostop_strict=$(PAYLOAD_LOADER_NOSTOP_STRICT) \
		--set loader_verify_writes=$(PAYLOAD_LOADER_VERIFY_WRITES) \
		--set loader_cave_bootstrap=$(PAYLOAD_LOADER_CAVE_BOOTSTRAP) \
		--set loader_cave_addr=$(PAYLOAD_LOADER_CAVE_ADDR) \
		--set loader_cave_alloc=$(PAYLOAD_LOADER_CAVE_ALLOC) \
		--set loader_cave_timeout=$(PAYLOAD_LOADER_CAVE_TIMEOUT_MS) \
		--set cave_inject=$(PAYLOAD_LOADER_CAVE_INJECT) \
		--set loader_broker=$(PAYLOAD_LOADER_INSTALL_BROKER) \
		--set loader_phase=$(PAYLOAD_LOADER_INSTALL_RENDER_PHASE) \
		--set loader_guard=$(PAYLOAD_LOADER_INJECT_GUARD) \
		--set loader_verify_version=$(PAYLOAD_LOADER_VERIFY_VERSION)

# OnionHEN-managed persistent watcher for the hardware-validated 01.010.002 cave lane. The menu
# worker is embedded in a loader-daemon ELF, and that ELF is embedded in the installable plugin.
# The supervisor asks OnionHEN's private :9020 loader to start the daemon as a fresh payload process
# with its own payload_args/kernel binding. It must never fork the managed plugin process: the SDK
# runtime and its kernel primitive are not fork-safe on target.
onionhen:
	@if [ "$(GTAV_TARGET)" != "ppsa04264-01.010.002" ]; then \
		echo "error: the OnionHEN plugin is currently gated to ppsa04264-01.010.002" >&2; \
		exit 1; \
	fi
	$(MAKE) GTAV_BUILD_PROFILE=production GTAV_DELIVERY=onionhen \
		WORKER_REQUIRE_CONTEXT=1 PAYLOAD_LOADER_WAIT_FOR_GAME=1 \
		PAYLOAD_LOADER_PERSISTENT=1 PAYLOAD_LOADER_SP_READY=1 \
		PAYLOAD_LOADER_SP_READY_DEREF=1 PAYLOAD_LOADER_SP_READY_MODE=1 \
		PAYLOAD_LOADER_SP_READY_ADDR=$(GTAV_TARGET_PLAYER_PED_ANCHOR) \
		PAYLOAD_LOADER_SP_READY_DEREF_OFFSET=$(GTAV_TARGET_PLAYER_PED_OFFSET) \
		PAYLOAD_LOADER_SP_READY_CONFIRMATIONS=3 _onionhen

_onionhen: $(ONIONHEN_PLUGIN_ELF)

onionhen-plugin-build: onionhen

$(ONIONHEN_DAEMON_ELF): $(PAYLOAD_LOADER_SRCS) src/payload_loader/embedded_worker.S \
		include/gtavmenu/loader_pins_generated.h include/gtavmenu/daemon_control.h \
		include/gtavmenu/daemon_lifecycle.h include/gtavmenu/render_phase_discovery.h \
		$(FEATURE_MENU_FRAME_HOOK_PLAYERPED_ELF) $(BUILD_CONFIG_STAMP) | $(ONIONHEN_OUTPUT_DIR) require-ps5-sdk
	@if [ "$(GTAV_TARGET)" != "ppsa04264-01.010.002" ]; then \
		echo "error: refusing to build the 01.010.002-pinned OnionHEN plugin for $(GTAV_TARGET)" >&2; \
		exit 1; \
	fi
	$(CC) $(CFLAGS) \
		-DGTAV_LOADER_PROBE_NOSTOP=1 \
		-DGTAV_PROC_NOSTOP_IO=1 \
		-DGTAV_PROC_NOSTOP_STRICT=1 \
		-DGTAV_ELF_INJECT_VERIFY_WRITES=1 \
		-DGTAV_SDK_HAS_PROC_COPYIO=$(GTAV_SDK_HAS_PROC_COPYIO) \
		-DGTAV_LOADER_CAVE_BOOTSTRAP=1 \
		-DGTAV_LOADER_CAVE_ADDR=$(PAYLOAD_LOADER_CAVE_ADDR) \
		-DGTAV_LOADER_CAVE_ALLOC=$(PAYLOAD_LOADER_CAVE_ALLOC) \
		-DGTAV_LOADER_CAVE_TIMEOUT_MS=$(PAYLOAD_LOADER_CAVE_TIMEOUT_MS) \
		-DGTAV_LOADER_CAVE_INJECT=1 \
		-DGTAV_MENU_PAYLOAD_INJECT=1 \
		-DGTAV_MENU_INSTALL_PATCH_BROKER=1 \
		-DGTAV_LOADER_INSTALL_RENDER_PHASE=1 \
		-DGTAV_PAYLOAD_WAIT_FOR_GAME=$(PAYLOAD_LOADER_WAIT_FOR_GAME) \
		-DGTAV_PAYLOAD_WAIT_TIMEOUT_SEC=$(PAYLOAD_LOADER_WAIT_TIMEOUT_SEC) \
		-DGTAV_PAYLOAD_PERSISTENT=$(PAYLOAD_LOADER_PERSISTENT) \
		-DGTAV_PAYLOAD_SP_READY=$(PAYLOAD_LOADER_SP_READY) \
		-DGTAV_PAYLOAD_SP_READY_ADDR=$(PAYLOAD_LOADER_SP_READY_ADDR) \
		-DGTAV_PAYLOAD_SP_READY_SIZE=$(PAYLOAD_LOADER_SP_READY_SIZE) \
		-DGTAV_PAYLOAD_SP_READY_MASK=$(PAYLOAD_LOADER_SP_READY_MASK) \
		-DGTAV_PAYLOAD_SP_READY_VALUE=$(PAYLOAD_LOADER_SP_READY_VALUE) \
		-DGTAV_PAYLOAD_SP_READY_MODE=$(PAYLOAD_LOADER_SP_READY_MODE) \
		-DGTAV_PAYLOAD_SP_READY_TIMEOUT_SEC=$(PAYLOAD_LOADER_SP_READY_TIMEOUT_SEC) \
		-DGTAV_PAYLOAD_SP_READY_SETTLE_USEC=$(PAYLOAD_LOADER_SP_READY_SETTLE_USEC) \
		-DGTAV_PAYLOAD_SP_READY_DEREF=$(PAYLOAD_LOADER_SP_READY_DEREF) \
		-DGTAV_PAYLOAD_SP_READY_DEREF_OFFSET=$(PAYLOAD_LOADER_SP_READY_DEREF_OFFSET) \
		-DGTAV_PAYLOAD_SP_READY_CONFIRMATIONS=$(PAYLOAD_LOADER_SP_READY_CONFIRMATIONS) \
		-DGTAV_PAYLOAD_SP_READY_GENTLE=$(PAYLOAD_LOADER_SP_READY_GENTLE) \
		-DGTAV_PAYLOAD_SP_READY_POLL_USEC=$(PAYLOAD_LOADER_SP_READY_POLL_USEC) \
		-DGTAV_PAYLOAD_SP_READY_POLL_MAX_USEC=$(PAYLOAD_LOADER_SP_READY_POLL_MAX_USEC) \
		-DGTAV_PAYLOAD_INJECT_GUARD=$(PAYLOAD_LOADER_INJECT_GUARD) \
		-DGTAV_LOADER_VERIFY_VERSION=$(PAYLOAD_LOADER_VERIFY_VERSION) \
		-DGTAV_MENU_EMBEDDED_WORKER=1 \
		'-DGTAV_LOADER_EXPECT_TARGET_ID="PPSA04264_01.010.002_DISC"' \
		'-DGTAV_EMBEDDED_WORKER_PATH="$(abspath $(FEATURE_MENU_FRAME_HOOK_PLAYERPED_ELF))"' \
		$(LDFLAGS) -o $@ $(PAYLOAD_LOADER_SRCS) \
			src/payload_loader/embedded_worker.S $(PAYLOAD_LOADER_LDLIBS)

$(ONIONHEN_PLUGIN_ELF): src/payload_loader/onion_plugin.c \
		src/payload_loader/embedded_loader.S include/gtavmenu/onion_plugin.h \
		$(ONIONHEN_DAEMON_ELF) $(BUILD_CONFIG_STAMP) | $(ONIONHEN_OUTPUT_DIR) require-ps5-sdk
	$(CC) $(CFLAGS) \
		'-DGTAV_ONION_PLUGIN_VERSION="$(ONIONHEN_PLUGIN_VERSION)"' \
		'-DGTAV_EMBEDDED_LOADER_PATH="$(abspath $(ONIONHEN_DAEMON_ELF))"' \
		$(LDFLAGS) -o $@ src/payload_loader/onion_plugin.c \
			src/payload_loader/embedded_loader.S
	$(PYTHON) tools/inspect_onion_plugin.py $@ --expect-id $(ONIONHEN_PLUGIN_ID) \
		--expect-version $(ONIONHEN_PLUGIN_VERSION)

package-onionhen: onionhen
	$(PYTHON) tools/package_onionhen.py --plugin $(ONIONHEN_PLUGIN_ELF) \
		--output $(ONIONHEN_PACKAGE_DIR) --plugin-id $(ONIONHEN_PLUGIN_ID) \
		--version $(ONIONHEN_PLUGIN_VERSION) --target $(GTAV_TARGET) \
		--target-manifest $(GTAV_TARGET_MANIFEST) \
		--build-config $(ONIONHEN_OUTPUT_DIR)/build-config.json
	@echo "OnionHEN package staged in $(ONIONHEN_PACKAGE_DIR)"

# Standalone, etaHEN-free payload loader (SDK-native process-control backend). Rebuilds
# whenever the generated build-pin table changes: the table is the loader's auto-detect
# source (one row per supported build), so a new/updated pin must re-link the loader.
payload-loader-build: $(PAYLOAD_LOADER_ELF)

# Non-TU loader inputs: the pins header is a real prerequisite (a rebuilt table must
# re-link the loader) but is textually #included via build_pin.h, never compiled, so
# the recipe compiles $(PAYLOAD_LOADER_SRCS) explicitly (not $^).
$(PAYLOAD_LOADER_ELF): $(PAYLOAD_LOADER_SRCS) include/gtavmenu/loader_pins_generated.h \
		include/gtavmenu/daemon_control.h include/gtavmenu/daemon_lifecycle.h \
		include/gtavmenu/render_phase_discovery.h $(BUILD_CONFIG_STAMP) | $(BUILD_PROFILE_DIR) require-ps5-sdk
	$(CC) $(CFLAGS) \
		-DGTAV_LOADER_PROBE_NOSTOP=$(PAYLOAD_LOADER_PROBE_NOSTOP) \
		-DGTAV_PROC_NOSTOP_IO=$(PAYLOAD_LOADER_NOSTOP_IO) \
		-DGTAV_PROC_NOSTOP_STRICT=$(PAYLOAD_LOADER_NOSTOP_STRICT) \
		-DGTAV_ELF_INJECT_VERIFY_WRITES=$(PAYLOAD_LOADER_VERIFY_WRITES) \
		-DGTAV_SDK_HAS_PROC_COPYIO=$(GTAV_SDK_HAS_PROC_COPYIO) \
		-DGTAV_LOADER_QUIESCE_ADDR=$(PAYLOAD_LOADER_QUIESCE_ADDR) \
		-DGTAV_LOADER_QUIESCE_TIMEOUT_MS=$(PAYLOAD_LOADER_QUIESCE_TIMEOUT_MS) \
		-DGTAV_LOADER_CAVE_BOOTSTRAP=$(PAYLOAD_LOADER_CAVE_BOOTSTRAP) \
		-DGTAV_LOADER_CAVE_ADDR=$(PAYLOAD_LOADER_CAVE_ADDR) \
		-DGTAV_LOADER_CAVE_ALLOC=$(PAYLOAD_LOADER_CAVE_ALLOC) \
		-DGTAV_LOADER_CAVE_TIMEOUT_MS=$(PAYLOAD_LOADER_CAVE_TIMEOUT_MS) \
		-DGTAV_LOADER_CAVE_INJECT=$(PAYLOAD_LOADER_CAVE_INJECT) \
		-DGTAV_MENU_PAYLOAD_INJECT=$(PAYLOAD_LOADER_INJECT) \
		-DGTAV_MENU_INSTALL_PATCH_BROKER=$(PAYLOAD_LOADER_INSTALL_BROKER) \
		-DGTAV_LOADER_INSTALL_RENDER_PHASE=$(PAYLOAD_LOADER_INSTALL_RENDER_PHASE) \
		-DGTAV_PAYLOAD_WAIT_FOR_GAME=$(PAYLOAD_LOADER_WAIT_FOR_GAME) \
		-DGTAV_PAYLOAD_WAIT_TIMEOUT_SEC=$(PAYLOAD_LOADER_WAIT_TIMEOUT_SEC) \
		-DGTAV_PAYLOAD_PERSISTENT=$(PAYLOAD_LOADER_PERSISTENT) \
		-DGTAV_PAYLOAD_SP_READY=$(PAYLOAD_LOADER_SP_READY) \
		-DGTAV_PAYLOAD_SP_READY_ADDR=$(PAYLOAD_LOADER_SP_READY_ADDR) \
		-DGTAV_PAYLOAD_SP_READY_SIZE=$(PAYLOAD_LOADER_SP_READY_SIZE) \
		-DGTAV_PAYLOAD_SP_READY_MASK=$(PAYLOAD_LOADER_SP_READY_MASK) \
		-DGTAV_PAYLOAD_SP_READY_VALUE=$(PAYLOAD_LOADER_SP_READY_VALUE) \
		-DGTAV_PAYLOAD_SP_READY_MODE=$(PAYLOAD_LOADER_SP_READY_MODE) \
		-DGTAV_PAYLOAD_SP_READY_TIMEOUT_SEC=$(PAYLOAD_LOADER_SP_READY_TIMEOUT_SEC) \
		-DGTAV_PAYLOAD_SP_READY_SETTLE_USEC=$(PAYLOAD_LOADER_SP_READY_SETTLE_USEC) \
		-DGTAV_PAYLOAD_SP_READY_DEREF=$(PAYLOAD_LOADER_SP_READY_DEREF) \
		-DGTAV_PAYLOAD_SP_READY_DEREF_OFFSET=$(PAYLOAD_LOADER_SP_READY_DEREF_OFFSET) \
		-DGTAV_PAYLOAD_SP_READY_CONFIRMATIONS=$(PAYLOAD_LOADER_SP_READY_CONFIRMATIONS) \
		-DGTAV_PAYLOAD_SP_READY_GENTLE=$(PAYLOAD_LOADER_SP_READY_GENTLE) \
		-DGTAV_PAYLOAD_SP_READY_POLL_USEC=$(PAYLOAD_LOADER_SP_READY_POLL_USEC) \
		-DGTAV_PAYLOAD_SP_READY_POLL_MAX_USEC=$(PAYLOAD_LOADER_SP_READY_POLL_MAX_USEC) \
		-DGTAV_PAYLOAD_INJECT_GUARD=$(PAYLOAD_LOADER_INJECT_GUARD) \
		-DGTAV_LOADER_VERIFY_VERSION=$(PAYLOAD_LOADER_VERIFY_VERSION) \
		$(LOADER_TARGET_CFLAGS) \
		$(LDFLAGS) -o $@ $(PAYLOAD_LOADER_SRCS) $(PAYLOAD_LOADER_LDLIBS)

deploy-payload-loader: $(PAYLOAD_LOADER_ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $<

# menu-ctl builds a delivery-specific loader first (watch/persistent/readiness settings included).
# Re-entering the ordinary deploy target without those command-line settings would refresh the
# configuration stamp and rebuild the ELF with standalone defaults immediately before upload.
# This internal target deliberately deploys the already-built artifact without evaluating build
# prerequisites; menu-ctl verifies the file exists after its exact build before invoking it.
deploy-built-payload-loader: | require-ps5-sdk
	@test -f "$(PAYLOAD_LOADER_ELF)" || { echo "missing built loader: $(PAYLOAD_LOADER_ELF)" >&2; exit 1; }
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(PAYLOAD_LOADER_ELF)

package-payload: all
	$(PYTHON) tools/package_payload.py --loader $(PAYLOAD_LOADER_ELF) \
		--worker $(FEATURE_MENU_FRAME_HOOK_PLAYERPED_ELF) \
		--target-manifest data/targets/$(GTAV_TARGET).json \
		--build-config $(BUILD_CONFIG_STAMP) --output $(PAYLOAD_PACKAGE_DIR)

# The injected menu worker: external-install CHAIN frame hook on the target build's
# PLAYER_PED_ID native, which the game's scripts call every frame in valid script
# context, so queued game-thread actions (vehicle spawn, weapons, skin) drain with
# no input. The loader writes the prologue jump via kernel R/W and installs the
# in-ELF patch broker. The per-target hook bytes come from data/targets/*.json via
# tools/feature_menu_target_cflags.py and are guarded by the target-manifest tests.
$(FEATURE_MENU_FRAME_HOOK_PLAYERPED_ELF): $(MODULE_SRCS) $(MODULE_DEPS) $(BUILD_CONFIG_STAMP) | $(BUILD_PROFILE_DIR) require-ps5-sdk
	@# A script-global target mismatch is a blind write, so exact identity is a build gate.
	@if [ "$(SCRIPT_GLOBALS)" = "1" ]; then \
		anchor=$$(sed -n 's/^#define GTAV_SCRIPT_GLOBALS_ANCHOR_TARGET_ID "\(.*\)"/\1/p' \
			include/gtavmenu/script_globals_addresses_generated.h 2>/dev/null); \
		if [ "$$anchor" != "$(GTAV_TARGET)" ]; then \
			echo "error: script-global data targets '$$anchor', expected $(GTAV_TARGET); refusing BLIND WRITE" >&2; \
			exit 1; \
		fi; \
	fi
	$(CC) $(PAD_INPUT_CFLAGS) $(QUIT_GUARD_CFLAGS) $(FEATURE_MENU_CFLAGS) $(FEATURE_MENU_TARGET_CFLAGS) \
		-DGTAV_MENU_ENABLE_FRAME_HOOK=$(FEATURE_MENU_ENABLE_FRAME_HOOK) \
		-DGTAV_FRAME_HOOK_EXTERNAL_INSTALL=$(FRAME_HOOK_EXTERNAL_INSTALL) \
		-DGTAV_FRAME_HOOK_CAPTURE_CONTEXT=$(FRAME_HOOK_CAPTURE_CONTEXT) \
		-DGTAV_FRAME_HOOK_REQUIRE_CONTEXT_FOR_JOBS=$(FRAME_HOOK_REQUIRE_CONTEXT) \
		-DGTAV_FRAME_HOOK_SELF_START_WORKER=$(FRAME_HOOK_SELF_START_WORKER) \
		-DGTAV_MENU_ENABLE_SCRIPT_GLOBALS=$(SCRIPT_GLOBALS) \
		-DGTAV_MENU_ENABLE_PROLOGUE_SKIP=0 \
		-DGTAV_MENU_WORKER_REQUIRE_CONTEXT=$(WORKER_REQUIRE_CONTEXT) \
		-DGTAV_FRAME_HOOK_PATCH_LEN=14u \
		-DGTAV_FRAME_HOOK_STOLEN_LEN=16u \
		$(LDFLAGS) -o $@ $(MODULE_SRCS) $(PAD_INPUT_LDLIBS) $(QUIT_GUARD_LDLIBS)
	$(PYTHON) tools/patch_inject_skip_sdk_patch.py $@

feature-menu-frame-hook-playerped-build: $(FEATURE_MENU_FRAME_HOOK_PLAYERPED_ELF)

clean:
	rm -rf $(BUILD_DIR)
