#!/bin/bash
# Full reset of a Mezulla board: erase flash, flash firmware, set identity, capture token.
# Usage: ./reset-mezulla.sh [PORT] [OWNER_NAME] [SHORT_NAME]

set -e

PORT="${1:-/dev/ttyACM0}"
OWNER="${2:-Mezulla 007}"
SHORT="${3:-007}"
FIRMWARE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DEEPLINK_FILE="$HOME/src/Tern/docs/handoffs/mezulla-deeplink.txt"

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

echo "5. Rebooting and capturing token..."
(meshtastic --port "$PORT" --reboot 2>&1 &)
sleep 13

TOKEN=$(python3 -c "
import serial, time, re
ser = serial.Serial('$PORT', 115200, timeout=1)
data = b''
end = time.time() + 15
while time.time() < end:
    c = ser.read(4096)
    if c: data += c
ser.close()
text = data.decode('utf-8', errors='replace')
m = re.search(r'token=([a-f0-9]{6,})', text)
if m: print(m.group(1)[:8])
")

if [ -z "$TOKEN" ]; then
    echo "ERROR: Could not capture token"
    exit 1
fi

# Read OUR node ID from myNodeNum (not from the mesh node list which includes other devices)
NODE=$(meshtastic --port "$PORT" --info 2>/dev/null | grep -o '"myNodeNum": [0-9]*' | grep -o '[0-9]*' | python3 -c "import sys; print(f'{int(sys.stdin.read().strip()):08x}')")
if [ -z "$NODE" ]; then
    echo "ERROR: Could not read node ID"
    exit 1
fi
echo "Node ID: $NODE"
URL="tern://p?n=${NODE}&t=${TOKEN}"
echo "$URL" > "$DEEPLINK_FILE"

echo ""
echo "=== Done ==="
echo "Deep link: $URL"
echo "Written to: $DEEPLINK_FILE"
echo "Board is unclaimed, showing QR, ready for pairing test."
