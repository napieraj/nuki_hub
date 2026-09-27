#ifdef NUKI_HUB_WAVESHARE_8DI8RO

#include "WaveshareBoard.h"
#include "PreferencesKeys.h"
#include <Wire.h>
#include <time.h>
#include <sys/time.h>
#include "esp_attr.h"
#include "esp_system.h"
#include "EspMillis.h"
#include "Logger.h"
#include "esp_timer.h"
#include "WaveshareRtcLogic.h"

// WS2812 colour order; build with -DWAVESHARE_LED_ORDER=LED_COLOR_ORDER_RGB if
// "green" shows red.
#ifndef WAVESHARE_LED_ORDER
#define WAVESHARE_LED_ORDER LED_COLOR_ORDER_GRB
#endif

namespace
{
    bool tcaWrite(uint8_t reg, uint8_t value)
    {
        Wire.beginTransmission(WaveshareBoard::TCA9554_ADDR);
        Wire.write(reg);
        Wire.write(value);
        return Wire.endTransmission() == 0;
    }

    bool tcaRead(uint8_t reg, uint8_t& value)
    {
        Wire.beginTransmission(WaveshareBoard::TCA9554_ADDR);
        Wire.write(reg);
        if(Wire.endTransmission(false) != 0 || Wire.requestFrom(WaveshareBoard::TCA9554_ADDR, (uint8_t)1) != 1)
        {
            return false;
        }
        value = Wire.read();
        return true;
    }

    // Relays: shadow of the TCA9554 output register. relayLock serializes the
    // read-modify-write plus its I2C write (httpd task and esp_timer task).
    uint8_t relayOutputs = 0;
    bool relaysReady = false; // output register read back 0, pins are outputs
    SemaphoreHandle_t relayLock = nullptr;
    esp_timer_handle_t relayOffTimers[WaveshareBoard::RELAY_COUNT] = {};
    uint32_t relayOffFailures[WaveshareBoard::RELAY_COUNT] = {};
    constexpr uint64_t RELAY_OFF_RETRY_US = 100 * 1000;

    // Output register (0x01) to 0, read back, and only then all pins to
    // outputs (0x03). The TCA9554 powers up with the output register at 0xFF
    // and all pins as inputs, and it has no reset line: enabling the outputs
    // while the output register still holds 0xFF (e.g. its write failed)
    // would close all eight relays. Never swap or skip a step.
    // After a warm reset (panic, watchdog, reboot) the pins are still outputs
    // with the last state, so a relay closed mid-pulse stays closed until here.
    bool initRelays()
    {
        uint8_t out = 0xff, cfg = 0xff;
        if(!tcaWrite(0x01, 0x00) || !tcaRead(0x01, out) || out != 0x00)
        {
            return false;
        }
        relayOutputs = 0;
        return tcaWrite(0x03, 0x00) && tcaRead(0x03, cfg) && cfg == 0x00;
    }

    bool setRelay(int relay, bool on, TickType_t wait)
    {
        if(relayLock == nullptr || xSemaphoreTake(relayLock, wait) != pdTRUE)
        {
            return false;
        }
        // Not ready (init failed so far): retry it here. It opens all relays,
        // which also clears one left closed by a warm reset mid-pulse.
        bool ok = relaysReady || (relaysReady = initRelays());
        if(ok)
        {
            const uint8_t bit = (uint8_t)(1u << (relay - 1));
            const uint8_t out = on ? (relayOutputs | bit) : (relayOutputs & ~bit);
            ok = tcaWrite(0x01, out);
            if(ok)
            {
                relayOutputs = out;
            }
        }
        xSemaphoreGive(relayLock);
        return ok;
    }

    // esp_timer callback. It must not block (the esp_timer task also runs
    // NimBLE's timers), so a failed write re-arms the timer instead of waiting:
    // a relay left closed would keep the door open, so retry until it opens.
    void relayOff(void* arg)
    {
        const int relay = (int)(intptr_t)arg;
        if(setRelay(relay, false, pdMS_TO_TICKS(20)))
        {
            relayOffFailures[relay - 1] = 0;
            return;
        }
        if(relayOffFailures[relay - 1]++ % 50 == 0)
        {
            log_e("relay%d: opening failed, retrying", relay);
        }
        esp_timer_start_once(relayOffTimers[relay - 1], RELAY_OFF_RETRY_US);
    }

    constexpr uint32_t TIME_SYNCED_MAGIC = 0x54494d45; // "TIME"
    constexpr time_t TIME_FLOOR = (time_t)WaveshareRtcLogic::TIME_FLOOR; // 2026-01-01T00:00:00Z
    RTC_NOINIT_ATTR uint32_t timeSyncedMagic;

    constexpr uint32_t BLE_LOG_MAGIC = 0x424c4532; // "BLE2" (layout version)
    constexpr size_t BLE_LOG_SIZE = 10;

    struct BleEventEntry
    {
        int64_t epoch;    // UTC seconds, 0 if the clock wasn't set
        int64_t uptimeMs; // espMillis() at the event
        uint8_t reason;   // WaveshareBoard::BleEvent
    };

    struct BleEventLog
    {
        uint32_t magic;
        uint32_t errors;      // since power-on
        uint32_t beaconLost;  // since power-on
        uint32_t stalled;     // since power-on
        uint32_t next;        // ring index of the next entry
        uint32_t stored;      // entries in the ring, <= BLE_LOG_SIZE
        BleEventEntry events[BLE_LOG_SIZE];
    };

    RTC_NOINIT_ATTR BleEventLog bleLog;
    portMUX_TYPE bleLogMux = portMUX_INITIALIZER_UNLOCKED;

    bool isColdBoot()
    {
        const esp_reset_reason_t reason = esp_reset_reason();
        return reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT ||
               reason == ESP_RST_EXT || reason == ESP_RST_UNKNOWN;
    }

    void initBleLog()
    {
        if(isColdBoot() || bleLog.magic != BLE_LOG_MAGIC ||
           bleLog.next >= BLE_LOG_SIZE || bleLog.stored > BLE_LOG_SIZE)
        {
            memset(&bleLog, 0, sizeof(bleLog));
            bleLog.magic = BLE_LOG_MAGIC;
        }
    }

    esp_timer_handle_t statusLedOffTimer = nullptr;

    void statusLedOff(void*)
    {
        rgbLedWriteOrdered(WaveshareBoard::STATUS_LED, WAVESHARE_LED_ORDER, 0, 0, 0);
    }

    // Only write when the stored value differs, to spare flash.
    void putIntIfDifferent(Preferences* p, const char* key, int value, bool& changed)
    {
        if(p->getInt(key, INT32_MIN) != value)
        {
            p->putInt(key, value);
            changed = true;
        }
    }
}

void WaveshareBoard::earlyInit()
{
    // Relays off first (register order: see initRelays). If that fails, the
    // pins stay inputs and the next relay write retries the init.
    // The bus stays up: relays and the RTC use it later, from several tasks
    // (Wire locks each transaction).
    Wire.begin(I2C_SDA, I2C_SCL, 100000);
    for(int i = 0; i < 3 && !(relaysReady = initRelays()); i++)
    {
        delay(5);
    }
    relayLock = xSemaphoreCreateMutex();

    initBleLog();

    // The WS2812 can latch random data at power-up.
    statusLedOff(nullptr);

    // W5500 hardware reset. An ESP32 reset alone does not reset the W5500.
    pinMode(ETH_RST, OUTPUT);
    digitalWrite(ETH_RST, LOW);
    delay(2);
    digitalWrite(ETH_RST, HIGH);
    delay(5);
}

void WaveshareBoard::markTimeSynced()
{
    timeSyncedMagic = TIME_SYNCED_MAGIC;
}

bool WaveshareBoard::timeSurvivedReset()
{
    // Power loss, brownout or the reset pin also reset the RTC timer, so time()
    // restarts near 1970; RTC_NOINIT memory is garbage after power-on.
    if(isColdBoot() || timeSyncedMagic != TIME_SYNCED_MAGIC || time(nullptr) < TIME_FLOOR)
    {
        timeSyncedMagic = 0;
        return false;
    }
    return true;
}

void WaveshareBoard::recordBleEvent(uint8_t reason)
{
    const time_t now = time(nullptr);
    BleEventEntry entry;
    entry.epoch = now >= TIME_FLOOR ? (int64_t)now : 0;
    entry.uptimeMs = espMillis();
    entry.reason = reason;

    taskENTER_CRITICAL(&bleLogMux);
    if(reason == BLE_EVENT_ERROR)
    {
        bleLog.errors++;
    }
    else if(reason == BLE_EVENT_BEACON_LOST)
    {
        bleLog.beaconLost++;
    }
    else if(reason == BLE_EVENT_STALLED)
    {
        bleLog.stalled++;
    }
    bleLog.events[bleLog.next] = entry;
    bleLog.next = (bleLog.next + 1) % BLE_LOG_SIZE;
    if(bleLog.stored < BLE_LOG_SIZE)
    {
        bleLog.stored++;
    }
    taskEXIT_CRITICAL(&bleLogMux);
}

void WaveshareBoard::printBleEvents(Print& out)
{
    BleEventLog snapshot;
    taskENTER_CRITICAL(&bleLogMux);
    memcpy(&snapshot, &bleLog, sizeof(snapshot));
    taskEXIT_CRITICAL(&bleLogMux);

    out.print("\n------------ BLE EVENTS (since power-on, each one rebooted the board) ------------");
    out.printf("\nBLE errors: %u", (unsigned)snapshot.errors);
    out.printf("\nBeacon lost: %u", (unsigned)snapshot.beaconLost);
    out.printf("\nNuki task stalled (webhook): %u", (unsigned)snapshot.stalled);
    out.printf("\nLast %u (newest first):", (unsigned)snapshot.stored);
    for(uint32_t i = 0; i < snapshot.stored; i++)
    {
        const BleEventEntry& e = snapshot.events[(snapshot.next + BLE_LOG_SIZE - 1 - i) % BLE_LOG_SIZE];
        char when[24] = "time not set";
        if(e.epoch > 0)
        {
            const time_t t = (time_t)e.epoch;
            struct tm tmUtc;
            gmtime_r(&t, &tmUtc);
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmUtc);
        }
        out.printf("\n  %s UTC | uptime %lld s | %s", when, (long long)(e.uptimeMs / 1000),
                   e.reason == BLE_EVENT_ERROR ? "BLE error" :
                   e.reason == BLE_EVENT_BEACON_LOST ? "beacon lost" :
                   e.reason == BLE_EVENT_STALLED ? "nuki task stalled" : "unknown");
    }
}

bool WaveshareBoard::pulseRelay(int relay, uint32_t pulseMs)
{
    if(relay < 1 || relay > RELAY_COUNT)
    {
        return false;
    }
    esp_timer_handle_t& timer = relayOffTimers[relay - 1];
    if(timer == nullptr)
    {
        esp_timer_create_args_t args = {};
        args.callback = relayOff;
        args.arg = (void*)(intptr_t)relay;
        args.name = "relayOff";
        if(esp_timer_create(&args, &timer) != ESP_OK)
        {
            timer = nullptr;
            return false;
        }
    }
    esp_timer_stop(timer); // not running is fine
    const bool closed = setRelay(relay, true, pdMS_TO_TICKS(500));
    // Arm the off timer even if the write reported an error: the TCA9554 may
    // have latched it anyway. INVALID_STATE: relayOff re-armed it meanwhile
    // (retrying), and that opens the relay too.
    const esp_err_t err = esp_timer_start_once(timer, (uint64_t)pulseMs * 1000);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        relayOff((void*)(intptr_t)relay);
        return false;
    }
    return closed;
}

void WaveshareBoard::flashStatusLed(bool ok)
{
    // Only called from the httpd task; the off timer runs in the esp_timer task.
    if(statusLedOffTimer == nullptr)
    {
        esp_timer_create_args_t args = {};
        args.callback = statusLedOff;
        args.name = "statusLedOff";
        if(esp_timer_create(&args, &statusLedOffTimer) != ESP_OK)
        {
            statusLedOffTimer = nullptr;
            return;
        }
    }
    esp_timer_stop(statusLedOffTimer); // not running is fine
    rgbLedWriteOrdered(STATUS_LED, WAVESHARE_LED_ORDER, ok ? 0 : 40, ok ? 40 : 0, 0);
    esp_timer_start_once(statusLedOffTimer, 400 * 1000);
}

void WaveshareBoard::storeTimeInRtc()
{
    // Called from the SNTP callback (lwIP tcpip thread); Wire locks each
    // transaction against the relay writes from other tasks.
    uint8_t t[7];
    if(!WaveshareRtcLogic::encode((int64_t)time(nullptr), t))
    {
        return;
    }
    // Decoding assumes 24 h mode and a running clock; clear 12_24 and STOP if
    // anything (e.g. a demo firmware) set them. CAP_SEL and the rest are kept.
    Wire.beginTransmission(PCF85063_ADDR);
    Wire.write(0x00);
    bool ok = Wire.endTransmission(false) == 0 && Wire.requestFrom(PCF85063_ADDR, (uint8_t)1) == 1;
    if(ok)
    {
        const uint8_t ctrl1 = Wire.read();
        if(ctrl1 & (WaveshareRtcLogic::CTRL1_STOP | WaveshareRtcLogic::CTRL1_12_24))
        {
            Wire.beginTransmission(PCF85063_ADDR);
            Wire.write(0x00);
            Wire.write((uint8_t)(ctrl1 & ~(WaveshareRtcLogic::CTRL1_STOP | WaveshareRtcLogic::CTRL1_12_24)));
            ok = Wire.endTransmission() == 0;
        }
    }
    if(ok)
    {
        Wire.beginTransmission(PCF85063_ADDR);
        Wire.write(0x04);                    // Seconds; writing it clears the OS flag
        Wire.write(t, sizeof(t));
        ok = Wire.endTransmission() == 0;
    }
    Log->println(ok ? "PCF85063 RTC updated" : "PCF85063 RTC write failed");
}

bool WaveshareBoard::restoreTimeFromRtc()
{
    // Control_1..Years (0x00..0x0A) in one read.
    uint8_t r[11] = {};
    Wire.beginTransmission(PCF85063_ADDR);
    Wire.write(0x00);
    bool ok = Wire.endTransmission(false) == 0 && Wire.requestFrom(PCF85063_ADDR, (uint8_t)sizeof(r)) == sizeof(r);
    for(size_t i = 0; ok && i < sizeof(r); i++)
    {
        r[i] = Wire.read();
    }
    if(!ok)
    {
        Log->println("PCF85063 RTC not readable, waiting for NTP");
        return false;
    }
    int64_t epoch = 0;
    switch(WaveshareRtcLogic::decode(r[0], &r[4], epoch))
    {
    case WaveshareRtcLogic::RtcStatus::Ok:
        break;
    case WaveshareRtcLogic::RtcStatus::OscillatorStopped:
        // Power loss without a charged backup cell (or never set): invalid.
        Log->println("PCF85063 RTC lost its time (no/flat backup cell?), waiting for NTP");
        return false;
    case WaveshareRtcLogic::RtcStatus::ClockStopped:
    case WaveshareRtcLogic::RtcStatus::TwelveHourMode:
        Log->printf("PCF85063 RTC not in use (Control_1 0x%02x), waiting for NTP\n", r[0]);
        return false;
    default:
        Log->println("PCF85063 RTC holds no plausible date, waiting for NTP");
        return false;
    }
    struct timeval tv = { (time_t)epoch, 0 };
    if(settimeofday(&tv, nullptr) != 0)
    {
        return false;
    }
    struct tm utc;
    const time_t now = (time_t)epoch;
    gmtime_r(&now, &utc);
    char when[24];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &utc);
    Log->printf("PCF85063 RTC time: %s UTC\n", when);
    return true;
}

int WaveshareBoard::pinNetworkHardware(Preferences* preferences)
{
    bool changed = false;
    putIntIfDifferent(preferences, preference_network_hardware, 11, changed);   // custom LAN
    putIntIfDifferent(preferences, preference_network_custom_phy, 1, changed);  // W5500
    putIntIfDifferent(preferences, preference_network_custom_addr, ETH_ADDR, changed);
    putIntIfDifferent(preferences, preference_network_custom_cs, ETH_CS, changed);
    putIntIfDifferent(preferences, preference_network_custom_irq, ETH_IRQ, changed);
    putIntIfDifferent(preferences, preference_network_custom_rst, ETH_RST, changed);
    putIntIfDifferent(preferences, preference_network_custom_sck, ETH_SCK, changed);
    putIntIfDifferent(preferences, preference_network_custom_miso, ETH_MISO, changed);
    putIntIfDifferent(preferences, preference_network_custom_mosi, ETH_MOSI, changed);
    if(changed)
    {
        preferences->putBool(preference_ntw_reconfigure, true);
    }
    return 11;
}

void WaveshareBoard::applyDefaults(Preferences* preferences)
{
    // Never pull firmware from upstream: no update checks, no MQTT-triggered
    // update, no pending URL-based OTA (a config import or the advanced page
    // could otherwise store one). Enforced on every boot.
    if(preferences->getBool(preference_check_updates, true))
    {
        preferences->putBool(preference_check_updates, false);
    }
    if(preferences->getBool(preference_update_from_mqtt, true))
    {
        preferences->putBool(preference_update_from_mqtt, false);
    }
    if(preferences->isKey(preference_ota_updater_url) && preferences->getString(preference_ota_updater_url, "").length() > 0)
    {
        preferences->putString(preference_ota_updater_url, "");
    }
    if(preferences->isKey(preference_ota_main_url) && preferences->getString(preference_ota_main_url, "").length() > 0)
    {
        preferences->putString(preference_ota_main_url, "");
    }

#ifdef NUKI_HUB_PROTECT_WEBHOOK
    // The webhook lives on the web server, and nothing may switch the network
    // (and so the webhook) off by accident. Enforced on every boot.
    if(!preferences->getBool(preference_webserver_enabled, true))
    {
        preferences->putBool(preference_webserver_enabled, true);
    }
    if(preferences->getBool(preference_disable_network_not_connected, false))
    {
        preferences->putBool(preference_disable_network_not_connected, false);
    }
    if(preferences->getBool(preference_restart_on_disconnect, false))
    {
        preferences->putBool(preference_restart_on_disconnect, false);
    }
    // The webhook refuses every press without a synced clock, and SNTP only
    // starts when "Update ... time using NTP" is on (upstream default: off).
    if(!preferences->getBool(preference_update_time, false))
    {
        preferences->putBool(preference_update_time, true);
    }

#endif
    // Upstream reads an unset TX power as 0 and applies +3 dBm, while its web UI
    // shows 9 on S3, so re-saving "9" is a no-op. Make the default explicit.
    if(!preferences->isKey(preference_ble_tx_power))
    {
        preferences->putInt(preference_ble_tx_power, DEFAULT_BLE_TX_POWER);
    }
}

#endif
