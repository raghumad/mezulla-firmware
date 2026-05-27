#include "MezullaQrScreen.h"
#include "MezullaOwnershipModule.h"
#include "NodeDB.h"
#include "configuration.h"
#include "qrcodegen.h"
#include <cstdio>
#include <cstring>

// QR Version 2: 25×25 modules. At 2px/module with 3-module quiet zone:
// (25 + 6) × 2 = 62×62 pixels. Fits 128×64 SSD1306.
#define QR_VERSION_MIN 2
#define QR_VERSION_MAX 2
#define QR_MODULE_PX 2
#define QR_QUIET_ZONE 3

static uint8_t qrcode[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_VERSION_MAX)];
static uint8_t tempBuf[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_VERSION_MAX)];
static bool qrGenerated = false;
static int qrSize = 0;
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
             owner.id + 1,
             token);

    bool ok = qrcodegen_encodeText(url, tempBuf, qrcode,
        qrcodegen_Ecc_LOW, QR_VERSION_MIN, QR_VERSION_MAX,
        qrcodegen_Mask_AUTO, true);

    if (!ok) {
        LOG_ERROR("[MEZULLA] qr: encode failed for url=%s", url);
        return;
    }

    qrSize = qrcodegen_getSize(qrcode);
    strncpy(lastToken, token, sizeof(lastToken) - 1);
    qrGenerated = true;

    LOG_INFO("[MEZULLA] qr: displayed, token=%s size=%d", token, qrSize);
}

void MezullaQrScreen::drawPairingQrFrame(OLEDDisplay *display, OLEDDisplayUiState *state,
                                          int16_t x, int16_t y)
{
    ensureQrGenerated();

    if (!qrGenerated)
        return;

    int totalPx = (qrSize + 2 * QR_QUIET_ZONE) * QR_MODULE_PX;

    display->setColor(BLACK);
    display->fillRect(0, 0, 128, 64);

    int16_t originX = (128 - totalPx) / 2;
    int16_t originY = (64 - totalPx) / 2;

    // White background for QR + quiet zone
    display->setColor(WHITE);
    display->fillRect(originX, originY, totalPx, totalPx);

    // Black modules
    display->setColor(BLACK);
    for (int my = 0; my < qrSize; my++) {
        for (int mx = 0; mx < qrSize; mx++) {
            if (qrcodegen_getModule(qrcode, mx, my)) {
                display->fillRect(
                    originX + (QR_QUIET_ZONE + mx) * QR_MODULE_PX,
                    originY + (QR_QUIET_ZONE + my) * QR_MODULE_PX,
                    QR_MODULE_PX, QR_MODULE_PX);
            }
        }
    }
}
