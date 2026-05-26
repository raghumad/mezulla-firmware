#pragma once

#include "SinglePortModule.h"

// Mezulla sub-commands sent as the first byte of the PRIVATE_APP payload.
enum MezullaCommand : uint8_t {
    MEZULLA_CMD_CLAIM = 0x01,
    MEZULLA_CMD_QUERY = 0x02,
    MEZULLA_CMD_RELEASE = 0x03,
};

// Response status codes (first byte of reply payload).
enum MezullaStatus : uint8_t {
    MEZULLA_STATUS_OK = 0x00,
    MEZULLA_STATUS_TOKEN_MISMATCH = 0x01,
    MEZULLA_STATUS_ALREADY_CLAIMED = 0x02,
    MEZULLA_STATUS_NOT_OWNER = 0x03,
};

class MezullaOwnershipModule : public SinglePortModule
{
  public:
    MezullaOwnershipModule();

    bool isClaimed() const;
    const char *getOwnerId() const;
    const char *getPairingToken() const;

    void clearOwnership();
    void generatePairingToken();

  protected:
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    virtual meshtastic_MeshPacket *allocReply() override;

  private:
    void handleClaim(const meshtastic_MeshPacket &mp);
    void handleQuery(const meshtastic_MeshPacket &mp);
    void handleRelease(const meshtastic_MeshPacket &mp);

    MezullaStatus lastReplyStatus = MEZULLA_STATUS_OK;
};

extern MezullaOwnershipModule *mezullaOwnershipModule;
