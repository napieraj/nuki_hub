## Setup guide: Waveshare 8DI-8RO + Nuki Lock Ultra + UniFi Protect fobs

From an empty board to "hold the fob button, the door unlocks". Follow the steps in
order. Reference material: `WAVESHARE.md` (what the board build changes),
`PROTECT_SETUP.md` (webhook details, response codes), `FINDINGS.md` (background).
All names, addresses and keys below are invented examples.

```
USL-FOB ─SuperLink─▶ gateway ─▶ Protect Alarm Manager (one alarm per button + gesture)
   ─ POST http://<board>/protect?r=<token>, Authorization: Bearer <secret> ─▶
Waveshare ESP32-S3-POE-ETH-8DI-8RO (Ethernet only) ─ BLE ─▶ Nuki Lock Ultra
```

### 0. What you need
- **Board:** Waveshare ESP32-S3-POE-ETH-8DI-8RO, an SMA antenna (the module has no
  PCB antenna), a USB-C data cable.
- **Power and network:** an 802.3af PoE switch port (or the board's DC input, per
  Waveshare's spec, plus a normal Ethernet port).
- **Optional:** a rechargeable **ML1220** cell for the RTC holder (not CR1220), so
  the board keeps the time through power cuts.
- **Lock:** Nuki Lock Ultra (or Go / 5th gen), set up in the Nuki app, and its 6-digit PIN.
- **UniFi:** a console with UniFi OS >= 5.1.11 and Protect >= 7.1.60, a SuperLink
  Gateway, and USL-FOB key fobs (firmware >= 1.4.0).
- **Computer:** git, Python 3.11+, `openssl`, and PlatformIO Core 6.1.19
  (`pip install platformio==6.1.19`).

### 1. Get the code
```
git clone --recurse-submodules -b waveshare-8di8ro https://github.com/napieraj/nuki_hub
cd nuki_hub
```
If you cloned without `--recurse-submodules`: `git submodule update --init --recursive`.

### 2. Webhook config: nothing to do before building
The webhook (secret, source IP, rules, relays) is configured later on the
board's web page **Protect Webhook & Relays** (step 9) and stored on the board.
There is no config file to fill in, and changing a rule never needs a reflash.

Optional: if you prefer to prepare the values up front, copy
`src/ProtectWebhookConfig.h.example` to `src/ProtectWebhookConfig.h` (ignored by
git; never commit it) and fill it in. It is only used once, to fill the web
page's settings on the first boot of an empty board; after that the web page
wins and the file is ignored. The build refuses the file if the secret or a
token is still a placeholder or too short, or a rule is incomplete.

### 3. Build
```
pio run -e esp32-s3-waveshare-8di8ro
```
The first build downloads the toolchain and takes a while. It ends with
`SUCCESS`, and the files to flash are in `release/esp32s3oct/`:
`nuki_hub_esp32s3oct.bin`, `nuki_hub_bootloader_esp32s3oct.bin`,
`nuki_hub_partitions_esp32s3oct.bin`, `boot_app0.bin`.

Optional check of the webhook logic on your computer: `pio test -e native`.

### 4. Flash over USB-C
1. Screw on the antenna. Connect USB-C to your computer (PoE can stay connected).
2. First flash only: erase everything, including settings from any earlier firmware.
   If the board isn't detected, unplug it, hold **BOOT** while plugging USB-C back
   in (with PoE disconnected), then release **BOOT**.
   ```
   pio run -e esp32-s3-waveshare-8di8ro -t erase
   ```
3. Flash:
   ```
   pio run -e esp32-s3-waveshare-8di8ro -t upload
   ```
   Or with esptool, from `release/esp32s3oct/` (port is `/dev/ttyACM0` on Linux,
   `/dev/cu.usbmodem…` on macOS, `COMx` on Windows):
   ```
   esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write-flash -z \
     --flash-mode dio --flash-freq keep --flash-size detect \
     0x0 nuki_hub_bootloader_esp32s3oct.bin 0x8000 nuki_hub_partitions_esp32s3oct.bin \
     0xe000 boot_app0.bin 0x10000 nuki_hub_esp32s3oct.bin
   ```
4. Watch the log: `pio device monitor -b 115200`. If it's quiet, power-cycle the board.

**Later updates** also go over USB: build, then step 4.3 without the erase.
Settings and the lock pairing are kept. The web UI's upload only accepts a Nuki
Hub *updater* image, which this build doesn't have, and online updates are
disabled on purpose.

### 5. First boot and web UI
1. Plug the board into its PoE port. It uses Ethernet with DHCP; Wi-Fi never
   starts, and there is no "NukiHub" access point.
2. Find its IP in your UniFi client list (or in the USB log) and give it a fixed
   DHCP reservation, since Protect's webhook URL will point to it.
3. Open `http://<board-ip>` and set up, in this order:
   - **Credentials:** a user and password (Digest or Form auth), then a **TOTP**
     secret or Duo, so the web UI needs a second factor.
   - **Network:** leave the hardware as is (the build pins it to the W5500).
   - **Nuki Configuration:** "Update Nuki Hub and Lock/Opener time using NTP" is
     switched on by the build; set the **NTP server** there if your DHCP doesn't
     hand one out (see step 8).
   - **MQTT:** leave empty. Nothing here needs a broker.
   - **Do not enable HTTPS.** Protect rejects self-signed certificates and doesn't
     follow redirects.

### 6. Pair the lock
1. In the Nuki app: **Settings → Features & Configuration → Button and LED →
   Bluetooth pairing** on.
2. In Nuki Hub **Basic Nuki Configuration**: enable **Nuki Smartlock enabled** and
   **Nuki Smartlock Ultra/Go/5th gen enabled**, Save. It pairs as an app (the
   Ultra has no bridge mode).
3. **Credentials → Nuki Lock PIN**: enter the lock's 6-digit PIN, Save.
4. Hold the lock's button until the LED ring stays lit. The status page should
   show **Paired: Yes** within a minute.
5. **Access Level Configuration → Nuki Lock Access Control**: tick **only** the
   actions your rules use (e.g. Unlock and Lock). Webhook actions go through this
   list; GPIO inputs don't.

Upstream recommends Hybrid mode (Thread/Wi-Fi + Nuki MQTT) for the Ultra. This
setup doesn't use it: lock actions work without it, but state changes made at the
lock (keypad, turning by hand) only show up at the next status poll.

### 7. Confirm what a fob press looks like (once, 5 minutes)
1. On your computer: `scripts/protect_webhook_test.py listen --port 8099`
2. In Protect → **Alarm Manager** → Create Alarm: trigger **Button**, gesture
   **Long Press (3s)**, scope the fob's **Right** button. Action **Custom Webhook**,
   `http://<computer-ip>:8099/capture`, method **POST**.
3. Hold Right for 3 s. Check that the JSON has `"key":"sensor_button_pressed"`,
   `"button":"right"`, `"value":"longPress"` and the fob MAC in `"device"`. If
   anything differs, use what you see in your rules (step 9).
4. Press Right briefly, and press Left: nothing should arrive.
5. Delete this test alarm.

### 8. Network
- Put the board on its own VLAN. Firewall: allow only `<console-ip> → <board-ip>` TCP 80,
  plus the web UI from your admin machine.
- **Time is required.** The board refuses presses until its clock is set
  (`503 no_time`, log `time not synced`). The build switches on "Update Nuki Hub
  and Lock/Opener time using NTP" (Nuki Configuration) at every boot. It uses the NTP
  server from DHCP option 42 if there is one, else the "NTP server" set there
  (default `pool.ntp.org`). Allow UDP 123 from the board to it. The log shows
  `NTP time synced` once it works.
- Bluetooth range: the lock needs a good signal. Mount the antenna outside any
  metal cabinet, with a clear path towards the lock.

### 9. Configure the webhook on the board
Open **Protect Webhook & Relays** in the web UI (it needs the login from step 5;
the page shows a red warning if the web UI has no password).
1. **Secret:** press *Generate random*, copy the 64-character value somewhere safe
   (you need it for every Protect alarm in step 10), then continue. After saving,
   the page never shows it again, only "set, 64 characters".
2. **Source IP:** your UniFi console's IP, e.g. `192.0.2.10`.
3. **Rules:** one per alarm. For "Fob A - Hold Right":
   enabled, name `Fob A - Hold Right`, token *Generate random* (copy it: it goes into
   the alarm's URL), key `sensor_button_pressed`, device `AA:BB:CC:DD:EE:01` (the
   fob MAC from the fob's page in Protect or from step 7), field `button` =
   `right`, field 2 `value` = `longPress`, action **Lock: unlock**.
   "Fob A - Press Right": the same with its own token, value 2 `press`, action
   **Lock: lock**. Hold (3 s) to unlock is hard to trigger by accident in a
   pocket. `unlatch` also works; it is never retried after a timeout, so it
   can't open the door twice. `lock` isn't either, so a retry can't hit the
   bolt while it's still moving.
4. Tick **Webhook enabled** and **Save**. The page checks everything (secret
   ≥ 32 characters, tokens ≥ 16, a MAC and an action per rule, …) and says what
   to fix; nothing is stored until it passes. Changes apply at once.
5. The status box at the top should read **Webhook: Active** and **Clock: Synced**.

Optional hardening on the same page (all off by default, one line of help each):
*Bearer only*, *Require source IP*, *Require a TOTP code for every save*, and the
*Settings lock* (saving only while a DI input you wired is active). Recovery
over USB-C serial: `forkcfg unlock`, `forkcfg off`. Details: `PROTECT_SETUP.md`.

### 10. Create the real alarms in Protect
One alarm per rule, e.g. "Fob A - Hold Right" and "Fob A - Press Right":
- Trigger: **Button**, the gesture (**Long Press (3s)** or **Press**), scope the fob's **Right** button.
- Action: **Custom Webhook**, `http://<board-ip>/protect?r=<that rule's token>`, method **POST**.
- Authentication: **Bearer**, token = the secret from step 9.
- Leave **Ignore repeated actions** off. Don't tie the alarm to an arm profile.

### 11. Test without moving the lock
1. In **Nuki Lock Access Control**, untick the actions your rules use.
2. The board only accepts requests from the console's IP. For this test, either run
   the tool on a machine with that IP, or temporarily set the source IP on the
   web page to your computer's IP (set it back afterwards).
3. Run:
   ```
   scripts/protect_webhook_test.py suite --url http://<board-ip>/protect --bearer \
     --secret <secret> --token <hold token> --device AA:BB:CC:DD:EE:01 \
     --key-checked --field button --value right --field2 value --value2 longPress \
     --token2 <press token> --rule2-value2 press --acl-disabled
   ```
   Expected: **17/17 PASS**. The valid press ends in `403 acl_denied`, which means it
   passed every check and only the unticked action stopped it.
4. Tick the actions again. Hold the real fob's Right button for 3 s: the board's
   LED flashes **green** and the lock unlocks. A short press locks.

### Everyday use
- **LED after a press:** short green = accepted and sent to the lock; red = no
  rule matched, the press was too old, or the action is not allowed. No flash =
  busy, repeated press, no time yet, or not authenticated.
- **Response codes** Protect sees: see `PROTECT_SETUP.md`.
- **Info page** ("System Information"): the last restart reason, and every
  Bluetooth error or beacon loss that rebooted the board (last 10 and counts).
- **A press did nothing:** check the web page's status box and recent requests,
  the USB log or the info page. Common causes: no
  NTP (`503 no_time`), the lock out of range (`Lock action expired` after 8 s),
  a Protect alarm pointing at the wrong token, or the action unticked in the ACL.
- **Lost fob:** delete it in Protect **and** disable its rules on the web page
  (applies at once). In a hurry: *Switch the webhook off now* on the page, or
  `forkcfg off` on the USB console.
- **What happened to a press:** the web page's "Recent webhook requests" lists
  the last 10 results since boot.
- **Exit button:** wire a dry contact between a DI input (DI1-8 = GPIO 4-11) and
  DGND and give that GPIO the role **Input: Unlock**. GPIO actions bypass the ACL,
  so keep the button and wiring on the inside.

### Checklist
- [ ] Builds with `SUCCESS`
- [ ] Flashed; web UI reachable at a reserved IP
- [ ] Credentials + TOTP/Duo set; HTTPS off
- [ ] Lock paired ("Paired: Yes"), ACL allows only the needed actions
- [ ] Payload captured once and rules match it
- [ ] Board has NTP (no `503 no_time`); firewall allows only the console on port 80
- [ ] Webhook configured on the web page: status "Active", clock "Synced"
- [ ] Protect alarms created with Bearer auth and per-alarm tokens
- [ ] Bench suite 17/17, then a real fob press unlocks and locks
