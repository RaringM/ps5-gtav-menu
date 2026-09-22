PS5_PAYLOAD_SDK ?= $(HOME)/Projects/PS5/ps5-payload-sdk
PS5_LLVM_MAJOR ?= 20
# Diagnostic escape hatch only. Publication builds use the pinned major above; setting this to 1
# records the exception in build-config.json and must be followed by a fresh hardware gate.
GTAV_ALLOW_UNVALIDATED_TOOLCHAIN ?= 0

# Does the installed SDK provide kernel_proc_copyin/copyout? Those try the debug-memory path and
# then fall back to a page-table walk + kernel direct-map write. That fallback is mandatory on
# firmware >= 8.40, where the debug-memory WRITE service is refused outright (EPERM) -- see
# docs/ptrace-free-injection.md. Older SDKs only ship mdbg_copyin/copyout, so this is detected
# rather than assumed: the loader keeps building against either, and CI's SDK release decides
# which path it gets.
GTAV_SDK_HAS_PROC_COPYIO := $(shell grep -qs 'kernel_proc_copyin' \
  "$(PS5_PAYLOAD_SDK)/target/include/ps5/kernel.h" && echo 1 || echo 0)
PS5_HOST ?= 192.168.0.57
PS5_PORT ?= 9021
PS5_FTP_PORT ?= 1337
PS5DEBUG_HOST ?= $(PS5_HOST)
PS5DEBUG_PORT ?= 744
PYTHON ?= python3
MENU_COMMAND ?= toggle
GTAV_BUILD_PROFILE ?= production
GTAV_DELIVERY ?= standalone
ONIONHEN_PLUGIN_VERSION ?= 1.03
ONIONHEN_PACKAGE_DIR ?= build/pkg/onionhen

# The current production tree supports one exact target. The reproducible 01.005.000 build lives at
# the signed archive/ppsa04264-01.005.000 tag and is intentionally absent from current build logic.
GTAV_TARGET ?= ppsa04264-01.010.002
ifneq ($(GTAV_TARGET),ppsa04264-01.010.002)
$(error current builds support only ppsa04264-01.010.002; use archive/ppsa04264-01.005.000 for 01.005.000)
endif

# The target manifest is authoritative for the player-world readiness chain used by persistent
# delivery. Keep these out of generic loader defaults so a manual build remains fail-closed; the
# OnionHEN rule opts into this exact pair together with its readiness gate.
GTAV_TARGET_MANIFEST := data/targets/$(GTAV_TARGET).json
GTAV_TARGET_LOADER_READINESS := $(shell $(PYTHON) tools/target_loader_config.py \
	--target-manifest $(GTAV_TARGET_MANIFEST))
GTAV_TARGET_PLAYER_PED_ANCHOR := $(word 1,$(GTAV_TARGET_LOADER_READINESS))
GTAV_TARGET_PLAYER_PED_OFFSET := $(word 2,$(GTAV_TARGET_LOADER_READINESS))
GTAV_TARGET_CAVE_ADDR := $(word 3,$(GTAV_TARGET_LOADER_READINESS))
GTAV_TARGET_CAVE_ALLOC := $(word 4,$(GTAV_TARGET_LOADER_READINESS))
ifeq ($(words $(GTAV_TARGET_LOADER_READINESS)),4)
else
$(error failed to read loader target values from $(GTAV_TARGET_MANIFEST))
endif

# Defaults below remain conservative for direct variable inspection. make/profiles/production.mk
# turns on the complete validated feature set; make/profiles/smoke.mk selects the inert link check.
FEATURE_MENU_ENABLE_NATIVE_FEATURES ?= 1
FEATURE_MENU_ENABLE_FRAME_HOOK ?= 0
FRAME_HOOK_EXTERNAL_INSTALL ?= 0
FRAME_HOOK_CAPTURE_CONTEXT ?= 0
GTAV_MENU_ENABLE_WORKER_TEXT_OVERLAY ?= 1
GTAV_MENU_ENABLE_TOASTS ?= 1
GTAV_MENU_ENABLE_VEHICLE_PREVIEW ?= 0
GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM ?= 0
GTAV_MENU_ENABLE_BUTTON_GLYPHS ?= 0
RENDER_DIAGNOSTICS ?= 0
RENDER_DIAG_MODE ?= 0
RENDER_DIAG_ALPHA ?= 255
RENDER_DIAG_BUILD_ID ?= unrecorded
RENDER_CYCLE_PROBE ?= 0
RENDER_PATH_PROBE ?= 0
RENDER_BANK_PROBE ?= 0
RENDER_PHASE_INTERCEPT ?= 0
GTAV_MENU_PHASE_DRAW_LIST ?= 0

# The generated header and target manifest are the only publication-side target inputs.
GTAV_MENU_NATIVE_ADDRESSES_HEADER ?= "gtavmenu/native_addresses_ppsa04264-01.010.002_generated.h"
PAYLOAD_LOADER_CAVE_ADDR ?= $(GTAV_TARGET_CAVE_ADDR)
PAYLOAD_LOADER_CAVE_ALLOC ?= $(GTAV_TARGET_CAVE_ALLOC)

FEATURE_MENU_TARGET_ID ?= GTAV_FEATURE_MENU

# Worker and delivery settings with validated production meaning. The profiles own the feature
# gates; watch/OnionHEN override only context readiness and persistence.
FEATURE_MENU_INTERVAL ?= 1
FEATURE_MENU_MAX_TICKS ?= 0
FEATURE_MENU_GATE_MAINTHREAD ?= 1
SCRIPT_GLOBALS ?= 0
WORKER_REQUIRE_CONTEXT ?= 0
