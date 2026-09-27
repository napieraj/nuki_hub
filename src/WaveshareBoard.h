#pragma once

// Board support for the Waveshare ESP32-S3-POE-ETH-8DI-8RO as a single-board
// Nuki bridge: PoE/W5500 for the network, the S3's own radio for BLE only.
//
// Enabled with -DNUKI_HUB_WAVESHARE_8DI8RO. Effects:
//  - Network hardware is pinned to the on-board W5500 on every boot, whatever
//    the web UI or a bootloop reset stored. Wi-Fi is never started: no station,
//    no fallback, no "NukiHub" access point. If Ethernet fails, the ESP reboots.
//  - The eight relays (TCA9554 on I2C) are forced off before anything else runs,
//    and the W5500 gets a full hardware reset pulse (its RSTn is on GPIO39).
//  - BLE TX power defaults to +9 dBm when unset (upstream would silently use +3).
//  - Thread layout: BT controller, NimBLE host and the nuki task on core 0
//    (upstream defaults); network task, lwIP and httpd on core 1.
//
// Module is an ESP32-S3-WROOM-1U (external antenna via SMA on the case).

#ifdef NUKI_HUB_WAVESHARE_8DI8RO

#include <Arduino.h>
#include <Preferences.h>

// W5500 interrupt line. The ESP-IDF driver can stall RX up to 1 s when an
// edge is missed; build with -DWAVESHARE_ETH_IRQ=-1 to poll every 10 ms instead.
#ifndef WAVESHARE_ETH_IRQ
#define WAVESHARE_ETH_IRQ 12
#endif

namespace WaveshareBoard
{
    // W5500 (Waveshare pin table and demo code WS_ETH.h)
    constexpr int ETH_ADDR = 1;
    constexpr int ETH_CS   = 16;
    constexpr int ETH_IRQ  = WAVESHARE_ETH_IRQ;
    constexpr int ETH_RST  = 39;
    constexpr int ETH_SCK  = 15;
    constexpr int ETH_MISO = 14;
    constexpr int ETH_MOSI = 13;

    // TCA9554 relay driver
    constexpr int I2C_SDA = 42;
    constexpr int I2C_SCL = 41;
    constexpr uint8_t TCA9554_ADDR = 0x20;

    // Core for everything network-facing (httpd, lwIP, network task).
    constexpr int NETWORK_CORE = 1;

    constexpr int DEFAULT_BLE_TX_POWER = 9;

    // First thing in setup(): relays off, then a >= 1 ms W5500 reset pulse
    // (ESP-IDF's own pulse is 100 us; the W5500 datasheet asks for 500 us).
    void earlyInit();

    // Writes the custom-W5500 network preferences if they differ and returns
    // the hardware-detect value to use (11 = custom LAN).
    int pinNetworkHardware(Preferences* preferences);

    // Board defaults that only apply when the user has not set a value.
    void applyDefaults(Preferences* preferences);
}

#endif
