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

void WaveshareBoard::relaysOff()
{
    // Output register (0x01) low first, then configure all pins as outputs (0x03).
    Wire.begin(I2C_SDA, I2C_SCL, 100000);
    tcaWrite(0x01, 0x00);
    tcaWrite(0x03, 0x00);
    Wire.end();
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

#endif
