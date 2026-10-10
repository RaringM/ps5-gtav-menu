# Pack author guide

This guide is for people who make custom packs: converting PC mods with the `menu-ctl.sh convert-*`
commands, checking the result, and testing it on a console. To install and load packs as a player,
read the [user guide](user-guide.md).

The [pack reference](pack-reference.md)
has the `pack.cfg` grammar ([pack.cfg rows](pack-reference.md#packcfg-rows)), the content tools
([Content tools](pack-reference.md#content-tools)), every convert option
([Converting PC mods](pack-reference.md#converting-pc-mods)) and every refusal text
([Troubleshooting](pack-reference.md#troubleshooting)). This guide does not repeat them; it shows the
route and the rules.

## 1. The route

```text
mod as downloaded --convert-*--> build/custom-assets/<id>/ --pack-validate--> --pack-install--> console
      --Manage Packs: select--> Load --> use it in game --pack-notes / pack-check--> pass or fix
```

1. **Convert** the mod with one `convert-*` command (section 4). The pack is written to
   `build/custom-assets/<id>/` and checked.
2. **Validate** it alone and with the packs it will be loaded with (section 6).
3. **Install** it on the console, optionally selecting it (section 6).
4. **Test** it in a fresh GTA process and check the menu's pack lines (section 7).

## 2. Set up the host

- Linux is the tested host. You need Python 3.11 or newer, `curl`, and this repository.
- Run `export PS5_HOST=<console-ip>` before console commands (and set `PS5_FTP_PORT` if your FTP
  server does not use 2121). Offline conversion and validation work without a console address.
- Model, vehicle, stock replacement and clothing converters are public tools and need `numpy`
  (`python3 -m pip install numpy` for the Python that `menu-ctl.sh` runs: the checkout's executable
  `.venv`, otherwise `python3`). A command reports missing inputs before conversion.
  `convert-map`, `convert-bounds` and `convert-mlo` without `--models` need no extra package.
- Stock conversion uses a reviewed catalog for PPSA04264 01.010.002: Combat Pistol, Police3,
  Franklin Torso 14 / Legs 6, and new MP Male Legs styles. Other names and slots refuse;
  see [stock input coverage](stock-conversion.md). Pass `--game DIR` for your read-only game copy,
  or `--stock-cache DIR` for an already verified cache. `--target TARGET` selects the catalog
  (currently `ppsa04264-01.010.002`); `--stock-manifest FILE` accepts a relocated copy of that
  exact reviewed metadata. `GTAV_HOST_BUILD_DIR` relocates work, outputs and default caches.
- **Mods for GTA V Enhanced (PC).** Every converter reads both texture dictionary builds: the
  "Legacy" PC `.ytd` and the "Enhanced" one. Enhanced models (`.ydr`, `.ydd`, `.yft`) are not read
  yet; most Enhanced mods also ship a Legacy build, so point the command at that one.

## 3. Templates come from your own game

The converters build PS5 resources on top of templates: game files of the same kind, taken from your
own copy of the game. No game file ships with these tools.

```sh
PS5_HOST=<console-ip> ./menu-ctl.sh fetch-templates          # GTA V running: reads the game over FTP
./menu-ctl.sh fetch-templates --source file:///path/to/app0  # or a local copy of the game files
```

`fetch-templates` reads each pinned template from the game image, checks its sha256 and stores it
in `build/retail-templates/`. The map commands also use an archetype index of your game (which objects
are stock, their typ and size): `./menu-ctl.sh index-archetypes` (same `--source` choices) reads the
game's typ files the same way and writes `build/archetype-index/<target>.json`. The game image is mounted only while
GTA V runs. The lists are pinned for PPSA04264 01.010.002 (the default `GTAV_TARGET`). Selecting
`GTAV_TARGET=ppsa04263-01.010.002` reads the EU game's source and checks against those same reviewed
pins; archive sizes and every resource hash must match. The index keeps its selected EU output name
and records the US pin provenance. Other versions have no fallback.

Verified template cache entries and a completed index are reused without rereading the source. A
cached run does not verify a different game image. To check another source, use a new empty
`fetch-templates --cache DIR` and run `index-archetypes --force` (or choose a new `--output FILE`).

Some templates are encrypted game files that the host cannot read: the vehicle layouts
(`vehiclelayouts.meta`) of `update.rpf`, which a vehicle mod that ships its own layouts needs,
the game's audio data, against which `convert-vehicle --audio` checks a sound name, and the four
weapon XML metas (weapons, animations, archetypes and components), plus the timecycle XML used by
`author-timecycle`. With the menu
injected, have the game export them:

```sh
PS5_HOST=<console-ip> ./menu-ctl.sh export-templates
```

The menu writes them to `/data/gtavmenu/custom/exports/`. GTA must have access to the console's
shared `/data` through ShadowMountPlus or the HEN setup; otherwise the export is refused.
The command confirms that the live worker uses that path, waits for each new export success, and
verifies the downloaded bytes even if the host cache is already populated. Every convert command takes
`--templates DIR` to use another cache.

Some converters also take inputs that you extract from your game yourself: a one-entity `.pmap`
(`convert-map --template`) and a donor ped's
`InitData` (`convert-ped --init-data`; `tools/make_ped_initdata.py` writes one). `convert-replace`,
`convert-override` and `convert-clothing` read the stock files they replace or extend from a local
copy of your game (`--game DIR`, read only).

## 4. The convert commands

All of them work the same way:

- The mod is read as untrusted data. It is copied into a fresh `build/convert-<kind>/<id>/input`
  (symbolic links are refused) and only that copy is converted.
- The pack goes to `build/custom-assets/<id>/`, is checked with `tools/validate_runtime_pack.py`,
  and a summary lists its members, sizes and `pack.cfg` rows.
- `--install` uploads it like `pack-install`; `--activate` also selects it. Without either, the
  command prints the `pack-install` line to run.
- Rerunning with the same `--id` rebuilds the pack. A pack directory that another command made is
  left alone.
- Run a command without arguments for its full usage. The pack reference's
  [Converting PC mods](pack-reference.md#converting-pc-mods) section describes every option.

### convert-map: a map that places stock objects

```sh
./menu-ctl.sh convert-map ~/Downloads/garage/dlc.rpf --id garage-v1 \
  --template work/template.pmap --teleport -205,-1310,31,"Garage"
```

Reads a `dlc.rpf`, `.ymap`, CodeWalker `.ymap.xml` or mod folder. `--template` is a one-entity
`.pmap` from your game. With an archetype index (`--archetype-index`) only stock objects are kept;
without one every entity is kept unchecked. A mod that places its own models is refused: use
`convert-mapmod`.

### convert-mapmod: a whole map mod

```sh
./menu-ctl.sh convert-mapmod ~/Downloads/villa --id villa-v1 --dry-run
./menu-ctl.sh convert-mapmod ~/Downloads/villa --id villa-v1 --name "Villa" \
  --map-teleport popolo=-2225,-95,1,"Villa: below the house" --drawn-collision embedded
```

Looks at everything the mod ships (maps, own models, collision, interiors) and splits it into packs
that respect the limits: a map pack `<id>`, an interior pack (`-int`), a collision pack (`-col`) and,
with `--archetypes-from MOD`, a props pack (`-props`) for another mod's props that the maps place
(Map Builder). `--dry-run` prints the split and the cap use and writes nothing. Load the resulting
packs together. Own models, the props pack and `--drawn-collision` need numpy; without it the split leaves
them out and says so.

The props pack also gets an **Objects** spawn row per prop (named after the model, for example
`Black Tiles`), so the props can be placed by hand and kept with **Save Map**, which stores them by
name with the pack id. Every converted interior gets a **Locations** teleport just inside it unless
`--map-teleport` names one.

### convert-model: a mod's own models

```sh
./menu-ctl.sh convert-model ~/Downloads/benches/dlc.rpf --id benches-v1 --archetype my_bench_01
```

Props become **Objects** spawn rows. `--maps` also converts the mod's placements onto them.
With `--maps`, `--map-prefix gm_benches` names the resulting maps `gm_benches_0.pmap`,
`gm_benches_1.pmap`, and so on. Use a different prefix for each pack loaded together; by default it
comes from the pack id. Converted models get no collision of their own (use `convert-bounds`, or `convert-mapmod
--drawn-collision`). Needs numpy.

### convert-bounds: static collision

```sh
./menu-ctl.sh convert-bounds ~/Downloads/villa/dlc.rpf --id villa-col-v1 --only villa_col \
  --teleport -1200,500,80,"Villa terrace"
```

One `bounds` row per `.ybn` (at most 8; `--merge` makes one row), at the mod's own coordinates unless
`--translate DX,DY,DZ` moves them. Public converter; the PS5 class tag of each bound type comes from
the bounds templates `fetch-templates` caches (`bounds-templates/`: a small static `.pbn` and a prop
drawable), so it needs no developer files.

### convert-mlo: an interior

```sh
./menu-ctl.sh convert-mlo ~/Downloads/villa/dlc.rpf --id villa-int-v1 --mlo villa_shell=gmmlo_villa \
  --models --bounds --map-teleport gmmlo_villa=-1205,505,81,"Villa hall"
```

The MLO archetype and its placement, with `--models` (its own models) and `--bounds` (its room
collision). It needs an archetype index of your game (`--archetype-index`, default
`build/archetype-index/<target>.json`, which `./menu-ctl.sh index-archetypes` writes). The interior
step and `--bounds` need no package; `--models` needs numpy. With `--models`, repeat
`--rename-txd OLD=NEW` to give texture dictionaries unique names, for example
`--rename-txd villa_textures=gm_villa_textures`. This updates the models' archetype references as
well, avoiding dictionary-name clashes with other selected packs.

### convert-vehicle: an add-on car, bike or emergency vehicle

```sh
./menu-ctl.sh convert-vehicle ~/Downloads/protopolice/dlc.rpf --id proto-v1 --model protopolice \
  --name "Prototipo Police" --make-label GMPROTO_MAKE=Grotti --mod-kit --siren-preset classic-wigwag
```

The model, textures, LS Customs parts (`--mod-kit`) and the handling, carcols, layouts, vehicles and
variations data files. Exporter quirks are repaired by `--repair auto` (the default). When a repair
needs bone names, the run stops with one line naming them, for example
`add --repair seat-mirror=seat_dside_f,seat_pside_f --repair auto and rerun`. A mod that ships its
own wheel models is refused: add `--drop-wheels` and convert the wheels with `convert-wheels`. A run
takes a few minutes. The public converter uses reviewed format contracts and freshly checks the
material, shader and dictionary data in your verified retail templates. It needs numpy for texture
conversion. `GTAV_HOST_BUILD_DIR` selects a separate host build directory for work files, template
cache and generated packs (relative values are resolved against the checkout).

Liveries: texture liveries and the mod kit's livery parts (`VMT_LIVERY_MOD`) both convert, and the
menu lists them under **LS Customs > Plates & Livery > Livery** (texture liveries first, then the kit
liveries by name). Each kit livery part carries its own livery texture only in packs converted from
2026-10-08 on; convert an older pack again if its kit liveries draw nothing. The car's engine sound
is a stock sound (the run prints one `audio:` line with the choice; `--audio NAME` picks one, and
`--audio-engine NAME` gives the car its own sound with the engine of base-game car `NAME`); the mod's
own sound recordings are not converted.

### convert-replace: a car shipped under a stock name

```sh
./menu-ctl.sh convert-replace ~/Downloads/PoliceReplace --id replace-police3-v1 \
  --name "Police3 (replace)" --game /path/to/app0 --repair texture-stride --repair fragment-quirks
```

An override pack: the mod's body and textures replace the stock car's members, and every car of
that name has the new body after Load. Only one pack per replaced car can be active. The handling,
carcols and sirens stay stock. Only [reviewed stock names](stock-conversion.md) are enabled.

### convert-wheels: the rims a car mod ships

```sh
./menu-ctl.sh convert-wheels ~/Downloads/carpack.oiv --list
./menu-ctl.sh convert-wheels ~/Downloads/carpack.oiv --id carpack-wheels-v1 \
  --only wheel_spt_forgelineGA1R --label wheel_spt_forgelineGA1R="Forgeline GA1R"
```

One `<wheel name>.pdr` per wheel with its textures inside, and a carcols row with a shop label per
wheel. `--list` prints the mod's wheels and writes nothing. The wheels appear only in the menu's
**Vehicles > LS Customs > Cosmetics**. `fetch-templates` must have fetched `wheels/wheel_loride_01.pdr`.
Needs numpy.

### convert-ped: an add-on ped

```sh
./menu-ctl.sh convert-ped ~/Downloads/hero/hero.ydd --id hero-v1 --ped gm_hero_01 \
  --init-data work/donor_peds.meta --label "Hero (PC ped)"
```

Every component drawable of the `.ydd` with its textures, and the props of `<stem>_p.ydd` (hats,
glasses), on the standard ped skeleton. Props are discovered beside the ped or in its sibling
`props/` directory, matching the ped's exact stem without regard to case. Links, competing copies,
or a dictionary/texture pair split between the two directories are refused; no recursive search
or unrelated prop files are used. The new ped copies a donor ped's behaviour from
`--init-data`. The summary ends with a variation round trip (`--strict-variations` refuses a gap).
Hair/cutout shaders (`ped_hair_cutout_alpha`, `ped_hair_spiked`) are supported; the pack includes
`long_hair_noise` from your fetched game templates. Female ambient rigs use the fetched
`a_f_y_topless_01` carrier; choose a female behaviour donor with `--like A_F_Y_Topless_01`.
Use the mod's PC Legacy model files; Enhanced model conversion is not supported. Needs numpy.

`--init-data` must be actual `CPedModelInfo__InitDataList` XML from your game or your ped mod;
`--like` selects one donor when it contains several. The game's `update:/ps5/data/peds.pmt` is
binary PSO, and automatic PSO-to-XML donor conversion remains unavailable. An exported PSO file
alone is not a usable donor. Per-ped `.pmt`/`.ymt` variation resources cannot replace InitData.

### convert-weapon: a weapon model as a new weapon

```sh
./menu-ctl.sh convert-weapon ~/Downloads/glock/w_pi_combatpistol.ydr --id glock-v1 \
  --weapon WEAPON_GMGLOCK --like WEAPON_COMBATPISTOL --weapons-meta work/weapons.meta \
  --animations-meta work/weaponanimations.meta --archetypes-meta work/weaponarchetypes.meta \
  --text "Glock 17"
```

`--like` names the donor weapon, which decides the class, stats, ammo, animations, attachment points,
wheel icon and slot order. Donors that cannot work (unarmed, gadgets, vehicle weapons) are refused
with the reason. The metas are the decrypted files of your own game. `export-templates` fills
`retail-metas/{weapons,weaponanimations,weaponarchetypes,weaponcomponents}.meta` in the verified
cache; the explicit meta options above remain supported for other donors. The standalone
`tools/make_weapon_model_pack.py --templates DIR` also verifies omitted meta inputs against the
reviewed manifest before using them. These base templates preserve the common.rpf donor files;
a DLC weapon whose donor is absent needs explicit XML for that donor. Needs numpy.

Tints: the opaque body materials of the converted model take the game's tint-palette shader, so
**Weapon Tint** recolours the weapon like a stock one. The pack gets a `tints` row per weapon:
`palette`, or `none` when no material could take the shader (or with `--no-tints`, which keeps the
mod's own shaders); with `none` the menu's Weapon Tint shows `no tint palette`. A pack with a `tints`
row needs the current menu build; an older one refuses its `pack.cfg`. `--attachments`
also converts the mod's magazine and attachment models as pack components, which **Weapons >
Attachments > Component** fits.

### convert-clothing: clothing for a story character or a freemode ped

```sh
./menu-ctl.sh convert-clothing ~/Downloads/joggers --ped player_one --slot legs --drawable 6 \
  --id franklin-joggers-v1 --game /path/to/game
./menu-ctl.sh convert-clothing ~/Downloads/suits --ped mp_m_freemode_01 --slot legs \
  --id fm-suits-v1 --game /path/to/game
```

The mod is a folder, `dlc.rpf` or `.oiv` with `<comp>_<nnn>_<u|r>.ydd` drawables and their
`<comp>_diff_<nnn>_<x>_<race>.ytd` textures. `--slot` takes the Wardrobe names (face, mask, hair,
torso, legs, hands, shoes, neck, undershirt, armor, decals, tops). This release enables only the
reviewed Franklin and MP Male slots [listed above](stock-conversion.md). Stock clothing, skeletons
and variation files come from your `--game` copy or verified `--stock-cache`.

- **Story characters** (`player_zero`, `player_one`, `player_two`: Michael, Franklin, Trevor): the
  mod's drawable **replaces** the character's stock style `--drawable N` (an override pack, so it
  must load before the character wears it). The converter picks the stock drawable whose skeleton
  fits the mod and refuses with one `skeleton mismatch: ...` line when none does.
  `--texture-letter` maps the mod's texture variants onto the stock ones (`c=a,b`).
- **Freemode** (`mp_m_freemode_01`, `mp_f_freemode_01`): the mod's drawables (every one of the slot,
  or the ones `--drawable` names) are **added** as new styles after the game's own, which Wardrobe
  lists. The mod's own `.ymt` data, props (hats, glasses), alternate open/closed versions and cloth
  physics are not converted; one slot per pack.

`--dry-run` prints what would be built without writing a pack.
Freemode console checks used full-body suit inputs; typical downloaded freemode clothing remains
an input-coverage gap. Check the mod's rig and the [reviewed slots](stock-conversion.md) before converting.

### Stock replacements other than cars

`convert-override` turns a supported PC "replace" mod into an override pack. It looks up each
file in the [reviewed stock catalog](stock-conversion.md), refuses unreviewed names, converts each file and writes one `override` row per member:

```sh
./menu-ctl.sh convert-override "$HOME/Downloads/Glock17Gen5/Combat Pistol replace" --id ovr-glock-v1 --game /path/to/game
./menu-ctl.sh convert-override ~/Downloads/franklin-hoodie --id ovr-hoodie-v1 --only 'uppr_*' --folder player_one --game /path/to/game
```

`--member STOCK=FILE` adds a loose file under a stock name. A model still in use cannot be reverted at once:
Manage Packs -> "Revert overrides" waits until it is unloaded (up to 3 minutes; move away).
The rows themselves are described in the [pack.cfg rows](pack-reference.md#packcfg-rows).

### Author labels, timecycle modifiers and particle clones

`tools/author_runtime_pack.py` builds these small pack types using the normal builder and validator.
It stages the result, validates it, then creates a new pack under `build/custom-assets/<id>`;
existing output directories are refused. `--output-root DIR` chooses another destination,
`--against PACK` checks coexistence, and `--description`, `--author`, `--version` add Manage Packs text.

For labels, supply an ASCII text file with one `KEY=TEXT` per line. Blank lines and `#` comments
are ignored. Each key and text is 1–63 characters; keys use `[A-Za-z0-9_]`, and text cannot contain
tabs or `~`. Up to 128 labels fit a pack. Duplicate name hashes, including differently spelled
keys with the same hash, are refused. These labels supply text when another game/pack feature
looks up their keys; they do not create menu actions themselves.

```sh
python3 tools/author_runtime_pack.py labels work/labels.txt --id my-labels-v1
python3 tools/author_runtime_pack.py timecycle --like BlackOut --name gm_my_dark \
  --id my-dark-v1 --text "My dark screen effect"
python3 tools/author_runtime_pack.py ptfx --name gm_my_sparks --effect scr_indep_firework_fountain \
  --id my-sparks-v1 --text "My spark fountain"
```

The timecycle command clones only the selected modifier and changes its name; the menu row toggles
it on/off. Its default input is verified `retail-metas/timecycle_mods_4.xml` from `export-templates`.
Pass an XML file immediately after `timecycle` for an explicit source. It must have the retail
`timecycle_modifier_data` root/version and a nonempty modifier whose `numMods` matches its scalar
variable count. DTDs/entities, duplicate modifier hashes, malformed values and a new name already
present in that source are refused. The live menu also checks for names already loaded in the game.
`--meta NAME.meta` sets the data filename; `--archive NAME.rpf` sets the archive filename.

Labels and timecycle packs need an archive even though their content is in descriptor/data rows.
The default filler is your verified `corpus/tornado6.ptd` template under a new member name, with
no card row or streaming request. `--carrier FILE.ptd --carrier-name NAME.ptd` supplies another
existing PS5 texture dictionary. No retail or demonstration image is bundled with this tool.

The particle command copies an existing PS5 `.ppt` dictionary under a new name. Its default
source is verified `ptfx/scr_indep_fireworks.ppt` from `fetch-templates`; pass a `.ppt` file
immediately after `ptfx` for another source. `--effect` names an existing effect, and the menu row
plays it at the player's feet. The name is checked syntactically and recorded as caller-declared:
the tool does not prove that the dictionary contains that effect or that it will render. It does
not convert PC `.ypt` files. Dictionary and effect names together, including `:`, must fit 63
characters. `--templates DIR` selects the verified cache for all three commands.

## 5. Ids and names

A pack is loaded next to up to seven others, and everything in it lives in the game's own tables.
`pack-validate --against` and the menu's clash check catch a repeat, but choose names that cannot
clash in the first place:

- **Pack id:** lowercase letters, digits and `-`, at most 64 characters. End it with a version
  (`villa-v1`, `villa-v2`): a changed pack under a new id can sit next to the old one on the console.
- **Archive names:** the convert commands name archives after the id (for example
  `gmveh_<id>.rpf`); `--archive` overrides it. Archive, data file, typ, map and collision file names
  must be unique across active packs. File and model names are lowercase `[a-z0-9_]` plus the
  extension, under 64 characters.
- **New content needs new names.** A new model, weapon, component or screen effect must not use a
  name the game has. Only `override` rows replace game content.
- **Mod kit, light and siren ids:** they must lie above the game's own ids (kits 0-613 and 999,
  lights 0-211, sirens 0-20), and must not repeat across packs loaded together. `convert-vehicle`
  picks the lowest ids that no pack under `build/custom-assets` uses. Packs built on another
  computer are not seen, so give packs that will be shared or combined distinct `--kit-id`,
  `--light-id` and `--siren-id`.
- **Wheel names** must not repeat across active packs. **Label keys** (`[A-Za-z0-9_]`) should carry a
  prefix of your own (`GM_<id>_...`); a key the game already has keeps the game's text.
- **Weapon data files** are named per weapon (`--meta-prefix`, default the model name), and each
  converted weapon gets slot order numbers right after its donor's. Give two weapons of one class
  distinct `--orders`.
- **Menu texts** (spawn rows, teleports) are printable ASCII without `~`, under 40 characters.
- **Add-on peds:** the game has room for about 4 add-on peds per GTA process, across every loaded
  pack. Load refuses a selection with more (`<pack>: ped model slots full (430); deselect a ped
  pack`) before anything loads. Plan ped packs with that in mind; clothing packs do not count.

## 6. Validate and install

```sh
./menu-ctl.sh pack-validate build/custom-assets/villa-v1
./menu-ctl.sh pack-validate build/custom-assets/villa-v1 --against build/custom-assets/villa-int-v1
PS5_HOST=<console-ip> ./menu-ctl.sh pack-install build/custom-assets/villa-v1 --activate
PS5_HOST=<console-ip> ./menu-ctl.sh pack-install build/custom-assets/villa-int-v1 --activate-add
./menu-ctl.sh pack-list
```

`pack-validate` checks rows, limits, sizes, hashes, carcols ids, and with `--against` the merge
rules (names, caps, cross-pack typ dependencies). `pack-install` validates again, uploads with
readback and updates the console's installed list; `--activate` / `--activate-add` set the
selection the next Load uses. `pack-uninstall ID [--purge]` removes a pack.

## 7. Test on a console

Packs load once per GTA process, so test each change in a fresh GTA process.

1. Select the packs from the host before GTA starts (`pack-install --activate` / `--activate-add`,
   or `pack-select N`).
2. Start GTA V, load Story Mode, and inject: `./menu-ctl.sh cave-inject`.
3. Start following the pack lines **before** you press Load. The console keeps only the last 128
   lines, and a busy load can push early lines out:

   ```sh
   ./menu-ctl.sh pack-notes --follow --out build/pack-notes/villa-v1.log
   ```

4. In game, press **Custom Packs > Load N selected packs**, then use the content: spawn it, walk to
   it, wear it.
5. Check the log against an expectation file:

   ```sh
   ./menu-ctl.sh pack-check villa-v1.expect build/pack-notes/villa-v1.log
   ```

An expectation file holds one rule per line, and `#` lines are comments:

```text
title: villa-v1 map and collision
pass: pack map 0 entities=
pass>=5: pack bounds [0-9]+ loaded
fail: pack load failed
```

`pass: REGEX` needs at least one matching line, `pass>=N: REGEX` at least N, and `fail: REGEX` none.
`pack-check` prints PASS, MISSING or FAIL per rule and exits non-zero unless every rule holds. A
`# GAP:` line in a followed log means lines were lost between two polls.

When a load is refused, stop there: read `pack-notes` (the `GTAVMenu pack load failed: <step>:
<reason>` line names the gate and its numbers), fix the pack, and retry in a new GTA process.

## 8. Sharing packs

A converted pack holds data built from your own game's templates and from the mod you converted.
Share only what the mod's licence and the game's terms allow. A versioned format for distributing
packs is not defined yet; for now a pack is its folder (`resources/` with `pack.cfg`).

### Host shortcuts for small packs

The public helper also has `menu-ctl.sh` front doors. They validate the pack and print its install
command; `--install` uploads it, and `--activate` also selects it. Existing outputs are refused:
choose a new id for a revised pack.

```sh
./menu-ctl.sh author-labels labels.txt --id my-labels-v1
./menu-ctl.sh author-timecycle --like BlackOut --name gm_dark --id my-dark-v1
./menu-ctl.sh author-ptfx --name gm_sparks --effect scr_indep_firework_fountain --id my-sparks-v1
```

`author-labels` reads `KEY=TEXT` lines. All three accept `--templates DIR`, `--archive NAME.rpf`,
`--against PACK`, and `--description`, `--author`, `--version`. Labels and timecycle accept
`--carrier FILE.ptd` and `--carrier-name NAME.ptd`; timecycle accepts `--meta NAME.meta`.
Timecycle and particle commands accept `--text TEXT` and an optional XML/PPT input before the flags.
The default inputs come from `fetch-templates` / `export-templates`. The particle effect name is
caller-declared; its presence and playback must be checked in game. This command clones an existing
PS5 dictionary and does not convert PC YPTs.
