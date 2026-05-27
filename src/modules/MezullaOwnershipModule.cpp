#include "MezullaOwnershipModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "configuration.h"
#include "graphics/Screen.h"
#include <cstring>

#if defined(ARCH_ESP32)
#include <esp_random.h>
#endif

MezullaOwnershipModule *mezullaOwnershipModule;

MezullaOwnershipModule::MezullaOwnershipModule()
    : SinglePortModule("mezulla", meshtastic_PortNum_PRIVATE_APP)
{
    if (!isClaimed()) {
        generatePairingToken();
        // QR token is the authentication — BLE PIN on top is redundant
        // and causes silent write drops before bonding completes.
        config.bluetooth.mode = meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN;
        LOG_INFO("[MEZULLA] ownership: unclaimed, BLE set to NO_PIN (QR token is auth)");
    }
}

bool MezullaOwnershipModule::isClaimed() const
{
    return devicestate.mezulla_owner_id[0] != '\0';
}

const char *MezullaOwnershipModule::getOwnerId() const
{
    return devicestate.mezulla_owner_id;
}

const char *MezullaOwnershipModule::getPairingToken() const
{
    return devicestate.mezulla_pairing_token;
}

void MezullaOwnershipModule::generatePairingToken()
{
    uint8_t raw[4];
#if defined(ARCH_ESP32)
    esp_fill_random(raw, sizeof(raw));
#else
    for (int i = 0; i < 4; i++)
        raw[i] = (uint8_t)random(256);
#endif

    for (int i = 0; i < 4; i++) {
        snprintf(devicestate.mezulla_pairing_token + i * 2, 3, "%02x", raw[i]);
    }

    LOG_INFO("[MEZULLA] qr: new token=%s", devicestate.mezulla_pairing_token);
    nodeDB->saveToDisk(SEGMENT_DEVICESTATE);
}

void MezullaOwnershipModule::clearOwnership()
{
    devicestate.mezulla_owner_id[0] = '\0';
    devicestate.mezulla_pairing_token[0] = '\0';
    nodeDB->saveToDisk(SEGMENT_DEVICESTATE);
    generatePairingToken();
    LOG_INFO("[MEZULLA] reset: ownership cleared");

    if (screen)
        screen->setFrames();
}

ProcessMessage MezullaOwnershipModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (mp.decoded.payload.size < 1)
        return ProcessMessage::CONTINUE;

    uint8_t cmd = mp.decoded.payload.bytes[0];

    switch (cmd) {
    case MEZULLA_CMD_CLAIM:
        handleClaim(mp);
        return ProcessMessage::STOP;
    case MEZULLA_CMD_QUERY:
        handleQuery(mp);
        return ProcessMessage::STOP;
    case MEZULLA_CMD_RELEASE:
        handleRelease(mp);
        return ProcessMessage::STOP;
    default:
        return ProcessMessage::CONTINUE;
    }
}

void MezullaOwnershipModule::handleClaim(const meshtastic_MeshPacket &mp)
{
    // Payload: [cmd:1][token_len:1][token:N][owner_id:rest]
    if (mp.decoded.payload.size < 3) {
        lastReplyStatus = MEZULLA_STATUS_TOKEN_MISMATCH;
        return;
    }

    uint8_t tokenLen = mp.decoded.payload.bytes[1];
    if (mp.decoded.payload.size < 2u + tokenLen + 1u) {
        lastReplyStatus = MEZULLA_STATUS_TOKEN_MISMATCH;
        return;
    }

    char token[33] = {};
    size_t copyLen = tokenLen < 32 ? tokenLen : 32;
    memcpy(token, &mp.decoded.payload.bytes[2], copyLen);

    if (isClaimed()) {
        LOG_WARN("[MEZULLA] claim: rejected, reason=already_claimed");
        lastReplyStatus = MEZULLA_STATUS_ALREADY_CLAIMED;
        return;
    }

    if (strcmp(token, devicestate.mezulla_pairing_token) != 0) {
        LOG_WARN("[MEZULLA] claim: rejected, reason=token_mismatch");
        lastReplyStatus = MEZULLA_STATUS_TOKEN_MISMATCH;
        return;
    }

    const char *ownerId = (const char *)&mp.decoded.payload.bytes[2 + tokenLen];
    size_t ownerIdLen = mp.decoded.payload.size - 2 - tokenLen;
    size_t maxCopy = sizeof(devicestate.mezulla_owner_id) - 1;
    size_t toCopy = ownerIdLen < maxCopy ? ownerIdLen : maxCopy;
    memcpy(devicestate.mezulla_owner_id, ownerId, toCopy);
    devicestate.mezulla_owner_id[toCopy] = '\0';

    nodeDB->saveToDisk(SEGMENT_DEVICESTATE);
    LOG_INFO("[MEZULLA] claim: accepted, owner=%s", devicestate.mezulla_owner_id);
    lastReplyStatus = MEZULLA_STATUS_OK;

    if (screen)
        screen->setFrames();
}

void MezullaOwnershipModule::handleRelease(const meshtastic_MeshPacket &mp)
{
    if (!isClaimed()) {
        lastReplyStatus = MEZULLA_STATUS_OK;
        return;
    }

    // Payload: [cmd:1][owner_id:rest] — sender must prove they're the owner
    const char *senderId = (const char *)&mp.decoded.payload.bytes[1];
    size_t senderIdLen = mp.decoded.payload.size - 1;

    if (senderIdLen == 0 || strncmp(senderId, devicestate.mezulla_owner_id, senderIdLen) != 0) {
        LOG_WARN("[MEZULLA] release: rejected, reason=not_owner");
        lastReplyStatus = MEZULLA_STATUS_NOT_OWNER;
        return;
    }

    clearOwnership();
    lastReplyStatus = MEZULLA_STATUS_OK;
}

void MezullaOwnershipModule::handleQuery(const meshtastic_MeshPacket &mp)
{
    lastReplyStatus = MEZULLA_STATUS_OK;
}

meshtastic_MeshPacket *MezullaOwnershipModule::allocReply()
{
    auto reply = allocDataPacket();
    if (!reply)
        return nullptr;

    // Reply: [status:1][owner_id:rest] (owner_id empty if unclaimed)
    reply->decoded.payload.bytes[0] = lastReplyStatus;
    size_t ownerLen = strlen(devicestate.mezulla_owner_id);
    if (ownerLen > 0) {
        memcpy(&reply->decoded.payload.bytes[1], devicestate.mezulla_owner_id, ownerLen);
    }
    reply->decoded.payload.size = 1 + ownerLen;

    return reply;
}
