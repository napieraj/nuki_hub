# Handoff: `waveshare-8di8ro` (state as of 2026-09-29)

**Repo** https://github.com/napieraj/nuki_hub (fork of technyon/nuki_hub, upstream
`master` @ `e0aa97a`, v9.18). **Branch** `waveshare-8di8ro`. Push only here, never
to technyon. The previous handoff (2026-09-27) is in git history
(`git show 91a0766:HANDOFF.md`); the task list before that is in `6c92b93`.

**Flashed on the user's board:** the QoS 1 test build of `2d5c62d` minus the
skip grace window (i.e. up to `e159c23`/`fa2159e` built with
`-DLOCK_MQTT_ACTION_QOS=1`). The user was about to flash `2d5c62d`+ (skip grace,
QoS 1 as default).

## What this fork is
One Waveshare ESP32-S3-POE-ETH-8DI-8RO (PoE, W5500 Ethernet, Wi-Fi compiled out)
runs Nuki Hub for a Nuki Smart Lock Ultra, paired over BLE as an app. UniFi
Protect Alarm Manager sends a webhook per fob button + gesture
(`POST http://<board>/protect?r=<rule token>`, `Authorization: Bearer <secret>`);
the board checks it and sends a lock action or pulses a relay.

Lock actions go over **Nuki's official MQTT API** when the lock is connected:
the board runs a tiny single-client MQTT server (port 1883) that only the lock
logs into (over its Wi-Fi); Nuki Hub talks to it in-process (upstream "Hybrid
mode"). Otherwise, and as fallback, over BLE. Nuki Hub's own external MQTT
client, HA discovery, OTA and the HTTPS server are compiled out.

- Setup from scratch: `SETUP.md`. Webhook, settings page, lock MQTT, response
  codes: `PROTECT_SETUP.md`. Board behaviour and log lines: `WAVESHARE.md`.
  Background: `FINDINGS.md`.
- **Settings live in NVS** (namespace `forkcfg`, one versioned blob), edited on
  the web page **Protect Webhook & Relays**; changes apply live. The optional
  git-ignored `src/ProtectWebhookConfig.h` only seeds NVS on a board with no
  `forkcfg` (fresh, erased, factory reset). Secrets are write-only on the page.
  USB console recovery: `forkcfg off`, `forkcfg unlock`.

## Build, flash, test
```
source ~/.venvs/pio/bin/activate
pio run -e esp32-s3-waveshare-8di8ro -t upload; git checkout src/Config.h
pio device monitor -e esp32-s3-waveshare-8di8ro
pio test -e native
```
- Waveshare is the default env now; plain `pio run` / `pio device monitor` work too.
- Updates are USB-only. Flash without erase: NVS (pairing, settings) is kept.
  `-t erase` wipes the pairing.
- The build rewrites the date in `src/Config.h`; don't commit that.
- **Never run two builds in the same folder at once** (they delete each other's
  object files; seen 2026-09-28).
- Full-flash backup/restore: `esptool --chip esp32s3 --port /dev/cu.usbmodem101
  --baud 921600 read-flash 0 ALL <file>` / `write-flash 0 <file>` (esptool is
  installed in the pio venv). A backup exists at `~/nukihub-backup-2026-09-27.bin`
  (contains secrets and the pairing keys).
- CI: `.github/workflows/waveshare.yml`: host tests, a build without the header,
  a build with the example header.

## Verified on hardware (2026-09-28)
- Migration from the header build: settings seeded into NVS, then loaded from NVS
  after reboot; same IP/MAC; pairing kept.
- Fob → webhook → lock: lock, unlock, and **hold Right → unlatch** (over MQTT).
- Lock MQTT: the Ultra connects with MQTT 3.1.1, client `Nuki_4BB2979F`,
  keepalive 300 s; subscribes to `nuki/4BB2979F/lockAction`; state, battery,
  `lockActionEvent`, `commandResponse` arrive.
- **Redundant lock skipped** while already locked (no motor, no jam).
- Timing (ms after the webhook): BLE lock confirmed 1617. MQTT QoS 2 unlatch:
  received 241, PUBCOMP 1428, lockActionEvent 1696. MQTT QoS 1 lock: received
  179, lockActionEvent 1526. Felt clearly snappier over MQTT/QoS 1 → QoS 1 is the
  default (`2d5c62d`).
- A single BLE lock on an already-locked Ultra ended in `motorBlocked` (no retry
  involved): redundant locks, not retries, caused the jam notifications.

## What changed since 2026-09-27 (all merged into `waveshare-8di8ro`)
- **settings-ui:** NVS settings + web page, CSRF token + Origin check, hardening
  options (Bearer only, require source IP, broad rules, TOTP per save, DI
  settings lock), last 10 webhook results, relay count, factory reset clears it.
- **lock-no-resend:** lock / lock 'n' go / full lock are not re-sent after an
  ambiguous result (like unlatch). Unlock still retries.
- **no-wifi:** `NUKI_HUB_NO_WIFI`, coexistence off, −285 KiB.
- **lock-mqtt:** embedded single-client MQTT server (`src/LockMqtt*`), in-process
  hybrid wiring, BLE fallback (2 s), skip redundant lock/unlock only while the
  session is live and the state is fresh, `Protect timing:` log lines,
  `NUKI_HUB_NO_EXTERNAL_MQTT` (strips MQTT client, HA discovery, OTA task, HTTPS).
- **fa2159e:** periodic lock queries (config, battery, …) now run without an
  external MQTT connection. Before, the config was never read, so the Nuki ID
  stayed 0 (MQTT topics `nuki/0/…`) and the PIN looked invalid.
- **tidy-fixes:** truthful boot/retry log lines, Key/Field/Value charset check on
  save, UTF-8 page, Waveshare default env.
- **skip-grace:** never skip a lock/unlock within N s (default 60, 0 = off) of the
  previous fob lock action. A *skipped* press also counts as previous action
  (open question for the user, see below).
- **QoS 1** for lockAction by default (`-DLOCK_MQTT_ACTION_QOS=2` for Nuki's QoS 2).

## Open / next
1. Flash `2d5c62d`+ and check the "Skip grace" field (60 s) on the page.
2. **Decision:** should a skipped press start the skip-grace window (current), or
   only presses actually sent to the lock? (Remove `notePrevLockAction(m)` in the
   skip branch of `ProtectWebhook.cpp` for the latter.)
3. **Rotate the secret** on the page (the original was pasted in chat), update the
   Bearer token in every Protect alarm. Then update or delete the user's
   `ProtectWebhookConfig.h` in `/Users/oskar/nuki_hub/src/` so an erase can't
   restore the old secret or the old `unlock` hold rule.
4. **Relay:** rule `Fob · Double Right -> Relay 1` + Protect alarm (Double Press)
   were being set up. Bench test before wiring the intercom (no click at boot,
   one ~3 s pulse, `429` within the cooldown).
5. Battery/BLE: upstream hybrid mode does a full BLE state read (and a log fetch on
   some states) after every MQTT state change. Trim if lock battery suffers.
6. Door sensor: no `doorsensorState` seen over MQTT yet; open/close once and check.
7. Crash decoder can't find `firmware.elf` (the release script moves it to
   `release/esp32s3oct/`).
8. Not merged, off by default: branch `protect-ws-prewarm` (Protect event stream →
   BLE warm-up). Mostly obsolete with MQTT; its `race` command and timing work
   are the useful parts.
9. Earlier items still open: LED colour order, `ble_stalled` in normal use, RTC
   restore after a PoE power cut with an ML1220, Left-button rules, updater
   decision (USB-only today), M6 BLE-reboot rate limiting.
10. Settings page security review (auth dispatch, CSRF/Origin, MFA re-grant,
    write-only secrets) was recommended and not done.

## Related
- nRF52840 bridge (`napieraj/nRF52840_nuki_bridge`): findings doc for that repo at
  `~/nrf-findings-from-nuki-hub.md` / `~/nrf-findings.bundle` (branch
  `findings-from-nuki-hub`, not pushed). Key points: its `cmd_engine` re-sends lock
  actions on timeout; it uses Simple Lock Action 0x0100 on the Ultra (unverified
  challenge-free); possible gen 1–4 challenge bug.

## Code map (fork-only files)
- `src/ProtectWebhook.cpp/.h`, `src/ProtectWebhookLogic.h`: route and checks.
- `src/ForkSettings*.cpp/.h`, `src/ForkSettingsLogic.h`: NVS settings, page, validation.
- `src/LockMqttServer.cpp/.h`, `src/LockMqttLogic.h`: embedded MQTT server, skip rule.
- `src/ProtectTiming.h`: per-press timing lines.
- `src/WaveshareBoard.cpp/.h`: pins, W5500, relays (TCA9554), WS2812, PCF85063 RTC.
- Host tests: `test/test_protect_webhook`, `test_fork_settings`, `test_lock_mqtt`,
  `test_waveshare_rtc`.
- Upstream edits are small and guarded by `NUKI_HUB_WAVESHARE_8DI8RO`,
  `NUKI_HUB_PROTECT_WEBHOOK`, `NUKI_HUB_NO_WIFI`, `NUKI_HUB_NO_EXTERNAL_MQTT`,
  `NUKI_HUB_EMBEDDED_LOCK_MQTT`.

## Gotchas
- After a board reboot the lock takes 30–90 s to reconnect over MQTT.
- "Expected client ID" on the page must be empty or exactly `Nuki_<ID>`; anything
  else refuses the lock.
- The ACL (Nuki Lock Access Control) gates webhook lock actions, not GPIO inputs
  and not relay rules.
- Never paste the secret or tokens into chats or issues. Log lines never contain them.

## Upstream issue to file (I-Connect/NukiBleEsp32)
Draft: `docs/upstream-issue-nukible-accepted-vs-complete.md`. At `NukiBle.hpp:413`,
`(CommandStatus)lastMsgCodeReceived == Complete` matches `Command::Empty` (both 0),
so lockAction returns Success ~10 ms after ACCEPTED, and errors after acceptance
(motor blocked, canceled) read as Success. Seen on hardware 2026-09-28: `Lock action
result: success`, then `motorBlocked`. Related: technyon/nuki_hub#278.
