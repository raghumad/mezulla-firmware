#!/bin/bash
# Reads the Mezulla deep link by decoding the QR code from the OLED screen dump.
# Board must be unclaimed and showing QR. Reboots to trigger a fresh screen dump.
# Usage: ./get-mezulla-token.sh [PORT]

set -e

PORT="${1:-/dev/ttyACM0}"
FIRMWARE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DECODER="$FIRMWARE_DIR/scripts/decode-mezulla-screen.py"
OUTFILE="$HOME/src/Tern/docs/handoffs/mezulla-deeplink.txt"

echo "Rebooting board and capturing QR screen dump..." >&2
(meshtastic --port "$PORT" --reboot 2>&1 &)
sleep 13

URL=$(python3 -c "
import serial, time, sys, importlib.util

ser = serial.Serial('$PORT', 115200, timeout=1)
data = b''
end = time.time() + 20
while time.time() < end:
    c = ser.read(4096)
    if c: data += c
ser.close()
text = data.decode('utf-8', errors='replace')

spec = importlib.util.spec_from_file_location('decoder', '$DECODER')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

url = mod.decode_screen_dump(text)
if url:
    print(url)
else:
    print('DECODE_FAILED', file=sys.stderr)
")

if [ -z "$URL" ] || [ "$URL" = "DECODE_FAILED" ]; then
    echo "ERROR: Could not decode QR from OLED screen dump" >&2
    echo "Is the board unclaimed and showing QR?" >&2
    exit 1
fi

echo "$URL"
echo "$URL" > "$OUTFILE"
echo "Written to $OUTFILE" >&2
