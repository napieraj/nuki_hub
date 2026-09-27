## UniFi SuperLink KeyFob → Protect → Nuki Hub: setup and bench test

For builds with `NUKI_HUB_PROTECT_WEBHOOK` (e.g. `esp32-s3-waveshare-8di8ro`).
All values below are placeholders. Background and sources: `FINDINGS.md`.

```
USL-FOB ──SuperLink──▶ SuperLink Gateway ─▶ Protect Alarm Manager
   alarm: Button / Long Press (3s) / scope fob:right
        ──POST http://<board>/protect?r=<token>, Authorization: Bearer <secret>──▶ Nuki Hub ──BLE──▶ Nuki lock
```

### Requirements
- SuperLink Gateway (USL-Gateway or USL-G2-Gateway-HA), UniFi OS ≥ 5.1.11,
  Protect ≥ 7.1.60, fob firmware ≥ 1.4.0.
- Adopting SuperLink puts Alarm Manager in **Global** mode. The webhook body is the same in both modes.
- The board on a routable LAN IP, reachable over **plain HTTP**. Protect rejects
  self-signed HTTPS and does not follow redirects, so **do not enable HTTPS in Nuki Hub**
  (it would turn port 80 into a redirect).

### What a press looks like
From the UniFi Protect 7.2.105 source (not yet confirmed by a live capture):
```json
{"alarm":{"name":"Fob A - Hold Right",
          "sources":[{"device":"AABBCCDDEEFF","type":"include","buttons":["right"]}],
          "conditions":[{"condition":{"type":"is","source":"sensor_button_pressed","value":"longPress"}}],
          "triggers":[{"key":"sensor_button_pressed","value":"longPress","device":"AABBCCDDEEFF",
                       "button":"right","eventId":"…","timestamp":1790000000000}]},
 "timestamp":1790000000000}
```
- `button`: `arm` (top left), `night` (top right), `disarm` (bottom left),
  `panic` (bottom right), `left`, `right`
- `value`: `press`, `longPress` (the app's **Hold**, 3 s), `doublePress`

Suggested mapping: **Hold Right = unlock**, **Press Right = lock**. A 3 s hold
is hard to trigger by accident in a pocket, and the face buttons stay free for
the alarm. Use `unlock`, not `unlatch`: a timed-out command is retried, and a
second unlock is harmless where a second unlatch is not.

### 1. Confirm the payload once (5 minutes)
1. On a laptop: `scripts/protect_webhook_test.py listen --port 8099`
2. Protect → Alarm Manager → Create Alarm: Trigger **Button**, gesture
   **Long Press (3s)**, scope the fob's **Right** button. Action: Custom Webhook →
   `http://<laptop-ip>:8099/capture`, Advanced → **POST**.
3. Hold Right for 3 s. The JSON should match the example above. Note the fob MAC in `device`.
4. Press Right briefly, and press Left: **nothing may arrive**. That proves
   Protect filters by button and gesture.
5. If the fields differ from the example, adjust the rule's `key`/`field`/`value` to what you see.

### 2. Configure the board
Copy `src/ProtectWebhookConfig.h.example` to `src/ProtectWebhookConfig.h`:
- `PROTECT_WEBHOOK_SECRET`: `openssl rand -hex 32`
- `PROTECT_WEBHOOK_SOURCE_IP`: the UniFi OS console
- `PROTECT_WEBHOOK_RULES`: one line per alarm, with its own token (`openssl rand -hex 16`)
  **and** the key/button/gesture checks, e.g.
  `{ "<token>", "sensor_button_pressed", "<fob MAC>", "button", "right", "value", "longPress", "unlock" }`.
  One fob per person, so the fob is the identity.

The build checks the config and stops with a clear error if the secret or a token
is still a placeholder or too short, a rule lacks a device or action, or a rule
would match any event from a fob. At boot, an unknown action disables `/protect`
(logged). **Existing config from before this check:** change
`static const ProtectRule PROTECT_WEBHOOK_RULES[]` to `static constexpr ...`, and
optionally add `#define PROTECT_WEBHOOK_ACTION_DEADLINE_MS 8000`.

In Nuki Hub → **Nuki Lock Access Control**, allow exactly the actions your rules use.

### 3. Point the alarms at the board
One alarm per rule (e.g. "Fob A - Hold Right", "Fob A - Press Right"):
- Trigger: Button, the gesture, scope the fob's button
- Custom Webhook: `http://<board-ip>/protect?r=<that rule's token>`, Method **POST**
- Auth: **Bearer**, token = `PROTECT_WEBHOOK_SECRET`. This keeps the secret out
  of the URL; `&k=<secret>` in the URL also works.
- Leave "Ignore repeated actions" **off**; the board has its own 10 s cooldown per
  rule (per alarm), so "hold to unlock" then "press to lock" works right away.
- Don't attach the alarm to an arm profile, so it works armed or disarmed.

### 4. Network
- Board on its own VLAN. Firewall: allow **only** `<console-ip> → <board-ip>:80`;
  web UI from your admin host only; turn on Duo or TOTP in Nuki Hub.
- The board needs NTP. It refuses events until its clock is set (`503 no_time`).
  It uses the NTP server from DHCP (option 42) first, then the "NTP server" set in
  Nuki Hub (default `pool.ntp.org`). Allow UDP 123 from the board to that server.
  On an isolated VLAN with neither, **every** press gets `503 no_time`.
  After a software reset (web UI reboot, watchdog, BLE restart) the clock and the
  "synced" state are kept, so presses work right away. Every NTP sync also stores
  the time in the board's PCF85063 RTC; with a charged **ML1220** cell in the RTC
  holder, the board restores it after a power cut too. Without the cell it waits
  for NTP again.
- Protect retries on 5xx/408 only (1 s, then 2 s). A retry of an accepted press
  gets `429 cooldown`, so it never runs twice. If the board is offline, the press is lost (Protect has no queue).
- `200 ack` means *queued*, not *done*. If the lock can't be reached (out of BLE
  range, busy), a queued action that hasn't been sent within 8 s
  (`PROTECT_WEBHOOK_ACTION_DEADLINE_MS`) is dropped and the log shows
  `Lock action expired`, so the door never opens long after the press.

### Host tests
`pio test -e native` runs the rule matching, timestamp, replay and per-rule
cooldown logic (`src/ProtectWebhookLogic.h`) on your computer, with the same
cases as the bench suite below. CI runs it on every push.

### 5. Bench test without moving the lock
1. In **Nuki Lock Access Control**, untick the action the rule uses (and the
   second rule's action, if you pass `--token2`). If it stays ticked, the
   "valid press" case really moves the lock; the tool asks unless you pass
   `--acl-disabled`.
2. Allow your laptop as source: run from a host with the console's IP, or
   flash a bench build with `PROTECT_WEBHOOK_SOURCE_IP` set to the laptop.
   Do not keep that build.
3. Run:
   ```
   scripts/protect_webhook_test.py suite --url http://<board-ip>/protect --bearer \
     --secret <secret> --token <rule token> --device <fob MAC> \
     --key-checked --field button --value right --field2 value --value2 longPress \
     --token2 <press-right token> --rule2-value2 press --acl-disabled
   ```
   Expected: **17/17 PASS** (16 without `--token2`). Rejections cover a wrong
   secret, bad JSON, missing triggers, an oversized body, another fob, stale and
   future timestamps, a stale legacy-shape payload, a wrong or missing alarm token,
   a wrong key, another button, another gesture, replay, and cooldown. The fully
   valid press returns **`403 acl_denied`**: it passed every check and only the
   ACL stopped it. The `--token2` case checks that another rule is accepted inside
   the first rule's cooldown (also `403 acl_denied`). Pass `--cooldown-ms` if you
   changed `PROTECT_WEBHOOK_COOLDOWN_MS`.
4. Tick the action again, hold the real fob button, and watch the Nuki Hub
   log (USB-C serial) for `Protect webhook: unlock -> queued`.

### Losing a fob
Remove it in Protect **and** delete its rules, then reflash. Either one alone
stops it; do both. Rotating a rule's token also kills that alarm's URL.

### Not used: the Integration API event stream
`wss://<console>/proxy/protect/integration/v1/subscribe/events` delivers
`sensorButtonPressed` with the button but **without the gesture**, and the board
would have to hold a Protect API key. The webhook keeps credentials off the lock board.

### Response codes
| Code | Result | Meaning |
|---|---|---|
| 200 | `ack` | Queued for the lock task |
| 400 | `bad_json` / `no_triggers` | Body not a Protect alarm |
| 403 | `denied` | Wrong source IP or secret |
| 403 | `no_match` | No rule matched (token/fob/key/button/gesture), or the event was stale |
| 403 | `acl_denied` | Matched, but the action is disabled in Nuki Hub's ACL |
| 413 | `bad_size` | Empty or > 4 KB body |
| 429 | `cooldown` | Replayed event, or < 10 s since the last accepted event for the same rule |
| 503 | `no_time` | Clock not synced yet (Protect retries twice) |
| 503 | `busy` | Another lock action is still queued or being sent (Protect retries twice) |
| 503 | `ble_stalled` | The BLE (nuki) task hasn't run for 30 s; the board reboots right after replying (also sent briefly after boot, before BLE starts) |
