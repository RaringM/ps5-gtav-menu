# PS4 Legacy plugin

`gtav_menu.prx` runs the menu in GTA V Legacy on a PS4: **CUSA00411, game version 01.57**. It is
a fake-signed SELF PRX plugin for **GoldHEN 2.3 or newer** and for **nexHEN PS4 v1.00**. It is a
plugin, not a payload: it needs no payload loader or debugger. Other title IDs and game versions
are refused before the plugin touches the game. Open the menu with **R1 + D-pad Left**.

## What has been checked

All checks ran on firmware 9.00, one fresh Story Mode process per run.

- **GoldHEN:** an earlier build of this plugin passed load, the menu, a benign action, profile save
  and load, the touchpad, unload and a game relaunch.
- **nexHEN PS4 v1.00:** load at game start, the menu, unload with the original hook and pad import
  restored, several unload and reload cycles in one game process, relaunch, a custom vehicle pack
  load and spawn, and the ped and vehicle capacity growth (vehicle models 640 to 840, peds 440 to
  540).

A newly built PRX has passed host checks only; treat every rebuild as a hardware candidate.

## Install

Never load a second copy of the menu into the same game (for example through a debugger) while
the plugin is loaded. Install the plugin under one host, and merge its entry into that host's
existing `plugins.ini`: add the line under the `[CUSA00411]` section, or add the section if it is
missing. Keep the file and its other plugins, and comment out (`;`) any other GTA V menu plugin in
that section. The release package carries both entries as `goldhen-plugins.ini.example` and
`nexhen-plugins.ini.example`.

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

Copy only the PRX from the release package. A file read back from the console over FTP is a
decrypted copy, not the signed PRX, unless the FTP server's `SELF` mode was used, and the decrypted
copy does not load. Some FTP servers (GoldHEN's among them) have no `SELF` mode: check an upload
there by its size, and never install a file read back from the console.

## At launch

The plugin waits for Story Mode, verifies the exact game fingerprints, then installs its
`PLAYER_PED_ID` game-thread hook; the menu becomes available about two minutes into a cold launch.
Diagnostics, one timestamped section per launch, go to `/data/GTAVMenu/ps4-plugin.log`.

**Boot pools.** When the plugin loads before the game sizes its asset stores, it grows them and
the ped and vehicle model capacities at boot. When it loads after that but before the first
session creates the model factories (nexHEN loads it about 10 to 14 seconds into the launch), it
grows only the ped and vehicle capacities. Later than that, for example after a reload into a
running game, it writes nothing. Every write is checked against the stock value first, and the
menu starts either way. The `boot-pool:` lines in the log say what happened; creating
`/data/GTAVMenu/boot-pools.off` turns boot pools off.

If GTA stays on "Loading Story Mode" with the screen flashing, check the console clock first: a
console whose clock reset (for example after a dead CMOS battery) has been seen to stall there
before the plugin did anything.

## Unload

Unload through the host's plugin menu with the menu closed (nexHEN; GoldHEN cannot unload a plugin
while the game runs, so there use the menu's **Settings > Runtime > Stop Runtime**). The plugin
stops its features, restores the original hook and the pad import, and unloads; gameplay continues
and a "GTAV Menu unloaded" notification confirms it ("GTAV Menu stopped" after Stop Runtime). An
unload that arrives while the menu is busy on the game thread (a custom pack Load) waits for it to
finish first, so it can take several seconds.

Custom packs stay in the game after an unload. When the plugin is loaded again into the same game,
**Load** takes them over instead of asking for a restart ("Custom packs kept from before the
reload"): the plugin recorded them in `/data/GTAVMenu/state/packs-loaded` when their load finished,
and the new instance checks the game still holds each archive and model first. Newly selected packs
then load with **Add N selected packs**. Packs with maps, typs, collision or overrides are not kept
this way: restart GTA to load more after those.

If the game is suspended at that moment (for example while a system menu is open), the log
reports `cooperative shutdown timed out (-1009)` and `gateway kept mapped (frames stalled)`: the
restore still completes, and the plugin leaves a small marker (`/data/GTAVMenu/state/unclean-stop`)
so the next load into the same game turns the features off and reclaims that memory. Stop at the
first crash, hang, wrong fingerprint, failed restoration or duplicate hook, and keep the log.

## Build from source

The plugin builds with clang and ld.lld from LLVM 20 and the OpenOrbis PS4 toolchain **v0.5.4**
(`toolchain-llvm-18.tar.gz`, SHA-256
`3c7cd5bb593ca74fa1c13fd59f3938dc0fc07985167f7275063019e63abe4526`):

```sh
make ps4-plugin-release OO_PS4_TOOLCHAIN=/path/to/OpenOrbis/PS4Toolchain
```

The package lands in `build/pkg/cusa00411-01.57/ps4-plugin/` with `validation.json` and
`package-manifest.json` (hashes, source commit, boot-pool mode). The PRX bytes do not depend on
where the checkout or the toolchain is installed.

`validation.json` also records the toolchain: the compiler and linker lines the build left in the
ELF, and the sha256 of the OpenOrbis files that shape the PRX (`link.x`, the `libkernel` and
`libSceLibcInternal` stubs, `create-fself` and the headers), with `pinned` true when they are the
v0.5.4 release's. `tools/release.py` ships a PRX only when those are the pinned OpenOrbis files and
LLVM 20 compiled and linked it. The plugin links only the console's libc and kernel stubs, never
OpenOrbis's static `libc.a`.
