# PS4 Legacy plugin: make ps4-plugin | ps4-plugin-package | ps4-plugin-release
#   [GTAV_PS4_TARGET=cusa00411-01.57] OO_PS4_TOOLCHAIN=/path/to/OpenOrbis-PS4-Toolchain
#
# A fake-signed SELF PRX (gtav_menu.prx) for the GoldHEN 2.3+ and nexHEN PS4 v1.00 plugin loaders.
# Every target-specific input comes from data/targets/ps4/$(GTAV_PS4_TARGET).json: tools/ps4_build.py
# validates it and writes the make fragment that names the target's generated headers and the -D
# flags derived from its build.features. The generated headers are committed. OpenOrbis v0.5.4
# (toolchain-llvm-18) provides the headers, link.x, the libraries and create-fself.
GTAV_PS4_TARGET ?= cusa00411-01.57
# The manifest pins the compiler major (build.toolchain.llvmMajor); prefer that LLVM's own bin dir.
PS4_LLVM_BINDIR ?= $(firstword $(wildcard /usr/lib64/llvm20/bin /usr/lib/llvm-20/bin))
PS4_CC ?= $(if $(PS4_LLVM_BINDIR),$(PS4_LLVM_BINDIR)/clang,clang)
PS4_CXX ?= $(if $(PS4_LLVM_BINDIR),$(PS4_LLVM_BINDIR)/clang++,clang++)
PS4_TARGET_MANIFEST := data/targets/ps4/$(GTAV_PS4_TARGET).json
PS4_BUILD_DIR := build/ps4/$(GTAV_PS4_TARGET)
PS4_OBJ_DIR := $(PS4_BUILD_DIR)/obj
PS4_CONFIG_MK := $(PS4_BUILD_DIR)/config.mk

PS4_PLUGIN_BUILD_DIR := $(PS4_BUILD_DIR)/plugin
PS4_PLUGIN_OBJ_DIR := $(PS4_PLUGIN_BUILD_DIR)/obj
PS4_PLUGIN_FLAGS_FILE := $(PS4_PLUGIN_OBJ_DIR)/build-flags.txt
PS4_PLUGIN_ELF := $(PS4_PLUGIN_BUILD_DIR)/gtav_menu.elf
PS4_PLUGIN_OELF := $(PS4_PLUGIN_BUILD_DIR)/gtav_menu.oelf
PS4_PLUGIN_PRX := $(PS4_PLUGIN_BUILD_DIR)/gtav_menu.prx
# Boot-time pool growth in the PRX: full for a release; off, observe or caps make a probe build,
# packaged apart (ps4-plugin-probe-<mode>) and labeled in the plugin description, its load notification
# and validation.json. A mode change rebuilds the PRX (the define is in its flags file).
PS4_BOOT_POOLS_MODE ?= full
PS4_BOOT_POOLS_MODES := off observe caps full
PS4_BOOT_POOLS_MODE_NUMBER := $(if $(filter off,$(PS4_BOOT_POOLS_MODE)),0,$(if $(filter observe,$(PS4_BOOT_POOLS_MODE)),1,$(if \
	$(filter caps,$(PS4_BOOT_POOLS_MODE)),2,$(if $(filter full,$(PS4_BOOT_POOLS_MODE)),3,))))
PS4_BOOT_POOLS_PROBE := $(if $(filter full,$(PS4_BOOT_POOLS_MODE)),,$(PS4_BOOT_POOLS_MODE))
PS4_PLUGIN_PACKAGE_DIR := build/pkg/$(GTAV_PS4_TARGET)/ps4-plugin$(if $(PS4_BOOT_POOLS_PROBE),-probe-$(PS4_BOOT_POOLS_PROBE))
PS4_PLUGIN_HEADER := include/gtavmenu/ps4_plugin_$(GTAV_PS4_TARGET)_generated.h
PS4_BOOT_POOL_DATA_HEADER := include/gtavmenu/boot_pool_data_$(GTAV_PS4_TARGET)_generated.h
PS4_PLUGIN_LD ?= $(if $(PS4_LLVM_BINDIR),$(PS4_LLVM_BINDIR)/ld.lld,ld.lld)
# OpenOrbis has no default location: set OO_PS4_TOOLCHAIN. Everything that names it expands late, so
# a value set after this file is read still applies; the flags-file recipe checks it before any compile.
PS4_PLUGIN_CREATE_FSELF ?= $(OO_PS4_TOOLCHAIN)/bin/linux/create-fself

ifneq ($(filter ps4%,$(MAKECMDGOALS)),)
ifeq ($(PS4_BOOT_POOLS_MODE_NUMBER),)
$(error PS4_BOOT_POOLS_MODE must be one of $(PS4_BOOT_POOLS_MODES), not '$(PS4_BOOT_POOLS_MODE)')
endif
$(PS4_CONFIG_MK): $(PS4_TARGET_MANIFEST) tools/ps4_build.py tools/gtavmenu_tools/ps4_build.py \
		tools/gtavmenu_tools/target_profile.py
	python3 -I tools/ps4_build.py --target-manifest $< --make-fragment $@
include $(PS4_CONFIG_MK)
ifneq ($(PS4_LLVM_MAJOR),)
PS4_CC_MAJOR := $(firstword $(subst ., ,$(shell $(PS4_CC) -dumpversion 2>/dev/null)))
ifneq ($(PS4_CC_MAJOR),$(PS4_LLVM_MAJOR))
ifneq ($(PS4_ALLOW_UNPINNED_LLVM),1)
$(error $(GTAV_PS4_TARGET) pins LLVM $(PS4_LLVM_MAJOR) but $(PS4_CC) is LLVM '$(PS4_CC_MAJOR)'; set PS4_LLVM_BINDIR \
	(or PS4_ALLOW_UNPINNED_LLVM=1 for an unvalidated experiment))
endif
endif
endif
endif

# The menu objects are freestanding: clang's own headers (stdint, stdarg, ...) plus the small
# vendored libc surface in src/module/ps4/include, which declares only what src/module/ps4 implements.
PS4_CPPFLAGS := -Iinclude -nostdlibinc -isystem src/module/ps4/include $(PS4_TARGET_DEFINES) \
	$(if $(PS4_CUSTOM_LANE_HEADER),-include $(PS4_CUSTOM_LANE_HEADER),)
PS4_COMMON_FLAGS := -O2 -g -fPIE -fPIC -mcmodel=small -march=btver2 -mtune=btver2 \
	-m64 -mabi=sysv -masm=intel -ffreestanding -fno-builtin -fno-stack-protector \
	-ffunction-sections -fdata-sections -Wall -Wextra -Werror -MMD -MP \
	-ffile-prefix-map=$(CURDIR)=. -fdebug-prefix-map=$(CURDIR)=. -fdebug-compilation-dir=.
PS4_CFLAGS := $(PS4_COMMON_FLAGS) -std=c11
PS4_CXXFLAGS := $(PS4_COMMON_FLAGS) -std=c++17 -fno-exceptions -fno-rtti

PS4_PLUGIN_CPPFLAGS := $(PS4_CPPFLAGS) -DORBIS -D_GNU_SOURCE \
	-DGTAV_PS4_BOOT_POOLS_MODE=$(PS4_BOOT_POOLS_MODE_NUMBER)
PS4_PLUGIN_ORBIS_CPPFLAGS = -Iinclude -DORBIS -D_GNU_SOURCE $(PS4_TARGET_DEFINES) \
	-DGTAV_PS4_BOOT_POOLS_MODE=$(PS4_BOOT_POOLS_MODE_NUMBER) \
	-isysroot $(OO_PS4_TOOLCHAIN) -isystem $(OO_PS4_TOOLCHAIN)/include
# The toolchain path is mapped like the checkout's, so the PRX bytes do not depend on where
# OpenOrbis is installed (its headers otherwise land in the debug info).
PS4_PLUGIN_COMMON_FLAGS = $(PS4_COMMON_FLAGS) --target=x86_64-pc-freebsd12-elf \
	-fvisibility=hidden -funwind-tables -ffile-prefix-map=$(OO_PS4_TOOLCHAIN)=/OpenOrbis/PS4Toolchain
PS4_PLUGIN_CFLAGS = $(PS4_PLUGIN_COMMON_FLAGS) -std=c11
PS4_PLUGIN_CXXFLAGS = $(PS4_PLUGIN_COMMON_FLAGS) -std=c++17 -fno-exceptions -fno-rtti \
	-isystem $(OO_PS4_TOOLCHAIN)/include/c++/v1
PS4_PLUGIN_LDFLAGS = -m elf_x86_64 -pie --export-dynamic --script $(OO_PS4_TOOLCHAIN)/link.x -e _init \
	--eh-frame-hdr --gc-sections -L$(OO_PS4_TOOLCHAIN)/lib
# Only the console's own libc and kernel stubs: OpenOrbis's static libc.a (musl) is never linked, so a
# symbol only it defines fails the link instead of landing in the PRX.
PS4_PLUGIN_LIBS := -lSceLibcInternal -lkernel

# The menu objects every PS4 delivery shares. Link order is part of the layout; keep it stable. The
# plugin's entry/CRT files and all plugin objects live in their own object tree.
PS4_PLATFORM_C := control worker memory format math platform storage_probe stdio_fs pad_hook \
	pack_platform
PS4_PLATFORM_CXX := bridge
PS4_MODULE_C := menu_draw_list native_job_queue pad_input feature_catalog menu_controller \
	command_mailbox $(PS4_FEATURE_MODULE_C)
PS4_COMMON_C := scene_map strutil localization rootdir $(PS4_FEATURE_COMMON_C)
PS4_MODULE_CXX := native_bridge features
PS4_SHARED_OBJECTS := $(addprefix $(PS4_OBJ_DIR)/ps4/,$(addsuffix .o,$(PS4_PLATFORM_C) $(PS4_PLATFORM_CXX))) \
	$(addprefix $(PS4_OBJ_DIR)/module/,$(addsuffix .o,$(PS4_MODULE_C))) \
	$(addprefix $(PS4_OBJ_DIR)/common/,$(addsuffix .o,$(PS4_COMMON_C))) \
	$(addprefix $(PS4_OBJ_DIR)/module/,$(addsuffix .o,$(PS4_MODULE_CXX))) \
	$(PS4_OBJ_DIR)/ps4/game.o
PS4_PLUGIN_OBJECTS := $(filter-out $(PS4_PLUGIN_OBJ_DIR)/common/sha256.o,\
	$(patsubst $(PS4_OBJ_DIR)/%,$(PS4_PLUGIN_OBJ_DIR)/%,$(PS4_SHARED_OBJECTS))) \
	$(PS4_PLUGIN_OBJ_DIR)/common/sha256.o \
	$(PS4_PLUGIN_OBJ_DIR)/common/boot_pool_data.o \
	$(PS4_PLUGIN_OBJ_DIR)/ps4/plugin_runtime.o \
	$(PS4_PLUGIN_OBJ_DIR)/ps4/plugin_log.o \
	$(PS4_PLUGIN_OBJ_DIR)/ps4/plugin_boot_pools.o \
	$(PS4_PLUGIN_OBJ_DIR)/ps4/plugin_entry.o \
	$(PS4_PLUGIN_OBJ_DIR)/ps4/plugin_crt.o
# Generated from the target manifest and committed (the source tree regenerates them).
PS4_GENERATED_HEADERS := $(PS4_HEADER) $(PS4_CAPABILITY_HEADER) $(PS4_FEATURE_HEADER) \
	$(PS4_SCRIPT_GLOBALS_HEADER) $(PS4_CUSTOM_LANE_HEADER) $(PS4_PLUGIN_HEADER) $(PS4_BOOT_POOL_DATA_HEADER)

.PHONY: FORCE ps4-plugin ps4-plugin-package ps4-plugin-release
FORCE:

ps4-plugin: $(PS4_PLUGIN_PRX)
	python3 -I tools/inspect_ps4_plugin.py --elf $(PS4_PLUGIN_ELF) --prx $(PS4_PLUGIN_PRX) \
		--target-manifest $(PS4_TARGET_MANIFEST) --toolchain $(OO_PS4_TOOLCHAIN)

ps4-plugin-package: ps4-plugin
	python3 -I tools/package_ps4_plugin.py --elf $(PS4_PLUGIN_ELF) --prx $(PS4_PLUGIN_PRX) \
		--target-manifest $(PS4_TARGET_MANIFEST) --toolchain $(OO_PS4_TOOLCHAIN) --output $(PS4_PLUGIN_PACKAGE_DIR) \
		$(if $(PS4_BOOT_POOLS_PROBE),--probe $(PS4_BOOT_POOLS_PROBE))

# The release package: boot pools in the full mode, whatever PS4_BOOT_POOLS_MODE says.
ps4-plugin-release:
	$(MAKE) PS4_BOOT_POOLS_MODE=full ps4-plugin-package

$(PS4_PLUGIN_FLAGS_FILE): FORCE | $(PS4_PLUGIN_OBJ_DIR)
	@test -n "$(OO_PS4_TOOLCHAIN)" && test -f "$(OO_PS4_TOOLCHAIN)/link.x" || { \
		echo "set OO_PS4_TOOLCHAIN to an OpenOrbis PS4 toolchain (v0.5.4, toolchain-llvm-18) with link.x;" \
			"got '$(OO_PS4_TOOLCHAIN)'" >&2; exit 1; }
	@printf '%s\n' '$(PS4_PLUGIN_CPPFLAGS)' '$(PS4_PLUGIN_ORBIS_CPPFLAGS)' \
		'$(PS4_PLUGIN_COMMON_FLAGS)' '$(PS4_PLUGIN_LDFLAGS) $(PS4_PLUGIN_LIBS)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm $@.tmp; fi

$(PS4_PLUGIN_OBJECTS): $(PS4_PLUGIN_FLAGS_FILE) $(PS4_GENERATED_HEADERS)

$(PS4_PLUGIN_OBJ_DIR) $(PS4_PLUGIN_OBJ_DIR)/ps4 $(PS4_PLUGIN_OBJ_DIR)/module \
	$(PS4_PLUGIN_OBJ_DIR)/common:
	mkdir -p $@

$(PS4_PLUGIN_OBJ_DIR)/ps4/plugin_runtime.o: src/module/ps4/plugin_runtime.cpp \
		include/gtavmenu/ps4_plugin.h $(PS4_HEADER) $(PS4_PLUGIN_HEADER) | $(PS4_PLUGIN_OBJ_DIR)/ps4
	$(PS4_CXX) $(PS4_PLUGIN_ORBIS_CPPFLAGS) $(PS4_PLUGIN_CXXFLAGS) -c $< -o $@

$(PS4_PLUGIN_OBJ_DIR)/ps4/plugin_log.o: src/module/ps4/plugin_log.cpp \
		include/gtavmenu/ps4_plugin_log.h | $(PS4_PLUGIN_OBJ_DIR)/ps4
	$(PS4_CXX) $(PS4_PLUGIN_ORBIS_CPPFLAGS) $(PS4_PLUGIN_CXXFLAGS) -c $< -o $@

$(PS4_PLUGIN_OBJ_DIR)/ps4/plugin_entry.o: src/module/ps4/plugin_entry.cpp \
		include/gtavmenu/ps4_plugin.h include/gtavmenu/ps4_plugin_log.h \
		include/gtavmenu/ps4_plugin_boot_pools.h include/gtavmenu/plugin_lifecycle.h \
		| $(PS4_PLUGIN_OBJ_DIR)/ps4
	$(PS4_CXX) $(PS4_PLUGIN_ORBIS_CPPFLAGS) $(PS4_PLUGIN_CXXFLAGS) -c $< -o $@

$(PS4_PLUGIN_OBJ_DIR)/ps4/plugin_crt.o: src/module/ps4/plugin_crt.c | $(PS4_PLUGIN_OBJ_DIR)/ps4
	$(PS4_CC) $(PS4_PLUGIN_ORBIS_CPPFLAGS) $(PS4_PLUGIN_CFLAGS) -c $< -o $@

$(PS4_PLUGIN_OBJ_DIR)/ps4/%.o: src/module/ps4/%.c | $(PS4_PLUGIN_OBJ_DIR)/ps4
	$(PS4_CC) $(PS4_PLUGIN_CPPFLAGS) $(PS4_PLUGIN_CFLAGS) -c $< -o $@

$(PS4_PLUGIN_OBJ_DIR)/ps4/%.o: src/module/ps4/%.cpp | $(PS4_PLUGIN_OBJ_DIR)/ps4
	$(PS4_CXX) $(PS4_PLUGIN_CPPFLAGS) $(PS4_PLUGIN_CXXFLAGS) -c $< -o $@

$(PS4_PLUGIN_OBJ_DIR)/module/%.o: src/module/%.c | $(PS4_PLUGIN_OBJ_DIR)/module
	$(PS4_CC) $(PS4_PLUGIN_CPPFLAGS) $(PS4_PLUGIN_CFLAGS) -c $< -o $@

$(PS4_PLUGIN_OBJ_DIR)/module/%.o: src/module/%.cpp | $(PS4_PLUGIN_OBJ_DIR)/module
	$(PS4_CXX) $(PS4_PLUGIN_CPPFLAGS) $(PS4_PLUGIN_CXXFLAGS) -c $< -o $@

$(PS4_PLUGIN_OBJ_DIR)/common/%.o: src/common/%.c | $(PS4_PLUGIN_OBJ_DIR)/common
	$(PS4_CC) $(PS4_PLUGIN_CPPFLAGS) $(PS4_PLUGIN_CFLAGS) -c $< -o $@

$(PS4_PLUGIN_ELF): $(PS4_PLUGIN_OBJECTS)
	$(PS4_PLUGIN_LD) $(PS4_PLUGIN_OBJECTS) -o $@ $(PS4_PLUGIN_LDFLAGS) $(PS4_PLUGIN_LIBS)

$(PS4_PLUGIN_PRX): $(PS4_PLUGIN_ELF)
	@test -x "$(PS4_PLUGIN_CREATE_FSELF)" || { echo "missing OpenOrbis create-fself: $(PS4_PLUGIN_CREATE_FSELF)" >&2; exit 1; }
	@# create-fself refuses to run unless OO_PS4_TOOLCHAIN is in its environment; a makefile default
	@# is not exported on its own.
	OO_PS4_TOOLCHAIN=$(OO_PS4_TOOLCHAIN) $(PS4_PLUGIN_CREATE_FSELF) -in=$< -out=$(PS4_PLUGIN_OELF) --lib=$@ \
		--paid 0x3800000000000011 --libname gtav_menu

# Header and .inc dependencies (a stale object once cost a hardware attempt).
-include $(PS4_PLUGIN_OBJECTS:.o=.d)
