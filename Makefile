# GTAV Menu build.
#
#   make/config.mk      tunable variables (target, console address, ports, feature knobs)
#   make/profiles/      named production/smoke feature profiles
#   make/flags.mk       build directories, compiler flags and source lists
#   make/targets.mk     build, packaging and verification rules (includes make/universal.mk)
#   make/ps4-plugin.mk  the PS4 Legacy plugin (make ps4-plugin, ps4-plugin-package, ps4-plugin-release)
#
# config.mk must precede the toolchain include (it sets PS5_PAYLOAD_SDK and the
# values prospero.mk consumes); flags.mk must follow it (CFLAGS uses WARNINGS and
# INCLUDES from prospero.mk); targets.mk comes last and holds the default goal.

include make/config.mk
include make/profiles/$(GTAV_BUILD_PROFILE).mk

PS5_TOOLCHAIN_FILE := $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
ifneq ($(wildcard $(PS5_TOOLCHAIN_FILE)),)
include $(PS5_TOOLCHAIN_FILE)
PS5_TOOLCHAIN_AVAILABLE := 1
else
# Host-only checks still need to inspect the effective Make database on machines
# without the PS5 SDK. Keep the tool paths concrete for dry runs, then fail any
# build or deploy target through require-ps5-sdk in make/targets.mk.
PS5_TOOLCHAIN_AVAILABLE := 0
PS5_DEPLOY := $(PS5_PAYLOAD_SDK)/bin/prospero-deploy
CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
CXX := $(PS5_PAYLOAD_SDK)/bin/prospero-clang++
endif

include make/flags.mk
include make/targets.mk
include make/ps4-plugin.mk
