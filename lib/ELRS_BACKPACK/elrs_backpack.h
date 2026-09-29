#pragma once

#include <stdint.h>
#include <esp_wifi_types.h>

#include "config.h"
#include "laptimer.h"

// How often (ms) we re-check whether ESP-NOW needs to be (re)initialized,
// e.g. because the binding phrase changed or the WiFi mode changed.
#define ELRS_BACKPACK_CHECK_INTERVAL_MS 1000

// Minimum spacing (ms) between individual MSP frames sent for a single lap
// time update, so the goggles/backpack aren't hit with a burst of ESP-NOW
// packets all at once.
#define MSP_FRAME_STAGGER_MS 20

// 4 lap time writes + draw.
#define MSP_FRAME_QUEUE_CAPACITY 5

class ElrsBackpack {
   public:
    void init(Config *config, LapTimer *lapTimer);
    void update(uint32_t currentTimeMs);

    // Called from the ESP-NOW receive callback (must be lightweight).
    // Validates the packet and, if it is a valid MSP_SET_VTX_CONFIG packet
    // from the bound peer, stores the channel index for processing in update().
    void handleEspNowPacket(const uint8_t *mac, const uint8_t *data, int len);

    // Queues MSP_DISPLAYPORT frames to show the last three lap times and their
    // total (milliseconds) on a bound HDZero goggle's OSD via the ELRS Backpack.
    // Frames are sent one at a time, staggered by MSP_FRAME_STAGGER_MS, from update().
    void sendLapTime(uint32_t currentTimeMs, uint32_t lapTimeMs[3]);
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

    // Frames awaiting transmission, drained one at a time (staggered) by
    // processFrameQueue() so building a lap time update never blocks update().
    struct PendingMspFrame {
        uint16_t function;
        uint8_t payload[20];
        uint8_t payloadSize;
    };
    PendingMspFrame frameQueue[MSP_FRAME_QUEUE_CAPACITY];
    uint8_t queueHead = 0;
    uint8_t queueCount = 0;
    uint32_t lastFrameSentMs = 0;

    // Lap times currently shown on the OSD. A new lap sets pendingLapTimeValid
    // and is drawn once any in-flight WRITE/DRAW sequence has fully completed,
    // so a burst is never truncated.
    bool pendingLapTimeValid = false;
    uint32_t osdLapTimeMs[3] = {0, 0, 0};

    // The burst for the latest lap is repeated a few times (see
    // osdLapRepeatDelaysMs) to recover from dropped ESP-NOW packets. The
    // padded writes are idempotent, so repeating them is harmless.
    uint32_t lapBurstStartMs = 0;
    uint8_t lapRepeatsSent = UINT8_MAX;  // No lap drawn yet, nothing to repeat.

    void deriveUidFromPhrase(const char *phrase, uint8_t *uidOut);
    bool setupEspNow();
    void applyChannelIndex(uint8_t index);
    bool addPeer();
    void sendMspFrame(uint16_t function, const uint8_t *payload, uint16_t payloadSize);
    void enqueueFrame(uint16_t function, const uint8_t *payload, uint8_t payloadSize);
    void startLapTimeBurst(uint32_t currentTimeMs, uint32_t lapTimeMs[3]);
    void processFrameQueue(uint32_t currentTimeMs);
};
