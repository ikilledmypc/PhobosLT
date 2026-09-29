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
#define MSP_DISPLAYPORT_SUBCMD_WRITE_STRING 3
#define MSP_DISPLAYPORT_SUBCMD_DRAW_SCREEN 4

// OSD character attribute used for the lap time text.
#define LAPTIME_OSD_ATTR 0

// Every lap time string is space-padded to this many characters, so a shorter
// time fully overwrites a longer one and no CLEAR command is needed. Fits the
// longest expected string, "MM:SS.mmm".
#define LAPTIME_OSD_WIDTH 9

// An OSD burst is one WRITE per line (3 lap times + their total) followed by a DRAW.
#define OSD_LINE_COUNT 4
#define OSD_BURST_FRAMES (OSD_LINE_COUNT + 1)

// Delays (ms after a lap's first burst started) at which that burst is sent
// again, so a dropped ESP-NOW packet is recovered on the goggles. The padded
// writes are idempotent, so repeating them is harmless.
static const uint32_t osdLapRepeatDelaysMs[] = {300, 1000};
#define OSD_LAP_REPEAT_COUNT (sizeof(osdLapRepeatDelaysMs) / sizeof(osdLapRepeatDelaysMs[0]))

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

// CRC8-DVB-S2 over an MSPv2 frame, from the flags byte up to the end of the payload.
static uint8_t mspCrc(const uint8_t *frame, uint16_t payloadSize) {
    uint8_t crc = 0;
    for (int i = 3; i < 8 + payloadSize; i++) {
        crc = crc8_dvb_s2(crc, frame[i]);
    }
    return crc;
}

// Formats a time as "S.mmm" or "M:SS.mmm", space-padded to LAPTIME_OSD_WIDTH.
static void formatLapTime(uint32_t ms, char *text, size_t textSize) {
    char timeText[16];
    uint32_t minutes = ms / 60000;
    uint32_t seconds = (ms / 1000) % 60;
    uint32_t millisPart = ms % 1000;

    if (minutes > 0) {
        snprintf(timeText, sizeof(timeText), "%lu:%02lu.%03lu", (unsigned long)minutes,
                 (unsigned long)seconds, (unsigned long)millisPart);
    } else {
        snprintf(timeText, sizeof(timeText), "%lu.%03lu", (unsigned long)seconds, (unsigned long)millisPart);
    }
    snprintf(text, textSize, "%-*s", LAPTIME_OSD_WIDTH, timeText);
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

    if (esp_wifi_set_mac(iface, uid) != ESP_OK) {
        DEBUG("ElrsBackpack: failed to set soft MAC address\n");
        return false;
    }

    if (esp_now_init() != ESP_OK) {
        DEBUG("ElrsBackpack: esp_now_init failed\n");
        return false;
    }

    esp_now_register_recv_cb(onEspNowDataRecv);

    if (!addPeer(iface)) {
        return false;
    }

    DEBUG("ElrsBackpack: ESP-NOW active, UID = %02x:%02x:%02x:%02x:%02x:%02x, wifi mode = %d, channel = %d\n",
          uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], (int)mode, (int)WiFi.channel());

    return true;
}

bool ElrsBackpack::addPeer(wifi_interface_t iface) {
    if (esp_now_is_peer_exist(uid)) {
        return true;
    }

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, uid, sizeof(uid));
    peerInfo.channel = 0;  // Use current WiFi channel.
    peerInfo.ifidx = iface;
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
        uint16_t frequency = elrsVtxFrequencyTable[pendingChannelIndex];
        DEBUG("ElrsBackpack: applying VTX channel index %u -> %u MHz\n", pendingChannelIndex, frequency);
        conf->setFrequency(frequency);
    }

    if (lapTimer != nullptr && lapTimer->isLapAvailableForBackpack()) {
        // Always consume the lap, even if the OSD is off, so a stale lap isn't drawn later.
        lapTimer->getLapTimeForBackpack(osdLapTimeMs);
        // Start a new burst right away; it rewrites every line, so an in-flight
        // burst for the previous lap can safely be abandoned.
        burstStep = 0;
        lapRepeatsSent = 0;
        lapBurstStartMs = currentTimeMs;
        lastFrameSentMs = currentTimeMs - MSP_FRAME_STAGGER_MS;
    }

    processOsd(currentTimeMs);
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

    if (mspCrc(data, payloadSize) != data[8 + payloadSize]) {
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
    frame[8 + payloadSize] = mspCrc(frame, payloadSize);

    esp_err_t result = esp_now_send(uid, frame, 8 + payloadSize + 1);
    if (result != ESP_OK) {
        DEBUG("ElrsBackpack: esp_now_send failed for function %u, err=%d\n", function, (int)result);
    }
}

void ElrsBackpack::processOsd(uint32_t currentTimeMs) {
    if (!espNowActive || !conf->getOsdEnabled()) {
        // Cancel any in-flight burst and remaining repeats.
        burstStep = OSD_BURST_FRAMES;
        lapRepeatsSent = OSD_LAP_REPEAT_COUNT;
        return;
    }

    if (burstStep >= OSD_BURST_FRAMES) {
        if (lapRepeatsSent >= OSD_LAP_REPEAT_COUNT ||
            (currentTimeMs - lapBurstStartMs) < osdLapRepeatDelaysMs[lapRepeatsSent]) return;
        lapRepeatsSent++;
        burstStep = 0;
    }

    if ((currentTimeMs - lastFrameSentMs) < MSP_FRAME_STAGGER_MS) return;
    lastFrameSentMs = currentTimeMs;

    sendOsdFrame(burstStep++);
}

void ElrsBackpack::sendOsdFrame(uint8_t step) {
    if (step >= OSD_LINE_COUNT) {
        // Commit/draw the screen.
        uint8_t drawPayload[1] = {MSP_DISPLAYPORT_SUBCMD_DRAW_SCREEN};
        sendMspFrame(MSP_DISPLAYPORT, drawPayload, sizeof(drawPayload));
        return;
    }

    // Lines 0-2 are the last three lap times, line 3 is their total.
    uint32_t ms = step < 3 ? osdLapTimeMs[step] : osdLapTimeMs[0] + osdLapTimeMs[1] + osdLapTimeMs[2];

    char text[16];
    formatLapTime(ms, text, sizeof(text));
    size_t textLen = strlen(text);

    uint8_t writePayload[4 + sizeof(text)];
    writePayload[0] = MSP_DISPLAYPORT_SUBCMD_WRITE_STRING;
    writePayload[1] = conf->getOsdRow() + step;
    writePayload[2] = conf->getOsdCol();
    writePayload[3] = LAPTIME_OSD_ATTR;
    memcpy(&writePayload[4], text, textLen);
    sendMspFrame(MSP_DISPLAYPORT, writePayload, 4 + textLen);
}
