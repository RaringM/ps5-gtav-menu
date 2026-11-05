# GTAV-Menu for PS5

A single-player mod menu for GTA V Enhanced on a jailbroken PS5. Spawn and customize vehicles,
change your character and weapons, teleport, build scenes, and load custom content from PC mods.
The menu starts hidden; press **R1 + D-pad Left** to open it.

## Compatibility

Each universal package automatically selects the matching title ID and exact game content version.

| Title ID | Game version | Menu | Custom packs |
| --- | --- | --- | --- |
| PPSA04264 (US) | 01.005.000 | Supported | Unavailable |
| PPSA04264 (US) | 01.010.002 | Supported | Experimental |
| PPSA04263 (EU) | 01.010.002 | Supported | Experimental |
| PPSA04264 (US) | 01.011.001 | Supported | Experimental |
| CUSA00411 (PS4 Legacy) | 01.57 | Experimental plugin | Experimental |

On 01.011.001 the loader also grows the game's vehicle, ped and asset tables once per launch,
which leaves room for far more add-on vehicles and peds
(see [known limits](docs/user-guide.md#8-known-limits)). It must be running before GTA starts; a
loader started into a running game keeps the stock tables.

The PS4 Legacy row is a separate download, `GTAVMenu-PS4-CUSA00411-v01.57-plugin.zip`: a plugin for
GoldHEN 2.3+ or nexHEN PS4 v1.00 (firmware 9.00 tested), installed and checked as described in the
[PS4 plugin guide](docs/ps4-plugin.md).

Other title IDs and versions are not supported. This project is for **Story Mode**.
You need your own copy of the game, a jailbroken console, and a compatible payload launcher.
Universal selection has been tested on both 01.010.002 titles; 01.005.000 and 01.011.001 selection,
and the OnionHEN and etaHEN deliveries on 01.011.001, remain untested. The 01.011.001 exact-target
standalone loader passed its one-shot, persistent and pool-growth hardware gates.

## Install a package

Choose **one** delivery. Each universal bundle supports all four builds above and includes setup instructions.

| Delivery | Installation |
| --- | --- |
| Standalone | Send `gtav-menu-daemon.elf` with your payload launcher before starting GTA. All four workers are embedded; no separate worker upload is needed. |
| OnionHEN | Copy `GTAV00001.elf` to `/data/OnionHEN/plugins/`. In Toolbox, open **Payloads & Kernel > Plugins > GTAV Menu** and enable **Running**. The package targets OnionHEN v0.0.13. |
| etaHEN | Copy `GTAV00001.plugin` to `/data/etaHEN/plugins/`. Enable **Running** under **Toolbox > Plugins**. Targets the etaHEN 2.5B plugin interface; see [tested setups](docs/user-guide.md#delivery-compatibility-and-updates). |

Start GTA and load Story Mode. After the **GTAVMenu injected** notification, open the menu.
Packaged loaders watch for later GTA relaunches; plugin **Auto-start** also starts the watcher
when the launcher starts.

To stop or update a plugin, disable **Auto-start** and **Running** in its Toolbox, then wait for
`/data/GTAVMenu/daemon.lock` to disappear. For standalone, create an empty
`/data/GTAVMenu/daemon.stop` file. Run only one loader at a time.
After replacing an etaHEN plugin, reboot and reload etaHEN before enabling it to clear cached plugin code.

An experimental native nexHEN module is also implemented for **PPSA04264
01.010.002**. It requires the matching corrected protocol-4 loader because stock
nexHEN v0.90's attach-based native path has repeatedly broken GTA streaming.
After two fail-closed requests, the corrected v9/v7 route loaded and activated
version 1.0.1 on hardware. The menu rendered, callbacks advanced, gameplay
remained healthy and GTA exited normally. The nexHEN off row then issued a
second load because stock v0.90 ShellUI discarded its false value, so that run
did not exercise unload. Version 1.0.2 now propagates checked restoration
failures and latches an unsafe stop; the paired v10 loader patch sends command
32 with the exact row value. Their hardware follow-up completed one exact UI
load/unload: callbacks and cleanup succeeded, the original frame hook and render
slot were restored, the module was absent, the menu chord stopped opening it,
brief gameplay remained healthy and GTA exited normally. Repeated sustained
cycles remain required before this becomes a supported delivery. See the
[native nexHEN guide](docs/nexhen-plugin.md); do not treat it as a supported
release delivery yet.

## Controls and features

| Control | Action |
| --- | --- |
| R1 + D-pad Left | Open / hide |
| D-pad Up / Down | Navigate |
| D-pad Left / Right | Change a value |
| Cross | Select / apply |
| Circle | Back |

- **Vehicles:** class filters, previews, saved vehicles, spawning and LS Customs parts.
- **Player and world:** skins, wardrobe, weapons and attachments, teleportation, weather and time.
- **Scenes:** spawn and move objects, peds and vehicles; save and load your placements.
- **Custom packs:** vehicles, wheels, weapons, peds, clothing, props, maps, interiors, textures
  and effects on the 01.010.002 and 01.011.001 targets.
- **Interface:** English and Brazilian Portuguese, configurable appearance and keybinds.

## Build and package

Linux is the tested build host. Install **make**, **Python 3.11+**, **LLVM 20** (Clang, LLD and
llvm-config), and **PS5 payload SDK v0.44**. Select the SDK and toolchain explicitly:

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
export LLVM_CONFIG=/usr/bin/llvm-config-20
make package-universal
```

Universal ELFs are written under `build/ps5/universal/production/`; the three bundles are under
`build/pkg/universal/`. To build only one delivery, add `UNIVERSAL_DELIVERIES=standalone`
(or `onionhen` / `etahen`). Builds do not deploy to the console.

Exact-target builds remain available: `make GTAV_TARGET=ppsa04263-01.010.002 all` builds the EU
loader and worker, and `package-target` packages its three deliveries. The default target is
`ppsa04264-01.010.002`; use a lowercase title/version from the table.

`make package-nexhen` builds, validates and stages the hardware-unqualified
native test module under `build/pkg/ppsa04264-01.010.002/nexhen/`. The target
refuses every title/build other than PPSA04264 01.010.002 until the corrected
broker gains another exact profile. Building or packaging does not deploy it.

Alternatively, the included container pins the SDK and compiler:

```sh
podman build -t gtav-menu-builder .
podman run --rm --userns=keep-id -v "$PWD:/workspace:Z" gtav-menu-builder \
  package-universal
```

## Run from source

Host deployment also needs **curl**, **socat**, and the console's FTP, ps5debug and payload services
(default ports: 2121, 744 and 9021). Set your target and console address, start GTA, and wait until
you have control in Story Mode:

```sh
export GTAV_TARGET=ppsa04264-01.010.002  # ppsa04263-01.010.002 for EU, ppsa04264-01.011.001
export PS5_HOST=<console-ip>
./menu-ctl.sh cave-inject
./menu-ctl.sh doctor
```

To start the loader **before** launching GTA, use `./menu-ctl.sh watch` instead. Add `--persist`
to inject after each relaunch; `./menu-ctl.sh daemon-stop` stops that watcher. Use Toolbox to stop
an OnionHEN or etaHEN plugin. On 01.011.001, `watch` also grows the game's tables at launch;
`--no-boot-pools` keeps the stock tables.

For health checks of an installed universal package, use `GTAV_AUTO_TARGET=1 ./menu-ctl.sh doctor`.
Source injection and pack conversion still use an explicit `GTAV_TARGET`.

The loader checks the exact game build and verifies its writes. After a failed or interrupted
injection, restart GTA before trying again. Run `./menu-ctl.sh help` for all host commands.

## Custom packs

Custom assets require the console's `/data` to be shared with GTA by ShadowMountPlus or your HEN
setup. Without game access to that folder, the regular menu works but custom assets are unavailable.
Packs are installed separately and load only when you press **Load** in the menu:

```sh
./menu-ctl.sh pack-install /path/to/my-pack --activate-add
```

In Story Mode, open **Custom Packs > Manage Packs** to select content, then return and press
**Load N selected packs**. Up to 16 packs can be active, with 128 installed. Restart GTA to change
the loaded set; packs cannot be unloaded during a session.

The published converters build runtime packs from supported PC **Legacy** mods and templates
from your own game. Model converters need NumPy (`python3 -m pip install numpy`); ordinary menu
builds and deployment do not. Enhanced PC textures are supported, but Enhanced PC models are not.
No game files or third-party mod assets are included.

| Guide | Contents |
| --- | --- |
| [User guide](docs/user-guide.md) | Install and load packs, menu usage, known limits and troubleshooting |
| [Pack author guide](docs/pack-author-guide.md) | Obtain templates, convert mods, validate and share packs |
| [Pack reference](docs/pack-reference.md) | `pack.cfg` format, commands, limits and refusal messages |
| [Stock conversion coverage](docs/stock-conversion.md) | Supported replacements and clothing slots |

## License

See [LICENSE](LICENSE) and [third-party notices](THIRD_PARTY_NOTICES).
