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

void ElrsBackpack::init(Config *config) {
    conf = config;
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

    DEBUG("ElrsBackpack: ESP-NOW active, UID = %02x:%02x:%02x:%02x:%02x:%02x\n",
          uid[0], uid[1], uid[2], uid[3], uid[4], uid[5]);

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
