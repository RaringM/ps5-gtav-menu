#!/usr/bin/env bash
# menu-ctl.sh -- build, deploy, and operate the supported GTA V menu.
#
# The single supported lane: build the injected menu worker ELF (the PLAYER_PED_ID
# frame-hook variant) + the SDK payload loader, deploy the loader to the PS5 via
# prospero-deploy, and let it map the worker into the foreground GTA and write the
# external-install CHAIN frame hook on PLAYER_PED_ID (0x18f7170). That native fires
# every frame in valid script context, so queued game-thread actions (vehicle spawn,
# weapons, skin) drain with no controller input.
#
#   ./menu-ctl.sh cave-inject [--show]          inject into an already-ready Story session
#   ./menu-ctl.sh watch [same opts] [--persist] wait for player-world/load readiness, then inject
#                                             (run BEFORE launching GTA -- it parks on the console and
#                                              polls an RE'd SP_READY_ADDR anchor; --persist keeps it
#                                              resident to re-inject on each relaunch)
#   ./menu-ctl.sh stop                        tell the running menu worker to stop
#   ./menu-ctl.sh restart                     rejected: a fresh GTA process is required
#   ./menu-ctl.sh status [--pid N]            print the live menu status block
#   ./menu-ctl.sh show | hide                 toggle menu visibility
#   ./menu-ctl.sh spawn [index]               send a one-shot vehicle-spawn command
#   ./menu-ctl.sh map-upload [package-dir]     verify + upload a prepared map.cfg
#   ./menu-ctl.sh map-load                    load the uploaded map through the game-thread lane
#   ./menu-ctl.sh map-cancel                  stop an in-progress map load
#   ./menu-ctl.sh map-clear                   cancel loading and delete menu-owned map entities
#   ./menu-ctl.sh pack-register               register the active runtime pack's archive
#   ./menu-ctl.sh pack-up                     register, load archive, parse data rows, request typ rows
#   ./menu-ctl.sh pack-load-archive           stream its archive TOC; then pack-request <card>
#   ./menu-ctl.sh pack-request N              request card N's texture dictionary once
#   ./menu-ctl.sh pack-load-data N            load data row N through its data-file mounter once
#   ./menu-ctl.sh spawn-model NAME            spawn a vehicle by model name
#   ./menu-ctl.sh spawn-object NAME           spawn an object by model name
#   ./menu-ctl.sh spawn-ped NAME              spawn a ped by model name
#   ./menu-ctl.sh give-weapon NAME            give the player a weapon by name (e.g. weapon_gmpistol)
#   ./menu-ctl.sh pack-install DIR [--activate|--activate-add]
#                                              validate + upload a built pack, then print the in-game step
#   ./menu-ctl.sh convert-map MOD --id ID --template T.pmap [--teleport X,Y,Z,TEXT]
#       [--archetype-index FILE] [--install|--activate]
#                                              convert a PC map mod (dlc.rpf, .ymap, .ymap.xml or
#                                              folder) that places stock models into a validated pack
#                                              under build/custom-assets/ID; optionally install it
#   ./menu-ctl.sh convert-replace MOD --id ID [--model NAME] [--repair NAME ...] [--game DIR]
#                                              convert a PC "replace" vehicle mod (a car under a stock
#                                              name) into a stock override pack (reviewed stock names)
#   ./menu-ctl.sh convert-override [MOD] --id ID [--only PATTERN ...] [--member STOCK=FILE ...] [--game DIR]
#                                              convert a PC "replace" mod of a reviewed stock asset (textures, props,
#                                              weapons, ped clothing, vehicles) or loose files into a stock
#                                              override pack (reviewed stock names)
#   ./menu-ctl.sh convert-clothing MOD --ped PED --slot SLOT --id ID [--drawable N ...] [--game DIR|--stock-cache DIR]
#                                              a PC clothing mod for Michael/Franklin/Trevor (replaces stock
#                                              drawable N) or the freemode peds (adds new drawables)
#   ./menu-ctl.sh author-labels FILE --id ID [--carrier FILE.ptd] [--install|--activate]
#   ./menu-ctl.sh author-timecycle [FILE.xml] --id ID --like NAME --name NEW [--install|--activate]
#   ./menu-ctl.sh author-ptfx [FILE.ppt] --id ID --name NEW --effect NAME [--install|--activate]
#   ./menu-ctl.sh convert-bounds MOD... --id ID [--only NAME ...] [--merge] [--translate DX,DY,DZ]
#   ./menu-ctl.sh convert-model MOD --id ID [--archetype NAME ...] [--maps]
#   ./menu-ctl.sh convert-mlo MOD --id ID --mlo NAME[=MAP] ... [--models] [--bounds]
#   ./menu-ctl.sh convert-mapmod MOD --id ID [--dry-run] [--translate DX,DY,DZ] [--drawn-collision embedded|all]
#   ./menu-ctl.sh convert-ped MOD --id ID --ped NAME --init-data FILE
#   ./menu-ctl.sh convert-weapon MOD --id ID --weapon WEAPON_NEW --like WEAPON_DONOR
#   ./menu-ctl.sh convert-vehicle MOD --id ID --model NAME [--name TEXT] [--mod-kit] [--repair NAME ...]
#   ./menu-ctl.sh convert-wheels MOD --id ID [--type TYPE] [--only NAME ...] [--label NAME=TEXT ...] [--list]
#                                              PC mod collision / own models / interiors / add-on peds /
#                                              weapon models / add-on vehicles / a mod's own wheels into a
#                                              validated pack (the model converters need numpy);
#                                              each takes [--install|--activate]
#                                              and prints its full usage
#   ./menu-ctl.sh pack-list                   show the console's installed and active packs
#   ./menu-ctl.sh pack-notes                  print this session's "GTAVMenu pack ..." lines
#   ./menu-ctl.sh pack-notes --follow [--out F] keep every pack line in a local log while a load runs
#   ./menu-ctl.sh pack-check EXPECT [NOTES]   check pack lines against a run's expectation file
#   ./menu-ctl.sh pack-revert                 put the loaded packs' stock overrides back (in-game:
#                                              Manage Packs > Revert overrides); refuses while in use
#   ./menu-ctl.sh pack-upload DIR [--activate|--activate-add]
#                                              upload a built runtime pack; optionally mark it active
#   ./menu-ctl.sh pack-uninstall ID [--purge]  drop a pack from packs/installed + packs/active
#                                              (--purge also deletes its files on the console)
#   ./menu-ctl.sh pack-validate DIR [--against DIR ...]
#                                              check a pack directory (files, limits, merge rules)
#   ./menu-ctl.sh fetch-templates [--source URL] [--cache DIR]
#                                              fetch the vehicle converter's retail templates from the
#                                              game image (default: the console over FTP, GTA V running)
#   ./menu-ctl.sh export-templates [--cache DIR] have the injected menu export the encrypted templates
#                                              (vehiclelayouts.meta, base audio data) and pull them into the cache
#   ./menu-ctl.sh index-archetypes [--source URL] [--output FILE]
#                                              build the archetype index convert-map/-mapmod/-mlo read from
#                                              the game image's typs (default: the console over FTP, GTA V running)
#   ./menu-ctl.sh pack-select N              toggle installed pack N (Custom Packs order) in packs/active
#   ./menu-ctl.sh pack-autoload               the Custom Packs one-press load, from the host
#   ./menu-ctl.sh pack-load-typ N             request pack archetype definitions row N
#   ./menu-ctl.sh pack-load-map N             load and activate pack map data row N
#   ./menu-ctl.sh pack-load-bounds N          load pack static collision bounds row N (.pbn)
#   ./menu-ctl.sh pack-unhide                 undo the loaded packs' applied `hide` rows (debug)
#   ./menu-ctl.sh pack-card-show              draw the last requested pack card centred on screen
#   ./menu-ctl.sh pack-card-release           hide the card, then release after the draw lists drain
#   ./menu-ctl.sh doctor                      read-only health report
#   ./menu-ctl.sh session stage|next|go|check|stop PLAN [NAME] [...]
#                                              a hardware test session from a plan (tools/hw_session.py)
#
# --all is a documented no-op: the game-thread rows (weapons, skin, spawn) already
# unlock once the frame hook is live. The menu stays hidden after injection and opens with
# R1+D-pad Left; --show explicitly opens it for diagnostic runs.
#
# Env: PS5_HOST (required for console commands), PS5DEBUG_PORT (744, ps5debug control),
# PS5_PORT (9021, prospero-deploy), PS5_FTP_PORT (2121, ELF upload). The menu injects
# into the foreground GTA title selected by GTAV_TARGET, so the two-eboot decoy is handled by
# pinning ps5debug to the injected pid.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
CALLER_DIR="$PWD"
cd "$ROOT"

PS5_HOST="${PS5_HOST:-}"
PS5DEBUG_PORT="${PS5DEBUG_PORT:-744}"
PS5_PORT="${PS5_PORT:-9021}"
PS5_FTP_PORT="${PS5_FTP_PORT:-2121}"
GTAV_TARGET="${GTAV_TARGET:-ppsa04264-01.010.002}"
GTAV_BUILD_PROFILE="${GTAV_BUILD_PROFILE:-production}"
GTAV_DELIVERY="${GTAV_DELIVERY:-menu-ctl}"
export GTAV_TARGET GTAV_BUILD_PROFILE GTAV_DELIVERY
BUILD_PROFILE_DIR="build/ps5/${GTAV_TARGET}/${GTAV_BUILD_PROFILE}/${GTAV_DELIVERY}"
PAYLOAD_LOADER_ELF="$BUILD_PROFILE_DIR/gtav-menu-payload-loader.elf"
PAYLOAD_LOG_REMOTE="/data/GTAVMenu/gtav-menu.log"
# Stop sentinel for a resident --persist loader daemon: dropping this file asks the daemon to exit
# at its next poll tick (gtav_daemon_stop_path in include/gtavmenu/daemon_lifecycle.h).
DAEMON_STOP_REMOTE="/data/GTAVMenu/daemon.stop"
# Where the payload loader reads the feature-menu ELF it injects
# (GTAV_MENU_INJECT_ELF_PATH in src/payload_loader/main.c).
PAYLOAD_INJECT_ELF_REMOTE="/data/GTAVMenu/gtav-menu-feature-menu.elf"
CUSTOM_REMOTE="/data/GTAVMenu/custom"
MAP_REMOTE="$CUSTOM_REMOTE/maps/active.map.cfg"
# The injected worker: the PLAYER_PED_ID frame-hook variant (the one working lane).
FRAME_HOOK_ELF="$BUILD_PROFILE_DIR/gtav-menu-feature-menu.elf"
if [ -x "$ROOT/.venv/bin/python" ]; then
  PY="$ROOT/.venv/bin/python"
else
  PY="$(command -v python3 || command -v python)"
fi
TARGET_PROFILE="$("$PY" tools/target_build_config.py \
  --target-manifest "data/targets/$GTAV_TARGET.json" --make 2>/dev/null)" || {
  echo "[menu-ctl] invalid or missing target manifest: data/targets/$GTAV_TARGET.json" >&2
  exit 2
}
for required_profile_token in channel=primary cave_inject=1 classic_inject=0 phase_intercept=1; do
  case " $TARGET_PROFILE " in
    *" $required_profile_token "*) ;;
    *)
      echo "[menu-ctl] target is not an admitted primary ptrace-free phase target: $GTAV_TARGET" >&2
      exit 2
      ;;
  esac
done
TARGET_TITLE_ID=""
TARGET_CUSTOM_PACKS=0
for profile_token in $TARGET_PROFILE; do
  case "$profile_token" in
    title_id=*) TARGET_TITLE_ID="${profile_token#title_id=}" ;;
    custom_packs=*) TARGET_CUSTOM_PACKS="${profile_token#custom_packs=}" ;;
  esac
done
[ -n "$TARGET_TITLE_ID" ] || {
  echo "[menu-ctl] target profile has no title identity: $GTAV_TARGET" >&2
  exit 2
}
CLIENT="tools/menu_client.py"
TARGET_ID="GTAV_FEATURE_MENU"
# Code cave for the ptrace-free bootstrap stub: the executable segment's tail padding. Build-specific,
# so the value lives in make/config.mk's per-target arm (PAYLOAD_LOADER_CAVE_ADDR) rather than here --
# hardcoding 01.010.002's 0x3796000 in the lane would write the stub into live code on another build.
# Set CAVE_ADDR in the environment only to override the target's value.
CAVE_ADDR="${CAVE_ADDR:-}"
CAVE_ALLOC="${CAVE_ALLOC:-0x200000}"
HOOK_STATUS_NAME="disabled"

# Keep host-only conversion, validation and local log checks usable without a console address.
require_console_host() {
  [ -n "${PS5_HOST//[[:space:]]/}" ] && return 0
  echo "[menu-ctl] set PS5_HOST=<console-ip> for console commands" >&2
  return 2
}

# When RUN_CLIENT_TIMEOUT is set (seconds), bound each ps5debug call so a slow/never-
# matching scan can't hang the script. Numeric, so unquoted expansion is safe.
RUN_CLIENT_TIMEOUT=""
run_client() {
  local -a target_args=()
  case "${GTAV_AUTO_TARGET:-0}" in
    1) target_args=(--auto-target) ;;
    0) ;;
    *) echo "[menu-ctl] GTAV_AUTO_TARGET must be 0 or 1" >&2; return 2 ;;
  esac
  ${RUN_CLIENT_TIMEOUT:+timeout $RUN_CLIENT_TIMEOUT} \
    "$PY" "$CLIENT" --host "$PS5_HOST" --port "$PS5DEBUG_PORT" "${target_args[@]}" "$@";
}

# Pin ps5debug at the exact pid the payload injected into -- ps5debug otherwise
# targets foreground()'s pid, which can be the title's other eboot (the two-process
# decoy). Set by the injection path once the injected pid is known. Numeric: unquoted is safe.
TARGET_PID_ARGS=""

wait_running() {
  local addrs status_arg=""
  addrs="$(resolved_block_addrs)"
  [ -n "$addrs" ] && status_arg="--address ${addrs##* }"
  run_client status-wait \
    --target-id "$TARGET_ID" \
    --hook-status-name "$HOOK_STATUS_NAME" \
    --require-running \
    --require-no-error \
    --require-tick-increase \
    $TARGET_PID_ARGS \
    $status_arg \
    --output build/gtav-status-menu-ctl-running.json
}

# Resolve the injected worker's mailbox/status block addresses from the worker ELF's symbols plus
# the module base the loader logged. Echoes "<mailbox_hex> <status_hex>", or nothing on failure.
#
# The client's own discovery sweeps ANONYMOUS maps looking for the block magic, but on the
# frame-hook lane the worker is a MAPPED MODULE -- so the mailbox and status blocks live inside it
# and the sweep never sees them ("no GTAVMenu status block found"). Every status poll and mailbox
# command then fails against a perfectly healthy worker. Resolve the status and command blocks
# by symbol instead. Echoing nothing leaves the client to fall back on its scan.
# Absolute address of one worker symbol: the module base the loader logged plus the symbol's offset
# in the worker ELF. Echoes "0x<hex>", or nothing when anything is missing (no inject logged yet, no
# ELF to read, no llvm-nm) -- every caller treats that as "fall back to the client's own scan".
# Map a worker symbol name to the key the loader publishes it under.
_block_key_for_symbol() {
  case "$1" in
    gtav_menu_status) echo "status" ;;
    gtav_menu_command_mailbox) echo "mailbox" ;;
    gtav_menu_log_ring) echo "logring" ;;
    *) echo "" ;;
  esac
}

resolved_symbol_addr() {
  local name="$1" base off staged_len local_len staged_from_archive=0 key published

  # Prefer the address the LOADER published ("inject: blocks status=0x.. mailbox=0x.. logring=0x..").
  # It is authoritative: computing base+offset from the local worker ELF is wrong whenever that copy
  # is not byte-identical to the staged one, and a rebuild with different flags can keep the same file
  # size while moving symbols by 0x4000 -- observed, and not catchable by the size check below.
  key="$(_block_key_for_symbol "$name")"
  if [ -n "$key" ]; then
    # The LIVE log first, and only then the archive. Concatenating both and taking the last match
    # lets a stale archived address win, which reads as "bad status magic" against a healthy worker --
    # the archive is the PREVIOUS run's log, so its addresses belong to a worker that is gone.
    published="$(ftp_cat "$PAYLOAD_LOG_REMOTE" \
      | sed -n "s/.*inject: blocks .*$key=0x\([0-9a-f]*\).*/\1/p" | tail -1)"
    if [ -z "$published" ] && [ -f "$PAYLOAD_LOG_ARCHIVE" ]; then
      published="$(sed -n "s/.*inject: blocks .*$key=0x\([0-9a-f]*\).*/\1/p" \
        "$PAYLOAD_LOG_ARCHIVE" | tail -1)"
    fi
    if [ -n "$published" ]; then
      echo "0x$published"
      return 0
    fi
  fi
  # A universal loader may select a different worker than this checkout's GTAV_TARGET.
  # Use its published addresses or the client's validated block scan, never local ELF offsets.
  [ "${GTAV_AUTO_TARGET:-0}" != "1" ] || return 0
  base="$(ftp_cat "$PAYLOAD_LOG_REMOTE" | sed -n 's/.*inject: mapped base=0x\([0-9a-f]*\).*/\1/p' | tail -1)"
  if [ -z "$base" ] && [ -f "$PAYLOAD_LOG_ARCHIVE" ]; then
    # The live log has no base: a later run cleared it and did not get as far as mapping. The worker
    # from the previous run may still be resident, so fall back to the archived log. A stale base is
    # safe here -- these are read-only lookups, and the block magic simply will not match.
    base="$(sed -n 's/.*inject: mapped base=0x\([0-9a-f]*\).*/\1/p' "$PAYLOAD_LOG_ARCHIVE" | tail -1)"
    staged_from_archive=1
  fi
  [ -n "$base" ] || return 0
  [ -f "$FRAME_HOOK_ELF" ] || return 0
  command -v llvm-nm >/dev/null 2>&1 || return 0
  # Offsets are only meaningful if the local ELF IS the one that was injected. A rebuild with
  # different flags moves every symbol, and resolving against it would hand out confidently wrong
  # addresses -- worse than not resolving at all, because the caller would read whatever is there.
  # The loader logs the staged ELF's byte length, so compare against that and fall back to the
  # client's own scan on any mismatch.
  if [ "${staged_from_archive:-0}" = "1" ]; then
    staged_len="$(sed -n 's/.*inject: loaded .* (\([0-9]*\) bytes).*/\1/p' "$PAYLOAD_LOG_ARCHIVE" | tail -1)"
  else
    staged_len="$(ftp_cat "$PAYLOAD_LOG_REMOTE" | sed -n 's/.*inject: loaded .* (\([0-9]*\) bytes).*/\1/p' | tail -1)"
  fi
  local_len="$(wc -c < "$FRAME_HOOK_ELF" 2>/dev/null | tr -d ' ')"
  if [ -n "$staged_len" ] && [ -n "$local_len" ] && [ "$staged_len" != "$local_len" ]; then
    return 0
  fi
  off="$(llvm-nm --defined-only "$FRAME_HOOK_ELF" 2>/dev/null | awk -v n="$name" '$3 == n {print $1}')"
  [ -n "$off" ] || return 0
  "$PY" - "$base" "$off" <<'PYEOF'
import sys
base, off = (int(a, 16) for a in sys.argv[1:3])
print(f"0x{base + off:x}")
PYEOF
}

resolved_block_addrs() {
  local mailbox status
  mailbox="$(resolved_symbol_addr gtav_menu_command_mailbox)"
  status="$(resolved_symbol_addr gtav_menu_status)"
  [ -n "$mailbox" ] && [ -n "$status" ] || return 0
  echo "$mailbox $status"
}

menu_command() {
  local command="$1"
  shift || true
  local addrs blocks=""
  addrs="$(resolved_block_addrs)"
  if [ -n "$addrs" ]; then
    blocks="--mailbox-address ${addrs%% *} --status-address ${addrs##* }"
  fi
  run_client menu-command \
    --target-id "$TARGET_ID" \
    --hook-status-name "$HOOK_STATUS_NAME" \
    --require-running \
    --require-no-error \
    --command "$command" \
    $TARGET_PID_ARGS \
    $blocks \
    "$@" \
    --yes-command
}

# Pull a console file over FTP to stdout (empty if absent).
ftp_cat() { curl -s --max-time 10 "ftp://$PS5_HOST:$PS5_FTP_PORT$1" 2>/dev/null || true; }
# Archive the loader log locally, then clear it on the console. Every lane clears it so that what it
# reports comes from THIS launch -- but the log is also where resolved_symbol_addr() reads the injected
# worker's module base, so clearing it for a run that then FAILS leaves a perfectly healthy worker
# unaddressable (status/doctor/logs --worker/mailbox commands all stop working until the next
# successful inject). Keeping the previous copy gives the resolver a fallback.
PAYLOAD_LOG_ARCHIVE="build/gtav-menu-loader-prev.log"
archive_and_clear_payload_log() {
  mkdir -p "$(dirname "$PAYLOAD_LOG_ARCHIVE")"
  local prev
  prev="$(ftp_cat "$PAYLOAD_LOG_REMOTE")"
  [ -n "$prev" ] && printf '%s\n' "$prev" > "$PAYLOAD_LOG_ARCHIVE"
  ftp_rm "$PAYLOAD_LOG_REMOTE"
}
ftp_rm() { curl -s --max-time 10 -Q "DELE $1" "ftp://$PS5_HOST:$PS5_FTP_PORT/" >/dev/null 2>&1 || true; }
# Worker notes are available only through the console /data shared with GTA by the HEN or mount setup.
game_custom_cat() {
  local path="/data/gtavmenu/custom/$1" text
  text="$(ftp_cat "$path")"
  [ -n "$text" ] && echo "[menu-ctl] read $path" >&2
  printf '%s' "$text"
}
ftp_mkdir() { curl -s --max-time 10 -Q "MKD $1" "ftp://$PS5_HOST:$PS5_FTP_PORT/" >/dev/null 2>&1 || true; }
# Upload a local file ($1) to a remote path ($2) over FTP -- the feature-menu ELF the
# payload loader maps into GTA.
ftp_put() { curl -s --max-time 60 -T "$1" "ftp://$PS5_HOST:$PS5_FTP_PORT$2"; }

# Queue one pack action and wait for its terminal feature event, so the result is not
# lost when heartbeats rotate the 16-slot status ring.
pack_action() {
  local action
  action="$(native_action_id "$1")"
  menu_command activate-action --argument "$action" \
    --wait-feature-action "$2" --feature-wait-timeout 5 --event-interval 0.005
}

# Queue one action with a 32-bit parameter (mailbox command 39) and wait for its feature event.
pack_action_param() {
  local action
  action="$(native_action_id "$1")" || return 1
  # Unsigned hex: bash arithmetic is signed 64-bit, so params >= 0x80000000 would print negative.
  menu_command activate-action-param --argument "$(printf '0x%x' "$(( action | ($3 << 32) ))")" \
    --wait-feature-action "$2" --feature-wait-timeout 5 --event-interval 0.005
}

# Runtime pack lane (CUSTOM_PACKS=1 builds): the pack named by /data/gtavmenu/custom/packs/active.
do_pack_register() {
  [ "$#" -eq 0 ] || { echo "usage: $0 pack-register" >&2; return 2; }
  pack_action REGISTER_PACK register_pack
  echo "[menu-ctl] expect one 'pack registered a=<n> idx=<n> toc=-1 load=1/1' per archive; then: $0 pack-load-archive"
}

# True when an inspect result reports every archive of the pack resident ("pack res=<n>/<n>").
pack_all_resident() {
  local res
  res="$(printf '%s\n' "$1" | sed -n 's/.*pack res=\([0-9]*\/[0-9]*\).*/\1/p' | tail -1)"
  [ -n "$res" ] && [ "${res%/*}" = "${res#*/}" ]
}

# One step for a fresh process: register the active pack, load its archive, wait until it is
# resident, then load + parse every data row in order and request every archetype (typ) row.
do_pack_up() {
  [ "$#" -eq 0 ] || { echo "usage: $0 pack-up" >&2; return 2; }
  local out data typs maps labels row attempt
  out="$(pack_action REGISTER_PACK register_pack 2>&1)"
  printf '%s\n' "$out" | grep -o 'pack cards=[^"]*\|pack store free[^"]*\|action=register_pack message=[^"]*' |
    awk '!seen[$0]++'
  case "$out" in *"feature ok action=register_pack"*) ;; *) return 1 ;; esac
  # The terminal result carries the counts: "pack registered d=<data> t=<typs> m=<maps> l=<labels> a=<archives>".
  data="$(printf '%s\n' "$out" | sed -n 's/.*pack registered d=\([0-9]*\) .*/\1/p' | head -1)"
  typs="$(printf '%s\n' "$out" | sed -n 's/.*pack registered d=[0-9]* t=\([0-9]*\) .*/\1/p' | head -1)"
  maps="$(printf '%s\n' "$out" | sed -n 's/.*pack registered d=[0-9]* t=[0-9]* m=\([0-9]*\) .*/\1/p' | head -1)"
  labels="$(printf '%s\n' "$out" | sed -n 's/.*pack registered d=[0-9]* t=[0-9]* m=[0-9]* l=\([0-9]*\) .*/\1/p' | head -1)"
  [ -n "$data" ] && [ -n "$typs" ] || { echo "[menu-ctl] pack counts not reported" >&2; return 1; }
  pack_action LOAD_PACK_ARCHIVE load_pack_archive 2>&1 | grep -o 'pack archive request[^"]*' | head -1
  for attempt in 1 2 3 4 5 6 7 8 9 10; do
    sleep 1
    out="$(pack_action INSPECT_PACK inspect_pack 2>&1)"
    pack_all_resident "$out" && break
  done
  printf '%s\n' "$out" | grep -o 'pack res=[^"]*\|pack stores[^"]*' | awk '!seen[$0]++'
  pack_all_resident "$out" || { echo "[menu-ctl] pack archives not resident" >&2; return 1; }
  row=0
  while [ "$row" -lt "$data" ]; do
    do_pack_load_data "$row" || return 1
    row=$((row + 1))
  done
  row=0
  while [ "$row" -lt "$typs" ]; do
    pack_action_param LOAD_PACK_TYP load_pack_typ "$row" 2>&1 | grep -o 'pack typ [0-9][^"]*' | head -1
    row=$((row + 1))
  done
  [ "${labels:-0}" -gt 0 ] && do_pack_labels
  echo "[menu-ctl] pack up: $data data rows parsed, $typs typ rows requested, ${labels:-0} labels"
  # Map activation writes engine bookkeeping; it stays an explicit step after the other checks.
  [ "${maps:-0}" -gt 0 ] && echo "[menu-ctl] ${maps} map rows: run $0 pack-load-map <row> when ready"
  return 0
}

do_pack_load_archive() {
  [ "$#" -eq 0 ] || { echo "usage: $0 pack-load-archive" >&2; return 2; }
  pack_action LOAD_PACK_ARCHIVE load_pack_archive
  echo "[menu-ctl] wait ~3 s, then: $0 pack-inspect (archive flags 0x100001, re-parsed TOC n)"
}

do_pack_inspect() {
  [ "$#" -eq 0 ] || { echo "usage: $0 pack-inspect" >&2; return 2; }
  pack_action INSPECT_PACK inspect_pack
}

# Game thread, all or nothing: every override target must be unloaded with no refs.
do_pack_revert() {
  [ "$#" -eq 0 ] || { echo "usage: $0 pack-revert" >&2; return 2; }
  pack_action REVERT_PACK_OVERRIDES revert_pack_overrides
  echo "[menu-ctl] per-row result: $0 pack-notes | grep 'pack revert'"
}

do_pack_request() {
  case "${1:-}" in
    ''|*[!0-9]*) echo "usage: $0 pack-request <card index>" >&2; return 2 ;;
  esac
  [ "$#" -eq 1 ] || { echo "usage: $0 pack-request <card index>" >&2; return 2; }
  pack_action_param REQUEST_PACK_CARD request_pack_card "$1"
  echo "[menu-ctl] then wait ~5 s and run: $0 pack-card-show"
}

do_pack_load_data() {
  case "${1:-}" in
    ''|*[!0-9]*) echo "usage: $0 pack-load-data <data row>" >&2; return 2 ;;
  esac
  [ "$#" -eq 1 ] || { echo "usage: $0 pack-load-data <data row>" >&2; return 2; }
  pack_action_param LOAD_PACK_DATA load_pack_data "$1" || return 1
  # The read completes asynchronously; the pump action refuses ("retry") until it has.
  local attempt out
  for attempt in 1 2 3 4 5 6; do
    sleep 1
    out="$(pack_action PUMP_PACK_DATA pump_pack_data 2>&1)"
    case "$out" in
      *"feature ok action=pump_pack_data"*) printf '%s\n' "$out" | grep -o 'pack data [0-9]* parsed[^"]*' | head -1; return 0 ;;
      *"read pending"*) continue ;;
      *) printf '%s\n' "$out" | grep -o 'action=pump_pack_data[^"]*' | head -2; return 1 ;;
    esac
  done
  echo "[menu-ctl] pack data read never completed" >&2
  return 1
}

do_pack_load_map() {
  case "${1:-}" in
    ''|*[!0-9]*) echo "usage: $0 pack-load-map <map row>" >&2; return 2 ;;
  esac
  [ "$#" -eq 1 ] || { echo "usage: $0 pack-load-map <map row>" >&2; return 2; }
  local out
  out="$(pack_action_param LOAD_PACK_MAP load_pack_map "$1" 2>&1)"
  printf '%s\n' "$out" | grep -o 'pack map [^"]*' | awk '!seen[$0]++'
  case "$out" in *"feature ok action=load_pack_map"*) ;; *) return 1 ;; esac
  do_pack_finish_map "$1"
}

# Request a static collision bounds row keep-resident, then retry until the worker confirms the
# composite root, its box and its physics instances.
do_pack_load_bounds() {
  case "${1:-}" in
    ''|*[!0-9]*) echo "usage: $0 pack-load-bounds <bounds row>" >&2; return 2 ;;
  esac
  [ "$#" -eq 1 ] || { echo "usage: $0 pack-load-bounds <bounds row>" >&2; return 2; }
  local out attempt
  for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    out="$(pack_action_param LOAD_PACK_BOUNDS load_pack_bounds "$1" 2>&1)"
    case "$out" in
      *"pack bounds $1 loaded"*)
        printf '%s\n' "$out" | grep -o 'pack bounds [0-9]* \(loaded\|box\)[^"]*' | awk '!seen[$0]++'
        case "$out" in *"feature ok action=load_pack_bounds"*) return 0 ;; *) return 1 ;; esac ;;
      *"requested status"*|*"load pending"*|*"contents pending"*)
        sleep 1
        continue ;;
      *)
        printf '%s\n' "$out" | grep -o 'action=load_pack_bounds[^"]*' | head -1
        return 1 ;;
    esac
  done
  echo "[menu-ctl] pack bounds never finished loading" >&2
  return 1
}

# Retry finish_pack_map: bounds load -> remove/initialise/reload -> entity confirmation. Status
# events are truncated, so progress is matched on the start of each message.
do_pack_finish_map() {
  case "${1:-}" in
    ''|*[!0-9]*) echo "usage: $0 pack-finish-map <map row>" >&2; return 2 ;;
  esac
  local out attempt
  for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    sleep 1
    out="$(pack_action_param FINISH_PACK_MAP finish_pack_map "$1" 2>&1)"
    case "$out" in
      *"feature ok action=finish_pack_map"*)
        printf '%s\n' "$out" | grep -o 'pack map [0-9]* entities[^"]*\|pack map bounds[^"]*' | awk '!seen[$0]++'
        return 0 ;;
      *"reason=pack map load pending"*|*"reason=pack map contents pending"*|*"reason=pack map entities pending"*|*"reason=pack map [0-9]* initialised"*)
        printf '%s\n' "$out" | grep -o 'reason=pack map [0-9]* initialised[^"]*' | head -1
        continue ;;
      *)
        printf '%s\n' "$out" | grep -o 'pack map bounds[^"]*\|action=finish_pack_map[^"]*' | awk '!seen[$0]++'
        return 1 ;;
    esac
  done
  echo "[menu-ctl] pack map never finished loading" >&2
  return 1
}

do_pack_labels() {
  [ "$#" -eq 0 ] || { echo "usage: $0 pack-labels" >&2; return 2; }
  pack_action ADD_PACK_LABELS add_pack_labels 2>&1 | grep -o 'action=add_pack_labels message=[^"]*' | tail -1
}

do_pack_load_typ() {
  case "${1:-}" in
    ''|*[!0-9]*) echo "usage: $0 pack-load-typ <typ row>" >&2; return 2 ;;
  esac
  [ "$#" -eq 1 ] || { echo "usage: $0 pack-load-typ <typ row>" >&2; return 2; }
  pack_action_param LOAD_PACK_TYP load_pack_typ "$1"
  echo "[menu-ctl] the archetypes register once the .ptyp streams in (a few seconds); then spawn-object"
}

# Spawn an entity by model name (joaat) through ACTION (SPAWN_VEHICLE / SPAWN_OBJECT). The game
# validates the model; the outcome arrives as plain status events, printed for a few seconds.
spawn_by_name() {
  local action_name="$1" name="$2" hash action i addrs block=""
  case "$name" in
    *[!a-z0-9_]*|'') echo "[menu-ctl] model names are lowercase [a-z0-9_]" >&2; return 2 ;;
  esac
  hash="$(PYTHONPATH="$ROOT/tools${PYTHONPATH:+:$PYTHONPATH}" "$PY" -c \
    'import sys; from gtavmenu_tools.hashes import joaat; print(joaat(sys.argv[1]))' "$name")" || return 1
  echo "[menu-ctl] spawn $name hash=$(printf '0x%08x' "$hash")"
  action="$(native_action_id "$action_name")" || return 1
  menu_command activate-action-param --argument "$(printf '0x%x' "$(( action | (hash << 32) ))")" \
    >/dev/null || return 1
  addrs="$(resolved_block_addrs)"
  [ -n "$addrs" ] && block="--address ${addrs##* }"
  for i in 1 2 3 4 5 6; do
    # shellcheck disable=SC2086
    run_client status --target-id "$TARGET_ID" --hook-status-name "$HOOK_STATUS_NAME" $block \
      2>/dev/null | sed -n 's/^ *"message": "\(.*\)",\{0,1\}$/\1/p' |
      grep -v '^worker tick=\|^hook a=' || true
    sleep 1
  done | awk '!seen[$0]++'
}

do_spawn_model() {
  [ "$#" -eq 1 ] || { echo "usage: $0 spawn-model <model name>" >&2; return 2; }
  spawn_by_name SPAWN_VEHICLE "$1"
}

do_spawn_object() {
  [ "$#" -eq 1 ] || { echo "usage: $0 spawn-object <model name>" >&2; return 2; }
  spawn_by_name SPAWN_OBJECT "$1"
}

do_spawn_ped() {
  [ "$#" -eq 1 ] || { echo "usage: $0 spawn-ped <model name>" >&2; return 2; }
  spawn_by_name SPAWN_PED "$1"
}

do_give_weapon() {
  [ "$#" -eq 1 ] || { echo "usage: $0 give-weapon <weapon name>" >&2; return 2; }
  spawn_by_name GIVE_WEAPON "$1"
}

# Upload a built runtime pack (PACK_DIR/resources/pack.cfg and every file it names, verified by
# readback) over FTP. Without a flag nothing is activated: select the pack on the Custom Packs page.
do_pack_upload() {
  local usage="usage: $0 pack-upload PACK_DIR [--activate|--activate-add]"
  case "$#:${2:-}" in
    1:|2:--activate|2:--activate-add) ;;
    *) echo "$usage" >&2; return 2 ;;
  esac
  [ -f "$1/resources/pack.cfg" ] || { echo "[menu-ctl] no pack descriptor: $1/resources/pack.cfg" >&2; return 2; }
  "$PY" tools/upload_runtime_pack.py "$1" ${2:+"$2"} --host "$PS5_HOST" --port "$PS5_FTP_PORT"
}

# Unlist a pack (packs/active first, then packs/installed); --purge also deletes <pack root>/<id>/.
do_pack_uninstall() {
  local usage="usage: $0 pack-uninstall PACK_ID [--purge]"
  case "$#:${2:-}" in
    1:|2:--purge) ;;
    *) echo "$usage" >&2; return 2 ;;
  esac
  "$PY" tools/upload_runtime_pack.py --uninstall "$1" ${2:+"$2"} --host "$PS5_HOST" --port "$PS5_FTP_PORT"
}

# Host-only: check a pack directory before uploading it (no console access).
do_pack_validate() {
  [ "$#" -ge 1 ] || { echo "usage: $0 pack-validate PACK_DIR [--against PACK_DIR ...]" >&2; return 2; }
  "$PY" tools/validate_runtime_pack.py "$@"
}

# Host: fetch the retail templates the PC vehicle converter reads (its --templates cache) from this
# console's game image over FTP (read-only range reads, no game keys), each verified against the
# target's template manifest; rerunning skips verified files. The image is mounted only while GTA V
# runs. --source file:///path/to/app0 reads a local copy of the game files instead.
do_fetch_templates() {
  local usage="usage: $0 fetch-templates [--source ftp://HOST:PORT/mnt/sandbox/<TITLE>_000/app0|file:///path/to/app0] [--cache DIR]"
  local source="" cache="$ROOT/build/retail-templates" manifest="data/retail_templates/$GTAV_TARGET.json"
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --source | --cache)
        [ "$#" -ge 2 ] || { echo "$usage" >&2; return 2; }
        if [ "$1" = --source ]; then source="$2"; else cache="$2"; fi
        shift 2
        ;;
      *) echo "$usage" >&2; return 2 ;;
    esac
  done
  if [ "$GTAV_TARGET" = ppsa04263-01.010.002 ]; then
    manifest="data/retail_templates/ppsa04264-01.010.002.json"
    echo "[menu-ctl] $GTAV_TARGET: checking source against PPSA04264 01.010.002 pins ($manifest)"
  fi
  [ -f "$manifest" ] || { echo "[menu-ctl] no retail template manifest for $GTAV_TARGET ($manifest)" >&2; return 2; }
  if [ -z "$source" ]; then
    require_console_host || return 2
    local app0="/mnt/sandbox/${TARGET_TITLE_ID}_000/app0"
    source="ftp://$PS5_HOST:$PS5_FTP_PORT$app0"
    if ! curl -s --max-time 10 -I "$source/common.rpf" 2>/dev/null | grep -qi '^content-length:'; then
      echo "[menu-ctl] the game image is not mounted at $app0 on $PS5_HOST (it is mounted only while GTA V runs)." >&2
      echo "[menu-ctl]   Start GTA V on the console, then rerun: $0 fetch-templates" >&2
      echo "[menu-ctl]   Or read a local copy of the game files: $0 fetch-templates --source file:///path/to/app0" >&2
      return 1
    fi
  fi
  local -a export_args=()
  # From the console, also pick up what `export-templates` left in the menu's export directory.
  case "$source" in
    ftp://"$PS5_HOST":*) export_args=(--exports "ftp://$PS5_HOST:$PS5_FTP_PORT/data/gtavmenu/custom/exports") ;;
  esac
  "$PY" tools/fetch_retail_templates.py --manifest "$manifest" --source "$source" --cache "$cache" \
    "${export_args[@]}" || return 1
  echo "[menu-ctl] templates ready in $cache: pass --templates $cache to the vehicle converter"
}

# Host: the archetype index of this game (stock archetype -> typ, box, lodDist) that convert-map, convert-mapmod
# and convert-mlo read, built from the game image's retail typs: one verified range read per typ at the offset
# pinned in data/archetype_index/<target>.json (no game keys). Same source rules as fetch-templates.
do_index_archetypes() {
  local usage="usage: $0 index-archetypes [--source ftp://HOST:PORT/mnt/sandbox/<TITLE>_000/app0|file:///path/to/app0] [--output FILE] [--force]"
  local source="" build="${GTAV_HOST_BUILD_DIR:-$ROOT/build}" output=""
  local -a force=()
  case "$build" in /*) ;; *) build="$ROOT/$build" ;; esac
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --source | --output)
        [ "$#" -ge 2 ] || { echo "$usage" >&2; return 2; }
        if [ "$1" = --source ]; then source="$2"; else output="$2"; fi
        shift 2
        ;;
      --force) force=(--force); shift ;;
      *) echo "$usage" >&2; return 2 ;;
    esac
  done
  case "$output" in /* | '') ;; *) output="$CALLER_DIR/$output" ;; esac
  [ -n "$output" ] || output="$build/archetype-index/$GTAV_TARGET.json"
  if [ -z "$source" ]; then
    require_console_host || return 2
    local app0="/mnt/sandbox/${TARGET_TITLE_ID}_000/app0"
    source="ftp://$PS5_HOST:$PS5_FTP_PORT$app0"
    if ! curl -s --max-time 10 -I "$source/common.rpf" 2>/dev/null | grep -qi '^content-length:'; then
      echo "[menu-ctl] the game image is not mounted at $app0 on $PS5_HOST (it is mounted only while GTA V runs)." >&2
      echo "[menu-ctl]   Start GTA V on the console, then rerun: $0 index-archetypes" >&2
      echo "[menu-ctl]   Or read a local copy of the game files: $0 index-archetypes --source file:///path/to/app0" >&2
      return 1
    fi
  fi
  "$PY" -I tools/index_game_archetypes.py --target "$GTAV_TARGET" --source "$source" --output "$output" "${force[@]}" || return 1
  echo "[menu-ctl] archetype index ready: convert-map, convert-mapmod and convert-mlo read $output"
}

# Host: the converter's encrypted templates (retail .meta members the key-free fetch skips) come
# from the menu: EXPORT_RETAIL_FILE (param = row of `fetch_retail_templates.py --exportable`) reads
# one through the running game's update.rpf or common.rpf packfile, which decrypts and inflates it,
# and writes plaintext under the worker's current custom root. Require a new success note and pull
# each actual export into the cache, verified against the manifest even if the cache is warm.
# Needs GTA V running with the menu injected; never deletes an earlier export before replacement.
do_export_templates() {
  local usage="usage: $0 export-templates [--cache DIR]"
  local cache="$ROOT/build/retail-templates" manifest="data/retail_templates/$GTAV_TARGET.json"
  case "$#:${1:-}" in
    0:) ;;
    2:--cache) cache="$2" ;;
    *) echo "$usage" >&2; return 2 ;;
  esac
  # PPSA04263 (EU) runs PPSA04264's code; its export is checked against the PPSA04264 pins.
  if [ ! -f "$manifest" ] && [ "$GTAV_TARGET" = ppsa04263-01.010.002 ]; then
    manifest="data/retail_templates/ppsa04264-01.010.002.json"
    echo "[menu-ctl] $GTAV_TARGET: checking the export against $manifest"
  fi
  [ -f "$manifest" ] || { echo "[menu-ctl] no retail template manifest for $GTAV_TARGET ($manifest)" >&2; return 2; }
  local remote=/data/gtavmenu/custom
  local endpoint="ftp://$PS5_HOST:$PS5_FTP_PORT" notes_path rows line row cache_name name bytes
  local before last_note latest_note notes new_notes tries result pid
  local first_pid="" exports success
  local -a rows_list=()
  rows="$("$PY" tools/fetch_retail_templates.py --manifest "$manifest" --exportable)" || return 1
  [ -n "$rows" ] || { echo "[menu-ctl] the manifest has no encrypted template to export"; return 0; }
  mapfile -t rows_list <<< "$rows"
  for line in "${rows_list[@]}"; do
    read -r row cache_name name bytes <<< "$line"
    # Snapshot the shared /data notes BEFORE the action. An unreadable baseline cannot prove
    # freshness; the action must also confirm that the live worker exported through /data.
    before="$(curl --fail --silent --max-time 10 "$endpoint$remote/pack-notes.log" 2>/dev/null)" || before=""
    result="$(pack_action_param EXPORT_RETAIL_FILE export_retail_file "$row")" || { printf '%s\n' "$result"; return 1; }
    printf '%s\n' "$result"
    pid="$(printf '%s\n' "$result" | "$PY" -c '
import json, sys
try:
    result = json.load(sys.stdin)
    prefix = "feature ok action=export_retail_file message=export queued root="
    message = result["featureActionResult"]["message"]
    root = message[len(prefix):] if message.startswith(prefix) else ""
    pid = result["pid"]
    if result["featureActionSucceeded"] is not True or root != "data" or type(pid) is not int or pid <= 0:
        raise ValueError("live worker export requires shared /data")
    print(pid)
except (KeyError, TypeError, ValueError) as error:
    print(f"[menu-ctl] cannot identify this worker export: {error}; inject the current worker in a fresh process", file=sys.stderr)
    sys.exit(1)
')" || return 1
    if [ -n "$first_pid" ] && [ "$pid" != "$first_pid" ]; then
      echo "[menu-ctl] the worker changed during export; stop and collect a fresh session" >&2
      return 1
    fi
    first_pid="$pid"
    notes_path="$remote/pack-notes.log"
    exports="$endpoint$remote/exports"
    last_note="$(printf '%s\n' "$before" | sed -n 's/^\([0-9][0-9]*\) GTAVMenu pack .*/\1/p' | tail -n 1)"
    if [ -z "$last_note" ]; then
      echo "[menu-ctl] no readable pack-note baseline at $notes_path; export is unverified, rerun after checking pack-notes" >&2
      return 1
    fi
    success=""
    for tries in $(seq 1 30); do
      notes="$(curl --fail --silent --max-time 10 "$endpoint$notes_path" 2>/dev/null)" || notes=""
      latest_note="$(printf '%s\n' "$notes" | sed -n 's/^\([0-9][0-9]*\) GTAVMenu pack .*/\1/p' | tail -n 1)"
      if [ -n "$latest_note" ] && [ "$latest_note" -lt "$last_note" ]; then
        echo "[menu-ctl] pack-note sequence reset during export; stop and collect a fresh session" >&2
        return 1
      fi
      new_notes="$(printf '%s\n' "$notes" | awk -v after="$last_note" '$1 + 0 > after + 0 && / pack export /')"
      if printf '%s\n' "$new_notes" | grep -q " pack export FAILED"; then
        printf '%s\n' "$new_notes" >&2
        echo "[menu-ctl] the menu refused the export of $name (see the line above)" >&2
        return 1
      fi
      success="$(printf '%s\n' "$new_notes" | grep -E " pack export [^ ]+ bytes=$bytes sha=[0-9a-f]{16} row=$row$" || true)"
      [ -n "$success" ] && break
      sleep 1
    done
    if [ -z "$success" ]; then
      echo "[menu-ctl] no fresh success for export row $row ($name) after $tries polls; check $0 pack-notes" >&2
      return 1
    fi
    printf '%s\n' "$success"
    "$PY" tools/fetch_retail_templates.py --manifest "$manifest" --exports "$exports" --refresh-exports \
      --cache "$cache" --only "$cache_name" || return 1
  done
  "$PY" tools/fetch_retail_templates.py --manifest "$manifest" --cache "$cache" --check || true
  echo "[menu-ctl] exported templates are in $cache; the rest come from: $0 fetch-templates"
}

# End-user install: validate the pack, upload it (verified by readback; the uploader re-checks the
# merge with the console's active packs for --activate-add), then name the in-game step.
do_pack_install() {
  require_console_host || return 2
  local usage="usage: $0 pack-install PACK_DIR [--activate|--activate-add]"
  case "$#:${2:-}" in
    1:|2:--activate|2:--activate-add) ;;
    *) echo "$usage" >&2; return 2 ;;
  esac
  [ -f "$1/resources/pack.cfg" ] || { echo "[menu-ctl] no pack descriptor: $1/resources/pack.cfg" >&2; return 2; }
  local report pack_id
  report="$("$PY" tools/validate_runtime_pack.py "$1")" || return 1
  printf '%s\n' "$report"
  pack_id="$(printf '%s\n' "$report" | sed -n 's/.* OK pack=\([^ ]*\).*/\1/p' | head -1)"
  "$PY" tools/upload_runtime_pack.py "$1" ${2:+"$2"} --host "$PS5_HOST" --port "$PS5_FTP_PORT" || return 1
  if [ -n "${2:-}" ]; then
    echo "[menu-ctl] installed and selected $pack_id. In game: Custom Packs -> Load selected packs"
  else
    echo "[menu-ctl] installed $pack_id. In game: Custom Packs -> Manage Packs -> select $pack_id, then Custom Packs -> Load"
  fi
  echo "[menu-ctl] a GTA session loads packs once: restart GTA before loading a different selection"
}

# Host: one command from a PC map mod that places stock models to a validated (and optionally
# installed) pack. The mod is untrusted data: it is copied into a fresh build/convert-map/ID/input
# (symbolic links refused) and read only by tools/convert_ymap.py under `python3 -I`, from the
# repository root. Mods with their own models (custom archetypes) are refused before anything is
# written. GTAV_HOST_BUILD_DIR (default build/) moves the work and pack directories.
do_convert_map() {
  local usage="usage: $0 convert-map MOD --id PACK_ID --template TEMPLATE.pmap [--teleport X,Y,Z[,TEXT]]
       [--archetype-index FILE] [--install|--activate]
  MOD: the mod's dlc.rpf, a .ymap, a CodeWalker .ymap.xml, or an unpacked mod folder"
  local mod="" id="" template="" index="" install="" value x y z text
  local -a places=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --id | --template | --archetype-index | --teleport)
        [ "$#" -ge 2 ] || { echo "$usage" >&2; return 2; }
        value="$2"
        case "$1" in
          --id) id="$value" ;;
          --template) template="$value" ;;
          --archetype-index) index="$value" ;;
          --teleport)
            IFS=, read -r x y z text <<< "$value"
            if [ -z "$text" ] && [ "${z#*:}" != "$z" ]; then text="${z#*:}"; z="${z%%:*}"; fi
            for value in "$x" "$y" "$z"; do
              [[ "$value" =~ ^-?[0-9]+(\.[0-9]+)?$ ]] || {
                echo "[menu-ctl] --teleport takes X,Y,Z or X,Y,Z,TEXT (numbers), not '$2'" >&2
                return 2
              }
            done
            places+=(--place "$x,$y,$z${text:+:$text}")
            ;;
        esac
        shift 2
        ;;
      --install | --activate)
        [ -z "$install" ] || { echo "$usage" >&2; return 2; }
        install="$1"
        shift
        ;;
      -*) echo "$usage" >&2; return 2 ;;
      *)
        [ -z "$mod" ] || { echo "$usage" >&2; return 2; }
        mod="$1"
        shift
        ;;
    esac
  done
  [ -n "$mod" ] && [ -n "$id" ] || { echo "$usage" >&2; return 2; }
  if ! [[ "$id" =~ ^[a-z0-9]([a-z0-9-]*[a-z0-9])?$ ]] || [ "${#id}" -gt 64 ]; then
    echo "[menu-ctl] --id must be lowercase letters, digits and '-' (at most 64), e.g. ymap-garage-v1" >&2
    return 2
  fi
  case "$mod" in /*) ;; *) mod="$CALLER_DIR/$mod" ;; esac
  case "$template" in /* | '') ;; *) template="$CALLER_DIR/$template" ;; esac
  local build="${GTAV_HOST_BUILD_DIR:-$ROOT/build}"
  case "$build" in /*) ;; *) build="$ROOT/$build" ;; esac
  if [ -n "$index" ]; then
    case "$index" in /*) ;; *) index="$CALLER_DIR/$index" ;; esac
    [ -f "$index" ] || { echo "[menu-ctl] no archetype index at $index" >&2; return 2; }
  else
    index="$build/archetype-index/$GTAV_TARGET.json"  # where index-archetypes writes it
  fi
  if [ -z "$template" ] || [ ! -f "$template" ]; then
    echo "[menu-ctl] convert-map needs --template: a one-entity .pmap taken from your own game" >&2
    echo "[menu-ctl]   (the map template tools/make_pmap.py writes every converted map from)" >&2
    return 2
  fi
  if [ -L "$mod" ] || { [ ! -f "$mod" ] && [ ! -d "$mod" ]; }; then
    echo "[menu-ctl] not a mod file or folder (symbolic links are refused): $mod" >&2
    return 2
  fi
  local work="$build/convert-map/$id" packs="$build/custom-assets"
  local input="$work/input" out="$work/maps" pack="$packs/$id"
  # A pack this command built before (its marker is in the work directory) is rebuilt; any other
  # directory of that name is left alone.
  if [ -e "$pack" ] && [ ! -f "$work/pack-built" ]; then
    echo "[menu-ctl] $pack exists and was not made by convert-map: pick another --id or remove it" >&2
    return 2
  fi
  rm -rf -- "$work" "$pack"
  mkdir -p -- "$input" "$packs"
  if [ -d "$mod" ]; then
    cp -RP -- "$mod/." "$input/"
    if [ -n "$(find "$input" -type l -print -quit)" ]; then
      echo "[menu-ctl] the mod folder holds symbolic links; copy the real files into a folder first" >&2
      return 2
    fi
  else
    case "$(printf '%s' "$mod" | tr '[:upper:]' '[:lower:]')" in
      *.rpf | *.ymap | *.ymap.xml) ;;
      *)
        echo "[menu-ctl] convert-map reads a dlc.rpf, .ymap, .ymap.xml or a mod folder: $mod" >&2
        return 2
        ;;
    esac
    cp -P -- "$mod" "$input/"
  fi
  # Maps (.rpf, .ymap, .ymap.xml), plus a folder's loose .ytyp/.ydr/.yft/.ydd: the mod's own models.
  local -a inputs=()
  local has_map="" file
  mapfile -d '' -t inputs < <(find "$input" -type f \( -iname '*.rpf' -o -iname '*.ymap' -o -iname '*.ymap.xml' \
    -o -iname '*.ytyp' -o -iname '*.ydr' -o -iname '*.yft' -o -iname '*.ydd' \) -print0 | sort -z)
  for file in "${inputs[@]}"; do
    case "$(printf '%s' "$file" | tr '[:upper:]' '[:lower:]')" in
      *.rpf | *.ymap | *.ymap.xml) has_map=1 ;;
    esac
  done
  [ -n "$has_map" ] || { echo "[menu-ctl] no .rpf, .ymap or .ymap.xml in $mod" >&2; return 2; }
  # Map and archive names come from the id (unique per pack): gmymap_<id>_N.pmap, gmymap_<id>.rpf.
  local prefix="${id#gtavmenu-}"
  prefix="gmymap_${prefix//-/_}"
  prefix="${prefix:0:41}"
  while [ "${prefix%_}" != "$prefix" ]; do prefix="${prefix%_}"; done
  local -a convert=(--out "$out" --prefix "$prefix" --pack-id "$id" --output-root "$packs" --label "$id"
    --template "$template" --refuse-custom)
  if [ -f "$index" ]; then
    echo "[menu-ctl] archetype index: $index"
    convert+=(--archetype-index "$index")
  else
    echo "[menu-ctl] no archetype index ($index): every entity is kept unchecked;" \
      "check the map in game (build the index: $0 index-archetypes)"
    convert+=(--keep-unresolved)
  fi
  echo "[menu-ctl] converting $(basename -- "$mod") into $pack"
  : > "$work/pack-built"  # from here on $pack is this command's to replace
  if ! "$PY" -I "$ROOT/tools/convert_ymap.py" "${inputs[@]}" "${convert[@]}" "${places[@]}"; then
    echo "[menu-ctl] conversion stopped; nothing was installed (the copied mod: $input)" >&2
    return 1
  fi
  "$PY" "$ROOT/tools/validate_runtime_pack.py" "$pack" || return 1
  echo "[menu-ctl] pack $id: $pack (report: $out/report.json)"
  if [ "$install" = --activate ]; then
    do_pack_install "$pack" --activate
  elif [ -n "$install" ]; then
    do_pack_install "$pack"
  else
    echo "[menu-ctl] install it with: $0 pack-install $pack [--activate]"
  fi
}

# ---- PC mod converters: convert-bounds, -model, -mlo, -ped, -weapon, -vehicle ---------------------------------
# One command each from a PC mod as downloaded to a validated (and optionally installed) pack. Each copies the
# mod into a fresh <build>/convert-<kind>/<id>/input (symbolic links refused; the mod is untrusted data and the
# converters read only that copy, never from the mod's own folder), converts it, builds the pack into
# <build>/custom-assets/<id> with tools/build_runtime_pack.py, checks it with tools/validate_runtime_pack.py and
# installs it like pack-install (--install, --activate) or prints the command that does. <build> is
# GTAV_HOST_BUILD_DIR (default build/). Retail templates come from your own game: the fetch-templates cache
# (<build>/retail-templates, or --templates DIR) or the explicit flags. The bounds, model, map, ped, weapon and wheel
# converters, including vehicle, override and clothing, are in tools/. Model conversion needs numpy;
# stock conversion is limited to the published reviewed catalog. Missing inputs are reported before conversion.

# Include flags of retail world collision; every converted bounds row so far uses them (--include keep: the mod's).
CV_INCLUDE_DEFAULT=0x07f3bec0

# convert_setup COMMAND ID: check ID and set the CV_* paths; refuse a pack directory COMMAND did not build.
convert_setup() {
  if ! [[ "$2" =~ ^[a-z0-9]([a-z0-9-]*[a-z0-9])?$ ]] || [ "${#2}" -gt 64 ]; then
    echo "[menu-ctl] --id must be lowercase letters, digits and '-' (at most 64), e.g. ${1#convert-}-mine-v1" >&2
    return 2
  fi
  CV_ID="$2"
  CV_BUILD="${GTAV_HOST_BUILD_DIR:-$ROOT/build}"
  case "$CV_BUILD" in /*) ;; *) CV_BUILD="$ROOT/$CV_BUILD" ;; esac
  CV_WORK="$CV_BUILD/$1/$2"
  CV_INPUT="$CV_WORK/input"
  CV_PACKS="$CV_BUILD/custom-assets"
  CV_PACK="$CV_PACKS/$2"
  CV_TEMPLATES="$CV_BUILD/retail-templates"
  CV_REFERENCE="$CV_BUILD/assets"
  # Default member and archive names: the id without a gtavmenu- prefix, '-' -> '_'.
  CV_SLUG="${2#gtavmenu-}"
  CV_SLUG="${CV_SLUG//-/_}"
  CV_PLACES=()
  if [ -e "$CV_PACK" ] && [ ! -f "$CV_WORK/pack-built" ]; then
    echo "[menu-ctl] $CV_PACK exists and was not made by $1: pick another --id or remove it" >&2
    return 2
  fi
}

# convert_short NAME MAX: NAME cut to MAX characters, without trailing '_'.
convert_short() {
  local name="${1:0:$2}"
  while [ "${name%_}" != "$name" ]; do name="${name%_}"; done
  printf '%s\n' "$name"
}

# convert_abs PATH: PATH from the caller's directory unless absolute.
convert_abs() {
  case "$1" in /* | '') printf '%s\n' "$1" ;; *) printf '%s\n' "$CALLER_DIR/$1" ;; esac
}

# convert_lower TEXT
convert_lower() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }

# convert_tool NAME COMMAND: CV_TOOL = NAME.py from tools/, else from the developer converters.
convert_tool() {
  CV_TOOL="$ROOT/tools/$1.py"
  [ -f "$CV_TOOL" ] && return 0
  CV_TOOL="$ROOT/research/tools/$1.py"
  [ -f "$CV_TOOL" ] && return 0
  echo "[menu-ctl] $2 needs the developer converter $1.py, which this checkout does not have" \
    "(development checkouts only, not in this release); nothing was written" >&2
  return 2
}

# convert_python COMMAND [PACKAGES]: CV_PY = the interpreter command for converters that need the Python PACKAGES
# (default numpy; all published converters use the same package).
# numpy alone: $PY -I when it imports it (nothing from PYTHONPATH, the user site or the current directory).
# Otherwise $PY -P when it imports them (PYTHONPATH from the environment and the user site count), else $PY -P
# with <build>/assets-venv's site-packages.
convert_python() {
  local site packages="${2:-numpy}" check
  check="import ${packages// /, }"
  if [ "$packages" = numpy ] && "$PY" -I -c "$check" 2> /dev/null; then
    CV_PY=("$PY" -I)
    return 0
  fi
  CV_PY=("$PY" -P)
  "$PY" -c "$check" 2> /dev/null && return 0
  for site in "$CV_BUILD"/assets-venv/lib/python3*/site-packages; do
    if [ -d "$site" ] && PYTHONPATH="$site" "$PY" -c "$check" 2> /dev/null; then
      CV_PY=(env "PYTHONPATH=$site" "$PY" -P)
      return 0
    fi
  done
  if [ "$packages" = numpy ]; then
    echo "[menu-ctl] $1 needs the Python package numpy: install it for $PY (python3 -m pip install numpy)," \
      "or set PYTHONPATH to its site-packages; nothing was written" >&2
  else
    echo "[menu-ctl] $1 needs the Python packages ${packages// / and }: install them, or set PYTHONPATH to" \
      "their site-packages; nothing was written" >&2
  fi
  return 2
}

# convert_template NAME FLAG COMMAND [GIVEN]: CV_FILE = GIVEN (FLAG's file) or <templates>/NAME, else a refusal.
convert_template() {
  CV_FILE="${4:-$CV_TEMPLATES/$1}"
  [ -f "$CV_FILE" ] && return 0
  if [ -n "${4:-}" ]; then
    echo "[menu-ctl] $3: $2 names no file: $CV_FILE" >&2
  else
    echo "[menu-ctl] $3 needs the retail template $1 from your game: run $0 fetch-templates" \
      "(it fills $CV_TEMPLATES; or pass --templates DIR)${2:+, or give $2 FILE}" >&2
  fi
  return 2
}

# Pin omitted retail metadata inputs; explicit XML remains a caller-selected donor.
convert_verified_template() {
  convert_template "$@" || return 2
  [ -n "${4:-}" ] && return 0
  if ! "$PY" "$ROOT/tools/fetch_retail_templates.py" --cache "$CV_TEMPLATES" --only "$1" --check; then
    echo "[menu-ctl] $3: $1 is missing or changed; run export-templates / fetch-templates" >&2
    return 2
  fi
}

# convert_carriers COMMAND [SETS...]: CV_CARRIERS = --shader-template rows for every cached shader carrier of
# SETS (cache folders, in this order; each folder in byte order), the drawables the converted models take their
# PS5 shader schemas from.
convert_carriers() {
  local command="$1" set file
  shift
  CV_CARRIERS=()
  for set in "$@"; do
    while IFS= read -r -d '' file; do
      CV_CARRIERS+=(--shader-template "$file")
    done < <(find "$CV_TEMPLATES/$set" -maxdepth 1 -type f -name '*.pdr' -print0 2> /dev/null | LC_ALL=C sort -z)
  done
  [ "${#CV_CARRIERS[@]}" -gt 0 ] && return 0
  echo "[menu-ctl] $command needs the retail shader carriers ($*) from your game: run $0 fetch-templates" \
    "(it fills $CV_TEMPLATES; or pass --templates DIR), or give --shader-template FILE" >&2
  return 2
}

# convert_place VALUE: append X,Y,Z[,TEXT] (whole metres) as a --place row to CV_PLACES (default text: the id).
convert_place() {
  local x y z text value
  IFS=, read -r x y z text <<< "$1"
  for value in "$x" "$y" "$z"; do
    [[ "$value" =~ ^-?[0-9]+$ ]] || {
      echo "[menu-ctl] --teleport takes X,Y,Z or X,Y,Z,TEXT in whole metres, not '$1'" >&2
      return 2
    }
  done
  CV_PLACES+=(--place "$x,$y,$z:${text:-$(convert_short "$CV_ID" 40)}")
}

# convert_check_source PATH SUFFIXES COMMAND: refuse PATH unless it is a folder or a regular file ending in one
# of SUFFIXES (space-separated, lower case); symbolic links are refused.
convert_check_source() {
  local lower suffix
  if [ -L "$1" ] || { [ ! -f "$1" ] && [ ! -d "$1" ]; }; then
    echo "[menu-ctl] not a mod file or folder (symbolic links are refused): $1" >&2
    return 2
  fi
  [ -d "$1" ] && return 0
  lower="$(convert_lower "$1")"
  for suffix in $2; do
    case "$lower" in *"$suffix") return 0 ;; esac
  done
  echo "[menu-ctl] $3 reads ${2// /, } files or a mod folder, not $1" >&2
  return 2
}

# convert_start: from here on CV_PACK is this command's to replace; a fresh work directory.
convert_start() {
  rm -rf -- "$CV_WORK" "$CV_PACK"
  mkdir -p -- "$CV_INPUT" "$CV_PACKS"
  : > "$CV_WORK/pack-built"
}

# convert_copy SOURCE: copy a mod file or folder to CV_COPY = CV_INPUT/<its name> (symbolic links refused).
convert_copy() {
  CV_COPY="$CV_INPUT/$(basename -- "$1")"
  if [ -e "$CV_COPY" ]; then
    echo "[menu-ctl] two inputs are named $(basename -- "$1"); rename one" >&2
    return 2
  fi
  if [ -d "$1" ]; then
    cp -RP -- "$1" "$CV_COPY"
    if [ -n "$(find "$CV_COPY" -type l -print -quit)" ]; then
      echo "[menu-ctl] the mod folder holds symbolic links; copy the real files into a folder first" >&2
      return 2
    fi
  else
    cp -P -- "$1" "$CV_COPY"
  fi
}

# convert_run LOG COMMAND...: run a conversion step, its output to LOG; on failure show LOG's end and stop.
convert_run() {
  local log="$1"
  shift
  if ! "$@" > "$log" 2>&1; then
    tail -n 20 "$log" >&2
    echo "[menu-ctl] conversion stopped ($(basename -- "$log")); nothing was installed (the copied mod: $CV_INPUT)" >&2
    return 1
  fi
}

# convert_finish COMMAND INSTALL: validate the pack, print its summary, then install it or print the commands.
convert_finish() {
  local file
  "$PY" "$ROOT/tools/validate_runtime_pack.py" "$CV_PACK" || return 1
  echo "[menu-ctl] $1: pack $CV_ID in $CV_PACK (work files and reports: $CV_WORK)"
  for file in "$CV_PACK"/resources/*; do
    echo "[menu-ctl]   $(basename -- "$file")  $(wc -c < "$file") bytes"
  done
  sed -n '3,$s/^/[menu-ctl]   pack.cfg: /p' "$CV_PACK/resources/pack.cfg"
  case "$2" in
    --activate) do_pack_install "$CV_PACK" --activate ;;
    --install) do_pack_install "$CV_PACK" ;;
    *)
      echo "[menu-ctl] upload it as the only selected pack: PS5_HOST=${PS5_HOST:-<console-ip>} $0 pack-install $CV_PACK --activate"
      echo "[menu-ctl] or next to the selected packs:      PS5_HOST=${PS5_HOST:-<console-ip>} $0 pack-install $CV_PACK --activate-add"
      ;;
  esac
}

# Host authoring keeps the helper's refusal to overwrite an existing pack.
do_author_pack() {
  local command="$1" kind="${1#author-}" id="" install="" templates="" input="" value
  shift
  local -a args=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      -h | --help)
        "$PY" -I "$ROOT/tools/author_runtime_pack.py" "$kind" --help
        return
        ;;
      --install | --activate)
        [ -z "$install" ] || { echo "[menu-ctl] choose --install or --activate" >&2; return 2; }
        install="$1"
        shift
        ;;
      --id | --templates | --carrier | --against | --archive | --carrier-name | --meta | --name | --like | \
        --effect | --text | --description | --author | --version)
        [ "$#" -ge 2 ] || { echo "[menu-ctl] $1 needs a value" >&2; return 2; }
        value="$2"
        case "$1" in
          --id) id="$value" ;;
          --templates) templates="$(convert_abs "$value")" ;;
          --carrier | --against) args+=("$1" "$(convert_abs "$value")") ;;
          *) args+=("$1" "$value") ;;
        esac
        shift 2
        ;;
      -*) echo "[menu-ctl] unsupported option $1; use $0 $command --help" >&2; return 2 ;;
      *)
        [ -z "$input" ] || { echo "[menu-ctl] $command accepts one input file" >&2; return 2; }
        input="$(convert_abs "$1")"
        shift
        ;;
    esac
  done
  [ -n "$id" ] || { echo "[menu-ctl] $command requires --id ID; use --help for inputs" >&2; return 2; }
  convert_setup "$command" "$id" || return 2
  [ -z "$input" ] || args=("$input" "${args[@]}")
  mkdir -p "$CV_WORK"
  convert_run "$CV_WORK/author.log" "$PY" -I "$ROOT/tools/author_runtime_pack.py" "$kind" "${args[@]}" \
    --id "$id" --output-root "$CV_PACKS" --templates "${templates:-$CV_TEMPLATES}" || return 1
  cat "$CV_WORK/author.log"
  convert_finish "$command" "$install"
}

# Host: a PC "replace" vehicle mod (a car shipped under a STOCK name: <m>.yft, <m>_hi.yft, <m>.ytd, <m>+hi.ytd,
# often handling.meta) to a stock override pack: convert_vehicle_replace.py checks the override
# gates on the host (stock vehicle member, DLC-overlaid names, HD dictionary, residency advice), converts the files
# with convert_vehicle.py's steps and builds one overlay archive with an `override` row per member plus a vehicle
# spawn row. Its work files go to build/assets/convert-<id> (the converters write only there); --game names a local
# copy of your game (read-only), or --stock-cache a verified cache; reviewed stock members only.
do_convert_replace() {
  local usage="usage: $0 convert-replace MOD --id PACK_ID [--model NAME] [--name TEXT] [--archive NAME.rpf]
       [--repair NAME[=BONE,...] ...] [--game DIR] [--max-texture-size N] [--no-hd-textures] [--templates DIR]
       [--stock-cache DIR] [--stock-manifest FILE] [--target TARGET] [--install|--activate]
  MOD: a PC replace mod: its folder (<model>.yft, <model>_hi.yft, <model>.ytd, <model>+hi.ytd, handling.meta),
       a dlc.rpf holding those files, or an .oiv package
  --repair: exporter-quirk repairs in order (texture-stride, duplicate-bones, fragment-quirks, ...), as for
            convert-vehicle"
  local mod="" id="" install="" model="" name="" archive="" game="" max_texture="" templates="" value tool
  local -a repairs=() extra=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --id | --model | --name | --archive | --repair | --game | --max-texture-size | --templates | \
        --stock-cache | --stock-manifest | --target)
        [ "$#" -ge 2 ] || { echo "$usage" >&2; return 2; }
        value="$2"
        case "$1" in
          --id) id="$value" ;;
          --model) model="$value" ;;
          --name) name="$value" ;;
          --archive) archive="$value" ;;
          --repair) repairs+=(--repair "$value") ;;
          --game) game="$(convert_abs "$value")" ;;
          --stock-cache | --stock-manifest) extra+=("$1" "$(convert_abs "$value")") ;;
          --target) extra+=(--target "$value") ;;
          --max-texture-size) max_texture="$value" ;;
          --templates) templates="$(convert_abs "$value")" ;;
        esac
        shift 2
        ;;
      --no-hd-textures)
        extra+=(--no-hd-textures)
        shift
        ;;
      --install | --activate)
        [ -z "$install" ] || { echo "$usage" >&2; return 2; }
        install="$1"
        shift
        ;;
      -*) echo "$usage" >&2; return 2 ;;
      *)
        [ -z "$mod" ] || { echo "$usage" >&2; return 2; }
        mod="$1"
        shift
        ;;
    esac
  done
  [ -n "$mod" ] && [ -n "$id" ] || { echo "$usage" >&2; return 2; }
  convert_setup convert-replace "$id" || return 2
  CV_TEMPLATES="${templates:-$CV_TEMPLATES}"
  [ -z "$model" ] || [[ "$model" =~ ^[a-z0-9_]{1,40}$ ]] || {
    echo "[menu-ctl] --model takes the stock model name ([a-z0-9_], e.g. police3)" >&2
    return 2
  }
  [ "${#name}" -le 40 ] || { echo "[menu-ctl] --name is at most 40 characters" >&2; return 2; }
  [ -z "$archive" ] || [[ "$archive" =~ ^[a-z0-9_]{1,59}\.rpf$ ]] || {
    echo "[menu-ctl] --archive takes a lowercase NAME.rpf" >&2
    return 2
  }
  [ -z "$max_texture" ] || [[ "$max_texture" =~ ^[1-9][0-9]{0,4}$ ]] || {
    echo "[menu-ctl] --max-texture-size takes a number" >&2
    return 2
  }
  for value in "${repairs[@]}"; do
    [ "$value" = --repair ] || [[ "$value" =~ ^[a-z-]+(=[a-z0-9_]+(,[a-z0-9_]+)*)?$ ]] || {
      echo "[menu-ctl] --repair takes NAME or NAME=BONE,... (e.g. texture-stride, pose-precision=hbgrip_l)" >&2
      return 2
    }
  done
  [ -z "$game" ] || [ -d "$game" ] || { echo "[menu-ctl] --game names no folder: $game" >&2; return 2; }
  convert_tool convert_vehicle_replace convert-replace || return 2
  tool="$CV_TOOL"
  convert_python convert-replace numpy || return 2
  convert_template corpus/tornado6.pft "" convert-replace || return 2
  mod="$(convert_abs "$mod")"
  convert_check_source "$mod" ".rpf .oiv .zip" convert-replace || return 2
  # The converters' own work directory (build/assets/convert-ID): rebuilt only when this command made it.
  local work="$CV_BUILD/assets/convert-$CV_ID"
  if [ -e "$work" ] && [ ! -f "$CV_WORK/pack-built" ]; then
    echo "[menu-ctl] $work exists and was not made by convert-replace: pick another --id or remove it" >&2
    return 2
  fi
  convert_start
  rm -rf -- "$work"
  convert_copy "$mod" || return 2
  [ -z "$model" ] || extra+=(--model "$model")
  [ -z "$name" ] || extra+=(--name "$name")
  [ -z "$archive" ] || extra+=(--archive "$archive")
  [ -z "$game" ] || extra+=(--game "$game")
  [ -z "$max_texture" ] || extra+=(--max-texture-size "$max_texture")
  echo "[menu-ctl] converting the replace mod $(basename -- "$mod") (about a minute)"
  convert_run "$CV_WORK/convert.log" "${CV_PY[@]}" "$tool" --source "$CV_COPY" --id "$CV_ID" \
    --templates "$CV_TEMPLATES" --output-root "$CV_PACKS" "${repairs[@]}" "${extra[@]}" || return 1
  grep -E '^(replace |     (note|residency|handling): )' "$CV_WORK/convert.log" | sed 's/^ */[menu-ctl]   /' || true
  echo "[menu-ctl] report: $work/replace-report.json"
  convert_finish convert-replace "$install"
}

# Host: a PC "replace" mod of any stock asset the override lane takes (textures .ytd, drawables and props .ydr incl.
# _hi, weapons w_*.ydr, ped components .ydd with their .ytd, vehicles .yft) or loose files (--member STOCK=FILE, PC or
# converted PS5 resources) to a stock override pack: convert_override.py finds each file's stock owner
# (--game: the local game copy, read-only; DLC-overlaid names), checks the override gates on the host (stock name,
# folder, shared parents, 16 rows, residency advice by asset class), converts through the existing converters (a
# stock vehicle's .yft goes to convert-replace's converter) and builds one overlay archive with an `override` row per
# member. Work files: build/assets/convert-<id> (override-report.json).
do_convert_override() {
  local usage="usage: $0 convert-override [MOD] --id PACK_ID [--only PATTERN ...] [--member STOCK=FILE ...]
       [--folder NAME] [--archive NAME.rpf] [--name TEXT] [--spawn KIND:MODEL=TEXT | --no-spawn]
       [--shader-template STOCK.pdr|FILE ...] [--like STOCK.pdd=OTHER.pdd ...] [--max-texture-size N]
       [--game DIR] [--templates DIR] [--reference-dir DIR] [--repair NAME ...]
       [--no-hd-textures] [--dry-run]
       [--stock-cache DIR] [--stock-manifest FILE] [--target TARGET] [--install|--activate]
  MOD: a PC replace mod: its folder (files under stock names: <name>.ytd/.ydr/.ydd/.yft, _hi and +hi
       variants), a dlc.rpf holding them, or an .oiv package; optional with --member
  --only: keep the stock names matching PATTERN (e.g. 'uppr_*'); --folder: the ped folder (player_one)
  --member STOCK=FILE: a loose file for a stock member (e.g. w_pi_stungun.pdr=donor.pdr)"
  local mod="" id="" install="" value tool dry="" templates="" reference=""
  local -a extra=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --id | --only | --member | --folder | --archive | --name | --spawn | --shader-template | --like | \
        --max-texture-size | --game | --templates | --reference-dir | --repair | --stock-cache | --stock-manifest | --target)
        [ "$#" -ge 2 ] || { echo "$usage" >&2; return 2; }
        value="$2"
        case "$1" in
          --id) id="$value" ;;
          --only)
            [[ "$value" =~ ^[a-z0-9_+*?/.]{1,80}$ ]] || {
              echo "[menu-ctl] --only takes a stock name or pattern ([a-z0-9_+./] and * ?), e.g. 'uppr_*'" >&2
              return 2
            }
            extra+=(--only "$value")
            ;;
          --member)
            [[ "$value" =~ ^([a-z0-9_]+/)?[a-z0-9_+]+\.(ptd|pdr|pft|pdd)=.+$ ]] || {
              echo "[menu-ctl] --member takes STOCK=FILE (STOCK: [folder/]name.ptd|pdr|pft|pdd)" >&2
              return 2
            }
            extra+=(--member "${value%%=*}=$(convert_abs "${value#*=}")")
            ;;
          --folder)
            [[ "$value" =~ ^[a-z0-9_]{1,63}$ ]] || { echo "[menu-ctl] --folder takes a ped folder name" >&2; return 2; }
            extra+=(--folder "$value")
            ;;
          --archive)
            [[ "$value" =~ ^[a-z0-9_]{1,59}\.rpf$ ]] || {
              echo "[menu-ctl] --archive takes a lowercase NAME.rpf" >&2
              return 2
            }
            extra+=(--archive "$value")
            ;;
          --name)
            [ "${#value}" -le 40 ] || { echo "[menu-ctl] --name is at most 40 characters" >&2; return 2; }
            extra+=(--name "$value")
            ;;
          --spawn) extra+=(--spawn "$value") ;;
          --shader-template)
            if [ -f "$(convert_abs "$value")" ]; then
              extra+=(--shader-template "$(convert_abs "$value")")
            elif [[ "$value" =~ ^[a-z0-9_]+\.(pdr|ydr)$ ]]; then
              extra+=(--shader-template "$value")
            else
              echo "[menu-ctl] --shader-template takes a stock drawable name (name.pdr) or a file" >&2
              return 2
            fi
            ;;
          --like)
            [[ "$value" =~ ^[a-z0-9_/]+\.pdd=[a-z0-9_/]+\.pdd$ ]] || {
              echo "[menu-ctl] --like takes STOCK.pdd=OTHER.pdd" >&2
              return 2
            }
            extra+=(--like "$value")
            ;;
          --max-texture-size)
            [[ "$value" =~ ^(0|[1-9][0-9]{0,4})$ ]] || {
              echo "[menu-ctl] --max-texture-size takes a number (0 keeps every mip)" >&2
              return 2
            }
            extra+=(--max-texture-size "$value")
            ;;
          --stock-cache | --stock-manifest) extra+=("$1" "$(convert_abs "$value")") ;;
          --target) extra+=(--target "$value") ;;
          --game)
            value="$(convert_abs "$value")"
            [ -d "$value" ] || { echo "[menu-ctl] --game names no folder: $value" >&2; return 2; }
            extra+=(--game "$value")
            ;;
          --templates) templates="$(convert_abs "$value")" ;;
          --reference-dir) reference="$(convert_abs "$value")" ;;
          --repair)
            [[ "$value" =~ ^[a-z-]+(=[a-z0-9_]+(,[a-z0-9_]+)*)?$ ]] || {
              echo "[menu-ctl] --repair takes NAME or NAME=BONE,... (vehicle replace mods)" >&2
              return 2
            }
            extra+=(--repair "$value")
            ;;
        esac
        shift 2
        ;;
      --no-spawn | --no-hd-textures)
        extra+=("$1")
        shift
        ;;
      --dry-run)
        dry=1
        shift
        ;;
      --install | --activate)
        [ -z "$install" ] || { echo "$usage" >&2; return 2; }
        install="$1"
        shift
        ;;
      -*) echo "$usage" >&2; return 2 ;;
      *)
        [ -z "$mod" ] || { echo "$usage" >&2; return 2; }
        mod="$1"
        shift
        ;;
    esac
  done
  [ -n "$id" ] || { echo "$usage" >&2; return 2; }
  [ -n "$mod" ] || [[ " ${extra[*]} " == *" --member "* ]] || { echo "$usage" >&2; return 2; }
  [ -z "$dry" ] || [ -z "$install" ] || { echo "[menu-ctl] --dry-run builds nothing to install" >&2; return 2; }
  convert_setup convert-override "$id" || return 2
  CV_TEMPLATES="${templates:-$CV_TEMPLATES}"
  CV_REFERENCE="${reference:-$CV_REFERENCE}"
  if [ -n "$mod" ]; then
    mod="$(convert_abs "$mod")"
    convert_check_source "$mod" ".rpf .oiv .zip" convert-override || return 2
  fi
  convert_tool convert_override convert-override || return 2
  tool="$CV_TOOL"
  convert_python convert-override numpy || return 2
  if [ -n "$dry" ]; then
    "${CV_PY[@]}" "$tool" ${mod:+--source "$mod"} --id "$CV_ID" --templates "$CV_TEMPLATES" \
      --reference-dir "$CV_REFERENCE" --output-root "$CV_PACKS" "${extra[@]}" --dry-run
    return
  fi
  # The converters' own work directory (build/assets/convert-ID): rebuilt only when this command made it.
  local work="$CV_BUILD/assets/convert-$CV_ID"
  if [ -e "$work" ] && [ ! -f "$CV_WORK/pack-built" ]; then
    echo "[menu-ctl] $work exists and was not made by convert-override: pick another --id or remove it" >&2
    return 2
  fi
  convert_start
  rm -rf -- "$work"
  local -a source=()
  if [ -n "$mod" ]; then
    convert_copy "$mod" || return 2
    source=(--source "$CV_COPY")
  fi
  echo "[menu-ctl] converting the replace mod ${mod:+$(basename -- "$mod") }(a minute or two)"
  convert_run "$CV_WORK/convert.log" "${CV_PY[@]}" "$tool" "${source[@]}" --id "$CV_ID" \
    --templates "$CV_TEMPLATES" --reference-dir "$CV_REFERENCE" --output-root "$CV_PACKS" "${extra[@]}" || return 1
  grep -E '^(override|replace|vehicle) |^     (note|residency|rows|handling): ' "$CV_WORK/convert.log" |
    sed 's/^ */[menu-ctl]   /' || true
  echo "[menu-ctl] report: $work/override-report.json"
  convert_finish convert-override "$install"
}

# Host: a PC clothing mod to a pack in one command (convert_pc_clothing.py). Story
# characters (player_zero/one/two): the mod's drawable REPLACES the stock drawable N of the slot (an override pack
# built by convert_override.py; the template whose skeleton fits the mod's rig is picked, a mismatch refused).
# Freemode (mp_m/mp_f_freemode_01): the mod's drawables are ADDED after the retail ones (a SHOP_PED_APPAREL pack:
# patched retail variation file, <ped>_<dlc>/ folder, shop meta). Work files: build/assets/convert-<id>[-clothing].
do_convert_clothing() {
  local usage="usage: $0 convert-clothing MOD --ped PED --slot SLOT --id PACK_ID [--drawable N|STEM[:ENTRY] ...]
       [--from N|STEM[:ENTRY]] [--texture-letter X[=Y][,...] ...] [--archive NAME.rpf] [--dlc NAME]
       [--max-texture-size N] [--game DIR] [--templates DIR] [--reference-dir DIR] [--dry-run]
       [--stock-cache DIR] [--stock-manifest FILE] [--target TARGET] [--install|--activate]
  MOD: the clothing mod (folder, dlc.rpf or .oiv) with <comp>_<nnn>_<u|r>.ydd and its
       <comp>_diff_<nnn>_<x>_<race>.ytd files (or a whole-ped .ydd named as --drawable STEM:ENTRY)
  --ped: player_zero|player_one|player_two (Michael, Franklin, Trevor: replaces the stock --drawable N)
         or mp_m_freemode_01|mp_f_freemode_01 (adds the mod's --drawable(s) as new Wardrobe styles)
  --slot: face mask hair torso legs hands shoes neck undershirt armor decals tops (or uppr, lowr, jbib, ...)
  --game DIR: your game copy (read-only), or use a verified --stock-cache DIR; reviewed names only"
  local mod="" id="" install="" value tool dry="" templates="" reference=""
  local -a extra=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --id | --ped | --slot | --drawable | --from | --texture-letter | --archive | --dlc | --max-texture-size | \
        --game | --templates | --reference-dir | --stock-cache | --stock-manifest | --target)
        [ "$#" -ge 2 ] || { echo "$usage" >&2; return 2; }
        value="$2"
        case "$1" in
          --id) id="$value" ;;
          --ped)
            [[ "$value" =~ ^(player_zero|player_one|player_two|mp_m_freemode_01|mp_f_freemode_01)$ ]] || {
              echo "[menu-ctl] --ped takes player_zero, player_one, player_two, mp_m_freemode_01 or mp_f_freemode_01" >&2
              return 2
            }
            extra+=(--ped "$value")
            ;;
          --slot)
            [[ "$value" =~ ^[a-z]{3,10}$ ]] || { echo "[menu-ctl] --slot takes a Wardrobe slot name (e.g. torso)" >&2; return 2; }
            extra+=(--slot "$value")
            ;;
          --drawable | --from)
            [[ "$value" =~ ^([0-9]{1,3}|[A-Za-z0-9_+-]{1,63}(:[a-z0-9_]{1,40})?)$ ]] || {
              echo "[menu-ctl] $1 takes a drawable number or STEM[:ENTRY] (e.g. 14, or mysuit:head_000_r)" >&2
              return 2
            }
            extra+=("$1" "$value")
            ;;
          --texture-letter)
            [[ "$value" =~ ^[a-z](=[a-z])?(,[a-z](=[a-z])?)*$ ]] || {
              echo "[menu-ctl] --texture-letter takes letters, e.g. a,b or c=a" >&2
              return 2
            }
            extra+=(--texture-letter "$value")
            ;;
          --archive)
            [[ "$value" =~ ^[a-z0-9_]{1,59}\.rpf$ ]] || {
              echo "[menu-ctl] --archive takes a lowercase NAME.rpf" >&2
              return 2
            }
            extra+=(--archive "$value")
            ;;
          --dlc)
            [[ "$value" =~ ^[a-z0-9_]{1,12}$ ]] || { echo "[menu-ctl] --dlc takes [a-z0-9_], at most 12" >&2; return 2; }
            extra+=(--dlc "$value")
            ;;
          --max-texture-size)
            [[ "$value" =~ ^(0|[1-9][0-9]{0,4})$ ]] || {
              echo "[menu-ctl] --max-texture-size takes a number (0 keeps every mip)" >&2
              return 2
            }
            extra+=(--max-texture-size "$value")
            ;;
          --stock-cache | --stock-manifest) extra+=("$1" "$(convert_abs "$value")") ;;
          --target) extra+=(--target "$value") ;;
          --game)
            value="$(convert_abs "$value")"
            [ -d "$value" ] || { echo "[menu-ctl] --game names no folder: $value" >&2; return 2; }
            extra+=(--game "$value")
            ;;
          --templates) templates="$(convert_abs "$value")" ;;
          --reference-dir) reference="$(convert_abs "$value")" ;;
        esac
        shift 2
        ;;
      --dry-run)
        dry=1
        shift
        ;;
      --install | --activate)
        [ -z "$install" ] || { echo "$usage" >&2; return 2; }
        install="$1"
        shift
        ;;
      -*) echo "$usage" >&2; return 2 ;;
      *)
        [ -z "$mod" ] || { echo "$usage" >&2; return 2; }
        mod="$1"
        shift
        ;;
    esac
  done
  [ -n "$id" ] && [ -n "$mod" ] || { echo "$usage" >&2; return 2; }
  [[ " ${extra[*]} " == *" --ped "* ]] && [[ " ${extra[*]} " == *" --slot "* ]] || { echo "$usage" >&2; return 2; }
  [ -z "$dry" ] || [ -z "$install" ] || { echo "[menu-ctl] --dry-run builds nothing to install" >&2; return 2; }
  convert_setup convert-clothing "$id" || return 2
  CV_TEMPLATES="${templates:-$CV_TEMPLATES}"
  CV_REFERENCE="${reference:-$CV_REFERENCE}"
  mod="$(convert_abs "$mod")"
  convert_check_source "$mod" ".rpf .oiv .zip" convert-clothing || return 2
  convert_tool convert_pc_clothing convert-clothing || return 2
  tool="$CV_TOOL"
  convert_python convert-clothing numpy || return 2
  if [ -n "$dry" ]; then
    "${CV_PY[@]}" "$tool" --source "$mod" --id "$CV_ID" --templates "$CV_TEMPLATES" \
      --reference-dir "$CV_REFERENCE" --output-root "$CV_PACKS" "${extra[@]}" --dry-run
    return
  fi
  # The converters' work directories (build/assets/convert-ID and -clothing): rebuilt only when this command made them.
  local work="$CV_BUILD/assets/convert-$CV_ID"
  if { [ -e "$work" ] || [ -e "$work-clothing" ]; } && [ ! -f "$CV_WORK/pack-built" ]; then
    echo "[menu-ctl] $work exists and was not made by convert-clothing: pick another --id or remove it" >&2
    return 2
  fi
  convert_start
  rm -rf -- "$work" "$work-clothing"
  convert_copy "$mod" || return 2
  echo "[menu-ctl] converting the clothing mod $(basename -- "$mod") (a minute or two)"
  convert_run "$CV_WORK/convert.log" "${CV_PY[@]}" "$tool" --source "$CV_COPY" --id "$CV_ID" \
    --templates "$CV_TEMPLATES" --reference-dir "$CV_REFERENCE" --output-root "$CV_PACKS" "${extra[@]}" || return 1
  grep -E '^(clothing|override|variations|in game): |^     (note|residency|rows): |^     [a-z]{4}_[0-9]{3}_u <- ' \
    "$CV_WORK/convert.log" | sed 's/^ */[menu-ctl]   /' || true
  convert_finish convert-clothing "$install"
}

# convert_bound_templates COMMAND: CV_BOUND_TEMPLATES = --template rows for the cached retail bounds
# (<templates>/bounds-templates: a static .pbn and a prop drawable), which supply the PS5 class tag of each bound
# type the converted rows use; the bounds converter ships none.
convert_bound_templates() {
  local file
  CV_BOUND_TEMPLATES=()
  while IFS= read -r -d '' file; do
    CV_BOUND_TEMPLATES+=(--template "$file")
  done < <(find "$CV_TEMPLATES/bounds-templates" -maxdepth 1 -type f \( -name '*.pbn' -o -name '*.pdr' \) -print0 \
    2> /dev/null | LC_ALL=C sort -z)
  [ "${#CV_BOUND_TEMPLATES[@]}" -gt 0 ] && return 0
  echo "[menu-ctl] $1 needs the retail bounds templates (bounds-templates/) from your game: run $0 fetch-templates" \
    "(it fills $CV_TEMPLATES; or pass --templates DIR); nothing was written" >&2
  return 2
}

# Host: PC static collision (.ybn) to bounds rows (.pbn). tools/convert_pc_bounds.py re-packs each file the
# retail way (one row per file, or --merge: one row for all), class tags from the cached retail bounds templates;
# the boxes stay where the mod put them unless --translate moves them. A mod's dlc.rpf is read by
# tools/extract_mod_files.py.
do_convert_bounds() {
  local usage="usage: $0 convert-bounds MOD... --id PACK_ID [--only NAME ...] [--merge] [--translate DX,DY,DZ]
       [--include MASK|keep] [--name NAME=MEMBER ...] [--archive NAME.rpf] [--teleport X,Y,Z[,TEXT]]
       [--templates DIR] [--install|--activate]
  MOD: a PC collision file (.ybn), the mod's dlc.rpf or a mod folder (their .ybn files)"
  local id="" install="" merge="" translate="" include="$CV_INCLUDE_DEFAULT" archive="" templates="" value mod tool
  local -a mods=() only=() names=() teleports=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --id | --only | --translate | --include | --name | --archive | --teleport | --templates)
        [ "$#" -ge 2 ] || { echo "$usage" >&2; return 2; }
        case "$1" in
          --templates) templates="$(convert_abs "$2")" ;;
          --id) id="$2" ;;
          --only) only+=("$2") ;;
          --translate) translate="$2" ;;
          --include) include="$2" ;;
          --name) names+=("$2") ;;
          --archive) archive="$2" ;;
          --teleport) teleports+=("$2") ;;
        esac
        shift 2
        ;;
      --merge)
        merge=1
        shift
        ;;
      --install | --activate)
        [ -z "$install" ] || { echo "$usage" >&2; return 2; }
        install="$1"
        shift
        ;;
      -*) echo "$usage" >&2; return 2 ;;
      *)
        mods+=("$1")
        shift
        ;;
    esac
  done
  [ "${#mods[@]}" -gt 0 ] && [ -n "$id" ] || { echo "$usage" >&2; return 2; }
  convert_setup convert-bounds "$id" || return 2
  CV_TEMPLATES="${templates:-$CV_TEMPLATES}"
  for value in "${teleports[@]}"; do convert_place "$value" || return 2; done
  local -a moves=()
  if [ -n "$translate" ]; then
    IFS=, read -r -a moves <<< "$translate"
    [ "${#moves[@]}" -eq 3 ] || moves=()
    for value in "${moves[@]}"; do [[ "$value" =~ ^-?[0-9]+(\.[0-9]+)?$ ]] || moves=(); done
    [ "${#moves[@]}" -eq 3 ] || {
      echo "[menu-ctl] --translate takes DX,DY,DZ in metres, not '$translate'" >&2
      return 2
    }
  fi
  [[ "$include" =~ ^(keep|0x[0-9a-fA-F]{1,8})$ ]] || {
    echo "[menu-ctl] --include takes a hex mask (default $CV_INCLUDE_DEFAULT) or keep, not '$include'" >&2
    return 2
  }
  local prefix
  prefix="$(convert_short "gmcol_$CV_SLUG" 24)"
  archive="${archive:-$prefix.rpf}"
  [[ "$archive" =~ ^[a-z0-9_]{1,59}\.rpf$ ]] || {
    echo "[menu-ctl] --archive must be a lowercase NAME.rpf ([a-z0-9_]), not '$archive'" >&2
    return 2
  }
  local -A rename=()
  for value in "${names[@]}"; do
    [[ "$value" =~ ^[^=]+=[a-z0-9_]{1,59}(\.pbn)?$ ]] && [ -z "$merge" ] || {
      echo "[menu-ctl] --name takes NAME=MEMBER (a .ybn base name, a [a-z0-9_] member name; not with --merge)" >&2
      return 2
    }
    mod="$(convert_lower "${value%%=*}")"
    rename["${mod%.ybn}"]="${value#*=}"
  done
  convert_tool convert_pc_bounds convert-bounds || return 2
  tool="$CV_TOOL"
  for mod in "${mods[@]}"; do
    convert_check_source "$(convert_abs "$mod")" ".ybn .rpf" convert-bounds || return 2
  done
  convert_bound_templates convert-bounds || return 2

  convert_start
  local -a files=() hits=() order=() conv=() rows=()
  local file stem n=0
  for mod in "${mods[@]}"; do
    convert_copy "$(convert_abs "$mod")" || return 2
    if [ -d "$CV_COPY" ]; then
      mapfile -d '' -t hits < <(find "$CV_COPY" -type f -iname '*.ybn' -print0 | LC_ALL=C sort -z)
    elif [[ "$(convert_lower "$CV_COPY")" == *.rpf ]]; then
      n=$((n + 1))
      convert_run "$CV_WORK/extract-$n.log" "$PY" -I "$ROOT/tools/extract_mod_files.py" "$CV_COPY" --suffix .ybn \
        --out "$CV_WORK/ybn/$n" || return 1
      mapfile -d '' -t hits < <(find "$CV_WORK/ybn/$n" -type f -name '*.ybn' -print0 | LC_ALL=C sort -z)
    else
      hits=("$CV_COPY")
    fi
    files+=("${hits[@]}")
  done
  local -A by_stem=()
  for file in "${files[@]}"; do
    stem="$(convert_lower "$(basename -- "$file")")"
    stem="${stem%.ybn}"
    [ -z "${by_stem[$stem]+x}" ] || { echo "[menu-ctl] two .ybn files are named $stem" >&2; return 2; }
    by_stem[$stem]="$file"
    order+=("$stem")
  done
  if [ "${#only[@]}" -gt 0 ]; then
    order=()
    for stem in "${only[@]}"; do
      stem="$(convert_lower "$stem")"
      stem="${stem%.ybn}"
      [ -n "${by_stem[$stem]+x}" ] || {
        echo "[menu-ctl] --only $stem: no such .ybn in the mod (it has: ${!by_stem[*]})" >&2
        return 2
      }
      order+=("$stem")
    done
  fi
  [ "${#order[@]}" -gt 0 ] || { echo "[menu-ctl] no .ybn collision file in the mod" >&2; return 2; }
  for stem in "${!rename[@]}"; do
    [[ " ${order[*]} " == *" $stem "* ]] || { echo "[menu-ctl] --name $stem: not one of the converted files" >&2; return 2; }
  done
  if [ -z "$merge" ] && [ "${#order[@]}" -gt 8 ]; then
    echo "[menu-ctl] the mod has ${#order[@]} collision files and a pack holds at most 8 bounds rows:" \
      "pick up to 8 with --only NAME, or --merge them into one row (${order[*]})" >&2
    return 2
  fi
  [ "${#moves[@]}" -eq 0 ] || conv+=(--translate "${moves[@]}")
  [ "$include" = keep ] || conv+=(--include "$include")
  mkdir -p -- "$CV_WORK/bounds"
  local member out box
  local -A taken=()
  if [ -n "$merge" ]; then
    member="$prefix"
    out="$CV_WORK/bounds/$member"
    local -a merged=()
    for stem in "${order[@]}"; do merged+=("${by_stem[$stem]}"); done
    convert_run "$out.log" "$PY" -I "$tool" merge "${merged[@]}" --output "$out.pbn" --report "$out.json" \
      "${CV_BOUND_TEMPLATES[@]}" "${conv[@]}" || return 1
    rows+=(--member "$member.pbn=$out.pbn" --bounds "$member.pbn")
    echo "[menu-ctl] $member.pbn <- ${#merged[@]} merged file(s): $(wc -c < "$out.pbn") bytes"
  else
    for stem in "${order[@]}"; do
      member="${rename[$stem]:-$(convert_short "${prefix}_${stem//[^a-z0-9_]/_}" 59)}"
      member="${member%.pbn}"
      [ -z "${taken[$member]+x}" ] || { echo "[menu-ctl] two rows would be named $member: use --name" >&2; return 2; }
      taken[$member]=1
      out="$CV_WORK/bounds/$member"
      convert_run "$out.log" "$PY" -I "$tool" convert "${by_stem[$stem]}" --output "$out.pbn" \
        --report "$out.json" "${CV_BOUND_TEMPLATES[@]}" "${conv[@]}" || return 1
      rows+=(--member "$member.pbn=$out.pbn" --bounds "$member.pbn")
      box="$(sed -n 's/^composite .* box=\(([^)]*)-([^)]*)\).*/\1/p' "$out.log" | head -n 1)"
      echo "[menu-ctl] $member.pbn <- $stem.ybn: $(wc -c < "$out.pbn") bytes${box:+, box $box}"
    done
  fi
  convert_run "$CV_WORK/build.log" "$PY" "$ROOT/tools/build_runtime_pack.py" --id "$CV_ID" --archive "$archive" \
    --output-root "$CV_PACKS" "${rows[@]}" "${CV_PLACES[@]}" || return 1
  [ "${#CV_PLACES[@]}" -gt 0 ] ||
    echo "[menu-ctl] no --teleport: collision is invisible; add a teleport row above a box listed here to find it"
  convert_finish convert-bounds "$install"
}

# Host: a PC mod's own models (props, or a map mod's buildings) to a pack typ + .pdr + .ptd members.
# tools/convert_pc_map_models.py converts each archetype's .ydr onto a retail building drawable's root
# words (shader schemas from the cached retail shader carriers), its textures into one .ptd per texture dictionary
# and the mod's .ytyp definitions into one pack typ; the models get no collision (convert-bounds converts the
# mod's .ybn). Without --maps each model is a Custom Packs spawn row (object); with --maps the mod's placements
# come along (tools/convert_ymap.py, as convert-map, with these models as pack archetypes).
do_convert_model() {
  local usage="usage: $0 convert-model MOD --id PACK_ID [--archetype NAME ...] [--typ-name NAME] [--archive NAME.rpf]
       [--maps [--template T.pmap] [--archetype-index FILE] [--map-prefix NAME] [--max-entities N]
               [--max-retail-typs N]]
       [--rename-txd OLD=NEW ...] [--no-lights] [--max-texture-size N] [--teleport X,Y,Z[,TEXT]]
       [--drawable-template F.pdr] [--shader-template F.pdr ...] [--typ-template F.ptyp]
       [--templates DIR] [--reference-dir DIR] [--install|--activate]
  MOD: the mod's dlc.rpf, or an unpacked mod folder (its .ytyp, .ydr and .ytd files)"
  local mod="" id="" install="" typ="" archive="" maps="" pmap="" index="" max_entities=300 max_typs=15 prefix=""
  local lights="" max_texture=1024 root_pdr="" typ_template="" templates="" reference="" value
  local -a archetypes=() renames=() shaders=() teleports=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --id | --archetype | --typ-name | --archive | --template | --archetype-index | --map-prefix | --max-entities | \
        --max-retail-typs | --rename-txd | --max-texture-size | --teleport | --drawable-template | \
        --shader-template | --typ-template | --templates | --reference-dir)
        [ "$#" -ge 2 ] || { echo "$usage" >&2; return 2; }
        value="$2"
        case "$1" in
          --id) id="$value" ;;
          --archetype) archetypes+=("$(convert_lower "$value")") ;;
          --typ-name) typ="$value" ;;
          --archive) archive="$value" ;;
          --template) pmap="$(convert_abs "$value")" ;;
          --archetype-index) index="$(convert_abs "$value")" ;;
          --map-prefix) prefix="$value" ;;
          --max-entities) max_entities="$value" ;;
          --max-retail-typs) max_typs="$value" ;;
          --rename-txd) renames+=(--rename-txd "$value") ;;
          --max-texture-size) max_texture="$value" ;;
          --teleport) teleports+=("$value") ;;
          --drawable-template) root_pdr="$(convert_abs "$value")" ;;
          --shader-template) shaders+=(--shader-template "$(convert_abs "$value")") ;;
          --typ-template) typ_template="$(convert_abs "$value")" ;;
          --templates) templates="$(convert_abs "$value")" ;;
          --reference-dir) reference="$(convert_abs "$value")" ;;
        esac
        shift 2
        ;;
      --maps | --no-lights)
        if [ "$1" = --maps ]; then maps=1; else lights=--no-lights; fi
        shift
        ;;
      --install | --activate)
        [ -z "$install" ] || { echo "$usage" >&2; return 2; }
        install="$1"
        shift
        ;;
      -*) echo "$usage" >&2; return 2 ;;
      *)
        [ -z "$mod" ] || { echo "$usage" >&2; return 2; }
        mod="$1"
        shift
        ;;
    esac
  done
  [ -n "$mod" ] && [ -n "$id" ] || { echo "$usage" >&2; return 2; }
  convert_setup convert-model "$id" || return 2
  CV_TEMPLATES="${templates:-$CV_TEMPLATES}"
  CV_REFERENCE="${reference:-$CV_REFERENCE}"
  for value in "${teleports[@]}"; do convert_place "$value" || return 2; done
  for value in "$max_entities" "$max_typs" "$max_texture"; do
    [[ "$value" =~ ^[1-9][0-9]{0,4}$ ]] || { echo "[menu-ctl] expected a positive number, not '$value'" >&2; return 2; }
  done
  [ "$max_typs" -le 15 ] || { echo "[menu-ctl] --max-retail-typs is at most 15 (the pack typ takes one of 16 rows)" >&2; return 2; }
  typ="${typ:-$(convert_short "gm_$CV_SLUG" 40)}"
  archive="${archive:-$(convert_short "gmmdl_$CV_SLUG" 59).rpf}"
  # Map names: <prefix>_N.pmap (unique per pack, as convert-map).
  prefix="${prefix:-$(convert_short "gmymap_$CV_SLUG" 41)}"
  [[ "$typ" =~ ^[a-z0-9_]{1,59}$ ]] && [[ "$archive" =~ ^[a-z0-9_]{1,59}\.rpf$ ]] && [[ "$prefix" =~ ^[a-z0-9_]{1,41}$ ]] || {
    echo "[menu-ctl] --typ-name, --map-prefix and --archive take lowercase [a-z0-9_] names (NAME, NAME.rpf)" >&2
    return 2
  }
  for value in "${archetypes[@]}"; do
    [[ "$value" =~ ^[a-z0-9_]{1,63}$ ]] || { echo "[menu-ctl] --archetype takes a model name ([a-z0-9_]), not '$value'" >&2; return 2; }
  done
  if [ -n "$index" ]; then
    [ -f "$index" ] || { echo "[menu-ctl] no archetype index at $index" >&2; return 2; }
  else
    index="$CV_BUILD/archetype-index/$GTAV_TARGET.json" # where index-archetypes writes it; optional
  fi
  local models_tool
  convert_tool convert_pc_map_models convert-model || return 2
  models_tool="$CV_TOOL"
  convert_python convert-model numpy || return 2
  if [ -n "$maps" ]; then
    convert_template map-templates/hei_dt1_02_impexpemproxy_c.pmap --template convert-model "$pmap" || return 2
    pmap="$CV_FILE"
  fi
  convert_template shader-carriers/ch3_03_ss_cb.pdr --drawable-template convert-model "$root_pdr" || return 2
  root_pdr="$CV_FILE"
  convert_template typ-templates/v_int_22.ptyp --typ-template convert-model "$typ_template" || return 2
  typ_template="$CV_FILE"
  if [ "${#shaders[@]}" -eq 0 ]; then
    convert_carriers convert-model shader-carriers shader-carriers-interior || return 2
    shaders=("${CV_CARRIERS[@]}")
  fi
  mod="$(convert_abs "$mod")"
  convert_check_source "$mod" ".rpf" convert-model || return 2

  convert_start
  convert_copy "$mod" || return 2
  local copy="$CV_COPY" models="$CV_WORK/models"
  # Map inputs for convert_ymap: the archive, or a folder's maps and model files (as convert-map).
  local -a inputs=() select=()
  local has_maps=""
  if [ -d "$copy" ]; then
    mapfile -d '' -t inputs < <(find "$copy" -type f \( -iname '*.ymap' -o -iname '*.ymap.xml' -o -iname '*.ytyp' \
      -o -iname '*.ydr' -o -iname '*.yft' -o -iname '*.ydd' \) -print0 | LC_ALL=C sort -z)
    [ -z "$(find "$copy" -type f \( -iname '*.ymap' -o -iname '*.ymap.xml' \) -print -quit)" ] || has_maps=1
  else
    inputs=("$copy")
    has_maps=1
  fi
  if [ -n "$maps" ] && [ -z "$has_maps" ]; then
    echo "[menu-ctl] --maps: the mod folder has no .ymap or .ymap.xml" >&2
    return 2
  fi
  local -a classify=()
  [ ! -f "$index" ] || classify=(--archetype-index "$index")
  if [ "${#archetypes[@]}" -gt 0 ]; then
    for value in "${archetypes[@]}"; do select+=(--archetype "$value"); done
  elif [ -n "$has_maps" ]; then
    # The mod's own models its maps place: tools/convert_ymap.py classes them "custom".
    convert_run "$CV_WORK/classify.log" "$PY" -I "$ROOT/tools/convert_ymap.py" "${inputs[@]}" "${classify[@]}" \
      --out "$CV_WORK/classify" --prefix "$prefix" || return 1
    select=(--report "$CV_WORK/classify/report.json")
  else
    while IFS= read -r -d '' value; do
      value="$(convert_lower "$(basename -- "$value")")"
      select+=(--archetype "${value%.ydr}")
    done < <(find "$copy" -type f -iname '*.ydr' -print0 | LC_ALL=C sort -z)
    [ "${#select[@]}" -gt 0 ] || { echo "[menu-ctl] no .ydr model in the mod folder" >&2; return 2; }
  fi
  echo "[menu-ctl] converting the models of $(basename -- "$mod") (this can take minutes)"
  convert_run "$CV_WORK/models.log" "${CV_PY[@]}" "$models_tool" "$copy" "${select[@]}" --template "$root_pdr" \
    "${shaders[@]}" --typ-template "$typ_template" --typ-name "$typ" --reference-dir "$CV_REFERENCE" \
    --templates "$CV_TEMPLATES" --max-texture-size "$max_texture" "${renames[@]}" ${lights:+"$lights"} \
    --out "$models" || {
    grep -q 'no archetype to convert' "$CV_WORK/models.log" &&
      echo "[menu-ctl] the mod's maps place none of its own models: name them with --archetype NAME" >&2
    return 1
  }
  grep -E '^(refused|note):|archetypes converted' "$CV_WORK/models.log" | sed 's/^/[menu-ctl]   /'
  if [ -n "$maps" ]; then
    local -a mapargs=(--pack-archetypes "$models/pack-archetypes.json" --template "$pmap" --out "$CV_WORK/maps"
      --prefix "$prefix" --max-entities "$max_entities" --max-retail-typs "$max_typs" --pack-id "$CV_ID"
      --archive "$archive" --output-root "$CV_PACKS")
    if [ -f "$index" ]; then
      mapargs=(--archetype-index "$index" "${mapargs[@]}")
    else
      echo "[menu-ctl] no archetype index ($index): every stock entity is kept unchecked; check the map in game" \
        "(build the index: $0 index-archetypes)"
      mapargs+=(--keep-unresolved)
    fi
    [ "${#CV_PLACES[@]}" -gt 0 ] || mapargs+=(--label "$(convert_short "$CV_ID" 40)")
    local place
    for place in "${CV_PLACES[@]}"; do [ "$place" = --place ] || mapargs+=(--place "$place"); done
    convert_run "$CV_WORK/maps.log" "$PY" -I "$ROOT/tools/convert_ymap.py" "${inputs[@]}" "${mapargs[@]}" || return 1
    grep -E 'entities converted|archetypes|typ rows' "$CV_WORK/maps.log" | sed 's/^/[menu-ctl]   /'
  else
    local -a rows=()
    mapfile -d '' -t rows < <("$PY" -I -c 'import json, sys
t = json.load(open(sys.argv[1], encoding="utf-8"))
out = []
for member, path in sorted(t["members"].items()):
    out += ["--member", member + "=" + sys.argv[2] + "/" + path]
out += ["--typ", t["typ"] + ".ptyp"]
names = sorted(t["names"].values())
for name in names[:24]:
    out += ["--spawn", "object:" + name + "=" + name[:40]]
if len(names) > 24:
    print("[menu-ctl] " + str(len(names) - 24) + " models have no spawn row (24 per pack)", file=sys.stderr)
sys.stdout.write("\0".join(out) + "\0")' "$models/pack-archetypes.json" "$models")
    [ "${#rows[@]}" -gt 0 ] || { echo "[menu-ctl] no converted model to pack" >&2; return 1; }
    convert_run "$CV_WORK/build.log" "$PY" "$ROOT/tools/build_runtime_pack.py" --id "$CV_ID" --archive "$archive" \
      --output-root "$CV_PACKS" "${rows[@]}" "${CV_PLACES[@]}" || return 1
    echo "[menu-ctl] the models have no collision; spawn them from Custom Packs (Spawn) after Load"
  fi
  convert_finish convert-model "$install"
}

# Host: a PC map mod's own interiors (MLO archetype + its placement) to a pack typ + interior maps. The converter
# (convert_pc_mlo.py: tools/make_mlo_ptyp.py and tools/make_addon_mlo_pmap.py on the mod's .ytyp and .ymap)
# keeps rooms, portals and the stock props the archetype index knows; --models also converts the interior's
# own models (convert-model's converter), --bounds the MLO's own collision (<mlo>.ybn, convert-bounds' converter).
do_convert_mlo() {
  local usage="usage: $0 convert-mlo MOD --id PACK_ID --mlo NAME[=MAP] ... [--typ-name NAME] [--archetype-index FILE]
       [--max-retail-typs N] [--models] [--bounds] [--no-entities] [--rename-txd OLD=NEW ...]
       [--archive NAME.rpf] [--teleport X,Y,Z[,TEXT]] [--map-teleport MAP=X,Y,Z[,TEXT] ...]
       [--templates DIR] [--reference-dir DIR] [--install|--activate]
  MOD: the mod's dlc.rpf (its .ytyp defines the MLO, its .ymap places it)"
  local mod="" id="" install="" typ="" archive="" index="" max_typs="" models_on="" bounds_on="" no_entities=""
  local templates="" reference="" value
  local -a mlos=() renames=() teleports=() map_teleports=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --id | --mlo | --typ-name | --archetype-index | --max-retail-typs | --rename-txd | --archive | --teleport | \
        --map-teleport | --templates | --reference-dir)
        [ "$#" -ge 2 ] || { echo "$usage" >&2; return 2; }
        value="$2"
        case "$1" in
          --id) id="$value" ;;
          --mlo) mlos+=("$(convert_lower "$value")") ;;
          --typ-name) typ="$value" ;;
          --archetype-index) index="$(convert_abs "$value")" ;;
          --max-retail-typs) max_typs="$value" ;;
          --rename-txd) renames+=(--rename-txd "$value") ;;
          --archive) archive="$value" ;;
          --teleport) teleports+=("$value") ;;
          --map-teleport) map_teleports+=("$value") ;;
          --templates) templates="$(convert_abs "$value")" ;;
          --reference-dir) reference="$(convert_abs "$value")" ;;
        esac
        shift 2
        ;;
      --models | --bounds | --no-entities)
        case "$1" in --models) models_on=1 ;; --bounds) bounds_on=1 ;; *) no_entities=--no-entities ;; esac
        shift
        ;;
      --install | --activate)
        [ -z "$install" ] || { echo "$usage" >&2; return 2; }
        install="$1"
        shift
        ;;
      -*) echo "$usage" >&2; return 2 ;;
      *)
        [ -z "$mod" ] || { echo "$usage" >&2; return 2; }
        mod="$1"
        shift
        ;;
    esac
  done
  [ -n "$mod" ] && [ -n "$id" ] && [ "${#mlos[@]}" -gt 0 ] || { echo "$usage" >&2; return 2; }
  convert_setup convert-mlo "$id" || return 2
  CV_TEMPLATES="${templates:-$CV_TEMPLATES}"
  CV_REFERENCE="${reference:-$CV_REFERENCE}"
  for value in "${teleports[@]}"; do convert_place "$value" || return 2; done
  typ="${typ:-$(convert_short "gm_$CV_SLUG" 40)}"
  archive="${archive:-$(convert_short "gmmlo_$CV_SLUG" 59).rpf}"
  [[ "$typ" =~ ^[a-z0-9_]{1,58}$ ]] && [[ "$archive" =~ ^[a-z0-9_]{1,59}\.rpf$ ]] || {
    echo "[menu-ctl] --typ-name and --archive take lowercase [a-z0-9_] names (NAME, NAME.rpf)" >&2
    return 2
  }
  local -a mlo_args=() map_names=() mlo_names=()
  local name map
  for value in "${mlos[@]}"; do
    name="${value%%=*}"
    map="${value#*=}"
    [ "$map" != "$value" ] || map="$(convert_short "gmmlo_${name}" 59)"
    map="${map%.pmap}"
    [[ "$name" =~ ^[a-z0-9_]{1,59}$ ]] && [[ "$map" =~ ^[a-z0-9_]{1,59}$ ]] || {
      echo "[menu-ctl] --mlo takes the MLO archetype NAME[=MAP] ([a-z0-9_]), not '$value'" >&2
      return 2
    }
    mlo_args+=(--mlo "$name=$map")
    mlo_names+=("$name")
    map_names+=("$map")
  done
  local -a map_places=()
  local x y z text
  for value in "${map_teleports[@]}"; do
    map="${value%%=*}"
    map="${map%.pmap}"
    IFS=, read -r x y z text <<< "${value#*=}"
    [[ " ${map_names[*]} " == *" $map "* ]] && [[ "$x,$y,$z" =~ ^-?[0-9]+,-?[0-9]+,-?[0-9]+$ ]] || {
      echo "[menu-ctl] --map-teleport takes MAP=X,Y,Z[,TEXT] (one of the --mlo maps, whole metres), not '$value'" >&2
      return 2
    }
    map_places+=(--map-place "$map.pmap=$x,$y,$z:${text:-$map}")
  done
  if [ -n "$max_typs" ]; then
    [[ "$max_typs" =~ ^[0-9]{1,2}$ ]] && [ "$max_typs" -le 15 ] || {
      echo "[menu-ctl] --max-retail-typs takes 0..15, not '$max_typs'" >&2
      return 2
    }
  else
    max_typs=15
    [ -z "$models_on" ] || max_typs=14 # the models typ takes a row too
  fi
  local typ_template milo bounds_tool="" models_tool="" mlo_tool root_pdr=""
  convert_tool convert_pc_mlo convert-mlo || return 2
  mlo_tool="$CV_TOOL"
  if [ -n "$models_on" ]; then
    convert_tool convert_pc_map_models convert-mlo || return 2
    models_tool="$CV_TOOL"
    convert_python convert-mlo numpy || return 2
  fi
  if [ -n "$bounds_on" ]; then
    convert_tool convert_pc_bounds convert-mlo || return 2
    bounds_tool="$CV_TOOL"
    convert_bound_templates convert-mlo || return 2
  fi
  [ -n "$index" ] || index="$CV_BUILD/archetype-index/$GTAV_TARGET.json"
  [ -f "$index" ] || {
    echo "[menu-ctl] convert-mlo needs an archetype index of your game's archetypes: --archetype-index FILE" \
      "(default $CV_BUILD/archetype-index/$GTAV_TARGET.json, which $0 index-archetypes writes)" >&2
    return 2
  }
  convert_template typ-templates/v_int_22.ptyp "" convert-mlo || return 2
  typ_template="$CV_FILE"
  convert_template map-templates/ch3_03_interior_v_gun2_milo_.pmap "" convert-mlo || return 2
  milo="$CV_FILE"
  if [ -n "$models_on" ]; then
    convert_template shader-carriers/ch3_03_ss_cb.pdr "" convert-mlo || return 2
    root_pdr="$CV_FILE"
    convert_carriers convert-mlo shader-carriers shader-carriers-interior || return 2
  fi
  mod="$(convert_abs "$mod")"
  convert_check_source "$mod" ".rpf" convert-mlo || return 2
  [ ! -d "$mod" ] || { echo "[menu-ctl] convert-mlo reads the mod's dlc.rpf, not a folder: $mod" >&2; return 2; }

  convert_start
  convert_copy "$mod" || return 2
  local copy="$CV_COPY"
  local -a base=("$PY" -I "$mlo_tool" "$copy" --archetype-index "$index" --typ-template "$typ_template"
    --milo-template "$milo" --typ-name "$typ" "${mlo_args[@]}" --max-retail-typs "$max_typs" ${no_entities:+"$no_entities"})
  local -a extra=() mloargs=()
  if [ -n "$models_on" ]; then
    # Pass 1 lists the own archetypes the MLOs place; the model step converts exactly those.
    convert_run "$CV_WORK/plan.log" "${base[@]}" --out "$CV_WORK/plan" || return 1
    local -a select=()
    mapfile -d '' -t select < <("$PY" -I -c 'import json, sys
names = json.load(open(sys.argv[1], encoding="utf-8"))["ownToConvert"]
sys.stdout.write("".join("--archetype\0" + name + "\0" for name in names))' "$CV_WORK/plan/report.json")
    if [ "${#select[@]}" -eq 0 ]; then
      echo "[menu-ctl] --models: the interiors place none of the mod's own models"
    else
      echo "[menu-ctl] converting the interiors' $((${#select[@]} / 2)) own models (this can take minutes)"
      convert_run "$CV_WORK/models.log" "${CV_PY[@]}" "$models_tool" "$copy" "${select[@]}" --template "$root_pdr" \
        "${CV_CARRIERS[@]}" --typ-template "$typ_template" --typ-name "${typ}m" --reference-dir "$CV_REFERENCE" \
        --templates "$CV_TEMPLATES" --max-texture-size 1024 "${renames[@]}" --out "$CV_WORK/models" || return 1
      grep -E '^(refused|note):|archetypes converted' "$CV_WORK/models.log" | sed 's/^/[menu-ctl]   /'
      mloargs=(--pack-archetypes "$CV_WORK/models/pack-archetypes.json")
      mapfile -d '' -t extra < <("$PY" -I -c 'import json, sys
t = json.load(open(sys.argv[1], encoding="utf-8"))
out = []
for member, path in sorted(t["members"].items()):
    out += ["--member", member + "=" + sys.argv[2] + "/" + path]
sys.stdout.write("\0".join(out + ["--typ", t["typ"] + ".ptyp"]) + "\0")' \
        "$CV_WORK/models/pack-archetypes.json" "$CV_WORK/models")
    fi
  fi
  convert_run "$CV_WORK/mlo.log" "${base[@]}" "${mloargs[@]}" --out "$CV_WORK/mlo" || return 1
  sed 's/^/[menu-ctl]   /' "$CV_WORK/mlo.log"
  local -a rows=() bounds=()
  mapfile -d '' -t rows < <("$PY" -I -c 'import json, sys
sys.stdout.write("\0".join(json.load(open(sys.argv[1], encoding="utf-8"))) + "\0")' \
    "$CV_WORK/mlo/build_runtime_pack.args.json")
  if [ -n "$bounds_on" ]; then
    # The interior bounds binder pairs the MLO with a bounds row named like the MLO archetype: <mlo>.pbn,
    # MLO-local coordinates and room ids as the mod has them.
    local -a only=()
    for name in "${mlo_names[@]}"; do only+=(--only "$name"); done
    convert_run "$CV_WORK/extract.log" "$PY" -I "$ROOT/tools/extract_mod_files.py" "$copy" --suffix .ybn "${only[@]}" \
      --out "$CV_WORK/ybn" || return 1
    mkdir -p -- "$CV_WORK/bounds"
    for name in "${mlo_names[@]}"; do
      convert_run "$CV_WORK/bounds/$name.log" "$PY" -I "$bounds_tool" convert "$CV_WORK/ybn/$name.ybn" \
        --include "$CV_INCLUDE_DEFAULT" --output "$CV_WORK/bounds/$name.pbn" --report "$CV_WORK/bounds/$name.json" \
        "${CV_BOUND_TEMPLATES[@]}" || return 1
      bounds+=(--member "$name.pbn=$CV_WORK/bounds/$name.pbn" --bounds "$name.pbn")
      echo "[menu-ctl] $name.pbn <- $name.ybn: $(wc -c < "$CV_WORK/bounds/$name.pbn") bytes"
    done
  fi
  convert_run "$CV_WORK/build.log" "$PY" "$ROOT/tools/build_runtime_pack.py" "${extra[@]}" "${rows[@]}" --id "$CV_ID" \
    --archive "$archive" --output-root "$CV_PACKS" "${bounds[@]}" "${map_places[@]}" "${CV_PLACES[@]}" || return 1
  if [ "${#map_places[@]}" -eq 0 ]; then
    "$PY" -I -c 'import json, sys
for m in json.load(open(sys.argv[1], encoding="utf-8"))["mlos"]:
    i = m["instance"]
    print("[menu-ctl] %s.pmap places %s at %s: add --map-teleport %s=X,Y,Z,TEXT on its floor (an INTERIOR row)"
          % (m["map"], m["mlo"], i.get("position"), m["map"]))' "$CV_WORK/mlo/report.json" || true
  fi
  convert_finish convert-mlo "$install"
}

# Host: a whole PC map mod (its dlc.rpf or folder: .ymap + .ytyp + own .ydr/.ytd + .ybn + MLO interiors) to one or
# more packs within the pack caps. tools/convert_map_mod.py inventories the copied mod, plans the packs (world: the
# placements, the own models they place and the exterior collision rows; interior: the MLOs, their own models and
# shell collision; collision: the rows past a pack's 8) and runs convert-map/-model/-bounds/-mlo's converters on the
# copy, as those commands do. --dry-run prints the plan and the cap use and writes nothing; --write-plan FILE saves
# the plan, --plan FILE converts an edited one. Every pack it builds is listed in <work>/packs-built (rebuilt on the
# next run with this id); the packs are validated against each other and installed together.
# mapmod_copy_from FROM: a props mod given to convert-mapmod --archetypes-from is copied (like the mod) into
# CV_INPUT/archetypes-from; fromarg names the copy. CV_COPY keeps naming the mod's copy. A pack directory: nothing.
mapmod_copy_from() {
  [ -n "$1" ] && [ ! -f "$1/resources/pack.cfg" ] || return 0
  local input="$CV_INPUT" copy="$CV_COPY"
  CV_INPUT="$input/archetypes-from"
  mkdir -p -- "$CV_INPUT"
  convert_copy "$1" || { CV_INPUT="$input"; return 2; }
  fromarg=(--archetypes-from "$CV_COPY")
  CV_INPUT="$input"
  CV_COPY="$copy"
}

do_convert_mapmod() {
  local usage="usage: $0 convert-mapmod MOD --id PACK_ID [--dry-run] [--write-plan FILE] [--plan FILE] [--name TEXT]
       [--translate DX,DY,DZ] [--bounds rows|merge] [--drawn-collision embedded|all] [--no-interiors]
       [--max-entities N] [--max-texture-size N] [--teleport X,Y,Z[,TEXT] ...] [--map-teleport MAP=X,Y,Z[,TEXT] ...]
       [--archetype-index FILE] [--templates DIR] [--reference-dir DIR] [--install|--activate]
       [--archetypes-from MOD|PACK_DIR [--archetypes-id ID]]
  MOD: the mod's dlc.rpf, or its folder (its one dlc.rpf, or loose .ymap/.ytyp/.ydr/.ytd/.ybn files)
  --archetypes-from: another mod whose props the maps place (e.g. Map Builder's dlc.rpf): the placed models become
       one more pack (--archetypes-id, default ID with -props); or a pack directory built before (its archetypes)"
  local mod="" id="" install="" dry="" index="" templates="" reference="" name="" from="" value
  local -a pass=() fromarg=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --id | --name | --archetype-index | --templates | --reference-dir | --plan | --write-plan | --translate | \
        --bounds | --drawn-collision | --max-entities | --max-texture-size | --teleport | --map-teleport | \
        --archetypes-from | --archetypes-id)
        [ "$#" -ge 2 ] || { echo "$usage" >&2; return 2; }
        value="$2"
        case "$1" in
          --id) id="$value" ;;
          --name) name="$value" ;;
          --archetypes-from) from="$(convert_abs "$value")" ;;
          --archetype-index) index="$(convert_abs "$value")" ;;
          --templates) templates="$(convert_abs "$value")" ;;
          --reference-dir) reference="$(convert_abs "$value")" ;;
          --plan | --write-plan) pass+=("$1" "$(convert_abs "$value")") ;;
          *) pass+=("$1" "$value") ;;
        esac
        shift 2
        ;;
      --dry-run | --no-interiors)
        if [ "$1" = --dry-run ]; then dry=1; else pass+=("$1"); fi
        shift
        ;;
      --install | --activate)
        [ -z "$install" ] || { echo "$usage" >&2; return 2; }
        install="$1"
        shift
        ;;
      -*) echo "$usage" >&2; return 2 ;;
      *)
        [ -z "$mod" ] || { echo "$usage" >&2; return 2; }
        mod="$1"
        shift
        ;;
    esac
  done
  [ -n "$mod" ] && [ -n "$id" ] || { echo "$usage" >&2; return 2; }
  convert_setup convert-mapmod "$id" || return 2
  CV_TEMPLATES="${templates:-$CV_TEMPLATES}"
  CV_REFERENCE="${reference:-$CV_REFERENCE}"
  mod="$(convert_abs "$mod")"
  convert_check_source "$mod" ".rpf" convert-mapmod || return 2
  # --archetypes-from: a pack directory is read in place; a props mod (untrusted) is copied like the mod.
  if [ -n "$from" ] && [ ! -f "$from/resources/pack.cfg" ]; then
    convert_check_source "$from" ".rpf" convert-mapmod || return 2
  elif [ -n "$from" ]; then
    fromarg=(--archetypes-from "$from")
  fi
  if [ -z "$name" ]; then
    name="$(basename -- "$mod")"
    [ "$(convert_lower "$name")" != dlc.rpf ] || name="$(basename -- "$(dirname -- "$mod")")"
    name="${name%.[rR][pP][fF]}"
  fi
  local -a tools=(--templates "$CV_TEMPLATES" --reference-dir "$CV_REFERENCE")
  [ -n "$index" ] || index="$CV_BUILD/archetype-index/$GTAV_TARGET.json" # where index-archetypes writes it
  if [ -f "$index" ]; then
    tools+=(--archetype-index "$index")
  else
    echo "[menu-ctl] no archetype index ($index): stock entities are kept unchecked and interiors are left out" \
      "(build the index: $0 index-archetypes)"
  fi
  # The converters (tools/; numpy for the model ones) are optional: without one the plan leaves out the parts that
  # need it and says so.
  if convert_tool convert_pc_bounds convert-mapmod 2> /dev/null; then tools+=(--bounds-tool "$CV_TOOL"); fi
  if convert_python convert-mapmod numpy 2> /dev/null; then
    [ "${CV_PY[0]}" != env ] || tools+=(--model-pythonpath "${CV_PY[1]#PYTHONPATH=}")
    if convert_tool convert_pc_map_models convert-mapmod 2> /dev/null; then tools+=(--models-tool "$CV_TOOL"); fi
    if convert_tool drawable_collision convert-mapmod 2> /dev/null; then tools+=(--collision-tool "$CV_TOOL"); fi
  fi
  local -a owned=()
  [ ! -f "$CV_WORK/packs-built" ] || mapfile -t owned < "$CV_WORK/packs-built"
  local -a convert=("$PY" -I "$ROOT/tools/convert_map_mod.py")
  local -a common=(--id "$CV_ID" --packs-root "$CV_PACKS" --name "$name" "${tools[@]}" "${pass[@]}")
  if [ -n "$dry" ]; then
    # The mod is read from a throw-away copy; nothing is kept (folders this made are removed again).
    local made=() dir status=0
    for dir in "$CV_BUILD" "$CV_BUILD/convert-mapmod"; do [ -d "$dir" ] || made=("$dir" "${made[@]}"); done
    mkdir -p -- "$CV_BUILD/convert-mapmod"
    CV_INPUT="$(mktemp -d "$CV_BUILD/convert-mapmod/.dry-run-XXXXXX")"
    { convert_copy "$mod" && mapmod_copy_from "$from"; } &&
      "${convert[@]}" "$CV_COPY" --work "$CV_INPUT" --dry-run "${common[@]}" "${fromarg[@]}" || status=$?
    rm -rf -- "$CV_INPUT"
    for dir in "${made[@]}"; do rmdir -- "$dir" 2> /dev/null || true; done
    return "$status"
  fi
  convert_start
  convert_copy "$mod" || return 2
  mapmod_copy_from "$from" || return 2
  echo "[menu-ctl] converting $(basename -- "$mod") (models can take minutes; logs in $CV_WORK)"
  if ! "${convert[@]}" "$CV_COPY" --work "$CV_WORK" "${common[@]}" "${fromarg[@]}" "${owned[@]/#/--owned=}"; then
    echo "[menu-ctl] conversion stopped; nothing was installed (logs and the copied mod: $CV_WORK)" >&2
    return 1
  fi
  local -a ids=()
  mapfile -t ids < "$CV_WORK/packs.txt"
  local pid first=1
  for pid in "${ids[@]}"; do
    case "$install" in
      --activate)
        if [ -n "$first" ]; then do_pack_install "$CV_PACKS/$pid" --activate || return 1; else
          do_pack_install "$CV_PACKS/$pid" --activate-add || return 1
        fi
        ;;
      --install) do_pack_install "$CV_PACKS/$pid" || return 1 ;;
      *)
        if [ -n "$first" ]; then
          echo "[menu-ctl] upload them as the selected set: PS5_HOST=${PS5_HOST:-<console-ip>} $0 pack-install $CV_PACKS/$pid --activate"
        else
          echo "[menu-ctl]                                   PS5_HOST=${PS5_HOST:-<console-ip>} $0 pack-install $CV_PACKS/$pid --activate-add"
        fi
        ;;
    esac
    first=""
  done
}

# Host: a whole PC add-on ped (.ydd, .ytd, .ymt, .yft; props in <stem>_p.ydd/.ytd) as a new pack ped
# (tools/convert_pc_ped.py ped): every component drawable of the .ydd converted onto a retail cop entry
# of its kind (a drawable with its own skeleton copy, the head, onto the cop head, which carries the standard
# 98-bone skeleton copy; the others index the mod's .yft skeleton), every prop onto a retail cop prop, the
# textures into the ped's .ptd / <ped>_p.ptd, the .ymt as the .pmt, the cop skeleton (.pft) for the rig; then a
# round trip of the variation layout against the written members. --drawable adds or replaces a component from
# another PC .ydd (its textures renamed for the new slot); --variations retail keeps the cop's variation layout,
# drawables, textures and props (PC parts replace keys of that layout). The InitData (peds.meta) is a donor
# ped's under the new name (tools/make_ped_initdata.py; PropsName = <ped>_p when props are shipped).
do_convert_ped() {
  local usage="usage: $0 convert-ped MOD --id PACK_ID --ped NAME --init-data FILE [--like NAME] [--entry NAME]
       [--drawable KEY=FILE[:ENTRY] ...] [--variations mod|retail] [--skip-unsupported] [--placeholders]
       [--strict-variations] [--label TEXT] [--archive NAME.rpf] [--head-only-pmt] [--max-texture-size N]
       [--prop-transform KEY=RX,RY,RZ,DX,DY,DZ|KEY=auto|KEY=none ...] [--keep-prop-colours]
       [--templates DIR] [--reference-dir DIR] [--install|--activate]
  MOD: the ped's .ydd (its .ytd, .ymt and .yft next to it, props in <stem>_p.ydd/.ytd), or a folder holding one
       ped's files
  --init-data: a CPedModelInfo__InitDataList XML holding the donor ped (e.g. from your game's peds.meta)
  --entry: convert only this drawable of the .ydd (default: every one)
  --drawable KEY=FILE[:ENTRY]: KEY (e.g. accs_001_u, p_eyes_000) from another PC .ydd entry (or .ydr)
  --variations retail: the retail template ped's variation layout (s_m_y_cop_01: 3 heads, 2 uppers, ...,
       5 props) with its drawables, textures and props wherever no PC part replaces them
  --prop-transform: place prop KEY in its anchor bone's frame (degrees about X, Y, Z, then metres); a map
       object .ydr as a hat stands on top of the head by default (auto)"
  local mod="" id="" install="" ped="" initdata="" like="" entry="" label="" archive="" head_only=""
  local max_texture=1024 templates="" reference="" variations=mod value
  local -a drawables=() flags=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --id | --ped | --init-data | --like | --entry | --label | --archive | --max-texture-size | --templates | \
        --reference-dir | --drawable | --variations | --prop-transform)
        [ "$#" -ge 2 ] || { echo "$usage" >&2; return 2; }
        value="$2"
        case "$1" in
          --id) id="$value" ;;
          --ped) ped="$value" ;;
          --init-data) initdata="$(convert_abs "$value")" ;;
          --like) like="$value" ;;
          --entry) entry="$value" ;;
          --label) label="$value" ;;
          --archive) archive="$value" ;;
          --max-texture-size) max_texture="$value" ;;
          --templates) templates="$(convert_abs "$value")" ;;
          --reference-dir) reference="$(convert_abs "$value")" ;;
          --drawable) drawables+=("$value") ;;
          --variations) variations="$value" ;;
          --prop-transform) flags+=(--prop-transform "$value") ;;
        esac
        shift 2
        ;;
      --head-only-pmt)
        head_only=1
        shift
        ;;
      --skip-unsupported | --placeholders | --strict-variations | --keep-prop-colours)
        flags+=("$1")
        shift
        ;;
      --install | --activate)
        [ -z "$install" ] || { echo "$usage" >&2; return 2; }
        install="$1"
        shift
        ;;
      -*) echo "$usage" >&2; return 2 ;;
      *)
        [ -z "$mod" ] || { echo "$usage" >&2; return 2; }
        mod="$1"
        shift
        ;;
    esac
  done
  [ -n "$mod" ] && [ -n "$id" ] && [ -n "$ped" ] || { echo "$usage" >&2; return 2; }
  convert_setup convert-ped "$id" || return 2
  CV_TEMPLATES="${templates:-$CV_TEMPLATES}"
  CV_REFERENCE="${reference:-$CV_REFERENCE}"
  [[ "$ped" =~ ^[a-z0-9_]{1,40}$ ]] || { echo "[menu-ctl] --ped takes the new model name ([a-z0-9_], e.g. gm_hero_01)" >&2; return 2; }
  [ -z "$entry" ] || [[ "$entry" =~ ^[a-z]+_[0-9]{3}_[a-z]$ ]] || {
    echo "[menu-ctl] --entry takes a component drawable (e.g. head_000_r)" >&2
    return 2
  }
  [[ "$max_texture" =~ ^[1-9][0-9]{0,4}$ ]] || { echo "[menu-ctl] --max-texture-size takes a number" >&2; return 2; }
  [ "$variations" = mod ] || [ "$variations" = retail ] || {
    echo "[menu-ctl] --variations takes mod (the mod's .ymt) or retail (the retail ped's layout)" >&2
    return 2
  }
  [ -z "$head_only" ] || [ "$variations" = mod ] || {
    echo "[menu-ctl] --head-only-pmt reduces the mod's .ymt; it does not go with --variations retail" >&2
    return 2
  }
  [ -n "$initdata" ] && [ -f "$initdata" ] || {
    echo "[menu-ctl] convert-ped needs --init-data FILE: a CPedModelInfo__InitDataList XML holding the donor ped" \
      "whose behaviour the new ped copies (e.g. the InitData of your game's peds.meta)" >&2
    return 2
  }
  # peds_<name>.meta: the ped name without a gm_ prefix and an _01 suffix (gm_goose_01 -> peds_goose.meta,
  # gm_goose_02 -> peds_goose_02.meta); data file names must not repeat across active packs.
  local stem="${ped#gm_}"
  stem="${stem%_01}"
  archive="${archive:-$(convert_short "gmped_$CV_SLUG" 59).rpf}"
  label="${label:-$ped (PC ped)}"
  [[ "$archive" =~ ^[a-z0-9_]{1,59}\.rpf$ ]] || { echo "[menu-ctl] --archive takes a lowercase NAME.rpf" >&2; return 2; }
  [ "${#label}" -le 40 ] || { echo "[menu-ctl] --label is at most 40 characters" >&2; return 2; }
  local pft pdd pmt ped_tool carrier props_pdd="" ptd="" props_ptd=""
  convert_tool convert_pc_ped convert-ped || return 2
  ped_tool="$CV_TOOL"
  convert_python convert-ped numpy || return 2
  convert_template peds/s_m_y_cop_01.pft "" convert-ped || return 2
  pft="$CV_FILE"
  convert_template peds/s_m_y_cop_01.pdd "" convert-ped || return 2
  pdd="$CV_FILE"
  convert_template peds/s_m_y_cop_01.pmt "" convert-ped || return 2
  pmt="$CV_FILE"
  convert_template peds/u_m_m_juggernaut_03.pdd "" convert-ped || return 2
  carrier="$CV_FILE"
  # Map and interior shader carriers lend schemas a ped dictionary lacks (a prop made from a map object).
  local -a carriers=()
  if convert_carriers convert-ped shader-carriers shader-carriers-interior 2> /dev/null; then
    carriers=("${CV_CARRIERS[@]}")
  fi
  mod="$(convert_abs "$mod")"
  convert_check_source "$mod" ".ydd" convert-ped || return 2
  local ydd
  if [ -d "$mod" ]; then
    local -a hits=()
    mapfile -d '' -t hits < <(find "$mod" -maxdepth 1 -type f -iname '*.ydd' ! -iname '*_p.ydd' -print0)
    [ "${#hits[@]}" -eq 1 ] || {
      echo "[menu-ctl] the folder must hold exactly one ped (.ydd, besides its _p.ydd props); it has ${#hits[@]}:" \
        "give the .ydd itself" >&2
      return 2
    }
    ydd="${hits[0]}"
  else
    ydd="$mod"
  fi
  local ystem="${ydd%.*}" ext file dir
  dir="$(dirname -- "$ydd")"
  for ext in ytd ymt; do
    [ "$ext" = ymt ] && [ "$variations" = retail ] && continue
    file="$(find "$dir" -maxdepth 1 -type f -iname "$(basename -- "$ystem").$ext" -print -quit)"
    [ -n "$file" ] || { echo "[menu-ctl] no $(basename -- "$ystem").$ext next to $(basename -- "$ydd")" >&2; return 2; }
    [ ! -L "$file" ] || { echo "[menu-ctl] symbolic links are refused: $file" >&2; return 2; }
  done
  # Check both supported prop locations before replacing a previous conversion's work.
  # A checked temporary file preserves failures (process substitution would hide them).
  local has_props="" props_list prop_kind prop_file
  local -A props=()
  props_list="$(mktemp)" || return 2
  if ! "${CV_PY[@]}" "$ped_tool" props --ydd "$ydd" --null > "$props_list"; then
    rm -f -- "$props_list"
    return 2
  fi
  while IFS= read -r -d '' prop_kind && IFS= read -r -d '' prop_file; do
    props[$prop_kind]="$prop_file"
  done < "$props_list"
  rm -f -- "$props_list"
  [ -z "${props[ydd]:-}" ] || has_props=1
  local row key source
  for row in "${drawables[@]}"; do
    key="${row%%=*}"
    source="${row#*=}"
    source="${source%%:*}"
    [ "$key" != "$row" ] && [ -n "$source" ] || { echo "[menu-ctl] --drawable takes KEY=FILE[:ENTRY], not '$row'" >&2; return 2; }
    convert_check_source "$(convert_abs "$source")" ".ydd .ydr" convert-ped || return 2
    [ -f "$(convert_abs "$source")" ] || { echo "[menu-ctl] --drawable names a file, not a folder: $source" >&2; return 2; }
    case "$key" in p_*) has_props=1 ;; esac
  done
  if [ -n "$has_props" ] || [ "$variations" = retail ]; then
    convert_template peds/s_m_y_cop_01_p.pdd "" convert-ped || return 2
    props_pdd="$CV_FILE"
  fi
  if [ "$variations" = retail ]; then
    convert_template peds/s_m_y_cop_01.ptd "" convert-ped || return 2
    ptd="$CV_FILE"
    convert_template peds/s_m_y_cop_01_p.ptd "" convert-ped || return 2
    props_ptd="$CV_FILE"
  fi

  convert_start
  # The mod's files (and each --drawable file with its .ytd/.yft) are copied into the work input; the
  # converter reads only the copies.
  local -A copied=() taken=()
  local base
  for ext in ydd ytd ymt yft; do
    file="$(find "$dir" -maxdepth 1 -type f -iname "$(basename -- "$ystem").$ext" -print -quit)"
    [ -z "$file" ] || [ -L "$file" ] || { convert_copy "$file" && copied[$ext]="$CV_COPY" && taken[$file]="$CV_COPY"; } ||
      return 2
  done
  for ext in ydd ytd; do
    file="${props[$ext]:-}"
    [ -z "$file" ] || { convert_copy "$file" && copied[p$ext]="$CV_COPY" && taken[$file]="$CV_COPY"; } || return 2
  done
  local -a pedargs=()
  for row in "${drawables[@]}"; do
    key="${row%%=*}"
    source="${row#*=}"
    value=""
    case "$source" in *:*) value=":${source#*:}" ;; esac
    source="$(convert_abs "${source%%:*}")"
    base="$(basename -- "${source%.*}")"
    local -a siblings=("$source")
    for ext in ytd yft; do
      file="$(find "$(dirname -- "$source")" -maxdepth 1 -type f -iname "$base.$ext" -print -quit)"
      [ -z "$file" ] || siblings+=("$file")
    done
    for file in "${siblings[@]}"; do
      [ ! -L "$file" ] && [ -z "${taken[$file]:-}" ] || continue
      convert_copy "$file" || return 2
      taken[$file]="$CV_COPY"
    done
    pedargs+=(--drawable "$key=${taken[$source]}$value")
  done
  local out="$CV_WORK/out"
  [ -z "${copied[ytd]:-}" ] || pedargs+=(--ytd "${copied[ytd]}")
  [ -z "${copied[ymt]:-}" ] || pedargs+=(--ymt "${copied[ymt]}")
  [ -z "${copied[yft]:-}" ] || pedargs+=(--yft "${copied[yft]}")
  [ -z "${copied[pydd]:-}" ] || pedargs+=(--props-ydd "${copied[pydd]}")
  [ -z "${copied[pytd]:-}" ] || pedargs+=(--props-ytd "${copied[pytd]}")
  [ -z "$props_pdd" ] || pedargs+=(--props-pdd "$props_pdd")
  [ -z "$ptd" ] || pedargs+=(--ptd "$ptd" --props-ptd "$props_ptd")
  [ -z "$entry" ] || pedargs+=(--only "$entry")
  [ -z "$head_only" ] || pedargs+=(--reduce-pmt "${entry:-head_000_r}")
  echo "[menu-ctl] converting $(basename -- "$ydd") onto the retail ped templates (this can take minutes)"
  convert_run "$CV_WORK/ped.log" "${CV_PY[@]}" "$ped_tool" ped --ydd "${copied[ydd]}" --name "$ped" --pdd "$pdd" \
    --pft "$pft" --retail-pmt "$pmt" --carrier "$carrier" "${carriers[@]}" --variations "$variations" "${pedargs[@]}" \
    "${flags[@]}" --max-texture-size "$max_texture" --reference-dir "$CV_REFERENCE" --templates "$CV_TEMPLATES" \
    --out "$out" ||
    return 1
  local -a likeargs=() propsargs=() members=()
  [ -z "$like" ] || likeargs=(--like "$like")
  # the converter writes the rig it converted on (the cop's .pft, or a fetched female rig the mod's .yft matches)
  [ ! -f "$out/$ped.pft" ] || pft="$out/$ped.pft"
  members=(--member "$ped.pft=$pft" --member "$ped.pdd=$out/$ped.pdd" --member "$ped.ptd=$out/$ped.ptd"
    --member "$ped.pmt=$out/$ped.pmt")
  if [ -f "$out/${ped}_p.pdd" ]; then
    propsargs=(--props "${ped}_p")
    members+=(--member "${ped}_p.pdd=$out/${ped}_p.pdd" --member "${ped}_p.ptd=$out/${ped}_p.ptd")
  fi
  convert_run "$CV_WORK/initdata.log" "$PY" -I "$ROOT/tools/make_ped_initdata.py" "$initdata" --name "$ped" \
    "${likeargs[@]}" "${propsargs[@]}" --output "$out/peds_$stem.meta" || return 1
  grep -v '^wrote ' "$CV_WORK/ped.log" | sed 's/^/[menu-ctl]   /'
  sed 's/^/[menu-ctl]   /' "$CV_WORK/initdata.log"
  convert_run "$CV_WORK/build.log" "$PY" "$ROOT/tools/build_runtime_pack.py" --output-root "$CV_PACKS" --id "$CV_ID" \
    --archive "$archive" "${members[@]}" --data "PED_METADATA_FILE=$out/peds_$stem.meta" --spawn "ped:$ped=$label" ||
    return 1
  echo "[menu-ctl] in game after Load: Custom Packs -> Spawn -> $label (or $0 spawn-ped $ped); to wear it and pick"
  echo "[menu-ctl] its drawables: Self -> Appearance -> Skin Changer -> Custom -> $label, then Self -> Appearance ->"
  echo "[menu-ctl] Wardrobe (Slot / Style / Texture; props: Hat, Glasses, ...)"
  convert_finish convert-ped "$install"
}

# Host: a PC weapon model (.ydr + .ytd) as a NEW pack weapon: the model converted onto the retail drawable of the
# weapon it replaces on PC (tools/convert_pc_drawable.py), then cloned as a new model + weapon (make_weapon_model_pack.py:
# the donor weapon's stats, animations and archetype row from the metas you give, a new slot and label). The
# pack builder adds the default weapon-wheel icon row (the donor slot's retail icon).
do_convert_weapon() {
  local usage="usage: $0 convert-weapon MOD --id PACK_ID --weapon WEAPON_NEW --like WEAPON_DONOR
       [--weapons-meta F --animations-meta F --archetypes-meta F] [--source-model NAME] [--model NAME]
       [--slot SLOT_NAME] [--label KEY] [--orders NAV1,NAV2,BEST] [--text TEXT] [--tint none|RRGGBB]
       [--template F.pdr] [--shader-template F.pdr ...] [--archive NAME.rpf] [--meta-prefix NAME]
       [--attachments [--components-meta F]] [--max-texture-size N] [--templates DIR] [--reference-dir DIR]
       [--no-tints] [--install|--activate]
  MOD: the weapon model's .ydr (its .ytd next to it), or a folder holding them (a PC replace mod's files; w_at_*
       attachment models are not the weapon; a mod that ships only <model>_hi.ydr converts that as the model)
  --attachments: also convert the folder's magazine and attachment models named like the donor's retail
          component models (<model>_mag1, w_at_scope_medium, w_at_ar_supp, ...; onto weapons/<name>.pdr from
          fetch-templates) as pack components on the new weapon: the magazine becomes its default clip, the others
          Custom Packs toggle rows. --components-meta: the decrypted retail weaponcomponents.meta
          (default: <templates>/retail-metas/weaponcomponents.meta)
  --meta-prefix: data file names NAME_weapons.meta etc. (default: the model name, so packs coexist)
  tints: the model's opaque body materials take the retail tint shader (weapon_normal_spec_detail_palette) with a
          tint palette, so Weapon Tint recolours it like a retail gun; its diffuse textures are brightened (the
          palette's Normal row darkens them back). A model without such a material (or a template family without
          the shader) prints one 'tints: none' line. --no-tints keeps the mod's own shaders (no tints).
  --like: the donor; it decides the weapon class (pistol, smg, mg, rifle, shotgun, sniper, heavy, melee,
          throwable): fire type, ammo, stats, animations, attachment points, the wheel icon and, without
          --orders, the slot order (right after the donor's). Refused with the reason: unarmed, gadgets,
          vehicle weapons, particle sprayers, a default component on a bone the model lacks.
  the model: converted onto the retail drawable of its own name (weapons/<model>.pdr), else onto the
             donor's model (the mod's skeleton must equal it)
  the metas: decrypted retail weapons.meta, weaponanimations.meta, weaponarchetypes.meta from your game
             (default: <templates>/retail-metas/<name>)"
  local mod="" id="" install="" weapon="" like="" wmeta="" ameta="" rmeta="" source="" model=""
  local slot="" label="" orders="" text="" tint=none template="" archive="" max_texture=1024 templates=""
  local reference="" prefix="" value attachments="" cmeta="" tints=auto
  local -a shaders=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --id | --weapon | --like | --weapons-meta | --animations-meta | --archetypes-meta | --source-model | --model | \
        --slot | --label | --orders | --text | --tint | --template | --shader-template | --archive | \
        --meta-prefix | --max-texture-size | --templates | --reference-dir | --components-meta)
        [ "$#" -ge 2 ] || { echo "$usage" >&2; return 2; }
        value="$2"
        case "$1" in
          --id) id="$value" ;;
          --weapon) weapon="$value" ;;
          --like) like="$value" ;;
          --weapons-meta) wmeta="$(convert_abs "$value")" ;;
          --animations-meta) ameta="$(convert_abs "$value")" ;;
          --archetypes-meta) rmeta="$(convert_abs "$value")" ;;
          --source-model) source="$(convert_lower "$value")" ;;
          --model) model="$value" ;;
          --slot) slot="$value" ;;
          --label) label="$value" ;;
          --orders) orders="$value" ;;
          --text) text="$value" ;;
          --tint) tint="$value" ;;
          --template) template="$(convert_abs "$value")" ;;
          --shader-template) shaders+=(--shader-template "$(convert_abs "$value")") ;;
          --archive) archive="$value" ;;
          --meta-prefix) prefix="$value" ;;
          --max-texture-size) max_texture="$value" ;;
          --templates) templates="$(convert_abs "$value")" ;;
          --reference-dir) reference="$(convert_abs "$value")" ;;
          --components-meta) cmeta="$(convert_abs "$value")" ;;
        esac
        shift 2
        ;;
      --attachments)
        attachments=1
        shift
        ;;
      --no-tints)
        tints=none
        shift
        ;;
      --install | --activate)
        [ -z "$install" ] || { echo "$usage" >&2; return 2; }
        install="$1"
        shift
        ;;
      -*) echo "$usage" >&2; return 2 ;;
      *)
        [ -z "$mod" ] || { echo "$usage" >&2; return 2; }
        mod="$1"
        shift
        ;;
    esac
  done
  [ -n "$mod" ] && [ -n "$id" ] && [ -n "$weapon" ] && [ -n "$like" ] || { echo "$usage" >&2; return 2; }
  convert_setup convert-weapon "$id" || return 2
  CV_TEMPLATES="${templates:-$CV_TEMPLATES}"
  CV_REFERENCE="${reference:-$CV_REFERENCE}"
  [[ "$weapon" =~ ^WEAPON_[A-Z0-9_]{1,40}$ ]] && [[ "$like" =~ ^WEAPON_[A-Z0-9_]{1,40}$ ]] || {
    echo "[menu-ctl] --weapon and --like take WEAPON_* names (upper case), e.g. --weapon WEAPON_GMGLOCK" >&2
    return 2
  }
  local short="${weapon#WEAPON_}"
  model="${model:-$(convert_short "w_$(convert_lower "$short")" 40)}"
  slot="${slot:-$(convert_short "SLOT_$short" 63)}"
  label="${label:-$(convert_short "WT_$short" 63)}"
  text="${text:-$short}"
  archive="${archive:-$(convert_short "gmwpn_$CV_SLUG" 59).rpf}"
  [[ "$archive" =~ ^[a-z0-9_]{1,59}\.rpf$ ]] || { echo "[menu-ctl] --archive takes a lowercase NAME.rpf" >&2; return 2; }
  prefix="${prefix:-$model}"
  [[ "$prefix" =~ ^[a-z0-9_]{1,40}$ ]] || { echo "[menu-ctl] --meta-prefix takes a lowercase NAME" >&2; return 2; }
  [[ "$max_texture" =~ ^[1-9][0-9]{0,4}$ ]] || { echo "[menu-ctl] --max-texture-size takes a number" >&2; return 2; }
  [ "${#text}" -le 40 ] || { echo "[menu-ctl] --text is at most 40 characters" >&2; return 2; }
  local drawable_tool pack_tool model_tool
  convert_tool convert_pc_drawable convert-weapon || return 2
  drawable_tool="$CV_TOOL"
  model_tool="$drawable_tool"
  if [ "$tints" = auto ]; then
    convert_tool weapon_tint_drawable convert-weapon || return 2
    model_tool="$CV_TOOL"  # the weapon model with the tint shader + palette (attachments keep their shaders)
  fi
  convert_tool make_weapon_model_pack convert-weapon || return 2
  pack_tool="$CV_TOOL"
  convert_verified_template retail-metas/weapons.meta --weapons-meta convert-weapon "$wmeta" || return 2
  wmeta="$CV_FILE"
  convert_verified_template retail-metas/weaponanimations.meta --animations-meta convert-weapon "$ameta" || return 2
  ameta="$CV_FILE"
  convert_verified_template retail-metas/weaponarchetypes.meta --archetypes-meta convert-weapon "$rmeta" || return 2
  rmeta="$CV_FILE"
  local -a classify_extra=()
  if [ -n "$attachments" ]; then
    convert_verified_template retail-metas/weaponcomponents.meta --components-meta convert-weapon "$cmeta" || return 2
    cmeta="$CV_FILE"
    classify_extra=(--components "$cmeta")
  fi
  local donor_line donor_model
  donor_line="$("$PY" -I "$pack_tool" --classify --weapons "$wmeta" --source "$like" "${classify_extra[@]}" 2>&1)" || {
    echo "[menu-ctl] convert-weapon: --like ${donor_line#error: }" >&2
    return 2
  }
  donor_model="$(sed -n 's/.* model=\([a-z0-9_]*\) .*/\1/p' <<< "$donor_line")"
  echo "[menu-ctl] donor: $donor_line"
  convert_python convert-weapon numpy || return 2
  mod="$(convert_abs "$mod")"
  convert_check_source "$mod" ".ydr" convert-weapon || return 2
  local ydr=""
  if [ -d "$mod" ]; then
    if [ -n "$source" ]; then
      ydr="$(find "$mod" -maxdepth 1 -type f -iname "$source.ydr" -print -quit)"
    else
      local -a hits=()
      mapfile -d '' -t hits < <(find "$mod" -maxdepth 1 -type f -iname '*.ydr' ! -iname '*_hi.ydr' ! -iname '*_mag*.ydr' \
        ! -iname 'w_at_*' -print0)
      if [ "${#hits[@]}" -eq 0 ]; then
        # Some replace mods ship only the HD model (<model>_hi.ydr, the Tactical MP5K): it becomes the model.
        mapfile -d '' -t hits < <(find "$mod" -maxdepth 1 -type f -iname '*_hi.ydr' ! -iname 'w_at_*' -print0)
        [ "${#hits[@]}" -ne 1 ] || echo "[menu-ctl] the folder has no base model, only $(basename -- "${hits[0]}"):" \
          "converting it as the weapon's model"
      fi
      [ "${#hits[@]}" -ne 1 ] || ydr="${hits[0]}"
      [ "${#hits[@]}" -le 1 ] || {
        echo "[menu-ctl] the folder holds several weapon models: name one with --source-model NAME" >&2
        return 2
      }
    fi
    [ -n "$ydr" ] || { echo "[menu-ctl] no weapon model (.ydr${source:+ named $source}) in $mod" >&2; return 2; }
  else
    ydr="$mod"
  fi
  [ ! -L "$ydr" ] || { echo "[menu-ctl] symbolic links are refused: $ydr" >&2; return 2; }
  source="$(convert_lower "$(basename -- "${ydr%.*}")")"
  source="${source%_hi}"  # an HD-only mod's model: <model>_hi.ydr, textures in <model>.ytd
  [[ "$source" =~ ^[a-z0-9_]{1,40}$ ]] || { echo "[menu-ctl] unusable model name $source" >&2; return 2; }
  local ytd
  ytd="$(find "$(dirname -- "$ydr")" -maxdepth 1 -type f -iname "$source.ytd" -print -quit)"
  [ -n "$ytd" ] && [ ! -L "$ytd" ] || { echo "[menu-ctl] no $source.ytd next to $(basename -- "$ydr")" >&2; return 2; }
  # --attachments: the folder's models named like the donor's component models, each with its .ytd and a cached
  # retail template of its name.
  local -a parts=()
  if [ -n "$attachments" ]; then
    local wanted part stem
    wanted="$(sed -n 's/.* component-models=\([a-z0-9_,]*\).*/\1/p' <<< "$donor_line")"
    if [ -d "$mod" ]; then
      while IFS= read -r -d '' part; do
        stem="$(convert_lower "$(basename -- "${part%.*}")")"
        case ",$wanted," in *",$stem,"*) ;; *) continue ;; esac
        if [ -L "$part" ] || [ -z "$(find "$mod" -maxdepth 1 -type f -iname "$stem.ytd" -print -quit)" ]; then
          echo "[menu-ctl] attachment $stem: no $stem.ytd next to it (or a symbolic link): left out"
        elif [ ! -f "$CV_TEMPLATES/weapons/$stem.pdr" ]; then
          echo "[menu-ctl] attachment $stem: no retail template weapons/$stem.pdr (run $0 fetch-templates): left out"
        else
          parts+=("$part")
        fi
      done < <(find "$mod" -maxdepth 1 -type f -iname '*.ydr' ! -iname '*_hi.ydr' -print0 | LC_ALL=C sort -z)
    fi
    [ "${#parts[@]}" -gt 0 ] || echo "[menu-ctl] --attachments: the mod has no model named like a component of" \
      "$like ($wanted); converting the weapon alone"
  fi
  if [ -z "$template" ] && [ ! -f "$CV_TEMPLATES/weapons/$source.pdr" ] && [ -n "$donor_model" ] &&
    [ -f "$CV_TEMPLATES/weapons/$donor_model.pdr" ]; then
    echo "[menu-ctl] no retail drawable named $source: converting onto the donor's model $donor_model (the mod's" \
      "skeleton must equal that model's)"
    template="$CV_TEMPLATES/weapons/$donor_model.pdr"
  fi
  convert_template "weapons/$source.pdr" --template convert-weapon "$template" || {
    [ -n "$CV_TEMPLATES" ] && [ -z "${template:-}" ] &&
      echo "[menu-ctl] (or the donor's model weapons/$donor_model.pdr; the cache has:" \
        "$(find "$CV_TEMPLATES/weapons" -maxdepth 1 -name '*.pdr' ! -name '*_hi.pdr' ! -name '*_mag*' \
          -printf '%f ' 2> /dev/null))" >&2
    return 2
  }
  template="$CV_FILE"

  convert_start
  convert_copy "$ydr" || return 2
  local pdr_in="$CV_COPY"
  convert_copy "$ytd" || return 2
  local ytd_in="$CV_COPY" out="$CV_WORK/model"
  mkdir -p -- "$out"
  echo "[menu-ctl] converting $(basename -- "$ydr") onto the retail $(basename -- "${template%.pdr}") drawable"
  convert_weapon_drawable "$model_tool" "$pdr_in" "$ytd_in" "$template" "$out/$source" "$CV_WORK/drawable.log" \
    "$max_texture" "${shaders[@]}" || return 1
  local -a extra=(--orders "${orders:-auto}")
  if [ "${#parts[@]}" -gt 0 ]; then
    local part_in part_ytd stem
    mkdir -p -- "$CV_WORK/components"
    extra+=(--components "$cmeta")
    for part in "${parts[@]}"; do
      stem="$(convert_lower "$(basename -- "${part%.*}")")"
      convert_copy "$part" || return 2
      part_in="$CV_COPY"
      convert_copy "$(find "$(dirname -- "$part")" -maxdepth 1 -type f -iname "$stem.ytd" -print -quit)" || return 2
      part_ytd="$CV_COPY"
      echo "[menu-ctl] converting attachment $stem.ydr onto the retail $stem drawable"
      convert_weapon_drawable "$drawable_tool" "$part_in" "$part_ytd" "$CV_TEMPLATES/weapons/$stem.pdr" \
        "$CV_WORK/components/$stem" "$CV_WORK/drawable-$stem.log" "$max_texture" || return 1
      extra+=(--attachment "$CV_WORK/components/$stem.pdr")
    done
  fi
  convert_run "$CV_WORK/weapon.log" "$PY" -I "$pack_tool" --pdr "$out/$source.pdr" --ptd "$out/$source.ptd" \
    --weapons "$wmeta" --animations "$ameta" --archetypes "$rmeta" --source-model "$source" \
    --source "$like" --model "$model" --weapon "$weapon" --slot "$slot" --label "$label" "${extra[@]}" --tint "$tint" \
    --tint-palette "$tints" --text "$text" --archive "$archive" --meta-prefix "$prefix" --out "$CV_WORK/src" \
    --build "$CV_ID" --output-root "$CV_PACKS" || return 1
  grep -v '^+ ' "$CV_WORK/weapon.log" | grep -E '^(class=|component |note:|wrote|recoloured|tints: )' |
    sed 's/^/[menu-ctl]   /'
  echo "[menu-ctl] in game after Load: Weapon Browser -> Custom -> $text (or $0 give-weapon $(convert_lower "$weapon"))"
  convert_finish convert-weapon "$install"
}

# convert_weapon_drawable TOOL YDR YTD TEMPLATE OUT_STEM LOG MAX_TEXTURE [--shader-template F ...]: OUT_STEM.pdr/.ptd.
# Shader schemas: the given --shader-template rows, else the template's own family (<model>_hi, _mag1). When the
# family lacks a shader the mod uses, or its members disagree on a schema (w_pi_pistol and w_mg_minigun carry an
# older palette shader than their _hi), the retry takes every cached magazine drawable instead (normal_spec and
# spec, one schema each). When those lack a shader too (a mod on `default`, a scope lens exported as vehicle
# glass), the last retry takes each shader from the first cached weapon drawable that has it (family, magazines,
# then the rest in byte order) and retargets the known PC-only ones (vehicle_vehglass -> weapon_normal_spec_alpha).
convert_weapon_drawable() {
  local tool="$1" ydr="$2" ytd="$3" template="$4" stem="$5" log="$6" max_texture="$7" file name
  shift 7
  local -a shaders=("$@") fallback=() everything=()
  name="$(basename -- "${template%.pdr}")"
  if [ "${#shaders[@]}" -eq 0 ]; then
    while IFS= read -r -d '' file; do
      [ "$file" = "$template" ] && continue
      case "$(basename -- "$file")" in "${name}"_*.pdr) shaders+=(--shader-template "$file") ;; esac
      case "$(basename -- "$file")" in *_mag[0-9]*.pdr) fallback+=(--shader-template "$file") ;; esac
    done < <(find "$CV_TEMPLATES/weapons" -maxdepth 1 -type f -name '*.pdr' -print0 2> /dev/null | LC_ALL=C sort -z)
    if [ "${#fallback[@]}" -gt 0 ]; then
      everything=("${shaders[@]}" "${fallback[@]}")
      while IFS= read -r -d '' file; do
        [ "$file" = "$template" ] && continue
        case " ${everything[*]} " in *" $file "*) ;; *) everything+=(--shader-template "$file") ;; esac
      done < <(find "$CV_TEMPLATES/weapons" -maxdepth 1 -type f -name '*.pdr' -print0 2> /dev/null | LC_ALL=C sort -z)
    fi
  fi
  local -a drawable=("${CV_PY[@]}" "$tool" --source "$ydr" --template "$template")
  local -a outputs=(--output "$stem.pdr" --ytd "$ytd" --ptd-output "$stem.ptd" --max-texture-size "$max_texture"
    --templates "$CV_TEMPLATES" --reference-dir "$CV_REFERENCE")
  local missing='no retail template carries shader[^;]*|native shader schema differs'
  if ! "${drawable[@]}" "${shaders[@]}" "${outputs[@]}" > "$log" 2>&1; then
    if [ "${#fallback[@]}" -gt 0 ] && grep -qE "$missing" "$log"; then
      echo "[menu-ctl] the $name family cannot give the mod's shader schemas" \
        "($(grep -oE "$missing" "$log" | head -n 1)): taking them from the cached magazine drawables"
      if ! "${drawable[@]}" "${fallback[@]}" "${outputs[@]}" > "$log" 2>&1; then
        if grep -qE "$missing" "$log"; then
          echo "[menu-ctl] the magazines cannot either ($(grep -oE "$missing" "$log" | head -n 1)): taking each" \
            "shader from the first cached weapon drawable that has it"
          convert_run "$log" "${drawable[@]}" "${everything[@]}" --first-carrier --shader-substitute auto \
            "${outputs[@]}" || {
            echo "[menu-ctl] give a retail drawable that uses the mod's shader with --shader-template FILE" >&2
            return 1
          }
        else
          tail -n 20 "$log" >&2
          echo "[menu-ctl] conversion stopped ($(basename -- "$log")); nothing was installed" \
            "(the copied mod: $CV_INPUT)" >&2
          return 1
        fi
      fi
    else
      tail -n 20 "$log" >&2
      echo "[menu-ctl] conversion stopped ($(basename -- "$log")); nothing was installed (the copied mod: $CV_INPUT)" >&2
      return 1
    fi
  fi
  # (the tint plan and its palette stand-in: make_weapon_model_pack.py reports the tints)
  grep -v -e '^tint: ' -e 'lacks gm_weapon_tint_pal' "$log" | sed 's/^/[menu-ctl]   /'
}

# Host: a PC add-on vehicle (its dlc.rpf) as a pack vehicle (convert_vehicle.py): import,
# the exporter-quirk repairs you name, material reports, the converter's references stage on the frozen constants,
# LODs, collision octants, textures, metas (handling, carcols + mod kit, layouts, vehicles, variations) and the pack.
# Retail inputs come only from the template cache (fetch-templates; the encrypted vehiclelayouts.meta, update.rpf's
# copy, and the base audio data that --audio is checked against, from export-templates); the converter's work files go to build/assets/convert-<id> (it writes only there).
# Kit, light and siren ids default to the lowest ids of the reserved ranges that no pack in <build>/custom-assets
# uses (one per kit and per light item); archive tables are plain and keyed on the console. Exporter quirks: --repair auto (and --parts-repair auto) unless repairs are named; a refused step
# ends with one line naming the --repair or --parts-repair to add. --siren-preset/--siren-spec replace the mod's
# sirens by one pattern item (lamps from the skeleton's siren bones unless --siren-lamps); custom wheels whose
# models the mod does not ship are dropped (shipped wheel models refuse unless --drop-wheels).
do_convert_vehicle() {
  local usage="usage: $0 convert-vehicle MOD --id PACK_ID --model NAME [--name TEXT] [--mod-kit]
       [--make-label KEY=TEXT | --make NAME] [--kit-id N] [--light-id N] [--siren-id N | --drop-sirens]
       [--siren-preset NAME | --siren-spec FILE] [--siren-lamps KEYS] [--drop-wheels] [--max-texture-size N]
       [--part-texture-size N] [--lower-lods auto|copy|empty]
       [--audio NAME [--audio-unchecked]] [--audio-engine NAME] [--driveby NAME ...]
       [--repair NAME[=BONE,...] ...] [--parts-repair NAME[=BONE,...] ...]
       [--archive NAME.rpf] [--parts-archive NAME.rpf] [--templates DIR] [--retail-layouts FILE]
       [--dry-run] [--install|--activate]
  MOD: the add-on's dlc.rpf, its .oiv/.zip package (one dlc.rpf inside), or a folder holding exactly one dlc.rpf
  --model: the add-on's model name (its <model>.yft); --mod-kit also converts its LSC parts (vehiclemods
  and the parts its kits list)
  repairs: default auto (every repair that needs no bones, detected; none = no repair); a refused step names
  the one to add, e.g. seat-mirror=BONES beside auto (--repair seat-mirror=A,B --repair auto); for the
  --mod-kit parts --parts-repair, e.g. inverse-precision=BONES
  sirens: --siren-preset test-white-slow|classic-wigwag|fast-strobe|rotating-beacon or --siren-spec FILE.json
  (one pattern item, id --siren-id or the lowest free); --siren-lamps 20 keys for siren1..20 ('-' unused;
  default from the car's siren bones)
  audio: default the mod's audioNameHash when it names another car, else a base-game sound of the vehicle class
  (BANSHEE for VC_SPORT, ADDER for VC_SUPER, ...); --audio NAME is checked against your game's base car sounds
  (export-templates; --audio-unchecked for a DLC car's); --audio-engine NAME: own sound = that sound with NAME's
  engine
  --make-label: a key starting with GM, at most 11 characters for the game's own name popup (e.g. GMA80_MAKE);
  the converter gives the car a pack-owned name key too (the mod's gameName may be the game's: TURISMOR)
  --part-texture-size N: largest size of the textures LSC parts carry themselves (livery parts: 2048 each), which
  join the car's .ptd; --lower-lods empty: no medium/low/very-low copies of a heavy model (default auto)"
  local mod="" id="" install="" model="" name="" make_label="" make="" kit="" light="" siren="" drop="" audio=""
  local archive="" parts_archive="" templates="" layouts="" dry="" modkit="" value
  local preset="" spec="" lamps="" max_size="" drop_wheels="" audio_unchecked="" audio_engine="" part_size="" lower=""
  local -a repairs=() drivebys=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --id | --model | --name | --make-label | --make | --kit-id | --light-id | --siren-id | --audio | --driveby | \
        --repair | --parts-repair | --archive | --parts-archive | --templates | --retail-layouts | \
        --siren-preset | --siren-spec | --siren-lamps | --max-texture-size | --audio-engine | --part-texture-size | \
        --lower-lods)
        [ "$#" -ge 2 ] || { echo "$usage" >&2; return 2; }
        value="$2"
        case "$1" in
          --id) id="$value" ;;
          --model) model="$value" ;;
          --name) name="$value" ;;
          --make-label) make_label="$value" ;;
          --make) make="$value" ;;
          --kit-id) kit="$value" ;;
          --light-id) light="$value" ;;
          --siren-id) siren="$value" ;;
          --audio) audio="$value" ;;
          --audio-engine) audio_engine="$value" ;;
          --driveby) drivebys+=(--driveby "$value") ;;
          --repair | --parts-repair)
            [[ "$value" =~ ^[a-z-]+(=[A-Za-z0-9_]+(,[A-Za-z0-9_]+)*)?$ ]] || {
              echo "[menu-ctl] $1 takes NAME or NAME=BONE,BONE (e.g. seat-mirror=seat_dside_f,seat_pside_f)" >&2
              return 2
            }
            repairs+=("$1" "$value")
            ;;
          --archive) archive="$value" ;;
          --parts-archive) parts_archive="$value" ;;
          --templates) templates="$(convert_abs "$value")" ;;
          --retail-layouts) layouts="$(convert_abs "$value")" ;;
          --siren-preset) preset="$value" ;;
          --siren-spec) spec="$(convert_abs "$value")" ;;
          --siren-lamps) lamps="$value" ;;
          --max-texture-size) max_size="$value" ;;
          --part-texture-size) part_size="$value" ;;
          --lower-lods) lower="$value" ;;
        esac
        shift 2
        ;;
      --mod-kit | --drop-sirens | --drop-wheels | --dry-run | --audio-unchecked)
        case "$1" in
          --audio-unchecked) audio_unchecked=1 ;;
          --mod-kit) modkit=1 ;;
          --drop-sirens) drop=1 ;;
          --drop-wheels) drop_wheels=1 ;;
          --dry-run) dry=1 ;;
        esac
        shift
        ;;
      --install | --activate)
        [ -z "$install" ] || { echo "$usage" >&2; return 2; }
        install="$1"
        shift
        ;;
      -*) echo "$usage" >&2; return 2 ;;
      *)
        [ -z "$mod" ] || { echo "$usage" >&2; return 2; }
        mod="$1"
        shift
        ;;
    esac
  done
  [ -n "$mod" ] && [ -n "$id" ] && [ -n "$model" ] || { echo "$usage" >&2; return 2; }
  convert_setup convert-vehicle "$id" || return 2
  CV_TEMPLATES="${templates:-$CV_TEMPLATES}"
  [[ "$model" =~ ^[a-z0-9_]{1,40}$ ]] || {
    echo "[menu-ctl] --model takes the add-on's model name in lower case (its <model>.yft, e.g. a80)" >&2
    return 2
  }
  name="${name:-$model}"
  [[ "$name" =~ ^[[:print:]]{1,40}$ ]] || { echo "[menu-ctl] --name is 1-40 printable characters" >&2; return 2; }
  if [ -n "$make_label" ] && [ -n "$make" ]; then
    echo "[menu-ctl] give --make-label KEY=TEXT (a new make label) or --make NAME (a retail one), not both" >&2
    return 2
  fi
  [ -z "$make_label" ] || [[ "$make_label" =~ ^[A-Za-z0-9_]{1,63}=[[:print:]]{1,63}$ ]] || {
    echo "[menu-ctl] --make-label takes KEY=TEXT, KEY [A-Za-z0-9_] (e.g. GMPRT_MAKE=Grotti)" >&2
    return 2
  }
  for value in "$make" "$audio" "$audio_engine" "${drivebys[@]}"; do
    [ "$value" = --driveby ] || [ -z "$value" ] || [[ "$value" =~ ^[A-Za-z0-9_]{1,63}$ ]] || {
      echo "[menu-ctl] --make, --audio, --audio-engine and --driveby take retail names ([A-Za-z0-9_], e.g. --audio BANSHEE)" >&2
      return 2
    }
  done
  for value in "$kit" "$light" "$siren"; do
    [ -z "$value" ] || [[ "$value" =~ ^[0-9]{1,5}$ ]] || {
      echo "[menu-ctl] --kit-id, --light-id and --siren-id take numbers (default: the lowest free reserved id)" >&2
      return 2
    }
  done
  if [ -n "$siren" ] && [ -n "$drop" ]; then
    echo "[menu-ctl] give --siren-id N (keep the mod's sirens from id N) or --drop-sirens, not both" >&2
    return 2
  fi
  if [ -n "$preset" ] && [ -n "$spec" ]; then
    echo "[menu-ctl] give --siren-preset NAME or --siren-spec FILE, not both" >&2
    return 2
  fi
  if [ -n "$preset$spec" ] && [ -n "$drop" ]; then
    echo "[menu-ctl] --siren-preset/--siren-spec replace the mod's sirens; --drop-sirens drops them: not both" >&2
    return 2
  fi
  [ -z "$preset" ] || [[ "$preset" =~ ^[a-z0-9-]{1,40}$ ]] || {
    echo "[menu-ctl] --siren-preset takes a preset name (e.g. fast-strobe; tools/prepare_carcols.py --list-siren-presets)" >&2
    return 2
  }
  [ -z "$spec" ] || [ -f "$spec" ] || { echo "[menu-ctl] --siren-spec names no file: $spec" >&2; return 2; }
  if [ -n "$lamps" ]; then
    [ -n "$preset$spec" ] || { echo "[menu-ctl] --siren-lamps goes with --siren-preset or --siren-spec" >&2; return 2; }
    [[ "$lamps" =~ ^[A-Za-z0-9-]{20}$ ]] || {
      echo "[menu-ctl] --siren-lamps takes 20 group keys for siren1..siren20, '-' unused (e.g. LLRR------------LRLR)" >&2
      return 2
    }
  fi
  for value in "$max_size" "$part_size"; do
    [ -z "$value" ] || [[ "$value" =~ ^(4|8|16|32|64|128|256|512|1024|2048|4096|8192)$ ]] || {
      echo "[menu-ctl] --max-texture-size and --part-texture-size take a power of two (e.g. 1024)" >&2
      return 2
    }
  done
  [ -z "$lower" ] || [[ "$lower" =~ ^(auto|copy|empty)$ ]] || {
    echo "[menu-ctl] --lower-lods takes auto (default), copy or empty" >&2
    return 2
  }
  if [ -n "$dry" ] && [ -n "$install" ]; then
    echo "[menu-ctl] --dry-run builds nothing to install; drop $install" >&2
    return 2
  fi
  archive="${archive:-$(convert_short "gmveh_$CV_SLUG" 59).rpf}"
  parts_archive="${parts_archive:-$(convert_short "gmvehp_$CV_SLUG" 59).rpf}"
  for value in "$archive" "$parts_archive"; do
    [[ "$value" =~ ^[a-z0-9_]{1,59}\.rpf$ ]] || {
      echo "[menu-ctl] --archive and --parts-archive take a lowercase NAME.rpf" >&2
      return 2
    }
  done
  [ "$archive" != "$parts_archive" ] || { echo "[menu-ctl] --archive and --parts-archive must differ" >&2; return 2; }
  [ -z "$layouts" ] || [ -f "$layouts" ] || { echo "[menu-ctl] --retail-layouts names no file: $layouts" >&2; return 2; }
  mod="$(convert_abs "$mod")"
  convert_check_source "$mod" ".rpf .oiv .zip" convert-vehicle || return 2
  local rpf="$mod"
  if [ -d "$mod" ]; then
    local -a hits=()
    mapfile -d '' -t hits < <(find "$mod" -type f -iname 'dlc.rpf' -print0 | LC_ALL=C sort -z)
    [ "${#hits[@]}" -eq 1 ] || {
      echo "[menu-ctl] the folder must hold exactly one add-on dlc.rpf; it has ${#hits[@]}: give the dlc.rpf itself" >&2
      [ "${#hits[@]}" -eq 0 ] || printf '[menu-ctl]   %s\n' "${hits[@]}" >&2
      return 2
    }
    rpf="${hits[0]}"
  fi
  local tool
  convert_tool convert_vehicle convert-vehicle || return 2
  tool="$CV_TOOL"
  convert_python convert-vehicle numpy || return 2
  convert_template corpus/tornado6.pft "" convert-vehicle || return 2
  # The converter's own work directory (fixtures, reports, partial resources): rebuilt with the pack it belongs to.
  local assets_work="$CV_BUILD/assets/convert-$CV_ID"
  if [ -e "$assets_work" ] && [ ! -f "$CV_WORK/pack-built" ] && [ -z "$dry" ]; then
    echo "[menu-ctl] $assets_work exists and was not made by convert-vehicle: pick another --id or remove it" >&2
    return 2
  fi
  local -a opts=(--model "$model" --id "$CV_ID" --name "$name" --templates "$CV_TEMPLATES" --output-root "$CV_PACKS"
    --ids-from "$CV_PACKS" --archive "$archive" --parts-archive "$parts_archive" "${repairs[@]}" "${drivebys[@]}")
  [ -z "$modkit" ] || opts+=(--mod-kit)
  [ -z "$make_label" ] || opts+=(--make-label "$make_label")
  [ -z "$make" ] || opts+=(--make "$make")
  [ -z "$kit" ] || opts+=(--kit-id "$kit")
  [ -z "$light" ] || opts+=(--light-id "$light")
  [ -z "$siren" ] || opts+=(--siren-id "$siren")
  [ -z "$drop" ] || opts+=(--drop-sirens)
  [ -z "$audio" ] || opts+=(--audio "$audio")
  [ -z "$audio_unchecked" ] || opts+=(--audio-unchecked)
  [ -z "$audio_engine" ] || opts+=(--audio-engine "$audio_engine")
  [ -z "$layouts" ] || opts+=(--retail-layouts "$layouts")
  [ -z "$preset" ] || opts+=(--siren-preset "$preset")
  [ -z "$spec" ] || opts+=(--siren-spec "$spec")
  [ -z "$lamps" ] || opts+=(--siren-lamps "$lamps")
  [ -z "$max_size" ] || opts+=(--max-texture-size "$max_size")
  [ -z "$part_size" ] || opts+=(--part-texture-size "$part_size")
  [ -z "$lower" ] || opts+=(--lower-lods "$lower")
  [ -z "$drop_wheels" ] || opts+=(--drop-wheels)
  if [ -n "$dry" ]; then
    echo "[menu-ctl] dry run: the converter's steps for $(basename -- "$rpf") (nothing is written)"
    "${CV_PY[@]}" "$tool" --source "$rpf" --dry-run "${opts[@]}"
    return
  fi

  convert_start
  rm -rf -- "$assets_work"
  convert_copy "$rpf" || return 2
  echo "[menu-ctl] converting $(basename -- "$rpf") ($model) into $CV_PACK; this takes a few minutes" \
    "(progress: $CV_WORK/convert.log)"
  convert_run "$CV_WORK/convert.log" "${CV_PY[@]}" "$tool" --source "$CV_COPY" "${opts[@]}" || return 1
  grep -E '^     (ids|metas|vehicles\.meta|sirens|wheels|audio|parts|textures|liveries):|^note: --make-label|^done:' "$CV_WORK/convert.log" | sed 's/^ */[menu-ctl]   /'
  echo "[menu-ctl] in game after Load: Vehicle Browser -> Custom -> $name (or $0 spawn-model $model)"
  convert_finish convert-vehicle "$install"
}

# Host: a PC car mod's own wheel models (the <Wheels> items of its carcols) to a wheel pack:
# tools/convert_pc_wheel.py converts each <wheelName>.ydr into <wheelname>.pdr with its textures embedded
# (the engine finds a wheel's drawable by its name and gives it the vehicle-mod texture chain, so a pack .ptd would
# not reach it), writes the carcols row through tools/prepare_carcols.py --ship-wheel and builds the pack. In game:
# Vehicle -> LS Customs -> Wheel Type (apply the wheels' type first) -> Wheels; the real LS Customs shop lists a fixed
# set of designs and never shows pack wheels.
do_convert_wheels() {
  local usage="usage: $0 convert-wheels MOD --id PACK_ID [--type TYPE] [--only NAME ...] [--label NAME=TEXT ...]
       [--label-prefix PREFIX] [--archive NAME.rpf] [--description TEXT] [--max-texture-size N] [--templates DIR]
       [--list] [--install|--activate]
  MOD: the car mod as downloaded: its .oiv/.zip package, its dlc.rpf, or the mod folder
  --type: put every wheel in this wheel type (sport, muscle, lowrider, suv, offroad, tuner, bike, highend,
          bennys-original, bennys-bespoke, open-wheel, street, track); default: the type the mod gives it
  --only: convert only these wheels (<wheelName>, repeatable); --list prints the mod's wheels and writes nothing
  --label: the shop name of one wheel (default: the mod's own text, else its label key, e.g. Chiron Classique 01)"
  local mod="" id="" install="" type="" prefix="" archive="" description="" max_size="" templates="" list="" value
  local -a only=() labels=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --id | --type | --only | --label | --label-prefix | --archive | --description | --max-texture-size | --templates)
        [ "$#" -ge 2 ] || { echo "$usage" >&2; return 2; }
        value="$2"
        case "$1" in
          --id) id="$value" ;;
          --type) type="$value" ;;
          --only)
            [[ "$value" =~ ^[A-Za-z0-9_]{1,63}$ ]] || {
              echo "[menu-ctl] --only takes a wheel name (<wheelName>, [A-Za-z0-9_])" >&2
              return 2
            }
            only+=(--only "$value")
            ;;
          --label)
            [[ "$value" =~ ^[A-Za-z0-9_]{1,63}=[[:print:]]{1,63}$ ]] || {
              echo "[menu-ctl] --label takes NAME=TEXT (a wheel name and its shop name, e.g. wheel_x_01=\"Mesh Rim\")" >&2
              return 2
            }
            labels+=(--label "$value")
            ;;
          --label-prefix) prefix="$value" ;;
          --archive) archive="$value" ;;
          --description) description="$value" ;;
          --max-texture-size) max_size="$value" ;;
          --templates) templates="$(convert_abs "$value")" ;;
        esac
        shift 2
        ;;
      --list)
        list=1
        shift
        ;;
      --install | --activate)
        [ -z "$install" ] || { echo "$usage" >&2; return 2; }
        install="$1"
        shift
        ;;
      -*) echo "$usage" >&2; return 2 ;;
      *)
        [ -z "$mod" ] || { echo "$usage" >&2; return 2; }
        mod="$1"
        shift
        ;;
    esac
  done
  [ -n "$mod" ] && { [ -n "$id" ] || [ -n "$list" ]; } || { echo "$usage" >&2; return 2; }
  convert_setup convert-wheels "${id:-wheel-list}" || return 2
  CV_TEMPLATES="${templates:-$CV_TEMPLATES}"
  case "$type" in
    '' | sport | muscle | lowrider | suv | offroad | tuner | bike | highend | bennys-original | bennys-bespoke | \
      open-wheel | street | track) ;;
    *)
      echo "[menu-ctl] --type takes a wheel type: sport, muscle, lowrider, suv, offroad, tuner, bike, highend," \
        "bennys-original, bennys-bespoke, open-wheel, street, track" >&2
      return 2
      ;;
  esac
  [ -z "$prefix" ] || [[ "$prefix" =~ ^[A-Za-z0-9_]{1,40}$ ]] || {
    echo "[menu-ctl] --label-prefix takes [A-Za-z0-9_], at most 40 characters (labels PREFIX_WHEEL_<n>)" >&2
    return 2
  }
  [ -z "$archive" ] || [[ "$archive" =~ ^[a-z0-9_]{1,59}\.rpf$ ]] || {
    echo "[menu-ctl] --archive takes a lowercase NAME.rpf" >&2
    return 2
  }
  [ "${#description}" -le 80 ] || { echo "[menu-ctl] --description is at most 80 characters" >&2; return 2; }
  [ -z "$max_size" ] || [[ "$max_size" =~ ^(4|8|16|32|64|128|256|512|1024|2048|4096|8192)$ ]] || {
    echo "[menu-ctl] --max-texture-size takes a power of two (e.g. 1024)" >&2
    return 2
  }
  if [ -n "$list" ] && [ -n "$install" ]; then
    echo "[menu-ctl] --list builds nothing to install; drop $install" >&2
    return 2
  fi
  mod="$(convert_abs "$mod")"
  convert_check_source "$mod" ".rpf .oiv .zip" convert-wheels || return 2
  local tool
  convert_tool convert_pc_wheel convert-wheels || return 2
  tool="$CV_TOOL"
  convert_python convert-wheels numpy || return 2
  local -a opts=(--templates "$CV_TEMPLATES" --reference-dir "$CV_REFERENCE")
  [ -z "$max_size" ] || opts+=(--max-texture-size "$max_size")
  if [ -n "$list" ]; then
    # The listing reads the mod in place (an .oiv is unpacked into a scratch work directory, removed after).
    local scratch
    scratch="$(mktemp -d "${TMPDIR:-/tmp}/gtavmenu-wheel-list.XXXXXXXX")" || return 2
    if "${CV_PY[@]}" "$tool" pack --source "$mod" --work "$scratch" --list "${opts[@]}"; then value=0; else value=$?; fi
    rm -rf -- "$scratch"
    return "$value"
  fi
  convert_template wheels/wheel_loride_01.pdr "" convert-wheels || return 2
  convert_template corpus/vehshare-a.ptd "" convert-wheels || return 2
  [ -z "$type" ] || opts+=(--type "$type")
  [ -z "$prefix" ] || opts+=(--label-prefix "$prefix")
  [ -z "$description" ] || opts+=(--description "$description")
  opts+=(--archive "${archive:-$(convert_short "gmwhl_$CV_SLUG" 59).rpf}" "${only[@]}" "${labels[@]}")
  convert_start
  convert_copy "$mod" || return 2
  echo "[menu-ctl] converting the wheels of $(basename -- "$mod") into $CV_PACK (progress: $CV_WORK/convert.log)"
  convert_run "$CV_WORK/convert.log" "${CV_PY[@]}" "$tool" pack --source "$CV_COPY" --id "$CV_ID" \
    --work "$CV_WORK/work" --output-root "$CV_PACKS" "${opts[@]}" || return 1
  grep -E '^(wheel |note: )' "$CV_WORK/convert.log" | sed 's/^/[menu-ctl]   /' || true
  echo "[menu-ctl] in game after Load: Vehicle -> LS Customs -> Wheel Type (the wheels' type, Cross applies) ->" \
    "Wheels: the pack's designs are the last ones (\"N/M <name>\")"
  convert_finish convert-wheels "$install"
}

# Read packs/installed and packs/active over FTP (no changes); see tools/upload_runtime_pack.py --list.
do_pack_list() {
  [ "$#" -eq 0 ] || { echo "usage: $0 pack-list" >&2; return 2; }
  "$PY" tools/upload_runtime_pack.py --list --host "$PS5_HOST" --port "$PS5_FTP_PORT"
}

# The running session's "GTAVMenu pack ..." lines, written by the worker about once a second
# (src/module/klog.c). Survives klog delivery gaps and the status ring; overwritten per session. The worker keeps
# only the last 128 lines: --follow polls the file (every --interval seconds, default 1) and appends each line
# once to --out (default build/pack-notes/<time>.log) and stdout, so a busy multi-pack load loses nothing; lines
# that still left the ring between two polls show as a `# GAP:` line (tools/pack_notes.py merge). Runs until
# Ctrl-C, or --for SECONDS.
do_pack_notes() {
  local usage="usage: $0 pack-notes [--follow [--out FILE] [--interval SECONDS] [--for SECONDS]]"
  local follow="" out="" interval=1 limit=""
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --follow) follow=1; shift ;;
      --out | --interval | --for)
        [ "$#" -ge 2 ] || { echo "$usage" >&2; return 2; }
        case "$1" in
          --out) out="$2" ;;
          --interval) interval="$2" ;;
          --for) limit="$2" ;;
        esac
        shift 2
        ;;
      *) echo "$usage" >&2; return 2 ;;
    esac
  done
  [[ "$interval" =~ ^[1-9][0-9]{0,2}$ ]] && { [ -z "$limit" ] || [[ "$limit" =~ ^[1-9][0-9]{0,5}$ ]]; } || {
    echo "$usage (whole seconds)" >&2
    return 2
  }
  if [ -z "$follow" ]; then
    [ -z "$out$limit" ] && [ "$interval" = 1 ] || { echo "$usage" >&2; return 2; }
  else
    out="${out:-$ROOT/build/pack-notes/$(date +%Y%m%d-%H%M%S).log}"
    mkdir -p -- "$(dirname -- "$out")" || return 1
    local state="$out.state" started=$SECONDS
    rm -f -- "$state"
    echo "[menu-ctl] following pack notes every ${interval}s into $out (Ctrl-C stops)" >&2
    while [ -z "$limit" ] || [ $((SECONDS - started)) -lt "$limit" ]; do
      game_custom_cat pack-notes.log 2> /dev/null | "$PY" -I "$ROOT/tools/pack_notes.py" merge --state "$state" |
        tee -a -- "$out"
      sleep "$interval"
    done
    return 0
  fi
  local notes
  notes="$(game_custom_cat pack-notes.log)"
  if [ -z "$notes" ]; then
    echo "[menu-ctl] no pack notes at /data/gtavmenu/custom/pack-notes.log; custom assets require shared game /data" >&2
    return 1
  fi
  printf '%s\n' "$notes"
}

# A run's pack-notes lines against its expectation file (pass:/pass>=N:/fail: regex rules; tools/pack_notes.py
# check): the followed log given as NOTES (pack-notes --follow --out), else the console's current pack-notes.
do_pack_check() {
  local usage="usage: $0 pack-check EXPECT_FILE [NOTES_FILE]"
  [ "$#" -ge 1 ] && [ "$#" -le 2 ] || { echo "$usage" >&2; return 2; }
  [ -f "$1" ] || { echo "[menu-ctl] no expectation file: $1" >&2; return 2; }
  if [ "$#" -eq 2 ]; then
    [ -f "$2" ] || { echo "[menu-ctl] no notes file: $2" >&2; return 2; }
    "$PY" -I "$ROOT/tools/pack_notes.py" check "$1" "$2"
    return
  fi
  require_console_host || return 2
  local notes
  notes="$(game_custom_cat pack-notes.log)"
  [ -n "$notes" ] || { echo "[menu-ctl] no pack notes yet" >&2; return 1; }
  printf '%s\n' "$notes" | "$PY" -I "$ROOT/tools/pack_notes.py" check "$1"
}

# Custom Packs selection and one-press load (worker-side actions; see custom_pack_select.inc).
do_pack_select() {
  case "${1:-}" in
    ''|*[!0-9]*) echo "usage: $0 pack-select <installed pack index>" >&2; return 2 ;;
  esac
  [ "$#" -eq 1 ] || { echo "usage: $0 pack-select <installed pack index>" >&2; return 2; }
  pack_action_param TOGGLE_PACK_ACTIVE toggle_pack_active "$1"
}

# Debug undo of the one-press load's `hide` rows: REMOVE_MODEL_HIDE for every applied row (param
# 0xffff of PACK_HIDE; custom_pack_hide.inc). Each removed row is a "pack unhide" pack-notes line.
do_pack_unhide() {
  [ "$#" -eq 0 ] || { echo "usage: $0 pack-unhide" >&2; return 2; }
  pack_action_param PACK_HIDE pack_hide 65535
}

do_pack_autoload() {
  [ "$#" -eq 0 ] || { echo "usage: $0 pack-autoload" >&2; return 2; }
  pack_action_param PACK_AUTOLOAD pack_autoload 0
  echo "[menu-ctl] progress and the result arrive as toasts / 'pack autoload done|failed' status events"
}

do_pack_card_show() {
  [ "$#" -eq 0 ] || { echo "usage: $0 pack-card-show" >&2; return 2; }
  pack_action SHOW_PACK_CARD show_pack_card
  echo "[menu-ctl] expect 'pack card shown dict=<dict> tex=<texture> res=<w>x<h>' in: $0 status"
}

do_pack_card_release() {
  [ "$#" -eq 0 ] || { echo "usage: $0 pack-card-release" >&2; return 2; }
  pack_action RELEASE_PACK_CARD release_pack_card
  echo "[menu-ctl] expect 'pack card released dict=<dict> loaded=<0|1>' in: $0 status within a second"
}

do_map_upload() {
  if [ "$#" -gt 1 ]; then
    echo "usage: $0 map-upload [package-dir]" >&2
    return 2
  fi
  local package="${1:-build/maps/gtavmenu-test-yard}"
  if [ "$#" -eq 0 ]; then
    make custom-map
  fi
  "$PY" tools/prepare_map.py --verify-package "$package"
  local map_file="$package/map.cfg"
  local readback
  readback="$(mktemp /tmp/gtavmenu-map-readback.XXXXXX)"
  ftp_mkdir "$CUSTOM_REMOTE"
  ftp_mkdir "$CUSTOM_REMOTE/maps"
  if ! ftp_put "$map_file" "$MAP_REMOTE"; then
    rm -f "$readback"
    echo "[menu-ctl] map upload failed" >&2
    return 1
  fi
  if ! curl --fail --silent --show-error --max-time 30 \
      "ftp://$PS5_HOST:$PS5_FTP_PORT$MAP_REMOTE" -o "$readback"; then
    rm -f "$readback"
    echo "[menu-ctl] could not read back uploaded map" >&2
    return 1
  fi
  if ! cmp -s "$map_file" "$readback"; then
    rm -f "$readback"
    echo "[menu-ctl] uploaded map readback differs" >&2
    return 1
  fi
  rm -f "$readback"
  echo "[menu-ctl] verified map uploaded to $MAP_REMOTE"
}

# The loader's sandbox nullfs route was retired after the 2026-10-04 teardown failure (normal title
# exit walked the retained mount into system sandboxes and crashed ShellUI). Its code and commands
# are retired; the old CUSTOM_MOUNT=1 request still refuses before any I/O.
refuse_retired_custom_mount() {
  [ "${CUSTOM_MOUNT:-0}" = "1" ] || return 0
  echo "[menu-ctl] CUSTOM_MOUNT is retired: the sandbox nullfs route failed hardware teardown" >&2
  echo "[menu-ctl] custom assets require console /data shared with GTA by ShadowMountPlus or the HEN setup" >&2
  exit 2
}

# Resolve action ids from the production ABI header instead of copying numeric values into this
# operator script. The header remains the single source of truth as feature actions are added.
native_action_id() {
  local name="$1" ids
  ids="$(sed -n \
    "s/^[[:space:]]*GTAV_NATIVE_SHELL_ACTION_${name}[[:space:]]*=[[:space:]]*\([0-9][0-9]*\),.*/\1/p" \
    include/gtavmenu/native_bridge.h)"
  case "$ids" in
    ''|*[!0-9]*)
      echo "[menu-ctl] could not resolve unique action id for $name" >&2
      return 1
      ;;
  esac
  printf '%s\n' "$ids"
}

do_map_action() {
  if [ "$#" -ne 1 ]; then
    echo "[menu-ctl] internal map action error" >&2
    return 2
  fi
  local action
  action="$(native_action_id "$1")"
  menu_command activate-action --argument "$action"
}

do_map_clear() {
  # Cancellation may report "no active map load"; the mailbox command still acknowledges it.
  # Always follow it with the game-thread deletion action so this is safe during or after a load.
  do_map_action CANCEL_MAP_LOAD
  do_map_action CLEAR_SPAWNED_ALL
}

# Read-only health report: GTA foreground, frame-hook heartbeat, menu status. ps5debug
# is used only for the foreground/status query (safe -- diagnostic, not pre-hook).
do_doctor() {
  echo "[menu-ctl] === doctor ==="
  echo "[menu-ctl] foreground:"
  run_client foreground 2>/dev/null | grep -iE '"pid"|titleId|appVersion' || echo "  (ps5debug unreachable)"
  # Frame-hook heartbeat: the loader broker probes the hook's fire count over a fixed
  # window at inject time (file logging is disabled in the worker, so this loader-side
  # probe -- not the worker's status events -- is what lands in the payload log). A
  # climbing count => the PLAYER_PED_ID native is firing per frame. (The worker's own
  # calls=N (+d) / ctx_hits=M (+e) events are readable via
  # 'status --pid <pid> --address <probe@0x...>'.)
  echo "[menu-ctl] frame-hook heartbeat (loader broker probe, from last inject; read idle):"
  ftp_cat "$PAYLOAD_LOG_REMOTE" 2>/dev/null \
    | grep -aE "broker:|thunk fires=" | tail -3 | sed 's/^/  /' \
    || echo "  (no frame-hook telemetry in payload log yet)"
  echo "[menu-ctl] menu worker status:"
  # Resolve the status block by symbol, as `status` and `menu_command` do: the client's own scan only
  # sweeps small writable anonymous maps, so on the frame-hook lane (and more so on the ptrace-free
  # lane, whose region is RWX) it reports a perfectly healthy worker as missing.
  local doctor_status_arg=""
  doctor_status="$(resolved_symbol_addr gtav_menu_status)"
  [ -n "$doctor_status" ] && doctor_status_arg="--address $doctor_status"
  run_client status --target-id "$TARGET_ID" --hook-status-name "$HOOK_STATUS_NAME" \
    $doctor_status_arg 2>/dev/null \
    | grep -iE '"stateName"|"ticks"|"visible"|"lastErrorName"' | sed 's/^/  /' || echo "  (not running)"
}

parse_start_args() {
  # These defaults mirror make/profiles/production.mk. Command-line switches are deliberate,
  # visible deviations for diagnosis; the normal path is the validated complete profile.
  SHOW_AFTER_START=0
  PERSIST_ENABLE=0
  SCRIPT_GLOBALS_ENABLE="${SCRIPT_GLOBALS:-1}"
  VEHICLE_PREVIEW_ENABLE="${GTAV_MENU_ENABLE_VEHICLE_PREVIEW:-1}"
  INSTRUCTIONAL_SCALEFORM_ENABLE="${GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM:-1}"
  BUTTON_GLYPHS_ENABLE="${GTAV_MENU_ENABLE_BUTTON_GLYPHS:-0}"
  PHASE_RENDERER_ENABLE=1
  INJECT_GUARD_ENABLE=1
  PAD_HOOK_ENABLE=0
  WORKER_KLOG_ENABLE="${WORKER_KLOG:-1}"
  QUIT_GUARD_ENABLE="${QUIT_GUARD:-1}"
  QUIT_GUARD_APPSTATE_ENABLE="${QUIT_GUARD_APPSTATE:-1}"
  PREFLIGHT_ENABLE=1
  CAVE_LANE_ENABLE=1
  GENTLE_POLL_ENABLE="${GENTLE_POLL:-0}"

  for a in "$@"; do
    case "$a" in
      --all)
        echo "[menu-ctl] --all is a retired experimental profile; production features are fixed" >&2
        exit 2
        ;;
      --no-show) SHOW_AFTER_START=0 ;;
      --show) SHOW_AFTER_START=1 ;;
      --persist) PERSIST_ENABLE=1 ;;
      --no-persist) PERSIST_ENABLE=0 ;;
      --script-globals) SCRIPT_GLOBALS_ENABLE=1 ;;
      --no-script-globals) SCRIPT_GLOBALS_ENABLE=0 ;;
      --vehicle-preview) VEHICLE_PREVIEW_ENABLE=1 ;;
      --no-vehicle-preview) VEHICLE_PREVIEW_ENABLE=0 ;;
      --native-scaleform) INSTRUCTIONAL_SCALEFORM_ENABLE=1; BUTTON_GLYPHS_ENABLE=0 ;;
      --inline-glyphs) INSTRUCTIONAL_SCALEFORM_ENABLE=0; BUTTON_GLYPHS_ENABLE=1 ;;
      --no-native-glyphs) INSTRUCTIONAL_SCALEFORM_ENABLE=0; BUTTON_GLYPHS_ENABLE=0 ;;
      --klog) WORKER_KLOG_ENABLE=1 ;;
      --no-klog) WORKER_KLOG_ENABLE=0 ;;
      --quit-guard) QUIT_GUARD_ENABLE=1 ;;
      --no-quit-guard) QUIT_GUARD_ENABLE=0 ;;
      --quit-appstate) QUIT_GUARD_APPSTATE_ENABLE=1 ;;
      --no-quit-appstate) QUIT_GUARD_APPSTATE_ENABLE=0 ;;
      --cave) ;;
      --gentle-poll) GENTLE_POLL_ENABLE=1 ;;
      --no-gentle-poll) GENTLE_POLL_ENABLE=0 ;;
      --force)
        echo "[menu-ctl] --force is unsafe/unsupported: fully relaunch GTA before another inject." >&2
        exit 2
        ;;
      --no-cave|--no-preflight|--stage|--stage=*|--use-pad-hook|--no-pad-hook)
        echo "[menu-ctl] $a is retired and unavailable in the production launcher." >&2
        exit 2
        ;;
      *)
        echo "[menu-ctl] unknown option: $a" >&2
        exit 2
        ;;
    esac
  done
}

# Build the injected menu worker ELF (PLAYER_PED_ID frame-hook variant). Leaves it at
# $FRAME_HOOK_ELF. The menu is useless without controller input, so verify the
# pad-input code actually made it into the binary before injecting.
build_feature_menu_elf() {
  echo "[menu-ctl] building frame-hook menu worker ELF (playerped, pad_input=1, " \
    "pad_hook=$PAD_HOOK_ENABLE, vehicle_preview=$VEHICLE_PREVIEW_ENABLE, " \
    "native_glyphs=$INSTRUCTIONAL_SCALEFORM_ENABLE, worker_klog=$WORKER_KLOG_ENABLE, " \
    "quit_guard=$QUIT_GUARD_ENABLE/appstate=$QUIT_GUARD_APPSTATE_ENABLE, custom_packs=$CUSTOM_PACKS_ENABLE) ..."
  # The profile stamp records every delivery-sensitive gate, so Make rebuilds when a command
  # changes context, persistence, logging, renderer, or injection settings.
  # The scePadReadState GOT-swap (PAD_HOOK=1) masks the menu's own buttons (and the open
  # chord's DpadLeft) from the game at the HID layer -- this is what stops R1+Left from
  # skipping the radio station when opening in a vehicle. It is OFF by default because the
  # GOT-swap mprotect needs jailbreak privileges and crashes the game on inject without
  # them. The validated production profile leaves the pad hook disabled.
  make feature-menu-frame-hook-playerped-build ENABLE_PAD_INPUT=1 PAD_HOOK="$PAD_HOOK_ENABLE" \
    FEATURE_MENU_GATE_MAINTHREAD=1 \
    FRAME_HOOK_SELF_START_WORKER="${SELF_START_WORKER:-0}" \
    GTAV_MENU_ENABLE_VEHICLE_PREVIEW="$VEHICLE_PREVIEW_ENABLE" \
    CUSTOM_PACKS="$CUSTOM_PACKS_ENABLE" \
    GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM="$INSTRUCTIONAL_SCALEFORM_ENABLE" \
    GTAV_MENU_ENABLE_BUTTON_GLYPHS="$BUTTON_GLYPHS_ENABLE" \
    RENDER_PHASE_INTERCEPT="$PHASE_RENDERER_ENABLE" \
    GTAV_MENU_PHASE_DRAW_LIST="$PHASE_RENDERER_ENABLE" \
    SCRIPT_GLOBALS="$SCRIPT_GLOBALS_ENABLE" \
    GTAV_MENU_ENABLE_WORKER_KLOG="$WORKER_KLOG_ENABLE" \
    GTAV_MENU_ENABLE_QUIT_GUARD="$QUIT_GUARD_ENABLE" \
    GTAV_MENU_QUIT_GUARD_APPSTATE="$QUIT_GUARD_APPSTATE_ENABLE" \
    WORKER_REQUIRE_CONTEXT="${WORKER_REQUIRE_CONTEXT_ENABLE:-0}" >/dev/null
  if [ ! -f "$FRAME_HOOK_ELF" ] || ! grep -aq "pad input ready" "$FRAME_HOOK_ELF"; then
    echo "[menu-ctl] !! frame-hook ELF build failed or lacks pad-input code" >&2
    exit 1
  fi
  if [ "$PAD_HOOK_ENABLE" = "1" ]; then
    if ! grep -aq "pad hook installed" "$FRAME_HOOK_ELF"; then
      echo "[menu-ctl] !! frame-hook ELF lacks pad-hook code (PAD_HOOK build mismatch)" >&2
      exit 1
    fi
    echo "[menu-ctl] pad hook ENABLED: needs healthy mprotect privileges"
  fi
  if [ "$WORKER_KLOG_ENABLE" = "1" ]; then
    echo "[menu-ctl] worker kernel-log ENABLED (default): stream crash logs with './menu-ctl.sh logs --kernel' (--no-klog to disable)"
  fi
  if [ "$QUIT_GUARD_ENABLE" = "1" ]; then
    if [ "$QUIT_GUARD_APPSTATE_ENABLE" = "1" ]; then
      echo "[menu-ctl] teardown guard ENABLED incl. app-state listener (HW-unverified; --no-quit-appstate drops just that, --no-quit-guard disables all)"
    else
      echo "[menu-ctl] teardown guard ENABLED (frame-hook heartbeat watchdog only)"
    fi
  fi
  echo "[menu-ctl] frame-hook ELF: $FRAME_HOOK_ELF"
}

# The ptrace-free cave loader never starts a worker thread itself. Refuse a final worker artifact
# whose symbol table disagrees with the requested self-start profile; this check runs both after the
# standalone worker build and after the loader build, because the latter embeds (and may rebuild)
# the worker. The loader repeats this gate from the embedded bytes before worker mapping and hook
# installation; its cave bootstrap may already have reserved memory in the game at that point.
verify_worker_self_start_profile() {
  local symbol symbols found
  command -v llvm-nm >/dev/null 2>&1 || {
    echo "[menu-ctl] !! llvm-nm is required to verify the worker self-start profile" >&2
    exit 1
  }
  if ! symbols="$(llvm-nm --defined-only "$FRAME_HOOK_ELF" 2>/dev/null)"; then
    echo "[menu-ctl] !! cannot read worker symbols; refusing deployment before target contact" >&2
    exit 1
  fi
  for symbol in gtav_frame_hook_self_start_rc gtav_frame_hook_self_start_authorized; do
    found=0
    if awk -v name="$symbol" '$3 == name { found=1 } END { exit !found }' <<<"$symbols"; then
      found=1
    fi
    if [ "${SELF_START_WORKER:-0}" = "1" ] && [ "$found" != "1" ]; then
      echo "[menu-ctl] !! cave worker lacks $symbol; refusing deployment before target contact" >&2
      exit 1
    fi
    if [ "${SELF_START_WORKER:-0}" != "1" ] && [ "$found" = "1" ]; then
      echo "[menu-ctl] !! non-cave worker unexpectedly exports $symbol; refusing mixed profile" >&2
      exit 1
    fi
  done
}

# Build the injected worker ELF and stage it on the console over FTP. Shared by the
# inject-now (cave-inject) and wait-then-inject (watch) lanes.
stage_worker_elf() {
  build_feature_menu_elf
  verify_worker_self_start_profile
  echo "[menu-ctl] uploading feature-menu ELF to $PAYLOAD_INJECT_ELF_REMOTE ..."
  ftp_rm "$PAYLOAD_INJECT_ELF_REMOTE"
  if ! ftp_put "$FRAME_HOOK_ELF" "$PAYLOAD_INJECT_ELF_REMOTE"; then
    echo "[menu-ctl] !! failed to upload feature-menu ELF over FTP" >&2
    exit 1
  fi
}

# Build the SDK payload loader (maps the worker into GTA + writes the PLAYER_PED_ID
# frame hook from the broker the worker publishes). Any extra make vars are forwarded
# -- watch passes PAYLOAD_LOADER_WAIT_FOR_GAME=1 so the loader parks until GTA is up.
build_payload_loader() {
  local phase_install="${PHASE_RENDERER_ENABLE:-0}"
  echo "[menu-ctl] building standalone payload loader (inject + frame-hook broker) ..."
  # The guard is mandatory: the external eboot jump/mapped gateway cannot yet be reconciled or
  # unmapped safely inside the same game process. Forward the complete worker profile too: the
  # build stamp covers loader and embedded-worker settings, so changing the loader's wait/mount
  # variables must not silently rebuild a different worker after stage_worker_elf uploaded it.
  make payload-loader-build PAYLOAD_LOADER_INJECT=1 PAYLOAD_LOADER_INSTALL_BROKER="${PAYLOAD_LOADER_INSTALL_BROKER:-1}" \
    ENABLE_PAD_INPUT=1 PAD_HOOK="$PAD_HOOK_ENABLE" \
    FEATURE_MENU_GATE_MAINTHREAD=1 \
    FRAME_HOOK_SELF_START_WORKER="${SELF_START_WORKER:-0}" \
    GTAV_MENU_ENABLE_VEHICLE_PREVIEW="$VEHICLE_PREVIEW_ENABLE" \
    GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM="$INSTRUCTIONAL_SCALEFORM_ENABLE" \
    GTAV_MENU_ENABLE_BUTTON_GLYPHS="$BUTTON_GLYPHS_ENABLE" \
    RENDER_PHASE_INTERCEPT="$phase_install" \
    GTAV_MENU_PHASE_DRAW_LIST="$phase_install" \
    SCRIPT_GLOBALS="$SCRIPT_GLOBALS_ENABLE" \
    GTAV_MENU_ENABLE_WORKER_KLOG="$WORKER_KLOG_ENABLE" \
    GTAV_MENU_ENABLE_QUIT_GUARD="$QUIT_GUARD_ENABLE" \
    GTAV_MENU_QUIT_GUARD_APPSTATE="$QUIT_GUARD_APPSTATE_ENABLE" \
    WORKER_REQUIRE_CONTEXT="${WORKER_REQUIRE_CONTEXT_ENABLE:-0}" \
    PAYLOAD_LOADER_INSTALL_RENDER_PHASE="$phase_install" \
    PAYLOAD_LOADER_INJECT_GUARD="${INJECT_GUARD_ENABLE:-1}" \
    CUSTOM_PACKS="$CUSTOM_PACKS_ENABLE" \
    PAYLOAD_LOADER_PROBE_NOSTOP="${PROBE_NOSTOP:-1}" \
    PAYLOAD_LOADER_NOSTOP_IO="${NOSTOP_IO:-1}" \
    PAYLOAD_LOADER_QUIESCE_ADDR="${QUIESCE_ADDR:-0}" "$@" >/dev/null
  if [ ! -f "$PAYLOAD_LOADER_ELF" ]; then
    echo "[menu-ctl] !! build failed: $PAYLOAD_LOADER_ELF not produced" >&2
    echo "[menu-ctl]    (needs the PS5 payload SDK; see PS5_PAYLOAD_SDK in make/config.mk)" >&2
    exit 1
  fi
  # payload-loader-build embeds the worker and can trigger a profile-sensitive worker rebuild.
  # Recheck the final artifact so staging and embedding cannot silently diverge.
  verify_worker_self_start_profile
}

# Confirm the inject landed and the worker loop is alive, then pin the control channel
# to the injected pid and (optionally) open the menu. Takes the payload log as $1. The
# payload's probe is ground truth: ticks=A->B with B>A means the worker loop is running
# in the injected process. Shared by cave-inject and watch.
report_inject_result() {
  local log="$1"
  if ! printf '%s\n' "$log" | grep -q "inject: overall OK"; then
    echo "[menu-ctl] !! injection did not report success -- see the inject lines above." >&2
    return 1
  fi
  if [ "${PHASE_RENDERER_ENABLE:-0}" = "1" ] && \
     ! printf '%s\n' "$log" | grep -q "render-phase: installed and verified"; then
    echo "[menu-ctl] !! worker is live but the default phase renderer was not verified." >&2
    echo "[menu-ctl]    Fully close GTA; this process is quarantined and must not be reused." >&2
    return 1
  fi
  local ticks a b injected_pid fg_pid
  ticks="$(printf '%s\n' "$log" | grep -oE 'ticks=[0-9]+->[0-9]+' | tail -1)"
  a="${ticks#ticks=}"; a="${a%%->*}"; b="${ticks##*->}"
  # Persistent logs contain the daemon owner's pid before the target pid. Bind controls to the
  # successful injection record, never the first incidental `pid=` in the log. One-shot loaders
  # predate the parenthesized pid field, so retain a target-specific fallback for them.
  # The one-shot loader still emits the older line without `(pid=N)`. Under set -e + pipefail,
  # a no-match here would abort the entire launcher before the fallback below can run.
  injected_pid="$(printf '%s\n' "$log" | grep -oE 'inject: overall OK \(pid=[0-9]+\)' | tail -1 | grep -oE '[0-9]+' | tail -1 || true)"
  if [ -z "$injected_pid" ]; then
    injected_pid="$(printf '%s\n' "$log" | grep -oE 'found game [^ ]+ pid=[0-9]+' | tail -1 | grep -oE 'pid=[0-9]+' | cut -d= -f2)"
  fi
  if [ -n "$b" ] && [ -n "$a" ] && [ "$b" -gt "$a" ] 2>/dev/null; then
    echo "[menu-ctl] menu worker is ALIVE in pid $injected_pid (ticks $a->$b)."
  else
    echo "[menu-ctl] !! worker did not tick (ticks=$ticks); it mapped but isn't looping." >&2
    return 1
  fi

  # 5. Pin the control channel at the injected pid (ps5debug otherwise targets the
  # foreground pid, which may be the title's other eboot), then show the menu.
  [ -n "$injected_pid" ] && TARGET_PID_ARGS="--pid $injected_pid"
  RUN_CLIENT_TIMEOUT=20
  fg_pid="$(run_client foreground 2>/dev/null | grep -oE '"pid"[: ]+[0-9]+' | grep -oE '[0-9]+' | head -1)"
  if [ -n "$fg_pid" ] && [ -n "$injected_pid" ] && [ "$fg_pid" != "$injected_pid" ]; then
    echo "[menu-ctl] note: ps5debug foreground pid=$fg_pid != injected pid=$injected_pid (two-eboot decoy)."
  fi
  if [ "$SHOW_AFTER_START" = "1" ]; then
    menu_command show >/dev/null 2>&1 || true  # best-effort; menu also opens with R1+D-pad Left
  fi
  RUN_CLIENT_TIMEOUT=""
  echo "[menu-ctl] menu running. Open/toggle with R1+D-pad Left."
  echo "[menu-ctl] controls: D-pad Up/Down rows, Cross select, Circle or D-pad Left back; D-pad Left at root hides."
  echo "[menu-ctl] vehicle spawn, weapons, and skin drain on the game thread via the frame hook."
}

# cave-inject: GTA must already be in Story Mode with player control. Build + stage the worker,
# one-shot loader, deploy it, and confirm the inject landed right away.

# watch: deploy a loader that PARKS on the console and injects itself once GTA reaches
# stable player-world readiness -- so you can run this BEFORE launching GTA. The loader waits for
# the GTA foreground big-app, then polls a read-only game-memory anchor and only injects
# once it reports the player exists in the world (player control). Injecting any earlier
# (boot / update / story-select / mid-load) is destructive -- it crashes the load.
# Game-mode selection is intentionally outside this boot/loading readiness predicate.
#
# Default anchor = the local player ped: PLAYER_PED_ID resolves ped = *(*(0x51cf258)+8),
# and ped != 0 means the player is in the world. That is the SAME stable point
# `./menu-ctl.sh cave-inject` works at. Override any field via env:
#   SP_READY_ADDR=0x.. SP_READY_DEREF=0|1 SP_READY_DEREF_OFFSET=0x.. SP_READY_MODE=0|1
#   SP_READY_VALUE=.. SP_READY_MASK=0x.. SP_READY_SIZE=8 SP_READY_TIMEOUT=0 ./menu-ctl.sh watch
# VALIDATE FIRST (read-only): tools/read_player_ped_anchor.py should report NOT READY at
# the menus and READY only once you have player control.
WATCH_TIMEOUT="${WATCH_TIMEOUT:-600}"
WATCH_POLL_INTERVAL="${WATCH_POLL_INTERVAL:-3}"

# The runtime pack lane is one flag, CUSTOM_PACKS. It defaults to the
# target manifest's features.customPacks (on for the 01.010.002 executables) and builds the worker's
# pack lane. Custom assets require the console /data to be shared with GTA by ShadowMountPlus
# or the HEN setup. Without that shared path the menu still runs, with custom assets unavailable.
# CUSTOM_PACKS=0 builds the lane-off menu.
#
# The old per-part switches refuse for one release so existing commands name their replacement.
resolve_custom_packs() {
  local name
  for name in CUSTOM_STAGE CUSTOM_PACK_STAGE CUSTOM_DEVICE CUSTOM_STREAM; do
    [ -n "${!name:-}" ] || continue
    echo "[menu-ctl] $name was removed; use CUSTOM_PACKS=0 or CUSTOM_PACKS=1" >&2
    exit 2
  done
  case "${CUSTOM_PACKS:-}" in
    ''|0|1) ;;
    *)
      echo "[menu-ctl] CUSTOM_PACKS must be 0 or 1, not '$CUSTOM_PACKS'" >&2
      exit 2
      ;;
  esac
  CUSTOM_PACKS_ENABLE="${CUSTOM_PACKS:-$TARGET_CUSTOM_PACKS}"
  if [ "$CUSTOM_PACKS_ENABLE" = "1" ] && [ "$TARGET_CUSTOM_PACKS" != "1" ]; then
    echo "[menu-ctl] the custom pack lane is not admitted for $GTAV_TARGET; use CUSTOM_PACKS=0" >&2
    exit 2
  fi
  if [ "$CUSTOM_PACKS_ENABLE" = "1" ] && \
      { [ "$VEHICLE_PREVIEW_ENABLE" != "1" ] || [ "$WORKER_KLOG_ENABLE" != "1" ]; }; then
    echo "[menu-ctl] the custom pack lane needs vehicle preview and klog on; add CUSTOM_PACKS=0 to turn it off" >&2
    exit 2
  fi
  if [ "$CUSTOM_PACKS_ENABLE" = "1" ]; then
    echo "[menu-ctl] custom pack lane ON (requires shared game /data; CUSTOM_PACKS=0 to build without it)"
  fi
}

do_watch() {
  parse_start_args "$@"
  refuse_retired_custom_mount
  resolve_custom_packs

  if [ "$CAVE_LANE_ENABLE" != "1" ]; then
    echo "[menu-ctl] !! the ptrace-free cave route is mandatory for $GTAV_TARGET; --no-cave is unsupported." >&2
    exit 2
  fi

  # Player-ped readiness anchor (overridable). MODE 1 = ready when the dereferenced
  # value is non-zero (the player ped pointer is non-null => player in control).
  # The readiness anchor is per-build, so read it from the target manifest instead of hardcoding one
  # build's address here. gtavmenu_tools.target honours GTAV_TARGET and falls back to the repo default,
  # so this is the same value the loader and every probe use -- one source, no copy to drift.
  local sp_addr sp_deref sp_deref_off sp_mode
  if [ -z "${SP_READY_ADDR:-}" ] || [ -z "${SP_READY_DEREF_OFFSET:-}" ]; then
    local anchor_pair
    if ! anchor_pair="$(PYTHONPATH="$ROOT/tools${PYTHONPATH:+:$PYTHONPATH}" "$PY" -c 'from gtavmenu_tools.target import PLAYER_PED_ANCHOR as a, PED_OFFSET as o; print(hex(a), hex(o))' 2>&1)"; then
      echo "[menu-ctl] !! cannot read the player-ped anchor from the target manifest:" >&2
      printf '%s\n' "$anchor_pair" | sed 's/^/[menu-ctl]    /' >&2
      echo "[menu-ctl]    A publication checkout needs no Python package install; verify tools/gtavmenu_tools and data/targets are intact." >&2
      exit 1
    fi
    if [ -z "$anchor_pair" ]; then
      echo "[menu-ctl] !! cannot read the player-ped anchor from the target manifest." >&2
      echo "[menu-ctl]    Verify tools/gtavmenu_tools and data/targets are intact, or pass SP_READY_ADDR= and SP_READY_DEREF_OFFSET=." >&2
      exit 1
    fi
    SP_READY_ADDR="${SP_READY_ADDR:-${anchor_pair%% *}}"
    SP_READY_DEREF_OFFSET="${SP_READY_DEREF_OFFSET:-${anchor_pair##* }}"
  fi
  sp_addr="$SP_READY_ADDR"
  sp_deref="${SP_READY_DEREF:-1}"
  sp_deref_off="$SP_READY_DEREF_OFFSET"
  sp_mode="${SP_READY_MODE:-1}"
  echo "[menu-ctl] readiness anchor: [$sp_addr] + $sp_deref_off (from the target manifest)"

  # The watch lane injects before SP finishes loading, so build the worker with the
  # context gate on: its render/getter natives stay dormant until the frame hook reports
  # live gameplay, which keeps the worker from crashing the SP load.
  WORKER_REQUIRE_CONTEXT_ENABLE=1
  # The ptrace-free route needs a worker that starts its own thread, because the loader will not.
  # The bootstrap re-runs per game instance, so this composes with --persist.
  local cave_args=""
  if [ "$CAVE_LANE_ENABLE" = "1" ]; then
    SELF_START_WORKER=1
    cave_args="PAYLOAD_LOADER_NOSTOP_STRICT=1 PAYLOAD_LOADER_VERIFY_WRITES=1 \
      PAYLOAD_LOADER_CAVE_BOOTSTRAP=1 ${CAVE_ADDR:+PAYLOAD_LOADER_CAVE_ADDR=$CAVE_ADDR} \
      PAYLOAD_LOADER_CAVE_ALLOC=$CAVE_ALLOC PAYLOAD_LOADER_CAVE_INJECT=1"
    echo "[menu-ctl] watch: ptrace-free, no-target-elevation lane (cave bootstrap + self-starting worker)"
  fi
  stage_worker_elf
  # Unquoted on purpose: cave_args is a pre-split list of make assignments, or empty.
  # shellcheck disable=SC2086
  build_payload_loader $cave_args PAYLOAD_LOADER_WAIT_FOR_GAME=1 PAYLOAD_LOADER_PERSISTENT="$PERSIST_ENABLE" \
    PAYLOAD_LOADER_SP_READY=1 \
    PAYLOAD_LOADER_SP_READY_ADDR="$sp_addr" \
    PAYLOAD_LOADER_SP_READY_DEREF="$sp_deref" \
    PAYLOAD_LOADER_SP_READY_DEREF_OFFSET="$sp_deref_off" \
    PAYLOAD_LOADER_SP_READY_VALUE="${SP_READY_VALUE:-0}" \
    PAYLOAD_LOADER_SP_READY_MASK="${SP_READY_MASK:-0xFFFFFFFFFFFFFFFF}" \
    PAYLOAD_LOADER_SP_READY_SIZE="${SP_READY_SIZE:-8}" \
    PAYLOAD_LOADER_SP_READY_MODE="$sp_mode" \
    PAYLOAD_LOADER_SP_READY_TIMEOUT_SEC="${SP_READY_TIMEOUT:-0}" \
    PAYLOAD_LOADER_SP_READY_SETTLE_USEC="${SP_READY_SETTLE_USEC:-0}" \
    PAYLOAD_LOADER_SP_READY_CONFIRMATIONS="${SP_READY_CONFIRMATIONS:-3}" \
    PAYLOAD_LOADER_SP_READY_GENTLE="$GENTLE_POLL_ENABLE" \
    PAYLOAD_LOADER_SP_READY_POLL_USEC="${SP_READY_POLL_USEC:-2000000}" \
    PAYLOAD_LOADER_SP_READY_POLL_MAX_USEC="${SP_READY_POLL_MAX_USEC:-5000000}"
  if [ "$sp_deref" = "1" ]; then
    echo "[menu-ctl] SP-ready gate: inject when *([$sp_addr] + $sp_deref_off) $([ "$sp_mode" = "1" ] && echo '!=' || echo '==') ${SP_READY_VALUE:-0} (player ped exists), debounced x${SP_READY_CONFIRMATIONS:-3}"
  else
    echo "[menu-ctl] SP-ready gate: inject when [$sp_addr] $([ "$sp_mode" = "1" ] && echo '!=' || echo '==') ${SP_READY_VALUE:-0}, debounced x${SP_READY_CONFIRMATIONS:-3}"
  fi
  # The anchor + version signature are selected from the target manifest. If the console
  # auto-updated GTA, the pin is stale -- the loader will refuse to inject (version check) or,
  # worse, gate on a wrong address. Validate the anchor read-only (game in control => READY,
  # menus/loading => NOT READY) BEFORE trusting watch after any game update:
  echo "[menu-ctl] pinned to GTA target $GTAV_TARGET; after a game update re-validate first:"
  echo "[menu-ctl]   python3 tools/read_player_ped_anchor.py --host $PS5_HOST"
  if [ "$GENTLE_POLL_ENABLE" = "1" ]; then
    echo "[menu-ctl] gentle poll ENABLED: not-ready reads back off ${SP_READY_POLL_USEC:-2000000}->${SP_READY_POLL_MAX_USEC:-5000000}us (fewer game freezes during load); fast once the ped appears"
  fi

  archive_and_clear_payload_log  # clear it, but keep a copy: the resolver reads the base from it
  echo "[menu-ctl] launching parked payload on $PS5_HOST:$PS5_PORT via prospero-deploy ..."
  if [ "$PERSIST_ENABLE" = "1" ]; then
    # prospero-deploy's socat remains connected until the remote payload process exits. A persistent
    # daemon intentionally does not exit after injection, so a foreground deploy would block here
    # forever and never reach the result poll below. Keep that transport process alongside the
    # daemon; it exits naturally when daemon-stop completes.
    make deploy-built-payload-loader PS5_HOST="$PS5_HOST" PS5_PORT="$PS5_PORT" &
    local deploy_pid=$!
    sleep 2
    if ! kill -0 "$deploy_pid" 2>/dev/null; then
      if ! wait "$deploy_pid"; then
        echo "[menu-ctl] !! persistent payload deploy failed (is the payload server listening on $PS5_HOST:$PS5_PORT?)" >&2
        exit 1
      fi
    fi
    echo "[menu-ctl] persistent deploy transport pid=$deploy_pid (ends with the daemon)"
  elif ! make deploy-built-payload-loader PS5_HOST="$PS5_HOST" PS5_PORT="$PS5_PORT"; then
    echo "[menu-ctl] !! payload deploy failed (is the payload server listening on $PS5_HOST:$PS5_PORT?)" >&2
    exit 1
  fi

  if [ "$PERSIST_ENABLE" = "1" ]; then
    echo "[menu-ctl] loader deployed as a PERSISTENT daemon: it re-injects on every GTA relaunch."
  else
    echo "[menu-ctl] loader deployed and WAITING on the console (one-shot: injects once, then exits)."
  fi
  echo "[menu-ctl] >>> Launch GTA V and explicitly enter Story Mode; injection follows player readiness. <<<"
  echo "[menu-ctl] polling for the inject (up to ${WATCH_TIMEOUT}s; Ctrl-C to stop watching -- the loader keeps waiting) ..."

  local log waited=0
  while :; do
    log="$(ftp_cat "$PAYLOAD_LOG_REMOTE")"
    if printf '%s\n' "$log" | grep -q "inject: overall"; then
      break  # the loader found the game and ran the inject (OK or FAIL)
    fi
    if [ "$waited" -ge "$WATCH_TIMEOUT" ]; then
      echo "[menu-ctl] !! no inject after ${WATCH_TIMEOUT}s. The loader may still be waiting for GTA;" >&2
      echo "[menu-ctl]    re-run 'watch' once GTA is launching, or check the payload log." >&2
      printf '%s\n' "$log" | tail -20 | sed 's/^/  /'
      return 1
    fi
    sleep "$WATCH_POLL_INTERVAL"
    waited=$((waited + WATCH_POLL_INTERVAL))
  done

  echo "[menu-ctl] inject detected after ~${waited}s. payload log ($PAYLOAD_LOG_REMOTE):"
  printf '%s\n' "$log" | tail -20 | sed 's/^/  /'
  report_inject_result "$log"
  if [ "$PERSIST_ENABLE" = "1" ]; then
    echo "[menu-ctl] (persistent) the loader stays resident and will re-inject when GTA is relaunched."
  fi
}

do_spawn() {
  local index="${1:-0}"
  echo "[menu-ctl] sending feature-menu vehicle spawn command (index=$index)..."
  wait_running >/dev/null
  menu_command spawn_vehicle \
    --argument "$index" \
    --output "build/gtav-menu-command-menu-ctl-spawn-${index}.json" >/dev/null
  echo "[menu-ctl] spawn command accepted; the frame hook will drain it on the game thread."
}

# Retrieve logs. Default: the payload loader's on-console file over FTP (the injector's
# lifecycle + inject trace). --worker: the injected worker's in-memory log ring over ps5debug
# (init breadcrumbs by default; verbose once Telemetry is toggled on in the menu). FTP has no
# true follow, so the loader view is a one-shot tail, not -f.
do_logs() {
  if [ "${1:-}" = "--worker" ]; then
    shift
    # As above: hand the client the ring's address rather than letting it scan for the magic, which
    # it only does across small writable anonymous maps. An explicit --address in "$@" wins.
    local ring_arg=""
    if ! printf '%s\n' "$@" | grep -q '^--address$'; then
      ring="$(resolved_symbol_addr gtav_menu_log_ring)"
      [ -n "$ring" ] && ring_arg="--address $ring"
    fi
    run_client worker-log $ring_arg "$@"
    return
  fi
  # --kernel: stream the PS5 kernel log (ps5debug forwarder, port 3232) to a local file,
  # filtered to our worker lines. This is the crash-capture lane: the kernel log is
  # kernel-side, so it retains the breadcrumbs after GTA dies (the in-memory ring does not).
  # Requires the worker to have been built with --klog. Optional first arg = capture seconds
  # (default ~24h = until Ctrl-C). Extra args pass through to the client `klog` command.
  if [ "${1:-}" = "--kernel" ] || [ "${1:-}" = "--klog" ]; then
    shift
    local secs="${KLOG_SECONDS:-86400}"
    if printf '%s' "${1:-}" | grep -qE '^[0-9]+$'; then secs="$1"; shift; fi
    local out="${KLOG_OUTPUT:-build/gtav-menu-klog.log}"
    echo "[menu-ctl] streaming PS5 kernel log (filter '${KLOG_FILTER:-GTAVMenu}', ${secs}s) -> $out"
    echo "[menu-ctl] crash capture: survives GTA dying. Build the worker with --klog. Ctrl-C to stop."
    run_client klog --klog-port "${PS5_KLOG_PORT:-3232}" --filter "${KLOG_FILTER:-GTAVMenu}" \
      --seconds "$secs" --output "$out" "$@"
    return
  fi
  local n="${1:-40}"
  echo "[menu-ctl] === loader log ($PAYLOAD_LOG_REMOTE, last $n lines) ==="
  ftp_cat "$PAYLOAD_LOG_REMOTE" | tail -n "$n"
}

# daemon-stop: retire a resident `watch --persist` installation as one transaction. This is not an
# OnionHEN plugin stop; disable GTAV00001 in Toolbox so its supervisor does not relaunch the daemon.
# The sentinel
# makes the daemon stop the worker, wait for STOPPED (including phase-slot restoration), exact-
# restore its loader-owned PLAYER_PED_ID prologue through the ptrace-free kernel-copy lane, verify
# the readback, and only then exit/release ownership. If GTA is backgrounded, the daemon stays alive
# until that exact instance is foreground again or demonstrably gone; it never writes through an
# ambiguous/recycled pid.
do_daemon_stop() {
  echo "[menu-ctl] note: OnionHEN users must disable GTAV00001 in Toolbox; daemon-stop controls watch --persist."
  local before log fresh listing waited=0 timeout="${DAEMON_STOP_TIMEOUT:-30}"
  # ftp_cat intentionally masks read errors for display commands. Here a complete baseline is
  # required: old exit records cannot acknowledge this stop request.
  if ! before="$(curl -s --max-time 10 "ftp://$PS5_HOST:$PS5_FTP_PORT$PAYLOAD_LOG_REMOTE" 2>/dev/null)"; then
    echo "[menu-ctl] !! cannot read the loader log baseline; no stop request sent." >&2
    return 1
  fi
  echo "[menu-ctl] asking the resident loader to stop the worker and exact-retire its hooks ..."
  if ! ftp_put /dev/null "$DAEMON_STOP_REMOTE"; then
    echo "[menu-ctl] !! could not write the stop sentinel over FTP (is FTP up on $PS5_HOST:$PS5_FTP_PORT?)" >&2
    exit 1
  fi
  # Require a newly appended terminal record and a successful listing without the lock marker.
  # The loader releases that marker before logging its exit; a replacement daemon may acquire it
  # again. FTP failures and rewritten/truncated logs never establish either condition.
  while :; do
    fresh=""
    if log="$(curl -s --max-time 10 "ftp://$PS5_HOST:$PS5_FTP_PORT$PAYLOAD_LOG_REMOTE" 2>/dev/null)"; then
      case "$log" in
        "$before"*) fresh="${log#"$before"}" ;;
      esac
    fi
    if printf '%s\n' "$fresh" | grep -q "persistent: daemon exited"; then
      if listing="$(curl -s --max-time 10 "ftp://$PS5_HOST:$PS5_FTP_PORT${DAEMON_STOP_REMOTE%/*}/" 2>/dev/null)" &&
          ! printf '%s\n' "$listing" | grep -Eq '(^|[[:space:]])daemon\.lock([[:space:]]|$)'; then
        echo "[menu-ctl] worker stopped, owned hooks retired, and daemon exited (fresh record; daemon.lock absent)."
        return 0
      fi
    fi
    if [ "$waited" -ge "$timeout" ]; then
      echo "[menu-ctl] !! exact retirement did not reach a fresh 'persistent: daemon exited' with daemon.lock absent within ${timeout}s." >&2
      echo "[menu-ctl]    The stop request was sent, but retirement is unconfirmed; inspect the log and daemon ownership." >&2
      return 1
    fi
    sleep 2
    waited=$((waited + 2))
  done
}


# cave-inject uses the validated ptrace-free bootstrap and production render-phase install.
do_cave_inject() {
  parse_start_args "$@"
  refuse_retired_custom_mount
  resolve_custom_packs

  if [ "$PREFLIGHT_ENABLE" = "1" ]; then
    echo "[menu-ctl] pre-flight: confirming player-world readiness (read-only anchor check)..."
    if ! "$PY" tools/read_player_ped_anchor.py --host "$PS5_HOST" --port "$PS5DEBUG_PORT" --require-ready; then
      echo "[menu-ctl] !! pre-flight failed: player world not ready, wrong build, or ps5debug unreachable." >&2
      exit 1
    fi
    echo "[menu-ctl] pre-flight OK: player ped is live on the pinned build."
  fi

  # The worker starts its own thread on this lane; the loader deliberately does not. Keep this
  # local assignment alive for BOTH stage_worker_elf and build_payload_loader. Prefixing only the
  # first function call restores the old value before the loader build and embeds a non-starting
  # worker, which hardware caught as PARTIAL_BROKER_FAILED on PID 283.
  local SELF_START_WORKER=1
  stage_worker_elf
  echo "[menu-ctl] building the cave-inject loader (cave=$CAVE_ADDR alloc=$CAVE_ALLOC) ..."
  build_payload_loader \
    PAYLOAD_LOADER_NOSTOP_STRICT=1 \
    PAYLOAD_LOADER_VERIFY_WRITES=1 \
    PAYLOAD_LOADER_CAVE_BOOTSTRAP=1 \
    ${CAVE_ADDR:+PAYLOAD_LOADER_CAVE_ADDR="$CAVE_ADDR"} \
    PAYLOAD_LOADER_CAVE_ALLOC="$CAVE_ALLOC" \
    PAYLOAD_LOADER_CAVE_INJECT=1

  archive_and_clear_payload_log  # clear it, but keep a copy: the resolver reads the base from it
  echo "[menu-ctl] launching payload on $PS5_HOST:$PS5_PORT via prospero-deploy ..."
  if ! make deploy-built-payload-loader PS5_HOST="$PS5_HOST" PS5_PORT="$PS5_PORT"; then
    echo "[menu-ctl] !! payload deploy failed (is the payload server listening on $PS5_HOST:$PS5_PORT?)" >&2
    exit 1
  fi

  sleep 4
  echo "[menu-ctl] payload log ($PAYLOAD_LOG_REMOTE):"
  local log
  log="$(ftp_cat "$PAYLOAD_LOG_REMOTE")"
  printf '%s\n' "$log" | tail -30 | sed 's/^/  /'

  if printf '%s' "$log" | grep -q "falling back to ptrace"; then
    echo "[menu-ctl] !! the loader FELL BACK TO PTRACE -- the game was stopped; findings are void." >&2
    exit 1
  fi
  report_inject_result "$log"
}

# session: one hardware test session from a plan (plan.json: processes, their packs, steps and expectation files;
# tools/hw_session.py). stage uploads the plan's packs once; next selects a process's packs before GTA restarts;
# go waits for player control in single-player (the cave-inject pre-flight probe), starts the pack-notes follower
# (and the klog collector) in the background, injects with the process's target and prints the Load prompt;
# check stops the followers and checks the latest process's lines; stop stops them. Logs: build/sessions/<plan>/.
do_session() {
  local usage="usage: $0 session stage PLAN [--dry-run] | next PLAN NAME [SELECTION] [--dry-run]
       | go PLAN NAME [--klog] [--timeout S] [--poll S] [--dry-run] | check PLAN NAME [NOTES] [--keep]
       | stop | show PLAN [NAME]"
  case "${1:-}" in
    stage | next | go | check | stop | show) ;;
    *) echo "$usage" >&2; return 2 ;;
  esac
  # Plan and log paths are the caller's; the tool finds the checkout by its own path.
  (cd -- "$CALLER_DIR" &&
    PS5_HOST="$PS5_HOST" PS5_FTP_PORT="$PS5_FTP_PORT" PS5DEBUG_PORT="$PS5DEBUG_PORT" \
      "$PY" "$ROOT/tools/hw_session.py" "$@")
}

cmd="${1:-}"
shift || true
# Refuse before a build, connection or upload when a command always needs the console.
# Mixed host/console commands check at their optional console step instead.
case "$cmd" in
  watch|cave-inject|stop|daemon-stop|status|show|hide|spawn|map-upload|map-load|map-cancel|map-clear|\
  pack-register|pack-up|pack-load-archive|pack-inspect|pack-revert|pack-request|pack-load-data|\
  spawn-model|spawn-object|pack-load-typ|spawn-ped|give-weapon|pack-install|pack-list|pack-notes|\
  pack-upload|pack-uninstall|export-templates|pack-select|pack-autoload|pack-load-map|pack-load-bounds|\
  pack-unhide|pack-labels|pack-finish-map|pack-card-show|pack-card-release|doctor|logs)
    require_console_host || exit 2
    ;;
esac
case "$cmd" in
  watch)
    do_watch "$@"
    ;;
  cave-inject)
    do_cave_inject "$@"
    ;;
  stop)
    echo "[menu-ctl] sending stop command to the menu worker..."
    menu_command stop || {
      echo "[menu-ctl] stop command failed (menu may not be running)."; exit 0; }
    echo "[menu-ctl] stop sent. A persistent loader will now exact-retire its external frame hook."
    ;;
  daemon-stop)
    do_daemon_stop
    ;;
  restart)
    echo "[menu-ctl] same-process restart is unsafe/unsupported (external hook mapping persists)." >&2
    echo "[menu-ctl] Run 'stop', fully exit/relaunch GTA into Story Mode, then run 'cave-inject'." >&2
    exit 2
    ;;
  status)
    status_addrs="$(resolved_block_addrs)"
    status_block_arg=""
    [ -n "$status_addrs" ] && status_block_arg="--address ${status_addrs##* }"
    run_client status \
      --target-id "$TARGET_ID" \
      --hook-status-name "$HOOK_STATUS_NAME" \
      --require-running \
      --require-no-error \
      $status_block_arg \
      "$@"
    ;;
  show|hide)
    menu_command "$cmd"
    ;;
  spawn)
    do_spawn "$@"
    ;;
  map-upload)
    do_map_upload "$@"
    ;;
  map-load)
    [ "$#" -eq 0 ] || { echo "usage: $0 map-load" >&2; exit 2; }
    do_map_action LOAD_MAP
    ;;
  map-cancel)
    [ "$#" -eq 0 ] || { echo "usage: $0 map-cancel" >&2; exit 2; }
    do_map_action CANCEL_MAP_LOAD
    ;;
  map-clear)
    [ "$#" -eq 0 ] || { echo "usage: $0 map-clear" >&2; exit 2; }
    do_map_clear
    ;;
  pack-register)
    do_pack_register "$@"
    ;;
  pack-up)
    do_pack_up "$@"
    ;;
  pack-load-archive)
    do_pack_load_archive "$@"
    ;;
  pack-inspect)
    do_pack_inspect "$@"
    ;;
  pack-revert)
    do_pack_revert "$@"
    ;;
  pack-request)
    do_pack_request "$@"
    ;;
  pack-load-data)
    do_pack_load_data "$@"
    ;;
  spawn-model)
    do_spawn_model "$@"
    ;;
  spawn-object)
    do_spawn_object "$@"
    ;;
  pack-load-typ)
    do_pack_load_typ "$@"
    ;;
  spawn-ped)
    do_spawn_ped "$@"
    ;;
  give-weapon)
    do_give_weapon "$@"
    ;;
  pack-install)
    do_pack_install "$@"
    ;;
  author-labels | author-timecycle | author-ptfx)
    do_author_pack "$cmd" "$@"
    ;;
  convert-map)
    do_convert_map "$@"
    ;;
  convert-replace)
    do_convert_replace "$@"
    ;;
  convert-override)
    do_convert_override "$@"
    ;;
  convert-clothing)
    do_convert_clothing "$@"
    ;;
  convert-bounds)
    do_convert_bounds "$@"
    ;;
  convert-model)
    do_convert_model "$@"
    ;;
  convert-mlo)
    do_convert_mlo "$@"
    ;;
  convert-mapmod)
    do_convert_mapmod "$@"
    ;;
  convert-ped)
    do_convert_ped "$@"
    ;;
  convert-weapon)
    do_convert_weapon "$@"
    ;;
  convert-vehicle)
    do_convert_vehicle "$@"
    ;;
  convert-wheels)
    do_convert_wheels "$@"
    ;;
  pack-list)
    do_pack_list "$@"
    ;;
  pack-notes)
    do_pack_notes "$@"
    ;;
  pack-check)
    do_pack_check "$@"
    ;;
  pack-upload)
    do_pack_upload "$@"
    ;;
  pack-uninstall)
    do_pack_uninstall "$@"
    ;;
  pack-validate)
    do_pack_validate "$@"
    ;;
  fetch-templates)
    do_fetch_templates "$@"
    ;;
  export-templates)
    do_export_templates "$@"
    ;;
  index-archetypes)
    do_index_archetypes "$@"
    ;;
  pack-select)
    do_pack_select "$@"
    ;;
  pack-autoload)
    do_pack_autoload "$@"
    ;;
  pack-load-map)
    do_pack_load_map "$@"
    ;;
  pack-load-bounds)
    do_pack_load_bounds "$@"
    ;;
  pack-unhide)
    do_pack_unhide "$@"
    ;;
  pack-labels)
    do_pack_labels "$@"
    ;;
  pack-finish-map)
    do_pack_finish_map "$@"
    ;;
  pack-card-show)
    do_pack_card_show "$@"
    ;;
  pack-card-release)
    do_pack_card_release "$@"
    ;;
  texture-card-stock)
    echo "[menu-ctl] texture-card-stock was removed; use pack-card-show" >&2
    exit 2
    ;;
  texture-release-stock)
    echo "[menu-ctl] texture-release-stock was removed; use pack-card-release" >&2
    exit 2
    ;;
  doctor)
    do_doctor
    ;;
  session)
    do_session "$@"
    ;;
  logs)
    do_logs "$@"
    ;;
  *)
    cat >&2 <<EOF
usage: $0 COMMAND [OPTIONS]

Production commands:
  cave-inject [--show]       inject into a ready single-player session
  watch [--persist] [opts]   wait for readiness, then inject once or per relaunch
  stop                       stop the menu worker and restore owned hooks
  daemon-stop                retire a menu-ctl watch --persist daemon and worker
  status [--pid N]           read the live status block
  show | hide                change menu visibility
  spawn [index]              queue a vehicle spawn on the game thread
  map-upload [package-dir]   verify and upload a prepared custom-map package
  map-load                   load the uploaded map through the game-thread lane
  map-cancel                 stop an in-progress map load
  map-clear                  cancel loading and delete menu-owned map entities
  pack-register              register the active runtime pack's archive
  pack-up                    register + load archive + parse all data rows + request typ rows
  pack-load-archive          request only the pack archive streamable (TOC re-parse, no member)
  pack-inspect               read the pack archive/member streaming-info words (no request)
  pack-revert                put the loaded packs' stock overrides back (refuses while in use)
  pack-request N             request card N's texture dictionary once
  pack-load-data N           load data row N through its data-file mounter once
  spawn-model NAME           spawn a vehicle by model name (joaat; the game validates it)
  spawn-object NAME          spawn an object by model name (joaat; the game validates it)
  pack-load-typ N            request the pack's archetype definitions row N (.ptyp) once
  pack-load-map N            load and activate the pack's map data row N (.pmap) once
  pack-load-bounds N         load the pack's static collision bounds row N (.pbn) once
  pack-unhide                undo the loaded packs' applied hide rows (host debug)
  pack-labels                add the pack's text labels (vehicle names) to the game's text map
  spawn-ped NAME             spawn a ped by model name (e.g. a pack's add-on ped)
  give-weapon NAME           give the player a weapon by name (e.g. weapon_gmpistol)
  pack-install DIR [--activate|--activate-add]
                             validate and upload a built pack, then print the in-game step
                             (Custom Packs -> Manage Packs -> select it, then Load)
  convert-map MOD --id ID --template T.pmap [--teleport X,Y,Z[,TEXT]] [--archetype-index FILE]
      [--install|--activate]
                             convert a PC map mod that places stock models (dlc.rpf, .ymap,
                             .ymap.xml or folder) into build/custom-assets/ID, validate it and
                             optionally install it; mods with their own models are refused
  convert-replace MOD --id ID [--model NAME] [--repair NAME ...] [--game DIR] [...]
                             convert a PC "replace" vehicle mod (a car shipped under a stock
                             name) into a stock override pack (reviewed stock names)
  convert-override [MOD] --id ID [--only PATTERN ...] [--member STOCK=FILE ...] [--game DIR] [...]
                             convert a PC "replace" mod of a reviewed stock asset (textures, props,
                             weapons, ped clothing, vehicles) or loose files into a stock
                             override pack (reviewed stock names)
  convert-clothing MOD --ped PED --slot SLOT --id ID [--drawable N ...] [--game DIR|--stock-cache DIR] [...]
                             convert a PC clothing mod: a story character's stock drawable N
                             replaced, or new freemode drawables added (reviewed stock names)
  author-labels FILE --id ID [--carrier FILE.ptd] [--install|--activate]
                             build a pack of KEY=TEXT labels
  author-timecycle [FILE.xml] --id ID --like NAME --name NEW [--install|--activate]
                             clone one timecycle modifier from XML or the verified template cache
  author-ptfx [FILE.ppt] --id ID --name NEW --effect NAME [--install|--activate]
                             clone an existing PS5 particle dictionary with a declared effect
  convert-bounds MOD... --id ID [--only NAME ...] [--merge] [--translate DX,DY,DZ] [...]
                             convert a PC mod's static collision (.ybn, dlc.rpf or folder) into
                             bounds rows of a validated pack
  convert-model MOD --id ID [--archetype NAME ...] [--maps] [...]
                             convert a PC mod's own models (props, or a map mod's buildings; with
                             --maps also its placements) into a pack typ + .pdr + .ptd
  convert-mlo MOD --id ID --mlo NAME[=MAP] ... [--models] [--bounds] [...]
                             convert a PC map mod's interior (MLO + placement, optionally its own
                             models and collision) into a pack typ + interior maps
  convert-mapmod MOD --id ID [--dry-run] [--translate DX,DY,DZ] [--drawn-collision embedded|all] [...]
                             convert a whole PC map mod (maps, own models, collision, interiors)
                             into one or more packs within the pack caps; --dry-run prints the split
  convert-ped MOD --id ID --ped NAME --init-data FILE [...]
                             convert a whole PC add-on ped (.ydd/.ytd/.ymt/.yft, every component
                             and texture, props from <stem>_p.ydd) into a new pack ped
  convert-weapon MOD --id ID --weapon WEAPON_NEW --like WEAPON_DONOR [...]
                             convert a PC weapon model (.ydr/.ytd) into a new pack weapon
  convert-vehicle MOD --id ID --model NAME [--name TEXT] [--mod-kit] [--repair NAME ...] [...]
                             convert a PC add-on vehicle (dlc.rpf) into a pack vehicle (model,
                             textures, LSC parts, handling/carcols/layouts/vehicles/variations)
  convert-wheels MOD --id ID [--type TYPE] [--only NAME ...] [--label NAME=TEXT ...] [--list] [...]
                             convert the wheel models a PC car mod ships (its carcols <Wheels>)
                             into a wheel pack: <wheel>.pdr with embedded textures + a carcols row
                             (each convert-* command: --install|--activate; see its usage)
  pack-list                  show the console's installed packs (* = selected in packs/active)
  pack-notes [--follow [--out FILE] [--interval S] [--for S]]
                             print this session's pack lines (worker copy of the pack klog);
                             --follow keeps every line in a local log while a load runs
  pack-check EXPECT [NOTES]  check pack lines (the console's, or a followed log) against an
                             expectation file: PASS / MISSING / FAIL per rule
  pack-upload DIR [--activate|--activate-add]
                             upload a built runtime pack; --activate makes it the only active
                             pack, --activate-add adds it (max 8); otherwise select it in the menu
  pack-uninstall ID [--purge]
                             remove pack ID from packs/active and packs/installed (files stay);
                             --purge also deletes its directory on the console
  pack-validate DIR [--against DIR ...]
                             check a pack directory: files, sizes, hashes, row limits and the
                             merge rules for packs active together (host only)
  fetch-templates [--source URL] [--cache DIR]
                             fetch the PC vehicle converter's retail templates (verified, read-only)
                             from the game image: default the console over FTP while GTA V runs,
                             or --source file:///path/to/app0; cache default build/retail-templates
  export-templates [--cache DIR]
                             the injected menu reads the encrypted templates (vehiclelayouts.meta, audio)
                             through the running game and exports them; pulled into the cache, verified
  index-archetypes [--source URL] [--output FILE]
                             build the archetype index (stock archetypes -> typ, box) that convert-map,
                             convert-mapmod and convert-mlo read, from the game image's typs (read-only)
  pack-select N              toggle installed pack N (Custom Packs page order) in packs/active
  pack-autoload              run the Custom Packs one-press load from the host
  pack-card-show             draw the last requested pack card centred on screen (host debug)
  pack-card-release          hide the card and release the dict after the draw lists drain
  doctor                     run read-only target and worker checks
  session stage|next|go|check|stop PLAN [NAME] [...]
                             a hardware test session from a plan (developer): stage its packs,
                             select a process's packs, wait for single-player and inject with the
                             pack-notes follower (go --klog: also the klog) running, check its lines
  logs [N|--worker|--kernel] inspect loader or worker diagnostics

Set PS5_HOST=<console-ip> for console commands; offline conversion and validation do not need it.
The menu starts hidden and opens with R1 + D-pad Left. Select an admitted target with
GTAV_TARGET; ppsa04264-01.010.002 remains the default. Historical measurement commands
are unavailable in this launcher. Set GTAV_AUTO_TARGET=1 for health/control commands against
an installed universal package; source injection and offline conversion still use GTAV_TARGET.
EOF
    exit 2
    ;;
esac
