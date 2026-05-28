#!/bin/bash
# Mezulla screendump — captures and decodes what's on the OLED right now.
# Like QEMU's screendump command, but over serial.
#
# Usage: ./screendump.sh [PORT] [SAVE_PATH]
#
# Sends a query packet to trigger MezullaScreenDump::dumpToSerial(),
# captures the hex output, decodes it to an image.

set -e

PORT="${1:-/dev/ttyACM0}"
SAVE="${2:-/tmp/mezulla-screen.png}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DECODER="$SCRIPT_DIR/decode-mezulla-screen.py"

python3 << PYEOF
import serial, time, threading, sys, re
import importlib.util

PORT = '$PORT'
SAVE = '$SAVE'
DECODER = '$DECODER'

# Load decoder
spec = importlib.util.spec_from_file_location('decoder', DECODER)
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)

# Start serial reader in background
serial_data = []
stop = False

def reader():
    global stop
    ser = serial.Serial(PORT, 115200, timeout=1)
    while not stop:
        c = ser.read(4096)
        if c:
            serial_data.append(c.decode('utf-8', errors='replace'))
    ser.close()

t = threading.Thread(target=reader, daemon=True)
t.start()
time.sleep(1)

# Send query packet via meshtastic lib
try:
    import meshtastic.serial_interface
    iface = meshtastic.serial_interface.SerialInterface(PORT, noClose=True)
    # Send PRIVATE_APP query (cmd=0x02) to self
    my_num = iface.myInfo.my_node_num
    iface.sendData(bytes([0x02]), portNum=256, wantAck=False,
                   wantResponse=True, destinationId=my_num)
    print(f"Query sent to node 0x{my_num:08x}", file=sys.stderr)
    iface.close()
except Exception as e:
    print(f"Could not send query: {e}", file=sys.stderr)
    print("Falling back to reboot...", file=sys.stderr)
    import subprocess
    stop = True
    t.join(timeout=2)
    subprocess.run(['meshtastic', '--port', PORT, '--reboot'],
                   capture_output=True, timeout=15)
    time.sleep(3)
    stop = False
    t = threading.Thread(target=reader, daemon=True)
    t.start()

# Wait for screen dump to appear
time.sleep(5)
stop = True
t.join(timeout=3)

text = ''.join(serial_data)
url = decoder.decode_screen_dump(text)

buf = decoder.parse_screen_pages(text)
if buf:
    img = decoder.ssd1306_to_image(buf)
    img.save(SAVE)
    print(f"Image: {SAVE}", file=sys.stderr)

    if url:
        print(f"QR: {url}", file=sys.stderr)
    else:
        # Count set pixels to describe content
        clean = re.sub(r'\x1b\[[0-9;]*m', '', text)
        for line in clean.split('\n'):
            if 'MEZULLA-SCREEN' in line and 'dump:' in line:
                print(line.strip(), file=sys.stderr)
                break

    print(SAVE)
else:
    print("ERROR: No screen dump captured", file=sys.stderr)
    sys.exit(1)
PYEOF
