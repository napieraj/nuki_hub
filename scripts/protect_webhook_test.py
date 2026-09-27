#!/usr/bin/env python3
"""Bench tool for the Nuki Hub UniFi Protect webhook (NUKI_HUB_PROTECT_WEBHOOK).

Standard library only. Three commands:

  listen   Print every POST it receives. Point a Protect Alarm Manager webhook
           at it once to capture a real payload and confirm the field names.

  send     Send one Protect-shaped NFC event to the board.

  suite    Run the full accept/reject sequence against the board and check
           every response code. Run it with "Unlock" DISABLED in Nuki Hub's
           "Nuki Lock Access Control": a fully valid event then ends in
           403 acl_denied, which proves the whole path without moving the lock.

The board only accepts requests from PROTECT_WEBHOOK_SOURCE_IP. For bench runs
either run this on a host with that IP, or build a bench firmware with that
IP set to your laptop (or "" to disable the check). Never ship the latter.

Examples:
  ./protect_webhook_test.py listen --port 8099
  ./protect_webhook_test.py suite --url http://192.0.2.20/protect \\
      --secret "$SECRET" --mac AA:BB:CC:DD:EE:FF \\
      --user 00000000-0000-0000-0000-000000000001
"""

import argparse
import http.server
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid


def build_event(mac, user, key="nfc_registered", ts_ms=None, event_id=None):
    """Mirror the shape UniFi Protect Alarm Manager sends for an NFC scan."""
    now = int(time.time() * 1000) if ts_ms is None else ts_ms
    return {
        "alarm": {
            "name": "Bench NFC test",
            "sources": [{"device": mac, "type": "include"}],
            "conditions": [{"condition": {"type": "is", "source": key}}],
            "triggers": [{
                "key": key,
                "device": mac,
                "value": user,
                "eventId": event_id or uuid.uuid4().hex[:24],
                "timestamp": now,
            }],
        },
        "timestamp": now + 25,
    }


def post(url, secret, body, timeout=5.0):
    """POST raw bytes to url?k=secret. Returns (status, parsed-or-text body)."""
    parts = urllib.parse.urlsplit(url)
    query = urllib.parse.urlencode({"k": secret})
    target = urllib.parse.urlunsplit((parts.scheme, parts.netloc, parts.path, query, ""))
    data = body if isinstance(body, bytes) else json.dumps(body).encode()
    req = urllib.request.Request(target, data=data, method="POST",
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            status, raw = resp.status, resp.read()
    except urllib.error.HTTPError as err:
        status, raw = err.code, err.read()
    try:
        return status, json.loads(raw)
    except ValueError:
        return status, raw.decode(errors="replace")


# ---------------------------------------------------------------- commands

def cmd_listen(args):
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length)
            print(f"\n--- {time.strftime('%H:%M:%S')} POST {self.path} from {self.client_address[0]}")
            for k, v in self.headers.items():
                print(f"{k}: {v}")
            try:
                print(json.dumps(json.loads(body), indent=2))
            except ValueError:
                print(body.decode(errors="replace"))
            self.send_response(200)
            self.end_headers()

        def log_message(self, *_):
            pass

    print(f"Listening on 0.0.0.0:{args.port} — Ctrl-C to stop")
    http.server.HTTPServer(("0.0.0.0", args.port), Handler).serve_forever()


def cmd_send(args):
    status, body = post(args.url, args.secret, build_event(args.mac, args.user, key=args.key))
    print(status, body)
    return 0 if status in (200, 403) else 1


def cmd_suite(args):
    mac, user, url, secret = args.mac, args.user, args.url, args.secret
    now = lambda: int(time.time() * 1000)
    valid_id = uuid.uuid4().hex[:24]

    # (name, secret, body, expected status, expected result)
    cases = [
        ("wrong secret",      "x" * len(secret), build_event(mac, user), 403, "denied"),
        ("malformed JSON",    secret, b"{not json", 400, "bad_json"),
        ("no triggers",       secret, {"alarm": {}}, 400, "no_triggers"),
        ("oversized body",    secret, b"{" + b" " * 5000 + b"}", 413, "bad_size"),
        ("unknown user",      secret, build_event(mac, "11111111-2222-3333-4444-555555555555"), 403, "no_match"),
        ("other reader MAC",  secret, build_event("00:11:22:33:44:55", user), 403, "no_match"),
        ("wrong trigger key", secret, build_event(mac, user, key="fingerprint_identified"), 403, "no_match"),
        ("stale (-60 s)",     secret, build_event(mac, user, ts_ms=now() - 60_000), 403, "no_match"),
        ("future (+60 s)",    secret, build_event(mac, user, ts_ms=now() + 60_000), 403, "no_match"),
        # Everything valid. With Unlock disabled in the ACL this is acl_denied;
        # with Unlock enabled it is ack and THE LOCK WILL MOVE.
        ("valid event",       secret, build_event(mac, user, event_id=valid_id), 403 if not args.live else 200,
                                                                   "acl_denied" if not args.live else "ack"),
        ("replay same event", secret, build_event(mac, user, event_id=valid_id), 429, "cooldown"),
        ("new event < 10 s",  secret, build_event(mac, user), 429, "cooldown"),
    ]

    if args.live:
        print("LIVE MODE: the 'valid event' case will actuate the lock.")
        if input("Type 'unlock' to continue: ").strip() != "unlock":
            return 1

    failures = 0
    for name, sec, body, want_status, want_result in cases:
        status, resp = post(url, sec, body)
        result = resp.get("result") if isinstance(resp, dict) else resp
        ok = status == want_status and result == want_result
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name:<18} -> {status} {result}"
              + ("" if ok else f"   (expected {want_status} {want_result})"))
        time.sleep(0.2)

    print(f"\n{len(cases) - failures}/{len(cases)} passed")
    if failures:
        print("Hints: 503 no_time = board has no NTP time yet; "
              "403 denied on every case = source IP not allowed or secret mismatch.")
    return 1 if failures else 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("listen", help="print incoming webhooks")
    s.add_argument("--port", type=int, default=8099)
    s.set_defaults(func=cmd_listen)

    for name, func in (("send", cmd_send), ("suite", cmd_suite)):
        s = sub.add_parser(name)
        s.add_argument("--url", required=True, help="http://<board>/protect")
        s.add_argument("--secret", required=True)
        s.add_argument("--mac", required=True, help="reader MAC configured on the board")
        s.add_argument("--user", required=True, help="allowed Protect user GUID")
        if name == "send":
            s.add_argument("--key", default="nfc_registered")
        else:
            s.add_argument("--live", action="store_true",
                           help="expect 200 ack for the valid case (lock moves)")
        s.set_defaults(func=func)

    args = p.parse_args()
    sys.exit(args.func(args) or 0)


if __name__ == "__main__":
    main()
