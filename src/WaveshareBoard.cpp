#ifdef NUKI_HUB_WAVESHARE_8DI8RO

#include "WaveshareBoard.h"
#include "PreferencesKeys.h"
#include <Wire.h>

namespace
{
    bool tcaWrite(uint8_t reg, uint8_t value)
    {
        Wire.beginTransmission(WaveshareBoard::TCA9554_ADDR);
        Wire.write(reg);
        Wire.write(value);
        return Wire.endTransmission() == 0;
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

    // W5500 hardware reset. An ESP32 reset alone does not reset the W5500.
    pinMode(ETH_RST, OUTPUT);
    digitalWrite(ETH_RST, LOW);
    delay(2);
    digitalWrite(ETH_RST, HIGH);
    delay(5);
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
    // Upstream reads an unset TX power as 0 and applies +3 dBm, while its web UI
    // shows 9 on S3, so re-saving "9" is a no-op. Make the default explicit.
    if(!preferences->isKey(preference_ble_tx_power))
    {
        preferences->putInt(preference_ble_tx_power, DEFAULT_BLE_TX_POWER);
    }
}

#endif
