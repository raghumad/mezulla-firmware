#include "MezullaScreenDump.h"
#include "graphics/Screen.h"
#include "configuration.h"
#include <cstdio>

// SSD1306 buffer layout: 128 columns x 8 pages (each page = 8 rows).
// buffer[page * 128 + col] contains 8 vertical pixels, LSB = top row of page.
// Total: 128 * 8 = 1024 bytes = 128 * 64 pixels.

static bool getPixel(const uint8_t *buf, int x, int y)
{
    if (!buf || x < 0 || x >= 128 || y < 0 || y >= 64)
        return false;
    int page = y / 8;
    int bit = y % 8;
    return (buf[page * 128 + x] >> bit) & 1;
}

void MezullaScreenDump::dumpToSerial()
{
    if (!screen) {
        LOG_INFO("[MEZULLA-SCREEN] screen is null");
        return;
    }

    auto *display = screen->getDisplayDevice();
    if (!display) {
        LOG_INFO("[MEZULLA-SCREEN] display is null");
        return;
    }

    const uint8_t *buf = display->buffer;
    if (!buf) {
        LOG_INFO("[MEZULLA-SCREEN] buffer is null");
        return;
    }

    // Count set pixels
    int setPixels = 0;
    for (int i = 0; i < 1024; i++) {
        uint8_t b = buf[i];
        while (b) { setPixels += (b & 1); b >>= 1; }
    }

    LOG_INFO("[MEZULLA-SCREEN] dump: %d/%d pixels set (%.1f%%)",
             setPixels, 128 * 64, setPixels * 100.0 / (128 * 64));

    // Dump as hex, 128 bytes per line (one page)
    for (int page = 0; page < 8; page++) {
        char hex[128 * 2 + 1];
        for (int col = 0; col < 128; col++) {
            snprintf(hex + col * 2, 3, "%02x", buf[page * 128 + col]);
        }
        LOG_INFO("[MEZULLA-SCREEN] page%d: %s", page, hex);
    }
}

bool MezullaScreenDump::bufferContainsQR()
{
    if (!screen) return false;
    auto *display = screen->getDisplayDevice();
    if (!display || !display->buffer) return false;

    const uint8_t *buf = display->buffer;

    // QR finder pattern: 7x7 square with 5x5 inner, 3x3 core.
    // At 2px/module, each finder is 14x14 pixels.
    // Check top-left corner for a dark region (the finder pattern).
    // The QR is centered, so the top-left finder starts at roughly
    // (128-62)/2 = 33px from the left, (64-62)/2 = 1px from the top.

    // Simple heuristic: count dark pixels in the expected finder region.
    // If >60% are dark, it's a QR finder.
    int darkCount = 0;
    int total = 0;
    for (int y = 0; y < 16; y++) {
        for (int x = 30; x < 48; x++) {
            if (getPixel(buf, x, y)) darkCount++;
            total++;
        }
    }

    float ratio = (float)darkCount / total;
    LOG_INFO("[MEZULLA-SCREEN] QR check: finder region %d/%d dark (%.0f%%)", darkCount, total, ratio * 100);

    return ratio > 0.4;
}

bool MezullaScreenDump::bufferIsBlank()
{
    if (!screen) return true;
    auto *display = screen->getDisplayDevice();
    if (!display || !display->buffer) return true;

    int setPixels = 0;
    for (int i = 0; i < 1024; i++) {
        uint8_t b = display->buffer[i];
        while (b) { setPixels += (b & 1); b >>= 1; }
    }

    float pct = setPixels * 100.0 / (128 * 64);
    return pct < 5.0;
}
