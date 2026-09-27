## Research notes: USL-FOB → Protect → Nuki Hub (Waveshare 8DI-8RO) → Nuki Ultra

Collected 2026-09-27. Confidence tags: **[verified]** = checked in vendor or
upstream source code; **[primary]** = vendor docs / maintainer statement;
**[reports]** = community reports; **[inference]**.

### 1. What Protect sends when a fob button is pressed
- **[verified]** Alarm Manager trigger for a fob press, from UniFi Protect
  7.2.105 `service.js` (public firmware `.deb`):
  `{key:"sensor_button_pressed", value:<buttonPressType>, device:<MAC>, button:<button>}`.
  `button` = `metadata.button.text` (default `function`); `value` =
  `metadata.buttonPressType.text` (default `press`).
- **[verified]** Gestures: `press`, `longPress`, `doublePress` (`SENSOR_BUTTON_EVENTS`).
  The Alarm Manager UI labels long press "Long Press (3s)" = the app's "Hold".
- **[verified]** Buttons: `arm` (top left), `night` (top right), `disarm` (bottom left),
  `panic` (bottom right), `left`, `right`. All gestures are enabled on adoption.
- **[verified]** One alarm = one fob + one button + one gesture: the condition is
  `{type:"is", source:"sensor_button_pressed", value:<gesture>}`, the scope is
  `"<MAC>:button=<id>"`, serialized as `sources:[{device, type:"include", buttons:[id]}]`.
- **[verified]** Envelope `{"alarm":{name,sources,conditions,triggers},"timestamp"}`;
  `eventId` and `timestamp` are injected per trigger at processing time
  (`Date.now()`); `eventPath`/`eventLocalLink` are only added for detections and
  `ring`, **not** for button presses.
- **[verified]** Global Alarm Manager mode (forced when any SuperLink device is
  adopted): UniFi OS calls back Protect at
  `/automationManager/external/actions/webhook`, which runs the **same**
  `buildHttpActionPayload` with the recorded trigger, so the body shape is the same.
- **[verified]** The public Integration API (`/v1/subscribe/events`, API key)
  emits `sensorButtonPressed` with `metadata.button` only. Its schema whitelist
  drops `buttonPressType`, so **no gesture** there. HA's fob event entity
  (PR #175630, in 2026.10) is built on it and has no gestures either.
- Still unconfirmed by a live capture: do it once during bring-up
  (`scripts/protect_webhook_test.py listen`).

### 2. How Protect delivers webhooks
- **[verified]** Up to 3 attempts, retried **only on HTTP 5xx or 408**, with 1 s then
  2 s backoff; 30 s timeout (configurable, max 300 s); **redirects are not followed**;
  POST sends `Content-Type: application/json`, `User-Agent: protect-alarm-manager`;
  auth options none / Basic / **Bearer**, plus custom headers; body capped at 4096 chars.
- **[reports]** Self-signed HTTPS fails (`UNABLE_TO_VERIFY_LEAF_SIGNATURE`), with no
  "ignore certificate" option. Digest auth is unsupported.
- **[reports]** No queue: if the board is offline the press is lost.
- **[reports]** "Ignore repeated actions" (cooldown) is off by default; when
  enabled it defaults to 600 s. Leave it off; the board has its own 10 s cooldown.
- **[reports]** An alarm not attached to an arm profile fires regardless of arm state.
- **[verified]** Global-mode delivery refuses localhost and link-local targets, so the board needs a routable LAN IP.

### 3. Fob and SuperLink
- **[reports]** Needs a SuperLink Gateway (USL-Gateway or USL-G2-Gateway-HA),
  UniFi OS ≥ 5.1.11, Protect ≥ 7.1.60, fob firmware ≥ 1.4.0 (1.5.1 had regressions
  with relay outputs, and the console offers a revert).
- **[reports]** "No Action" buttons still emit events and reach Alarm Manager.
  Known annoyance: a push notification on every press, even with no alarm.
- **[reports]** The fob sleeps. The first press after idle can lag seconds,
  sometimes needing a second press. Presses are "instant" once awake.
- **[reports / single RE project]** The radio is proprietary LoRa-based sub-GHz
  with per-device keys (Curve25519 handshake), authenticated encryption and an
  incrementing frame counter. No published audit. The weak link is the LAN
  webhook hop, not the radio.

### 4. Nuki Smart Lock Ultra over BLE
- **[primary + verified]** **No bridge pairing** on 5th gen / Ultra: its auth
  message has no ID-type field (Nuki BLE API 2.3.1). ESPHome_nuki_lock's "bridge"
  default is silently app pairing. Nuki staff: "5th gen locks do not support the
  Nuki bridge anymore".
- **[primary]** No state-change beacon for app authorizations: in BLE-only
  mode, changes made elsewhere (keypad, app, knob) are seen only at the next
  poll (nuki_hub default 1800 s). Each poll wakes the lock.
- **[primary]** Battery (Nuki help): 7–9 months without remote access. BLE-only
  is the lowest drain; firmware 5.9.4 caps charging at 80 % by default.
- **[verified]** nuki_hub reports a lock action as successful once the lock
  *accepts* it (an empty message code 0 compares equal to `Complete` 0 in
  `NukiBle.hpp`). If another message lands in that window, the command times
  out and the retry handler resends it. Harmless for unlock/lock, a second
  pull for unlatch, so **prefer "unlock"**.
- **[reports]** Some 5th-gen firmware stopped advertising until the lock was restarted
  (fixed in 5.7.11 / 5.8.11). Keep the lock firmware current.
- **[primary]** Lock actions carry no PIN; the 6-digit PIN is needed for pairing and
  admin commands. Clearing it from Nuki Hub after pairing *should* keep
  lock actions working. **[inference]**: test it on the bench.
- **[inference]** Turning off "Bluetooth pairing" (button) after pairing should not
  affect existing authorizations. The pairing shows as "NukiHub" in the app's users.

### 5. Waveshare ESP32-S3-POE-ETH-8DI-8RO
- **[primary]** ESP32-S3-**WROOM-1U**: an external 4 dBi antenna on SMA at the case edge. In a metal cabinet, route it outside.
- **[primary]** W5500 **RSTn is on GPIO39** (pin table and every Waveshare demo). Now used, with a
  ≥ 1 ms pulse at boot (ESP-IDF's own pulse is 100 µs).
- **[reports + code]** The W5500 IRQ path can stall RX up to 1 s on a missed edge.
  `-DWAVESHARE_ETH_IRQ=-1` switches to 10 ms polling; measure both.
- **[primary]** TCA9554 powers up with outputs latched high and pins as inputs, and has no reset
  line. The firmware clears the outputs before enabling them. Power-up relay state is
  unproven: don't wire relays to anything door-related without a PoE power-cycle test.
- **[primary]** DI inputs take a true dry contact between DIx and DGND (COM open).
  Use one for a door contact (pull-up, inverted, ~10 ms debounce).
- **[primary]** PoE is 802.3af via a daughterboard; plenty of headroom.
  The RTC holder takes a rechargeable **ML1220** only (not CR1220).
- **[verified]** Native USB only: logs and serial config import now use USB-C
  (`ARDUINO_USB_CDC_ON_BOOT=1`).
- **[reports]** No one has published BLE results on this exact board. The sibling
  Waveshare ESP32-S3-ETH worked fine as an ESPHome BLE proxy. Plan a soak test.

### 6. nuki_hub specifics
- **[verified]** Unset BLE TX power is applied as +3 dBm while the S3 UI shows 9,
  and re-saving 9 does nothing. The board build now stores 9 when unset.
- **[verified]** If you enable HTTPS in Nuki Hub, port 80 becomes a 301 redirect
  and `/protect` moves to 443 with a self-signed certificate. Protect follows neither.
  **Keep Nuki Hub on HTTP** and isolate the VLAN.
- **[verified]** Physical USB access = full control (serial config import,
  reset). Mount the board where only you can reach it.
- **[reports]** 9.15–9.16 boot-looped on octal-PSRAM S3 with a fresh config (fixed in 9.17;
  this fork has it). Open: #790, "lock state undefined" on a 5th-gen lock in Hybrid mode (not our mode).

### Sources
- UniFi Protect 7.2.105 firmware `.deb` (fw-download.ubnt.com), `service.js`; UniFi OS 5.1.42 `uos-agent`
- Protect Integration API spec 7.2.105: https://developer.ui.com/protect/
- Ubiquiti: https://help.ui.com/hc/en-us/articles/25478744592023 · SuperLink FAQ https://help.ui.com/hc/en-us/articles/29711478053911
- UniFi community (read via community.svc.ui.com): fob threads 8dea2f05, 92c782fd, 30b6eaaa, e506609f, 94e4471e, c9d2dbae, 3893aff4
- HA: https://github.com/home-assistant/core/pull/175630 · issues 171413, 171414
- https://github.com/uilibs/uiprotect (issue 1205) · https://github.com/hjdhjd/unifi-protect · https://github.com/hjdhjd/homebridge-unifi-protect
- https://github.com/alxgmpr/superlink (SuperLink reverse engineering)
- Nuki: BLE API 2.3.1 (developer.nuki.io), forum t/38668, t/1109, t/39932; help.nuki.io battery and security-code articles
- nuki_hub: discussions #671, #712, #606; issues #786, #790
- Waveshare: https://www.waveshare.com/wiki/ESP32-S3-ETH-8DI-8RO · product page · demo code `WS_ETH.h`; TI TCA9554 datasheet
- https://community.home-assistant.io/t/great-esp32-board-for-an-esphome-bluetooth-proxy/916767
