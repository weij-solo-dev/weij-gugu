#!/usr/bin/env bash
# monitor.sh — print the device log for N seconds (default 30) after resetting it.
#   ./monitor.sh            30 s of logs from a fresh boot
#   ./monitor.sh 120        two minutes
#   GUGU_GREP=call:,minimax: ./monitor.sh 60     only matching lines
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
exec "$PY" "$HERE/tools/monitor.py" --port "$PORT" --reset --seconds "${1:-30}" --grep "${GUGU_GREP:-}"
