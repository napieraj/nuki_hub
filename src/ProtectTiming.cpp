#ifdef NUKI_HUB_PROTECT_WEBHOOK

#include "ProtectTiming.h"
#include "esp_timer.h"
#include "EspMillis.h"
#include "Logger.h"
#include <cstring>
#include <freertos/FreeRTOS.h>

namespace
{
    enum class Transport : uint8_t { None, Ble, Mqtt };

    // Guarded by mux: written by the httpd, nuki and lock MQTT tasks.
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    int64_t pressTs = 0;        // webhook handed the action over; 0 = no press tracked
    uint8_t pressAction = 0xff;
    char pressName[16] = "";
    Transport transport = Transport::None;
    int64_t bleStartTs = 0;
    bool mqttAcked = false;
    bool mqttConfirmed = false;

    // A press older than this is forgotten (never confirmed, nothing logged later).
    constexpr int64_t kForgetMs = 30000;

    struct Snapshot
    {
        int64_t since; // ms since the webhook, -1 = no matching press
        char name[16];
    };

    // Caller holds mux.
    bool tracked(uint8_t action, int64_t now)
    {
        if(pressTs == 0)
        {
            return false;
        }
        if(now - pressTs > kForgetMs)
        {
            pressTs = 0;
            return false;
        }
        return action == 0xff || action == pressAction;
    }

    void clearLocked()
    {
        pressTs = 0;
        pressAction = 0xff;
        transport = Transport::None;
        bleStartTs = 0;
        mqttAcked = false;
        mqttConfirmed = false;
    }
}

void ProtectTiming::onWebhookAction(uint8_t lockAction, const char* name)
{
    const int64_t now = espMillis();
    taskENTER_CRITICAL(&mux);
    clearLocked();
    pressTs = now;
    pressAction = lockAction;
    strlcpy(pressName, name != nullptr ? name : "", sizeof(pressName));
    taskEXIT_CRITICAL(&mux);
}

void ProtectTiming::cancel()
{
    taskENTER_CRITICAL(&mux);
    clearLocked();
    taskEXIT_CRITICAL(&mux);
}

void ProtectTiming::onBleStart(uint8_t lockAction)
{
    const int64_t now = espMillis();
    Snapshot s = { -1, "" };
    taskENTER_CRITICAL(&mux);
    if(tracked(lockAction, now) && transport != Transport::Ble)
    {
        transport = Transport::Ble;
        bleStartTs = now;
        s.since = now - pressTs;
        memcpy(s.name, pressName, sizeof(s.name));
    }
    taskEXIT_CRITICAL(&mux);
    if(s.since >= 0)
    {
        Log->printf("Protect timing: lock action picked up %lld ms after the webhook\n", (long long)s.since);
    }
}

void ProtectTiming::onBleEnd(uint8_t lockAction, bool success)
{
    const int64_t now = espMillis();
    Snapshot s = { -1, "" };
    int64_t ble = 0;
    taskENTER_CRITICAL(&mux);
    if(tracked(lockAction, now) && transport == Transport::Ble)
    {
        s.since = now - pressTs;
        ble = now - bleStartTs;
        memcpy(s.name, pressName, sizeof(s.name));
        clearLocked();
    }
    taskEXIT_CRITICAL(&mux);
    if(s.since >= 0)
    {
        Log->printf("Protect timing: BLE lock action %s in %lld ms, %lld ms after the webhook\n",
                    success ? "done" : "failed", (long long)ble, (long long)s.since);
        if(success)
        {
            Log->printf("Protect timing: %s via BLE, lock confirmed %lld ms after the webhook\n", s.name, (long long)s.since);
        }
        else
        {
            Log->printf("Protect timing: %s via BLE failed, %lld ms after the webhook\n", s.name, (long long)s.since);
        }
    }
}

void ProtectTiming::onMqttSent(uint8_t lockAction)
{
    const int64_t now = espMillis();
    Snapshot s = { -1, "" };
    taskENTER_CRITICAL(&mux);
    if(tracked(lockAction, now) && transport == Transport::None)
    {
        transport = Transport::Mqtt;
        s.since = now - pressTs;
        memcpy(s.name, pressName, sizeof(s.name));
    }
    taskEXIT_CRITICAL(&mux);
    if(s.since >= 0)
    {
        Log->printf("Protect timing: %s via MQTT, sent %lld ms after the webhook\n", s.name, (long long)s.since);
    }
}

void ProtectTiming::onMqttAck(bool complete)
{
    const int64_t now = espMillis();
    Snapshot s = { -1, "" };
    taskENTER_CRITICAL(&mux);
    if(tracked(0xff, now) && transport == Transport::Mqtt && (complete || !mqttAcked))
    {
        mqttAcked = true;
        s.since = now - pressTs;
        memcpy(s.name, pressName, sizeof(s.name));
    }
    taskEXIT_CRITICAL(&mux);
    if(s.since >= 0)
    {
        Log->printf("Protect timing: %s via MQTT, lock %s %lld ms after the webhook\n", s.name,
                    complete ? "completed the delivery (PUBCOMP)" : "received it", (long long)s.since);
    }
}

void ProtectTiming::onMqttLockActionEvent(uint8_t lockAction)
{
    const int64_t now = espMillis();
    Snapshot s = { -1, "" };
    taskENTER_CRITICAL(&mux);
    if(tracked(lockAction, now) && transport == Transport::Mqtt && !mqttConfirmed)
    {
        mqttConfirmed = true;
        s.since = now - pressTs;
        memcpy(s.name, pressName, sizeof(s.name));
    }
    taskEXIT_CRITICAL(&mux);
    if(s.since >= 0)
    {
        Log->printf("Protect timing: %s via MQTT, lock confirmed (lockActionEvent) %lld ms after the webhook\n",
                    s.name, (long long)s.since);
    }
}

void ProtectTiming::onMqttCommandResponse(int code)
{
    const int64_t now = espMillis();
    Snapshot s = { -1, "" };
    taskENTER_CRITICAL(&mux);
    if(tracked(0xff, now) && transport == Transport::Mqtt)
    {
        s.since = now - pressTs;
        memcpy(s.name, pressName, sizeof(s.name));
        clearLocked();
    }
    taskEXIT_CRITICAL(&mux);
    if(s.since >= 0)
    {
        Log->printf("Protect timing: %s via MQTT, commandResponse %d %lld ms after the webhook\n",
                    s.name, code, (long long)s.since);
    }
}

void ProtectTiming::onMqttFallbackToBle(uint8_t lockAction)
{
    const int64_t now = espMillis();
    Snapshot s = { -1, "" };
    taskENTER_CRITICAL(&mux);
    if(tracked(lockAction, now) && transport == Transport::Mqtt)
    {
        transport = Transport::None; // onBleStart takes over
        s.since = now - pressTs;
        memcpy(s.name, pressName, sizeof(s.name));
    }
    taskEXIT_CRITICAL(&mux);
    if(s.since >= 0)
    {
        Log->printf("Protect timing: %s via MQTT not confirmed %lld ms after the webhook, retrying over BLE\n",
                    s.name, (long long)s.since);
    }
}

#endif
