#!/bin/bash
# Full reset of a Mezulla board: erase flash, flash firmware, set identity,
# decode deep link from the QR code on the OLED screen dump.
# Usage: ./reset-mezulla.sh [PORT] [OWNER_NAME] [SHORT_NAME]

set -e

PORT="${1:-/dev/ttyACM0}"
OWNER="${2:-Mezulla 007}"
SHORT="${3:-007}"
FIRMWARE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DEEPLINK_FILE="$HOME/src/Tern/docs/handoffs/mezulla-deeplink.txt"
DECODER="$FIRMWARE_DIR/scripts/decode-mezulla-screen.py"

echo "=== Mezulla Reset ==="
echo "Port: $PORT"
echo "Owner: $OWNER ($SHORT)"
echo ""

echo "1. Erasing flash..."
~/.platformio/penv/bin/python -m esptool --port "$PORT" erase_flash 2>&1 | tail -2

echo "2. Flashing firmware..."
cd "$FIRMWARE_DIR"
pio run -e tlora-v2-1-1_6 -t upload --upload-port "$PORT" 2>&1 | grep -E "SUCCESS|FAILED"

echo "3. Waiting for boot (15s)..."
sleep 15

echo "4. Setting device identity..."
meshtastic --port "$PORT" --set-owner "$OWNER" --set-owner-short "$SHORT" 2>&1 | tail -1

echo "5. Rebooting and decoding QR from OLED..."
(meshtastic --port "$PORT" --reboot 2>&1 &)
sleep 13

URL=$(python3 -c "
import serial, time, re, sys, importlib.util

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
    echo "ERROR: Could not decode QR from OLED screen dump"
    exit 1
fi

echo "$URL" > "$DEEPLINK_FILE"

echo ""
echo "=== Done ==="
echo "Deep link (from QR): $URL"
echo "Written to: $DEEPLINK_FILE"
echo "Board is unclaimed, showing QR, ready for pairing test."
