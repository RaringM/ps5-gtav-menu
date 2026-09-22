# GTAV-Menu build orchestrator.
#
# The build was split out of a single 3,000-line file into modular fragments
# under make/. They are included in their original order, so this is a
# behaviour-preserving refactor (verified by diffing `make -n` per target):
#
#   make/config.mk   tunable variables (hosts, ports, native addresses, probe knobs)
#   make/profiles/   named production/smoke feature profiles
#   make/flags.mk    build dirs, the CFLAGS variants, and source lists
#   make/targets.mk  all build / analysis / packaging / verification rules
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

# Private development targets are present on dev and deliberately absent from
# publication branches. The optional include keeps the production Makefile
# standalone without duplicating build rules.
-include devtools/make/targets.mk
