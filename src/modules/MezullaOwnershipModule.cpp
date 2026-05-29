#include "MezullaOwnershipModule.h"
#include "MezullaScreenDump.h"
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
    // Mezulla deliberately uses NO_PIN BLE mode forever (no mode switch
    // on claim). Per BLE Core Spec Vol 3 Part H + AOSP source:
    //   - NO_PIN keeps characteristics flagged WITHOUT _ENC/_AUTHEN, so
    //     Android does not need to bond / does not need to run SMP.
    //   - The only spec-supported silent pair path for non-privileged
    //     Android apps is Passkey Entry (variant 0), and modern Android
    //     apps default to IO_CAP_NoInputNoOutput which forces the
    //     negotiation to Just Works (variant 3) regardless of what the
    //     peripheral advertises. variant=3 needs setPairingConfirmation,
    //     which requires the system-only BLUETOOTH_PRIVILEGED permission.
    //
    // Trade-off (documented in the Tern repo at
    // docs/architecture/mezulla-security.md and surfaced to the pilot
    // in Tern's pair-priming screen):
    //   - BLE link is UNENCRYPTED. Position/SOS broadcasts visible to a
    //     BLE sniffer within ~10 m. Same data is already on LoRa, so
    //     marginal exposure is small.
    //   - Authentication is the QR token (8-hex random, per-boot), not
    //     a BLE bond. Token is verified in handleClaim; afterwards the
    //     owner_id check gates every command. Attacker can connect but
    //     cannot make the board do anything.
    config.bluetooth.mode = meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN;
    config.power.wait_bluetooth_secs = 0;

    if (!isClaimed()) {
        generatePairingToken();
        LOG_INFO("[MEZULLA] ownership: unclaimed, NO_PIN, no BLE encryption, QR-token auth");
    } else {
        LOG_INFO("[MEZULLA] ownership: claimed, owner=%s, NO_PIN, no BLE encryption",
                 devicestate.mezulla_owner_id);
    }

#ifdef MEZULLA_TEST_BUILD
    config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
    LOG_INFO("[MEZULLA] TEST BUILD — radio disabled (region forced to UNSET)");
#endif
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
    config.bluetooth.mode = meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN;
    config.power.wait_bluetooth_secs = 0;
    nodeDB->saveToDisk(SEGMENT_DEVICESTATE);
    nodeDB->saveToDisk(SEGMENT_CONFIG);
    generatePairingToken();
    LOG_INFO("[MEZULLA] reset: ownership cleared, BLE set to NO_PIN");

    if (screen)
        screen->setFrames();
}

ProcessMessage MezullaOwnershipModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    LOG_INFO("[MEZULLA] handleReceived: payloadSize=%d from=0x%x to=0x%x portnum=%d",
             mp.decoded.payload.size, mp.from, mp.to, mp.decoded.portnum);

    if (mp.decoded.payload.size < 1)
        return ProcessMessage::CONTINUE;

    uint8_t cmd = mp.decoded.payload.bytes[0];
    LOG_INFO("[MEZULLA] command=0x%02x", cmd);

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

    // Stay in NO_PIN after claim (no BLE mode switch). The QR token's
    // job is done at this point — owner_id is persisted and from here
    // on every command is gated on owner_id matching. BLE link remains
    // unencrypted by design; see MezullaOwnershipModule constructor
    // KDoc for the trade-off rationale.
    bool saved = nodeDB->saveToDisk(SEGMENT_DEVICESTATE);
    LOG_INFO("[MEZULLA] claim: accepted, owner=%s, saved=%s", devicestate.mezulla_owner_id, saved ? "YES" : "NO");
    lastReplyStatus = MEZULLA_STATUS_OK;

    if (screen) {
        screen->setFrames();
        MezullaScreenDump::dumpToSerial();
    }
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
    LOG_INFO("[MEZULLA] query: isClaimed=%s", isClaimed() ? "true" : "false");
    MezullaScreenDump::dumpToSerial();
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
