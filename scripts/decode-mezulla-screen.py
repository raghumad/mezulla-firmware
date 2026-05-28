#!/usr/bin/env python3
"""Decode the QR code from Mezulla's OLED screen dump.

Reads the [MEZULLA-SCREEN] page0..page7 hex lines from serial output,
reconstructs the 128x64 SSD1306 pixel buffer, and decodes the QR code.

Usage:
    # From serial capture:
    python3 decode-mezulla-screen.py < serial_output.txt

    # Pipe from board boot:
    meshtastic --port /dev/ttyACM0 --reboot && sleep 3 && \
      timeout 20 cat /dev/ttyACM0 | python3 decode-mezulla-screen.py

    # Programmatic (imported):
    from decode_mezulla_screen import decode_screen_dump
    url = decode_screen_dump(serial_text)
"""
import re
import sys
from PIL import Image
from pyzbar.pyzbar import decode as pyzbar_decode


def strip_ansi(text: str) -> str:
    """Remove ANSI escape sequences from text."""
    return re.sub(r'\x1b\[[0-9;]*m', '', text)


def parse_screen_pages(text: str) -> bytes | None:
    """Extract 8 pages of hex from [MEZULLA-SCREEN] log lines.

    Format: p<page><quarter>:<32 bytes hex>  (page 0-7, quarter 0-3)
    32 chunks total = 8 pages x 4 quarters x 32 bytes = 1024 bytes.
    """
    text = strip_ansi(text)
    chunks = {}  # {(page, quarter): bytes}

    for line in text.split('\n'):
        m = re.search(r'\[MEZULLA-SCREEN\] p(\d)(\d):([0-9a-f]+)', line)
        if m:
            page = int(m.group(1))
            quarter = int(m.group(2))
            chunks[(page, quarter)] = bytes.fromhex(m.group(3))

    if len(chunks) != 32:
        return None

    buf = b''
    for page in range(8):
        for q in range(4):
            chunk = chunks.get((page, q), b'')
            if len(chunk) != 32:
                return None
            buf += chunk

    return buf


def ssd1306_to_image(buf: bytes) -> Image.Image:
    """Convert SSD1306 buffer (1024 bytes) to a PIL Image (128x64)."""
    img = Image.new('L', (128, 64), 0)
    pixels = img.load()

    for page in range(8):
        for col in range(128):
            byte = buf[page * 128 + col]
            for bit in range(8):
                y = page * 8 + bit
                if byte & (1 << bit):
                    pixels[col, y] = 255

    return img


def decode_qr(img: Image.Image) -> str | None:
    """Decode QR code from image, return data string or None."""
    # pyzbar works better with upscaled images for small QR codes
    scale = 4
    big = img.resize((img.width * scale, img.height * scale), Image.NEAREST)

    results = pyzbar_decode(big)
    if results:
        return results[0].data.decode('utf-8')

    # Try inverted (QR might be white-on-black or black-on-white)
    from PIL import ImageOps
    inverted = ImageOps.invert(big)
    results = pyzbar_decode(inverted)
    if results:
        return results[0].data.decode('utf-8')

    return None


def decode_screen_dump(text: str) -> str | None:
    """Full pipeline: serial text → QR data string."""
    buf = parse_screen_pages(text)
    if buf is None:
        return None

    img = ssd1306_to_image(buf)
    return decode_qr(img)


if __name__ == '__main__':
    text = sys.stdin.read()

    buf = parse_screen_pages(text)
    if buf is None:
        print("ERROR: Could not parse screen dump pages", file=sys.stderr)
        sys.exit(1)

    img = ssd1306_to_image(buf)

    # Save image for debugging
    img.save('/tmp/mezulla-screen.png')
    print(f"Screen image saved to /tmp/mezulla-screen.png", file=sys.stderr)

    url = decode_qr(img)
    if url is None:
        print("ERROR: Could not decode QR code from screen", file=sys.stderr)
        sys.exit(1)

    print(url)
