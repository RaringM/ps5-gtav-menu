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
#   ./menu-ctl.sh custom-probe                upload a probe file and report what GTA can read
#   ./menu-ctl.sh custom-device               mount gtavmenu:/ in the engine (CUSTOM_DEVICE=1 build)
#   ./menu-ctl.sh custom-prepare              build the inactive authored-texture package
#   ./menu-ctl.sh custom-verify [package-dir] verify its exact offline identity
#   ./menu-ctl.sh custom-upload [package-dir] atomically stage it; activation remains unchanged
#   ./menu-ctl.sh custom-readback [package]   compare the staged console package byte-for-byte
#   ./menu-ctl.sh custom-activate             fail closed until the engine mount route is qualified
#   ./menu-ctl.sh custom-status [package]     report staged/catalog state without activating it
#   ./menu-ctl.sh custom-cleanup [package] --confirm-game-closed
#   ./menu-ctl.sh doctor                      read-only health report
#
# --all is a documented no-op: the game-thread rows (weapons, skin, spawn) already
# unlock once the frame hook is live. The menu stays hidden after injection and opens with
# R1+D-pad Left; --show explicitly opens it for diagnostic runs.
#
# Env: PS5_HOST (default 192.168.0.57), PS5DEBUG_PORT (744, ps5debug control),
# PS5_PORT (9021, prospero-deploy), PS5_FTP_PORT (1337, ELF upload). The menu injects
# into the foreground GTA title selected by GTAV_TARGET, so the two-eboot decoy is handled by
# pinning ps5debug to the injected pid.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

PS5_HOST="${PS5_HOST:-192.168.0.57}"
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
CUSTOM_PACK_ID="gtavmenu-authored-bc1-v1"
CUSTOM_PACKAGE_DEFAULT="build/custom-assets/$CUSTOM_PACK_ID"
CUSTOM_PACK_REMOTE="$CUSTOM_REMOTE/packs/$CUSTOM_PACK_ID"
CUSTOM_CATALOG_REMOTE="$CUSTOM_REMOTE/catalog.json"
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
CLIENT="tools/menu_client.py"
TARGET_ID="GTAV_FEATURE_MENU"
# Code cave for the ptrace-free bootstrap stub: the executable segment's tail padding. Build-specific,
# so the value lives in make/config.mk's per-target arm (PAYLOAD_LOADER_CAVE_ADDR) rather than here --
# hardcoding 01.010.002's 0x3796000 in the lane would write the stub into live code on another build.
# Set CAVE_ADDR in the environment only to override the target's value.
CAVE_ADDR="${CAVE_ADDR:-}"
CAVE_ALLOC="${CAVE_ALLOC:-0x200000}"
HOOK_STATUS_NAME="disabled"

# When RUN_CLIENT_TIMEOUT is set (seconds), bound each ps5debug call so a slow/never-
# matching scan can't hang the script. Numeric, so unquoted expansion is safe.
RUN_CLIENT_TIMEOUT=""
run_client() {
  ${RUN_CLIENT_TIMEOUT:+timeout $RUN_CLIENT_TIMEOUT} \
    "$PY" "$CLIENT" --host "$PS5_HOST" --port "$PS5DEBUG_PORT" "$@";
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
# command then fails against a perfectly healthy worker. Resolving by symbol is what
# research/tools/live/peek_frame_hook.py already does for its counters; this gives the status/command paths the
# same treatment. Echoing nothing leaves the client to fall back on its scan.
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
ftp_mkdir() { curl -s --max-time 10 -Q "MKD $1" "ftp://$PS5_HOST:$PS5_FTP_PORT/" >/dev/null 2>&1 || true; }
# Upload a local file ($1) to a remote path ($2) over FTP -- the feature-menu ELF the
# payload loader maps into GTA.
ftp_put() { curl -s --max-time 60 -T "$1" "ftp://$PS5_HOST:$PS5_FTP_PORT$2"; }

custom_remove_readback() {
  local directory="$1"
  rm -f "$directory/resources/gtavmenu_authored_bc1.ptd" \
    "$directory/manifest.json" "$directory/catalog-entry.json"
  rmdir "$directory/resources" "$directory" 2>/dev/null || true
}

custom_get() {
  local remote="$1" output="$2"
  curl --fail --silent --show-error --max-time 30 \
    "ftp://$PS5_HOST:$PS5_FTP_PORT$remote" -o "$output"
}

custom_put() {
  local source="$1" remote="$2"
  curl --fail --silent --show-error --max-time 60 -T "$source" \
    "ftp://$PS5_HOST:$PS5_FTP_PORT$remote"
}

custom_mkdir_new() {
  local remote="$1"
  curl --fail --silent --show-error --max-time 20 -Q "MKD $remote" \
    "ftp://$PS5_HOST:$PS5_FTP_PORT/" >/dev/null
}

custom_verify_local() {
  local package="$1"
  "$PY" tools/prepare_custom_pack.py verify "$package"
}

do_custom_prepare() {
  [ "$#" -eq 0 ] || { echo "usage: $0 custom-prepare" >&2; return 2; }
  make custom-texture-pack
  echo "[menu-ctl] custom pack verified: $CUSTOM_PACK_ID"
  echo "[menu-ctl] inactive package: $CUSTOM_PACKAGE_DEFAULT"
}

do_custom_verify() {
  [ "$#" -le 1 ] || { echo "usage: $0 custom-verify [package-dir]" >&2; return 2; }
  local package="${1:-$CUSTOM_PACKAGE_DEFAULT}"
  custom_verify_local "$package"
  echo "[menu-ctl] custom pack verified: $CUSTOM_PACK_ID"
  echo "[menu-ctl] activation remains disabled"
}

# Download exactly the three allowlisted package files, run the same strict verifier, and compare
# each byte with the already-verified local package. Nothing in this path writes catalog.json.
custom_compare_remote() {
  local package="$1" remote_root="$2" readback
  custom_verify_local "$package" >/dev/null
  readback="$(mktemp -d /tmp/gtavmenu-custom-readback.XXXXXX)"
  mkdir "$readback/resources"
  if ! custom_get "$remote_root/manifest.json" "$readback/manifest.json" \
      || ! custom_get "$remote_root/catalog-entry.json" "$readback/catalog-entry.json" \
      || ! custom_get "$remote_root/resources/gtavmenu_authored_bc1.ptd" \
        "$readback/resources/gtavmenu_authored_bc1.ptd"; then
    custom_remove_readback "$readback"
    echo "[menu-ctl] custom pack readback failed" >&2
    return 1
  fi
  if ! custom_verify_local "$readback" >/dev/null \
      || ! cmp -s "$package/manifest.json" "$readback/manifest.json" \
      || ! cmp -s "$package/catalog-entry.json" "$readback/catalog-entry.json" \
      || ! cmp -s "$package/resources/gtavmenu_authored_bc1.ptd" \
        "$readback/resources/gtavmenu_authored_bc1.ptd"; then
    custom_remove_readback "$readback"
    echo "[menu-ctl] custom pack readback differs from the verified local package" >&2
    return 1
  fi
  custom_remove_readback "$readback"
}

do_custom_readback() {
  [ "$#" -le 1 ] || { echo "usage: $0 custom-readback [package-dir]" >&2; return 2; }
  local package="${1:-$CUSTOM_PACKAGE_DEFAULT}"
  custom_compare_remote "$package" "$CUSTOM_PACK_REMOTE"
  echo "[menu-ctl] custom pack readback verified: $CUSTOM_PACK_ID"
}

do_custom_upload() {
  [ "$#" -le 1 ] || { echo "usage: $0 custom-upload [package-dir]" >&2; return 2; }
  local package="${1:-$CUSTOM_PACKAGE_DEFAULT}" existing staging
  custom_verify_local "$package" >/dev/null

  existing="$(ftp_cat "$CUSTOM_PACK_REMOTE/manifest.json")"
  if [ -n "$existing" ]; then
    do_custom_readback "$package"
    echo "[menu-ctl] custom pack already staged; activation unchanged"
    return 0
  fi

  staging="$CUSTOM_REMOTE/packs/.upload-$CUSTOM_PACK_ID-$$"
  ftp_mkdir "$CUSTOM_REMOTE"
  ftp_mkdir "$CUSTOM_REMOTE/packs"
  if ! custom_mkdir_new "$staging" || ! custom_mkdir_new "$staging/resources"; then
    echo "[menu-ctl] custom staging directory creation failed; catalog unchanged" >&2
    echo "[menu-ctl] no existing staging path will be overwritten: $staging" >&2
    return 1
  fi
  if ! custom_put "$package/resources/gtavmenu_authored_bc1.ptd" \
      "$staging/resources/gtavmenu_authored_bc1.ptd" \
      || ! custom_put "$package/manifest.json" "$staging/manifest.json" \
      || ! custom_put "$package/catalog-entry.json" "$staging/catalog-entry.json"; then
    echo "[menu-ctl] custom pack staging upload failed; catalog unchanged" >&2
    echo "[menu-ctl] incomplete staging path retained for diagnosis: $staging" >&2
    return 1
  fi
  if ! custom_compare_remote "$package" "$staging"; then
    echo "[menu-ctl] custom pack staging readback failed; catalog unchanged" >&2
    echo "[menu-ctl] incomplete staging path retained for diagnosis: $staging" >&2
    return 1
  fi

  # Rename the complete directory as one FTP operation. If the server cannot rename directories,
  # fail with the staging path intact; never fall back to piecemeal publication.
  if ! curl --fail --silent --show-error --max-time 30 \
      -Q "RNFR $staging" -Q "RNTO $CUSTOM_PACK_REMOTE" \
      "ftp://$PS5_HOST:$PS5_FTP_PORT/" >/dev/null; then
    echo "[menu-ctl] custom pack atomic publication failed; catalog unchanged" >&2
    echo "[menu-ctl] complete staging path retained for diagnosis: $staging" >&2
    return 1
  fi
  do_custom_readback "$package"
  echo "[menu-ctl] custom pack uploaded and read back; activation unchanged"
}

do_custom_activate() {
  [ "$#" -eq 0 ] || { echo "usage: $0 custom-activate" >&2; return 2; }
  echo "[menu-ctl] custom activation refused: engine mount/lifetime route is not qualified" >&2
  return 1
}

do_custom_preview() {
  [ "$#" -eq 0 ] || { echo "usage: $0 custom-preview" >&2; return 2; }
  echo "[menu-ctl] custom preview refused: engine mount/lifetime route is not qualified" >&2
  return 1
}

do_custom_status() {
  [ "$#" -le 1 ] || { echo "usage: $0 custom-status [package-dir]" >&2; return 2; }
  local package="${1:-$CUSTOM_PACKAGE_DEFAULT}" catalog
  custom_verify_local "$package" >/dev/null
  catalog="$(ftp_cat "$CUSTOM_CATALOG_REMOTE")"
  if [ -n "$catalog" ]; then
    echo "[menu-ctl] unexpected custom catalog present while activation is unqualified" >&2
    echo "[menu-ctl] no mutation performed; inspect $CUSTOM_CATALOG_REMOTE" >&2
    return 1
  fi
  if [ -n "$(ftp_cat "$CUSTOM_PACK_REMOTE/manifest.json")" ]; then
    do_custom_readback "$package"
    echo "[menu-ctl] custom status: staged, verified, inactive; fresh GTA process required after future activation"
  else
    echo "[menu-ctl] custom status: not staged, inactive, engine route unqualified"
  fi
}

do_custom_cleanup() {
  [ "$#" -ge 1 ] && [ "$#" -le 2 ] \
    || { echo "usage: $0 custom-cleanup [package-dir] --confirm-game-closed" >&2; return 2; }
  local package="$CUSTOM_PACKAGE_DEFAULT" confirmation catalog quarantine
  if [ "$#" -eq 1 ]; then
    confirmation="$1"
  else
    package="$1"
    confirmation="$2"
  fi
  if [ "$confirmation" != "--confirm-game-closed" ]; then
    echo "[menu-ctl] cleanup refused: fully exit GTA, then pass --confirm-game-closed" >&2
    return 2
  fi
  custom_verify_local "$package" >/dev/null
  catalog="$(ftp_cat "$CUSTOM_CATALOG_REMOTE")"
  if [ -n "$catalog" ]; then
    echo "[menu-ctl] cleanup refused: custom catalog exists; no file was removed" >&2
    return 1
  fi
  if [ -z "$(ftp_cat "$CUSTOM_PACK_REMOTE/manifest.json")" ]; then
    echo "[menu-ctl] custom cleanup: reviewed pack is absent; nothing changed"
    return 0
  fi
  do_custom_readback "$package" >/dev/null
  quarantine="$CUSTOM_REMOTE/packs/.remove-$CUSTOM_PACK_ID-$$"
  if ! curl --fail --silent --show-error --max-time 20 \
      -Q "RNFR $CUSTOM_PACK_REMOTE" -Q "RNTO $quarantine" \
      "ftp://$PS5_HOST:$PS5_FTP_PORT/" >/dev/null; then
    echo "[menu-ctl] cleanup could not atomically retire the verified pack; no delete attempted" >&2
    return 1
  fi
  if ! curl --fail --silent --show-error --max-time 20 \
      -Q "DELE $quarantine/resources/gtavmenu_authored_bc1.ptd" \
      -Q "DELE $quarantine/catalog-entry.json" \
      -Q "DELE $quarantine/manifest.json" \
      -Q "RMD $quarantine/resources" \
      -Q "RMD $quarantine" \
      "ftp://$PS5_HOST:$PS5_FTP_PORT/" >/dev/null; then
    echo "[menu-ctl] verified pack left the pack path but quarantine cleanup is incomplete: $quarantine" >&2
    return 1
  fi
  echo "[menu-ctl] removed verified inactive pack: $CUSTOM_PACK_REMOTE"
  echo "[menu-ctl] retained $CUSTOM_REMOTE and all unrelated content"
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

# Write a fresh marker to custom/probe.txt, then ask the worker to read it from inside GTA, which only
# works through the loader's sandbox mount (needs a CUSTOM_MOUNT=1 build).
do_custom_probe() {
  if [ "$#" -ne 0 ]; then
    echo "usage: $0 custom-probe" >&2
    return 2
  fi
  local marker probe
  marker="gtavmenu-probe-$(date +%s)"
  probe="$(mktemp /tmp/gtavmenu-probe.XXXXXX)"
  printf '%s\n' "$marker" >"$probe"
  ftp_mkdir "$CUSTOM_REMOTE"
  if ! ftp_put "$probe" "$CUSTOM_REMOTE/probe.txt"; then
    rm -f "$probe"
    echo "[menu-ctl] probe upload failed" >&2
    return 1
  fi
  rm -f "$probe"
  echo "[menu-ctl] uploaded $CUSTOM_REMOTE/probe.txt ($marker)"
  do_map_action PROBE_CUSTOM_MOUNT
  echo "[menu-ctl] expect 'custom probe $CUSTOM_REMOTE/probe.txt rc=<bytes> text=$marker' in: $0 status"
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
        echo "[menu-ctl] $a is unavailable in the production launcher; see research/ for historical lanes." >&2
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
    "quit_guard=$QUIT_GUARD_ENABLE/appstate=$QUIT_GUARD_APPSTATE_ENABLE) ..."
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
    GTAV_MENU_ENABLE_CUSTOM_DEVICE="${CUSTOM_DEVICE:-0}" \
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

# Build the injected worker ELF and stage it on the console over FTP. Shared by the
# inject-now (cave-inject) and wait-then-inject (watch) lanes.
stage_worker_elf() {
  build_feature_menu_elf
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
  # unmapped safely inside the same game process.
  make payload-loader-build PAYLOAD_LOADER_INJECT=1 PAYLOAD_LOADER_INSTALL_BROKER="${PAYLOAD_LOADER_INSTALL_BROKER:-1}" \
    PAYLOAD_LOADER_INSTALL_RENDER_PHASE="$phase_install" \
    PAYLOAD_LOADER_INJECT_GUARD="${INJECT_GUARD_ENABLE:-1}" \
    PAYLOAD_LOADER_CUSTOM_MOUNT="${CUSTOM_MOUNT:-0}" \
    GTAV_MENU_ENABLE_CUSTOM_DEVICE="${CUSTOM_DEVICE:-0}" \
    PAYLOAD_LOADER_PROBE_NOSTOP="${PROBE_NOSTOP:-1}" \
    PAYLOAD_LOADER_NOSTOP_IO="${NOSTOP_IO:-1}" \
    PAYLOAD_LOADER_QUIESCE_ADDR="${QUIESCE_ADDR:-0}" "$@" >/dev/null
  if [ ! -f "$PAYLOAD_LOADER_ELF" ]; then
    echo "[menu-ctl] !! build failed: $PAYLOAD_LOADER_ELF not produced" >&2
    echo "[menu-ctl]    (needs the PS5 payload SDK; see PS5_PAYLOAD_SDK in make/config.mk)" >&2
    exit 1
  fi
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
  injected_pid="$(printf '%s\n' "$log" | grep -oE 'inject: overall OK \(pid=[0-9]+\)' | tail -1 | grep -oE '[0-9]+' | tail -1)"
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
do_watch() {
  parse_start_args "$@"

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
  echo "[menu-ctl] asking the resident loader to stop the worker and exact-retire its hooks ..."
  if ! ftp_put /dev/null "$DAEMON_STOP_REMOTE"; then
    echo "[menu-ctl] !! could not write the stop sentinel over FTP (is FTP up on $PS5_HOST:$PS5_FTP_PORT?)" >&2
    exit 1
  fi
  # Confirm the terminal daemon-exited record, not merely the first stop acknowledgement: retirement
  # deliberately keeps ownership while worker/phase/.text cleanup is still pending.
  local waited=0 timeout="${DAEMON_STOP_TIMEOUT:-30}" log
  while :; do
    log="$(ftp_cat "$PAYLOAD_LOG_REMOTE")"
    if printf '%s\n' "$log" | grep -q "persistent: daemon exited"; then
      echo "[menu-ctl] worker stopped, owned hooks retired, and daemon exited."
      return 0
    fi
    if [ "$waited" -ge "$timeout" ]; then
      echo "[menu-ctl] !! exact retirement did not reach 'persistent: daemon exited' within ${timeout}s." >&2
      echo "[menu-ctl]    The stop request remains pending and daemon ownership is retained; inspect logs." >&2
      return 1
    fi
    sleep 2
    waited=$((waited + 2))
  done
}


# cave-inject uses the validated ptrace-free bootstrap and production render-phase install.
do_cave_inject() {
  parse_start_args "$@"

  if [ "$PREFLIGHT_ENABLE" = "1" ]; then
    echo "[menu-ctl] pre-flight: confirming player-world readiness (read-only anchor check)..."
    if ! "$PY" tools/read_player_ped_anchor.py --host "$PS5_HOST" --port "$PS5DEBUG_PORT" --require-ready; then
      echo "[menu-ctl] !! pre-flight failed: player world not ready, wrong build, or ps5debug unreachable." >&2
      exit 1
    fi
    echo "[menu-ctl] pre-flight OK: player ped is live on the pinned build."
  fi

  # The worker starts its own thread on this lane; the loader deliberately does not.
  SELF_START_WORKER=1 stage_worker_elf
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

cmd="${1:-}"
shift || true
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
  custom-probe)
    do_custom_probe "$@"
    ;;
  custom-device)
    [ "$#" -eq 0 ] || { echo "usage: $0 custom-device" >&2; exit 2; }
    do_map_action MOUNT_CUSTOM_DEVICE
    echo "[menu-ctl] expect 'custom device gtavmenu:/probe.txt lookup=ours size=<n>' in: $0 status"
    ;;
  custom-prepare)
    do_custom_prepare "$@"
    ;;
  custom-verify)
    do_custom_verify "$@"
    ;;
  custom-upload)
    do_custom_upload "$@"
    ;;
  custom-readback)
    do_custom_readback "$@"
    ;;
  custom-activate)
    do_custom_activate "$@"
    ;;
  custom-preview)
    do_custom_preview "$@"
    ;;
  custom-status)
    do_custom_status "$@"
    ;;
  custom-cleanup)
    do_custom_cleanup "$@"
    ;;
  doctor)
    do_doctor
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
  custom-probe               report whether GTA can read the custom-asset folder
  custom-device              mount gtavmenu:/ through the engine (CUSTOM_DEVICE=1 build)
  custom-prepare             build the verified inactive authored-texture pack
  custom-verify [package]    verify exact package schema, paths, sizes and hashes
  custom-upload [package]    atomically upload/read back; do not activate
  custom-readback [package]  verify the staged console package byte-for-byte
  custom-activate            refuse until the engine mount route is qualified
  custom-preview             refuse until activation and loading are qualified
  custom-status [package]    report staged and inactive catalog state
  custom-cleanup [package] --confirm-game-closed
                             remove only the verified inactive authored pack
  doctor                     run read-only target and worker checks
  logs [N|--worker|--kernel] inspect loader or worker diagnostics

The menu starts hidden and opens with R1 + D-pad Left. Select an admitted target with
GTAV_TARGET; ppsa04264-01.010.002 remains the default. Retired measurement commands
are preserved under research/.
EOF
    exit 2
    ;;
esac
