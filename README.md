# GTAV Menu

A single-player mod menu for GTA V on a jailbroken PS5, plus a plugin for GTA V Legacy on a
jailbroken PS4. Spawn and customize vehicles, change your character and weapons, teleport, build
scenes, and load custom content converted from PC mods. The menu starts hidden; press
**R1 + D-pad Left** to open it. It is for **Story Mode** only.

## Supported games

| Console | Title ID | Game version | Menu | Custom packs |
| --- | --- | --- | --- | --- |
| PS5 | PPSA04264 (US) | 01.011.001 | Supported | Experimental |
| PS5 | PPSA04264 (US) | 01.010.002 | Supported | Experimental |
| PS5 | PPSA04263 (EU) | 01.010.002 | Supported | Experimental |
| PS5 | PPSA04264 (US) | 01.005.000 | Supported | Unavailable |
| PS4 | CUSA00411 (Legacy) | 01.57 | Experimental | Experimental |

Other title IDs and versions are refused before the menu touches the game. You need your own copy
of the game and a jailbroken console with a payload launcher (PS5) or GoldHEN / nexHEN PS4 (PS4).

## Download

Each release has these files. Pick **one** PS5 delivery:

| File | For |
| --- | --- |
| `GTAVMenu-universal-standalone.zip` | PS5, sent with any payload launcher (no HEN required) |
| `GTAVMenu-universal-onionhen.zip` | PS5 with OnionHEN v0.0.13 |
| `GTAVMenu-universal-etahen.zip` | PS5 with etaHEN (2.5B plugin interface) |
| `GTAVMenu-PS4-CUSA00411-v01.57-plugin.zip` | PS4 with GoldHEN 2.3+ or nexHEN PS4 v1.00 |
| `SHA256SUMS` | Checksums of the files above |

Every PS5 package supports all four PS5 builds above and picks the right one automatically.

## Install

| Delivery | Steps |
| --- | --- |
| Standalone | Send `gtav-menu-daemon.elf` with your payload launcher **before** starting GTA. |
| OnionHEN | Copy `GTAV00001.elf` to `/data/OnionHEN/plugins/`. In Toolbox, open **Payloads & Kernel > Plugins > GTAV Menu** and enable **Running**. |
| etaHEN | Copy `GTAV00001.plugin` to `/data/etaHEN/plugins/`. Enable **Running** under **Toolbox > Plugins**. |
| PS4 plugin | Follow the [PS4 plugin guide](docs/ps4-plugin.md). |

Then start GTA and load Story Mode. After the **GTAVMenu injected** notification, press
**R1 + D-pad Left**. Updating, stopping, uninstalling and troubleshooting are in the
[user guide](docs/user-guide.md).

## Controls

| Control | Action |
| --- | --- |
| R1 + D-pad Left | Open / hide |
| D-pad Up / Down | Navigate |
| D-pad Left / Right | Change a value |
| Cross | Select / apply |
| Circle | Back |

## Features

- **Vehicles:** class filters, previews, saved vehicles, spawning and LS Customs parts.
- **Player and world:** skins, wardrobe, weapons and attachments, teleportation, weather and time.
- **Scenes:** spawn and move objects, peds and vehicles; save and load your placements.
- **Custom packs:** vehicles, wheels, weapons, peds, clothing, props, maps, interiors, textures and
  effects converted from PC mods, including replacements of stock cars (experimental).
- **Interface:** English and Brazilian Portuguese, configurable appearance and keybinds.

## Guides

| Guide | Contents |
| --- | --- |
| [User guide](docs/user-guide.md) | Install, update and uninstall; custom packs in game; limits and troubleshooting |
| [PS4 plugin guide](docs/ps4-plugin.md) | Installing and using the PS4 Legacy plugin |
| [Pack author guide](docs/pack-author-guide.md) | Converting PC mods into packs, validating and sharing them |
| [Pack reference](docs/pack-reference.md) | `pack.cfg` format, host commands, limits and every refusal message |
| [Building from source](docs/building.md) | Toolchains, builds and packages, injecting from a PC |
| [Tools](docs/tools.md) | What the scripts in `tools/` do |

No game files or third-party mod assets are included.

## License

See [LICENSE](LICENSE) and [third-party notices](THIRD_PARTY_NOTICES).
