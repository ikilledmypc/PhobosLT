#pragma once

#include <stdint.h>
#include <esp_wifi_types.h>

#include "config.h"
#include "laptimer.h"

// How often (ms) we re-check whether ESP-NOW needs to be (re)initialized,
// e.g. because the binding phrase changed or the WiFi mode changed.
#define ELRS_BACKPACK_CHECK_INTERVAL_MS 1000

class ElrsBackpack {
   public:
    void init(Config *config, LapTimer *lapTimer);
    void update(uint32_t currentTimeMs);

    // Called from the ESP-NOW receive callback (must be lightweight).
    // Validates the packet and, if it is a valid MSP_SET_VTX_CONFIG packet
    // from the bound peer, stores the channel index for processing in update().
    void handleEspNowPacket(const uint8_t *mac, const uint8_t *data, int len);

    // Builds and sends MSP_DISPLAYPORT frames to show the given lap time
    // (milliseconds) on a bound HDZero goggle's OSD via the ELRS Backpack.
    void sendLapTime(uint32_t lapTimeMs[3]);

   private:
    Config *conf = nullptr;
    LapTimer *lapTimer = nullptr;

    bool espNowActive = false;
    bool peerAdded = false;
    uint8_t uid[6] = {0, 0, 0, 0, 0, 0};
    char activeBindingPhrase[33] = {0};
    uint32_t lastCheckTimeMs = 0;
    wifi_interface_t espNowIface = WIFI_IF_STA;

    // Set by the ESP-NOW callback, consumed by update(). A single slot is
    // enough since only the latest channel index matters.
    volatile bool pendingChannelValid = false;
    volatile uint8_t pendingChannelIndex = 0;

    uint32_t lastOsdHeartbeatMs = 0;

    void deriveUidFromPhrase(const char *phrase, uint8_t *uidOut);
    bool setupEspNow();
    void applyChannelIndex(uint8_t index);
    bool addPeer();
    void sendMspFrame(uint16_t function, const uint8_t *payload, uint16_t payloadSize);
    void sendOsdHeartbeat();
};
