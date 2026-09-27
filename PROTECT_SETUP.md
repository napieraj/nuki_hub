## UniFi SuperLink KeyFob → Protect → Nuki Hub: setup and bench test

For builds with `NUKI_HUB_PROTECT_WEBHOOK` (e.g. `esp32-s3-waveshare-8di8ro`).
All values below are placeholders.

```
USL-FOB ──SuperLink RF──▶ SuperLink Gateway ─▶ Protect Alarm Manager
        "Activity → Button"  ──POST /protect?k=…──▶ Nuki Hub ──BLE──▶ Nuki lock
```

The fob has four fixed top buttons (Arm, Disarm, Night, Panic) and two
programmable side buttons. Use a **side button** for the door so arming,
disarming or panic never moves the lock.

### 1. Capture one real press (required)
Protect does not document how a button press is encoded in the webhook body.
The board matches `alarm.triggers[]` on `key`, `device` and one field that
names the button, and all three come from your own capture:

1. On a laptop on the LAN: `scripts/protect_webhook_test.py listen --port 8099`
2. Protect → Alarm Manager → Create Alarm:
   - Trigger: **Activity → Button**, select the fob and the side button
   - Action: Webhook → Custom Webhook → `http://<laptop-ip>:8099/capture`,
     Advanced → Method **POST**
3. Press the button. From the printed JSON note:
   - `triggers[].key` → rule `key`
   - `triggers[].device` → rule `device` (the fob)
   - the field that identifies the button, and its value → rule `field` / `value`.
     If the button is only identifiable through a different structure than a
     flat field in the trigger, stop: the parser needs extending first.
   - `triggers[].timestamp` should be milliseconds within a second of now, and
     `eventId` present. Both are required by the board.
4. Press the *other* side button and a top button. Confirm they differ in the
   field you picked, otherwise a rule could match the wrong button.

### 2. Configure the board
Copy `src/ProtectWebhookConfig.h.example` to `src/ProtectWebhookConfig.h`:
- `PROTECT_WEBHOOK_SECRET`: `openssl rand -hex 32`
- `PROTECT_WEBHOOK_SOURCE_IP`: the UniFi OS console
- `PROTECT_WEBHOOK_RULES`: one line per fob button, e.g. side button 1 → `"unlock"`,
  side button 2 → `"lock"`. One fob per person, so the fob is the identity.
  Leave `value` as `nullptr` only if you really want any button of that fob to act.

In Nuki Hub → **Nuki Lock Access Control**, allow exactly the actions your rules use.

### 3. Point the alarm at the board
Edit the alarm from step 1 (or make one per button):
- Trigger: Activity → Button → the fob → the side button(s)
- Delivery URL: `http://<board-ip>/protect?k=<secret>`, Method **POST**

### 4. Network
- Board on its own VLAN. Firewall: allow **only** `<console-ip> → <board-ip>:80`;
  web UI from your admin host only; turn on Duo or TOTP in Nuki Hub.
- The board needs NTP. It refuses events until its clock is set (`503 no_time`).

### 5. Bench test without moving the lock
1. In **Nuki Lock Access Control**, untick the action your rule uses (e.g. Unlock).
2. Allow your laptop as source: run from a host with the console's IP, or
   flash a bench build with `PROTECT_WEBHOOK_SOURCE_IP` set to the laptop.
   Do not keep that build.
3. Run, with the values from the capture:
   ```
   scripts/protect_webhook_test.py suite --url http://<board-ip>/protect \
     --secret <secret> --key <key> --device <fob> --field <field> --value <button>
   ```
   Expected: 12/12 PASS. Rejections cover a wrong secret, bad JSON, missing triggers,
   an oversized body, another fob, another button, a wrong trigger key, stale and
   future timestamps, replay, and cooldown. The fully valid press returns
   **`403 acl_denied`**, meaning it passed every webhook check and only the ACL stopped it.
4. Tick the action again, press the real fob, and watch the Nuki Hub log for
   `Protect webhook: unlock -> queued`. (`suite --live` also works, after a typed confirmation.)

### Losing a fob
Remove it in Protect (SuperLink devices) **and** delete its rules from
`ProtectWebhookConfig.h`, then reflash. Either one alone stops it; do both.

### Response codes
| Code | Result | Meaning |
|---|---|---|
| 200 | `ack` | Queued for the lock task |
| 400 | `bad_json` / `no_triggers` | Body not a Protect alarm |
| 403 | `denied` | Wrong source IP or secret |
| 403 | `no_match` | No trigger matched a rule, or it was stale |
| 403 | `acl_denied` | Matched, but the action is disabled in Nuki Hub's ACL |
| 413 | `bad_size` | Empty or > 4 KB body |
| 429 | `cooldown` | Replayed event ID, or < 10 s since the last accepted event |
| 503 | `no_time` | Clock not synced yet |
