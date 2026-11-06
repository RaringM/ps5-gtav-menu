# PS4 (GTA V Legacy) plugin

`gtav_menu.prx` runs the menu in GTA V Legacy on a PS4: **CUSA00411, game version 01.57**. It is a
plugin for **GoldHEN 2.3 or newer** and for **nexHEN PS4 v1.00**, not a payload: it needs no payload
loader or debugger. Other title IDs and game versions are refused before the plugin touches the
game. Open the menu with **R1 + D-pad Left**.

It is **experimental**. It has been tested on firmware 9.00 under GoldHEN and nexHEN PS4 v1.00:
loading at game start, the menu, profiles, the touchpad, custom vehicle packs (including stock car
replacements), unloading and reloading in one game, and relaunches.

## Install

Download `GTAVMenu-PS4-CUSA00411-v01.57-plugin.zip` from the release. Install the plugin under
one host, and merge its entry into that host's existing `plugins.ini`: add the line under the
`[CUSA00411]` section, or add the section if it is missing. Keep the file and its other plugins, and
comment out (`;`) any other GTA V menu plugin in that section. The package carries both entries as
`goldhen-plugins.ini.example` and `nexhen-plugins.ini.example`.

### GoldHEN 2.3 or newer

1. Copy `gtav_menu.prx` to `/data/GoldHEN/plugins/gtav_menu.prx`.
2. Merge this entry into the existing `/data/GoldHEN/plugins.ini`:

   ```ini
   [CUSA00411]
   /data/GoldHEN/plugins/gtav_menu.prx
   ```

3. Enable plugin loading in GoldHEN's settings, then cold-launch GTA V Legacy.

### nexHEN PS4 v1.00

1. Copy `gtav_menu.prx` to `/data/nexHEN/plugins/gtav_menu.prx`.
2. Merge this entry into the existing `/data/nexHEN/plugins.ini`:

   ```ini
   [CUSA00411]
   /data/nexHEN/plugins/gtav_menu.prx?autoload
   ```

3. Cold-launch GTA V Legacy.

Copy the PRX from the release package. A file read back from the console over FTP is a decrypted
copy that does not load, unless the FTP server's `SELF` mode was used; never install such a copy.

To update, replace the PRX and relaunch GTA. To uninstall, remove its line from `plugins.ini` and
delete the PRX; `/data/GTAVMenu/` holds the menu's settings, packs and logs.

## At launch

The plugin waits for Story Mode, checks the exact game build, then hooks the game; the menu becomes
available about two minutes into a cold launch. It writes one timestamped section per launch to
`/data/GTAVMenu/ps4-plugin.log`.

**Bigger tables.** Loaded early in the launch, the plugin enlarges the game's asset stores and its
ped and vehicle model capacities (vehicle models 640 to 840, peds 440 to 540), which leaves room
for more add-on vehicles and peds. GoldHEN loads it early enough for all of that; nexHEN loads it
about 10 to 14 seconds into the launch, in time for the ped and vehicle capacities only. Loaded into
a running game, it changes nothing. Every change is checked against the stock value first, and the
menu starts either way; the log's `boot-pool:` lines say what happened. Creating
`/data/GTAVMenu/boot-pools.off` keeps the stock tables.

## Custom packs

Packs work as on the PS5 (the [user guide](user-guide.md), sections 5 to 11), with three
differences:

- A PS4 pack needs the PS4 forms of game resources (`.oft`, `.otd`, `.odr`, `.odd`); a PS5 pack is
  refused. The [pack author guide](pack-author-guide.md) shows how to convert for PS4.
- Install a pack with the uploader directly, from a copy of this repository:

  ```sh
  python3 tools/upload_runtime_pack.py --host <ps4-ip> --port 2121 --target cusa00411-01.57 \
    --activate-add path/to/my-pack
  ```

- Packs stay in the game after the plugin unloads. When the plugin is loaded again into the same
  game, **Load** takes them over instead of asking for a restart ("Custom packs kept from before the
  reload"); newly selected packs then load with **Add N selected packs**. Packs with maps, typs,
  collision or stock replacements are not kept that way: restart GTA to load more after those.

## Unload

Unload through the host's plugin menu with the menu closed (nexHEN). GoldHEN cannot unload a plugin
while the game runs; there, use the menu's **Settings > Runtime > Stop Runtime**. The plugin stops
its features, restores the game's own code, and a "GTAV Menu unloaded" notification confirms it
("GTAV Menu stopped" after Stop Runtime); gameplay continues. An unload during a custom pack Load
waits for the Load to finish, so it can take several seconds.

## Troubleshooting

| Problem | What to do |
| --- | --- |
| GTA stays on "Loading Story Mode" with the screen flashing | Check the console clock first: a console whose clock reset (for example after a dead CMOS battery) has stalled there before the plugin did anything. Set the clock and relaunch. |
| The plugin does not load | Check the `plugins.ini` entry and the PRX path, and that you copied the PRX from the release package, not a copy read back over FTP. |
| The menu never appears | It appears about two minutes into a cold launch, in Story Mode. Check `/data/GTAVMenu/ps4-plugin.log`: a different game build is refused there. |
| The game was suspended during an unload (a system menu was open) | The log says `cooperative shutdown timed out` and `gateway kept mapped`. The unload still completes; the next load into the same game cleans up. |
| A crash or hang | Keep `/data/GTAVMenu/ps4-plugin.log` and include it when you report it. |

To build the plugin yourself, see [Building from source](building.md#ps4-plugin).
