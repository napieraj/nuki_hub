## UniFi SuperLink KeyFob → Protect → Nuki Hub: setup and bench test

For builds with `NUKI_HUB_PROTECT_WEBHOOK` (e.g. `esp32-s3-waveshare-8di8ro`).
All values below are placeholders.

```
USL-FOB ──SuperLink──▶ SuperLink Gateway ─▶ Protect Alarm Manager
   "Activity → Button" alarm  ──POST /protect?k=<secret>&r=<token>──▶ Nuki Hub ──BLE──▶ lock
```

### What is and isn't documented
| Source | What it tells us |
|---|---|
| Protect Integration API spec 7.2.105 (`developer.ui.com/protect`) | A fob press is a `sensorButtonPressed` event. `metadata.button.text` is one of `arm, disarm, night, panic, left, right`. **No gesture field.** The fob's `featureFlags.buttons` lists its buttons. |
| Alarm Manager webhook body | Only the envelope is documented: `alarm.{name, sources, conditions, triggers[]}` plus `timestamp`. The trigger `key` for a button press, and whether the button and gesture appear at all, are **not documented**. Older payloads carry just `key`/`device` per trigger; newer ones add `eventId`/`timestamp`. |
| Fob settings (Protect app) | 6 buttons × Press / Hold / Double = 18 events. The face buttons map to arm (top-left), night (top-right), disarm (bottom-left) and panic (bottom-right); the sides are left/right. |

Because the button encoding in the webhook is undocumented, **let Protect do the
filtering**: one Alarm Manager alarm per button+gesture you use, each with its
own URL token. The board then checks only secret + token + fob + freshness,
and parses nothing undocumented. If a capture shows Protect does include the
button/gesture, you can additionally match on those fields (`key`/`field`/`value`).

Suggested mapping: **Hold Right = unlock**, **Press Right = lock**. Leave these
buttons on "No Action" in the fob's own settings, so Protect doesn't also arm or
disarm when you use them.

### 1. Capture one real press
1. On a laptop on the LAN: `scripts/protect_webhook_test.py listen --port 8099`
2. Protect → Alarm Manager → Create Alarm:
   - Trigger: **Activity → Button**, select the fob and **Hold** on **Right**
     (if the gesture is not selectable here, note that. It matters, see step 4.)
   - Action: Webhook → Custom Webhook → `http://<laptop-ip>:8099/capture`, Advanced → **POST**
3. Hold the Right button. From the JSON note `triggers[].device` (the fob MAC,
   used as the rule's `device`) and whether `eventId`/`timestamp` are present.
4. Press (don't hold) the Right button, then press Left. **Neither may reach the
   listener.** If one does, Alarm Manager isn't filtering by gesture/button;
   then either use rules with captured `field`/`value`, or pick a trigger that
   is filterable.

### 2. Configure the board
Copy `src/ProtectWebhookConfig.h.example` to `src/ProtectWebhookConfig.h`:
- `PROTECT_WEBHOOK_SECRET`: `openssl rand -hex 32`
- `PROTECT_WEBHOOK_SOURCE_IP`: the UniFi OS console
- `PROTECT_WEBHOOK_RULES`: one line per alarm: `{ token, nullptr, "<fob MAC>", nullptr, nullptr, nullptr, nullptr, "unlock" }`,
  with each token from `openssl rand -hex 16`. One fob per person, so the fob is the identity.

In Nuki Hub → **Nuki Lock Access Control**, allow exactly the actions your rules use.

### 3. Point the alarms at the board
One alarm per rule, each restricted to the fob and its single button+gesture:
- Delivery URL: `http://<board-ip>/protect?k=<secret>&r=<that rule's token>`, Method **POST**

### 4. Network
- Board on its own VLAN. Firewall: allow **only** `<console-ip> → <board-ip>:80`;
  web UI from your admin host only; turn on Duo or TOTP in Nuki Hub.
- The board needs NTP. It refuses events until its clock is set (`503 no_time`).

### 5. Bench test without moving the lock
1. In **Nuki Lock Access Control**, untick the action the rule uses.
2. Allow your laptop as source: run from a host with the console's IP, or
   flash a bench build with `PROTECT_WEBHOOK_SOURCE_IP` set to the laptop.
   Do not keep that build.
3. Run:
   ```
   scripts/protect_webhook_test.py suite --url http://<board-ip>/protect \
     --secret <secret> --token <rule token> --device <fob MAC>
   ```
   Add `--legacy` if your capture had no `eventId`/`timestamp` in the trigger.
   Expected: **13/13 PASS**. Rejections cover a wrong secret, bad JSON, missing
   triggers, an oversized body, another fob, stale and future timestamps, a stale
   legacy-shape payload, a wrong or missing alarm token, replay, and cooldown.
   The fully valid press returns **`403 acl_denied`**, meaning it passed every
   check and only the ACL stopped it.
4. Tick the action again, hold the real fob button, and watch the Nuki Hub
   log for `Protect webhook: unlock -> queued`.

### Losing a fob
Remove it in Protect **and** delete its rules, then reflash. Either one alone
stops it; do both. Rotating a rule's token also kills that alarm's URL.

### Alternative: the documented event stream
The Integration API's `wss://<console>/proxy/protect/integration/v1/subscribe/events`
(API-key auth) delivers `sensorButtonPressed` with `metadata.button.text` exactly
as specified. But it has no gesture, and the board would have to hold a
Protect API key. Not implemented; the webhook keeps the credential off the lock board.

### Response codes
| Code | Result | Meaning |
|---|---|---|
| 200 | `ack` | Queued for the lock task |
| 400 | `bad_json` / `no_triggers` | Body not a Protect alarm |
| 403 | `denied` | Wrong source IP or secret |
| 403 | `no_match` | No rule matched (token/fob/fields), or the event was stale |
| 403 | `acl_denied` | Matched, but the action is disabled in Nuki Hub's ACL |
| 413 | `bad_size` | Empty or > 4 KB body |
| 429 | `cooldown` | Replayed event, or < 10 s since the last accepted event |
| 503 | `no_time` | Clock not synced yet |
