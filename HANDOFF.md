# Handoff: fix the code-review findings on `waveshare-8di8ro`

**Repo** https://github.com/napieraj/nuki_hub (fork of technyon/nuki_hub)
**Branch** `waveshare-8di8ro` (review was done at `16fdbb55`)
**Upstream** `technyon/nuki_hub` `master` @ `e0aa97aa` (v9.18)

```
git clone --recurse-submodules -b waveshare-8di8ro https://github.com/napieraj/nuki_hub
cd nuki_hub
git remote add upstream https://github.com/technyon/nuki_hub   # read-only reference
cp src/ProtectWebhookConfig.h.example src/ProtectWebhookConfig.h
pio run -e esp32-s3-waveshare-8di8ro
```

## Ground rules
- **Never push to `technyon/nuki_hub`.** Push only to `origin` (`napieraj/nuki_hub`), branch
  `waveshare-8di8ro`, one commit per task below.
- Keep the upstream diff minimal. Put new logic in `src/ProtectWebhook.*` and
  `src/WaveshareBoard.*`, and guard upstream edits with `#ifdef NUKI_HUB_WAVESHARE_8DI8RO`
  or `#ifdef NUKI_HUB_PROTECT_WEBHOOK`. Match upstream style (Allman braces, 4 spaces).
- Never commit `src/ProtectWebhookConfig.h` (git-ignored) or any real secret, MAC or IP.
  Use documentation values (`192.0.2.x`, `AA:BB:CC:DD:EE:01`).
- Examples, fixtures and docs use obviously invented names only, never real people.
- Build after every task. The build is good when the log shows `Total image size` and
  `.pio/build/esp32-s3-waveshare-8di8ro/firmware.elf` exists. The final packaging step
  **always fails** with `updater/release/esp32s3oct/updater.bin: No such file`. That is
  upstream's release packaging and is expected.
- No hardware is available to you. Anything that needs the board, the lock or the UniFi
  console goes on the "user must verify" list in your final report.

### Build gotchas
- PlatformIO Core **6.1.19** is known good (the pioarduino 55.03.311 platform needs ≥ 6.1.19).
- `lib/nuki_ble` is a git submodule: `git submodule update --init --recursive`.
- nuki_hub's component manager strips components from the shared
  `~/.platformio/packages/framework-arduinoespressif32*`. If another Arduino-only project
  later fails with `network_provisioning/network_config.h: No such file`, delete those two
  package directories and rebuild.

## System context
```
USL-FOB ─SuperLink─▶ gateway ─▶ Protect Alarm Manager (one alarm per button+gesture)
  ─POST http://<board>/protect?r=<token>, Authorization: Bearer <secret>─▶
Waveshare ESP32-S3-POE-ETH-8DI-8RO running this fork (W5500 Ethernet only, Wi-Fi never started)
  httpd task (core 1): ProtectWebhook::handle → checks → NukiWrapper::requestLockAction (queue only)
  nuki task (core 0, NimBLE): checkLockAction → BLE → Nuki Smart Lock Ultra
```
- The lock is **paired as app**. The Ultra has no bridge mode. No MQTT, no Hybrid.
- **Fob trigger** (from Protect 7.2.105 `service.js`; not yet confirmed by a live capture):
  `{"key":"sensor_button_pressed","value":"press|longPress|doublePress","device":"AABBCCDDEE01","button":"arm|night|disarm|panic|left|right","eventId":"…","timestamp":<ms>}`
  inside `{"alarm":{name,sources,conditions,triggers:[…]},"timestamp":<ms>}`.
  Global Alarm Manager mode uses the same builder.
- **Protect delivery:** 3 attempts, retried **only on 5xx/408** (1 s, 2 s), 30 s timeout,
  **no redirects**, **rejects self-signed HTTPS**, Bearer/Basic/custom headers, body ≤ 4096
  chars, `User-Agent: protect-alarm-manager`. No queue: a press is lost if the board is down.
- **Board pins:** W5500 CS16 IRQ12 **RST39** SCK15 MISO14 MOSI13; I2C SDA42 SCL41; TCA9554
  relays @0x20; DI1–8 GPIO4–11 (dry contact to DGND, pull-up, active low); WS2812 GPIO38;
  buzzer GPIO46 (strapping); BOOT GPIO0. Module is a WROOM-1U (external antenna). PCF85063 RTC on I2C.
- Background research with sources: `FINDINGS.md`. User-facing setup: `PROTECT_SETUP.md`, `WAVESHARE.md`.

### Facts verified in code during the review (don't re-derive)
- PsychicHttp `ENABLE_ASYNC` is **off**: all handlers run on the single httpd task, which
  is pinned to core 1 via `config.core_id` (HTTP only; see L12).
- Generated sdkconfig: `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU1=y`,
  `CONFIG_BT_NIMBLE_PINNED_TO_CORE=0`. The nuki task is pinned to core 0, the network task to core 1.
- TWDT is reconfigured to **300 s**, panic on, idle tasks on both cores watched (`main.cpp` `setupTasks`).
- Beacon watchdog `preference_restart_ble_beacon_lost` defaults to **60 s**
  (`NukiWrapper::checkRestartByBeacon`). `BLE_ERROR_ON_DISCONNECT` sets `_restartController = 1`.
- `restartServices()` is `restartEsp()` since upstream `35107f0f` (#778, 2026-08-29).
  `lockRestartControllerCount` (`main.cpp:73`) is a plain global, so the "> 3" escalation in
  `processLock()` is dead code. **Every BLE error and every 60 s beacon loss reboots the board.**
- Network watchdog prefs (`restdisc`, `disNtwNoCon`, network timeout) default **off**. With no
  MQTT broker configured they never fire.
- SNTP (`main.cpp` ~1794): the DHCP-provided server comes first, then `preference_time_server`
  (default `pool.ntp.org`), with a 12 h resync. `cbSyncTime` sets the plain global
  `timeSynced`, which is **false again after every reboot** even though the RTC keeps `time()` valid.
- `preference_check_updates` defaults **true**. `WebCfgServer::processUpdate` (~6908)
  downloads **upstream** binaries (`Config.h` `GITHUB_*_URL`).
- The web server, and so `/protect`, starts only if `preference_webserver_enabled` (`main.cpp:1785`).
- `/savewifi` and `/ssidlist` are registered only in AP mode, so Wi-Fi is never started in this build.
- `HWCDC::write` waits up to 20 × `tx_timeout_ms` (100 ms) when USB is plugged in but the
  host isn't reading. This build enables `ARDUINO_USB_CDC_ON_BOOT=1`.
- `PsychicHttpsServer` starts from `ssl_config.httpd`, **not** `config`.
- The W5500 RX task (`esp_eth_mac_w5500.c:983`) is created **unpinned**, priority 15.
- `PsychicClient::remoteIP()` returns the **low 32 bits of any IPv6 peer**. Ethernet has no
  IPv6 today (nuki_hub never calls `enableIPv6`), so this is latent.
- Lock actions: `_nextLockAction` is `volatile`, a single slot, cleared on success and after
  retries are exhausted (`NukiWrapper::checkLockAction` ~296–350). Retries: 3 (+1),
  100 ms delay, general timeout 10 s, command timeout 3 s, so the worst case is **~50 s**
  from queue to motor.
- NukiBleEsp32 reports Success once the lock *accepts* a command (`NukiBle.hpp` ~413,
  `CommandStatus::Complete == Command::Empty == 0`). In a race the command times out and is
  retried. **Don't fix this here**; note it for an upstream issue.
- `espMqttClient` publishes are mutex-protected, so cross-task publishes are safe.

## Tasks (priority order)

### H1: Never pull upstream firmware
- In `WaveshareBoard::applyDefaults`, force every boot: `preference_check_updates=false`,
  `preference_update_from_mqtt=false`.
- Under `NUKI_HUB_WAVESHARE_8DI8RO`, make `processUpdate` and the `autoupdate` / beta /
  master update paths return a page or 403 saying "updates disabled in this build; flash via USB".
  Keep `/uploadota` (manual upload of a locally built image), but check that it can't fetch
  upstream URLs.
- **Accept:** no code path in this env can reach `GITHUB_*_URL`. Explain how you checked.

### H2: USB log writes must never block
- `Serial.setTxTimeoutMs(0)` right after `Serial.begin(115200)` (`main.cpp:1531`), guarded by
  `NUKI_HUB_WAVESHARE_8DI8RO` and `ARDUINO_USB_CDC_ON_BOOT`.
- **Accept:** builds. User to verify: logs don't stall with USB attached to a host with no monitor open.

### H3: Time availability
- Persist "time was synced" across software resets: an `RTC_NOINIT_ATTR` magic value set in
  `cbSyncTime`. At boot, if the reset reason isn't power-on/brownout, the magic is present and
  `time(nullptr)` is later than a sane floor (e.g. 2026-01-01), set `timeSynced = true`.
- Optional (O3): seed and store the time in the on-board PCF85063, so time survives power loss.
- Document in `PROTECT_SETUP.md`: the board needs NTP from DHCP (option 42) or an NTP server set
  in Nuki Hub, with UDP 123 allowed. On an isolated VLAN without it, every press gets `503 no_time`.
- **Accept:** builds. User to verify: a web-UI reboot followed by an immediate webhook gives no 503.

### H4: Webhook actions expire
- Add a deadline to queued actions: e.g. `requestLockAction(const char* action, int64_t deadlineMs)`
  storing `_nextLockActionDeadlineTs`, where 0 means none (MQTT and GPIO paths unchanged).
- In `checkLockAction`, drop the action (log `Lock action expired`, clear the slot) if the
  deadline passed before the first attempt **or before any retry**.
- `PROTECT_WEBHOOK_ACTION_DEADLINE_MS`, default 8000, in the config example.
- **Accept:** builds; the logic is reviewable. User to verify: with the lock out of range, a press
  logs "expired" within ~8 s and nothing unlocks later.

### M5: The webhook can't be switched off by accident
- `applyDefaults` (webhook builds): force `preference_webserver_enabled=true`,
  `preference_disable_network_not_connected=false`, `preference_restart_on_disconnect=false`.
- Document it in `WAVESHARE.md`.

### M6: Reboot-on-BLE-error policy (**propose first, ask the user before coding**)
- Today every BLE disconnect error and every 60 s beacon loss reboots the whole board, so
  Ethernet drops and presses are lost. Persisting the counter alone doesn't help: both branches reboot.
- Options: (a) rate-limit, i.e. keep an error count and timestamps in RTC memory, and reboot only on
  ≥ N errors within M minutes, or when BLE hasn't connected successfully for > X minutes;
  (b) restore a BLE-only soft restart, after checking why upstream removed it in #778;
  (c) leave it and just measure.
- Write the proposal, recommend one, and wait for the user.

### M7: Don't ack an action that can't run
- `bool NukiWrapper::isLockActionPending() const` (`_nextLockAction != 0xff`).
- In `ProtectWebhook::handle`, if an action is pending, reply `503 busy` **before** replay and
  cooldown bookkeeping, so Protect's retry can succeed.

### M8: Per-rule cooldown
- Replace the global `lastAcceptedMs` with one per rule (index into `PROTECT_WEBHOOK_RULES`).
  Keep the global event-ID replay cache.
- Press-to-lock straight after hold-to-unlock must work.

### M9: Config validation
- Make the rules `static constexpr` and add a `constexpr` validator with `static_assert`s:
  - `device` non-null and non-empty; `action` non-null;
  - `value` implies `field`, and `value2` implies `field2`;
  - each rule has a token, or key + value (no "any event from this fob" rule unless
    `PROTECT_WEBHOOK_ALLOW_BROAD_RULES`);
  - secret ≥ 32 chars; tokens ≥ 16 chars;
  - neither secret nor tokens start with `replace`, unless `-DPROTECT_WEBHOOK_ALLOW_PLACEHOLDERS`
    (used by CI with the example config).
- At `registerRoute`, validate each `action` with `NukiHelper::lockActionToEnum` (`src/util/NukiHelper.*`).
  On error, log it and **don't register the route** (fail closed).
- **Accept:** the example config fails to build without `ALLOW_PLACEHOLDERS` and builds with it.

### M10: Source-IP check without the IPv6 hole
- Replace `request->client()->remoteIP().toString()` with `getpeername()` on
  `request->client()->socket()`. Accept `AF_INET`, or `AF_INET6` only if the address is
  IPv4-mapped (`::ffff:0:0/96`). Compare as `uint32_t` against the parsed `PROTECT_WEBHOOK_SOURCE_IP`.

### M11: Bench-tool safety (`scripts/protect_webhook_test.py`)
- Non-live `suite` requires `--acl-disabled` (or an interactive yes), stating that the rule's action
  is **unticked** in Nuki Lock Access Control. Otherwise the valid case really unlocks.
- Build the "valid press" and "new press" events right before sending them, not up front.
- Add `--cooldown-ms`. Update the expectations for per-rule cooldown (M8) and add a
  "different rule inside cooldown is accepted" case if a second token is given (`--token2`).

### L12: HTTPS core pinning
- In `main.cpp` (~663 and ~1673), set `psychicSSLServer->ssl_config.httpd.core_id` (and the
  `stack_size` upstream meant to set) instead of `config.*`. HTTPS stays unsupported for the
  webhook; this is correctness only.

### L13: BLE liveness for the webhook
- Add a heartbeat timestamp updated each loop in `nukiTask` (`main.cpp`). If it's older than 30 s
  when a webhook arrives, reply `503 ble_stalled` and request a restart (via the existing
  `restartEsp` mechanism). The TWDT otherwise needs 300 s.

### L14: Robust to future Protect payloads
- On no-match (after auth), log `key`, `device`, `button`, `value` (not the secret or token).
- `fieldEquals`: also accept `{"text": x}` wrappers (the Integration API style).
- Timestamps: treat `0 < ts < 1e11` as seconds. Reject negative or absurd values before `llabs`.
- Synthesized event-ID buffer: 48 → 64 bytes.

### L15: GPIO roles
- In `Gpio::getDisabledPins` (`src/Gpio.cpp:269`), under the board flag, add 0, 38, 41, 42, 46.
- Document in `WAVESHARE.md` that DI1–8 (GPIO4–11) can take the `InputUnlock` role for a
  hardwired exit button (dry contact to DGND).

### L16: Repo hygiene and CI
- Remove `[env:esp32-s3-protect]` from `platformio.ini` (an unsupported Wi-Fi variant).
- Update the header comment in `src/ProtectWebhook.h` (Bearer, `?r=` token).
- Add `.github/workflows/waveshare.yml`: checkout with submodules, PlatformIO 6.1.19, copy the
  example config, build `esp32-s3-waveshare-8di8ro` with `-DPROTECT_WEBHOOK_ALLOW_PLACEHOLDERS`,
  and treat the updater packaging error as success if `firmware.elf` exists. Also run
  `python -m py_compile scripts/*.py`.
- Check the inherited upstream workflows. Disable any that publish binaries or would fail
  noisily in the fork, and tell the user which.

### Optional
- **O1:** WS2812 (GPIO38) feedback after authentication: short green = queued, red = no match or denied.
- **O2:** Host unit tests. Extract rule matching (MAC/field compare, timestamp normalisation, rule
  selection) into a pure header with no Arduino dependency and add `[env:native]` Unity tests that
  mirror the bench suite.
- **O3:** PCF85063 time seed/store (see H3).

## Out of scope
- The NukiBleEsp32 "success at accept" quirk (upstream). Draft the issue text in your report instead.
- Changing the webhook design (event stream or MQTT alternatives were rejected; see `FINDINGS.md`).

## Report back
1. Per task: what changed (files), and anything you deviated from with the reason.
2. Build result for the Waveshare env, with and without `ALLOW_PLACEHOLDERS`.
3. The M6 proposal, if not yet approved.
4. The list of things the user must verify on hardware.
5. Anything in upstream `master` newer than `e0aa97aa` that affects these files (fetch `upstream` and check).
