#pragma once

#include <stdint.h>

#include "config.h"

// How often (ms) we re-check whether ESP-NOW needs to be (re)initialized,
// e.g. because the binding phrase changed or the WiFi mode changed.
#define ELRS_BACKPACK_CHECK_INTERVAL_MS 1000

class ElrsBackpack {
   public:
    void init(Config *config);
    void update(uint32_t currentTimeMs);

    // Called from the ESP-NOW receive callback (must be lightweight).
    // Validates the packet and, if it is a valid MSP_SET_VTX_CONFIG packet
    // from the bound peer, stores the channel index for processing in update().
    void handleEspNowPacket(const uint8_t *mac, const uint8_t *data, int len);

   private:
    Config *conf = nullptr;

    bool espNowActive = false;
    uint8_t uid[6] = {0, 0, 0, 0, 0, 0};
    char activeBindingPhrase[33] = {0};
    uint32_t lastCheckTimeMs = 0;

    // Set by the ESP-NOW callback, consumed by update(). A single slot is
    // enough since only the latest channel index matters.
    volatile bool pendingChannelValid = false;
    volatile uint8_t pendingChannelIndex = 0;

    void deriveUidFromPhrase(const char *phrase, uint8_t *uidOut);
    bool setupEspNow();
    void applyChannelIndex(uint8_t index);
};
