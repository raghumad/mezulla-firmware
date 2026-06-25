#include "MezullaQrScreen.h"
#include "MezullaOwnershipModule.h"
#include "MezullaScreenDump.h"
#include "NodeDB.h"
#include "configuration.h"
#include "qrcodegen.h"
#include "esp_mac.h"
#include <cstdio>
#include <cstring>

// QR Version 3: 29×29 modules — needed since the pairing URL now also carries
// the board's BLE MAC (&m=) so the phone connects to THIS board, not whichever
// Meshtastic board answers first. At 2px/module with a 2-module quiet zone:
// (29 + 4) × 2 = 66px; centered on the 64px-tall SSD1306 the modules (58px)
// sit fully on-screen with ~3px of quiet zone top/bottom — scannable.
// (Version 2 only holds ~32 bytes; n+t+m is ~45.)
#define QR_VERSION_MIN 2
#define QR_VERSION_MAX 3
#define QR_MODULE_PX 2
#define QR_QUIET_ZONE 2

static uint8_t qrcode[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_VERSION_MAX)];
static uint8_t tempBuf[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_VERSION_MAX)];
static bool qrGenerated = false;
static bool qrDumped = false;
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

    // Embed the board's BLE MAC (ESP_MAC_BT == the address Android sees while
    // scanning — same value shown on the right column below). The phone uses it
    // to connect to THIS exact board instead of the first Meshtastic board it
    // finds, which is what let pairing latch onto the wrong board. Keep n= so
    // the claim can be addressed to this node (avoids a LoRa broadcast) and so
    // the phone can cross-check the claim reply's `from`.
    uint8_t qrMac[6] = {0};
    esp_read_mac(qrMac, ESP_MAC_BT);
    char url[96];
    snprintf(url, sizeof(url), "tern://p?n=%s&t=%s&m=%02x%02x%02x%02x%02x%02x",
             owner.id + 1, token,
             qrMac[0], qrMac[1], qrMac[2], qrMac[3], qrMac[4], qrMac[5]);

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
    qrDumped = false;

    LOG_INFO("[MEZULLA] qr: url=%s", url);
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

    // Layout: QR left-justified, identity text on the right.
    // The pilot can cross-reference these three identifiers as Android's
    // "Pair and connect" dialog appears:
    //   - Long name  (e.g. "Mezulla 007") — what Android shows
    //   - MAC                              — verifies which physical board
    // This means the pair dialog is never a surprise — pilot pre-saw the
    // identity right here on the OLED.
    int16_t originX = 1;
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

    // Identity text on the right column. QR + quiet zone takes ~62px on
    // the left; the right column has ~64px to work with.
    const int16_t textX = originX + totalPx + 2;
    display->setColor(WHITE);
    display->setFont(ArialMT_Plain_10);
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    // Long name (e.g. "Mezulla 007") — matches what Android pair dialog shows
    display->drawString(textX, 0, owner.long_name);

    // MAC of the BT radio (efuse base + 2). Split across two lines because
    // 17 chars at ArialMT_Plain_10 doesn't fit in 64px.
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    char macLine1[10];
    char macLine2[12];
    snprintf(macLine1, sizeof(macLine1), "%02X:%02X:%02X",
             mac[0], mac[1], mac[2]);
    snprintf(macLine2, sizeof(macLine2), ":%02X:%02X:%02X",
             mac[3], mac[4], mac[5]);
    display->drawString(textX, 16, "MAC:");
    display->drawString(textX, 28, macLine1);
    display->drawString(textX, 40, macLine2);

    // Footer hint
    display->drawString(textX, 52, "Scan w/cam");

    if (!qrDumped) {
        qrDumped = true;
        MezullaScreenDump::dumpToSerial();
    }
}
