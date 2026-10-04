# GTAV-Menu for PS5

GTAV-Menu is an in-process, single-player menu for the disc release of GTA V on PS5. This branch
packages **PPSA04264 / 01.005.000**, **PPSA04264 / 01.010.002**, and
**PPSA04263 / 01.010.002** as primary targets. The menu starts hidden and opens with
**R1 + D-pad Left**.

## Requirements

- A legally owned PPSA04264 or PPSA04263 installation at the exact content version named in your package.
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

For etaHEN, copy
`build/pkg/ppsa04264-01.010.002/etahen/GTAV00001.plugin` to `/data/etaHEN/plugins/`, then use etaHEN
Toolbox to enable **Running** or **Auto-start**. The visible plugin launches an embedded helper
through etaHEN's utility service so the helper receives a valid payload runtime and kernel binding.
When Toolbox stops the visible plugin, the helper observes its supervisor lease closing, runs the
normal menu shutdown, verifies the render callback and frame hook were restored, and exits.

Tag pushes matching `v*` build standalone, OnionHEN, and etaHEN packages for all three exact targets
and publish nine checksummed ZIPs. You can also run the release workflow manually with an existing
tag. Names identify the game build and delivery, for example
`GTAVMenu-PPSA04264-v01.005.000-etahen.zip` and `GTAVMenu-PPSA04264-v01.010.002-etahen.zip`.
The menu release tag and source commit are recorded in `release-manifest.json`.
Releases retain debug symbols; rebuilt artifacts require a new on-hardware regression before
distribution.

To package a specific game version locally:

```sh
make GTAV_TARGET=ppsa04264-01.005.000 package-target
make GTAV_TARGET=ppsa04264-01.010.002 package-target
make GTAV_TARGET=ppsa04263-01.010.002 package-target
```

## etaHEN container tools

`python3 tools/inspect_etahen_plugin.py example.plugin` validates container metadata and ELF
structure and reports SHA-256 hashes. `python3 tools/make_etahen_plugin.py --help` describes the
offline container writer. Both tools use only the Python standard library. The `make etahen` target
uses the same format implementation for both the embedded helper and installable supervisor.

## Custom asset intake

Custom cars, props, and textures are under development. **This build does not convert or load
custom assets.** The offline inspector is available now and needs only Python's standard library:

```sh
python3 tools/inspect_assets.py status
python3 tools/inspect_assets.py inspect /path/to/addon.zip --output build/assets/intake.json
```

Inputs can be folders, ordinary ZIPs, or supported unencrypted PC RPF7 containers. Inspection
checks container bounds, resource compression/page sizes, XML syntax, path safety, and duplicate
resource names/hashes. GXT2 localization tables and PC Legacy YTD texture names, formats, pointer
bounds, and mip spans are also inspected. Per-texture diagnostics remain conversion blockers even
when container inspection succeeds. Names containing the exact `script_rt_` marker are reported
as target-native special resources and are ineligible for ordinary static-texture conversion,
while their source payload diagnostics remain available. No textures are resized, dropped, or
replaced. Localized readme filenames are accepted only as non-loadable ancillary files.

For setup-defined PC add-ons, the report traces declared files and selected vehicle/model,
texture, handling, and tuning references. It flags missing files, duplicate IDs/hashes, and
possible cross-pack conflicts. Stock dependencies remain unverified; another pack or an
undeclared loose file cannot silently satisfy a pack-local reference. This is a partial
dependency report, not complete dependency closure or an activation plan.

Inspection does not establish PS5 GPU compatibility, metadata semantics, stock
dependency closure, or runtime registration safety. Unsupported table encodings and scripts are
reported rather than guessed or executed. ZIP64 and protected archives require decoded input.

Reports are deterministic JSON and must use a new filename outside the input tree. An `inspect`
exit code of zero means only that inspection completed without errors. `validate`, `convert`, and
`package` currently return a nonzero result with the outstanding qualification gates; they never
create a loadable bundle. No manifest or command-line override can enable unqualified loading.

The intended first release is additive-only, rejects unsupported dependencies, and requires a
fresh game process after pack changes. The existing loader and retail archives remain untouched.

Large PC add-on RPFs can be inventoried without loading the whole archive into memory, and exact
vehicle source fixtures can be imported into `build/` for conversion work:

```sh
python3 tools/import_pc_assets.py inventory path/to/dlc.rpf --output build/assets/pack-inventory.json
python3 tools/import_pc_assets.py vehicle path/to/dlc.rpf --model model_name \
  --output-dir build/assets/pc-fixtures/model-name
```

The importer preserves exact RSC7 resources and pack metadata, records hashes and source table
identities, and keeps conversion/upload/runtime gates false. Vehicle imports also isolate and hash
the matching vehicle, variation, handling, layout and light records when they are uniquely present;
ambiguous or stock references remain explicit in `metadata-selection/selection.json`. Console-side
authored content is reserved under `/data/GTAVMenu/custom`, with converted packs planned beneath
`custom/packs`.

The first authored 16 x 16 BC1 texture can now be assembled into a strict inactive package with
`make custom-texture-pack` and checked with `./menu-ctl.sh custom-verify`. Its upload/readback
tooling is additive and does not change the activation catalog. The engine mount and ownership
contract is still unqualified, so `custom-activate` and `custom-preview` fail closed and this is
not yet a live-test-ready custom asset.

## Custom maps from stock models

The production menu now has a bounded custom-map lane for authored scenes made from stock models.
`make custom-map` builds an eight-prop portable test yard, and `./menu-ctl.sh map-upload` verifies,
uploads and reads back its versioned `map.cfg`. Load it from **World > Spawned Entities > Load Map**
or with `./menu-ctl.sh map-load`; the menu supports cancellation and owns every created entity for
cleanup. `docs/custom-maps.md` in the development checkout describes the manifest format and live
acceptance sequence. This lane does not claim custom mesh, texture, collision or metadata loading.

Licensed under the [MIT License](LICENSE). GTA V, PlayStation, the PS5 payload SDK, and third-party
catalog inputs are not covered by that license. See `THIRD_PARTY_NOTICES` for catalog attribution.
