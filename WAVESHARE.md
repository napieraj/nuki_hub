## Single-board build: Waveshare ESP32-S3-POE-ETH-8DI-8RO

`pio run -e esp32-s3-waveshare-8di8ro`

One board, PoE-powered, bridging a Nuki lock over BLE and taking lock actions
from a UniFi Protect Alarm Manager webhook. No Wi-Fi, no MQTT broker needed.

### What the build flag changes (`NUKI_HUB_WAVESHARE_8DI8RO`)
- Network hardware is pinned to the on-board W5500 (custom LAN: CS 16, IRQ 12,
  SCK 15, MISO 14, MOSI 13, reset 39) on **every** boot, after a >= 1 ms hardware
  reset pulse. `-DWAVESHARE_ETH_IRQ=-1` switches the W5500 to 10 ms polling. Web UI changes and the
  bootloop reset cannot switch it to Wi-Fi. Wi-Fi fallback is disabled: if the
  W5500 fails the ESP reboots and retries. The `NukiHub` access point never opens.
- The eight relays (TCA9554 @ 0x20, I2C SDA 42 / SCL 41) are forced off first thing in `setup()`.
- BLE TX power defaults to +9 dBm when unset (upstream applies +3 dBm while its UI shows 9).
- Logs and serial config import are on USB-C (`ARDUINO_USB_CDC_ON_BOOT=1`).
- The module is a WROOM-1U: use the SMA antenna, outside any metal cabinet.
- Thread layout:

  | Core 0 (radio)                 | Core 1 (network)                          |
  |--------------------------------|-------------------------------------------|
  | BT controller, NimBLE host     | lwIP tcpip task (`sdkconfig.defaults.waveshare-8di8ro`) |
  | `nuki` task (BLE to the lock)  | `ntw` task, httpd incl. `/protect` webhook |

  A webhook handler only queues the action; the nuki task on core 0 performs
  it over BLE. The W5500 driver's RX task is created by ESP-IDF without an
  affinity and may run on either core.
- Forced on every boot, whatever the web UI or a config import stored, so the
  webhook can't be switched off by accident: web server **on**, "Disable network
  if not connected" **off**, "Restart on disconnect" **off**. Changing them in the
  web UI has no effect after the next reboot.
- `NUKI_HUB_PROTECT_WEBHOOK` is on: configure `src/ProtectWebhookConfig.h`. KeyFob + Protect setup and bench test: see `PROTECT_SETUP.md`
  (copy the `.example`).

### First boot
1. Flash over USB-C. The board comes up on Ethernet (DHCP by default).
2. Open the web UI, set credentials, enable "Nuki Smartlock Ultra/Go/5th gen",
   enter the 6-digit PIN, allow Unlock in "Nuki Lock Access Control".
3. Put the lock in pairing mode. It pairs as app (Ultra has no bridge mode).
4. Do **not** enable HTTPS in Nuki Hub: Protect rejects self-signed certificates and
   does not follow the HTTP→HTTPS redirect.
5. Leave MQTT unconfigured if you don't use it; lock actions and the webhook
   do not need it. Without Hybrid mode, state changes not made by Nuki Hub
   (keypad, manual turns) are only seen at the next lock-state poll.

The key to the lock lives on this network-facing board: keep it on its own
VLAN, allow only the Protect console to reach port 80, and turn on Duo/TOTP
for the web UI.
