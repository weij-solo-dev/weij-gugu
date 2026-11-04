#!/usr/bin/env bash
# gugu.sh — build the firmware and flash the M5Stack StopWatch, from a fresh clone on any Mac.
#
#   ./gugu.sh                              find the board, build, flash, show the first seconds of the boot log
#   ./gugu.sh --port /dev/cu.usbmodemXXXX  when more than one serial device is attached
#
# Nothing has to be installed beforehand except the Xcode command line tools (git, python3, a C compiler).
# On the first run the script clones ESP-IDF v5.5.6 and installs its toolchain, cmake, ninja and Python
# environment (install.sh alone leaves cmake and ninja to Homebrew on macOS; this script installs them too, so
# a Mac without Homebrew works and every Mac builds with the same versions). Everything it creates lives under
# .derived_data/ next to this file: the IDF checkout, the tools, the build directory and sdkconfig. Delete the repository and the whole environment goes with it; nothing is
# written to $HOME. First run: ~5 GB on disk, 5–10 minutes depending on the line (measured 5.5 min: clone
# 1 min, tools 4.5 min, build 40 s, flash 20 s). Later runs: an incremental build and a flash, ~40 s in all. Flashing writes the bootloader, partition table, app and content image only, so Wi-Fi,
# API keys and recorded calls on the device are kept.
#
# Optional environment knobs:
#   GUGU_IDF_GIT=<url>   clone ESP-IDF from a mirror instead of github.com/espressif/esp-idf
#   GUGU_MIRROR=1        fetch the toolchain from dl.espressif.cn and Python packages from a China PyPI mirror
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DD="$HERE/.derived_data"
IDF_VERSION="v5.5.6"
IDF_DIR="$DD/esp-idf"
BUILD_DIR="$DD/build"
SDKCONFIG="$DD/sdkconfig"
TARGET="esp32s3"
export IDF_TOOLS_PATH="$DD/espressif"      # toolchain, cmake, ninja, esptool and IDF's Python venv

PORT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --port) PORT="${2:?--port needs a device path}"; shift 2 ;;
        -h|--help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1 (try --help)" >&2; exit 2 ;;
    esac
done

say() { printf '\033[1;36m[gugu]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[gugu]\033[0m %s\n' "$*" >&2; exit 1; }

[ -d "$HERE/app" ] && [ -f "$HERE/CMakeLists.txt" ] || die "app/ and CMakeLists.txt are not next to this script: run it from the gugu repository"

# ---- 1. the board: exactly one Espressif USB-Serial/JTAG device (VID 303A, PID 1001) ----------------
find_port() {
    [ -n "$PORT" ] && return
    local c; local cands=()
    if [ "$(uname -s)" = Darwin ]; then
        # ioreg prints each USB device followed by its children; the CDC child carries the tty path.
        # ioreg names the tty.* node; the cu.* callout device is the one to open on macOS.
        while IFS= read -r c; do case "$c" in /dev/tty.*) c="/dev/cu.${c#/dev/tty.}" ;; esac; cands+=("$c"); done < <(ioreg -r -c IOUSBHostDevice -l -w0 2>/dev/null |
            awk '/"idVendor" = /{v=$NF} /"idProduct" = /{p=$NF} /"IODialinDevice" = /{gsub(/"/,"",$NF); if (v==12346 && p==4097) print $NF}' |
            sort -u)
        if [ ${#cands[@]} -eq 0 ]; then for c in /dev/cu.usbmodem*; do [ -e "$c" ] && cands+=("$c"); done; fi
    else
        for c in /dev/serial/by-id/usb-Espressif*; do [ -e "$c" ] && cands+=("$(readlink -f "$c")"); done
        if [ ${#cands[@]} -eq 0 ]; then for c in /dev/ttyACM*; do [ -e "$c" ] && cands+=("$c"); done; fi
    fi
    case ${#cands[@]} in
        0) die "no StopWatch on USB: use a data cable, replug it, or pass --port (ls /dev/cu.usbmodem*)" ;;
        1) PORT="${cands[0]}" ;;
        *) die "more than one serial device: ${cands[*]} — pass --port <one of them>" ;;
    esac
}

# ---- 2. ESP-IDF, its tools and Python environment under .derived_data --------------------------------
ensure_idf() {
    if [ "$(uname -s)" = Darwin ] && ! xcode-select -p >/dev/null 2>&1; then
        xcode-select --install >/dev/null 2>&1 || true
        die "the Xcode command line tools are missing: a system dialog is installing them; rerun ./gugu.sh when it finishes"
    fi
    command -v git >/dev/null || die "git not found"
    command -v python3 >/dev/null || die "python3 not found"
    mkdir -p "$DD"
    if [ ! -f "$IDF_DIR/export.sh" ]; then
        say "ESP-IDF $IDF_VERSION is not in .derived_data yet: cloning it (~700 MB, shallow, a few minutes)..."
        rm -rf "$IDF_DIR"
        git clone --branch "$IDF_VERSION" --depth 1 --recursive --shallow-submodules \
            "${GUGU_IDF_GIT:-https://github.com/espressif/esp-idf.git}" "$IDF_DIR"
    fi
    # The stamp names what is installed; renaming it re-runs this block on an older checkout (install.sh is idempotent).
    local stamp="$DD/.tools-installed-$IDF_VERSION-$TARGET-cmake-ninja"
    if [ ! -f "$stamp" ]; then
        say "installing the $TARGET toolchain, cmake, ninja and IDF's Python environment into .derived_data/espressif (~4 GB, about 5 minutes)..."
        if [ "${GUGU_MIRROR:-0}" = 1 ]; then
            export IDF_GITHUB_ASSETS="dl.espressif.cn/github_assets"
            export PIP_INDEX_URL="${PIP_INDEX_URL:-https://pypi.tuna.tsinghua.edu.cn/simple}"
        fi
        "$IDF_DIR/install.sh" "$TARGET"
        # cmake and ninja are "on_request" tools on macOS/Linux in IDF's tools.json: install.sh skips them and
        # expects Homebrew/apt to provide them. Ask for them explicitly so .derived_data is self-contained.
        python3 "$IDF_DIR/tools/idf_tools.py" install cmake ninja
        rm -f "$DD"/.tools-installed-*; touch "$stamp"
    fi
    set +eu
    # shellcheck disable=SC1091
    . "$IDF_DIR/export.sh" >/dev/null
    set -eu
    local t
    for t in idf.py cmake ninja; do
        command -v "$t" >/dev/null || die "$t is not on PATH after sourcing export.sh: delete $stamp and rerun ./gugu.sh"
    done
}

# ---- 3. build, 4. flash, 5. a glance at the boot log --------------------------------------------------
find_port
say "board on $PORT"
ensure_idf
# sdkconfig is generated from sdkconfig.defaults once and then kept, and IDF never re-reads a default for an option
# the file already sets: when the defaults change (a git pull), start sdkconfig over. One full rebuild (a few minutes).
DEFAULTS_SUM="$(cksum < "$HERE/sdkconfig.defaults" | awk '{print $1}')"
DEFAULTS_STAMP="$DD/.sdkconfig-defaults-sum"
if [ -f "$SDKCONFIG" ] && [ "$(cat "$DEFAULTS_STAMP" 2>/dev/null)" != "$DEFAULTS_SUM" ]; then
    say "sdkconfig.defaults changed: regenerating sdkconfig (one full rebuild, a few minutes)"
    rm -f "$SDKCONFIG"
fi
# app/CMakeLists.txt finds the sources with a glob, which CMake evaluates only when it configures: a git pull that
# adds or removes a source file would otherwise build without it (and fail to link). Re-configure when the list changes.
SOURCES_SUM="$(cd "$HERE" && find app \( -name '*.c' -o -name '*.h' \) | LC_ALL=C sort | cksum | awk '{print $1}')"
SOURCES_STAMP="$DD/.app-sources-sum"
if [ -d "$BUILD_DIR" ] && [ "$(cat "$SOURCES_STAMP" 2>/dev/null)" != "$SOURCES_SUM" ]; then
    say "source files added or removed: re-running the CMake configure"
    idf.py -B "$BUILD_DIR" -DSDKCONFIG="$SDKCONFIG" -DIDF_TARGET="$TARGET" reconfigure
fi
say "building into .derived_data/build (incremental)"
idf.py -B "$BUILD_DIR" -DSDKCONFIG="$SDKCONFIG" -DIDF_TARGET="$TARGET" build
echo "$DEFAULTS_SUM" > "$DEFAULTS_STAMP"
echo "$SOURCES_SUM" > "$SOURCES_STAMP"
say "flashing $PORT: bootloader, partition table, app, content image (Wi-Fi, keys and data on the device are kept)"
idf.py -B "$BUILD_DIR" -p "$PORT" flash

say "boot log (8 s):"
LOG="$(python "$HERE/tools/dev/monitor.py" --port "$PORT" --seconds 8 \
      --grep "board ready,ui ready,store ready,wifi got ip,no Wi-Fi configured,no MiniMax key,app: ready,Guru Meditation" 2>&1 | tee /dev/stderr || true)"
echo
case "$LOG" in *"Guru Meditation"*) die "the device crashed while booting: see the log above" ;; esac
PY="$(command -v python)"
if [[ "$LOG" == *"no Wi-Fi configured"* || "$LOG" == *"no MiniMax key"* ]]; then
    say "first-time setup (stored in the device's NVS, survives reflashing):"
    echo "  $PY tools/dev/serial_cmd.py --port $PORT \"wifi <ssid> <password>\" \"key minimax <api key>\" \"stat\""
fi
say "done. Useful next:"
echo "  $PY tools/dev/serial_cmd.py --port $PORT --times \"replay /content/test/zh_01.wav 3\"   # one call, three turns"
echo "  $PY tools/dev/monitor.py --port $PORT --seconds 60                                       # watch the log"
