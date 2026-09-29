## Single-board build: Waveshare ESP32-S3-POE-ETH-8DI-8RO

`pio run -e esp32-s3-waveshare-8di8ro`, log: `pio device monitor -e esp32-s3-waveshare-8di8ro`
(this is the default environment in this fork, so `-e` can be left out).

Step-by-step setup from an empty board: **`SETUP.md`**.

One board, PoE-powered, bridging a Nuki lock over BLE and taking lock actions
from a UniFi Protect Alarm Manager webhook. No Wi-Fi, no MQTT broker needed.
Optionally the Nuki lock (Ultra, Go, 5th gen, 4th gen, 3.0 Pro) connects over
its own Wi-Fi to a tiny MQTT server built into this firmware (Nuki's official
MQTT API): near-instant lock actions and real-time lock state, with BLE as the
fallback. See "Nuki lock MQTT" below.

### What the build flag changes (`NUKI_HUB_WAVESHARE_8DI8RO`)
- Network hardware is pinned to the on-board W5500 (custom LAN: CS 16, IRQ 12,
  SCK 15, MISO 14, MOSI 13, reset 39) on **every** boot, after a >= 1 ms hardware
  reset pulse. `-DWAVESHARE_ETH_IRQ=-1` switches the W5500 to 10 ms polling. Web UI changes and the
  bootloop reset cannot switch it to Wi-Fi. Wi-Fi fallback is disabled: if the
  W5500 fails the ESP reboots and retries. The `NukiHub` access point never opens.
- Wi-Fi is compiled out (`-DNUKI_HUB_NO_WIFI`): the Wi-Fi driver is not linked
  and can't start, so the antenna serves BLE only. Wi-Fi/BT software coexistence
  is off (`sdkconfig.defaults.waveshare-8di8ro`). The web UI has no Wi-Fi pages,
  RSSI field or "Also reset WiFi settings" box. Saves about 285 KiB flash and
  14 KiB internal RAM. The flag is refused in builds without this board's flag.
- Nuki Hub's own MQTT is compiled out (`-DNUKI_HUB_NO_EXTERNAL_MQTT`): no MQTT
  client, no `nukihub/` topics, no Home Assistant discovery, no github.com ping,
  no "MQTT Configuration" pages or "MQTT Connected" row. The only MQTT on the
  board is the lock's session on the built-in server. What is left of the
  upstream publishing code (NukiNetworkLock builds its JSON, then `publish()`
  returns at once) is inert. The web serial log still works. The boot log says
  `External MQTT client compiled out (no broker connection)` instead of
  `MQTT Broker: <host>:<port>`.
- No HTTPS server (`NUKI_HUB_HTTPS_SERVER` unset): Protect needs plain HTTP on
  port 80, and a certificate would turn port 80 into a redirect. The OTA
  download task isn't built either (updates are USB-only).
- `-DNUKI_HUB_EMBEDDED_LOCK_MQTT`: the built-in MQTT server for the Nuki lock
  (off until enabled on the web page), see "Nuki lock MQTT" below.
- Sizes: no external MQTT saves ~159 KiB flash (Home Assistant discovery alone
  was ~97 KiB, espMqttClient ~10 KiB), no HTTPS server ~11 KiB; the lock server
  adds ~18 KiB. firmware.bin: 1,608,000 -> 1,451,536 bytes (-153 KiB) against the
  build before these changes.
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
  for that relay's pulse time (web page, "Pulse ms" per relay, default 3 s), e.g. wired across an
  intercom's door-open button: relay **COM + NO** in parallel with the button's
  two contacts. That only works if the button is a plain dry contact; 2-wire bus
  intercoms need their own interface. Relay rules skip the lock checks (BLE,
  busy) and Nuki Lock Access Control; replay, cooldown and all auth still apply.
  The log shows `Protect webhook: relay1 -> pulsed`. Before wiring a door, test
  that no relay clicks during a few PoE power cycles and reflashes (the power-up
  state of the TCA9554 driver is not yet proven, see `FINDINGS.md`).
  The pulse must be 100..30000 ms. "Relays available to rules" (1-8, web page)
  limits which `relayN` a rule may use; higher ones are refused when saving.
  With "Relay state outputs" on (below), only relays with the role *Webhook
  pulse* are available to rules instead. If opening the relay fails, the
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
  |                                | `lockmqtt` task (built-in MQTT server, 5 KiB stack, prio 3) |
  |                                | `relayout` task (relay state outputs, 4 KiB, prio 2; only once switched on) |

  A webhook handler only queues the action; the nuki task on core 0 performs
  it over BLE. With the lock's MQTT session up, the webhook handler instead
  publishes lockAction to the lock directly (no queue, no BLE). The W5500
  driver's RX task is created by ESP-IDF without an affinity and may run on
  either core.
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
5. There is no MQTT broker to configure. Optional but recommended for speed:
   let the lock connect to the built-in server ("Nuki lock MQTT" below).
   Without it, state changes not made by Nuki Hub (keypad, manual turns) are
   only seen at the next lock-state poll (default 30 min).

### Nuki lock MQTT (built-in server)
The lock joins your Wi-Fi and connects to this board's Ethernet IP, port 1883,
using Nuki's official MQTT API. Nuki Hub runs upstream's "hybrid mode" against
that session in-process: the lock's state, door sensor, lockActionEvent and
commandResponse arrive within milliseconds, and lock actions are published to
the lock instead of going over BLE. BLE stays: pairing, the extra information
upstream reads over BLE after each state change, and every action while the
lock isn't connected.

- **One client only: the lock.** Nuki Hub is not a network client, and nothing
  else can subscribe or publish, so no one on the network can send lockAction.
  A client must log in with the user name + password set on the web page (and
  in the Nuki app) and, if set, the expected client ID (the lock uses
  `Nuki_<Nuki ID in hex>`, e.g. `Nuki_2BB28570`; seen in a mosquitto log of a
  4th-gen lock, so check the page's "Lock connected" line for the Ultra's).
  Anything else gets a CONNACK refusal and is closed. A socket that hasn't
  logged in within 5 s is closed; at most two wait at a time and they never
  push out the lock's session.
- **Takeover:** a new login with the right credentials replaces the running
  session (as a broker does for the same client ID), so a half-open old
  connection can't lock the lock out after it roamed or rebooted. TCP
  keepalive (60 s idle, 3 probes 10 s apart) notices a lock that vanished in
  about 90 s; the MQTT keepalive (1.5 x the lock's 300 s) is the backstop.
- **Protocol:** MQTT 3.1.1 only what the lock uses: CONNECT/CONNACK, PUBLISH
  QoS 0/1/2 both ways (PUBACK, PUBREC/PUBREL/PUBCOMP), SUBSCRIBE/UNSUBSCRIBE
  (only to learn whether the lock listens to `nuki/<ID>/lockAction`), PINGREQ,
  DISCONNECT. No retained store, no wills, no bridging, clean sessions only.
  Messages larger than 320 bytes (e.g. Home Assistant discovery, if left on in
  the app) are acknowledged and dropped. lockAction is sent with QoS 1 (the lock acts ~180 ms after
  receipt instead of waiting for QoS 2's PUBREL; `-DLOCK_MQTT_ACTION_QOS=2` for
  Nuki's documented QoS 2).
- **Routing:** an action goes over MQTT only while the lock is connected, has
  published `connected=true`, subscribes to lockAction ("Allow locking" on in
  the app) and the action is one the MQTT API takes (1-6: unlock, lock,
  unlatch, lock 'n' go, lock 'n' go + unlatch, full lock). Fob actions and
  everything else use BLE. Enabling the server switches on Nuki Hub's hybrid
  settings (hybrid mode, "send actions through official MQTT", "retry over BLE
  if failed"); if hybrid mode was off at boot, MQTT actions start after a reboot.
- **Fallback:** if the lock doesn't confirm an MQTT action within 2 s
  (lockActionEvent or commandResponse 0), it is sent over BLE, within the
  webhook's action deadline. Exception: if the lock acknowledged the PUBLISH
  (PUBREC) of a send-once action (unlatch, lock 'n' go + unlatch, lock, lock 'n'
  go, full lock), it has the command and may be running it, so it isn't sent
  again: `Lock MQTT: the lock received unlatch but didn't confirm it within 2 s;
  not re-sent over BLE (it may have run)`.
- **When the session ends** the lock is marked offline at once (as its will
  would do) and actions go over BLE: `Lock MQTT: lock disconnected (...),
  actions use BLE`.
- **Plain MQTT on 1883, no TLS:** Nuki's MQTT API has no encryption ("does not
  support encrypted connections because of memory constraints", MQTT API 1.6,
  2.3) and always uses port 1883, so TLS isn't possible from the lock's side.
  Keep the lock and the board on a trusted VLAN; the password only keeps other
  clients out.
- **Resources:** 18 KiB flash, 2 KiB static RAM (three connection buffers of
  320 bytes plus state), a 5 KiB task stack (the page shows how much is
  unused), 4 of lwIP's 24 sockets (listener, the lock, two waiting logins)
  next to httpd's. The server task waits in select() and runs on core 1 with
  the network task; BLE (core 0) is untouched.

Log lines to expect:
```
Lock MQTT: listening on port 1883 for the Nuki lock
Lock MQTT: lock connected (client 'Nuki_2BB28570', keepalive 300 s, from 192.0.2.50)
Lock MQTT: lock subscribed to nuki/2BB28570/lockAction (QoS 2): lock actions go over MQTT
Lock MQTT: lockAction 1 sent (QoS 1, id 1)
Lock MQTT: refused client 'x' from 192.0.2.99: bad user name or password
Lock MQTT: takeover, closing the previous session from 192.0.2.50
```
The web page shows whether the lock is connected, since when, the last
message, the last state and where actions go.

The key to the lock lives on this network-facing board: keep it on its own
VLAN, allow only the Protect console to reach port 80 (and the lock port 1883,
if it uses the built-in MQTT server), and turn on Duo/TOTP
for the web UI.

### Relay state outputs
Optional, off by default (web page **Protect Webhook & Relays → Relays**, tick
**Relay state outputs**). The eight relays then follow the lock, the door
sensor and the keypad, one role per relay, e.g. as dry contacts into an alarm
panel's zones. **Relay contacts are dry contacts only**: no mains, no bus
intercoms. Setup steps: `SETUP.md` 9c.

"Closed" = relay energized, COM-NO closed. **Invert** flips it.

| Role | Kind | Closed when |
|---|---|---|
| Off | – | never |
| Webhook pulse | pulse | a webhook rule `relayN` fires (the only role rules can use) |
| Secure | steady | locked **and** door closed (both known) |
| Door open | steady | door opened |
| Locked | steady | locked (not while locking) |
| Lock fault | steady | motor blocked; the last lock action failed mechanically (motor blocked, low motor voltage, clutch, motor power, incomplete, failure; stays until the next action); the last BLE command failed after all retries; or the state is stale |
| Battery low | steady | lock, keypad or door sensor battery critical |
| Door sensor fault | steady | door sensor state unknown, uncalibrated or tampered, or the activity log says "jammed" (until the door is next reported opened/closed); never while calibrating |
| Keypad wrong code | pulse | keypad log entry "invalid code" / "not authorized" |
| Keypad valid entry | pulse | valid keypad code or fingerprint (MQTT `lockActionEvent` with a Code-ID, else the activity log; counted once) |
| Unlocked | steady | unlocked, unlocked (lock 'n' go) or unlatched |
| Night mode | steady | night mode active |

**Defaults** (only for relays without a stored role; *Reset relays to
defaults* restores them): 1 Webhook pulse, 2 Secure, 3 Door open, 4 Locked,
5 Lock fault **inverted**, 6 Battery low **inverted**, 7 Door sensor fault
**inverted**, 8 Keypad wrong code. The fault roles are inverted so that closed
means OK: a reboot, a power cut or a cut wire reads as a fault (supervised, like
a tamper loop). Relays with a role other than Webhook pulse can't be used by
rules; saving refuses a rule or a role change that would do that, and the
webhook refuses such a press (`500 error`).

**Fail-safe:** all relays stay open after boot until the first lock state
(MQTT or BLE), inverted ones too, then take their roles. **Every BLE error
and every beacon loss reboots the board** (upstream behaviour; "BLE EVENTS" on
the info page), so the relays drop for the boot time plus the first state each
time. The TCA9554 has no reset line: after a crash mid-pulse a relay stays as
it was until `setup()` opens it.

**Sources:** while the lock's MQTT session (built-in server) is live, its
`state`, `doorsensorState`, battery topics and `lockActionEvent` are used as
they arrive; the BLE key turner state fills in the rest (failed-action status,
night mode) and is the only source without MQTT. For each value the newer of
the two wins. **Stale** (Lock fault): neither a live MQTT session that was heard
from within "Skip only if the lock was heard from within" (default 330 s) nor a
successful BLE state read within 2 × the lock state poll interval + 60 s
(default 3660 s). Wrong keypad codes don't change the lock state and aren't
published over MQTT; they are only in the lock's activity log, read over BLE
after a BLE state read that finds the lock locked or unlocked (needs a valid
PIN and "Publish auth data"; only the newest "authlog max entries" per read).

| Role | With the lock on MQTT | Without |
|---|---|---|
| Secure, Door open, Locked, Unlocked, Battery low, Door sensor fault (state) | < 1 s | next lock state poll (default 30 min) |
| Lock fault: failed action, Night mode | ~1-3 s after the next state change (hybrid mode reads BLE then) | next poll |
| Lock fault: stale, BLE command failed | ≤ 1 s after the condition | same |
| Keypad valid entry | < 1 s | next activity-log read |
| Keypad wrong code, Door sensor jammed | next activity-log read (after the next lock state change or poll, + 5 s) | same |

The newest Nuki Keypad (NFC, Apple Home Key) may not be detected by Nuki Hub
("Has keypad: No"); the keypad roles don't depend on it. Keypad log entries
count whatever their source byte (code, fingerprint, and presumably NFC);
Apple Home Key entries are not keypad entries. Not verified on hardware yet.

Log lines: `Relay outputs: started`, `Relay outputs: first lock state known,
relays follow their roles`, `Relay outputs: relay5 (Lock fault, inverted)
closed`, `Relay outputs: relay8 (Keypad wrong code (pulse)) pulsed`. The info
page lists each relay's role and state; the settings page also shows the
inputs, the poll interval and keypad event counts.

Settings are in the `forkcfg` blob (format v4) like the rest of the page, not
in Nuki Hub's export/import. A board with settings from the previous firmware
(v3) starts with relay state outputs off and every relay's pulse set to the old
"Relay pulse". Flashing an older firmware back makes it refuse the v4 settings
(webhook off until saved again on its page).
