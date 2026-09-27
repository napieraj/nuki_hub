## UniFi Protect → Nuki Hub webhook: setup and bench test

For builds with `NUKI_HUB_PROTECT_WEBHOOK` (e.g. `esp32-s3-waveshare-8di8ro`).
All values below are placeholders.

### 1. Collect the identifiers
| What | Where | Goes into `ProtectWebhookConfig.h` |
|---|---|---|
| Reader MAC | Protect → Devices → the doorbell/reader → Settings/Details | `PROTECT_WEBHOOK_DEVICE_MAC` |
| User GUID per person allowed in | Protect → Admins & Users → user → the ID in the page URL | `PROTECT_WEBHOOK_USERS[]` |
| Console IP | UniFi OS → Console settings | `PROTECT_WEBHOOK_SOURCE_IP` |
| Secret | `openssl rand -hex 32` | `PROTECT_WEBHOOK_SECRET` |

Register each NFC card to its user in the Protect app (doorbell → NFC cards) first:
Protect only sends the user GUID for **registered** cards; unknown cards are
filtered out below and would carry no user anyway.

### 2. Capture one real payload (recommended before anything else)
The board's parser expects `alarm.triggers[].{key, device, value, eventId, timestamp}`,
taken from published examples, not from your console. Confirm it:

1. On a laptop on the LAN: `scripts/protect_webhook_test.py listen --port 8099`
2. Create the alarm below, but with Delivery URL `http://<laptop-ip>:8099/capture`.
3. Scan a registered card. Check the printed JSON: `key` should be
   `nfc_registered`, `device` the reader MAC, `value` the user GUID, and
   `timestamp` in milliseconds within a second of now.
4. If anything differs, stop and adjust the parser before going further.

### 3. Create the Alarm Manager alarm
Protect → Alarm Manager → Create Alarm:
- **Name:** e.g. `Door – NFC unlock`
- **Trigger:** Activity → **NFC Card Scan** → **Registered** only
- **Scope:** Include → only the door reader
- **Schedule:** always (or your own)
- **Action:** Webhook → **Custom Webhook**
  - Delivery URL: `http://<board-ip>/protect?k=<secret>`
  - Advanced settings → Method **POST**

Menu names move between Protect versions; the trigger, scope and POST method
are what matter.

### 4. Network
- Board and lock-side traffic on their own VLAN.
- Firewall: allow **only** `<console-ip> → <board-ip>:80`. Web UI access from
  your admin host only, and turn on Duo or TOTP in Nuki Hub.
- The board needs NTP (it refuses events until its clock is set: `503 no_time`).

### 5. Bench test without moving the lock
1. In Nuki Hub → **Nuki Lock Access Control**, **untick Unlock** and save.
2. Allow your laptop as source: either run the suite from a host with the
   console's IP, or flash a bench build with `PROTECT_WEBHOOK_SOURCE_IP` set
   to the laptop. Do not keep that build.
3. Run:
   ```
   scripts/protect_webhook_test.py suite --url http://<board-ip>/protect \
     --secret <secret> --mac <reader-mac> --user <allowed-guid>
   ```
   Expected: 12/12 PASS. Rejections cover a wrong secret, bad JSON, missing triggers,
   an oversized body, an unknown user, another reader, a wrong trigger key, stale and
   future timestamps, replay, and cooldown. The fully valid event returns
   **`403 acl_denied`**, which means it passed every webhook check and reached the lock
   wrapper, where the ACL stopped it.
4. Tick **Unlock** again. Final end-to-end: scan a real card at the door and
   watch the Nuki Hub log for `Protect webhook: unlock -> queued`.
   (`suite --live` also works, and asks for confirmation before the lock moves.)

### Response codes
| Code | Result | Meaning |
|---|---|---|
| 200 | `ack` | Queued for the lock task |
| 400 | `bad_json` / `no_triggers` | Body not a Protect alarm |
| 403 | `denied` | Wrong source IP or secret |
| 403 | `no_match` | No trigger passed key / reader / user / freshness |
| 403 | `acl_denied` | Valid, but the action is disabled in Nuki Hub's ACL |
| 413 | `bad_size` | Empty or > 4 KB body |
| 429 | `cooldown` | Replayed event ID, or < 10 s since the last accepted event |
| 503 | `no_time` | Clock not synced yet |
