# Stock replacements and clothing

`convert-override`, `convert-replace`, and `convert-clothing` use reviewed stock ownership metadata and resources from your own matching game copy. They do not read an executable, decrypt archive tables, or require game keys. NumPy and the ordinary retail template cache are needed for texture conversion.

The initial metadata covers PPSA04264 01.010.002:

| Route | Reviewed stock inputs |
| --- | --- |
| Weapon override | Combat Pistol models, magazine and textures, including the HD texture dictionary |
| Vehicle replacement | Police3 low/high fragments and base/HD texture dictionaries |
| Story clothing | Franklin (`player_one`), Torso drawable 14 and Legs drawable 6; matching textures and the reviewed alternate legs rig |
| Freemode apparel | MP Male (`mp_m_freemode_01`), new Legs drawables |

Other stock names and ped/slot combinations refuse. The converters can process different mods within these stock slots when their formats and rigs are supported. This is a scoped stock input catalog, not a scan of arbitrary game assets. The ordinary vehicle converter remains the route for a new vehicle name.

Pass `--game /path/to/app0` and `--templates build/retail-templates` to the conversion command. Stock resources are fetched on demand by pinned archive offset and length, rewrapped as RSC7, and checked against their size and SHA256. The source is opened read-only. Existing game files, archives and mod inputs are never rewritten.

`GTAV_HOST_BUILD_DIR` relocates generated work, default output packs, and default caches together.

Fetched resources are cached under `build/stock-members/ppsa04264-01.010.002`. `--stock-cache DIR` selects another cache. A later run can omit `--game` when every required resource is already cached; cached bytes must match the same reviewed pins. `--stock-manifest FILE` permits a relocated copy of the reviewed metadata only: changing owner paths, coverage or resource pins fails its identity check. Catalog expansion requires a reviewed metadata and identity update.

For example, using the tools directly:

```sh
python3 -I tools/convert_override.py --source /path/to/combat-pistol-mod \
  --id my-pistol --game /path/to/app0 --templates build/retail-templates
python3 -I tools/convert_pc_clothing.py --source /path/to/clothing-mod \
  --ped player_one --slot legs --drawable 6 --id my-legs \
  --game /path/to/app0 --templates build/retail-templates
python3 -I tools/convert_vehicle_replace.py --source /path/to/police3-mod \
  --id my-police --game /path/to/app0 --templates build/retail-templates \
  --repair texture-stride --repair duplicate-bones --repair fragment-quirks
```

The source can be a loose folder, an OPEN PC RPF, or an OIV/ZIP package. Encrypted PC mod archives need their files exported first. Resource collection is bounded and refuses symbolic links and conflicting duplicate names. No archive installer instructions are executed.

Ownership metadata preserves stock folders and every reviewed base/patch copy. Optional HD companions outside that catalog are reported as unknown and are not inferred; an unknown member explicitly supplied by the mod refuses. Shared vehicle dictionaries remain refused. The running menu still checks actual ownership and residency before loading: load before seeing the replaced vehicle, drawing the weapon, or wearing the replaced clothing. Stock vehicle handling values remain unchanged by a replacement pack.

Story clothing replaces the selected stock slot. It tests the reviewed target and alternate skeletons, and refuses a rig that fits neither. Freemode clothing adds new styles after the retail styles. Always inspect the printed residency and wardrobe instructions before testing the pack.

Freemode console checks used full-body suit inputs in the MP Male Legs slot. These checks do not
establish compatibility with typical downloaded freemode clothing mods.
