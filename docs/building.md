# Building from source

Release ZIPs are built by the same commands this page describes. You only need this page to build
the menu yourself, to inject it from a PC instead of a package, or to work on the code. Linux is the
tested build host.

## PS5 packages

Install **make**, **Python 3.11+**, **LLVM 20** (Clang, LLD and `llvm-config`) and the
**PS5 payload SDK v0.44**, then select the SDK and the toolchain explicitly:

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
export LLVM_CONFIG=/usr/bin/llvm-config-20
make package-universal
```

The universal ELFs are written under `build/ps5/universal/production/` and the three packages
(standalone, OnionHEN, etaHEN) under `build/pkg/universal/`. To build one delivery only, add
`UNIVERSAL_DELIVERIES=standalone` (or `onionhen` / `etahen`). Building never deploys anything to
the console.

Exact-target builds are also available: `make GTAV_TARGET=ppsa04263-01.010.002 all` builds the EU
loader and worker, and `make GTAV_TARGET=... package-target` packages its three deliveries. The
default target is `ppsa04264-01.010.002`; target names are the lowercase title ID and game version
from the README's table (`ppsa04264-01.011.001`, `ppsa04264-01.005.000`, ...).

The included container pins the PS5 SDK and the compiler (it does not include the PS4 toolchain):

```sh
podman build -t gtav-menu-builder .
podman run --rm --userns=keep-id -v "$PWD:/workspace:Z" gtav-menu-builder package-universal
```

`make help` lists the other targets.

## PS4 plugin

The PS4 plugin builds with clang and ld.lld from LLVM 20 and the OpenOrbis PS4 toolchain
**v0.5.4** (`toolchain-llvm-18.tar.gz`, SHA-256
`3c7cd5bb593ca74fa1c13fd59f3938dc0fc07985167f7275063019e63abe4526`):

```sh
make ps4-plugin-release OO_PS4_TOOLCHAIN=/path/to/OpenOrbis/PS4Toolchain
```

The package lands in `build/pkg/cusa00411-01.57/ps4-plugin/` with `validation.json` and
`package-manifest.json` (hashes, source commit, boot-pool mode). The PRX bytes do not depend on
where the checkout or the toolchain is installed. `validation.json` also records the toolchain:
the compiler and linker lines left in the ELF, and the SHA-256 of the OpenOrbis files that shape the
PRX (`link.x`, the `libkernel` and `libSceLibcInternal` stubs, `create-fself` and the headers), with
`pinned` true when they are the v0.5.4 release's. `tools/release.py` ships a PRX only when the pinned
OpenOrbis files and LLVM 20 built it. The plugin links only the console's libc and kernel stubs.

## Release assets

`tools/release.py` assembles and checks a release from the built packages:

```sh
make package-universal
make ps4-plugin-release OO_PS4_TOOLCHAIN=/path/to/OpenOrbis/PS4Toolchain
python3 tools/release.py assemble --tag vX.Y.Z --ps4-plugin
python3 tools/release.py verify --tag vX.Y.Z
```

Run it with the tag checked out. It writes the ZIPs, `release-manifest.json` and `SHA256SUMS`
listed in the README to `build/release/`. Pushing a `v*`
tag runs the same steps in the Forgejo and GitHub release workflows, which keep the result as a
workflow artifact for 14 days; they do not publish a release. `python3 tools/release.py publish
--platform forgejo|github --tag vX.Y.Z --assets DIR` publishes a verified asset folder (it reads
`FORGEJO_TOKEN` or `GITHUB_TOKEN`).

## Injecting from a PC

A PC can inject the menu it built instead of using a package. This needs **curl**, **socat**, and
the console's FTP, ps5debug and payload services (default ports 2121, 744 and 9021). Set the target
and the console's address, start GTA, wait until you control your character in Story Mode, then:

```sh
export GTAV_TARGET=ppsa04264-01.010.002  # ppsa04263-01.010.002 for EU, ppsa04264-01.011.001
export PS5_HOST=<console-ip>
./menu-ctl.sh cave-inject
./menu-ctl.sh doctor
```

To start the loader **before** launching GTA, run `./menu-ctl.sh watch` instead; add `--persist` to
inject after every relaunch, and `./menu-ctl.sh daemon-stop` stops that watcher. On 01.011.001,
`watch` also grows the game's tables at launch; `--no-boot-pools` keeps the stock tables.

The loader checks the exact game build and verifies its writes. After a failed or interrupted
injection, restart GTA before trying again. For a health check of an installed universal package,
use `GTAV_AUTO_TARGET=1 ./menu-ctl.sh doctor`. `./menu-ctl.sh help` lists every host command; the
[tools index](tools.md) says which script each one runs.
