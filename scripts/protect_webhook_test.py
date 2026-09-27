#!/usr/bin/env python3
"""Bench tool for the Nuki Hub UniFi Protect webhook (NUKI_HUB_PROTECT_WEBHOOK).

Standard library only. Three commands:

  listen   Print every POST it receives. Point an Alarm Manager webhook at it
           to capture a real fob press and confirm the fob's device value.

  send     Send one Protect-shaped event to the board.

  suite    Run the accept/reject sequence against the board and check every
           response. Run it with the rule's action DISABLED in Nuki Hub's
           "Nuki Lock Access Control": the fully valid event then ends in
           403 acl_denied, which proves the whole path without moving the lock.

The board only accepts requests from PROTECT_WEBHOOK_SOURCE_IP. For bench runs
either run this on a host with that IP, or build a bench firmware with that
IP set to your laptop (or "" to disable the check). Never ship the latter.

Example matching the recommended rule (token + key + button + gesture):
  ./protect_webhook_test.py suite --url http://192.0.2.20/protect --bearer \\
      --secret "$SECRET" --token "$HOLD_RIGHT_TOKEN" --device AA:BB:CC:DD:EE:01 \\
      --key-checked --field button --value right --field2 value --value2 longPress

Events are shaped like UniFi Protect 7.2.105's Alarm Manager fob trigger:
  {"key":"sensor_button_pressed","value":<press|longPress|doublePress>,
   "device":<fob MAC>,"button":<arm|night|disarm|panic|left|right>,"eventId","timestamp"}
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


def build_event(device, key="sensor_button_pressed", fields=None, ts_ms=None, event_id=None, legacy=False):
    """Alarm Manager envelope around one trigger.

    legacy=True mimics older payloads: no per-trigger eventId/timestamp, only
    the envelope timestamp (the board then derives an ID from ts|key|device).
    """
    now = int(time.time() * 1000) if ts_ms is None else ts_ms
    trigger = {"key": key, "device": device}
    if not legacy:
        trigger["eventId"] = event_id or uuid.uuid4().hex[:24]
        trigger["timestamp"] = now
    for f, v in (fields or {}).items():
        if f:
            trigger[f] = v
    source = {"device": device, "type": "include"}
    if trigger.get("button"):
        source["buttons"] = [trigger["button"]]
    condition = {"type": "is", "source": key}
    if trigger.get("value") is not None:
        condition["value"] = trigger["value"]
    return {
        "alarm": {
            "name": "Bench fob test",
            "sources": [source],
            "conditions": [{"condition": condition}],
            "triggers": [trigger],
        },
        "timestamp": now,
    }


BEARER = False  # set by --bearer: send the secret as "Authorization: Bearer" like Protect


def post(url, secret, body, token=None, timeout=5.0):
    """POST like Protect: url?r=token with ?k=secret or a Bearer header."""
    parts = urllib.parse.urlsplit(url)
    q = {} if BEARER else {"k": secret}
    if token is not None:
        q["r"] = token
    target = urllib.parse.urlunsplit((parts.scheme, parts.netloc, parts.path,
                                      urllib.parse.urlencode(q), ""))
    data = body if isinstance(body, bytes) else json.dumps(body).encode()
    headers = {"Content-Type": "application/json", "User-Agent": "protect-alarm-manager"}
    if BEARER:
        headers["Authorization"] = "Bearer " + secret
    req = urllib.request.Request(target, data=data, method="POST", headers=headers)
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


def _fields(args, value=None, value2=None):
    return {args.field: args.value if value is None else value,
            args.field2: args.value2 if value2 is None else value2}


def cmd_send(args):
    body = build_event(args.device, args.key, _fields(args), legacy=args.legacy)
    status, resp = post(args.url, args.secret, body, token=args.token)
    print(status, resp)
    return 0 if status in (200, 403) else 1


def cmd_suite(args):
    url, secret, token, dev, key = args.url, args.secret, args.token, args.device, args.key
    now = lambda: int(time.time() * 1000)

    def ev(device=None, key_=None, value=None, value2=None, ts_ms=None, event_id=None, legacy=None):
        return build_event(device or dev, key_ or key, _fields(args, value, value2),
                           ts_ms=ts_ms, event_id=event_id,
                           legacy=args.legacy if legacy is None else legacy)

    # (name, secret, token, body, expected status, expected result)
    cases = [
        ("wrong secret",        "x" * len(secret), token, ev(), 403, "denied"),
        ("malformed JSON",      secret, token, b"{not json", 400, "bad_json"),
        ("no triggers",         secret, token, {"alarm": {}}, 400, "no_triggers"),
        ("oversized body",      secret, token, b"{" + b" " * 5000 + b"}", 413, "bad_size"),
        ("other fob",           secret, token, ev(device="00:11:22:33:44:55"), 403, "no_match"),
        ("stale (-60 s)",       secret, token, ev(ts_ms=now() - 60_000), 403, "no_match"),
        ("future (+60 s)",      secret, token, ev(ts_ms=now() + 60_000), 403, "no_match"),
        ("legacy shape, stale", secret, token, ev(ts_ms=now() - 60_000, legacy=True), 403, "no_match"),
    ]
    if token is not None:
        cases.append(("wrong alarm token",   secret, "0" * len(token), ev(), 403, "no_match"))
        cases.append(("missing alarm token", secret, None, ev(), 403, "no_match"))
    if args.key_checked:
        cases.append(("wrong trigger key",   secret, token, ev(key_="motion"), 403, "no_match"))
    if args.field and args.value is not None:
        cases.append(("other button",        secret, token, ev(value="not-a-configured-button"), 403, "no_match"))
    if args.field2 and args.value2 is not None:
        cases.append(("other gesture",       secret, token, ev(value2="not-a-configured-gesture"), 403, "no_match"))

    # Everything valid. With the rule's action disabled in the ACL this is
    # acl_denied; with it enabled it is ack and THE LOCK WILL MOVE.
    valid = ev(ts_ms=now(), event_id=uuid.uuid4().hex[:24])
    cases += [
        ("valid press",       secret, token, valid, 200 if args.live else 403,
                                                    "ack" if args.live else "acl_denied"),
        ("replay same press", secret, token, valid, 429, "cooldown"),
        ("new press < 10 s",  secret, token, ev(), 429, "cooldown"),
    ]

    if args.live:
        print("LIVE MODE: the 'valid press' case will actuate the lock.")
        if input("Type 'unlock' to continue: ").strip() != "unlock":
            return 1

    failures = 0
    for name, sec, tok, body, want_status, want_result in cases:
        status, resp = post(url, sec, body, token=tok)
        result = resp.get("result") if isinstance(resp, dict) else resp
        ok = status == want_status and result == want_result
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name:<20} -> {status} {result}"
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
        s.add_argument("--device", required=True, help="fob MAC as Protect sends it")
        s.add_argument("--token", help="per-alarm rule token (?r=), if the rule uses one")
        s.add_argument("--key", default="sensor_button_pressed", help="trigger key to send")
        s.add_argument("--key-checked", action="store_true",
                       help="the rule checks --key (adds a wrong-key case)")
        s.add_argument("--field", help="button field, if the rule checks one")
        s.add_argument("--value", help="expected button value")
        s.add_argument("--field2", help="gesture field, if the rule checks one")
        s.add_argument("--value2", help="expected gesture value")
        s.add_argument("--bearer", action="store_true",
                       help="send the secret as 'Authorization: Bearer' instead of ?k=")
        s.add_argument("--legacy", action="store_true",
                       help="send old-style triggers without eventId/timestamp")
        if name == "suite":
            s.add_argument("--live", action="store_true",
                           help="expect 200 ack for the valid case (lock moves)")
        s.set_defaults(func=func)

    args = p.parse_args()
    global BEARER
    BEARER = getattr(args, "bearer", False)
    sys.exit(args.func(args) or 0)


if __name__ == "__main__":
    main()
