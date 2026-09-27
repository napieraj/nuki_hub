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
the alarm. `unlatch` works too (knob-only lock); it is never sent twice, so on
a bad BLE link it may fail without a retry (see section 4).

### 1. Confirm the payload once (5 minutes)
1. On a laptop: `scripts/protect_webhook_test.py listen --port 8099`
2. Protect → Alarm Manager → Create Alarm: Trigger **Button**, gesture
   **Long Press (3s)**, scope the fob's **Right** button. Action: Custom Webhook →
   `http://<laptop-ip>:8099/capture`, Advanced → **POST**.
3. Hold Right for 3 s. The JSON should match the example above. Note the fob MAC in `device`.
4. Press Right briefly, and press Left: **nothing may arrive**. That proves
   Protect filters by button and gesture.
5. If the fields differ from the example, adjust the rule's `key`/`field`/`value` to what you see.

### 2. Configure the board (web UI)
Everything is set in Nuki Hub → **Protect Webhook & Relays** and stored on the
board (NVS). Changes apply immediately, no reboot or reflash. Set a web UI
password (and TOTP or Duo) under **Credentials** first: this page decides who
can open the door, and it shows a big red warning while the web UI has no password.

- **Secret**: press *Generate random* (or paste `openssl rand -hex 32`), copy it
  into Protect (step 3) before saving. The secret and the tokens are
  **write-only**: after saving, the page only shows "set, 64 characters". An
  empty field keeps the stored value; *Clear* removes it.
- **Source IP**: the UniFi OS console.
- **Rules** (12 slots): one per alarm, each with its own token (*Generate random*,
  or `openssl rand -hex 16`) **and** the key/button/gesture checks, e.g. key
  `sensor_button_pressed`, device `<fob MAC>`, field `button` = `right`,
  field 2 `value` = `longPress`, action `unlock`. One fob per person, so the fob
  is the identity. Actions: the Nuki lock actions, or `relay1`..`relayN` (N =
  "Relays available to rules").
- **Webhook enabled**: the master switch. The status box at the top says whether
  the webhook is active and, if not, why (no secret, invalid rule, switched off),
  plus the clock, the BLE task and the last 10 requests.

Saving checks everything and refuses with a message if the secret (≥ 32
characters) or a token (≥ 16, URL-safe) is too short or still a placeholder, a
rule lacks a device (12 hex digits) or action, `value` lacks `field`, `value2`
lacks `field2`, or a rule would match any event from a fob (neither token nor
key + value; see *Allow broad rules*). Disabled rules are drafts and not checked.
Timings: clock skew 1–120 s (default 15 s), cooldown 1–600 s (10 s), action
deadline 1–60 s (8 s), BLE stall 5–600 s (30 s), relay pulse 100–30000 ms (3 s).

**Hardening options** (all off by default):
| Option | What it does |
|---|---|
| Bearer only | Refuses the secret as `?k=` in the URL (`403 denied`); Protect must use Auth = Bearer. Keeps the secret out of URLs and logs. |
| Require source IP | Saving refuses to enable the webhook while the source IP is empty. |
| Allow broad rules | Allows a rule with neither token nor key + value (it matches **any** event of that fob). Leave off. |
| Require a TOTP code for every save | The page asks for a current TOTP code on every save (and on "switch off"). Needs TOTP enabled under Credentials. |
| Settings lock (DI1–DI8) | Saving only works while that digital input is active (contact closed to DGND). Pick a DI with **no** role in GPIO Configuration; you must hold it active while turning the lock on (proves the wiring). While locked the page is read-only, except *Switch the webhook off now*. |

Recovery (USB-C serial console, 115200 baud, physical access): `forkcfg unlock`
turns off the settings lock and the TOTP requirement; `forkcfg off` switches
the webhook off. Nuki Hub's own **Credentials → Require MFA (Duo/TOTP) for all
sensitive operations** also covers this page (with Duo, approve, then save again).

**Not in export/import:** the settings live in their own NVS namespace
(`forkcfg`), so Nuki Hub's configuration export never contains the secret or
tokens, and an import doesn't change them. Nuki Hub's factory reset erases them.

**Optional compile-time seed:** `src/ProtectWebhookConfig.h` (copy of
`src/ProtectWebhookConfig.h.example`) is no longer needed. If it exists, its
values are stored once, on the first boot without saved settings (fresh board,
after an erase or factory reset); after that the web page wins and the file is
ignored. The build still refuses placeholders and broken rules in it. Without
it the webhook starts switched off until you configure it on the page.

In Nuki Hub → **Nuki Lock Access Control**, allow exactly the actions your rules use.

### 3. Point the alarms at the board
One alarm per rule (e.g. "Fob A - Hold Right", "Fob A - Press Right"):
- Trigger: Button, the gesture, scope the fob's button
- Custom Webhook: `http://<board-ip>/protect?r=<that rule's token>`, Method **POST**
- Auth: **Bearer**, token = the secret from the web page. This keeps the secret out
  of the URL; `&k=<secret>` in the URL also works unless *Bearer only* is set.
- Leave "Ignore repeated actions" **off**; the board has its own cooldown per
  rule (per alarm, default 10 s), so "hold to unlock" then "press to lock" works right away.
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
  (the action deadline on the web page) is dropped and the log shows
  `Lock action expired`, so the door never opens long after the press.
- A failed lock action is retried (Nuki Hub's retry setting), except an
  unlatching one (`unlatch`, `lockNgoUnlatch`, fob actions): a timeout may mean
  the lock got the command but its reply was lost, so a retry could open the door
  twice. It is retried only if it provably never reached the lock (not paired);
  otherwise the log shows `Unlatch: result ambiguous, not retrying` and MQTT
  `lock/retry` reads `ambiguous`. Check the lock state and press again if needed.

### Host tests
`pio test -e native` runs the rule matching, timestamp, replay and per-rule
cooldown logic (`src/ProtectWebhookLogic.h`) with the same cases as the bench
suite below, and the settings validation, storage format and form helpers
(`src/ForkSettingsLogic.h`), on your computer. CI runs it on every push.

### 5. Bench test without moving the lock
1. In **Nuki Lock Access Control**, untick the action the rule uses (and the
   second rule's action, if you pass `--token2`). If it stays ticked, the
   "valid press" case really moves the lock; the tool asks unless you pass
   `--acl-disabled`.
2. Allow your laptop as source: run from a host with the console's IP, or
   temporarily set the source IP on the web page to the laptop's IP (it
   applies at once). Set it back to the console afterwards.
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
   changed the cooldown on the web page. The page's "Recent webhook requests" lists each result.
4. Tick the action again, hold the real fob button, and watch the Nuki Hub
   log (USB-C serial) for `Protect webhook: unlock -> queued`.

### Losing a fob
Remove it in Protect **and** disable or delete its rules on the web page (applies
at once). Either one alone stops it; do both. Rotating a rule's token also kills
that alarm's URL. In a hurry: *Switch the webhook off now* on the page, or
`forkcfg off` on the USB console.

### Not used: the Integration API event stream
`wss://<console>/proxy/protect/integration/v1/subscribe/events` delivers
`sensorButtonPressed` with the button but **without the gesture**, and the board
would have to hold a Protect API key. The webhook keeps credentials off the lock board.

### Response codes
| Code | Result | Meaning |
|---|---|---|
| 200 | `ack` | Queued for the lock task; for a `relayN` rule: relay closed, it opens after the relay pulse time |
| 400 | `bad_json` / `no_triggers` | Body not a Protect alarm |
| 403 | `disabled` | Webhook switched off or its settings invalid (the web page says why) |
| 403 | `denied` | Wrong source IP or secret, or `?k=` with *Bearer only* |
| 403 | `no_match` | No rule matched (token/fob/key/button/gesture), or the event was stale |
| 403 | `acl_denied` | Matched, but the action is disabled in Nuki Hub's ACL |
| 413 | `bad_size` | Empty or > 4 KB body |
| 429 | `cooldown` | Replayed event, or within the cooldown (default 10 s) since the last accepted event for the same rule. Editing a rule resets its cooldown. |
| 500 | `error` | Lock action couldn't be queued, or the relay's I2C write failed (a retry then gets `429`) |
| 503 | `no_time` | Clock not synced yet (Protect retries twice) |
| 503 | `busy` | Another lock action is still queued or being sent, or settings are being saved (Protect retries twice) |
| 503 | `ble_stalled` | The BLE (nuki) task hasn't run for the BLE stall time (default 30 s); the board reboots right after replying (also sent briefly after boot, before BLE starts) |
