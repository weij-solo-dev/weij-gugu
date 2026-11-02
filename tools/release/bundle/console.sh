#!/usr/bin/env bash
# console.sh — send commands to the device console over USB and print what comes back.
#   ./console.sh "wifi <ssid> <password>" "key minimax <api key>" "stat"
#   ./console.sh "replay /content/test/zh_01.wav 3"
#   ./console.sh help
# Uses the virtualenv flash.sh created (run ./flash.sh once first). GUGU_PORT overrides port detection.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
VENV="${GUGU_FLASH_VENV:-$HOME/.venvs/gugu-flash}"
PY="$VENV/bin/python"
[ -x "$PY" ] || { echo "run ./flash.sh once first (it installs the serial tools)" >&2; exit 1; }
PORT="${GUGU_PORT:-}"
if [ -z "$PORT" ]; then
    PORT="$("$PY" -c "from serial.tools import list_ports; h=[p.device for p in list_ports.comports() if p.vid==0x303A and p.pid==0x1001]; print(h[0] if h else '')")"
fi
[ -n "$PORT" ] || { echo "no StopWatch found on USB" >&2; exit 1; }
exec "$PY" "$HERE/tools/serial_cmd.py" --port "$PORT" --wait 12 --timeout 120 --times "$@"
