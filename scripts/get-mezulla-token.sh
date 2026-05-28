#!/bin/bash
# Reads the current Mezulla pairing token from the board via serial.
# Outputs the full deep link URL.
# Usage: ./get-mezulla-token.sh [PORT] [TIMEOUT]

PORT="${1:-/dev/ttyACM0}"
TIMEOUT="${2:-20}"

echo "Waiting for boot log on $PORT (press RST or reflash)..." >&2

TOKEN=$(timeout "$TIMEOUT" cat "$PORT" 2>/dev/null | strings | grep -oP 'token=\K[a-f0-9]+' | head -1)

if [ -z "$TOKEN" ]; then
    echo "ERROR: no token found within ${TIMEOUT}s" >&2
    exit 1
fi

NODE=$(meshtastic --port "$PORT" --info 2>/dev/null | grep -o '"myNodeNum": [0-9]*' | grep -o '[0-9]*' | python3 -c "import sys; print(f'{int(sys.stdin.read().strip()):08x}')")

if [ -z "$NODE" ]; then
    echo "ERROR: could not read node ID" >&2
    exit 1
fi

URL="tern://p?n=${NODE}&t=${TOKEN}"
echo "$URL"

OUTFILE="$HOME/src/Tern/docs/handoffs/mezulla-deeplink.txt"
echo "$URL" > "$OUTFILE"
echo "Written to $OUTFILE" >&2
