#!/bin/bash
# Mezulla screendump — captures and decodes what's on the OLED right now.
# Like QEMU's screendump command, but over serial.
#
# Usage: ./screendump.sh [PORT] [SAVE_PATH]
#
# Design goals:
#   - Asynchronous to mezulla's normal operation — sends one short query
#     packet to trigger the dump, never blocks the firmware
#   - Single serial-port owner (no fighting between reader thread and
#     SerialInterface)
#   - Captures the dump that fires in response to the query
#
# Implementation: open the serial port ONCE. Send raw Meshtastic-framed
# ToRadio bytes for a PRIVATE_APP query (cmd=0x02). Read the resulting
# serial output (LOG_INFO MEZULLA-SCREEN p00..p73 chunks). Decode.

set -e

PORT="${1:-/dev/ttyACM0}"
SAVE="${2:-/tmp/mezulla-screen.png}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DECODER="$SCRIPT_DIR/decode-mezulla-screen.py"

python3 <<PYEOF
import serial, time, struct, importlib.util, sys, re, subprocess

PORT = "$PORT"
SAVE = "$SAVE"
DECODER = "$DECODER"

# Get our own node number first via meshtastic --info (closes port cleanly).
info = subprocess.run(["meshtastic", "--port", PORT, "--info"],
                      capture_output=True, text=True, timeout=15)
m = re.search(r'"myNodeNum":\s*(\d+)', info.stdout)
if not m:
    print("[screendump] could not read myNodeNum", file=sys.stderr)
    sys.exit(1)
my_num = int(m.group(1))

# Build the raw ToRadio bytes for a PRIVATE_APP query (cmd=0x02) to self.
# Protobuf encoding by hand:
#   Data: portnum=1(varint=256), payload=2(bytes=[0x02]), want_response=3(bool=1)
#   MeshPacket: to=2(fixed32), decoded=4(message), id=6(fixed32), want_ack=10(bool)
#   ToRadio: packet=1(message)
def varint(n):
    out = b""
    while n > 0x7f:
        out += bytes([(n & 0x7f) | 0x80])
        n >>= 7
    return out + bytes([n & 0x7f])

# Data
data = b""
data += bytes([0x08]) + varint(256)        # portnum = 256
data += bytes([0x12, 0x01, 0x02])          # payload = [0x02]
data += bytes([0x18, 0x01])                # want_response = true

# MeshPacket
mp = b""
mp += bytes([0x15]) + struct.pack("<I", my_num)  # to = fixed32 my_num
mp += bytes([0x22, len(data)]) + data            # decoded = Data
import time as _t
pkt_id = int(_t.time() * 1000) & 0xFFFFFFFF
mp += bytes([0x35]) + struct.pack("<I", pkt_id)  # id = fixed32

# ToRadio
to_radio = bytes([0x0a, len(mp)]) + mp           # packet = MeshPacket

# Meshtastic serial framing: 0x94 0xC3 [length:2 BE] [payload]
frame = bytes([0x94, 0xc3]) + struct.pack(">H", len(to_radio)) + to_radio

# Open the port ONCE. Send the frame. Then read response.
ser = serial.Serial(PORT, 115200, timeout=1)
# Drain anything already buffered
ser.reset_input_buffer()
ser.write(frame)
ser.flush()

# Read for up to 5 seconds OR until we see the last expected dump chunk.
data_bytes = b""
deadline = time.time() + 5
while time.time() < deadline:
    c = ser.read(4096)
    if c:
        data_bytes += c
        if b"MEZULLA-SCREEN] p73:" in data_bytes:
            break
ser.close()

text = data_bytes.decode("utf-8", errors="replace")

spec = importlib.util.spec_from_file_location("decoder", DECODER)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

buf = mod.parse_screen_pages(text)
if buf is None:
    chunk_count = len(re.findall(rb"MEZULLA-SCREEN\] p\d\d:", data_bytes))
    print(f"[screendump] only saw {chunk_count}/32 dump chunks", file=sys.stderr)
    sys.exit(1)

img = mod.ssd1306_to_image(buf)
img.save(SAVE)
print(f"[screendump] {SAVE}")
PYEOF
