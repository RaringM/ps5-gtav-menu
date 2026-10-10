# User guide: custom packs

This guide is for players: how to install the menu, how to put custom packs on the console, and how
to load them in game. To make packs, read the [pack author guide](pack-author-guide.md). The
[pack reference](pack-reference.md) covers the full `pack.cfg` format, commands and refusal messages.

Custom packs are **experimental**. They work only on the 01.010.002 game builds (PPSA04263 and
PPSA04264). The supported 01.005.000 build runs the ordinary menu with custom packs disabled.

## 1. What you need

- A jailbroken PS5 with a payload launcher, and GTA V (PPSA04263 or PPSA04264) at content version
  01.010.002.
- ShadowMountPlus or a HEN setup that shares the console's `/data` folder with GTA. This is required
  for custom assets, whether the game is installed from an exFAT image or a native `.pkg`.
- A PC (Linux is the tested host) with Python 3.11 or newer, `curl` and this repository, on the same
  network as the console.
- The console's FTP server. Run `export PS5_HOST=<console-ip>` once in your shell: console
  commands require this address. `PS5_FTP_PORT` defaults to 2121.

## 2. Install the menu

For a release ZIP, follow [Install a package](../README.md#install-a-package), then load Story Mode
and open the menu with **R1 + D-pad Left**. Universal packages detect the supported title/version
automatically and do not require a host compiler. Custom packs remain unavailable on 01.005.000.

To deploy from a source checkout, follow the README's
[build prerequisites](../README.md#build-and-package) and [Run from source](../README.md#run-from-source):

1. Select your exact title ID and game version, then build the menu (`make all`).
2. Start GTA V and load Story Mode, then inject from the PC:

   ```sh
   export GTAV_TARGET=ppsa04264-01.010.002  # use ppsa04263-01.010.002 for EU
   export PS5_HOST=<console-ip>
   ./menu-ctl.sh cave-inject
   ./menu-ctl.sh doctor
   ```

   Or arm the loader before you start GTA: `./menu-ctl.sh watch` (one launch)
   or `watch --persist` (every launch, until `./menu-ctl.sh daemon-stop`). The OnionHEN and etaHEN
   deliveries inject the same menu.
3. In game, **R1 + D-pad Left** opens or hides the menu. D-pad Up/Down moves, Left/Right changes a
   value, Cross selects, Circle goes back.

Use a fresh GTA process after a failed or interrupted injection.

### Delivery compatibility and updates

Standalone and OnionHEN have passed US/EU 01.010.002 selection and shutdown checks. Standalone
also works without a HEN when a compatible payload launcher is available. Automatic selection of
PPSA04264 01.005.000 is implemented but has not been tested on hardware.

The etaHEN package targets the 2.5B plugin interface. Console checks used Beta2.6A with a custom
lifecycle implementation; compatibility with stock 2.5B has not been verified on hardware.
Only install `GTAV00001.plugin`; its helper is embedded.

Before replacing either plugin, turn **Auto-start** and **Running** off in Toolbox and wait for
`/data/GTAVMenu/daemon.lock` to disappear. After replacing an etaHEN plugin, reboot and reload
etaHEN before enabling it: Toolbox can retain the previous plugin's code in memory. The startup
notification identifies the running menu by its release tag or source commit.

## 3. Shared game storage

Packs live at `/data/gtavmenu/custom/packs/<id>/` on the console. GTA must see that same `/data`
folder through ShadowMountPlus or the HEN setup. The loader does not mount or copy pack files into
the game. This direct route has passed asset testing on both 01.010.002 titles.

If GTA cannot access shared `/data`, the regular menu still starts. **Custom Packs** reports that
custom assets are unavailable; loading packs and exporting templates are refused. Saved maps and
profiles also require writable game access to their `/data` paths. Enable the shared folder in
your HEN/mount setup and restart GTA before retrying custom assets. Existing console packs remain
installed.

Missing-`/data` refusal and normal menu/gameplay have been verified on PPSA04264 01.010.002
without a HEN or ShadowMountPlus.

## 4. Install a pack

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

## 5. Select and load packs in game

1. Load Story Mode, inject the menu, and stay in free roam.
2. Open **Custom Packs > Manage Packs**. **Installed packs** lists up to 64 packs, 12 per page.
   Press Left/Right on that row to turn the page. Press Cross on a pack to select or deselect it.
   The line below the list describes the highlighted pack: its description, version and author,
   what it adds (for example `Adds 2 vehicles, 1 map, 4 labels`), and whether it clashes with the
   packs already selected.
3. Go back and press the first row of **Custom Packs**, **Load N selected packs**. All selected
   packs load together. A toast reports `Custom packs ready: ...` when they have loaded.
4. The loaded content then appears in folders on **Custom Packs**: **Vehicles**, **Weapons**,
   **Peds**, **Objects**, **Effects** (screen effects and particles) and **Locations** (teleports to
   the packs' maps). Each folder groups its rows by pack. The same rows appear in the **Custom**
   category of the Vehicle Browser, the Weapon Browser, **Spawn > Peds** and **Spawn > Objects**.

Things to know:

- **Loading cannot be undone.** Each pack loads once per GTA process. After a load the list row
  reads **Installed, next launch**: a selection you change then applies to the next GTA launch.
  To change the loaded set, change the selection, restart GTA and load again.
- **Stock replacements** (a pack that replaces a game car, texture or piece of clothing) must load
  before the game loads the original. If one is already in use, the load says
  `<member> in use; restart GTA`. **Manage Packs > Revert overrides (N)** puts the stock content back
  once nothing is using it. If it is in use, the menu asks you to move away for about 2 minutes and
  try again.
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

## 6. When a load is refused

A refused load shows on the first row of **Custom Packs** (`Load failed: <reason>`) and as a toast.
When one pack caused it, the text starts with that pack's id. The most common ones:

| Message | What to do |
| --- | --- |
| `Custom packs unavailable` / shared `/data` required | Enable shared game access to the console `/data` in your HEN or mount setup, then restart GTA. The regular menu remains usable. |
| `Load packs (none selected)` | Select a pack in **Manage Packs** first. |
| `Deselect <pack>: same <kind> <name>`, `Load blocked: packs clash` | Two packs use the same name. Select only one of them. |
| `Limit reached: too many <rows>; deselect a pack`, `8 packs selected; deselect one first` | The selection is over a limit. Deselect a pack. |
| `<pack>: ped model slots full (430); deselect a ped pack` | The game has room for about 4 add-on peds per process. Deselect a ped pack, or restart GTA if peds already loaded. |
| `<id>: pack.cfg invalid; reinstall it`, `Pack files damaged; reinstall it`, `Pack file missing; reinstall it` | Reinstall the pack with `pack-install`. |
| `Packs already loaded; restart GTA`, `Load failed; restart GTA to load again` | Packs load once per game process. Restart GTA. |
| `Load in Story mode, then retry`, `Game busy; retry in free roam` | Load in free roam in Story Mode. |
| `Too many custom vehicles loaded`, `Out of memory; load fewer packs` | The game's table or memory is full. Load fewer packs. |
| `Wrong game version (...)` | The game build is not 01.010.002. The menu refuses rather than guess. |
| `... see notes`, `... see pack-notes` | Run `./menu-ctl.sh pack-notes` on the PC for the details. |
| `Load pack <id> first` (from **Load Map**) | The saved map uses props of that pack. Load the pack from **Custom Packs** first. |

The pack reference's [Troubleshooting](pack-reference.md#troubleshooting) table lists every message.

## 7. What works

Everything below has passed on a console. Each item says which menu rows to use.

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
  Adder), with its textures. Its handling and sirens stay stock.
- **Props and maps.** New props with collision, map placements of up to several hundred objects
  each, whole PC map mods (villas, parks) split into several packs, new interiors, collision
  converted from PC mods, stock world objects hidden around a spot, and **Locations** teleports.
- **Peds and clothing.** Add-on peds, including multi-part peds with hats and glasses, hair, and
  female peds, worn from **Self > Appearance > Skin Changer > Custom** and dressed with
  **Self > Appearance > Wardrobe** (Cross applies the highlighted style). New clothing for story
  characters (replacing one of their outfits) and new styles for the online characters' freemode
  peds, which Wardrobe lists after the game's own.
- **Weapons.** New weapons with their own models and attachments, each cloned from a game weapon
  that decides its class (pistols, SMGs, rifles and a knife and a katana have been tested), with a
  weapon wheel icon borrowed from a game weapon in the same slot. Pack weapons are in the **Weapon
  Browser** (Custom). Its footer names the weapon wheel slot, the borrowed icon and how many
  components the weapon takes.
  **Weapons > Attachments > Component** works on the weapon in hand, pack or stock: Left/Right
  picks one of the parts it takes (a default part reads `(default)`), and Cross fits or removes it.
  The older per-slot rows for stock weapons are under **Attachments > Quick Slots (retail)**.
  **Weapon Tint** recolours converted weapons like stock ones, and **Give Max Ammo** also fills
  pack weapons.
- **Textures, screen effects and particles.** Texture replacements, new screen-effect (timecycle)
  modifiers and copies of existing particle effects.

The following have also passed console checks:

- **Save Map / Load Map with pack props** (section 5).
- **Tint status of pack weapons.** The Weapon Browser footer also says whether a pack weapon can be
  tinted (`Tints: 8` or `Tints: none`), and **Weapon Tint** on a weapon without a tint palette shows
  the toast `no tint palette` instead of doing nothing.
- **Clothing packs made with the new clothing converter** (the same kinds of clothing as above).
- **A Locations teleport inside every converted interior**, also when the pack author named none.

Freemode clothing acceptance used full-body suit inputs. A typical downloaded freemode clothing
mod remains an input-coverage gap; supported stock slots are listed in
[stock conversion coverage](stock-conversion.md).

## 8. Known limits

- **8 active packs, 64 installed.** At most 8 packs can be selected at once, and the menu lists up
  to 64 installed packs. Together, the active packs also share limits on archives, maps, collision
  files, labels and spawn rows. The pack reference's [pack.cfg rows](pack-reference.md#packcfg-rows) table lists
  them.
- **About 4 add-on peds per GTA process.** The game's table of ped models is nearly full. Load
  refuses a selection with more add-on peds than fit (`ped model slots full`) before anything
  loads. Clothing packs do not count.
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
- **Not supported:** streaming large city-scale content from disk instead of
  memory, more than 8 active or 64 installed packs, new vehicle sound recordings, brand-new particle
  effects, new weapon wheel artwork, whole replacement maps (a full city, level or water swap),
  other game versions, and peds with non-standard skeletons (animals, extra bones).

## 9. Known issues

- **If injection fails or reports a partial injection,** close GTA and start a fresh Story Mode
  session before retrying. Do not inject again into the same game process. Selected packs stay selected.
- **Kit liveries need packs built from 2026-10-08 on.** A car converted earlier shows its kit
  liveries in the list, but they draw nothing on the car. Convert the car again.
- **Packs with a `tints` row need the current menu build.** Weapons converted with the current
  tools carry that row, and an older menu refuses their `pack.cfg` (`<id>: pack.cfg invalid;
  reinstall it`). Inject the current menu build.
