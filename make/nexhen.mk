# Native GTA V module for the corrected nexHEN lifecycle bridge.
#
# This delivery keeps the menu implementation in this repository. The broker
# must arm gtav_nexhen_contract and install/verify the external game hook and
# render-phase callback before plugin_load is accepted.

# NEXHEN_MODE selects the module flavour:
#   broker  the corrected-loader lane (activation contract, externally installed hooks)
#   probe   lifecycle-only stock-loader feasibility probe; installs no hooks
NEXHEN_MODE ?= broker
ifeq ($(filter $(NEXHEN_MODE),broker probe),)
$(error NEXHEN_MODE must be broker or probe, not '$(NEXHEN_MODE)')
endif

NEXHEN_MODULE_NAME := gtav_menu_nexhen
NEXHEN_OUTPUT_DIR := $(BUILD_DIR)/$(GTAV_TARGET)/production/nexhen$(if $(filter-out broker,$(NEXHEN_MODE)),-$(NEXHEN_MODE))
NEXHEN_OBJECT_DIR := $(NEXHEN_OUTPUT_DIR)/objects
NEXHEN_GENERATED_DIR := $(NEXHEN_OUTPUT_DIR)/generated
NEXHEN_RAW_PRX := $(NEXHEN_OUTPUT_DIR)/$(NEXHEN_MODULE_NAME).prx
NEXHEN_SPRX := $(NEXHEN_OUTPUT_DIR)/$(NEXHEN_MODULE_NAME).sprx
NEXHEN_LD := $(shell $(PS5_PAYLOAD_SDK)/bin/prospero-llvm-config --bindir)/ld.lld
NEXHEN_NM := $(PS5_PAYLOAD_SDK)/bin/prospero-nm
NEXHEN_OBJCOPY := $(PS5_PAYLOAD_SDK)/bin/prospero-objcopy
NEXHEN_FSELF := $(PS5_PAYLOAD_SDK)/samples/install_app/make_fself.py

ifeq ($(NEXHEN_MODE),probe)
NEXHEN_SRCS := src/module/nexhen_probe.c \
	src/common/notify.c \
	src/module/nexhen_startup.c \
	src/module/nexhen_entry.S
NEXHEN_EXPORT_FLAGS := --without-contract
else
NEXHEN_SRCS := $(MODULE_SRCS) \
	src/module/nexhen_plugin.c \
	src/module/nexhen_startup.c \
	src/module/nexhen_entry.S
NEXHEN_EXPORT_FLAGS :=
endif
NEXHEN_C_SRCS := $(filter %.c,$(NEXHEN_SRCS))
NEXHEN_CPP_SRCS := $(filter %.cpp,$(NEXHEN_SRCS))
NEXHEN_ASM_SRCS := $(filter %.S,$(NEXHEN_SRCS))
NEXHEN_C_OBJS := $(addprefix $(NEXHEN_OBJECT_DIR)/,$(NEXHEN_C_SRCS:.c=.o))
NEXHEN_CPP_OBJS := $(addprefix $(NEXHEN_OBJECT_DIR)/,$(NEXHEN_CPP_SRCS:.cpp=.o))
NEXHEN_ASM_OBJS := $(addprefix $(NEXHEN_OBJECT_DIR)/,$(NEXHEN_ASM_SRCS:.S=.o))
NEXHEN_OBJS := $(NEXHEN_C_OBJS) $(NEXHEN_CPP_OBJS) $(NEXHEN_ASM_OBJS)

NEXHEN_COMMON_CFLAGS := $(PAD_INPUT_CFLAGS) $(QUIT_GUARD_CFLAGS) $(FEATURE_MENU_CFLAGS) \
	$(FEATURE_MENU_TARGET_CFLAGS) $(LOADER_TARGET_CFLAGS) \
	-fPIC -fplt -fvisibility=hidden -fno-stack-protector -mno-red-zone \
	-DGTAV_MENU_ENABLE_FRAME_HOOK=1 \
	-DGTAV_FRAME_HOOK_EXTERNAL_INSTALL=1 \
	-DGTAV_FRAME_HOOK_CAPTURE_CONTEXT=1 \
	-DGTAV_FRAME_HOOK_REQUIRE_CONTEXT_FOR_JOBS=1 \
	-DGTAV_FRAME_HOOK_SELF_START_WORKER=0 \
	-DGTAV_NEXHEN_MANAGED_LIFECYCLE=1 \
	-DGTAV_MENU_ENABLE_SCRIPT_GLOBALS=1 \
	-DGTAV_MENU_ENABLE_PROLOGUE_SKIP=0 \
	-DGTAV_DETOUR_SCE_MPROTECT=1 \
	-DGTAV_MENU_WORKER_REQUIRE_CONTEXT=0 \
	-DGTAV_FRAME_HOOK_PATCH_LEN=14u \
	-DGTAV_FRAME_HOOK_STOLEN_LEN=16u \
	-DGTAV_MENU_NO_STANDALONE_MAIN=1 \
	-DGTAV_MENU_SHUTDOWN_INPUT_RELEASE_USEC=250000u \
	-DGTAV_NEXHEN_TARGET_ID=\"$(GTAV_TARGET_ID)\" \
	-DGTAV_NEXHEN_TITLE_ID=\"$(GTAV_TARGET_TITLE_ID)\" \
	-DGTAV_NEXHEN_CONTENT_VERSION=\"$(GTAV_TARGET_CONTENT_VERSION)\"

NEXHEN_ROOTS := plugin_load plugin_unload g_pluginName g_pluginDesc g_pluginAuth \
	g_pluginVersion _init _fini gtav_nexhen_module_parameters \
	$(if $(NEXHEN_EXPORT_FLAGS),,gtav_nexhen_contract)
NEXHEN_LD_ROOTS := $(addprefix --undefined=,$(NEXHEN_ROOTS))
# Must equal the providers generate_exports.py activates for the mode's imports.
NEXHEN_PROVIDER_KEYS := $(if $(filter probe,$(NEXHEN_MODE)),libc kernel,libc kernel pad user system)
NEXHEN_PROVIDER_LIBS := $(addsuffix .so,$(addprefix $(NEXHEN_GENERATED_DIR)/,$(NEXHEN_PROVIDER_KEYS)))

.PHONY: nexhen _nexhen nexhen-plugin-build package-nexhen require-nexhen-target \
	nexhen-probe package-nexhen-probe

require-nexhen-target:
	@test "$(GTAV_TARGET)" = "ppsa04264-01.010.002" || { \
		echo "error: the corrected nexHEN broker is pinned to ppsa04264-01.010.002" >&2; \
		exit 1; \
	}

nexhen:
	$(MAKE) GTAV_BUILD_PROFILE=production GTAV_DELIVERY=nexhen CUSTOM_PACKS=0 \
		NEXHEN_MODE=$(NEXHEN_MODE) _nexhen

nexhen-probe:
	$(MAKE) NEXHEN_MODE=probe nexhen

_nexhen: require-nexhen-target $(NEXHEN_SPRX)

nexhen-plugin-build: nexhen

NEXHEN_STAGE_DIR := $(NEXHEN_PACKAGE_DIR)$(if $(filter-out broker,$(NEXHEN_MODE)),-$(NEXHEN_MODE))

package-nexhen: nexhen
	$(PYTHON) tools/package_nexhen.py --mode $(NEXHEN_MODE) --prx $(NEXHEN_RAW_PRX) \
		--plugin $(NEXHEN_SPRX) --providers $(NEXHEN_GENERATED_DIR)/providers.json \
		--target-manifest $(GTAV_TARGET_MANIFEST) --output $(NEXHEN_STAGE_DIR)
	@echo "nexHEN package staged in $(NEXHEN_STAGE_DIR)"

package-nexhen-probe:
	$(MAKE) NEXHEN_MODE=probe package-nexhen

$(NEXHEN_OBJECT_DIR)/%.o: %.c $(MODULE_DEPS) make/nexhen.mk | require-ps5-sdk
	@mkdir -p $(@D)
	$(CC) $(NEXHEN_COMMON_CFLAGS) -std=c11 -c $< -o $@

$(NEXHEN_OBJECT_DIR)/%.o: %.cpp $(MODULE_DEPS) make/nexhen.mk | require-ps5-sdk
	@mkdir -p $(@D)
	$(CXX) $(NEXHEN_COMMON_CFLAGS) -std=c++17 -fno-exceptions -fno-rtti -c $< -o $@

$(NEXHEN_OBJECT_DIR)/%.o: %.S make/nexhen.mk | require-ps5-sdk
	@mkdir -p $(@D)
	$(CC) $(NEXHEN_COMMON_CFLAGS) -c $< -o $@

$(NEXHEN_OUTPUT_DIR)/combined.o: $(NEXHEN_OBJS)
	$(NEXHEN_LD) -r $(NEXHEN_LD_ROOTS) -o $@ $^

$(NEXHEN_GENERATED_DIR)/stamp: $(NEXHEN_OUTPUT_DIR)/combined.o \
	tools/nexhen/generate_exports.py data/nexhen/nid-salt.txt
	@mkdir -p $(@D)
	$(NEXHEN_NM) --undefined-only $< > $(NEXHEN_OUTPUT_DIR)/undefined.txt
	$(PYTHON) tools/nexhen/generate_exports.py $(NEXHEN_OUTPUT_DIR)/undefined.txt $(@D) \
		--module $(NEXHEN_MODULE_NAME) --sdk $(PS5_PAYLOAD_SDK) --nm $(NEXHEN_NM) \
		$(NEXHEN_EXPORT_FLAGS)
	@touch $@

$(NEXHEN_OUTPUT_DIR)/nids.o: $(NEXHEN_OUTPUT_DIR)/combined.o $(NEXHEN_GENERATED_DIR)/stamp
	$(NEXHEN_OBJCOPY) @$(NEXHEN_GENERATED_DIR)/rename.rsp $< $@

$(NEXHEN_GENERATED_DIR)/%.so: $(NEXHEN_GENERATED_DIR)/stamp
	$(CC) -shared -nostdlib -nodefaultlibs -Wl,--hash-style=sysv \
		-Wl,-soname,$(if $(filter libc,$*),libSceLibcInternal,$(if $(filter kernel,$*),libkernel,$(if $(filter pad,$*),libScePad,$(if $(filter user,$*),libSceUserService,libSceSystemService)))).prx \
		-o $@ $(NEXHEN_GENERATED_DIR)/$*.S

$(NEXHEN_GENERATED_DIR)/metadata.o: $(NEXHEN_GENERATED_DIR)/stamp
	$(CC) -c $(NEXHEN_GENERATED_DIR)/metadata.S -o $@

$(NEXHEN_GENERATED_DIR)/prx.script: platform/nexhen/prx.script \
	tools/nexhen/prepare_prx_link_script.py $(NEXHEN_GENERATED_DIR)/stamp
	$(PYTHON) tools/nexhen/prepare_prx_link_script.py $< $@ \
		--providers $(NEXHEN_GENERATED_DIR)/providers.json

$(NEXHEN_OUTPUT_DIR)/linked.prx: $(NEXHEN_OUTPUT_DIR)/nids.o $(NEXHEN_PROVIDER_LIBS) \
	$(NEXHEN_GENERATED_DIR)/metadata.o $(NEXHEN_GENERATED_DIR)/prx.script \
	tools/nexhen/stamp_prx.py
	$(NEXHEN_LD) -m elf_x86_64 --shared --gc-sections --eh-frame-hdr --hash-style=sysv --build-id=sha1 \
		-z relro -z max-page-size=0x4000 -z common-page-size=0x4000 \
		-T $(NEXHEN_GENERATED_DIR)/prx.script \
		--version-script=$(NEXHEN_GENERATED_DIR)/exports.map -init _init -fini _fini \
		-o $@ $(NEXHEN_OUTPUT_DIR)/nids.o $(NEXHEN_GENERATED_DIR)/metadata.o \
		--as-needed $(NEXHEN_PROVIDER_LIBS) @$(NEXHEN_GENERATED_DIR)/exports.rsp
	$(PYTHON) tools/nexhen/stamp_prx.py $@ --providers $(NEXHEN_GENERATED_DIR)/providers.json

$(NEXHEN_RAW_PRX): $(NEXHEN_OUTPUT_DIR)/linked.prx tools/nexhen/prx_hash_fix.py
	$(PYTHON) tools/nexhen/prx_hash_fix.py $< $@ --module $(NEXHEN_MODULE_NAME)

$(NEXHEN_SPRX): $(NEXHEN_RAW_PRX)
	@test -f "$(NEXHEN_FSELF)" || { echo "missing SDK make_fself.py: $(NEXHEN_FSELF)" >&2; exit 1; }
	$(PYTHON) $(NEXHEN_FSELF) $< $@ --ptype fake --app-version 0x02000001 --fw-version 0x02000001
