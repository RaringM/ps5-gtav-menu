# Tools

The `tools/` folder holds the Python scripts behind `menu-ctl.sh`, the converters, and the build
and release steps. You rarely run them directly: `./menu-ctl.sh help` lists the host commands,
and each command below names the script it runs. Every script prints its options with `--help`.
They need Python 3.11+; the model converters also need NumPy (`python3 -m pip install numpy`).
Run them from the repository root.

## Packs

| Script | Command | What it does |
| --- | --- | --- |
| `upload_runtime_pack.py` | `pack-install` | Validate a pack, upload it to the console with readback, and select it |
| `validate_runtime_pack.py` | `pack-validate` | Check a pack (and, with `--against`, a set of packs) without uploading |
| `build_runtime_pack.py` | | Build a pack from loose resources and data files |
| `author_runtime_pack.py` | `author-labels`, `author-timecycle`, `author-ptfx` | Small packs from labels, a timecycle modifier or a particle clone |
| `pack_notes.py` | `pack-notes` | Print the menu's detailed pack lines (the reasons behind a refusal) |
| `fetch_retail_templates.py` | `fetch-templates`, `export-templates` | Collect the retail templates the converters need from your own game |
| `index_game_archetypes.py` | `index-archetypes` | Index the stock archetypes of your game copy for map conversion |
| `extract_mod_files.py` | | Copy files of given types out of a PC mod archive (`dlc.rpf`) into a folder |
| `install_release.py` | `./gtavmenu` | Check a staged universal package, or install a standalone worker over FTP |

## Converters (PC mods to packs)

| Script | Command |
| --- | --- |
| `convert_vehicle.py` | `convert-vehicle`: an add-on car, bike or emergency vehicle |
| `convert_vehicle_replace.py` | `convert-replace`: a car shipped under a stock name |
| `convert_override.py` | `convert-override`: a replacement of another reviewed stock asset |
| `convert_pc_clothing.py` | `convert-clothing`: clothing for a story character or a freemode ped |
| `convert_pc_ped.py` | `convert-ped`: an add-on ped |
| `convert_pc_wheel.py` | `convert-wheels`: the rims a car mod ships |
| `convert_pc_drawable.py`, `make_weapon_model_pack.py`, `weapon_tint_drawable.py` | `convert-weapon`: a weapon model as a new weapon |
| `convert_ymap.py` | `convert-map`: a map that places stock objects |
| `convert_map_mod.py` | `convert-mapmod`: a whole map mod |
| `convert_pc_map_models.py` | `convert-model`: a mod's own models |
| `convert_pc_mlo.py` | `convert-mlo`: an interior |
| `convert_pc_bounds.py`, `drawable_collision.py` | `convert-bounds`: static collision |
| `prepare_carcols.py` | Prepare a converted car's `carcols.meta` and `carvariations.meta` for loading |
| `ps4_fragment_convert.ps1` | Windows helper that re-saves PC vehicle models through CodeWalker for PS4 packs |

The [pack author guide](pack-author-guide.md) explains each command.

## Authoring helpers

`make_pmap.py`, `make_addon_ptyp.py`, `make_addon_mlo.py`, `make_addon_mlo_pmap.py`,
`make_mlo_ptyp.py`, `make_static_pbn.py`, `make_gxt2.py`, `make_audio_gamedata.py`,
`make_ped_initdata.py` and `make_ped_apparel_pack.py` write single pack files by hand (maps,
archetypes, interiors, collision, text, audio, ped data and clothing); the
[pack reference](pack-reference.md#content-tools) describes them.

## Console and diagnostics

| Script | Command | What it does |
| --- | --- | --- |
| `menu_client.py` | most live commands | Talk to the running menu: status, menu actions, pack steps |
| `crash_capture.py` | `klog-status`, `klog-stop`, `crash-pull` | Follow the console's kernel log (started by `watch` and `cave-inject`) and copy crash dumps |
| `pool_census.py` | `census` | Read the game's pool and store sizes |
| `hw_session.py` | `session` | Run a scripted console test from a plan file |
| `read_player_ped_anchor.py` | | Check the player-ped readiness anchor that `watch` waits for |

## Build, packaging and release

These run from `make` (see [Building from source](building.md)):

- **Build configuration:** `target_build_config.py`, `target_loader_config.py`,
  `feature_menu_target_cflags.py`, `generate_loader_profiles.py`, `ps4_build.py`.
- **Build checks and stamps:** `write_build_version.py`, `write_build_stamp.py`,
  `check_worker_span.py`, `check_worker_imports.py`, `patch_inject_skip_sdk_patch.py`.
- **Packages:** `build_universal.py`, `package_universal.py`, `package_payload.py`,
  `package_onionhen.py`, `package_etahen.py`, `make_etahen_plugin.py`, `package_ps4_plugin.py`,
  and their inspectors `inspect_onion_plugin.py`, `inspect_etahen_plugin.py`,
  `inspect_ps4_plugin.py`.
- **Release:** `release.py` assembles, verifies and publishes release assets.
- **Test map:** `prepare_map.py` builds the menu's test yard map package.
- **Locales:** `generate_locales.py` regenerates the menu's translation header.

## Libraries

The remaining scripts are modules the converters import. `gtavmenu_tools/` is the shared package
(resource formats, archives, pack descriptors, target manifests, console I/O). The flat modules
read and write game resources: `pc_*` read PC resources, `native_*` and `gen9_*` build and verify
the console forms, `repair_pc_*` fix known exporter quirks in PC models (selected with
`--repair NAME`), and `resource_*`, `shader_*` and `texture_*` hold the shared format rules.
