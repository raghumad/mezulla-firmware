#include "MezullaQrScreen.h"
#include "MezullaOwnershipModule.h"
#include "NodeDB.h"
#include "configuration.h"
#include "qrcode.h"
#include <cstdio>

// QR Version 3: 29×29 modules. At 2px/module = 58×58 pixels.
// Fits on 128×64 SSD1306 with room for a label below.
#define QR_VERSION 3
#define QR_MODULE_PX 2
#define QR_MODULES (4 * QR_VERSION + 17)  // 29
#define QR_SIZE_PX (QR_MODULES * QR_MODULE_PX)  // 58

static QRCode qrcode;
// QR V3 = 29×29 modules = 841 bits = 106 bytes. Round up to 128.
static uint8_t qrcodeData[128];
static bool qrGenerated = false;
static char lastToken[33] = {};

static void ensureQrGenerated()
{
    if (!mezullaOwnershipModule)
        return;

    const char *token = mezullaOwnershipModule->getPairingToken();
    if (token[0] == '\0')
        return;

    if (qrGenerated && strcmp(lastToken, token) == 0)
        return;

    char url[80];
    snprintf(url, sizeof(url), "tern://p?n=%s&t=%s",
             owner.id + 1, // skip the '!' prefix on the node hex id
             token);

    qrcode_initText(&qrcode, qrcodeData, QR_VERSION, ECC_LOW, url);
    strncpy(lastToken, token, sizeof(lastToken) - 1);
    qrGenerated = true;

    LOG_INFO("[MEZULLA] qr: displayed, token=%s", token);
}

void MezullaQrScreen::drawPairingQrFrame(OLEDDisplay *display, OLEDDisplayUiState *state,
                                          int16_t x, int16_t y)
{
    ensureQrGenerated();

    if (!qrGenerated)
        return;

    display->setColor(BLACK);
    display->fillRect(0, 0, 128, 64);

    // Center the QR code horizontally, leave 6px at bottom for label
    int16_t qrX = x + (128 - QR_SIZE_PX) / 2;
    int16_t qrY = y + 0;

    // Draw white quiet zone background (2px border around QR)
    display->setColor(WHITE);
    display->fillRect(qrX - 2, qrY - 2, QR_SIZE_PX + 4, QR_SIZE_PX + 4);

    // Draw QR modules
    display->setColor(BLACK);
    for (uint8_t my = 0; my < QR_MODULES; my++) {
        for (uint8_t mx = 0; mx < QR_MODULES; mx++) {
            if (qrcode_getModule(&qrcode, mx, my)) {
                display->fillRect(qrX + mx * QR_MODULE_PX,
                                  qrY + my * QR_MODULE_PX,
                                  QR_MODULE_PX, QR_MODULE_PX);
            }
        }
    }

    // Label below QR
    display->setColor(WHITE);
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->setFont(ArialMT_Plain_10);
    display->drawString(x + 64, y + QR_SIZE_PX + 1, "Scan to pair");
}
