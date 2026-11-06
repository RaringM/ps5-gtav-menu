# User guide

This guide is for players on a PS5: installing, updating and removing the menu, fixing common
problems, and loading custom packs in game. On a PS4, install the plugin as described in the
[PS4 plugin guide](ps4-plugin.md); its custom packs work as sections 5 to 11 describe. To make
packs, read the [pack author guide](pack-author-guide.md); the [pack reference](pack-reference.md)
covers the `pack.cfg` format, the host commands and every refusal message.

## 1. What you need

- A jailbroken PS5 with a payload launcher, or OnionHEN or etaHEN, and GTA V at one of the
  versions in the README's [supported games](../README.md#supported-games) table.
- For custom packs only:
  - ShadowMountPlus or a HEN setup that shares the console's `/data` folder with GTA, whether the
    game is installed from an exFAT image or a native `.pkg`.
  - A PC on the same network (Linux is the tested host) with Python 3.11 or newer, `curl` and a
    copy of this repository: the pack tools are in the repository, not in the release ZIPs.
  - The console's FTP server. Run `export PS5_HOST=<console-ip>` once in your shell; `PS5_FTP_PORT`
    defaults to 2121.

Custom packs are **experimental**. They work on 01.010.002 (PPSA04263 and PPSA04264) and
01.011.001 (PPSA04264); 01.005.000 runs the ordinary menu with custom packs off.

## 2. Install the menu

Download one PS5 package from the release (the README's [download](../README.md#download) table
lists them). To check the download on a PC, put `SHA256SUMS` next to the ZIP and run
`sha256sum --ignore-missing -c SHA256SUMS`. Every package supports all the PS5 builds in the table
and picks the right one at launch.

| Delivery | Steps |
| --- | --- |
| Standalone | Send `gtav-menu-daemon.elf` with your payload launcher **before** starting GTA. The worker for every build is embedded; nothing else is uploaded. |
| OnionHEN | Copy `GTAV00001.elf` to `/data/OnionHEN/plugins/`. In Toolbox, open **Payloads & Kernel > Plugins > GTAV Menu** and enable **Running** (and **Auto-start** to start it with the launcher). Targets OnionHEN v0.0.13. |
| etaHEN | Copy `GTAV00001.plugin` to `/data/etaHEN/plugins/` (only that file; its helper is embedded). Enable **Running** under **Toolbox > Plugins**. Targets the etaHEN 2.5B plugin interface. |

Start GTA and load Story Mode. After the **GTAVMenu injected** notification, press
**R1 + D-pad Left**. The loader keeps watching and injects again after every GTA
relaunch, so you install once per console boot (or never again, with **Auto-start**).

**Bigger tables on 01.011.001.** When the loader is running before GTA starts, it enlarges the
game's vehicle, ped and asset tables at launch, which leaves room for many more add-on vehicles
and peds (section 10). Started after GTA, it keeps the stock tables. If the loader cannot undo its
startup patch cleanly, it does not inject the menu into that GTA process; restart GTA. The
01.010.002 and 01.005.000 builds keep their stock tables.

**Tested setups.** Standalone and OnionHEN are tested on 01.010.002 (US and EU), and standalone on
01.011.001 (including relaunches and the bigger tables). Not yet tested on a console: automatic
selection of 01.005.000 and 01.011.001 by a package, and OnionHEN and etaHEN on 01.011.001. The
etaHEN package was tested on etaHEN Beta2.6A only, not on stock 2.5B.

To build the menu yourself or inject it from a PC, see [Building from source](building.md).

## 3. Update, stop or uninstall

- **Stop a plugin:** turn **Auto-start** and **Running** off in its Toolbox, then wait until
  `/data/GTAVMenu/daemon.lock` disappears. **Stop the standalone loader:** create an empty file
  `/data/GTAVMenu/daemon.stop`. Run only one loader at a time.
- **Update:** stop the loader, replace the file you installed with the new release's, and start it
  again. After replacing an etaHEN plugin, reboot and reload etaHEN before enabling it: Toolbox can
  keep the previous plugin's code in memory. The startup notification names the running release.
- **Uninstall:** stop the loader and delete the file you installed (`GTAV00001.elf` or
  `GTAV00001.plugin`); for standalone, stop sending the ELF. `/data/GTAVMenu/` holds the menu's
  settings, saved vehicles and scenes, installed packs and its log; delete it too to remove
  everything.

## 4. Troubleshooting

| Problem | What to do |
| --- | --- |
| No **GTAVMenu injected** notification | Make sure the loader runs (plugin **Running** on, or the standalone ELF sent) and that your game is a supported title and version: other builds are refused. Then restart GTA. |
| The notification appeared but the menu does not open | Press **R1 + D-pad Left** in Story Mode free roam; the menu starts hidden. |
| Injection failed or was partial | Close GTA and start a fresh Story Mode session. Do not inject twice into the same game process. |
| **Custom Packs** says custom assets are unavailable | Share `/data` with GTA (section 5), then restart GTA. The rest of the menu works without it. |
| Anything else | The loader writes `/data/GTAVMenu/gtav-menu.log`; include it when you report a problem. |

## 5. Custom packs: shared game storage

Packs live at `/data/GTAVMenu/custom/packs/<id>/` on the console. GTA must see that same `/data`
folder through ShadowMountPlus or the HEN setup; the loader does not mount or copy pack files into
the game.

If GTA cannot access shared `/data`, the regular menu still starts. **Custom Packs** reports that
custom assets are unavailable; loading packs and exporting templates are refused. Saved maps and
profiles also need writable game access to their `/data` paths. Enable the shared folder in your
HEN or mount setup and restart GTA before retrying. Installed packs stay installed.

## 6. Install a pack

A pack is a folder whose `resources/` subfolder holds a `pack.cfg` file, one to four `.rpf` archives
and sometimes `.meta` data files. To install one from the PC:

```sh
PS5_HOST=<console-ip> ./menu-ctl.sh pack-install path/to/my-pack            # install only
PS5_HOST=<console-ip> ./menu-ctl.sh pack-install path/to/my-pack --activate-add
```

`pack-install` checks the pack first and refuses a damaged or invalid one. It then uploads every
file, reads each one back to check it, adds the pack to the console's installed list and prints the
in-game step. `--activate` makes it the only selected pack. `--activate-add` adds it to the
selection, and refuses if it clashes with packs already selected.

Other host commands:

| Command | What it does |
| --- | --- |
| `./menu-ctl.sh pack-list` | Lists the installed packs in menu order. `*` marks selected packs, and the number is the menu row. |
| `./menu-ctl.sh pack-select N` | Selects or deselects installed pack N. |
| `./menu-ctl.sh pack-validate DIR --against OTHER` | Checks whether two packs can be selected together, without uploading anything. |
| `./menu-ctl.sh pack-uninstall ID [--purge]` | Removes a pack from the installed list and the selection. `--purge` also deletes its files. |
| `./menu-ctl.sh pack-autoload` | Presses Load from the PC. |
| `./menu-ctl.sh pack-notes` | Prints the detailed `GTAVMenu pack ...` lines behind a refusal. |

The full list is in the reference's [menu-ctl.sh pack commands](pack-reference.md#menu-ctlsh-pack-commands).

## 7. Select and load packs in game

1. Load Story Mode, wait for the menu, and stay in free roam.
2. Open **Custom Packs > Manage Packs**. Its first rows are the controls (**Load N selected
   packs**, **Uninstall**, and **Revert overrides** once overrides are loaded); below them
   **Installed packs** lists every installed pack, up to 128. Press Cross on a pack to select or
   deselect it. The line below the list describes the highlighted pack: its description, version
   and author, what it adds (for example `Adds 2 vehicles, 1 map, 4 labels`), and whether it clashes
   with the packs already selected.
3. Press **Load N selected packs** (the first row of Manage Packs, or of **Custom Packs**). All
   selected packs load together. A toast reports `Custom packs ready: ...` when they have loaded.
   Before a load, the line below the Load row shows the game's free slots for packs.
4. The loaded content then appears in folders on **Custom Packs**: **Vehicles**, **Weapons**,
   **Peds**, **Objects**, **Effects** (screen effects and particles) and **Locations** (teleports to
   the packs' maps). Each folder groups its rows by pack. The same rows appear in the **Custom**
   category of the Vehicle Browser, the Weapon Browser, **Spawn > Peds** and **Spawn > Objects**.

Things to know:

- **Loading cannot be undone.** Each pack loads once per GTA process and stays until GTA closes.
- **You can add packs later.** After a load, select more packs and press **Add N selected packs**
  (the same row): only the new packs load, and the loaded ones stay usable meanwhile. This works for
  vehicle, ped, weapon and effect packs. Packs with maps, archetypes (typ), collision, stock
  overrides or hides load only with the first Load of a game: once one of those is loaded, or to
  add one, the list row reads **Installed, next launch** and the selection applies to the next GTA
  launch.
- **Stock replacements** (a pack that replaces a game car, texture or piece of clothing) must load
  before the game loads the original. If one is already in use, the load says
  `<member> in use; restart GTA`. **Manage Packs > Revert overrides (N)** puts the stock content back
  once nothing is using it: the menu unloads the replaced files the game only keeps cached, so it
  finishes within seconds once the car is deleted (**Spawn > Spawned Entities > Clear Vehicles**),
  the weapon put away or the clothing changed. While something still uses them it waits and tries
  again every 2 seconds for up to 3 minutes.
- **Clothing for a story character or ped** must load before that ped is spawned.
- **Uninstall in game:** the last row of **Manage Packs**, **Uninstall: <pack>**. Pick the pack with
  Left/Right, then press Cross twice. The files stay on the console, and `pack-install` restores
  the pack.

### Saving a scene with pack props

**Spawn > Spawned Entities > Save Map** saves the vehicles, peds and objects you spawned with the
menu, and **Load Map** places them again in a later session. A prop from a loaded pack is saved by
its name together with its pack's id. Load Map then refuses the file with `Load pack <id> first`,
and places nothing, until **Custom Packs > Load** has loaded a pack that has that prop. So after a
restart: select and load the packs first, then press Load Map. A saved map holds at most 32
entities of each kind and props of up to 8 packs.

Props of a converted map mod's props pack are listed under **Spawn > Objects** (Category: Custom)
and **Custom Packs > Objects**, so you can place them yourself and save the result.

### Waiting for a tuned car

Packs that add mod kits or custom wheels change a game table that cars with LS Customs parts use.
While such a car is near you, the load waits instead of failing:

- At Load, the toast `Tuned car nearby (parked ones count): the load may wait` warns you.
- While it waits, the load row reads `Waiting: tuned car nearby; move away`. Any car with LS Customs
  parts counts: your own car, parked cars, and the character's personal car outside their house.
- Walk or drive away. When no tuned car is streamed in, the toast `Custom packs: tuned car gone,
  loading on` appears and the load continues by itself.
- After 3 minutes it stops with `Tuned car nearby (parked ones too)`. Restart GTA, then load
  somewhere without tuned cars (the Maze Bank roof works).

## 8. When a load is refused

A refused load shows on the first row of **Custom Packs** (`Load failed: <reason>`) and as a toast.
When one pack caused it, the text starts with that pack's id. The most common ones:

| Message | What to do |
| --- | --- |
| `Custom packs unavailable` / shared `/data` required | Enable shared game access to the console `/data` in your HEN or mount setup, then restart GTA. The regular menu remains usable. |
| `Load packs (none selected)` | Select a pack in **Manage Packs** first. |
| `Deselect <pack>: same <kind> <name>`, `Load blocked: packs clash` | Two packs use the same name. Select only one of them. |
| `Limit reached: too many <rows>; deselect a pack`, `16 packs selected; deselect one first` | The selection is over a limit. Deselect a pack. |
| `Load blocked: Vehicle slots full`, `Ped model slots full (<n> free, need <m>): deselect <packs>` | The game has room for a limited number of add-on vehicles and peds per process (section 10). Deselect the named packs, or restart GTA if packs already loaded. |
| `Load blocked: Ped metadata slots full`, `<Store> slots full (<n> free, need <m>): deselect <packs>` | The selected packs add more of that kind (archive files, texture or drawable slots, models, weapon components or weapon files) than the game has free. Deselect the named packs; **Manage Packs** marks each of them with `Load blocked`, starts each pack's line with what it needs (`Needs 12 frag, 31 txd, 1 vehicle.`) and shows the game's free slots below the **Load** row. |
| `Load failed: <Store> slots full`, `<Store> slots full (<n> free, need <m>); restart GTA` | The game used up slots between the check and the load, or a data file declared more models than expected. Restart GTA and load again; `./menu-ctl.sh pack-notes` shows each store's need and free slots (`pack store need ...`). |
| `<id>: pack.cfg invalid; reinstall it`, `Pack files damaged; reinstall it`, `Pack file missing; reinstall it` | Reinstall the pack with `pack-install`. |
| `Packs already loaded; restart GTA`, `Load failed; restart GTA to load again` | Packs load once per game process. Restart GTA. A Load refused before anything loaded (for example `Model name already used`, or slots full) needs no restart: deselect the named pack and press Load again. |
| `Selected packs are loaded`, `Loaded packs have maps or overrides; restart GTA to load more`, `<pack> has maps or overrides; restart GTA to load it` | **Add N selected packs** adds only new vehicle, ped, weapon and effect packs to a finished load. Select a pack that is not loaded yet, or restart GTA to load a pack with maps, typs, collision, overrides or hides. |
| `Load in Story mode, then retry`, `Game busy; retry in free roam` | Load in free roam in Story Mode. |
| `Too many custom vehicles loaded`, `Out of memory; load fewer packs` | The game's table or memory is full. Load fewer packs. |
| `Wrong game version (...)` | The game build is not one custom packs support (01.010.002 or 01.011.001). The menu refuses rather than guess. |
| `... see notes`, `... see pack-notes` | Run `./menu-ctl.sh pack-notes` on the PC for the details. |
| `Load pack <id> first` (from **Load Map**) | The saved map uses props of that pack. Load the pack from **Custom Packs** first. |

The pack reference's [Troubleshooting](pack-reference.md#troubleshooting) table lists every message.

## 9. What works

Each item says which menu rows to use.

- **Vehicles.** Converted PC add-on cars and motorbikes, including emergency vehicles with their own
  sirens and a wheelchair. Each has its make and model name, handling, mod kit and LS Customs parts
  with their real names, and its own engine-sound settings built from the game's sounds. Spawn them
  from the Vehicle Browser's **Custom** category or **Custom Packs > Vehicles**. **LS Customs > Part
  Type / Part** reaches every kit slot of the car, and **Vehicles > Spawn Options > Spawn Upgraded**
  fits the top part of every slot. Liveries are under **LS Customs > Plates & Livery > Livery**:
  Left/Right steps through the car's texture liveries and then its kit liveries (shown by name, or
  `Kit Livery N/M`).
- **Custom wheels.** Rims from a car mod, chosen in the menu's **Vehicle > LS Customs**. Set
  **Wheel Type** to the wheels' type, then step **Wheels**: the pack's wheels are the last designs
  of that type, shown with their position and name (for example `51/54 Chiron Classique`).
- **Replacing a stock car.** A PC "replace" mod becomes the body of a game car (for example every
  Adder, traffic included), with its textures. Its handling and sirens stay stock.
- **Props and maps.** New props with collision, map placements of up to several hundred objects
  each, whole PC map mods (villas, parks) split into several packs, new interiors (each with a
  **Locations** teleport inside), collision converted from PC mods, and stock world objects hidden
  around a spot.
- **Peds and clothing.** Add-on peds, including multi-part peds with hats and glasses, hair, and
  female peds, worn from **Self > Appearance > Skin Changer > Custom** and dressed with
  **Self > Appearance > Wardrobe** (Cross applies the highlighted style). New clothing for story
  characters (replacing one of their outfits) and new styles for the online characters' freemode
  peds, which Wardrobe lists after the game's own.
- **Weapons.** New weapons with their own models and attachments, each cloned from a game weapon
  that decides its class (pistols, SMGs, rifles, a knife and a katana have been tested), with a
  weapon wheel icon borrowed from a game weapon in the same slot. Pack weapons are in the **Weapon
  Browser** (Custom). Its footer names the weapon wheel slot, the borrowed icon, how many
  components the weapon takes and whether it can be tinted (`Tints: 8` or `Tints: none`).
  **Weapons > Attachments > Component** works on the weapon in hand, pack or stock: Left/Right
  picks one of the parts it takes (a default part reads `(default)`), and Cross fits or removes it.
  The older per-slot rows for stock weapons are under **Attachments > Quick Slots (retail)**.
  **Weapon Tint** recolours converted weapons like stock ones (a weapon without a tint palette
  shows `no tint palette`), and **Give Max Ammo** also fills pack weapons.
- **Textures, screen effects and particles.** Texture replacements, new screen-effect (timecycle)
  modifiers and copies of existing particle effects.
- **Saved scenes with pack props** (section 7).

## 10. Known limits

- **16 active packs, 128 installed.** At most 16 packs can be selected at once, and the menu lists
  up to 128 installed packs. Together, the active packs also share limits on archives, maps, collision
  files, labels and spawn rows. The pack reference's [pack.cfg rows](pack-reference.md#packcfg-rows) table lists
  them.
- **Add-on vehicles and peds per GTA process.** The game's model tables are nearly full: about 7
  vehicles and 4 peds fit on 01.010.002. On 01.011.001, with the loader started before GTA
  (section 2), the model tables have room for about 203 vehicles and 105 peds; the active-pack limit and
  store slots still apply. A 01.011.001 game the loader joined after it started
  keeps its stock tables: 3 vehicles and 5 peds, and ped metadata (needed by ped and clothing packs)
  has 1 free slot, so ped and clothing packs do not load. Weapon tables do not grow on any build:
  about 63 weapon models, 5 weapon components and 6 weapon files fit. Load refuses a selection that
  does not fit (`Load blocked: Vehicle slots full`) before anything loads.
- **No unload.** Restart GTA to change the loaded packs.
- **Custom wheels only in the menu's LS Customs.** The real LS Customs shop lists a fixed set of
  designs per wheel type and never shows pack wheels.
- **Pools made from props are not swimmable.** Many map mods build their pools from water-surface
  props. They look right, but the game treats them as solid props, not as water.
- **Very large models do not fit.** A model whose detailed level is larger than one game resource
  (64 MiB) is refused.
- **A vehicle mod's own engine recordings** are not converted. A converted car uses a game car's
  engine sound, optionally the engine of another game car.
- **Mods for GTA V Enhanced (PC):** their texture files convert, but their models do not yet. Use the
  mod's "Legacy" build, which most of them ship as well.
- **Freemode clothing mods** may use slots the converter does not cover yet; the
  [reviewed stock inputs](pack-author-guide.md#9-reviewed-stock-inputs) list what is covered.
- **Not supported:** streaming large city-scale content from disk instead of
  memory, more than 16 active or 128 installed packs, new vehicle sound recordings, brand-new particle
  effects, new weapon wheel artwork, whole replacement maps (a full city, level or water swap),
  game versions other than those in the README, and peds with non-standard skeletons (animals,
  extra bones).

## 11. Known issues

- **Kit liveries need cars converted with v0.2.0 or later tools.** A car converted earlier shows
  its kit liveries in the list, but they draw nothing on the car. Convert the car again.
- **Packs with a `tints` row need a current menu.** Weapons converted with the current tools carry
  that row, and an older menu refuses their `pack.cfg` (`<id>: pack.cfg invalid; reinstall it`).
  Install the current release.
