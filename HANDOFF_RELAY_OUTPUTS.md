# Handoff: relay state outputs on `waveshare-8di8ro`

**Repo** https://github.com/napieraj/nuki_hub, branch `waveshare-8di8ro`, written at `91a0766`.
Read `HANDOFF.md` first (ground rules, build, code map). This file adds one feature.

## Goal
Drive the board's 8 relays from lock, keypad and door-sensor state, so they can
feed dry contacts into something else (alarm panel zones, indicator lamps; the
user hasn't said which yet, so keep polarity configurable).

Setup: one Nuki Smart Lock Ultra (paired as app, BLE only), one Nuki Keypad
(2) and one Nuki Door Sensor, both paired to the lock. The board reads
keypad and door-sensor state **through the lock** (key turner state and the
lock's activity log). It never talks to them directly.

## The rule for this task: configurable roles, not hard-coded
The fork settings web UI is already implemented (your work). Build on it:

- **Every relay gets a role selected in that UI** and stored in NVS. No
  relay-to-signal mapping in code or in `ProtectWebhookConfig.h`.
- **A master switch** ("Relay state outputs", default **off**). With it off,
  nothing changes from today: relays only move for webhook `relayN` rules.
- **Defaults below apply only when the switch is on AND the relay has no
  stored role.** Never overwrite a role the user chose. Offer a "Reset relays
  to defaults" button that does overwrite, after a confirmation.
- Roles are an `enum class` with explicit, stable numeric values. **Append
  only**, never renumber: the numbers are what NVS stores. An unknown stored
  value reads as `Off`.
- Per relay, next to the role: **Invert** (checkbox) and, for pulse roles,
  **Pulse ms** (default 3000, 100-60000).
- NVS keys ≤ 15 chars, fork prefix, e.g. `wsRlyOut` (master), `wsRly1Role`,
  `wsRly1Inv`, `wsRly1Pls`. Add them to `PreferencesKeys.h` under the board
  flag, and to the config export/import lists, like the other fork settings.
- Show each relay's role and live state (closed/open) on the Info page.

## Roles
"Closed" means the relay coil is energized (COM-NO closed). **Invert** flips it.
`S` = steady (follows state), `P` = pulse (closes for Pulse ms on an event).

| Value | Role | Kind | Closed when |
|---|---|---|---|
| 0 | `Off` | – | never |
| 1 | `WebhookPulse` | P | a Protect webhook rule with action `relayN` fires (today's behaviour) |
| 2 | `Secure` | S | `lockState == Locked` **and** `doorSensorState == DoorClosed` |
| 3 | `DoorOpen` | S | `doorSensorState == DoorOpened` |
| 4 | `Locked` | S | `lockState == Locked` (not `Locking`; upstream's GPIO role counts `Locking` too, relays shouldn't click on a transient) |
| 5 | `LockFault` | S | any of: `lockState == MotorBlocked`; `lastLockActionCompletionStatus` in {MotorBlocked, LowMotorVoltage, ClutchFailure, MotorPowerFailure, IncompleteFailure, Failure}; BLE comm error (see below); **state stale** (see below) |
| 6 | `BatteryLow` | S | lock critical (`criticalBatteryState & 1`) or keypad critical or door sensor critical. Decode the accessory bits exactly like `NukiNetworkLock.cpp:535` (keypad) and `:554` (door sensor). |
| 7 | `DoorSensorFault` | S | `doorSensorState` in {DoorStateUnknown, Uncalibrated, Tampered}, or the last door-sensor log entry is `SensorJammed` (latched until the next DoorOpened/DoorClosed state). Not while `Calibrating`. |
| 8 | `KeypadWrongCode` | P | a new `KeypadAction` log entry with completion `0xE0` (invalid code) or `9` (not authorised) |
| 9 | `KeypadValidEntry` | P | a new `KeypadAction` log entry with completion `Success` |
| 10 | `Unlocked` | S | `lockState` in {Unlocked, UnlockedLnga, Unlatched} |
| 11 | `NightMode` | S | `nightModeActive == 1` |

A **new** log entry is one whose `index` is higher than the highest index seen
since boot. On the first log read after boot, record the highest index and
don't fire for old entries. The index goes in RTC_NOINIT memory, like the
BLE event log, so a software reset doesn't replay the last entry.

**State stale:** no successful `updateKeyTurnerState()` for longer than
`2 × lock state poll interval + 60 s`.
**BLE comm error:** the same condition that drives upstream's
`OutputHighBluetoothCommError` (`NukiRetryHandler::setCommErrorPins`,
`util/NukiRetryHandler.cpp:34/49`). Expose it through a callback or a getter
there, guarded by the board flag. Don't duplicate the retry logic.

### Defaults (master switch on, relay has no stored role)
| Relay | Role | Invert | Why |
|---|---|---|---|
| 1 | `WebhookPulse` | no | already planned: intercom door-open button (double press Right) |
| 2 | `Secure` | no | closed only when confirmed. A reboot or unknown state reads "not secure". |
| 3 | `DoorOpen` | no | |
| 4 | `Locked` | no | |
| 5 | `LockFault` | **yes** | closed = lock OK. A reboot, power loss or cut wire reads as a fault (supervised). |
| 6 | `BatteryLow` | **yes** | closed = all batteries OK (same reasoning) |
| 7 | `DoorSensorFault` | **yes** | closed = sensor OK (tamper loop style) |
| 8 | `KeypadWrongCode` | no | 3 s pulse |

Relays 2-8 are the seven state outputs. Put the defaults table in one
`constexpr` array so tests and the UI's reset button use the same source.

## Board specifics to merge in (don't re-derive; all from the current code)
- **Hardware:** 8 relays on a TCA9554 at I2C `0x20` (register `0x01` = output,
  `0x03` = config; relay N = bit N-1), bus SDA 42 / SCL 41 at 100 kHz. The same
  bus carries the PCF85063 RTC at `0x51`, written from the tcpip task after NTP
  syncs. **The bus is started once in `WaveshareBoard::earlyInit()` and stays
  up. Never call `Wire.begin()`/`Wire.end()` anywhere else.**
- **Every relay write goes through `setRelay()`** (`WaveshareBoard.cpp:34`): it
  holds `relayLock` and keeps the `relayOutputs` shadow register. Add a public
  `WaveshareBoard::setRelayState(int relay, bool on)` wrapping it. Write only
  when the target differs from the shadow, so a 30 s poll doesn't hit I2C.
- **`pulseRelay()` is documented "one task only (httpd)"** because its esp_timer
  is created lazily. Keypad pulse roles will call it from the nuki task
  (core 0). Create all 8 off-timers in `earlyInit()` (after the mutex) and drop
  the lazy path, then fix the comment. `relayOff()` retries 3×. Keep that.
- **Power-up order is load-bearing:** `earlyInit()` writes output `0x00`
  *before* config `0x00` (the TCA9554 powers up latched high with no reset
  line). Don't touch that order. Relays are all open until the first
  **successful** key turner state read after boot, and only then take their
  role state. Apply inverted roles only then too, so "OK" contacts don't close
  before anything is known.
- **Every BLE error and every 60 s beacon loss reboots the board** (upstream
  behaviour, measured by the M6 log). Relays drop on each reboot for the boot
  time plus the first state read. That's why the fault roles default to
  inverted. Note it in `WAVESHARE.md`; don't change the reboot policy here
  (pending user decision).
- **Webhook conflict, fail closed:** a Protect rule with action `relayN` may only
  target a relay whose role is `WebhookPulse`. Check it in
  `ProtectWebhook::registerRoute` (the relay branch, `ProtectWebhook.cpp:177`):
  a mismatch logs the rule and disables the route, as unknown actions do
  today. Also refuse to save a role change in the UI that would orphan a
  configured webhook rule. With the master switch off, all relays behave as
  `WebhookPulse` (today's behaviour).
- **Upstream GPIO roles can't reach the relays.** `Gpio`/`PinRole` only drive
  native ESP32 pins. Don't add relays to `Gpio::_availablePins` or `PinRole`.
  Keep this separate (new `WaveshareOutputs.cpp/.h`, or inside
  `WaveshareBoard.*`). DI1-8 (GPIO 4-11) stay in upstream's GPIO config as
  inputs. GPIO 0, 38, 41, 42, 46 stay reserved (`Gpio::getDisabledPins`).
- **Hook points** (guard with `#ifdef NUKI_HUB_WAVESHARE_8DI8RO`, one call each):
  - `NukiWrapper::updateKeyTurnerState()`, next to the door-sensor GPIO
    writes (~`NukiWrapper.cpp:666`): pass `_keyTurnerState` (and
    success/failure, for the stale timer).
  - Keypad and door-sensor log events: in `NukiWrapper::updateAuthData()` after
    `_nukiLock.getLogEntries(&log)` (~`:966` and ~`:987`). Don't parse in
    `NukiNetworkLock::publishAuthorizationInfo`: that's the MQTT publisher, and
    this build has no MQTT. The byte layout is decoded there, though
    (`NukiNetworkLock.cpp:739-790`: `data[1]` source, `data[2]` completion,
    `data[3..4]` code ID; door sensor `data[0]` 0/1/2 = opened/closed/jammed).
  - Stale timer: check it on the nuki task's loop (it already has a heartbeat
    for L13).
- **Log reads need two things.** Show a warning in the UI next to any relay with a
  keypad or door-sensor-log role if either is missing:
  - `preference_publish_authdata` must be on, or `updateAuthData` never runs
    (`NukiWrapper.cpp:644`). When a log role is assigned, force it on in
    `applyDefaults` (only for that case).
  - A **valid lock PIN** (`isPinValid()`). The user's log currently says "No valid
    Nuki Lock PIN set", so this is a real gap, not a hypothetical.
- **Latency: the thing to measure.** `FINDINGS.md`/`WAVESHARE.md` say an
  app-paired Ultra sets no "state changed" beacon flag, so changes made
  outside the board (keypad, knob, auto-lock) show only at the next lock state
  poll, **1800 s by default**. The code does react to the flag if it's set
  (`NukiWrapper::notify`, "KeyTurnerStatusUpdated"). Don't change the poll
  interval default. Show it next to the relay settings with a note ("each
  poll wakes the lock; lower = faster outputs, more battery"). Keypad
  wrong-code events change no lock state, so they're only seen at the next log
  read. Say so in the UI help text.

## Tests
- Put the evaluation in a pure header (pattern: `ProtectWebhookLogic.h`):
  role + invert + a plain struct of the fields above → relay on/off; battery
  bit decoding; log entry → pulse or not (new index, completion codes); the
  stale calculation; defaults table; "rule targets a non-WebhookPulse relay" check.
- Unity tests in `test/` (`pio test -e native`), next to the existing 15. CI
  (`.github/workflows/waveshare.yml`) already runs them.
- Build `esp32-s3-waveshare-8di8ro` with and without the master switch
  defaulting on. Neither may change behaviour for a board with no stored roles.

## Docs
- `WAVESHARE.md`: the relay roles table, defaults, polarity/fail-safe
  reasoning, latency, the reboot caveat, and "relay contacts are dry contacts
  only".
- `SETUP.md`: a short optional step, "Relay state outputs", with the PIN and
  auth-log prerequisites.
- `HANDOFF.md`: update the open items when done.

## User must verify on hardware
1. Latency: turn the knob by hand and use the keypad. Does the log print
   `KeyTurnerStatusUpdated` within seconds? If not, outputs lag by up to one poll.
2. No relay clicks during several PoE power cycles and reflashes (still open
   from the relay feature), now also with the master switch on.
3. After a web-UI reboot, all relays stay open until the first state read, then
   take their roles. Inverted roles don't close early.
4. Wrong keypad code → relay 8 pulses (after the next log read); valid code
   doesn't.
5. Door open/close → relays 2/3; pull the door-sensor battery or tamper it → relay 7 opens.
6. A webhook rule pointing at a non-`WebhookPulse` relay disables the route
   and the log says why.

## Out of scope
- Changing the BLE-reboot policy (M6), the poll interval default, or Hybrid mode.
- Anything on the ESP32's native pins.
