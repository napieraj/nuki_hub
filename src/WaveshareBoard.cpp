#ifdef NUKI_HUB_WAVESHARE_8DI8RO

#include "WaveshareBoard.h"
#include "PreferencesKeys.h"
#include <Wire.h>
#include <time.h>
#include "esp_attr.h"
#include "esp_system.h"
#include "EspMillis.h"

namespace
{
    bool tcaWrite(uint8_t reg, uint8_t value)
    {
        Wire.beginTransmission(WaveshareBoard::TCA9554_ADDR);
        Wire.write(reg);
        Wire.write(value);
        return Wire.endTransmission() == 0;
    }

    constexpr uint32_t TIME_SYNCED_MAGIC = 0x54494d45; // "TIME"
    constexpr time_t TIME_FLOOR = 1767225600;          // 2026-01-01T00:00:00Z
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
    // Relays: output register (0x01) low first, then all pins to outputs (0x03).
    // The TCA9554 powers up with outputs latched high and pins as inputs, and it
    // has no reset line, so this order matters. Never swap it.
    Wire.begin(I2C_SDA, I2C_SCL, 100000);
    tcaWrite(0x01, 0x00);
    tcaWrite(0x03, 0x00);
    Wire.end();

    initBleLog();

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

#endif
    // Upstream reads an unset TX power as 0 and applies +3 dBm, while its web UI
    // shows 9 on S3, so re-saving "9" is a no-op. Make the default explicit.
    if(!preferences->isKey(preference_ble_tx_power))
    {
        preferences->putInt(preference_ble_tx_power, DEFAULT_BLE_TX_POWER);
    }
}

#endif
