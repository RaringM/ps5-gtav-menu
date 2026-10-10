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
# Standalone payloads are self-contained by default. Keep the staged-worker lane available as an
# explicit development override, but never make end users supply the worker beside the daemon.
GTAV_MENU_EMBEDDED_WORKER ?= 1
ONIONHEN_PLUGIN_VERSION ?= 1.03
ONIONHEN_PACKAGE_DIR ?= build/pkg/$(GTAV_TARGET)/onionhen
ETAHEN_PLUGIN_VERSION ?= 1.00
ETAHEN_RUNTIME_VERSION ?= $(ETAHEN_PLUGIN_VERSION)
ETAHEN_PACKAGE_DIR ?= build/pkg/$(GTAV_TARGET)/etahen
ETAHEN_RUNTIME ?= 0

# The primary target remains 01.010.002. Other exact targets are admitted only by a schema-2 JSON
# profile whose input hashes, toolchain, features, and injection lane validate before make expands.
GTAV_TARGET ?= ppsa04264-01.010.002
GTAV_TARGET_MANIFEST := data/targets/$(GTAV_TARGET).json
GTAV_TARGET_PROFILE := $(shell $(PYTHON) tools/target_build_config.py \
	--target-manifest $(GTAV_TARGET_MANIFEST) --make)
target_profile_value = $(patsubst $1=%,%,$(filter $1=%,$(GTAV_TARGET_PROFILE)))
ifneq ($(call target_profile_value,profile_ok),1)
$(error invalid or missing target profile: $(GTAV_TARGET_MANIFEST))
endif
GTAV_TARGET_ID := $(call target_profile_value,target_id)
GTAV_TARGET_TITLE_ID := $(call target_profile_value,title_id)
GTAV_TARGET_CONTENT_ID := $(call target_profile_value,content_id)
GTAV_TARGET_CONTENT_VERSION := $(call target_profile_value,content_version)
GTAV_TARGET_CHANNEL := $(call target_profile_value,channel)
GTAV_TARGET_NATIVE_JSON := $(call target_profile_value,native_json)
GTAV_TARGET_NATIVE_HEADER := $(call target_profile_value,native_header)
GTAV_TARGET_NATIVE_SHA256 := $(call target_profile_value,native_sha256)
GTAV_TARGET_SCRIPT_JSON := $(call target_profile_value,script_json)
GTAV_TARGET_SCRIPT_HEADER := $(call target_profile_value,script_header)
GTAV_TARGET_SCRIPT_SHA256 := $(call target_profile_value,script_sha256)
GTAV_TARGET_NATIVE_FEATURES := $(call target_profile_value,native_features)
GTAV_TARGET_WORKER_TEXT := $(call target_profile_value,worker_text)
GTAV_TARGET_TOASTS := $(call target_profile_value,toasts)
GTAV_TARGET_VEHICLE_PREVIEW := $(call target_profile_value,vehicle_preview)
GTAV_TARGET_INSTRUCTIONAL_SCALEFORM := $(call target_profile_value,instructional_scaleform)
GTAV_TARGET_BUTTON_GLYPHS := $(call target_profile_value,button_glyphs)
GTAV_TARGET_SCRIPT_GLOBALS := $(call target_profile_value,script_globals)
GTAV_TARGET_PHASE_INTERCEPT := $(call target_profile_value,phase_intercept)
GTAV_TARGET_PHASE_DRAW_LIST := $(call target_profile_value,phase_draw_list)
GTAV_TARGET_CUSTOM_PACKS := $(call target_profile_value,custom_packs)
GTAV_TARGET_INJECTION_LANE := $(call target_profile_value,injection_lane)
GTAV_TARGET_PROBE_NOSTOP := $(call target_profile_value,probe_nostop)
GTAV_TARGET_NOSTOP_IO := $(call target_profile_value,nostop_io)
GTAV_TARGET_NOSTOP_STRICT := $(call target_profile_value,nostop_strict)
GTAV_TARGET_VERIFY_WRITES := $(call target_profile_value,verify_writes)
GTAV_TARGET_CAVE_BOOTSTRAP := $(call target_profile_value,cave_bootstrap)
GTAV_TARGET_CAVE_INJECT := $(call target_profile_value,cave_inject)
GTAV_TARGET_CLASSIC_INJECT := $(call target_profile_value,classic_inject)
GTAV_TARGET_SELF_START := $(call target_profile_value,self_start)
GTAV_TARGET_INSTALL_PHASE := $(call target_profile_value,install_phase)

# The target manifest is authoritative for the player-world readiness chain and cave allocation.
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
GTAV_MENU_NATIVE_ADDRESSES_HEADER ?= "$(GTAV_TARGET_NATIVE_HEADER)"
GTAV_MENU_SCRIPT_GLOBALS_HEADER ?= "$(GTAV_TARGET_SCRIPT_HEADER)"
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
