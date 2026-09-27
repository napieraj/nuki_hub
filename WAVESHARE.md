## Single-board build: Waveshare ESP32-S3-POE-ETH-8DI-8RO

`pio run -e esp32-s3-waveshare-8di8ro`

Step-by-step setup from an empty board: **`SETUP.md`**.

One board, PoE-powered, bridging a Nuki lock over BLE and taking lock actions
from a UniFi Protect Alarm Manager webhook. No Wi-Fi, no MQTT broker needed.

### What the build flag changes (`NUKI_HUB_WAVESHARE_8DI8RO`)
- Network hardware is pinned to the on-board W5500 (custom LAN: CS 16, IRQ 12,
  SCK 15, MISO 14, MOSI 13, reset 39) on **every** boot, after a >= 1 ms hardware
  reset pulse. `-DWAVESHARE_ETH_IRQ=-1` switches the W5500 to 10 ms polling. Web UI changes and the
  bootloop reset cannot switch it to Wi-Fi. Wi-Fi fallback is disabled: if the
  W5500 fails the ESP reboots and retries. The `NukiHub` access point never opens.
- The WS2812 (GPIO 38) flashes after each authenticated webhook call: short
  **green** = action queued, **red** = no rule matched (or stale) or refused by the
  ACL. Busy, cooldown and replays don't flash. If the colours are swapped, build
  with `-DWAVESHARE_LED_ORDER=LED_COLOR_ORDER_RGB`.
- Time: every NTP sync is written to the PCF85063 RTC (I2C 0x51, UTC). After a
  power cut the board restores the time from it if the RTC kept running, which
  needs a rechargeable ML1220 (not CR1220) in the holder. Otherwise it waits for NTP.
  Log: `NTP time synced` then `PCF85063 RTC updated` on every sync (at start, then
  every 12 h); after a power cut `PCF85063 RTC time: <date> UTC` and `Time restored
  from the PCF85063 RTC`, or a line saying why not (e.g. `lost its time`).
- **Relay rules:** a webhook rule with action `relay1`..`relay8` closes that relay
  for the relay pulse time (web page, default 3 s), e.g. wired across an
  intercom's door-open button: relay **COM + NO** in parallel with the button's
  two contacts. That only works if the button is a plain dry contact; 2-wire bus
  intercoms need their own interface. Relay rules skip the lock checks (BLE,
  busy) and Nuki Lock Access Control; replay, cooldown and all auth still apply.
  The log shows `Protect webhook: relay1 -> pulsed`. Before wiring a door, test
  that no relay clicks during a few PoE power cycles and reflashes (the power-up
  state of the TCA9554 driver is not yet proven, see `FINDINGS.md`).
  The pulse must be 100..30000 ms. "Relays available to rules" (1-8, web page)
  limits which `relayN` a rule may use; higher ones are refused when saving. If opening the relay fails, the
  board retries every 100 ms until it succeeds (`relayN: opening failed` in the log).
  The TCA9554 has no reset line: if the ESP resets mid-pulse (crash, watchdog,
  BLE reboot), the relay stays closed until the next boot clears it (boot time,
  measure it on the bench), and while the ESP is held in the bootloader (USB flashing) it stays as
  it was, so don't flash within a pulse.
- The eight relays (TCA9554 @ 0x20, I2C SDA 42 / SCL 41) are forced off first thing in `setup()`.
- GPIO 0 (BOOT), 38 (WS2812), 41/42 (I2C) and 46 (buzzer, strapping pin) can't be
  given a GPIO role. The eight digital inputs DI1-8 are GPIO 4-11 (dry contact to
  DGND, pulled up, active low). For a hardwired exit button, give its DI pin the
  role **Input: Unlock** in Nuki Hub's GPIO configuration. GPIO actions bypass
  Nuki Lock Access Control: anyone who can short that input to DGND can unlock,
  so keep the button and its wiring on the secure side of the door.
- Lock, LockNgo, FullLock, Unlatch, LockNgoUnlatch and fob actions are sent once
  (webhook, GPIO inputs, MQTT alike): after a timeout or other ambiguous result
  the lock may already be running them, so they are not retried (a lock retry
  into a moving bolt can raise a "motor jam" notification). Log:
  `Lock: result ambiguous (TIMEOUT), not retrying`. Unlock is still retried.
  Details: `PROTECT_SETUP.md` section 4.
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
  if not connected" **off**, "Restart on disconnect" **off**, and "Update Nuki Hub
  and Lock/Opener time using NTP" **on** (without it SNTP never starts and every
  press gets `503 no_time`). Changing them in the
  web UI has no effect after the next reboot.
- Every BLE disconnect error and every beacon-watchdog trigger still reboots the
  whole board (upstream behaviour). Each one is logged in RTC memory with UTC
  time and uptime; the info page ("System Information") shows the counts since
  power-on and the last 10 events. Use it to decide whether rate-limiting the
  reboots is worth it. The beacon timeout is "Restart if bluetooth beacons not received" in
  the Nuki configuration (default 60 s).
- `NUKI_HUB_PROTECT_WEBHOOK` is on: configure it in the web UI under **Protect
  Webhook & Relays** (stored in NVS namespace `forkcfg`, applied without a
  reboot, not part of Nuki Hub's export/import, erased by its factory reset).
  `src/ProtectWebhookConfig.h` is optional and only seeds an empty board.
  KeyFob + Protect setup, hardening options and bench test: see `PROTECT_SETUP.md`.
- The USB-C serial console always runs on this build (upstream only starts it
  with the access point open): `forkcfg unlock` switches off the fork page's
  settings lock and TOTP requirement, `forkcfg off` switches the webhook off.
  Upstream's serial config import is therefore available too; USB access already
  means full control (it can reflash the board).
- Settings lock option: a DI input (without a GPIO role) that must be active
  (closed to DGND) to save the fork settings page.

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
