#pragma once

// Relay state outputs (Waveshare 8DI-8RO): the eight relays follow lock,
// door-sensor and keypad state, one role per relay (RelayOutputsLogic.h),
// configured on the "Protect Webhook & Relays" page (forkcfg blob v4).
// Off by default; with the master switch off nothing here runs and the relays
// only move for webhook relayN rules, as before.
//
// Sources: the lock's official MQTT session (LockMqttServer, real time) while
// it is live, the BLE key turner state otherwise (and for what MQTT lacks),
// the lock's activity log over BLE for keypad and door-sensor events.
//
// Threads: the hooks below run in the nuki task (BLE), the lockmqtt task and
// httpd. They only copy values under a mutex and wake one worker task
// ("relayout", created when the master switch is first on). The worker
// evaluates the roles and writes the relays (one masked I2C write for the
// steady relays, plus pulses), so the MQTT task never touches I2C and steady
// outputs have one writer. It also wakes every second for the stale rule.

#ifdef NUKI_HUB_WAVESHARE_8DI8RO

#include <Arduino.h>
#include <list>
#include "../lib/nuki_ble/src/NukiLockConstants.h"
#include "ForkSettingsLogic.h"

namespace WaveshareOutputs
{
    // End of ForkSettings::begin() (before the lock objects exist): restores
    // the log baseline, switches "Publish auth data" on if a log role needs
    // it, and starts the worker if the master switch is on.
    void begin(const ForkSettingsLogic::Settings& s);

    // ForkSettings applied new settings (holding its mutex: only notifies,
    // starts the worker when the master switch goes on).
    void onSettingsApplied(bool relayOutputs);

    // After a save on the page: if a log role is assigned with the master
    // on and "Publish auth data" is off, switch it on. True if it changed
    // (Nuki Hub reads it at boot: applies after a reboot).
    bool ensureAuthLog(const ForkSettingsLogic::Settings& s);

    // --- inputs (any task) ---
    void onKeyTurnerState(const NukiLock::KeyTurnerState& kts); // successful BLE read
    void onLogEntries(const std::list<NukiLock::LogEntry>& log);
    void onBleResult(bool success);                              // end of each NukiRetryHandler::retryComm
    void onLockMqtt(const char* subTopic, const char* value);    // "/state", "3"
    void onLockMqttSession(bool up);

    struct Status
    {
        bool running;           // worker started (master switch was on at some point)
        bool enabled;           // master switch (as the worker last read it)
        bool armed;             // a lock state is known since boot
        bool stale;
        bool bleCommError;
        bool doorJammed;
        bool mqttLive;
        const char* lockSource; // "MQTT", "BLE", "none"
        int16_t lockState;      // -1 unknown
        int16_t doorState;      // -1 unknown
        int64_t bleAgeMs;       // -1 never
        int64_t lastLogReadAgeMs; // -1 never
        bool keypadBatteryReported; // key turner state accessory bit 0
        uint8_t closed;         // relay output register (bit N-1 = relay N)
        uint8_t pulsing;
        bool relaysReady;
        uint32_t stackFreeBytes;
        uint32_t keypadWrong;   // events since boot
        uint32_t keypadValid;
    };
    void status(Status& out);

    // Info page: master switch, each relay's role and state.
    void printInfo(Print& out);
}

#endif
