#ifdef NUKI_HUB_WAVESHARE_8DI8RO

#ifndef NUKI_HUB_PROTECT_WEBHOOK
#error "Relay state outputs keep their settings on the Protect Webhook & Relays page (NUKI_HUB_PROTECT_WEBHOOK)"
#endif

#include "WaveshareOutputs.h"
#include "WaveshareBoard.h"
#include "RelayOutputsLogic.h"
#include "ForkSettings.h"
#include "PreferencesKeys.h"
#include "MqttTopics.h"
#include "EspMillis.h"
#include "Logger.h"
#include <Preferences.h>
#include "esp_attr.h"
#include "esp_system.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#ifdef NUKI_HUB_EMBEDDED_LOCK_MQTT
#include "LockMqttServer.h"
#endif

using namespace RelayOutputsLogic;

namespace
{
    constexpr uint32_t kTaskStack = 4096;
    constexpr size_t kMaxLog = 32; // entries evaluated per read (authlog max is usually 3-5)

    // --- inputs: written by the hooks, read by the worker. Guarded by inLock.
    SemaphoreHandle_t inLock = nullptr;
    MqttInputs mqtt = {};
    BleInputs ble = {};
    bool bleCommError = false;
    JamLatch jam = {};
    PositionHold position = {}; // last definite lock state (option 2)
    KeypadDedupe dedupe = {};
    LogTracker logTracker = {};
    uint8_t pendingWrong = 0;   // keypad events waiting for the worker
    uint8_t pendingValid = 0;
    uint32_t keypadWrongCount = 0;
    uint32_t keypadValidCount = 0;
    int64_t lastLogReadMs = 0;

    // --- worker state. cfg and the status fields are guarded by inLock too.
    TaskHandle_t task = nullptr;
    uint32_t cfgGeneration = 0;
    bool cfgValid = false;
    bool enabled = false;
    RelayConfig cfg[RELAYS] = {};
    int64_t mqttFreshMs = ForkSettingsLogic::LOCK_SILENCE_MS.def;
    int32_t pollIntervalS = 1800;
    bool armed = false;
    Inputs lastInputs = {};
    bool lastMqttLive = false;
    bool lastLockFromMqtt = false;
    bool appliedEnabled = false;  // the last write was with the master on
    uint8_t lastTarget = 0;

    // The highest activity-log index seen survives software resets (not
    // power-on), so a reboot doesn't replay the last entry.
    constexpr uint32_t LOG_RTC_MAGIC = 0x524c4f47; // "RLOG"
    RTC_NOINIT_ATTR uint32_t logRtcMagic;
    RTC_NOINIT_ATTR uint32_t logRtcIndex;

    struct Guard
    {
        bool ok;
        Guard() : ok(inLock != nullptr && xSemaphoreTake(inLock, portMAX_DELAY) == pdTRUE) {}
        ~Guard()
        {
            if(ok)
            {
                xSemaphoreGive(inLock);
            }
        }
    };

    void wake()
    {
        if(task != nullptr)
        {
            xTaskNotifyGive(task);
        }
    }

    bool coldBoot()
    {
        const esp_reset_reason_t r = esp_reset_reason();
        return r == ESP_RST_POWERON || r == ESP_RST_BROWNOUT || r == ESP_RST_EXT || r == ESP_RST_UNKNOWN;
    }

    bool needsAuthLog(const ForkSettingsLogic::Settings& s)
    {
        if(!s.relayOutputs)
        {
            return false;
        }
        for(size_t i = 0; i < RELAYS; i++)
        {
            if(usesActivityLog(ForkSettingsLogic::configuredRole(s, i)))
            {
                return true;
            }
        }
        return false;
    }

    int32_t readPollInterval()
    {
        Preferences p;
        int32_t v = 1800;
        if(p.begin("nukihub", true))
        {
            v = p.getInt(preference_query_interval_lockstate, 1800);
            p.end();
        }
        return v > 0 ? v : 1800;
    }

    // Worker only. The settings mutex is taken without holding inLock
    // (order: settings -> inLock -> relayLock, never the other way).
    void refreshConfig()
    {
        const uint32_t gen = ForkSettings::generation();
        if(cfgValid && gen == cfgGeneration)
        {
            return;
        }
        RelayConfig next[RELAYS];
        bool nextEnabled = false;
        int64_t nextFresh = 0;
        {
            ForkSettings::ReadLock lock(pdMS_TO_TICKS(200));
            if(!lock.locked())
            {
                return; // a webhook request or a save: next wake
            }
            const ForkSettingsLogic::Settings& s = lock.settings();
            nextEnabled = s.relayOutputs;
            nextFresh = s.lockSilenceMs;
            for(size_t i = 0; i < RELAYS; i++)
            {
                next[i] = { ForkSettingsLogic::effectiveRole(s, i), ForkSettingsLogic::effectiveInvert(s, i),
                            s.relays[i].pulseMs };
            }
        }
        const int32_t poll = readPollInterval();
        Guard g;
        memcpy(cfg, next, sizeof(cfg));
        enabled = nextEnabled;
        mqttFreshMs = nextFresh;
        pollIntervalS = poll;
        cfgGeneration = gen;
        cfgValid = true;
    }

    void logChanges(uint8_t before, uint8_t after, const RelayConfig* c)
    {
        for(int i = 0; i < RELAYS; i++)
        {
            const uint8_t bit = (uint8_t)(1u << i);
            if((before ^ after) & bit)
            {
                Log->printf("Relay outputs: relay%d (%s%s) %s\n", i + 1, roleName(c[i].role), c[i].invert ? ", inverted" : "",
                            (after & bit) ? "closed" : "open");
            }
        }
    }

    void evaluate()
    {
        refreshConfig();

        bool mqttLive = false;
        int64_t lastRxAgeMs = -1;
#ifdef NUKI_HUB_EMBEDDED_LOCK_MQTT
        {
            LockMqttLogic::LiveState live;
            int64_t stateAgeMs;
            LockMqttServer::liveState(live, stateAgeMs);
            mqttLive = live.sessionLive;
            lastRxAgeMs = live.lastRxAgeMs;
        }
#endif
        const int64_t now = espMillis();
        RelayConfig c[RELAYS];
        bool on;
        uint8_t wrong, valid;
        Inputs in;
        bool wasArmed;
        {
            Guard g;
            memcpy(c, cfg, sizeof(c));
            on = enabled;
            in = merge(mqtt, mqttLive, ble);
            in.stale = isStale(mqttLive, lastRxAgeMs, mqttFreshMs, ble.have, ble.have ? now - ble.ms : -1,
                               bleFreshMs(pollIntervalS));
            in.bleCommError = bleCommError;
            in.doorJammed = jam.latched;
            position.update(in.haveLock, in.lockState);
            in.havePosition = position.have;
            in.position = position.state;
            wasArmed = armed;
            armed = armed || in.haveLock;
            lastInputs = in;
            lastMqttLive = mqttLive;
            lastLockFromMqtt = mqttLive && mqtt.lockState.have && (!ble.have || mqtt.lockState.ms >= ble.ms);
            wrong = pendingWrong;
            valid = pendingValid;
            pendingWrong = 0;
            pendingValid = 0;
        }
        if(on && armed && !wasArmed)
        {
            Log->println("Relay outputs: first lock state known, relays follow their roles");
        }

        // Steady relays (and the rest level of pulse roles), one I2C write,
        // only if something differs. Master off: once, to open what the
        // outputs had closed; after that nothing is written.
        if(on || appliedEnabled)
        {
            const uint8_t target = targetLevels(c, RELAYS, on, in, armed);
            if(WaveshareBoard::setRelayStates(0xFF, target))
            {
                if(target != lastTarget || on != appliedEnabled)
                {
                    logChanges(lastTarget, target, c);
                    if(!on)
                    {
                        Log->println("Relay outputs: switched off, relays only move for webhook rules");
                    }
                }
                lastTarget = target;
                appliedEnabled = on;
            }
        }

        // Keypad pulses.
        if(on && (wrong > 0 || valid > 0))
        {
            for(int i = 0; i < RELAYS; i++)
            {
                const bool fire = (c[i].role == RelayRole::KeypadWrongCode && wrong > 0) ||
                                  (c[i].role == RelayRole::KeypadValidEntry && valid > 0);
                if(fire)
                {
                    const bool ok = WaveshareBoard::pulseRelay(i + 1, c[i].pulseMs, c[i].invert);
                    Log->printf("Relay outputs: relay%d (%s) %s\n", i + 1, roleName(c[i].role), ok ? "pulsed" : "pulse failed");
                }
            }
        }
    }

    void workerTask(void*)
    {
        for(;;)
        {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            evaluate();
        }
    }

    void startWorker()
    {
        if(task != nullptr || inLock == nullptr)
        {
            return;
        }
        {
            // Keypad events seen while the outputs were off never fire late.
            Guard g;
            pendingWrong = 0;
            pendingValid = 0;
        }
        if(xTaskCreatePinnedToCore(workerTask, "relayout", kTaskStack, nullptr, 2, &task, WaveshareBoard::NETWORK_CORE) != pdPASS)
        {
            task = nullptr;
            Log->println("Relay outputs: task not started (out of memory)");
            return;
        }
        Log->println("Relay outputs: started");
    }

    bool setMqttFlag(Field& f, const char* value, int64_t now)
    {
        f = { true, (uint8_t)(strcmp(value, "true") == 0 ? 1 : 0), now };
        return true;
    }
}

void WaveshareOutputs::begin(const ForkSettingsLogic::Settings& s)
{
    if(inLock == nullptr)
    {
        inLock = xSemaphoreCreateMutex();
    }
    if(inLock == nullptr)
    {
        Log->println("Relay outputs: out of memory");
        return;
    }
    {
        Guard g;
        if(!coldBoot() && logRtcMagic == LOG_RTC_MAGIC)
        {
            logTracker = { true, logRtcIndex };
        }
        else
        {
            logRtcMagic = 0;
            logTracker = {};
        }
    }
    // Before NukiWrapper reads it (setup order): the log roles need it.
    ensureAuthLog(s);
    if(s.relayOutputs)
    {
        startWorker();
    }
}

void WaveshareOutputs::onSettingsApplied(bool relayOutputs)
{
    if(relayOutputs)
    {
        startWorker();
    }
    wake();
}

bool WaveshareOutputs::ensureAuthLog(const ForkSettingsLogic::Settings& s)
{
    if(!needsAuthLog(s))
    {
        return false;
    }
    Preferences p;
    bool changed = false;
    if(p.begin("nukihub", false))
    {
        if(!p.getBool(preference_publish_authdata, false))
        {
            p.putBool(preference_publish_authdata, true);
            changed = true;
            Log->println("Relay outputs: switched on 'Publish auth data' (the log roles need the lock's activity log)");
        }
        p.end();
    }
    return changed;
}

void WaveshareOutputs::onKeyTurnerState(const NukiLock::KeyTurnerState& kts)
{
    const int64_t now = espMillis();
    {
        Guard g;
        if(!g.ok)
        {
            return;
        }
        const uint8_t door = (uint8_t)kts.doorSensorState;
        if(ble.have && door != ble.doorState)
        {
            jam.onDoorStateChange(door);
        }
        ble.have = true;
        ble.ms = now;
        ble.lockState = (uint8_t)kts.lockState;
        ble.doorState = door;
        ble.criticalBatteryState = kts.criticalBatteryState;
        ble.accessoryBatteryState = kts.accessoryBatteryState;
        ble.nightModeActive = kts.nightModeActive;
        ble.completionStatus = (uint8_t)kts.lastLockActionCompletionStatus;
    }
    wake();
}

void WaveshareOutputs::onLogEntries(const std::list<NukiLock::LogEntry>& log)
{
    LogView views[kMaxLog];
    size_t n = 0;
    for(const NukiLock::LogEntry& e : log)
    {
        if(n == kMaxLog)
        {
            break;
        }
        views[n].index = e.index;
        views[n].type = (uint8_t)e.loggingType;
        memcpy(views[n].data, e.data, sizeof(views[n].data));
        n++;
    }
    const int64_t now = espMillis();
    bool fired = false;
    {
        Guard g;
        if(!g.ok)
        {
            return;
        }
        lastLogReadMs = now;
        processLog(logTracker, views, n, [&](LogEvent ev, uint16_t codeId)
        {
            jam.onLog(ev);
            if(ev == LogEvent::KeypadWrong)
            {
                pendingWrong = pendingWrong < 255 ? pendingWrong + 1 : 255;
                keypadWrongCount++;
                fired = true;
            }
            else if(ev == LogEvent::KeypadValid)
            {
                if(!dedupe.consumeLog(codeId, now))
                {
                    pendingValid = pendingValid < 255 ? pendingValid + 1 : 255;
                    keypadValidCount++;
                }
                fired = true;
            }
            else if(ev == LogEvent::DoorJammed || ev == LogEvent::DoorOpenedClosed)
            {
                fired = true;
            }
        });
        if(logTracker.haveBaseline)
        {
            logRtcIndex = logTracker.lastIndex;
            logRtcMagic = LOG_RTC_MAGIC;
        }
    }
    if(fired)
    {
        wake();
    }
}

void WaveshareOutputs::onBleResult(bool success)
{
    bool changed = false;
    {
        Guard g;
        if(!g.ok)
        {
            return;
        }
        changed = bleCommError == success; // error flag flips
        bleCommError = !success;
    }
    if(changed)
    {
        wake();
    }
}

void WaveshareOutputs::onLockMqtt(const char* sub, const char* value)
{
    if(sub == nullptr || value == nullptr)
    {
        return;
    }
    const int64_t now = espMillis();
    bool relevant = true;
    {
        Guard g;
        if(!g.ok)
        {
            return;
        }
        if(strcmp(sub, mqtt_topic_official_state) == 0)
        {
            mqtt.lockState = { true, (uint8_t)atoi(value), now };
        }
        else if(strcmp(sub, mqtt_topic_official_doorsensorState) == 0)
        {
            mqtt.doorState = { true, (uint8_t)atoi(value), now };
            jam.onDoorStateChange(mqtt.doorState.value); // only published on a change
        }
        else if(strcmp(sub, mqtt_topic_official_batteryCritical) == 0)
        {
            setMqttFlag(mqtt.lockCritical, value, now);
        }
        else if(strcmp(sub, mqtt_topic_official_keypadBatteryCritical) == 0)
        {
            setMqttFlag(mqtt.keypadCritical, value, now);
        }
        else if(strcmp(sub, mqtt_topic_official_doorsensorBatteryCritical) == 0)
        {
            setMqttFlag(mqtt.doorCritical, value, now);
        }
        else if(strcmp(sub, mqtt_topic_official_lockActionEvent) == 0)
        {
            LockActionEvent ev;
            relevant = parseLockActionEvent(value, ev) && isKeypadEntry(ev);
            if(relevant)
            {
                dedupe.noteMqtt((uint16_t)ev.codeId, now);
                pendingValid = pendingValid < 255 ? pendingValid + 1 : 255;
                keypadValidCount++;
            }
        }
        else
        {
            relevant = false;
        }
    }
    if(relevant)
    {
        wake();
    }
}

void WaveshareOutputs::onLockMqttSession(bool up)
{
    {
        Guard g;
        if(!g.ok)
        {
            return;
        }
        // A new or ended session: nothing it said before counts any more.
        mqtt = {};
    }
    (void)up;
    wake();
}

void WaveshareOutputs::status(Status& out)
{
    memset(&out, 0, sizeof(out));
    out.lockState = -1;
    out.doorState = -1;
    out.bleAgeMs = -1;
    out.lastLogReadAgeMs = -1;
    out.lockSource = "none";
    out.relaysReady = WaveshareBoard::relayStates(out.closed, out.pulsing);
    Guard g;
    if(!g.ok)
    {
        return;
    }
    const int64_t now = espMillis();
    out.running = task != nullptr;
    out.enabled = enabled && out.running;
    out.armed = armed;
    out.stale = lastInputs.stale;
    out.bleCommError = bleCommError;
    out.doorJammed = jam.latched;
    out.mqttLive = lastMqttLive;
    if(lastInputs.haveLock)
    {
        out.lockState = lastInputs.lockState;
        out.lockSource = lastLockFromMqtt ? "MQTT" : "BLE";
    }
    if(lastInputs.haveDoor)
    {
        out.doorState = lastInputs.doorState;
    }
    out.bleAgeMs = ble.have ? now - ble.ms : -1;
    out.lastLogReadAgeMs = lastLogReadMs > 0 ? now - lastLogReadMs : -1;
    out.keypadBatteryReported = ble.have && bleKeypadBatteryReported(ble.accessoryBatteryState);
    out.keypadWrong = keypadWrongCount;
    out.keypadValid = keypadValidCount;
    if(task != nullptr)
    {
        out.stackFreeBytes = (uint32_t)uxTaskGetStackHighWaterMark(task);
    }
}

void WaveshareOutputs::printInfo(Print& out)
{
    Status st;
    status(st);
    RelayConfig c[RELAYS];
    {
        Guard g;
        memcpy(c, cfg, sizeof(c));
    }
    out.print("\n------------ RELAYS ------------");
    out.printf("\nRelay state outputs: %s", st.enabled ? (st.armed ? "on" : "on, waiting for the first lock state") : "off");
    for(int i = 0; i < RELAYS; i++)
    {
        const uint8_t bit = (uint8_t)(1u << i);
        const RelayRole role = st.enabled ? c[i].role : RelayRole::WebhookPulse;
        out.printf("\nRelay %d: %s%s, %s%s", i + 1, roleName(role), st.enabled && c[i].invert ? " (inverted)" : "",
                   (st.closed & bit) ? "closed" : "open", (st.pulsing & bit) ? " (pulsing)" : "");
    }
    if(!st.relaysReady)
    {
        out.print("\nRelay driver (TCA9554) not initialized");
    }
}

#endif
