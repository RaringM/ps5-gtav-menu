# GTAV-Menu for PS5

GTAV-Menu is an in-process, single-player menu for the disc release of GTA V on PS5. This branch
supports exactly **PPSA04264 / 01.010.002**. The menu starts hidden and opens with **R1 + D-pad
Left**.

## Requirements

- A legally owned PPSA04264 installation at content version 01.010.002.
- A jailbroken PS5 and a compatible payload launcher.
- Stock etaHEN 2.5B or newer when using the etaHEN plugin delivery.
- The PS5 payload SDK v0.43, set with `PS5_PAYLOAD_SDK` or installed at
  `$HOME/Projects/PS5/ps5-payload-sdk`.
- `make`, Python 3.11+, `curl`, `socat`, and LLVM 20. Direct builds reject another LLVM major; set
  `LLVM_CONFIG` to the absolute path of LLVM 20's `llvm-config` when multiple versions are
  installed.

The operator scripts use the Python modules included under `tools/`; no `pip install` or virtual
environment is required on the publication branch. Build metadata records the exact compiler
version so console failures can be matched to the ELF that produced them.

## Build and package

```sh
make all
make package-payload
make package-onionhen
make etahen
make package-etahen
```

`make all` builds unstripped production loader and worker ELFs under `build/ps5`. The package
targets create standalone, OnionHEN, and etaHEN bundles under `build/pkg`. `make etahen` builds the
same production worker inside a Toolbox-managed `GTAV00001.plugin`. Building does not deploy or
modify a console.

Fedora's system LLVM may be newer than the release toolchain. Build with the included Podman image
without replacing system packages:

```sh
podman build -t gtav-menu-ps5-builder .
podman run --rm --userns=keep-id -v "$PWD:/workspace:Z" \
  gtav-menu-ps5-builder all package-payload package-onionhen package-etahen
```

For host-native builds and `menu-ctl.sh` on Fedora 44, install the parallel LLVM 20 packages once
and select them explicitly:

```sh
sudo dnf install clang20 llvm20 llvm20-devel lld20
export LLVM_CONFIG=/usr/lib64/llvm20/bin/llvm-config
```

## Run

With GTA already in single-player and player control available:

```sh
PS5_HOST=<console-ip> ./menu-ctl.sh cave-inject
./menu-ctl.sh doctor
```

To arm the loader before starting GTA:

```sh
PS5_HOST=<console-ip> ./menu-ctl.sh watch
PS5_HOST=<console-ip> ./menu-ctl.sh watch --persist
./menu-ctl.sh daemon-stop
```

Run `./menu-ctl.sh` without arguments for all operator commands. The supported route is ptrace-free,
checks the exact game build, verifies writes, and does not permanently elevate the target. Use a
fresh game process after a failed or interrupted injection.

Controls: D-pad Up/Down navigates, D-pad Left/Right changes values, Cross selects, and Circle goes
back. R1 + D-pad Left opens or hides the menu.

For etaHEN, copy `build/pkg/etahen/GTAV00001.plugin` to `/data/etaHEN/plugins/`, then use etaHEN
Toolbox to enable **Running** or **Auto-start**. The visible plugin launches an embedded helper
through etaHEN's utility service so the helper receives a valid payload runtime and kernel binding.
When Toolbox stops the visible plugin, the helper observes its supervisor lease closing, runs the
normal menu shutdown, verifies the render callback and frame hook were restored, and exits.

Tag pushes matching `v*` build all three packages and publish checksummed release assets. Releases
retain debug symbols; rebuilt artifacts require a new on-hardware regression before distribution.

## etaHEN container tools

`python3 tools/inspect_etahen_plugin.py example.plugin` validates container metadata and ELF
structure and reports SHA-256 hashes. `python3 tools/make_etahen_plugin.py --help` describes the
offline container writer. Both tools use only the Python standard library. The `make etahen` target
uses the same format implementation for both the embedded helper and installable supervisor.

Licensed under the [MIT License](LICENSE). GTA V, PlayStation, the PS5 payload SDK, and third-party
catalog inputs are not covered by that license. See `THIRD_PARTY_NOTICES` for catalog attribution.
