#!/usr/bin/env bash
# Build (CMake workflow), flash via J-Link/Commander, optional SEGGER RTT console.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CMAKE_DIR="${CMAKE_DIR:-$ROOT/cmake_gcc}"
TARGET_NAME="${TARGET_NAME:-opendisplay-bg22}"
FLASH_DEVICE="${FLASH_DEVICE:-EFR32BG22C222F352GM40}"
FLASH_BACKEND="${FLASH_BACKEND:-commander}"
SWD_SPEED="${SWD_SPEED:-4000}"
DO_FLASH_BOOTLOADER=1
DO_OTA_IMAGE=1
BOOTLOADER_ART="${BOOTLOADER_ART:-}"
OTA_IMAGE_OUT="${OTA_IMAGE_OUT:-}"

if [[ -f "$ROOT/scripts/env.sh" ]]; then
  # shellcheck source=/dev/null
  source "$ROOT/scripts/env.sh"
fi

ARTIFACTS_DIR="${ARTIFACTS_DIR:-$ROOT/artifacts}"
DO_COLLECT_ARTIFACTS=1

DO_BUILD=1
DO_FLASH=1
DO_RTT=0
RTT_ONLY=0
RTT_ENABLED=1
DO_GBL_ONLY=0
DO_CLEAN=0
DO_MASS_ERASE="${DO_MASS_ERASE:-0}"

usage() {
  cat <<EOF
Usage: $0 [options]

  --no-build          Skip cmake build (flash existing artifact only)
  --no-flash          Build only
  --gbl-only          Only run Simplicity Commander gbl create (no build/flash; needs .s37)
  --rtt               After flash: background J-Link RTT telnet + JLinkRTTClient
  --rtt-only          RTT only (no build/flash)
  --no-rtt            Build firmware with RTT disabled (OD_ENABLE_RTT=OFF)
  --no-bootloader     Do not flash bootloader image
  --no-ota-image      Do not generate OTA .gbl image
  --bootloader-art F  Explicit bootloader artifact (.s37/.hex)
  --ota-out FILE      Output OTA image path (default build/base/<target>.gbl)
  --artifacts-dir DIR Copy bootloader + app + OTA .gbl here (default: $ROOT/artifacts)
  --no-artifacts      Do not copy files or print memory summary
  --device PART       Simplicity Commander / J-Link device (default $FLASH_DEVICE)
  --cmake-dir DIR     CMake tree (default $CMAKE_DIR)
  --backend TYPE      commander | jlink (default $FLASH_BACKEND)
  --clean             Remove CMake build directory (\$CMAKE_DIR/build) before configure/build
  --clean-only        Only remove \$CMAKE_DIR/build (no configure, build, flash, or artifacts)
  --mass-erase        Before flashing: Simplicity Commander device recover (Series 2 AAP). Needed when
                      SWD fails with debug locked; clears lock + main flash. Uses Commander even
                      if --backend jlink. Plain mass erase cannot attach while locked. Default off.

  Env: COMMANDER, POST_BUILD_EXE (Commander path, same as CMake toolchain), ARTIFACTS_DIR,
       ARM_GCC_DIR (optional, for arm-none-eabi-size), FLASH_DEVICE,
       DO_MASS_ERASE (set to 1 for recover-before-flash, same as --mass-erase),
       SWD_SPEED, JLINK_GDB_SERVER, JLINKEXE, JLINK_RTT_CLIENT,
       RTT_BACKEND (gdbserver | exe, default gdbserver), RTT_GDBSERVER_ARGS,
       RTT_TELNET_PORT (default 19021), RTT_ARGS (extra args for JLinkRTTClient).
       Optional CMake (-D) when set: OPENDISPLAY_BUILD_ID, OD_APP_VERSION,
       OD_SL_APPLICATION_VERSION, OD_GENERATE_GBL_OTA

  Default RTT_BACKEND=gdbserver: JLinkGDBServer stays connected (J-Link Commander often exits and breaks exe mode).
  If Commander stays open on your setup, try RTT_BACKEND=exe (JLinkExe + sleep infinity) instead.
EOF
  exit 0
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build) DO_BUILD=0 ;;
    --no-flash) DO_FLASH=0 ;;
    --gbl-only) DO_BUILD=0; DO_FLASH=0; DO_GBL_ONLY=1 ;;
    --rtt) DO_RTT=1 ;;
    --rtt-only) RTT_ONLY=1; DO_BUILD=0; DO_FLASH=0; DO_RTT=1 ;;
    --no-rtt) RTT_ENABLED=0 ;;
    --no-bootloader) DO_FLASH_BOOTLOADER=0 ;;
    --no-ota-image) DO_OTA_IMAGE=0 ;;
    --bootloader-art) BOOTLOADER_ART="${2:?}"; shift ;;
    --ota-out) OTA_IMAGE_OUT="${2:?}"; shift ;;
    --artifacts-dir) ARTIFACTS_DIR="${2:?}"; shift ;;
    --no-artifacts) DO_COLLECT_ARTIFACTS=0 ;;
    --device) FLASH_DEVICE="${2:?}"; shift ;;
    --cmake-dir) CMAKE_DIR="${2:?}"; shift ;;
    --backend) FLASH_BACKEND="${2:?}"; shift ;;
    --clean) DO_CLEAN=1 ;;
    --mass-erase) DO_MASS_ERASE=1 ;;
    --clean-only)
      DO_CLEAN=1
      DO_BUILD=0
      DO_FLASH=0
      DO_OTA_IMAGE=0
      DO_COLLECT_ARTIFACTS=0
      DO_RTT=0
      RTT_ONLY=0
      DO_GBL_ONLY=0
      ;;
    -h|--help) usage ;;
    *) echo "Unknown option: $1" >&2; usage ;;
  esac
  shift
done

if [[ "$RTT_ONLY" -eq 1 ]] && [[ "$DO_RTT" -ne 1 ]]; then
  DO_RTT=1
fi

if [[ "$DO_GBL_ONLY" -eq 1 ]]; then
  DO_OTA_IMAGE=1
fi

if [[ "$DO_CLEAN" -eq 1 ]]; then
  b="$CMAKE_DIR/build"
  if [[ -e "$b" ]]; then
    echo "==> Clean: removing $b"
    rm -rf "$b"
  else
    echo "==> Clean: $b already absent"
  fi
  if [[ "$DO_BUILD" -eq 0 && "$DO_FLASH" -eq 0 && "$DO_GBL_ONLY" -eq 0 && "$DO_RTT" -eq 0 ]]; then
    exit 0
  fi
fi

find_commander() {
  local c dir
  if [[ -n "${COMMANDER:-}" && -x "${COMMANDER}" ]]; then
    echo "$COMMANDER"
    return 0
  fi
  if [[ -n "${POST_BUILD_EXE:-}" && -x "${POST_BUILD_EXE}" ]]; then
    echo "$POST_BUILD_EXE"
    return 0
  fi
  if command -v commander >/dev/null 2>&1; then
    command -v commander
    return 0
  fi
  if command -v slt >/dev/null 2>&1; then
    dir=$(slt where commander 2>/dev/null | tr -d '\r\n') || true
    if [[ -n "$dir" ]]; then
      if [[ -x "$dir/commander" ]]; then
        echo "$dir/commander"
        return 0
      fi
      if [[ "$(uname -s)" == Darwin && -x "$dir/Contents/MacOS/commander" ]]; then
        echo "$dir/Contents/MacOS/commander"
        return 0
      fi
      if [[ -x "$dir/commander.exe" ]]; then
        echo "$dir/commander.exe"
        return 0
      fi
    fi
  fi
  c="${HOME:-}/.silabs/slt/installs/archive/commander/commander"
  if [[ -x "$c" ]]; then
    echo "$c"
    return 0
  fi
  c="$ROOT/../SimplicityCommander-Linux/commander/commander"
  if [[ -x "$c" ]]; then
    echo "$c"
    return 0
  fi
  return 1
}

od_run_device_recover() {
  local cmd
  cmd=$(find_commander) || {
    echo "ERROR: --mass-erase needs Simplicity Commander (device recover) to unlock debug-locked EFR32. Set COMMANDER or install Commander." >&2
    return 1
  }
  echo "==> Device recover (debug lock / bricked recovery): $cmd device recover -d $FLASH_DEVICE --speed $SWD_SPEED"
  "$cmd" device recover -d "$FLASH_DEVICE" --speed "$SWD_SPEED"
}

find_bootloader_artifact() {
  if [[ -n "${BOOTLOADER_ART}" ]]; then
    if [[ -f "${BOOTLOADER_ART}" ]]; then
      echo "${BOOTLOADER_ART}"
      return 0
    fi
    echo "Bootloader artifact not found: ${BOOTLOADER_ART}" >&2
    return 1
  fi
  local f
  for f in \
    "$ROOT/bootloader-artifact/opendisplay-bootloader.s37" \
    "$CMAKE_DIR/build/base/bootloader-apploader.s37" \
    "$ROOT/../bootloader/artifact/opendisplay-bootloader.s37" \
    "$CMAKE_DIR/build/base/bootloader.s37" \
    "$CMAKE_DIR/build/base/apploader.s37"; do
    if [[ -f "$f" ]]; then
      echo "$f"
      return 0
    fi
  done
  f=$(find "$CMAKE_DIR/build" -maxdepth 5 \( -name "bootloader*.s37" -o -name "apploader*.s37" \) -print -quit)
  if [[ -n "$f" ]]; then
    echo "$f"
    return 0
  fi
  return 1
}

find_artifact() {
  local base="$CMAKE_DIR/build/base"
  local f
  for ext in s37 hex; do
    f="$base/${TARGET_NAME}.$ext"
    if [[ -f "$f" ]]; then
      echo "$f"
      return 0
    fi
  done
  f=$(find "$CMAKE_DIR/build" -maxdepth 4 \( -name "${TARGET_NAME}.s37" -o -name "${TARGET_NAME}.hex" \) -print -quit)
  if [[ -n "$f" ]]; then
    echo "$f"
    return 0
  fi
  echo "No ${TARGET_NAME}.s37 or .hex under $CMAKE_DIR/build — build first." >&2
  return 1
}

find_jlink_exe() {
  if [[ -n "${JLINKEXE:-}" && -x "${JLINKEXE}" ]]; then
    echo "$JLINKEXE"
    return 0
  fi
  if command -v JLinkExe >/dev/null 2>&1; then
    command -v JLinkExe
    return 0
  fi
  return 1
}

find_jlink_gdb_server() {
  if [[ -n "${JLINK_GDB_SERVER:-}" && -x "${JLINK_GDB_SERVER}" ]]; then
    echo "$JLINK_GDB_SERVER"
    return 0
  fi
  if command -v JLinkGDBServer >/dev/null 2>&1; then
    command -v JLinkGDBServer
    return 0
  fi
  if jlink=$(find_jlink_exe 2>/dev/null); then
    local d
    d="$(dirname "$jlink")"
    if [[ -x "$d/JLinkGDBServer" ]]; then
      echo "$d/JLinkGDBServer"
      return 0
    fi
  fi
  return 1
}

find_rtt_client() {
  if [[ -n "${JLINK_RTT_CLIENT:-}" && -x "${JLINK_RTT_CLIENT}" ]]; then
    echo "$JLINK_RTT_CLIENT"
    return 0
  fi
  if command -v JLinkRTTClient >/dev/null 2>&1; then
    command -v JLinkRTTClient
    return 0
  fi
  if jlink=$(find_jlink_exe 2>/dev/null); then
    local d
    d="$(dirname "$jlink")"
    for n in JLinkRTTClient JLinkRTTClientExe; do
      if [[ -x "$d/$n" ]]; then
        echo "$d/$n"
        return 0
      fi
    done
  fi
  return 1
}

find_arm_none_eabi_size() {
  local s root
  if [[ -n "${ARM_GCC_DIR:-}" && -x "${ARM_GCC_DIR}/bin/arm-none-eabi-size" ]]; then
    echo "${ARM_GCC_DIR}/bin/arm-none-eabi-size"
    return 0
  fi
  if command -v arm-none-eabi-size >/dev/null 2>&1; then
    command -v arm-none-eabi-size
    return 0
  fi
  if command -v slt >/dev/null 2>&1; then
    root=$(slt where gcc-arm-none-eabi 2>/dev/null | tr -d '\r\n') || true
    if [[ -n "$root" && -x "$root/bin/arm-none-eabi-size" ]]; then
      echo "$root/bin/arm-none-eabi-size"
      return 0
    fi
  fi
  return 1
}

# Must match autogen/linkerfile.ld (application slot + main RAM, excluding 4 B bootloader reset).
od_print_memory_summary() {
  local elf sz line text data bss flash_used ram_used f_free r_free
  local app_flash=$((0x44000))
  local ram_main=$((0x7ffc))
  elf="$CMAKE_DIR/build/base/${TARGET_NAME}.out"
  if [[ ! -f "$elf" ]]; then
    echo "==> Memory: no ELF at $elf (skip size summary)"
    return 0
  fi
  sz=$(find_arm_none_eabi_size) || {
    echo "==> Memory: arm-none-eabi-size not found (set ARM_GCC_DIR or PATH); skip summary"
    return 0
  }
  line=$("$sz" --format=berkeley "$elf" 2>/dev/null | tail -1) || true
  if [[ -z "$line" ]]; then
    echo "==> Memory: could not parse size output"
    return 0
  fi
  read -r text data bss _rest <<<"$line"
  if [[ -z "${text:-}" || -z "${data:-}" || -z "${bss:-}" ]]; then
    echo "==> Memory: unexpected size format: $line"
    return 0
  fi
  flash_used=$((text + data))
  ram_used=$((data + bss))
  f_free=$((app_flash - flash_used))
  r_free=$((ram_main - ram_used))
  echo "==> Memory / flash layout (linker: app FLASH 0x12000 + ${app_flash} bytes, RAM ${ram_main} bytes)"
  echo "    Application image in flash (text+data): $flash_used bytes ($((flash_used / 1024)) KiB)"
  echo "    Free in application FLASH region:       $f_free bytes ($((f_free / 1024)) KiB)"
  echo "    RAM static (data+bss):                  $ram_used bytes ($((ram_used / 1024)) KiB)"
  if [[ "$r_free" -lt 1024 ]]; then
    echo "    Free in main RAM region (approx):       $r_free bytes"
  else
    echo "    Free in main RAM region (approx):       $r_free bytes ($((r_free / 1024)) KiB)"
  fi
  echo "    (Bootloader+Apploader 0x00000–0x11FFF and NVM3 tail are not included in app slot figures.)"
}

od_collect_artifacts() {
  local dst bl app_s37 app_hex app_out gbl n
  dst="$ARTIFACTS_DIR"
  mkdir -p "$dst"
  n=0
  if bl=$(find_bootloader_artifact 2>/dev/null); then
    cp -f "$bl" "$dst/bootloader-apploader.s37"
    echo "    + bootloader-apploader.s37  (full flash, image 1)"
    n=$((n + 1))
  else
    echo "    (no bootloader .s37 found — skipped)"
  fi
  app_s37="$CMAKE_DIR/build/base/${TARGET_NAME}.s37"
  app_hex="$CMAKE_DIR/build/base/${TARGET_NAME}.hex"
  app_out="$CMAKE_DIR/build/base/${TARGET_NAME}.out"
  if [[ -f "$app_s37" ]]; then
    cp -f "$app_s37" "$dst/${TARGET_NAME}.s37"
    echo "    + ${TARGET_NAME}.s37  (full flash, application)"
    n=$((n + 1))
  fi
  if [[ -f "$app_hex" ]]; then
    cp -f "$app_hex" "$dst/${TARGET_NAME}.hex"
    echo "    + ${TARGET_NAME}.hex"
    n=$((n + 1))
  fi
  if [[ -f "$app_out" ]]; then
    cp -f "$app_out" "$dst/${TARGET_NAME}.out"
    echo "    + ${TARGET_NAME}.out  (ELF for debug)"
    n=$((n + 1))
  fi
  gbl="${OTA_IMAGE_OUT:-$CMAKE_DIR/build/base/${TARGET_NAME}.gbl}"
  if [[ -f "$gbl" ]]; then
    cp -f "$gbl" "$dst/${TARGET_NAME}.gbl"
    echo "    + ${TARGET_NAME}.gbl  (BLE OTA / Apploader)"
    n=$((n + 1))
  else
    echo "    (no .gbl at $gbl — skipped)"
  fi
  echo "==> Artifacts directory: $dst  ($n files copied)"
  ls -lh "$dst" 2>/dev/null | tail -n +2 || ls -l "$dst"
}

# JLinkRTTClient talks to the RTT port of a long-lived J-Link process.
# J-Link Commander (JLinkExe) often exits right after connect; JLinkGDBServer stays attached (default).
# Use RTT_BACKEND=exe (JLinkExe + sleep) when Commander stays running.
run_rtt_session() {
  local rtt log pid port mode gdb jlink
  rtt=$(find_rtt_client) || {
    echo "JLinkRTTClient not found. Add J-Link to PATH or set JLINK_RTT_CLIENT." >&2
    exit 1
  }
  port="${RTT_TELNET_PORT:-19021}"
  log=$(mktemp "${TMPDIR:-/tmp}/opendisplay-bg22-jlink-rtt.XXXXXX.log")
  mode="${RTT_BACKEND:-gdbserver}"

  if [[ "$mode" == "gdbserver" ]]; then
    gdb=$(find_jlink_gdb_server) || {
      echo "JLinkGDBServer not found. Install SEGGER J-Link, set JLINK_GDB_SERVER, or RTT_BACKEND=exe." >&2
      exit 1
    }
    echo "==> RTT: $gdb -nogui … -rtttelnetport $port (device $FLASH_DEVICE)"
    # shellcheck disable=SC2086
    "$gdb" -nogui -device "$FLASH_DEVICE" -if SWD -speed "$SWD_SPEED" -rtttelnetport "$port" ${RTT_GDBSERVER_ARGS:-} >"$log" 2>&1 &
    pid=$!
  elif [[ "$mode" == "exe" ]]; then
    jlink=$(find_jlink_exe) || {
      echo "JLinkExe not found. Install SEGGER J-Link or set JLINKEXE." >&2
      exit 1
    }
    echo "==> RTT: $jlink (Commander) … -RTTTelnetPort $port (RTT_BACKEND=exe; use gdbserver if this dies)"
    (
      set +e
      "$jlink" -device "$FLASH_DEVICE" -if SWD -speed "$SWD_SPEED" -autoconnect 1 "-RTTTelnetPort" "$port"
      exec sleep infinity
    ) >"$log" 2>&1 &
    pid=$!
  else
    echo "RTT_BACKEND must be gdbserver or exe (got $mode)" >&2
    rm -f "$log"
    exit 1
  fi

  echo "==> RTT: $rtt (Ctrl+C stops client and server)"
  sleep 3
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "ERROR: J-Link RTT server process exited. Log:" >&2
    cat "$log" >&2 || true
    rm -f "$log"
    exit 1
  fi

  cleanup_rtt_bg() {
    kill "$pid" 2>/dev/null || true
    rm -f "$log"
  }
  trap 'cleanup_rtt_bg; exit 130' INT
  trap 'cleanup_rtt_bg; exit 143' TERM

  # shellcheck disable=SC2086
  "$rtt" -RTTTelnetPort "$port" ${RTT_ARGS:-} || true

  cleanup_rtt_bg
}

if [[ "$DO_BUILD" -eq 1 ]]; then
  echo "==> Configure + build (RTT enabled: $RTT_ENABLED)"
  echo "    Directory: $CMAKE_DIR"
  cfg=(--preset project)
  [[ "$RTT_ENABLED" -eq 0 ]] && cfg+=(-DOD_ENABLE_RTT=OFF)
  [[ -n "${OPENDISPLAY_BUILD_ID:-}" ]] && cfg+=(-DOPENDISPLAY_BUILD_ID="${OPENDISPLAY_BUILD_ID}")
  [[ -n "${OD_APP_VERSION:-}" ]] && cfg+=(-DOD_APP_VERSION="${OD_APP_VERSION}")
  [[ -n "${OD_SL_APPLICATION_VERSION:-}" ]] && cfg+=(-DOD_SL_APPLICATION_VERSION="${OD_SL_APPLICATION_VERSION}")
  [[ -n "${OD_GENERATE_GBL_OTA:-}" ]] && cfg+=(-DOD_GENERATE_GBL_OTA="${OD_GENERATE_GBL_OTA}")
  (cd "$CMAKE_DIR" && cmake "${cfg[@]}" && cmake --build --preset default_config)
fi

if [[ "$DO_FLASH" -eq 1 ]]; then
  ART=$(find_artifact)
  echo "==> Artifact: $ART"

  if [[ "$DO_MASS_ERASE" -eq 1 ]]; then
    od_run_device_recover || exit 1
  fi

  if [[ "$FLASH_BACKEND" == commander ]]; then
    CMD=$(find_commander) || {
      echo "Simplicity Commander not found. Set COMMANDER or use --backend jlink" >&2
      exit 1
    }
    if [[ "$DO_FLASH_BOOTLOADER" -eq 1 ]]; then
      if BL_ART=$(find_bootloader_artifact); then
        echo "==> Flash bootloader: $CMD flash -d $FLASH_DEVICE $BL_ART"
        "$CMD" flash -d "$FLASH_DEVICE" "$BL_ART"
      else
        echo "==> Bootloader image not found under build output (set --bootloader-art to override); flashing app only"
      fi
    fi
    echo "==> Flash app: $CMD flash -d $FLASH_DEVICE $ART"
    "$CMD" flash -d "$FLASH_DEVICE" "$ART"
  elif [[ "$FLASH_BACKEND" == jlink ]]; then
    if [[ "$DO_FLASH_BOOTLOADER" -eq 1 ]]; then
      echo "==> NOTE: bootloader flashing is only supported by commander backend in this script"
    fi
    if ! command -v JLinkExe >/dev/null 2>&1; then
      echo "JLinkExe not in PATH" >&2
      exit 1
    fi
    TMP=$(mktemp)
    trap 'rm -f "$TMP"' EXIT
    {
      echo "loadfile $ART"
      echo "r"
      echo "q"
    } >"$TMP"
    echo "==> Flash: JLinkExe SWD device=$FLASH_DEVICE"
    JLinkExe -device "$FLASH_DEVICE" -if SWD -speed "$SWD_SPEED" -autoconnect 1 -CommanderScript "$TMP"
  else
    echo "FLASH_BACKEND must be commander or jlink" >&2
    exit 1
  fi
elif [[ "$DO_BUILD" -eq 1 ]] && [[ "$DO_FLASH" -eq 0 ]]; then
  echo "==> Skipping flash (--no-flash)"
fi

if [[ "$DO_OTA_IMAGE" -eq 1 ]] && [[ "$DO_BUILD" -eq 1 || "$DO_FLASH" -eq 1 || "$DO_GBL_ONLY" -eq 1 ]]; then
  APP_ART=$(find_artifact) || {
    if [[ "$DO_GBL_ONLY" -eq 1 ]]; then
      echo "ERROR: --gbl-only needs ${TARGET_NAME}.s37 or .hex under $CMAKE_DIR/build (build first)." >&2
      exit 1
    fi
    APP_ART=""
  }
  OUT_GBL="${OTA_IMAGE_OUT:-$CMAKE_DIR/build/base/${TARGET_NAME}.gbl}"
  CMD=$(find_commander) || {
    echo "WARN: Simplicity Commander not found; skipping OTA .gbl generation." >&2
    APP_ART=""
  }
  if [[ -n "${APP_ART}" ]]; then
    echo "==> OTA image: $CMD gbl create $OUT_GBL --app $APP_ART"
    if "$CMD" gbl create "$OUT_GBL" --app "$APP_ART"; then
      echo "==> OTA image generated: $OUT_GBL"
    else
      echo "WARN: OTA .gbl generation failed; continuing." >&2
      if [[ "$DO_GBL_ONLY" -eq 1 ]]; then
        exit 1
      fi
    fi
  elif [[ "$DO_GBL_ONLY" -eq 1 ]]; then
    exit 1
  fi
fi

if [[ "$DO_COLLECT_ARTIFACTS" -eq 1 ]] && [[ "$DO_BUILD" -eq 1 || "$DO_FLASH" -eq 1 || "$DO_GBL_ONLY" -eq 1 ]]; then
  echo "==> Staging artifacts + memory summary"
  od_collect_artifacts
  od_print_memory_summary
fi

if [[ "$DO_RTT" -eq 1 ]]; then
  run_rtt_session
fi

exit 0
