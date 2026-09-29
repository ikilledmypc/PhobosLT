#pragma once

#include <stdint.h>
#include <esp_wifi_types.h>

#include "config.h"
#include "laptimer.h"

// How often (ms) we re-check whether ESP-NOW needs to be (re)initialized,
// e.g. because the binding phrase changed.
#define ELRS_BACKPACK_CHECK_INTERVAL_MS 1000

// Minimum spacing (ms) between individual MSP frames sent for a single lap
// time update, so the goggles/backpack aren't hit with a burst of ESP-NOW
// packets all at once.
#define MSP_FRAME_STAGGER_MS 20

class ElrsBackpack {
   public:
    void init(Config *config, LapTimer *lapTimer);
    void update(uint32_t currentTimeMs);

    // Called from the ESP-NOW receive callback (must be lightweight).
    // Validates the packet and, if it is a valid MSP_SET_VTX_CONFIG packet
    // from the bound peer, stores the channel index for processing in update().
    void handleEspNowPacket(const uint8_t *mac, const uint8_t *data, int len);

   private:
    Config *conf = nullptr;
    LapTimer *lapTimer = nullptr;

    bool espNowActive = false;
    uint8_t uid[6] = {0, 0, 0, 0, 0, 0};
    char activeBindingPhrase[33] = {0};
    uint32_t lastCheckTimeMs = 0;

    // Set by the ESP-NOW callback, consumed by update(). A single slot is
    // enough since only the latest channel index matters.
    volatile bool pendingChannelValid = false;
    volatile uint8_t pendingChannelIndex = 0;

    // Lap times shown on the OSD (last three laps), sent to a bound HDZero
    // goggle via MSP_DISPLAYPORT as a burst of one WRITE per line plus a DRAW.
    // processOsd() sends one frame of the burst every MSP_FRAME_STAGGER_MS,
    // then repeats the whole burst a few times to recover from dropped packets.
    uint32_t osdLapTimeMs[3] = {0, 0, 0};
    uint8_t burstStep = UINT8_MAX;       // Next frame of the burst; past the end when idle.
    uint8_t lapRepeatsSent = UINT8_MAX;  // No lap drawn yet, nothing to repeat.
    uint32_t lapBurstStartMs = 0;
    uint32_t lastFrameSentMs = 0;

    void deriveUidFromPhrase(const char *phrase, uint8_t *uidOut);
    bool setupEspNow();
    bool addPeer(wifi_interface_t iface);
    void sendMspFrame(uint16_t function, const uint8_t *payload, uint16_t payloadSize);
    void processOsd(uint32_t currentTimeMs);
    void sendOsdFrame(uint8_t step);
};
