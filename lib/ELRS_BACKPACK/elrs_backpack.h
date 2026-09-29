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
#define MSP_FRAME_STAGGER_MS 250

// clear + 4 lap time writes + draw.
#define MSP_FRAME_QUEUE_CAPACITY 6

class ElrsBackpack {
   public:
    void init(Config *config, LapTimer *lapTimer);
    void update(uint32_t currentTimeMs);

    // Called from the ESP-NOW receive callback (must be lightweight).
    // Validates the packet and, if it is a valid MSP_SET_VTX_CONFIG packet
    // from the bound peer, stores the channel index for processing in update().
    void handleEspNowPacket(const uint8_t *mac, const uint8_t *data, int len);

    // Queues MSP_DISPLAYPORT frames to show the given lap time (milliseconds)
    // on a bound HDZero goggle's OSD via the ELRS Backpack. Frames are sent
    // one at a time, staggered by MSP_FRAME_STAGGER_MS, from update().
    void sendLapTime(uint32_t currentTimeMs, uint32_t lapTimeMs[3], boolean clearBefore);
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

    // Last lap times sent by sendLapTime(), replayed on the resend interval
    // so a dropped ESP-NOW packet is recovered on the goggles.
    uint32_t lastLapTimeMs[3] = {0, 0, 0};
    bool hasLastLapTimes = false;

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

    // A lap time update requested while a burst is still draining, applied
    // once the in-flight CLEAR/WRITE/DRAW sequence has fully completed so
    // it is never truncated.
    bool pendingLapTimeValid = false;
    uint32_t pendingLapTimeMs[3] = {0, 0, 0};

    void deriveUidFromPhrase(const char *phrase, uint8_t *uidOut);
    bool setupEspNow();
    void applyChannelIndex(uint8_t index);
    bool addPeer();
    void sendMspFrame(uint16_t function, const uint8_t *payload, uint16_t payloadSize);
    void enqueueFrame(uint16_t function, const uint8_t *payload, uint8_t payloadSize);
    void startLapTimeBurst(uint32_t currentTimeMs, uint32_t lapTimeMs[3], boolean clearBefore);
    void processFrameQueue(uint32_t currentTimeMs);
    void resendLastLapFrame(uint32_t currentTimeMs);
};
