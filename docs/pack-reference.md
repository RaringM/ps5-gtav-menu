# Custom pack reference

Format, command options and refusal messages for runtime packs on PPSA04263 / PPSA04264
01.010.002. Start with the [user guide](user-guide.md) to install and load packs, or the
[pack author guide](pack-author-guide.md) to convert and build them. Run commands from the
repository root.

## Limits

- Loading is one-way for the game process: there is no unload. Each pack row loads once per
  process; restart GTA to change the loaded set.
- At most 8 packs can be active at once. Together they may hold at most 16 archives (512 MiB),
  64 data files (128 MiB), 16 texture cards, 48 archetype rows, 16 maps, 32 collision files,
  32 weapon wheel icons, 192 labels, 64 spawn rows, 32 teleport rows, 64 hide rows and 64 stock
  overrides (the bracketed numbers in the row table below); `pack-validate --against` checks a set
  before you select it.
- Names (archives, data files, archetype and map files, labels, spawn models) must not repeat
  across active packs, and new models, weapons and screen effects must not reuse a game name.
  Only `override` rows replace existing game content (see below).

## What a pack is

A pack is one `resources/` directory: `pack.cfg`, one to four `.rpf` archives and any `.meta` data
files. `build_runtime_pack.py` writes all of it, including every size and sha256 in `pack.cfg`, so
you normally never edit `pack.cfg` by hand. Content (models, textures, archetypes, maps,
collision) is PS5 RSC7 resources that you convert or take from your own game; the tools below
build new ones from such templates.

## pack.cfg rows

`pack.cfg` is ASCII, LF line endings, one tab between fields, at most 8192 bytes. It starts with
`GTAVPACK,1` and `pack <id>` (`[a-z0-9-]`, starting and ending with a letter or digit, up to 64
characters), and the rows follow in the order of the table (the first three are optional and
appear at most once; older menu builds refuse a `pack.cfg` that has them): every row of one kind comes before any
row of the next. File and model names are lowercase `[a-z0-9_]` plus the extension, under 64
characters (the stock member of an `override` row may also hold `+`, as in `adder+hi.ptd`).
Coordinates are whole metres, at most 16000 from the origin on each axis. Menu texts
(map teleports, spawn and place rows) are printable ASCII without `~` under 40 characters. The limit column
gives the rows one pack may hold, then (in brackets) all active packs together.

| Row | Fields | Limit per pack [all active] | Builder option |
| --- | --- | --- | --- |
| `description` | what the pack is, printable text without `~`, up to 80 characters (**Manage Packs** shows it) | 0 or 1 | `--description` |
| `author` | who made it, printable text without `~`, up to 32 characters | 0 or 1 | `--author` |
| `version` | `[A-Za-z0-9._+-]`, up to 16 characters | 0 or 1 | `--version` |
| `archive` | size, sha256, `<name>.rpf`, optional `overlay` | 1 to 4 [16]; 64 MiB each, 512 MiB together | `--archive`, `--extra-archive`, `--override-archive` |
| `override` | overlay archive of this pack, stock `<stem>.ptd`, `.pft`, `.pdr` or `.pdd` it replaces (in its game folder, if any: `player_one/uppr_014_u.pdd`) | 16 [64] | `--override-archive` |
| `card` | texture dictionary, texture (host texture preview) | 8 [16] | `--card` |
| `typ` | `<name>.ptyp`, optional `retail` (a game typ to keep resident, not in the archive) | 16 [48] | `--typ`, `--retail-typ` |
| `typdep` | pack typ (not `retail`), the game typ it depends on (the game streams it with the pack typ) | one per typ row | `--typ-dep` |
| `map` | `<name>.pmap`, optional x, y, z and a menu text (a **Locations** teleport) | 8 [16] | `--map`, `--map-place` |
| `mapdep` | map, the typ it needs, optional `interior` or `retail` (forms below) | one per map row | `--map-dep` |
| `bounds` | `<name>.pbn` static collision (composite of world-space boxes) | 8 [32] | `--bounds` |
| `data` | type (list below), size, sha256, `<file>.meta` (`AUDIO_GAMEDATA`: `<chunk>_game.rel`) | 16 [64]; 4 MiB each, 128 MiB together | `--data TYPE=PATH` |
| `wicon` | pack weapon, game weapon in the same wheel slot whose weapon wheel icon it shows (a weapon without one shows the wheel's last drawn icon) | 8 [32] | `--wheel-icon WEAPON=DONOR`; added by default for every pack weapon without an icon (the wheel slot's default donor; `--no-default-wheel-icons`) |
| `label` | key `[A-Za-z0-9_]`, printable text without `~` under 64 characters | 128 [192] | `--label KEY=TEXT` |
| `spawn` | kind (list below), model, menu text | 24 [64] | `--spawn KIND:MODEL=TEXT` |
| `tints` | a `spawn weapon` row's weapon, `palette` (its model reads a tint palette: **Weapon Tint** recolours it) or `none` (**Weapon Tint** says `no tint palette` and changes nothing) | one per weapon spawn row | `--tints WEAPON=palette` or `WEAPON=none`; added from the `<!-- gtavmenu tints=... -->` line `convert-weapon` writes into the weapons meta |
| `place` | x, y, z, menu text: a **Locations** teleport without a map | 8 [32] | `--place` |
| `hide` | stock model, x, y, z, radius 1 to 200 | 32 [64] | `--hide MODEL=X,Y,Z,RADIUS` |

One example line per row, in the required order (`build_runtime_pack.py` writes the sizes and
hashes; the archive and data sha256 values here are placeholders):

```text
GTAVPACK,1
pack	my-props
description	Benches for the Davis lot
author	Ann
version	1.0
archive	4194304	9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08	myprops.rpf
archive	262144	60303ae22b998861bce3b28f33eec1be758a213c86c93c076dbe9f558c11c752	myovr.rpf	overlay
override	myovr.rpf	prop_bench_01a.pdr
card	myprop	myprop_diffuse
typ	myprop.ptyp
typ	gm_int_22.ptyp
typ	int_retail.ptyp	retail
typdep	gm_int_22.ptyp	v_int_22.ptyp
map	mymap.pmap	-90	-1792	29	My props
map	gmmap_gun2.pmap	-266	-1658	31	My gun shop
map	gmmap_gun2x.pmap
map	mymap_extra.pmap
mapdep	mymap.pmap	myprop.ptyp
mapdep	gmmap_gun2.pmap	gm_int_22.ptyp	interior
mapdep	gmmap_gun2x.pmap	koreatown_metadata_005_strm.ptyp	retail
mapdep	mymap_extra.pmap	otherprops.ptyp
bounds	mycol.pbn
data	HANDLING_FILE	2048	fd61a03af4f77d870fc21e05e7e80678095c92d808cfb3b5c279ee04c74aca13	handling.meta
wicon	weapon_mypistol	weapon_pistol
label	MYCAR	My Car
spawn	vehicle	mycar	My car
spawn	object	myprop	My prop
spawn	ped	myped	My ped
spawn	weapon	weapon_mypistol	My pistol
spawn	timecycle	my_tint	My screen tint
spawn	ptfx	my_fx:scr_my_fountain	Fountain
spawn	component	weapon_mypistol:component_mypistol_supp	My pistol: suppressor
tints	weapon_mypistol	palette
place	-72	-1778	28	Gas station
hide	prop_streetlight_01	-67	-1787	27	3
```

The three `mapdep` forms, tried in this order:

- the typ is a `typ` row of this pack: the map's archetypes come from it; with `interior` the map
  places that typ's MLO (an interior), as `gmmap_gun2.pmap` above;
- with `retail`, a game typ that no row of this pack lists: the game streams it with the map, as
  for a retail map (never an interior), as `gmmap_gun2x.pmap` above;
- otherwise a typ row of another active pack: it binds once both packs are loaded, so keep both
  selected and check them with `pack-validate --against`, as `mymap_extra.pmap` above.

Data types: `HANDLING_FILE`, `CARCOLS_FILE`, `VEHICLE_METADATA_FILE`, `VEHICLE_VARIATION_FILE`,
`VEHICLE_LAYOUTS_FILE`, `PED_METADATA_FILE`, `WEAPON_METADATA_FILE`, `WEAPONINFO_FILE`,
`WEAPONCOMPONENTSINFO_FILE`, `WEAPON_ANIMATIONS_FILE`, `SHOP_PED_APPAREL_META_FILE` (clothing for
an existing ped), `TIMECYCLEMOD_FILE` (screen effects) and `AUDIO_GAMEDATA` (vehicle sound
settings). Data files are XML meta, except `AUDIO_GAMEDATA`: an audio game-data chunk named
`<chunk>_game.rel`, where `<chunk>` (`[a-z0-9]`, up to 31 characters) is a name no game chunk has.
`tools/make_audio_gamedata.py car` writes one from your game's `game.dat151.rel`: a copy of a car's
sound settings under a new name, optionally with another vehicle's engine sound. A pack vehicle
uses it through `<audioNameHash>NEW_NAME</audioNameHash>` in its vehicles meta; list the
`AUDIO_GAMEDATA` row after that file and spawn the vehicle after loading.

Spawn kinds, each a row on the Custom Packs page once the pack is loaded:

- `vehicle`, `object`, `ped`: spawn the model next to you;
- `weapon`: give the pack's weapon (from its weapon data files, optionally with its own model);
- `timecycle`: apply or clear a screen-effect modifier from a `TIMECYCLEMOD_FILE`;
- `ptfx`: play a particle effect once; the model is `<asset>:<effect>`, where `<asset>.ppt` is a
  particle dictionary in the pack;
- `component`: toggle a weapon component on a weapon, giving the weapon first; the model is
  `<weapon>:<component>`.

Labels name things in game: a vehicle's make and model, LS Customs part names. An `overlay`
archive holds only the stock members its `override` rows replace (model, fragment, drawable or
texture dictionary), and every overlay archive needs at least one `override` row; everything else
in a pack is added under new names. A stock member that its game archive files in a folder keeps
that one folder in the row and in the overlay archive: the game names it by that path (character
clothing: `player_one/uppr_014_u.pdd`, `player_one/uppr_diff_014_a_uni.ptd`).

## Building content

Templates and inputs always come from your own game; none ship with these tools. The resource
writers read their output back before writing it and refuse to overwrite an existing file.

- `tools/make_addon_ptyp.py` makes a one-archetype add-on `.ptyp` by renaming a template typ's
  archetype. It refuses a drawable archetype without a physics dictionary (it would have no
  collision) unless you pass `--allow-no-collision`.
- `tools/make_pmap.py` writes map placements from a JSON spec (template: a one-entity `.pmap`).
  Add the map with `--map`, a teleport row with `--map-place`, and its archetypes with `--typ`
  (a stock archetype from a streamed game typ: `--retail-typ`). Each entity draws until the camera
  is its `lod` (lodDist, default 150 m) away; give a tall or far-seen object a large `lod` and its
  archetype's `bbox` so the map's extents cover it.
- `tools/convert_ymap.py` turns a PC map mod that places stock props and buildings (a binary
  `.ymap`, a CodeWalker `.ymap.xml` or the mod's `dlc.rpf`) into pack maps, and with `--pack-id`
  into a whole pack, in one command. It drops what pack maps cannot hold (custom models, LOD and
  MLO entities, car generators, occluders, LOD lights, grass) and lists it in `report.json`. With
  `--archetype-index` (an index of your game's archetypes, which `menu-ctl.sh index-archetypes`
  builds) it keeps only stock archetypes, sizes
  the map extents from their bounding boxes and adds the `--retail-typ` rows; without an index,
  `--keep-unresolved` keeps every entity. Run it as `python3 -I tools/convert_ymap.py ...`, never
  from inside the mod's folder.
- A map may place archetypes from another pack's typ: every typ row of every active pack is
  requested before any map loads. Keep both packs active and check them together with
  `validate_runtime_pack.py MAP_PACK --against TYP_PACK`, which notes where each map's archetypes
  come from.
- `tools/make_static_pbn.py` writes static collision: axis-aligned boxes from a JSON spec
  (`{"name": ..., "boxes": [{"centre": [x, y, z], "half_extents": [hx, hy, hz]}]}`, heading a
  multiple of 90, optional material). `--template` is required: a composite `.pbn` from your
  game, a drawable that embeds one, or a `.pbn` the tool wrote before. Add the file with
  `--member` and `--bounds`.
- `tools/make_addon_mlo.py` clones an interior `.ptyp` under a new MLO name, and
  `tools/make_addon_mlo_pmap.py` moves its `*_milo_.pmap` placement (optionally with a
  `tools/make_pmap.py` spec for the exterior building). They print the dependency rows the pack
  needs: `--typ-dep NEW.ptyp=SOURCE.ptyp` (the clone uses the source typ's room props, so the game
  streams the source typ with it) and `--map-dep SHELL.pmap=TYP.ptyp:retail` (the exterior's game
  typ), with `--retail-typ` rows as the fallback; bind the interior map with
  `--map-dep MAP.pmap=TYP.ptyp:interior`.
- `tools/prepare_carcols.py` assigns mod kit, light and siren ids and shop labels for a converted
  car (one id per kit and light, numbered up from `--kit-id` / `--light-id`), writes a siren setting
  from a pattern preset or JSON spec (`--siren-preset classic-wigwag|fast-strobe|rotating-beacon`,
  `--siren-spec FILE`, `--list-siren-presets`; lamp groups with a 32-step rhythm, a colour as RGB and
  flash or rotate, `--siren-lamps` maps the car's 20 siren lamps to groups), keeps only the custom
  wheels whose model the pack ships (`--ship-wheel NAME`, a `NAME.pdr` member) and writes livery
  names from the mod's text table as label rows. `tools/make_gxt2.py` writes or checks GXT2 text
  tables. Kit, light and siren ids must lie above the game's own (kits 0-613 and 999, lights 0-211,
  sirens 0-20) and must not repeat across packs loaded together; `validate_runtime_pack.py` and the
  uploader's `--activate-add` check this, and that every car's light/siren setting names an item
  that exists.

`build_runtime_pack.py` writes plain-table archives, which the menu keys on the console.
Override packs require `--plain-toc` with this tool.

## Content tools

Every tool runs with Python's standard library, except the PC model converters at the end of the table (numpy,
for their texture encoders), and prints its full reference with `--help`.

| Tool | Purpose |
| --- | --- |
| `tools/build_runtime_pack.py` | Build a pack (`resources/`: archives, `pack.cfg`, data files) from loose RSC7 resources and data files; every row has an option. |
| `tools/validate_runtime_pack.py` | Check a pack directory on the host: rows, limits, file sizes and hashes, carcols ids, the game's fixed tables (custom vehicles, peds, weapon models, weapon components and weapon files), and with `--against` the merge rules and those tables for packs active together. |
| `tools/upload_runtime_pack.py` | Upload a pack over FTP with readback, select it (`--activate`, `--activate-add`), list (`--list`) or uninstall (`--uninstall ID [--purge]`); `menu-ctl.sh pack-*` wraps it. |
| `tools/make_pmap.py` | Write a map placement `.pmap` of any size from a JSON spec of entities and grids; `--check` reads a `.pmap` back. |
| `tools/make_addon_ptyp.py` | Make a one-archetype add-on `.ptyp` by renaming a template typ's archetype. |
| `tools/make_addon_mlo.py` | Clone an interior `.ptyp` under a new MLO name and print its `--typ-dep` row. |
| `tools/make_addon_mlo_pmap.py` | Move and rename an interior placement (`*_milo_.pmap`), optionally with a spec for its exterior building and the `--map-dep ...:retail` row. |
| `tools/make_mlo_ptyp.py` | Write a whole interior `.ptyp` from a JSON description (`decode`, `import-xml`, `import-pc`, `edit`, `build`, `compare`, `check`). |
| `tools/make_static_pbn.py` | Write static collision (`.pbn`): axis-aligned world-space boxes from a JSON spec. |
| `tools/convert_ymap.py` | Turn a PC map mod that places stock models into pack maps, and with `--pack-id` into a whole pack (see [Converting PC mods](#converting-pc-mods)). |
| `tools/fetch_retail_templates.py` | Fetch and verify the pinned retail templates of `data/retail_templates/` from your own game image (for the PC converters behind the `menu-ctl.sh convert-*` commands). |
| `tools/index_game_archetypes.py` | Build the archetype index of your own game (stock archetype -> typ, bounding box, lodDist) from its typ files, read at the offsets pinned in `data/archetype_index/` and checked by sha256 (no game keys); `convert_ymap.py`, `convert_pc_mlo.py` and `menu-ctl.sh convert-map/-mapmod/-mlo` read it. |
| `tools/extract_mod_files.py` | Copy files of given types (for example `.ybn` collision) out of a PC mod's `dlc.rpf`, nested archives included, as loose files. |
| `tools/make_ped_initdata.py` | Write a one-ped `peds.meta` for a new pack ped, cloned from a donor ped's InitData under the new model name. |
| `tools/convert_pc_mlo.py` | Turn a PC map mod's own interior (an MLO archetype in its `.ytyp` and the `.ymap` placing it) into an interior typ and map, keeping the stock props an archetype index knows (`menu-ctl.sh convert-mlo` runs it). |
| `tools/make_weapon_model_pack.py` | Clone a weapon model under a new name and a stock weapon onto it (weapon, archetype and animation metas from your game's decrypted files), optionally building the pack (`menu-ctl.sh convert-weapon` runs it). |
| `tools/prepare_carcols.py` | Give a converted car's `carcols.meta` and `carvariations.meta` free mod kit, light and siren ids, new shop labels, a siren setting from a pattern preset or spec, and only the custom wheels the pack ships. |
| `tools/make_gxt2.py` | Write or check a GXT2 text table. |
| `tools/convert_pc_drawable.py` | Convert a PC drawable (`.ydr`) onto a retail PS5 drawable of your game: the weapon route (`menu-ctl.sh convert-weapon` runs it, through `tools/weapon_tint_drawable.py` for the tint shader) and the prop route the other model converters use; `--ytd` converts its textures. Its layouts come from `data/drawable_contracts/`. |
| `tools/convert_pc_map_models.py` | Convert a PC mod's own models (`.ydr`, textures, `.ytyp` definitions) into pack members and one pack typ (`menu-ctl.sh convert-model`, `convert-mlo --models` and `convert-mapmod` run it). |
| `tools/drawable_collision.py` | Static collision from the drawn geometry of placed models without collision, class tags from your game's bounds (`--template`; `convert-mapmod --drawn-collision` runs it). |
| `tools/convert_pc_ped.py` | Convert a PC add-on ped (`.ydd`, textures, `.ymt`, props) onto your game's retail ped dictionaries (`menu-ctl.sh convert-ped` runs it). |
| `tools/convert_pc_wheel.py` | Convert the wheel models a PC car mod ships into a wheel pack (`menu-ctl.sh convert-wheels` runs it). |
| `tools/check_worker_span.py` | Build time: fail the build when the menu worker no longer fits the injected memory region (`make` runs it). |

One example each, in the same order (`work/` holds files taken from your own game):

```sh
python3 tools/build_runtime_pack.py --id my-props --archive myprops.rpf \
  --member myprop.ptyp=work/myprop.ptyp --member myprop.pdr=work/myprop.pdr --typ myprop.ptyp \
  --spawn "object:myprop=My prop"
python3 tools/validate_runtime_pack.py build/custom-assets/my-map --against build/custom-assets/my-props
PS5_HOST=<console-ip> python3 tools/upload_runtime_pack.py build/custom-assets/my-props --activate-add
python3 tools/make_pmap.py work/mymap.json --template work/template.pmap --output work/mymap.pmap
python3 tools/make_addon_ptyp.py --template work/icons13.ptyp --new-name myprop --output work/myprop.ptyp
python3 tools/make_addon_mlo.py work/v_int_22.ptyp --typ-name gm_int_22 --mlo-name gm_gun2 \
  --output work/gm_int_22.ptyp
python3 tools/make_addon_mlo_pmap.py work/kt1_11_interior_v_gun2_milo_.pmap --name gmmap_gun2 \
  --mlo-name gm_gun2 --position -90 -1792 28.0 --output work/gmmap_gun2.pmap
python3 tools/make_mlo_ptyp.py build work/interior.json --template work/v_int_22.ptyp \
  --output work/gm_int_x.ptyp
python3 tools/make_static_pbn.py work/mycol.json --template work/template.pbn --output work/mycol.pbn
python3 -I tools/convert_ymap.py mod/dlc.rpf --keep-unresolved --template work/template.pmap \
  --out build/ymap-garage --prefix gmymap_garage --pack-id ymap-garage
PS5_HOST=<console-ip> ./menu-ctl.sh fetch-templates   # runs tools/fetch_retail_templates.py
PS5_HOST=<console-ip> ./menu-ctl.sh index-archetypes  # runs tools/index_game_archetypes.py
python3 -I tools/extract_mod_files.py mod/dlc.rpf --suffix .ybn --out work/ybn
python3 tools/make_ped_initdata.py work/donor_peds.meta --name gm_hero_01 --like S_M_Y_Cop_01 \
  --output work/peds_hero.meta
python3 -I tools/convert_pc_mlo.py mod/dlc.rpf --archetype-index work/index.json \
  --typ-template work/v_int_22.ptyp --milo-template work/template_milo_.pmap --typ-name gm_villa_int \
  --mlo villa_shell=gmmlo_villa --out work/villa-int
python3 tools/make_weapon_model_pack.py --pdr work/w_pi_pistol.pdr --ptd work/w_pi_pistol.ptd \
  --weapons work/weapons.meta --animations work/weaponanimations.meta \
  --archetypes work/weaponarchetypes.meta --source-model w_pi_pistol --source WEAPON_PISTOL \
  --model w_pi_gmpistol --weapon WEAPON_GMPISTOL2 --tint none --out work/gmpistol
python3 tools/prepare_carcols.py --carcols mod/carcols.meta --variations mod/carvariations.meta \
  --prefix GTAVMENU_ --kit-id 4609 --light-id 251 --siren-preset classic-wigwag --siren-id 242 \
  --out-carcols work/carcols.meta --out-variations work/carvariations.meta
python3 tools/make_gxt2.py --entry MYCAR="My Car" --output work/mycar.gxt2
python3 -I tools/convert_pc_drawable.py --source mod/w_pi_combatpistol.ydr --template work/w_pi_combatpistol.pdr \
  --output work/w_pi_combatpistol.pdr --ytd mod/w_pi_combatpistol.ytd --ptd-output work/w_pi_combatpistol.ptd
python3 -I tools/convert_pc_map_models.py mod/dlc.rpf --report work/classify/report.json \
  --template work/ch3_03_ss_cb.pdr --typ-template work/v_int_22.ptyp --typ-name gm_mymod --out work/models
python3 -I tools/drawable_collision.py work/mymap.ymap --drawables work/ydr --archetype 'mymod_*' \
  --template work/hut05_closed_1.pbn --output work/mymod_drawn.pbn
python3 -I tools/convert_pc_ped.py ped --help   # menu-ctl.sh convert-ped passes every retail template
python3 -I tools/convert_pc_wheel.py pack --source mod/carpack.oiv --work work/wheels --list
python3 tools/check_worker_span.py build/ps5/<target>/<profile>/<delivery>/gtav-menu-feature-menu.elf \
  --limit 0x200000
```

## menu-ctl.sh pack commands

The host side of the pack lane. `export PS5_HOST=<console-ip>` selects the console for the session;
console commands refuse when it is unset. Offline conversion and validation do not need it. Run
`./menu-ctl.sh help` for every command.

| Command | What it does |
| --- | --- |
| `pack-validate DIR [--against DIR ...]` | Check a pack directory on the host (files, sizes, hashes, row limits, merge rules); nothing is uploaded. |
| `pack-install DIR [--activate\|--activate-add]` | Validate, upload with readback, add the pack to the installed list and print the in-game step; `--activate` makes it the only selected pack, `--activate-add` adds it (max 8). |
| `pack-upload DIR [--activate\|--activate-add]` | The same validated upload without the summary. |
| `convert-map MOD --id ID --template T.pmap [...]` | Convert a PC map mod that places stock models into a validated pack, optionally installed (see [Converting PC mods](#converting-pc-mods)). |
| `convert-replace MOD --id ID [--model NAME] [--repair NAME ...] [--game DIR] [...]` | Convert a PC "replace" vehicle mod (a car shipped under a stock name) into a stock override pack (reviewed stock names; see [Converting PC mods](#converting-pc-mods)). |
| `convert-override [MOD] --id ID [--only PATTERN ...] [--member STOCK=FILE ...] [--game DIR] [...]` | Convert a supported PC "replace" mod or loose files into a stock override pack (reviewed stock names; see [Converting PC mods](#converting-pc-mods)). |
| `convert-bounds MOD... --id ID [...]` | Convert a PC mod's static collision (`.ybn`) into bounds rows of a validated pack (class tags from the `fetch-templates` cache; see [Converting PC mods](#converting-pc-mods)). |
| `convert-model MOD --id ID [...]` | Convert a PC mod's own models (props, or a map mod's buildings with `--maps` placing them) into a pack typ, `.pdr` and `.ptd` members (needs numpy). |
| `convert-mlo MOD --id ID --mlo NAME[=MAP] [...]` | Convert a PC map mod's interior (MLO archetype and placement) into a pack; `--bounds` adds its collision (bounds templates from `fetch-templates`), `--models` adds its own models (needs numpy). |
| `convert-mapmod MOD --id ID [--dry-run] [...]` | Convert a whole PC map mod as downloaded (stock-model maps, its interiors, collision and, with numpy, its own models and drawn collision) into as many validated packs as the limits need; `--dry-run` prints the split and writes nothing (see [Converting PC mods](#converting-pc-mods)). |
| `convert-ped MOD --id ID --ped NAME --init-data FILE [...]` | Convert a whole PC add-on ped (every component, its textures and props) into a new pack ped (needs numpy). |
| `convert-weapon MOD --id ID --weapon WEAPON_NEW --like WEAPON_DONOR [...]` | Convert a PC weapon model into a new pack weapon (needs numpy). |
| `convert-vehicle MOD --id ID --model NAME [...]` | Convert a PC add-on vehicle (`dlc.rpf`) into a pack vehicle with its textures, LSC parts and data files using verified retail templates. |
| `convert-wheels MOD --id ID [--list] [...]` | Convert the custom wheels a PC car mod ships into a wheel pack (one `.pdr` per wheel and a `CARCOLS_FILE` row; needs numpy; `--list` shows the mod's wheels). |
| `convert-clothing MOD --id ID --ped PED --slot SLOT [...]` | Convert clothing for the reviewed story/freemode slots; see [clothing conversion](pack-author-guide.md#convert-clothing-clothing-for-a-story-character-or-a-freemode-ped). |
| `author-labels FILE --id ID [...]` | Build labels from `KEY=TEXT` lines; see [small pack authoring](pack-author-guide.md#author-labels-timecycle-modifiers-and-particle-clones). |
| `author-timecycle [XML] --like DONOR --name NAME --id ID [...]` | Clone a screen-effect modifier from your game's XML; see [small pack authoring](pack-author-guide.md#author-labels-timecycle-modifiers-and-particle-clones). |
| `author-ptfx [PPT] --name NAME --effect EFFECT --id ID [...]` | Clone an existing PS5 particle dictionary; see [small pack authoring](pack-author-guide.md#author-labels-timecycle-modifiers-and-particle-clones). |
| `pack-list` | Show the console's installed packs; `*` marks the selected ones, the number is the menu row. |
| `pack-select N` | Toggle installed pack N (Custom Packs page order) in the selection. |
| `pack-autoload` | Run the Custom Packs one-press load from the host. |
| `pack-uninstall ID [--purge]` | Remove a pack from the installed list and the selection; `--purge` also deletes its files on the console. |
| `pack-notes` | Print this session's detailed `GTAVMenu pack ...` lines (the reasons behind a refusal). |
| `pack-notes --follow [--out FILE]` | Keep every pack line in a local log while you load (the console keeps only the last 128); run it before pressing Load. |
| `pack-check EXPECT [NOTES]` | Check the pack lines (the console's, or a followed log) against an expectation file of `pass:` / `pass>=N:` / `fail:` regex rules; prints PASS / MISSING / FAIL per rule. |
| `session stage\|next\|go\|check\|stop PLAN [NAME] [...]` | Developer hardware test sessions from a plan file (processes, their packs, in-game steps and expectation files; `tools/hw_session.py`): `stage` uploads the plan's packs, `next` selects a process's packs, `go` waits until GTA is in single-player, injects with the pack-notes follower (`--klog`: also the kernel log) already running and says when to press Load, `check` stops the followers and checks the run's lines. `stage --dry-run` reads inventory when `PS5_HOST` is set; without it, it only lists pack/uninstall candidates. `next --dry-run` and `go --dry-run` work offline. |
| `pack-unhide` | Bring back the stock entities the loaded packs' `hide` rows hid. |
| `pack-revert` | Put the loaded packs' stock overrides back (in game: **Manage Packs > Revert overrides**); while an overridden asset is still in use it waits (asks again every 2 s for up to 3 min). |
| `fetch-templates [--source URL] [--cache DIR]` | Fetch the retail templates from the game image (default: the console over FTP while GTA V runs; `--source file:///path/to/app0` reads a local copy) into `build/retail-templates`, verified. EU 01.010.002 sources are checked against the US 01.010.002 resource pins. Existing verified cache entries are reused. The `convert-*` commands read their templates from there. |
| `export-templates [--cache DIR]` | Export the encrypted layout, audio, weapon and timecycle templates through the running game; verify fresh exports from `/data/gtavmenu/custom/exports`. Requires console `/data` shared with GTA. |

The remaining `pack-*` commands (`pack-register`, `pack-up`, `pack-load-archive`, `pack-inspect`,
`pack-request`, `pack-load-data`, `pack-load-typ`, `pack-load-map`, `pack-finish-map`,
`pack-load-bounds`, `pack-labels`, `pack-card-show`, `pack-card-release`) and `spawn-model`, `spawn-object`,
`spawn-ped` and `give-weapon` run single load steps for debugging. A session loaded that way shows
**Loaded by host commands** on the Custom Packs page.

## Converting PC mods

- **Maps that place stock objects and buildings:** `tools/convert_ymap.py` (above). It reads a
  binary `.ymap`, a CodeWalker `.ymap.xml` or the mod's `dlc.rpf` as untrusted data; run it with
  `python3 -I`, never from inside the mod's folder. Custom models in the mod are dropped and listed
  in `report.json`. `--archetype-index` takes a JSON index of your game's archetypes (schema
  `gtavmenu-archetype-index-v1`). Run `./menu-ctl.sh index-archetypes` to build the index from
  your own game; see the [template setup](pack-author-guide.md#3-templates-come-from-your-own-game).

  One command does the whole route, from the mod as downloaded to an installed pack:

  ```sh
  PS5_HOST=<console-ip> ./menu-ctl.sh convert-map ~/Downloads/garage/dlc.rpf --id ymap-garage-v1 \
    --template work/template.pmap --teleport -205,-1310,31,"Garage" --activate
  ```

  The mod (a `dlc.rpf`, `.ymap`, `.ymap.xml` or an unpacked mod folder) is copied into a fresh
  `build/convert-map/<id>/input` and converted from there with `python3 -I tools/convert_ymap.py`;
  the pack is written to `build/custom-assets/<id>` and checked with
  `tools/validate_runtime_pack.py`, and the summary lists the entities converted and dropped, the
  custom and unknown archetypes and the `typ` rows (details in
  `build/convert-map/<id>/maps/report.json`). `--template` is a one-entity `.pmap` from your own
  game. `--archetype-index FILE` (default `build/archetype-index/<target>.json` when present) keeps
  only stock archetypes; without one every entity is kept unchecked. `--teleport X,Y,Z,TEXT` sets
  the Custom Packs teleport row (default: next to the converted entities). `--install` uploads the
  pack like `pack-install`, `--activate` also selects it; without either, the command prints the
  `pack-install` line to run. A mod that places its own models (custom archetypes, from its
  `.ytyp`, `.ydr`, `.yft` or `.ydd` files) is refused by this stock-only command; use
  `convert-mapmod` or `convert-model --maps` instead. Rerunning with the same `--id` rebuilds the pack.
- **Whole map mods in one command:** `./menu-ctl.sh convert-mapmod` takes a PC map mod as downloaded (its
  `dlc.rpf`, a folder holding its one `dlc.rpf`, or a folder of loose `.ymap`/`.ytyp`/`.ydr`/`.ytd`/`.ybn`
  files), looks at what it ships and splits it into packs that respect the limits above:

  | pack | id | holds |
  | --- | --- | --- |
  | map | `<id>` | the placements (as `convert-map`), the mod's own models they place (as `convert-model --maps`) and its outdoor collision (one `bounds` row per `.ybn`, as `convert-bounds`) |
  | interiors | `<id>` with `-int` before the version | the mod's interiors (MLOs) with their own models and room collision (as `convert-mlo --models --bounds`) |
  | collision | `<id>` with `-col` before the version | the collision rows past the map pack's 8 |
  | props | `--archetypes-id`, default `<id>` with `-props` before the version | with `--archetypes-from MOD`: the models of another mod (a prop pack such as Map Builder) that the maps place |

  ```sh
  ./menu-ctl.sh convert-mapmod ~/Downloads/villa --id villa-v1 --dry-run   # prints the split, writes nothing
  ./menu-ctl.sh convert-mapmod ~/Downloads/villa --id villa-v1 --name "Villa" \
    --map-teleport popolo=-2225,-95,1,"Villa: below the house" --activate
  ```

  `--dry-run` lists what the mod ships, each pack with its maps, entities per map, models, collision rows and
  teleports, what is left out and why (other mods' models, occluders, grass, car generators, Map Editor object
  lists), and the cap use per pack and for the set (maps, typ rows, bounds rows, places, archive size). A map
  pack's maps hold at most 300 entities each (1024 when 300 would need more than 8 maps; past that the mod's
  `.ymap` files are spread over more map packs); typ rows go to the pack's own models first, then to the stock
  typs most entities need; members past a 64 MiB archive move to further archives. `--bounds merge` puts the
  collision files into merged rows instead of companion packs. `--drawn-collision embedded` gives the mod's own
  props whose PC collision lives inside the model (benches, gates, railings: the model route drops it) a
  collision row made from their drawn shape; `--drawn-collision all` does that for every placed own model (mods
  that ship no collision for their buildings). `--translate DX,DY,DZ` moves maps and collision (not interiors:
  add `--no-interiors`). `--map-teleport NAME=X,Y,Z,TEXT` sets the teleport of a map (NAME: a `.ymap` name
  without `.ymap`, or an interior map the dry run lists), `--teleport` adds a teleport row; a Z below the ground
  lands on the highest surface there. `--write-plan FILE` saves the plan as JSON, `--plan FILE` converts an
  edited plan (other names, rows moved between packs or left out). The packs are checked against each other
  and installed together (`--install`, `--activate`): load them together. Own models and drawn collision
  need numpy (as `convert-model`); without it the plan leaves those parts out and says so. Collision rows use the public bounds converter (as `convert-bounds`), and a mod that places only stock
  objects converts with the public tools.

  Many villas and stunt maps are built from **another mod's props** that the map mod does not ship (Map
  Builder's floors, walls and tiles: the dry run lists them as "neither stock nor converted here"). Give that
  mod as well: `--archetypes-from MOD` (its `dlc.rpf` or folder; for Map Builder PRO the props are the
  `MapBuilder/content/<…>/dlc.rpf` of the unpacked download) converts only the props the maps place into one
  more pack (`--archetypes-id ID`, default `<id>` with `-props` before the version: their models, textures and
  typ, no maps), and the map packs place that pack's props, so activate them together. Map Builder ships no
  collision files: add `--drawn-collision embedded` to make the props' floors, walls and roads walkable from
  their drawn shape. `--archetypes-from PACK_DIR` uses a props pack you built before instead (nothing is
  converted; its props get no collision rows).

  ```sh
  ./menu-ctl.sh convert-mapmod ~/Downloads/VillaJayWay --id villa-v2 --name "Villa" \
    --archetypes-from "$HOME/Downloads/MB PRO (SP) 2.23/MapBuilder/content/d9680616-8d4b-4d88-8d58-de9a7871d4b1/dlc.rpf" \
    --archetypes-id mbprops-v1 --drawn-collision embedded --dry-run
  ```
- **Vehicles (add-on cars, bikes, emergency vehicles):** `./menu-ctl.sh convert-vehicle` turns a PC
  add-on vehicle's `dlc.rpf` (or the `.oiv`/`.zip` package holding it) into a pack vehicle: its model
  (`<model>.yft` and `_hi.yft`), textures,
  LSC parts (`--mod-kit`) and its handling, carcols, layouts, vehicles and variations data files. The
  public converter needs numpy for texture conversion. Its reviewed format contracts describe the
  serialized fields; material, shader and dictionary observations are rebuilt from verified templates.
  Retail inputs come only from your game: `fetch-templates` fills the cache and,
  for a mod that ships its own `vehiclelayouts.meta`, `export-templates` (menu injected) adds the
  game's `update.rpf` copy (and the game's base car sounds, used to check `--audio`). The command copies
  the `dlc.rpf` (the one `dlc.rpf` of a mod folder, or the `.oiv`/`.zip` package, whose `dlc.rpf` it
  unpacks) into `build/convert-vehicle/<id>/input`, converts only that copy (a few minutes; the converter's
  work files go to `build/assets/convert-<id>`) and then builds, checks and installs the pack like the
  commands below. Set `GTAV_HOST_BUILD_DIR` to put the copied inputs, intermediate files, template
  cache and generated packs under a separate host build directory; relative paths are resolved against
  the checkout.

  ```sh
  ./menu-ctl.sh convert-vehicle ~/Downloads/protopolice/Add-On/protopolice/dlc.rpf --id proto-v1 \
    --model protopolice --name "Prototipo Police" --make-label GMPRT_MAKE=Grotti --activate
  ```

  `--model` is the add-on's model name (its `<model>.yft`) and `--name` the spawn row and vehicle
  name (default: the model name). `--make-label KEY=TEXT` gives the make a new label (a PC mod's make
  usually has none on the console): the key must start with `GM` and should have at most 11
  characters (the game keeps a vehicle's name and make keys in 11 characters; a longer make key shows no
  make in the game's own name popup and shops), e.g. `GMA80_MAKE`; `--make NAME` uses a stock make
  (`KARIN`). **Name keys are the pack's own:** a pack label never replaces a text the game already has,
  so the car's name label goes under its own key: the mod's `gameName` stays only when it is the model
  name itself (`GTR` for `gtr`), otherwise it becomes `GM<MODEL>` (the run prints a `vehicles.meta:
  gameName ... ->` line; the LaFerrari mod's `TURISMOR` is the stock Turismo R's key, and its car would
  otherwise show "Turismo R"); LSC part, wheel and livery names get `GM<MODEL>_...` keys too. `--driveby NAME` puts
  stock first-person drive-by layouts in place of undefined ones. **Engine sound:** the mod's
  `audioNameHash` stays when it names another car's sound; when it names the car itself, nothing, or a
  sound the mod ships (its sound banks are not converted), a stock sound of the vehicle class is used
  (`BANSHEE` for sports cars, `ADDER` for supercars, `BATI` for motorbikes, `VOLTIC` for electric cars,
  ...). The run prints one `audio:` line with the choice. `--audio NAME` picks a stock sound yourself;
  it is checked against your game's base-game car sounds once `export-templates` has fetched them (a
  DLC car's sound is not in that list: add `--audio-unchecked`). `--audio-engine NAME` gives the car its
  own sound: its stock sound with the engine of base-game car `NAME` (e.g. `--audio-engine SANCHEZ`, a
  dirt-bike engine), loaded as an extra audio data row of the pack. `--mod-kit` also converts the LSC parts: the mod's
  `vehiclemods/<model>_*.yft` and every part its kits list, whatever archive holds them.
  **Exporter quirks** are found and repaired by `--repair auto` (and `--parts-repair auto` for the LSC
  parts), which runs unless you name repairs: it applies each repair that needs no bone list only
  where its check finds the quirk (`select-model`, `texture-stride`, `texture-names`,
  `nonfinite-texcoords`, `duplicate-bones`, `triangle-counts`, `pose-precision`, `fragment-quirks`).
  A quirk that needs bones ends the run with one line naming them, for example `add --repair
  seat-mirror=seat_dside_f,seat_pside_f --repair auto and rerun` (`--parts-repair` for the LSC parts,
  e.g. `inverse-precision=BONES`); rerun with the same `--id` and those options. Naming a repair
  replaces the default, so keep `--repair auto` beside it; `--repair none` converts without repairs.
  **Ids:** a mod that ships a carcols file gets mod kit ids, light ids and, when it ships sirens, siren
  ids; they default to the lowest ids that no pack in `build/custom-assets` uses (one kit id per kit
  and one light id per light setting of the file; kits from 4609, lights and sirens 224-254, which the
  game itself does not use). Packs built on another computer are not seen: give packs you load
  together distinct `--kit-id`, `--light-id` and `--siren-id` then. `--drop-sirens` drops the mod's
  own sirens (stock siren references stay). **Siren patterns:** `--siren-preset NAME`
  (`test-white-slow`, `classic-wigwag`, `fast-strobe`, `rotating-beacon`; `tools/prepare_carcols.py
  --list-siren-presets`) or `--siren-spec FILE.json` replaces the mod's sirens with one pattern (id
  `--siren-id`, default the lowest free one) used by every siren the car has; each lamp is placed on
  its side of the car from the model's siren bones (a lamp the model lacks stays off), or give
  `--siren-lamps` (20 keys for siren1..siren20, `-` unused, e.g. `LLRR------------LRLR`).
  **Custom wheels:** carcols wheels whose models the mod does not ship are left out; a mod that ships
  its own wheel models is refused here: convert the car with `--drop-wheels` and the wheels with
  `convert-wheels` (below). `--max-texture-size N` drops texture mips above N (e.g. 1024) for
  mods with very large textures. **Liveries:** texture liveries (`<prefix>_sign_1..N` textures) come
  with the car's textures and are picked in the menu under LS Customs -> Plates & Livery -> Livery
  (`Livery N/M`); livery parts of the mod kit (`VMT_LIVERY_MOD`) are LSC parts like any other (LS
  Customs -> Part Type: Livery). Part and livery names come from the mod's own English text table when
  it has one. The run prints `liveries:` lines (how many of each kind, and the names). Textures a model
  or an LSC part embeds (livery parts carry their livery texture, often 2048x2048 each) are moved into
  the car's texture dictionary, which the car and all its parts use: a part texture whose name another
  texture of the car already has (with other pixels) is renamed for that part (a `textures:` line), and a
  part whose own dictionary is malformed is left out (a `parts:` line). `--part-texture-size N` limits
  those part textures only (e.g. `512` for a car with 19 livery parts, about 6 MiB instead of 100 MiB).
  LSC parts with unskinned (rigid) models, or with a shader type the converter has no reference for, are
  left out and named in a `parts:` line. **Heavy cars** (a model above 16 MiB, up to the 64 MiB / 128-page
  limit of one game resource): the model is written in retail-style pages; when copies of the detailed
  model into the lower detail levels would pass that limit, those levels stay empty (a log line says
  so), and a texture set above 16 MiB is spread over 4 MiB pages. `--lower-lods empty` keeps those
  levels empty for any car (the detailed model is drawn at every distance, the car then needs about a
  quarter of the memory: the 14 MiB Prowler bike model instead of 60 MiB), `--lower-lods copy` refuses
  instead of leaving them empty, `auto` (the default) copies when it fits. Keep such cars' textures at
  `--max-texture-size 512` (retail cars stay under about 24 MiB of textures); a texture set over the converter's budget is refused with the size to
  pass. **Mod shapes handled:** model files in any letter case (`Prowler.yft` for `--model prowler`);
  motorbikes (an undefined first-person drive-by becomes the drive-by of a stock bike with the same seat
  layout, e.g. `BIKE_BAGGER_FRONT` for `LAYOUT_BIKE_FREEWAY`); a car on a stock handling (`handlingId`
  `POLICE`) keeps the game's handling; the mod's own siren settings are kept under free siren ids;
  texture dictionaries the mod ships as parents of the car's (`txdRelationships`, e.g. an interior
  dictionary) are converted into the pack too (one named like a game dictionary, such as a copy of
  `vehicles_race_generic`, goes in under a pack-owned name `gm<model>_<name>` so the game's stays untouched). `--dry-run` prints the steps without writing; `--archive` and
  `--parts-archive` name the archives (default `gmveh_<id>.rpf`, `gmvehp_<id>.rpf`). Not handled yet:
  new engine sound samples, and a model whose own detailed level passes one game resource (64 MiB / 128
  pages); a 19 MB PC Mercedes-AMG A45 converts (18 MiB, lower levels empty).
- **Collision, own models, interiors, peds and weapons**: one command each turns the mod as
  downloaded into a validated pack. Their converters are public tools (`tools/convert_pc_bounds.py`,
  `tools/convert_pc_mlo.py`, `tools/convert_pc_map_models.py`, `tools/convert_pc_ped.py`,
  `tools/convert_pc_drawable.py`); the model ones need the Python package numpy (`python3 -m pip install
  numpy`), and without it a command refuses before writing anything.
  `convert-mlo` needs an archetype index of your game (`--archetype-index`, as `convert-map`). The retail templates they convert onto come from
  your own game: run `./menu-ctl.sh fetch-templates` once (with GTA V running, or
  `--source file:///path/to/app0` for a local copy of the game files); `--templates DIR` points at
  another cache. Each command copies the mod into a fresh `build/convert-<kind>/<id>/input`
  (symbolic links refused) and converts only that copy, builds the pack into
  `build/custom-assets/<id>`, checks it with `validate_runtime_pack.py`, prints a summary (members,
  sizes, `pack.cfg` rows) and either installs it (`--install`, `--activate`) or prints the exact
  `pack-install` command. Rerunning with the same `--id` rebuilds the pack; a pack directory another
  command made is left alone. Each prints its full usage when run without arguments.

  ```sh
  # Static collision (.ybn files, a folder or the mod's dlc.rpf): one bounds row per file (at most 8;
  # --only NAME picks and orders them, --merge makes one row), at the mod's own coordinates unless
  # --translate DX,DY,DZ moves them; the summary lists each row's box. The PS5 class tags come from
  # the bounds templates fetch-templates caches (bounds-templates/).
  ./menu-ctl.sh convert-bounds ~/Downloads/villa/dlc.rpf --id villa-col-v1 --only villa_col \
    --teleport -1200,500,80,"Villa terrace"
  # A mod's own models: props (--archetype NAME, or every .ydr of a folder) become Custom Packs
  # spawn rows; --maps also converts the mod's placements (as convert-map) onto these models.
  # --map-prefix gm_villa names those maps gm_villa_0.pmap, gm_villa_1.pmap, ...; the default comes
  # from the pack id. Choose a unique prefix for packs that will be loaded together.
  ./menu-ctl.sh convert-model ~/Downloads/benches/dlc.rpf --id benches-v1 --archetype my_bench_01
  ./menu-ctl.sh convert-model ~/Downloads/villa/dlc.rpf --id villa-v1 --maps --activate
  # An interior: the MLO archetype and its placement, --models its own models, --bounds its
  # collision; --map-teleport puts the Custom Packs teleport inside. With --models, repeat
  # --rename-txd OLD=NEW (e.g. villa_textures=gm_villa_textures) to rename texture dictionaries and
  # their archetype references, avoiding dictionary-name clashes with other selected packs.
  ./menu-ctl.sh convert-mlo ~/Downloads/villa/dlc.rpf --id villa-int-v1 --mlo villa_shell=gmmlo_villa \
    --models --bounds --map-teleport gmmlo_villa=-1205,505,81,"Villa hall"
  # A whole add-on ped (.ydd, .ytd, .ymt, .yft; the standard 98-bone rig): every component drawable
  # (head, uppr, lowr, hair, hand, feet, accs, task, decl, jbib, ...) with its textures, and the props
  # of hero_p.ydd / hero_p.ytd next to it (hats, glasses: the ped's PropsName becomes gm_hero_01_p).
  # The new ped copies a donor ped's behaviour from --init-data (a CPedModelInfo InitData list you
  # supply). The summary ends with the variation round trip: every drawable, texture and prop the
  # .ymt lists, checked against the written members (--strict-variations refuses a gap).
  ./menu-ctl.sh convert-ped ~/Downloads/hero/hero.ydd --id hero-v1 --ped gm_hero_01 \
    --init-data work/donor_peds.meta --label "Hero (PC ped)"
  # Hair and cut-out parts (ped_hair_cutout_alpha, ped_hair_spiked) convert too: the pack gets a copy of
  # your game's long_hair_noise. A female ambient ped (its .yft is the 100-bone a_f_y rig) is converted
  # on your game's a_f_y_topless_01 (fetch-templates); give it a female donor, e.g. --like A_F_Y_Topless_01.
  # Use a mod's PC "legacy" files (.ydd v165); its "enhanced" (Gen9) copy is not read yet.
  # Parts of several mods in one ped: --drawable KEY=FILE[:ENTRY] puts another .ydd's entry under KEY
  # (its textures renamed for that slot); --variations retail keeps the stock cop's layout (3 faces,
  # 2 shirts, 3 accessories, ..., a cap and 4 sunglasses) wherever no PC part replaces a slot.
  ./menu-ctl.sh convert-ped ~/Downloads/suit/suit.ydd --id kit-v1 --ped gm_kit_01 \
    --init-data work/donor_peds.meta --variations retail \
    --drawable head_001_r="$HOME/Downloads/goose/BigGoose.ydd:head_000_r" --strict-variations
  # A map object (.ydr) as a prop: --drawable p_head_000=FILE.ydr stands it upright on top of the
  # head (with its own texture and the stock props' vertex colours); --prop-transform
  # p_head_000=RX,RY,RZ,DX,DY,DZ turns it (degrees) and moves it (metres: X up the head, Y forward,
  # Z to the ped's left) instead.
  # A weapon model (.ydr + .ytd of the weapon it replaces on PC) as a new weapon cloned from --like,
  # with the decrypted weapons.meta, weaponanimations.meta and weaponarchetypes.meta of your game.
  # The --like donor decides how it behaves: a pistol, SMG, machine gun, rifle, shotgun, sniper,
  # heavy weapon, melee weapon or throwable, with that weapon's stats, ammo, animations and wheel icon.
  ./menu-ctl.sh convert-weapon ~/Downloads/glock/w_pi_combatpistol.ydr --id glock-v1 \
    --weapon WEAPON_GMGLOCK --like WEAPON_COMBATPISTOL --weapons-meta work/weapons.meta \
    --animations-meta work/weaponanimations.meta --archetypes-meta work/weaponarchetypes.meta \
    --text "Glock 17"
  ./menu-ctl.sh convert-weapon ~/Downloads/glock/w_pi_combatpistol.ydr --id glock-smg-v1 \
    --weapon WEAPON_GMGLOCKSMG --like WEAPON_SMG --text "Glock 17 SMG" ...   # same metas
  # A rifle mod folder with its magazine and attachments (w_ar_carbinerifle_mag1, w_at_scope_medium,
  # w_at_ar_afgrip, w_at_ar_supp): --attachments converts each as a pack component of the new weapon
  # (with your game's decrypted weaponcomponents.meta).
  ./menu-ctl.sh convert-weapon ~/Downloads/m4a1/ --id m4a1-v1 --weapon WEAPON_GMM4A1 \
    --like WEAPON_CARBINERIFLE --text "Colt M4A1" --attachments \
    --components-meta work/weaponcomponents.meta ...   # same metas
  ```

  Converted models get no collision of their own (`convert-bounds` converts a map mod's `.ybn`);
  a converted weapon gets the weapon wheel icon of its slot's stock weapon (a `wicon` row) and a
  `tints` row: `palette` when its body took the game's tint shader (Weapon Tint recolours it), else
  `none` (`--no-tints`, or a donor family without the shader such as the bat). A weapon
  model converts onto the retail drawable of its own name; the fetched templates cover the pistol,
  combat pistol, SMG, carbine and assault rifle, pump shotgun, sniper rifle, RPG, minigun, knife, bat
  and grenade (with their `_hi` and magazine models) and the rifle scope, grip and suppressor. A mod
  model with another name converts onto the donor's model, which needs the same skeleton (bones); for
  any other weapon pass its retail drawable with `--template`. The command refuses, with the reason,
  donors that cannot work (unarmed, gadgets, vehicle weapons, the petrol can and fire extinguisher)
  and a donor whose default attachment needs a
  bone the model lacks (a sniper rifle's scope on a pistol model); attachments on missing bones are
  left out. Thrown and launched weapons keep the donor's projectile model. Several converted weapons
  can be active together: each gets its own data file names and, unless you give `--orders`, slot
  order numbers right after its donor's (give distinct `--orders` to two weapons of one class).
  A mod folder may hold attachment models (`w_at_*`, never taken as the weapon) and, for mods that
  replace only the close-up model, just `<model>_hi.ydr` (converted as the weapon's model). With
  `--attachments`, every model in the folder named like a component of the donor (its magazine, a
  scope, grip, suppressor or flashlight) becomes a new component on the weapon: the magazine replaces
  the default magazine, the others get a Custom Packs row each (`<text>: scope on/off`). The game
  holds at most 470 weapon components and about 5 are free, and about 6 weapon files: load at most a
  few converted weapons with attachments at a time (`Too many weapon components` /
  `Too many weapon files loaded` otherwise). Mods drawn with shaders no stock weapon uses (`default`,
  a scope lens exported as vehicle glass) take the schema of the first stock weapon model that has it,
  or a stand-in weapon shader (the command says so).
  A converted ped is worn from **Self > Appearance > Skin Changer > Custom**; **Self > Appearance >
  Wardrobe** then steps its drawables and textures (**Slot** picks the component: Face = head,
  Mask = berd, Hair = hair, Torso = uppr, Legs = lowr, Hands/Bags = hand, Shoes = feet, Neck = teef,
  Undershirt = accs, Armor = task, Decals = decl, Tops = jbib; Hat = p_head and Glasses = p_eyes props;
  **Style** and **Texture** step the selected one). Drawables
  with the hair/cutout shaders (`ped_hair_cutout_alpha`, `ped_hair_spiked`: most hair and some suits)
  convert like the others; their strands are cut out where the texture is transparent. Textures whose
  sides are not powers of two (e.g. 1000x1000) are resampled to the nearest power of two (a cut-out
  DXT1 texture keeps its holes as DXT5 alpha).
- **Custom wheels (the rims a car mod ships):** `./menu-ctl.sh convert-wheels` turns the wheel models
  a PC car mod lists in its carcols `<Wheels>` items into a wheel pack: one `<wheel name>.pdr` per
  wheel with its textures inside (the game gives a wheel model the shared vehicle texture chain, so a
  separate `.ptd` would not reach it) and a `CARCOLS_FILE` row with the wheels, each with a new shop
  label (`GM_<id>_WHEEL_<n>`) whose text is the mod's own wheel name when its text table has one. It
  reads the mod as downloaded (`.oiv`/`.zip` package, `dlc.rpf` or folder); `--list` prints the mod's
  wheels, their wheel type and model, and writes nothing. Exporter quirks of wheel models are repaired
  on the way (zero triangle counts; rims exported as one-bone skinned meshes are made rigid, as the
  game's own wheels are); a wheel shipped only as `.yft` is not converted yet. Needs numpy
  (`tools/convert_pc_wheel.py`); `fetch-templates` must have fetched
  `wheels/wheel_loride_01.pdr`.

  ```sh
  ./menu-ctl.sh convert-wheels "$HOME/Downloads/Gta5KoRn Car Pack v1.3.oiv" --list
  ./menu-ctl.sh convert-wheels "$HOME/Downloads/Gta5KoRn Car Pack v1.3.oiv" --id korn-wheels-v1 \
    --only wheel_spt_forgelineGA1R --label wheel_spt_forgelineGA1R="Forgeline GA1R" --activate
  ```

  `--type sport|muscle|lowrider|suv|offroad|tuner|bike|highend|bennys-original|bennys-bespoke|open-wheel|street|track`
  puts every wheel in one wheel type (default: the type the mod gives it); `--only NAME` converts only
  those wheels; `--label NAME=TEXT` sets one wheel's shop name; `--max-texture-size N` drops larger
  texture mips. **Picking a pack wheel in game:** load the pack on foot, away from modded cars, then in a
  car open **Vehicle > LS Customs**, set **Wheel Type** to the wheels' type (Cross applies it: a car's
  default type may be another one, e.g. High End on supercars), then step **Wheels**: pack wheels are
  the last designs of that type and the row shows the position and the name (`51/54 Chiron Classique`).
  The real LS Customs shop lists a fixed set of designs per type (Sport ends at Split Six) and never
  shows pack wheels. Packs loaded together must not add wheels of the same name (`pack-validate
  --against` refuses that).
- **Vehicle "replace" mods** (a car shipped under a stock name, installed on PC over the game's own
  `<model>.yft`, `<model>_hi.yft`, `<model>.ytd` and `<model>+hi.ytd`): `convert-replace` converts those
  files into a pack whose overlay archive replaces the stock car's members, one `override` row each
  (`<model>.pft`, `<model>_hi.pft`, `<model>.ptd`, `<model>+hi.ptd`), plus a `vehicle` spawn row. Every
  stock vehicle of that name then has the mod's body after Load: spawn it from the Vehicle Browser or the
  pack's row. It reads the mod's folder, a `dlc.rpf` holding those files or an `.oiv` package, and checks
  the override rules first: each member must be a stock vehicle member of your game (`--game DIR`, a local
  copy of the game files, read only; or a verified `--stock-cache DIR`), a `+hi` texture dictionary is kept
  only when the stock car has one, cars that patch updates re-ship are reported (the menu handles them),
  and shared dictionaries (`vehshare`) are refused. It prints where the stock car may already be loaded
  (traffic, police dispatch, a character's own car): load the pack before one appears, or the menu
  refuses with `<member> in use; restart GTA`. A `handling.meta` in the mod is reported but not applied
  (pack handling files only add new names); other metas are ignored.
  `--repair` names the exporter-quirk repairs the car needs, in order (as for new vehicles).
  This release covers Police3; see [reviewed stock input coverage](stock-conversion.md). Needs numpy:

  ```sh
  ./menu-ctl.sh convert-replace ~/Downloads/PrototipoPoliceCar/Replace --id replace-police3-v1 \
    --name "Police3 (Prototipo replace)" --game /path/to/app0 \
    --repair texture-stride --repair duplicate-bones --repair fragment-quirks
  ```

  Only one pack per replaced car can be active (`pack-validate --against` refuses a repeated `override`).
  Manage Packs > **Revert overrides** puts the stock car back once no such car is loaded.
- **Other "replace" mods** (any stock asset shipped under its own name: texture dictionaries `.ytd`,
  drawables and props `.ydr` with their `_hi` models, weapon models `w_*.ydr` with `+hi.ytd`, ped clothing
  `<comp>_<nnn>_<u|r>.ydd` with its `<comp>_diff_<nnn>_<x>_<race>.ytd`, vehicles `.yft`): `convert-override`
  finds each file's stock owner in your game (`--game DIR`, a local copy of the game files, read only;
  names re-shipped by patch updates are reported, the menu handles them), converts the files and builds a
  pack with one `override` row per stock member (models first, at most 16 rows per pack) and a spawn row for
  a weapon, vehicle or prop. A stock `_hi` model the mod does not ship is converted from its base model; a
  `_hi`/`+hi` file whose stock asset has none is left out. Ped clothing names its ped folder when several
  peds share the name (`--folder player_one` for Franklin); `--like STOCK.pdd=OTHER.pdd` converts onto
  another stock drawable of the same ped whose skeleton matches the mod's. A stock vehicle's `.yft` goes
  through `convert-replace`'s converter. `--only PATTERN` keeps matching stock names (split a mod over 16
  rows into packs), `--member STOCK=FILE` adds a loose file (a PC file or a converted PS5 resource under
  any name), `--max-texture-size N` (default 1024, 0 keeps every mip). It prints where the stock asset may
  already be loaded (a drawn weapon, worn clothing, nearby map objects): load the pack before it is, or the
  menu refuses with `<member> in use; restart GTA`. A PC fragment (`.yft`) of a non-vehicle model is
  refused (no converter yet). Needs numpy. Only the [reviewed stock names](stock-conversion.md)
  are enabled: Combat Pistol, Police3, Franklin Torso 14 / Legs 6 and MP Male Legs apparel.
  Unknown names refuse. `--stock-cache DIR` can supply verified stock inputs without `--game`.
  `--target TARGET` selects the single supported catalog (`ppsa04264-01.010.002`), and
  `--stock-manifest FILE` accepts a relocated copy of its exact reviewed metadata:

  ```sh
  ./menu-ctl.sh convert-override "$HOME/Downloads/Glock17Gen5/Combat Pistol replace" --id ovr-glock-v1 \
    --name "Combat Pistol (Glock 17)" --game /path/to/app0
  ./menu-ctl.sh convert-override "$HOME/Downloads/Nike Techfleece Sweatsuit Pack" --id ovr-franklin-top-v1 \
    --only 'uppr_*' --folder player_one --game /path/to/app0
  ```

  Packs that replace the same stock member cannot be active together. Manage Packs > **Revert overrides**
  puts the stock assets back; while one is still in use the row reads `Waiting: <member> in use` and the
  revert goes through by itself once you have moved away.

## Troubleshooting

A refused load shows on the **Custom Packs** page (the first row reads **Load failed: <reason>**)
and as a toast, in short words; the toast, and the line below the list while that row is
highlighted, start with the pack the load stopped at (`<pack>: <reason>`) when one pack's file or
row refused. The technical reason (which gate refused, with its numbers) goes to
the `GTAVMenu pack load failed: <step>: <reason>` line. Other refusals (selection, uninstall, spawn
rows) show as a toast. `./menu-ctl.sh pack-notes` prints the session's detailed `GTAVMenu pack ...`
lines, which the menu saves on the console about once a second; messages that say "see notes" or
"see klog" are explained there. `./menu-ctl.sh pack-list` shows what the console has installed and
selected.

| Message | Meaning and fix |
| --- | --- |
| `Custom packs unavailable: enable SMP/HEN shared /data, then restart GTA` | Enable game access to the console `/data` through ShadowMountPlus or the HEN setup, then restart GTA. The regular menu remains usable. |
| `Load packs (none selected)` | Toggle a pack on under **Installed packs** first. |
| `packs/active is invalid`, `packs/active is invalid; fix it on the host` | The console's selection file is damaged: reselect with `pack-install --activate` or `pack-select N`. |
| `<id>: pack.cfg invalid; reinstall it`, `Load blocked: bad pack.cfg`, `pack.cfg invalid; reinstall it.`, `pack.cfg unreadable; reinstall it` | That pack's `pack.cfg` is missing, damaged or from a newer builder: reinstall it (`pack-install`); `pack-validate` names the bad row. |
| `Deselect <pack>: same <kind> <name>`, `Clashes with <pack>: same <kind> <name>.`, `Load blocked: packs clash`, `<pack> and <pack>: same <kind> <name>; deselect one` | Two packs use the same archive, map, typ, data file, collision file, label, override target, wheel icon or vehicle/weapon/ped/prop/effect/attachment name; only one of them can be selected. Deselect the named pack, or rename the content (check a set with `pack-validate --against`). |
| `Limit reached: too many <rows>; deselect a pack`, `Over the limit with the others: too many <rows>.`, `Load blocked: over a limit` | Together the selected packs exceed a merged cap (the bracketed numbers in the row table, or the archive and data byte totals); deselect a pack. |
| `8 packs selected; deselect one first` | The selection is full (at most 8 packs can be active); deselect a pack first. |
| `Pack list changed; press again`, `Pack load is running; retry` | The list changed or a load is running; press the row again. |
| `Could not save the selection; retry`, `<id> still listed; uninstall on the host` | The console's pack folder could not be written; retry, or use `pack-select` / `pack-uninstall` on the host (`pack-notes` has the error numbers). |
| `Pack selection invalid; reselect`, `A pack.cfg is invalid; reinstall`, `Selected packs clash or too big` | The game could not read the selection or a `pack.cfg`, or the selected packs do not fit together: reinstall or reselect, then restart GTA. |
| `Menu busy; press Load again` | The menu was checking the selection at the same moment; press Load again. |
| `Pack files damaged; reinstall it`, `Pack file missing; reinstall it` | A file on the console differs from `pack.cfg` or is missing; reinstall (each upload is verified by readback). |
| `Pack archive is not a PS5 RPF`, `Pack archive format not supported`, `Pack archive has no usable files` | An archive is not a PS5 RPF7 archive, its table cannot be read, or it holds nothing the game can use; rebuild it with `build_runtime_pack.py`. |
| `Pack archive did not load`, `Game refused the pack archive`, `Packs not registered; restart GTA`, `Packs did not register; see notes` | The game did not take the pack's archives; restart GTA and load again, and read `pack-notes` if it repeats. |
| `Wrong game version (data loader)`, `Wrong game version (file device)`, `Wrong game version (archives)`, `Wrong game version (overrides)`, `Wrong game version (maps)`, `Wrong game version (collision)`, `Wrong game version (interiors)`, `Wrong game version (labels)`, `Wrong game version (clothing)`, `Wrong game version (components)`, `Wrong game version (audio)`, `Wrong game version (game code)` | The game build is not the supported one (a code check for that area failed); the menu refuses rather than guess. |
| `Too many custom vehicles loaded`, `Too many custom peds loaded`, `Too many custom weapon models`, `Too many weapon components`, `Too many weapon files loaded`, `Too many models; load fewer packs`, `Too many interiors; restart GTA` | The game's fixed table for that kind is full: load fewer packs, or restart GTA. In a fresh game about 7 custom vehicles, 4 peds, 63 weapon models, 5 weapon components and 6 weapon files fit across all selected packs; `pack-validate --against` and `pack-install --activate-add` count the set and refuse it first (`engine table: weapon components: 471 > 470 (465 retail + 6 in these packs: ...)`). |
| `Out of memory; load fewer packs`, `Game slots full; load fewer packs` | The game has too little free memory or too few free slots for the selected packs. |
| `Game busy; retry in free roam`, `Game busy loading; retry later`, `Game still starting; retry later` | The game was not at a safe point for that step. Retry in free roam. |
| `Load in Story mode, then retry` | Load packs once Story mode is running. |
| `Data file type not supported`, `Data file name too long` | Use a data type from the list above and a shorter file name. |
| `Data file empty or not XML`, `Data file is not valid XML`, `Data file layout not supported` | The data file lists nothing, is not plain XML meta (no DOCTYPE, `&lt;`/numeric references, `--` in comments, `<` or `>` in attribute values), or names an entry outside its `<InitDatas>` list. |
| `Data file in use; restart GTA`, `Game refused the data file`, `Data file did not load fully`, `Game did not queue the data file`, `A data file was refused; see notes` | The game did not take a data file; `pack-notes` names it. Restart GTA before retrying. |
| `Model name already used`, `Weapon name already used`, `Component name already used`, `Screen effect name already used` | New content must use a name the game and other packs do not use. |
| `Component name repeated in file`, `Screen effect name repeated`, `Each screen effect needs a name`, `Screen effects: none or over 64` | Fix the data file: each component or screen effect once, with a name, and 1 to 64 screen effects per file. |
| `Weapons did not register`, `Components did not register` | The weapon or component file loaded but its entries did not appear; `pack-notes` names them. |
| `Clothing file lacks ped/dlc names`, `fullDlcName must be ped_dlcName`, `Clothing ped model not found` | The clothing file needs one `pedName` (an existing ped), `dlcName` and `fullDlcName`, with `fullDlcName` set to `<pedName>_<dlcName>`. |
| `Clothing already added`, `Clothing archive not registered`, `Clothing did not attach to the ped` | The clothing loaded before, its archive is not in the pack, or the ped did not take it; restart GTA and check `pack-notes`. |
| `Ped in use; restart and load first` | Clothing must load before that ped is spawned. |
| `Waiting: tuned car nearby; move away`, `Tuned car nearby (parked ones too)` | Loading these mod kits or custom wheels now would free parts or wheels a tuned car near you is drawing: any car with LS Customs parts counts, parked ones (Franklin's at his house) and your own too. The load waits on that data file and goes on by itself once you walk or drive away (no tuned car streamed in); after 3 minutes it stops with the second text and you restart GTA. Load somewhere without tuned cars (the Maze Bank roof works). |
| `Custom wheels are not supported` | The game's wheel code is not the version the menu checks (custom wheels need the supported build); remove the custom wheels from the pack's `carcols.meta`. |
| `Audio file damaged or wrong format`, `Audio file has a reserved object` | An `AUDIO_GAMEDATA` file is not a game-data chunk the game can take (`make_audio_gamedata.py check` names the problem), or it defines the audio object the game re-initialises on every load (copy only the objects you need with `make_audio_gamedata.py car`). |
| `Audio name in use; rename it` | The game (or a pack loaded earlier in this process) already has an audio chunk of that name: rename the file's `<chunk>`, or restart GTA. |
| `Game refused some text labels`, `Pack has no labels` | The game refused some `label` rows (`pack-notes` gives the counts; a key the game already has is kept, not refused), or the pack has no `label` rows (host `pack-labels`). |
| `Override target not found`, `Override target not supported`, `Stock file overlaid by another mod`, `Override archive has extra files` | An `override` row names no stock member the menu can replace, the member is already replaced by another mod, or the overlay archive holds files no `override` row names. |
| `Stock asset in use; restart GTA`, `<member> in use; restart GTA` | A stock model or texture the pack overrides is already loaded: restart GTA and load the packs before using it. |
| `Override did not apply` | The stock member was not replaced; `pack-notes` names it. |
| `No overrides loaded`, `Overrides already reverted`, `Revert failed; see pack-notes` | **Revert overrides** (`pack-revert`) has nothing to put back, already did, or could not; read `pack-notes`. |
| `Stock typ not found in the game` | A `retail` typ row, `typdep` or `mapdep ... retail` row names a typ the game does not have. |
| `Typ already bound; restart GTA`, `Stock typ failed; see pack-notes`, `Timed out waiting for archetypes` | A typ or its dependency did not load; restart GTA and read `pack-notes`. |
| `Map archetype file not in pack`, `Map file not found in pack` | A `map` row's file or its `mapdep` typ is in no loaded archive: add it, or select the pack that holds the typ. |
| `Map file already in use`, `Map file not supported`, `Game refused the map file`, `Map did not activate`, `Map dependency failed; see notes`, `Map was refused; see pack-notes` | The map did not load; restart GTA, and rebuild it with `make_pmap.py` if it repeats. |
| `Collision file not found in pack`, `Collision file already in use`, `Collision file did not load`, `Collision did not load; see notes` | A `bounds` row's file is missing from the archives or did not load; rebuild it with `make_static_pbn.py`. |
| `Interior data freed; restart GTA`, `Interior map lacks its proxy`, `Interior collision not loaded yet`, `Interior collision did not attach` | The interior did not set up; restart GTA and load again, and check the `mapdep ... interior` row. |
| `Packs already loaded; restart GTA`, `Packs already loading or loaded`, `Load failed; restart GTA to load again`, `Loaded by host commands`, `Loaded by host commands; restart GTA` | Packs load once per game process; restart GTA to load a different set. |
| `Pack rows out of step; restart GTA`, `Game state unexpected; restart GTA`, `Game state unreadable; restart GTA` | The menu could not read a game table it checks first, or its own state is out of step; restart GTA (`pack-notes` names the table). |
| `timed out in <step>` | The step never finished; `pack-notes` shows the last state. Restart GTA before retrying. |
| `... see notes`, `... see pack-notes` | The step stopped without its own reason; `pack-notes` shows the last state. |
| `Load the packs first`, `Map is not active yet` | The row works once the load reaches **Ready**. |
| `Wait for the pack load to finish` | Uninstall waits until the load ends. |
| `Needs a newer menu build` | This menu build lacks a game function the row needs (effects, attachments); update the menu. |
| `Screen effect did not load; see notes`, `Screen effect did not apply`, `Effect file not found in pack`, `Effect did not load; see notes`, `Effect did not start; retry` | A pack screen effect or particle effect did not play: its file is missing from the pack or did not stream in; `pack-notes` has the details. |
| `Attachment did not come off`, `Weapon refused the attachment` | The weapon did not take or drop the pack component; the component must match the weapon (`pack-notes` has the details). |
| `Player not ready; retry in free roam`, `Map has no teleport point` | The row needs a player in free roam, or the map row has no teleport coordinates. |
| `Waiting: <member> in use; move away`, `Revert gave up: <member> still in use`, `Revert stopped; see pack-notes`, `<member> changed since load; restart GTA` | **Revert overrides**: the stock model is still in use, so the revert waits and tries again every 2 s for up to 3 min (put the weapon away or change the clothing, move far away; a removed weapon's model unloads about 2 minutes later) and then reverts by itself (`Overrides reverted after <n>s`); it gives up after 3 min (press it again), stops on another refusal, or the game replaced the asset since the load. |

Licensed under the [MIT License](../LICENSE). GTA V, PlayStation, the PS5 payload SDK, and third-party
catalog inputs are not covered by that license. See `THIRD_PARTY_NOTICES` for catalog attribution.
