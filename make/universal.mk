# Universal deliveries retain three separately compiled, exact-target workers.
# The host orchestrator builds them sequentially and validates every manifest,
# configuration, hash and mapped span before generating the runtime registry.
UNIVERSAL_DELIVERIES ?= standalone onionhen etahen
UNIVERSAL_DIR := $(BUILD_DIR)/universal/production
UNIVERSAL_DELIVERY_DIR := $(UNIVERSAL_DIR)/$(GTAV_DELIVERY)
UNIVERSAL_PACKAGE_DIR ?= build/pkg/universal
UNIVERSAL_INPUTS := $(UNIVERSAL_DIR)/loader_profiles.h $(UNIVERSAL_DIR)/embedded_workers.S \
	$(UNIVERSAL_DIR)/registry.json $(UNIVERSAL_DELIVERY_DIR)/build-config.json \
	$(UNIVERSAL_DELIVERY_DIR)/universal_identity.h $(BUILD_VERSION_HEADER)
UNIVERSAL_RUNTIME := $(UNIVERSAL_DELIVERY_DIR)/$(if $(filter standalone,$(GTAV_DELIVERY)),gtav-menu-payload-loader.elf,$(if $(filter onionhen,$(GTAV_DELIVERY)),gtav-menu-onion-daemon.elf,gtav-menu-etahen-runtime.elf))

.PHONY: universal package-universal _universal-link
universal: | require-ps5-sdk
	$(PYTHON) tools/build_universal.py --sdk "$(PS5_PAYLOAD_SDK)" --build-dir "$(BUILD_DIR)" \
		--deliveries $(UNIVERSAL_DELIVERIES) --onion-version $(ONIONHEN_PLUGIN_VERSION) \
		--eta-version $(ETAHEN_PLUGIN_VERSION)

package-universal: | require-ps5-sdk
	$(PYTHON) tools/build_universal.py --sdk "$(PS5_PAYLOAD_SDK)" --build-dir "$(BUILD_DIR)" \
		--deliveries $(UNIVERSAL_DELIVERIES) --onion-version $(ONIONHEN_PLUGIN_VERSION) \
		--eta-version $(ETAHEN_PLUGIN_VERSION) --package --package-dir "$(UNIVERSAL_PACKAGE_DIR)"

# Only common safety/lifecycle gates are compile-time settings. Every live target
# address, render contract and custom-pack capability comes from loader_profiles.h.
UNIVERSAL_CFLAGS := $(CFLAGS) $(LOADER_VERSION_CFLAGS) -I$(UNIVERSAL_DIR) -I$(UNIVERSAL_DELIVERY_DIR) \
	-DGTAV_LOADER_UNIVERSAL=1 -DGTAV_LOADER_PROBE_NOSTOP=1 \
	-DGTAV_PROC_NOSTOP_IO=1 -DGTAV_PROC_NOSTOP_STRICT=1 \
	-DGTAV_ELF_INJECT_VERIFY_WRITES=1 -DGTAV_SDK_HAS_PROC_COPYIO=$(GTAV_SDK_HAS_PROC_COPYIO) \
	-DGTAV_LOADER_CAVE_BOOTSTRAP=1 -DGTAV_LOADER_CAVE_INJECT=1 -DGTAV_LOADER_CLASSIC_INJECT=0 \
	-DGTAV_LOADER_CAVE_TIMEOUT_MS=$(PAYLOAD_LOADER_CAVE_TIMEOUT_MS) \
	-DGTAV_MENU_PAYLOAD_INJECT=1 -DGTAV_MENU_INSTALL_PATCH_BROKER=1 \
	-DGTAV_LOADER_INSTALL_RENDER_PHASE=1 -DGTAV_PAYLOAD_WAIT_FOR_GAME=1 \
	-DGTAV_PAYLOAD_WAIT_TIMEOUT_SEC=0 -DGTAV_PAYLOAD_PERSISTENT=1 \
	-DGTAV_PAYLOAD_SP_READY=1 -DGTAV_PAYLOAD_SP_READY_MODE=1 -DGTAV_PAYLOAD_SP_READY_DEREF=1 \
	-DGTAV_PAYLOAD_SP_READY_CONFIRMATIONS=3 \
	-DGTAV_PAYLOAD_SP_READY_SIZE=8 \
	-DGTAV_PAYLOAD_SP_READY_MASK=0xffffffffffffffffull \
	-DGTAV_PAYLOAD_SP_READY_VALUE=0 \
	-DGTAV_PAYLOAD_SP_READY_TIMEOUT_SEC=$(PAYLOAD_LOADER_SP_READY_TIMEOUT_SEC) \
	-DGTAV_PAYLOAD_SP_READY_SETTLE_USEC=$(PAYLOAD_LOADER_SP_READY_SETTLE_USEC) \
	-DGTAV_PAYLOAD_SP_READY_GENTLE=$(PAYLOAD_LOADER_SP_READY_GENTLE) \
	-DGTAV_PAYLOAD_SP_READY_POLL_USEC=$(PAYLOAD_LOADER_SP_READY_POLL_USEC) \
	-DGTAV_PAYLOAD_SP_READY_POLL_MAX_USEC=$(PAYLOAD_LOADER_SP_READY_POLL_MAX_USEC) \
	-DGTAV_PAYLOAD_INJECT_GUARD=1 \
	-DGTAV_LOADER_VERIFY_VERSION=1 -DGTAV_MENU_EMBEDDED_WORKER=1

_universal-link: $(UNIVERSAL_INPUTS) | require-ps5-sdk
	$(CC) $(UNIVERSAL_CFLAGS) \
		$(if $(filter etahen,$(GTAV_DELIVERY)),-DGTAV_MANAGED_RUNTIME=1) \
		$(LDFLAGS) -o $(UNIVERSAL_RUNTIME) $(PAYLOAD_LOADER_SRCS) \
		$(if $(filter etahen,$(GTAV_DELIVERY)),src/common/supervisor_lifecycle.c) \
		$(UNIVERSAL_DIR)/embedded_workers.S $(PAYLOAD_LOADER_LDLIBS)
ifeq ($(GTAV_DELIVERY),onionhen)
	$(CC) $(CFLAGS) '-DGTAV_ONION_PLUGIN_VERSION="$(ONIONHEN_PLUGIN_VERSION)"' \
		'-DGTAV_EMBEDDED_LOADER_PATH="$(abspath $(UNIVERSAL_RUNTIME))"' \
		$(LDFLAGS) -o $(UNIVERSAL_DELIVERY_DIR)/GTAV00001.elf \
		src/payload_loader/onion_plugin.c src/payload_loader/embedded_loader.S
	$(PYTHON) tools/inspect_onion_plugin.py $(UNIVERSAL_DELIVERY_DIR)/GTAV00001.elf \
		--expect-id GTAV00001 --expect-version $(ONIONHEN_PLUGIN_VERSION)
endif
ifeq ($(GTAV_DELIVERY),etahen)
	$(RM) $(UNIVERSAL_DELIVERY_DIR)/GTAV00002.plugin.tmp
	$(PYTHON) tools/make_etahen_plugin.py $(UNIVERSAL_RUNTIME) \
		--output $(UNIVERSAL_DELIVERY_DIR)/GTAV00002.plugin.tmp \
		--plugin-id GTAV00002 --version $(ETAHEN_RUNTIME_VERSION)
	mv $(UNIVERSAL_DELIVERY_DIR)/GTAV00002.plugin.tmp $(UNIVERSAL_DELIVERY_DIR)/GTAV00002.plugin
	$(CC) $(CFLAGS) '-DGTAV_ETAHEN_PLUGIN_ID="GTAV00001"' '-DGTAV_ETAHEN_RUNTIME_ID="GTAV00002"' \
		'-DGTAV_ETAHEN_RUNTIME_VERSION="$(ETAHEN_RUNTIME_VERSION)"' \
		'-DGTAV_EMBEDDED_ETAHEN_RUNTIME_PATH="$(abspath $(UNIVERSAL_DELIVERY_DIR)/GTAV00002.plugin)"' \
		$(LDFLAGS) -o $(UNIVERSAL_DELIVERY_DIR)/gtav-menu-etahen-supervisor.elf \
		src/payload_loader/etahen_plugin.c src/common/daemon_control.c \
		src/common/supervisor_lifecycle.c src/payload_loader/embedded_etahen_runtime.S
	$(RM) $(UNIVERSAL_DELIVERY_DIR)/GTAV00001.plugin.tmp
	$(PYTHON) tools/make_etahen_plugin.py $(UNIVERSAL_DELIVERY_DIR)/gtav-menu-etahen-supervisor.elf \
		--output $(UNIVERSAL_DELIVERY_DIR)/GTAV00001.plugin.tmp --plugin-id GTAV00001 --version $(ETAHEN_PLUGIN_VERSION)
	mv $(UNIVERSAL_DELIVERY_DIR)/GTAV00001.plugin.tmp $(UNIVERSAL_DELIVERY_DIR)/GTAV00001.plugin
	$(PYTHON) tools/inspect_etahen_plugin.py $(UNIVERSAL_DELIVERY_DIR)/GTAV00002.plugin \
		--expect-id GTAV00002 --expect-version $(ETAHEN_RUNTIME_VERSION)
	$(PYTHON) tools/inspect_etahen_plugin.py $(UNIVERSAL_DELIVERY_DIR)/GTAV00001.plugin \
		--expect-id GTAV00001 --expect-version $(ETAHEN_PLUGIN_VERSION)
endif
