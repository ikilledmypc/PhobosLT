#pragma once

#include "RX5808.h"
#include "buzzer.h"
#include "config.h"
#include "kalman.h"
#include "led.h"

typedef enum {
    STOPPED,
    WAITING,
    RUNNING
} laptimer_state_e;

#define LAPTIMER_LAP_HISTORY 10
#define LAPTIMER_RSSI_HISTORY 100

class LapTimer {
   public:
    void init(Config *config, RX5808 *rx5808, Buzzer *buzzer, Led *l);
    void start();
    void stop();
    void handleLapTimerUpdate(uint32_t currentTimeMs);
    uint8_t getRssi();
    uint32_t getLapTime();
    bool isLapAvailable();
    // Last three lap times, newest first. The holeshot and laps not flown yet are 0.
    void getLapTimeForBackpack(uint32_t lapTimes_out[3]);
    bool isLapAvailableForBackpack();

   private:
    laptimer_state_e state = STOPPED;
    RX5808 *rx;
    Config *conf;
    Buzzer *buz;
    Led *led;
    KalmanFilter filter;
    boolean lapCountWraparound;
    uint32_t raceStartTimeMs;
    uint32_t startTimeMs;
    uint8_t lapCount;
    uint32_t lapsFinished = 0;  // Including the holeshot, reset by stop().
    uint8_t rssiCount;
    uint32_t lapTimes[LAPTIMER_LAP_HISTORY];
    uint8_t rssi[LAPTIMER_RSSI_HISTORY];

    uint8_t rssiPeak;
    uint32_t rssiPeakTimeMs;

    bool lapAvailable = false;
    bool lapAvailableForBackpack = false;

    void lapPeakCapture();
    bool lapPeakCaptured();
    void lapPeakReset();

    void startLap();
    void finishLap();
};
