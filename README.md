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

Other title IDs and versions are not supported. This project is for **Story Mode**.
You need your own copy of the game, a jailbroken console, and a compatible payload launcher.
Universal selection has been tested on both 01.010.002 titles; 01.005.000 selection remains untested.

## Install a package

Choose **one** delivery. Each universal bundle supports all three builds above and includes setup instructions.

| Delivery | Installation |
| --- | --- |
| Standalone | Send `gtav-menu-daemon.elf` with your payload launcher before starting GTA. All three workers are embedded; no separate worker upload is needed. |
| OnionHEN | Copy `GTAV00001.elf` to `/data/OnionHEN/plugins/`. In Toolbox, open **Payloads & Kernel > Plugins > GTAV Menu** and enable **Running**. The package targets OnionHEN v0.0.13. |
| etaHEN | Copy `GTAV00001.plugin` to `/data/etaHEN/plugins/`. Enable **Running** under **Toolbox > Plugins**. Targets the etaHEN 2.5B plugin interface; see [tested setups](docs/user-guide.md#delivery-compatibility-and-updates). |

Start GTA and load Story Mode. After the **GTAVMenu injected** notification, open the menu.
Packaged loaders watch for later GTA relaunches; plugin **Auto-start** also starts the watcher
when the launcher starts.

To stop or update a plugin, disable **Auto-start** and **Running** in its Toolbox, then wait for
`/data/GTAVMenu/daemon.lock` to disappear. For standalone, create an empty
`/data/GTAVMenu/daemon.stop` file. Run only one loader at a time.
After replacing an etaHEN plugin, reboot and reload etaHEN before enabling it to clear cached plugin code.

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
  and effects on the 01.010.002 targets.
- **Interface:** English and Brazilian Portuguese, configurable appearance and keybinds.

## Build and package

Linux is the tested build host. Install **make**, **Python 3.11+**, **LLVM 20** (Clang, LLD and
llvm-config), and **PS5 payload SDK v0.43**. Select the SDK and toolchain explicitly:

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
export GTAV_TARGET=ppsa04264-01.010.002  # use ppsa04263-01.010.002 for EU
export PS5_HOST=<console-ip>
./menu-ctl.sh cave-inject
./menu-ctl.sh doctor
```

To start the loader **before** launching GTA, use `./menu-ctl.sh watch` instead. Add `--persist`
to inject after each relaunch; `./menu-ctl.sh daemon-stop` stops that watcher. Use Toolbox to stop
an OnionHEN or etaHEN plugin.

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
**Load N selected packs**. Up to 8 packs can be active, with 64 installed. Restart GTA to change
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
