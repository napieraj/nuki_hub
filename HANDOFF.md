# Handoff: `waveshare-8di8ro` (state as of 2026-09-27)

**Repo** https://github.com/napieraj/nuki_hub (fork of technyon/nuki_hub, upstream
`master` @ `e0aa97a`, v9.18; nothing newer upstream at the time of writing).
**Branch** `waveshare-8di8ro`. Last hardware-flashed code: `c94cf37`. Push only here, never to technyon.
The previous task list (H1–L16, O1–O3) is done; it's in git history
(`git show 6c92b93:HANDOFF.md`).

## What this fork is
One Waveshare ESP32-S3-POE-ETH-8DI-8RO board (PoE, W5500 Ethernet, no Wi-Fi) runs
Nuki Hub, paired over BLE (as app) with a Nuki Lock Ultra. UniFi Protect Alarm
Manager sends a webhook per fob button + gesture:
`POST http://<board>/protect?r=<rule token>`, `Authorization: Bearer <secret>`.
The board checks it and queues a Nuki action or pulses a relay. No MQTT, no Hybrid.

- Setup from scratch: `SETUP.md`. Webhook details and response codes:
  `PROTECT_SETUP.md`. Board behaviour: `WAVESHARE.md`. Background: `FINDINGS.md`.
- Config is compile-time in `src/ProtectWebhookConfig.h` (git-ignored, template
  `.example`). Rules are `static constexpr`; the build refuses placeholders,
  short secrets/tokens and incomplete rules.

## Build, flash, test
```
source ~/.venvs/pio/bin/activate            # PlatformIO Core 6.1.19
git pull && pio run -e esp32-s3-waveshare-8di8ro -t upload -t monitor
pio test -e native                          # 22 host tests, webhook + RTC logic
```
- The build ends in `SUCCESS`; images land in `release/esp32s3oct/`. It builds no
  updater, so **firmware updates are USB-only** (the web upload page only takes
  updater-sized images). Online/upstream updates are compiled out.
- CI: `.github/workflows/waveshare.yml` (host tests + build with the example
  config and `-DPROTECT_WEBHOOK_ALLOW_PLACEHOLDERS`). Publishing upstream
  workflows are fenced to `technyon/nuki_hub`.
- A build script rewrites the date in `src/Config.h`; don't commit that.

## Verified on hardware (user's board, 2026-09-27)
- Flash, Ethernet/DHCP, web UI, pairing the Ultra as app ("Paired: Yes").
- Real fob payload captured (Protect sends the shape the code expects, plus an
  ignored `alarm_id`). Protect → board → lock works: **press Right → lock**.
- Source-IP check and Bearer secret pass from the real console.
- Found and fixed on hardware: NTP never started (`b717859`, `ffc4812`), and
  packaging produced no flashable image (`dbe30c7`).

## Not yet verified / open
1. **NTP fix on hardware:** after flashing `ffc4812`+, the log should show
   `NTP time synced`; presses before that get `503 no_time`.
2. **Hold Right → unlatch:** the user switched the hold rule from `unlock` to
   `unlatch` (knob, no handle). Earlier the hold alarm hit `no rule` because the
   Protect alarm URL carried the wrong `?r=` token; fix the URL. Tick **Unlatch** in
   Nuki Lock Access Control.
3. **Double Right → relay (intercom / street door):** relay actions `relay1`..`relay8`
   landed in `c94cf37` (pulse `PROTECT_WEBHOOK_RELAY_PULSE_MS`, default 3 s). The user
   still has to set the double-press rule's action to e.g. `relay1`, wire relay
   COM+NO across the intercom's door-open button (dry contact only), and **first
   power-cycle/reflash a few times with nothing wired** to prove no relay clicks at
   power-up. Untested on hardware.
4. **Secret:** the user pasted the original secret in chat; it was to be rotated
   (new secret in the config file + reflash + new Bearer token in all Protect
   alarms). Confirm that happened.
5. Lock PIN shows "No valid Nuki Lock PIN set" in the log; re-enter it under
   Credentials (only needed for time sync to the lock).
6. Left button: three rules are prepared as commented placeholders in the user's
   config; uncomment, give each a token, add a Protect alarm scoped to Left.
7. Other hardware checks from the earlier report: LED colour order (GRB assumed),
   `ble_stalled` never firing during normal use, RTC restore after a PoE power cut
   with an ML1220 fitted, info page BLE event log.

## Since the last flash (2026-09-27, review pass, build + host tests green, untested on hardware)
- **Relays** (`4d3ea74`, `659da37`, `48fc15b`): the TCA9554 init writes the outputs off, reads them back, and
  only then makes the pins outputs. If that fails, the pins stay inputs, requests get a 500, and every relay
  write retries the init. The off timer re-arms every 100 ms until the relay opens, and never blocks the
  esp_timer task. The build refuses a pulse length outside 100–30000 ms. A reset mid-pulse leaves the relay
  closed until the next boot's init (the chip has no reset line). **Bench test before wiring the intercom:**
  WAVESHARE.md / PROTECT_SETUP.md; power-cycle and reflash with a continuity meter across COM–NO, then pulse,
  cooldown `429`, reset mid-pulse.
- **Unlatch is not re-sent after an ambiguous result** (`8647d98`): Unlatch, LockNgoUnlatch and FobAction1–3
  are retried only on `NotPaired`. TimeOut, Failed and Lock_Busy can each come after the lock got the command.
  The log shows `Unlatch: result ambiguous, not retrying`. The cost: on a weak link an unlatch can fail, and
  the user presses again.
- **Time** (`d1661ca`, `8e080ba`): an empty NTP server field falls back to pool.ntp.org. The PCF85063 restore
  now rejects bad BCD, impossible dates, STOP and 12 h mode (`src/WaveshareRtcLogic.h`, tested), and logs
  reads and writes. Expected log lines: WAVESHARE.md.

## Decisions pending with the user
- **Updater:** stay USB-only, or build/ship an updater so the web UI can update.
- **M6 (BLE reboots):** currently measure-only (every BLE error/beacon loss still
  reboots; logged in RTC memory, shown on the Info page). Decide on rate-limiting
  once there's data from the real Ultra.
- **Next feature (started, then stopped at the user's request, nothing merged):**
  move all fork settings into one new web UI menu item, stored in NVS instead of
  the compile-time header. Constraints the user gave: simple `#ifdef`-guarded hooks
  in upstream files (new code in its own files), security hardening **offered as
  options, not baked in**, relay count selectable. Open design points: precedence
  between the header and NVS, and making the header optional so the build works
  without it.

## Code map (fork-only files)
- `src/ProtectWebhook.cpp/.h`: route, checks in order: source IP, secret, size,
  clock, JSON, rule match (token/key/MAC/button/gesture), freshness, BLE alive,
  lock busy (both skipped for relay rules), replay + per-rule cooldown, then
  queue the action with an 8 s deadline, or pulse a relay.
- `src/ProtectWebhookLogic.h`: pure logic + `test/test_protect_webhook`.
- `src/WaveshareBoard.cpp/.h`: pins, W5500 reset, relays (TCA9554, I2C kept up
  from `earlyInit`), WS2812 feedback, PCF85063 RTC, time-sync persistence, BLE event
  log, forced settings (`applyDefaults`: no updates, web server on, NTP on, …).
- Upstream edits are small and guarded by `NUKI_HUB_WAVESHARE_8DI8RO` /
  `NUKI_HUB_PROTECT_WEBHOOK` (main.cpp, WebCfgServer.cpp, NukiWrapper, NukiNetwork,
  Gpio.cpp, Config.h, HomeAssistantDiscovery.cpp, NukiRetryHandler.cpp,
  pio_package_post.py).

## Gotchas
- A running board is quiet in the log; a "nukiTask is running" line appears every 2 min.
- `pio device monitor` needs `-e esp32-s3-waveshare-8di8ro`, or it warns about the
  exception decoder (harmless).
- zsh doesn't treat `#` as a comment; don't paste commands with trailing comments.
- Never paste the secret or tokens into chats or issues. Log lines never contain them.
- The ACL (Nuki Lock Access Control) gates webhook lock actions, not GPIO inputs
  and not relay rules.

## Upstream issue to file (I-Connect/NukiBleEsp32; iranl's fork has issues disabled)
Ready-to-file draft: `docs/upstream-issue-nukible-accepted-vs-complete.md`. At `NukiBle.hpp:413`,
`(CommandStatus)lastMsgCodeReceived == Complete` matches `Command::Empty` (both 0). So lockAction returns
Success about 10 ms after ACCEPTED, and errors after acceptance (motor blocked, canceled) read as Success.
Correction to the earlier note: the double unlatch doesn't come from this. It comes from a TimeOut while
waiting for ACCEPTED (hpp:357-364), when the lock may already have run the command. The fork handles that
itself now (see above). Related downstream report: technyon/nuki_hub#278.
