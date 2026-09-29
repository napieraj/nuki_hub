# Handoff: relay state outputs on `waveshare-8di8ro`

**Repo** https://github.com/napieraj/nuki_hub, branch `waveshare-8di8ro`. First
written at `91a0766` (2026-09-27), rewritten on 2026-09-29 against `e03f329`
(embedded lock MQTT, NVS settings blob, fa2159e). Read `HANDOFF.md` first
(ground rules, build, code map). This file adds one feature.

## Goal
Drive the board's 8 relays from lock, keypad and door-sensor state, so they can
feed dry contacts into something else (alarm panel zones, indicator lamps; the
user hasn't said which yet, so keep polarity configurable).

Setup: one Nuki Smart Lock Ultra (paired as app over BLE, firmware 5.9.4), one
Nuki Keypad (the newest model, with NFC and Apple Home Key) and one Nuki Door
Sensor, both paired to the lock. The lock is connected to the board's built-in
MQTT server (hybrid mode). The board reads keypad and door-sensor state
**through the lock** (its MQTT topics, key turner state, activity log). It
never talks to them directly.

Nuki Hub's Info page says "Has keypad: No" although a keypad is paired:
`_hasKeypad` only checks `hasKeypad`/`hasKeypadV2` from the lock config, which
this model apparently doesn't set. So **nothing here may depend on
`hasKeypad()`** (see "Keypad" below).

## The rule for this task: configurable roles, not hard-coded
- **Every relay gets a role selected on the "Protect Webhook & Relays" page.**
  No relay-to-signal mapping in code or in `ProtectWebhookConfig.h`.
- **A master switch** ("Relay state outputs", default **off**). With it off,
  nothing changes from today: relays only move for webhook `relayN` rules,
  "Relays available to rules" limits which, and no extra task runs.
- **Defaults below apply only when the switch is on AND the relay has no
  stored role.** Never overwrite a role the user chose. A "Reset relays to
  defaults" button (with a confirmation) does overwrite.
- Roles are an `enum class` with explicit, stable numeric values. **Append
  only**, never renumber: the numbers are what NVS stores. An unknown stored
  value reads as `Off`.
- Per relay, next to the role: **Invert** (checkbox) and **Pulse ms** (100-30000,
  the existing relay pulse range; used by pulse roles).
- Show each relay's role and live state (closed/open, pulsing) on the page, and
  on the Info page.

## Storage: the `forkcfg` blob, not separate keys
All fork settings live in NVS namespace `forkcfg`, one versioned blob (`cfg`,
`ForkSettingsLogic.h`, format **v3** today). The relay settings go into it:

- Bump the format to **v4** and append: master switch (flag), then per relay
  (8 slots): role (u8), flags (bit 0 invert, bit 1 "role stored"), pulse ms
  (u32).
- A **v3 blob must still load**: relay outputs off, no role stored, and every
  relay's pulse = the old global relay pulse (so `relayN` rules pulse exactly
  as long as before). Host test for loading a v3 blob.
- A relay whose role/invert the page submits unchanged from the default and
  that had no stored role stays "not stored", so a save doesn't freeze the
  defaults; changing anything stores it.
- `forkcfg` is **excluded from Nuki Hub's config export/import on purpose** (it
  holds the webhook secret, rule tokens, the lock MQTT password). Keep it so:
  relay roles are not exported either. No `PreferencesKeys.h` keys.
- Downgrade: a v3 firmware refuses a v4 blob ("stored settings unreadable,
  webhook off until saved again", blob kept). Say so in the docs.

## Relays already on the page: reconcile
Today the page has "Relays available to rules" (`relayCount`, 1-8) and one
global "Relay pulse" (`relayPulseMs`) used by every `relayN` rule.
- The global pulse is replaced by the per-relay **Pulse ms** (migrated from it,
  see above). The old field stays in the blob (unused) so the layout only grows.
- **Master off:** `relayN` rules are allowed for N <= "Relays available to
  rules" (as today) and pulse for that relay's Pulse ms.
- **Master on:** the role table decides: a rule may use `relayN` only if relay
  N's role is `WebhookPulse`; "Relays available to rules" is ignored (the page
  says so). Invert is refused for `WebhookPulse` (it would hold the intercom
  button pressed).
- The user's current rule "Fob · Double Right -> Relay 1" (action `relay1`,
  intercom door-open button) keeps working in both modes: relay 1's default role
  is `WebhookPulse`.

## Roles
"Closed" means the relay coil is energized (COM-NO closed). **Invert** flips it.
`S` = steady (follows state), `P` = pulse (closes for Pulse ms on an event).

| Value | Role | Kind | Closed when |
|---|---|---|---|
| 0 | `Off` | – | never |
| 1 | `WebhookPulse` | P | a Protect webhook rule with action `relayN` fires (today's behaviour) |
| 2 | `Secure` | S | lock state `Locked` **and** door `DoorClosed` |
| 3 | `DoorOpen` | S | door `DoorOpened` |
| 4 | `Locked` | S | lock state `Locked` (not `Locking`; upstream's GPIO role counts `Locking` too, relays shouldn't click on a transient) |
| 5 | `LockFault` | S | any of: lock state `MotorBlocked`; `lastLockActionCompletionStatus` in {MotorBlocked, LowMotorVoltage, ClutchFailure, MotorPowerFailure, IncompleteFailure, Failure}; BLE comm error; **state stale** (both below) |
| 6 | `BatteryLow` | S | lock, keypad or door sensor battery critical |
| 7 | `DoorSensorFault` | S | door state in {DoorStateUnknown, Uncalibrated, Tampered}, or the last door-sensor log entry is `SensorJammed` (latched until the next door open/close). Not while `Calibrating`. |
| 8 | `KeypadWrongCode` | P | a new keypad log entry with completion `0xE0` (invalid code) or `9` (not authorised) |
| 9 | `KeypadValidEntry` | P | a valid keypad entry (MQTT `lockActionEvent` or keypad log entry with completion `Success`, deduplicated) |
| 10 | `Unlocked` | S | lock state in {Unlocked, UnlockedLnga, Unlatched} |
| 11 | `NightMode` | S | `nightModeActive == 1` (BLE only) |

### Defaults (master switch on, relay has no stored role)
| Relay | Role | Invert | Why |
|---|---|---|---|
| 1 | `WebhookPulse` | no | the intercom door-open button (rule "Fob · Double Right -> Relay 1") |
| 2 | `Secure` | no | closed only when confirmed. A reboot or unknown state reads "not secure". |
| 3 | `DoorOpen` | no | |
| 4 | `Locked` | no | |
| 5 | `LockFault` | **yes** | closed = lock OK. A reboot, power loss or cut wire reads as a fault (supervised). |
| 6 | `BatteryLow` | **yes** | closed = all batteries OK (same reasoning) |
| 7 | `DoorSensorFault` | **yes** | closed = sensor OK (tamper loop style) |
| 8 | `KeypadWrongCode` | no | 3 s pulse |

One `constexpr` array holds the defaults; tests, the page and "Reset relays to
defaults" use it.

## State source: MQTT first, BLE otherwise
The lock pushes its state in real time over Nuki's official MQTT API to the
built-in server (`LockMqttServer.cpp`, `ConnEvents::onPublish`, line ~258):
`state`, `doorsensorState`, `batteryCritical`, `keypadBatteryCritical`,
`doorsensorBatteryCritical`, `lockActionEvent` (`action,trigger,authId,codeId,context`),
`commandResponse`, `connected`. Upstream hybrid mode then does a BLE key
turner state read after each MQTT state change.

- Each input field (lock state, door state, the three battery flags) keeps the
  value and time of its last report per source. The role logic uses the
  **newer** of the two: MQTT values count only while the lock's MQTT session is
  live (they are dropped when it ends); the BLE key turner state fills in
  whatever MQTT hasn't sent (and is the only source of completion status and
  night mode).
- **State stale** (for `LockFault`): stale unless **either**
  - the lock's MQTT session is live **and** the lock was last heard (any packet,
    `LockMqttServer::liveState().lastRxAgeMs`) within "Skip only if the lock was
    heard from within" (the same setting, default 330 s; the lock pings every
    300 s), **or**
  - the last successful BLE key turner state read is younger than
    `2 × lock state poll interval + 60 s` (default 3660 s).
  Before the first state since boot the relays aren't armed at all (below).
- **BLE comm error:** upstream's `OutputHighBluetoothCommError` pin is only high
  *during* retries (`NukiRetryHandler::retryComm`, `setCommErrorPins` lines
  37/71), a transient that would click a relay. For relays: the most recent
  `retryComm` (any BLE command to the lock) failed after all retries; cleared by
  the next successful one. One hook at the end of `retryComm` reports the
  result; no retry logic is duplicated.
- **Hooks** (guard with `#ifdef NUKI_HUB_WAVESHARE_8DI8RO`, one call each).
  `NukiNetworkLock`'s publish functions are inert in this build
  (`NUKI_HUB_NO_EXTERNAL_MQTT`), so nothing hooks there:
  - MQTT: `LockMqttServer.cpp` (fork file): each PUBLISH from the lock in
    `onPublish`, and the session end in `closeConn`/takeover.
  - `NukiWrapper::updateKeyTurnerState()` after a successful read (next to the
    door-sensor GPIO writes, `NukiWrapper.cpp:736`).
  - `NukiWrapper::updateAuthData()` after `_nukiLock.getLogEntries(&log)` and
    the sort (`:1035` and `:1056`). The byte layout is the one
    `NukiNetworkLock::publishAuthorizationInfo` decodes (`NukiNetworkLock.cpp:742`
    keypad: `data[1]` source, `data[2]` completion, `data[3..4]` code ID;
    `:781` door sensor `data[0]` 0/1/2 = opened/closed/jammed).
  - `NukiRetryHandler::retryComm()` end (BLE comm error).
  - Info page: next to `WaveshareBoard::printBleEvents` in `WebCfgServer.cpp`.
- **Battery bits:** BLE `criticalBatteryState & 1` (lock); accessory byte
  decoded like `NukiNetworkLock.cpp:538` (keypad: bits 0+1) and `:557` (door
  sensor: bits 2+3); MQTT `...BatteryCritical` = `true`/`false`.

## Keypad
- **Valid entries** appear over MQTT as `lockActionEvent` with a Code-ID
  (Nuki MQTT API: `LockAction,Trigger,Auth-ID,Code-ID,context`, context for the
  keypad 0 = back key, 1 = code, 2 = fingerprint; e.g. `3,0,54321,12345,1`).
  `KeypadValidEntry` fires on Code-ID > 0 with context 1 or 2. The keypad log
  entry for the same entry arrives later over BLE and is deduplicated by code ID
  (an MQTT entry consumes the next matching log entry within 30 min). Without
  MQTT the log entry fires it.
- **Wrong codes** change no lock state and are not in `lockActionEvent` by
  specification. (Nuki confirmed a firmware bug on 4th/5th gen that publishes
  `1,0,<authId>,0,1` for a wrong code; not used, it may be fixed any time.) They
  still need the lock's **activity log over BLE** (`updateAuthData` →
  `getLogEntries`): keypad log entry (`LoggingType::KeypadAction`, any source
  byte) with completion `0xE0` or `9`.
- **NFC and Apple Home Key** (newest keypad): not documented in MQTT API 1.6 /
  BLE API 2.3.0 as far as this fork knows. The keypad log is decoded
  source-agnostic (any source byte counts), so an NFC entry logged as a keypad
  action with a valid/invalid completion is counted like a code. Home Key
  (HomeKit/Matter) entries are not keypad entries. Unverified: see the hardware
  list.
- A **new** log entry has an `index` higher than the highest seen. The first
  log read after a cold boot only records the highest index; the index lives in
  RTC_NOINIT memory, so a software reset doesn't replay entries. Only the newest
  "authlog max entries" are fetched per read; older ones in between are missed.
- Keypad roles and the page never depend on `hasKeypad()`; the page notes that
  Nuki Hub may not detect the newest keypad.

## Board specifics (from the current code)
- **Hardware:** 8 relays on a TCA9554 at I2C `0x20` (register `0x01` = output,
  `0x03` = config; relay N = bit N-1), bus SDA 42 / SCL 41 at 100 kHz. The same
  bus carries the PCF85063 RTC at `0x51`, written from the tcpip task after NTP
  syncs. **The bus is started once in `WaveshareBoard::earlyInit()` and stays
  up. Never call `Wire.begin()`/`Wire.end()` anywhere else.**
- **Every relay write goes through `setRelay()`** (`WaveshareBoard.cpp:70`): it
  holds `relayLock` and keeps the `relayOutputs` shadow register. Add public
  `WaveshareBoard::setRelayState(int relay, bool on)` and a masked variant that
  sets several relays in **one** I2C write. Write only when the target differs
  from the shadow, so re-evaluating doesn't hit I2C. Steady writes skip relays
  that are pulsing.
- **Pulses:** `pulseRelay()` (`:277`) was "one task only (httpd)" because its
  esp_timer was created lazily. Create all 8 off-timers in `earlyInit()` (after
  the mutex), drop the lazy path, fix the comment. The off-timer restores the
  relay's rest level (open, or closed for an inverted pulse role). `relayOff()`
  retries until the write succeeds (every 100 ms). Keep that.
- **Power-up order is load-bearing:** `earlyInit()` writes output `0x00`
  *before* config `0x00` (the TCA9554 powers up latched high with no reset
  line). Don't touch that order. Relays are all open until the first lock state
  after boot (MQTT `state` or a successful BLE key turner state read), and only
  then take their role state. Inverted roles too, so "OK" contacts don't close
  before anything is known.
- **Every BLE error and every 60 s beacon loss reboots the board** (upstream
  behaviour, measured by the M6 log). Relays drop on each reboot for the boot
  time plus the first state. That's why the fault roles default to inverted.
  Note it in `WAVESHARE.md`; don't change the reboot policy here (pending user
  decision).
- **Webhook conflict, fail closed:** settings are live now (no route
  registration per rule), so: saving refuses a rule with `relayN` whose relay
  isn't `WebhookPulse` (with the master on), and a role change that would
  orphan an enabled rule; the webhook handler checks again per press and
  refuses (`500 error`, log line says why) instead of pulsing a state relay.
  With the master switch off all relays behave as `WebhookPulse`.
- **Upstream GPIO roles can't reach the relays.** `Gpio`/`PinRole` only drive
  native ESP32 pins. Don't add relays to `Gpio::_availablePins` or `PinRole`.
  New code in `RelayOutputsLogic.h` (pure) and `WaveshareOutputs.cpp/.h`.
  DI1-8 (GPIO 4-11) stay in upstream's GPIO config as inputs. GPIO 0, 38, 41,
  42, 46 stay reserved (`Gpio::getDisabledPins`).

## Concurrency
Inputs arrive from the nuki task (BLE, core 0), the `lockmqtt` task (core 1)
and httpd (settings save). Each hook only copies values into a small input
struct under a spinlock and wakes one worker task (`relayout`, created when the
master switch is first on); the worker evaluates the roles and does the I2C
write (one masked write for all steady relays, plus pulses). So the lock MQTT
task never touches I2C, and steady outputs have a single writer. The worker
also wakes every second for the stale rule. Webhook pulses stay in httpd, as
today; `relayLock` serializes all writes and the pulsing mask.

## Prerequisites for the log roles
The PIN is valid now (fa2159e: periodic queries run without external MQTT) and
"Publish auth data" is on in the user's build. The page still warns next to a
log role (`KeypadWrongCode`, `KeypadValidEntry`, `DoorSensorFault`'s jam part)
if either is missing:
- `preference_publish_authdata` must be on, or `updateAuthData` never runs
  (`NukiWrapper.cpp:714`). When a log role is assigned with the master on, it is
  switched on at boot (before `NukiWrapper` reads it) and on save (then it
  applies after a reboot; the page says so).
- A valid lock PIN (`isPinValid()`).

## Latency
With the lock connected over MQTT, lock and door changes arrive within
milliseconds; the worker writes the relay right after. The lock state poll
(1800 s by default) matters only without MQTT (an app-paired Ultra sets no
"state changed" beacon flag, see `FINDINGS.md`) and for BLE-only fields. Don't
change the poll default; show it next to the relay settings ("each poll wakes
the lock; lower = faster BLE-only outputs, more battery").

| Role | Source | With MQTT | Without MQTT |
|---|---|---|---|
| Secure, DoorOpen, Locked, Unlocked | MQTT state/doorsensorState, else BLE | < 1 s | next poll (≤ 1800 s) |
| BatteryLow | MQTT battery topics, else BLE | < 1 s (when the lock publishes) | next poll |
| LockFault: motor blocked state | MQTT state | < 1 s | next poll |
| LockFault: completion status | BLE key turner state | ~1-3 s (hybrid BLE read after each MQTT state) | next poll |
| LockFault: stale / BLE error | timers, retry handler | ≤ 1 s after the condition | same |
| NightMode | BLE key turner state | at the next BLE read (after a state change, or poll) | next poll |
| DoorSensorFault: state | MQTT doorsensorState, else BLE | < 1 s | next poll |
| DoorSensorFault: jammed | activity log | next log read | next log read |
| KeypadValidEntry | MQTT lockActionEvent, else log | < 1 s | next log read |
| KeypadWrongCode | activity log only | next log read | next log read |

A log read happens after a BLE key turner state read that finds the lock
Locked or Unlocked (i.e. after the next lock state change, or at the poll). A
wrong code changes nothing, so it can take up to one poll interval. Say so in
the page help text.

## Tests
- Pure header `RelayOutputsLogic.h` (pattern: `ProtectWebhookLogic.h`): role +
  invert + inputs → relay on/off; source merge (newer wins, MQTT only while
  live); stale rule; battery bit decoding; `lockActionEvent` parsing and the
  keypad-entry rule; log tracker (baseline, new index, completion codes, jam
  latch); MQTT/log dedupe; defaults table; "rule targets a non-WebhookPulse
  relay" check.
- `ForkSettingsLogic.h`: v4 round trip, **v3 blob loads** with outputs off and
  the migrated pulse, relay validation (range, invert on WebhookPulse, orphaned
  rule), "unchanged default stays unstored".
- Unity tests in `test/` (`pio test -e native`); CI already runs them.
- Build `esp32-s3-waveshare-8di8ro`. A board with no stored roles and the
  master off must behave as before.

## Docs
- `WAVESHARE.md`: relay roles table, defaults, polarity/fail-safe reasoning,
  latency, the reboot caveat, "relay contacts are dry contacts only".
- `SETUP.md`: a short optional step, "Relay state outputs", with the PIN and
  auth-log prerequisites.
- `PROTECT_SETUP.md`: relay rules with the master on (only `WebhookPulse`
  relays), per-relay pulse.
- `HANDOFF.md`: update the open items when done.

## User must verify on hardware
1. Latency with MQTT: turn the knob by hand, open/close the door: relays 2-4
   follow within a second. Then switch the lock's MQTT off in the app and check
   the fallback (next poll).
2. No relay clicks during several PoE power cycles and reflashes, with the
   master switch off and on.
3. After a web-UI reboot, all relays stay open until the first state, then take
   their roles. Inverted roles don't close early.
4. Keypad: wrong code → relay 8 pulses after the next log read (note how long);
   valid code → no relay 8 pulse. If `KeypadValidEntry` is assigned, a valid
   code pulses once (not twice). Also NFC tag and Home Key: which roles fire.
5. Door open/close → relays 2/3; pull the door-sensor battery or tamper it →
   relay 7 opens.
6. A rule pointing at a non-`WebhookPulse` relay can't be saved; relay 1 still
   pulses for "Fob · Double Right".
7. Keypad detection: Info page "Has keypad" and the page's "keypad battery
   state reported" line; `lockActionEvent` values for code, NFC, Home Key.

## Out of scope
- Changing the BLE-reboot policy (M6), the poll interval default, or hybrid mode.
- Anything on the ESP32's native pins.
