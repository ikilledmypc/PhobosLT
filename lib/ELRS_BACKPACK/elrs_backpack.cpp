#include "elrs_backpack.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <mbedtls/md5.h>

#include "debug.h"

// MSPv2 command used by the ExpressLRS Backpack/VTX Administrator to set the
// VTX band/channel. Payload byte 0 is a zero-based index (0-47) into the
// standard 48 channel table (6 bands x 8 channels: A, B, E, F, R, L).
#define MSP_SET_VTX_CONFIG 89
#define MSP_VTX_CHANNEL_COUNT 48

// MSP DisplayPort function used to draw text on a bound HDZero goggle's OSD
// via the ELRS Backpack. Sub-command is payload byte 0.
#define MSP_DISPLAYPORT 182
#define MSP_DISPLAYPORT_SUBCMD_HEARTBEAT 0
#define MSP_DISPLAYPORT_SUBCMD_RELEASE 1
#define MSP_DISPLAYPORT_SUBCMD_CLEAR 2
#define MSP_DISPLAYPORT_SUBCMD_WRITE_STRING 3
#define MSP_DISPLAYPORT_SUBCMD_DRAW_SCREEN 4

// How often (ms) to repeat the last MSP DisplayPort frame to keep the OSD
// "session" open on the goggles and recover from a dropped ESP-NOW packet.
// Betaflight-style DisplayPort receivers (including the HDZero VRX) revert
// to normal video and stop accepting write/draw commands if nothing is
// received for a short timeout.
#define OSD_RESEND_INTERVAL_MS 250

// Default OSD position for the lap time text (row/col in character cells),
// used only if Config does not provide a value (should not normally happen).
#define LAPTIME_OSD_ATTR 0

// Standard 48 channel frequency table, matching the frequency table used by
// the ExpressLRS Backpack RX5808 module, so that channel indices received
// over ESP-NOW line up with the frequencies we tune to.
static const uint16_t elrsVtxFrequencyTable[MSP_VTX_CHANNEL_COUNT] = {
    5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725,  // A
    5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866,  // B
    5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945,  // E
    5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880,  // F
    5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917,  // R
    5333, 5373, 5413, 5453, 5493, 5533, 5573, 5613   // L
};

// Single instance bridge, since the esp_now C API only accepts free function
// callbacks, not member functions.
static ElrsBackpack *instance = nullptr;

static uint8_t crc8_dvb_s2(uint8_t crc, uint8_t a) {
    crc ^= a;
    for (uint8_t i = 0; i < 8; i++) {
        if (crc & 0x80) {
            crc = (crc << 1) ^ 0xD5;
        } else {
            crc <<= 1;
        }
    }
    return crc;
}

static void onEspNowDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
    if (instance != nullptr) {
        instance->handleEspNowPacket(mac, data, len);
    }
}

void ElrsBackpack::init(Config *config, LapTimer *lap) {
    conf = config;
    lapTimer = lap;
    instance = this;
}

void ElrsBackpack::deriveUidFromPhrase(const char *phrase, uint8_t *uidOut) {
    // Reproduces ExpressLRS' BindPhraseConfigurable::SetBindPhrase() algorithm,
    // so that a matching binding phrase on the radio/backpack derives the same
    // UID (used as the soft MAC address for ESP-NOW binding).
    static const uint8_t BIND_KEY[] = "-DMY_BINDING_PHRASE=\"";
    size_t phraseLen = strlen(phrase);

    uint8_t md5Result[16] = {0};

    if (phraseLen > 0) {
        mbedtls_md5_context md5;
        mbedtls_md5_init(&md5);
        mbedtls_md5_starts_ret(&md5);
        mbedtls_md5_update_ret(&md5, BIND_KEY, sizeof(BIND_KEY) - 1);
        mbedtls_md5_update_ret(&md5, (const uint8_t *)phrase, phraseLen);
        mbedtls_md5_update_ret(&md5, &BIND_KEY[sizeof(BIND_KEY) - 2], 1);
        mbedtls_md5_finish_ret(&md5, md5Result);
        mbedtls_md5_free(&md5);
    }

    memcpy(uidOut, md5Result, 6);
    // MAC address can only be soft-set with unicast, so first byte must be even.
    uidOut[0] &= ~0x01;
}

bool ElrsBackpack::setupEspNow() {
    wifi_mode_t mode = WiFi.getMode();
    if (mode != WIFI_AP && mode != WIFI_STA && mode != WIFI_AP_STA) {
        // WiFi has not been brought up yet (still WIFI_OFF), try again later.
        return false;
    }

    wifi_interface_t iface = (mode == WIFI_STA) ? WIFI_IF_STA : WIFI_IF_AP;
    espNowIface = iface;

    if (esp_wifi_set_mac(iface, uid) != ESP_OK) {
        DEBUG("ElrsBackpack: failed to set soft MAC address\n");
        return false;
    }

    if (esp_now_init() != ESP_OK) {
        DEBUG("ElrsBackpack: esp_now_init failed\n");
        return false;
    }

    esp_now_register_recv_cb(onEspNowDataRecv);

    DEBUG("ElrsBackpack: ESP-NOW active, UID = %02x:%02x:%02x:%02x:%02x:%02x, wifi mode = %d, channel = %d\n",
          uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], (int)mode, (int)WiFi.channel());

    peerAdded = false;

    return true;
}

bool ElrsBackpack::addPeer() {
    if (esp_now_is_peer_exist(uid)) {
        return true;
    }

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, uid, sizeof(uid));
    peerInfo.channel = 0;  // Use current WiFi channel.
    peerInfo.ifidx = espNowIface;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        DEBUG("ElrsBackpack: failed to add ESP-NOW peer\n");
        return false;
    }

    return true;
}

void ElrsBackpack::update(uint32_t currentTimeMs) {
    if (conf == nullptr) return;

    char *phrase = conf->getBindingPhrase();

    if ((currentTimeMs - lastCheckTimeMs) > ELRS_BACKPACK_CHECK_INTERVAL_MS) {
        lastCheckTimeMs = currentTimeMs;

        bool phraseChanged = strncmp(phrase, activeBindingPhrase, sizeof(activeBindingPhrase)) != 0;

        if (phrase[0] == 0) {
            // Binding disabled.
            espNowActive = false;
            strlcpy(activeBindingPhrase, phrase, sizeof(activeBindingPhrase));
        } else if (!espNowActive || phraseChanged) {
            deriveUidFromPhrase(phrase, uid);
            espNowActive = setupEspNow();
            strlcpy(activeBindingPhrase, phrase, sizeof(activeBindingPhrase));
        }
    }

    if (pendingChannelValid) {
        pendingChannelValid = false;
        applyChannelIndex(pendingChannelIndex);
    }

    if (lapTimer != nullptr && lapTimer->isLapAvailableForBackpack()) {
        uint32_t lapTimeMs[3];
        lapTimer->getLapTimeForBackpack(lapTimeMs);
        if (espNowActive && conf != nullptr && conf->getOsdEnabled()) {
            sendLapTime(currentTimeMs, lapTimeMs);
        }
    }

    if (espNowActive && conf != nullptr && conf->getOsdEnabled() &&
        (currentTimeMs - lastOsdHeartbeatMs) > OSD_RESEND_INTERVAL_MS) {
        lastOsdHeartbeatMs = currentTimeMs;
        resendLastLapFrame(currentTimeMs);
    }

    processFrameQueue(currentTimeMs);
}

void ElrsBackpack::applyChannelIndex(uint8_t index) {
    if (index >= MSP_VTX_CHANNEL_COUNT) return;
    if (conf == nullptr) return;

    uint16_t frequency = elrsVtxFrequencyTable[index];
    DEBUG("ElrsBackpack: applying VTX channel index %u -> %u MHz\n", index, frequency);
    conf->setFrequency(frequency);
}

void ElrsBackpack::handleEspNowPacket(const uint8_t *mac, const uint8_t *data, int len) {
    if (!espNowActive) return;

    if (memcmp(mac, uid, sizeof(uid)) != 0) {
        // Not from our bound peer.
        return;
    }

    // Minimum MSPv2 frame: '$','X',type,flags,funcLo,funcHi,sizeLo,sizeHi,crc = 9 bytes
    if (len < 9) return;
    if (data[0] != '$' || data[1] != 'X') return;
    if (data[2] != '<' && data[2] != '>') return;

    uint16_t function = data[4] | (data[5] << 8);
    uint16_t payloadSize = data[6] | (data[7] << 8);

    if ((size_t)len != (size_t)(8 + payloadSize + 1)) return;

    uint8_t crc = 0;
    for (int i = 3; i < 8 + payloadSize; i++) {
        crc = crc8_dvb_s2(crc, data[i]);
    }
    if (crc != data[8 + payloadSize]) {
        DEBUG("ElrsBackpack: MSP CRC mismatch\n");
        return;
    }

    if (function != MSP_SET_VTX_CONFIG) return;
    if (payloadSize < 1) return;

    uint8_t channelIndex = data[8];
    if (channelIndex >= MSP_VTX_CHANNEL_COUNT) return;

    pendingChannelIndex = channelIndex;
    pendingChannelValid = true;
}

void ElrsBackpack::sendMspFrame(uint16_t function, const uint8_t *payload, uint16_t payloadSize) {
    if (!espNowActive) return;

    if (!peerAdded) {
        peerAdded = addPeer();
        if (!peerAdded) return;
    }

    // MSPv2 frame: '$','X','<',flags,funcLo,funcHi,sizeLo,sizeHi,payload...,crc
    uint8_t frame[8 + 64 + 1];
    if ((size_t)payloadSize > sizeof(frame) - 9) return;

    frame[0] = '$';
    frame[1] = 'X';
    frame[2] = '<';
    frame[3] = 0;  // flags
    frame[4] = function & 0xFF;
    frame[5] = (function >> 8) & 0xFF;
    frame[6] = payloadSize & 0xFF;
    frame[7] = (payloadSize >> 8) & 0xFF;

    if (payloadSize > 0 && payload != nullptr) {
        memcpy(&frame[8], payload, payloadSize);
    }

    uint8_t crc = 0;
    for (int i = 3; i < 8 + payloadSize; i++) {
        crc = crc8_dvb_s2(crc, frame[i]);
    }
    frame[8 + payloadSize] = crc;

    esp_err_t result = esp_now_send(uid, frame, 8 + payloadSize + 1);
    if (result != ESP_OK) {
        DEBUG("ElrsBackpack: esp_now_send failed for function %u, err=%d\n", function, (int)result);
    }
}

void ElrsBackpack::enqueueFrame(uint16_t function, const uint8_t *payload, uint8_t payloadSize) {
    if (queueCount >= MSP_FRAME_QUEUE_CAPACITY || payloadSize > sizeof(PendingMspFrame::payload)) return;

    PendingMspFrame &frame = frameQueue[(queueHead + queueCount) % MSP_FRAME_QUEUE_CAPACITY];
    frame.function = function;
    frame.payloadSize = payloadSize;
    memcpy(frame.payload, payload, payloadSize);
    queueCount++;
}

void ElrsBackpack::processFrameQueue(uint32_t currentTimeMs) {
    if (queueCount == 0) return;
    if ((currentTimeMs - lastFrameSentMs) < MSP_FRAME_STAGGER_MS) return;

    lastFrameSentMs = currentTimeMs;
    PendingMspFrame &frame = frameQueue[queueHead];
    sendMspFrame(frame.function, frame.payload, frame.payloadSize);
    queueHead = (queueHead + 1) % MSP_FRAME_QUEUE_CAPACITY;
    queueCount--;
}

void ElrsBackpack::resendLastLapFrame(uint32_t currentTimeMs) {
    if (!hasLastLapTimes) {
        // No lap drawn yet, send a heartbeat to keep the OSD session alive.
        uint8_t heartbeatPayload[1] = {MSP_DISPLAYPORT_SUBCMD_HEARTBEAT};
        sendMspFrame(MSP_DISPLAYPORT, heartbeatPayload, sizeof(heartbeatPayload));
        return;
    }
    sendLapTime(currentTimeMs, lastLapTimeMs);
}

void ElrsBackpack::sendLapTime(uint32_t currentTimeMs, uint32_t lapTimeMs[3]) {
    if (!espNowActive) return;
    if (conf == nullptr || !conf->getOsdEnabled()) return;

    uint8_t row = conf->getOsdRow();
    uint8_t col = conf->getOsdCol();

    memcpy(lastLapTimeMs, lapTimeMs, sizeof(lastLapTimeMs));
    hasLastLapTimes = true;

    // Replace any not-yet-sent frames from a previous update with this one.
    queueHead = 0;
    queueCount = 0;
    // Send the first queued frame immediately rather than waiting a full stagger interval.
    lastFrameSentMs = currentTimeMs - MSP_FRAME_STAGGER_MS;

    // Clear the display region.
    uint8_t clearPayload[1] = {MSP_DISPLAYPORT_SUBCMD_CLEAR};
    enqueueFrame(MSP_DISPLAYPORT, clearPayload, sizeof(clearPayload));

    for (uint32_t i = 0; i < 4; i++) {
        char text[16];
        uint32_t totalMs = 0;

        if (i > 2) {
            for (uint_fast32_t j = 0; j < 3; j++) {
                totalMs += lapTimeMs[j];
            }
        } else {
            totalMs = lapTimeMs[i];
        }

        uint32_t minutes = totalMs / 60000;
        uint32_t seconds = (totalMs / 1000) % 60;
        uint32_t millisPart = totalMs % 1000;

        if (minutes > 0) {
            snprintf(text, sizeof(text), "%lu:%02lu.%03lu", (unsigned long)minutes,
                    (unsigned long)seconds, (unsigned long)millisPart);
        } else {
            snprintf(text, sizeof(text), "%lu.%03lu", (unsigned long)seconds, (unsigned long)millisPart);
        }

        // Write the lap time string.
        size_t textLen = strlen(text);
        uint8_t writePayload[4 + 16] = {0};
        writePayload[0] = MSP_DISPLAYPORT_SUBCMD_WRITE_STRING;
        writePayload[1] = row + i;
        writePayload[2] = col;
        writePayload[3] = LAPTIME_OSD_ATTR;
        memcpy(&writePayload[4], text, textLen);
        enqueueFrame(MSP_DISPLAYPORT, writePayload, 4 + textLen);
    }

    // Commit/draw the screen.
    uint8_t drawPayload[1] = {MSP_DISPLAYPORT_SUBCMD_DRAW_SCREEN};
    enqueueFrame(MSP_DISPLAYPORT, drawPayload, sizeof(drawPayload));
}
