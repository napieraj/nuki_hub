#!/usr/bin/env python3
"""Bench tool for the Nuki Hub UniFi Protect webhook (NUKI_HUB_PROTECT_WEBHOOK).

Standard library only. Three commands:

  listen   Print every POST it receives. Point an Alarm Manager webhook at it
           to capture a real fob press and confirm the fob's device value.

  send     Send one Protect-shaped event to the board.

  suite    Run the accept/reject sequence against the board and check every
           response. Run it with the rule's action UNTICKED in Nuki Hub's
           "Nuki Lock Access Control" and pass --acl-disabled (or confirm at
           the prompt): the fully valid event then ends in 403 acl_denied,
           which proves the whole path without moving the lock. If the action
           is still ticked, the valid case REALLY moves the lock.
           With --token2 (a second rule, e.g. press-to-lock), also checks that
           a different rule is accepted inside the first rule's cooldown; its
           action must be unticked too.

The board only accepts requests from PROTECT_WEBHOOK_SOURCE_IP. For bench runs
either run this on a host with that IP, or build a bench firmware with that
IP set to your laptop (or "" to disable the check). Never ship the latter.

Example matching the recommended rules (token + key + button + gesture):
  ./protect_webhook_test.py suite --url http://192.0.2.20/protect --bearer \\
      --secret "$SECRET" --token "$HOLD_RIGHT_TOKEN" --device AA:BB:CC:DD:EE:01 \\
      --key-checked --field button --value right --field2 value --value2 longPress \\
      --token2 "$PRESS_RIGHT_TOKEN" --rule2-value2 press --acl-disabled

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
    cooldown_s = args.cooldown_ms / 1000
    now = lambda: int(time.time() * 1000)

    if args.live:
        print("LIVE MODE: the 'valid press' case will actuate the lock.")
        if input("Type 'unlock' to continue: ").strip() != "unlock":
            return 1
    elif not args.acl_disabled:
        print("The 'valid press' case passes every check on the board. Unless the rule's")
        print("action is UNTICKED in Nuki Hub -> Nuki Lock Access Control, it REALLY moves")
        print("the lock." + (" The same applies to the --token2 rule's action." if args.token2 else ""))
        if input("Is the action unticked? Type 'yes' to continue: ").strip() != "yes":
            return 1

    def ev(device=None, key_=None, value=None, value2=None, ts_ms=None, event_id=None, legacy=None):
        return build_event(device or dev, key_ or key, _fields(args, value, value2),
                           ts_ms=ts_ms, event_id=event_id,
                           legacy=args.legacy if legacy is None else legacy)

    # Bodies are built right before sending (callables), so timestamps are fresh.
    # (name, secret, token, body(), expected status, expected result(s))
    cases = [
        ("wrong secret",        "x" * len(secret), token, lambda: ev(), 403, "denied"),
        ("malformed JSON",      secret, token, lambda: b"{not json", 400, "bad_json"),
        ("no triggers",         secret, token, lambda: {"alarm": {}}, 400, "no_triggers"),
        ("oversized body",      secret, token, lambda: b"{" + b" " * 5000 + b"}", 413, "bad_size"),
        ("other fob",           secret, token, lambda: ev(device="00:11:22:33:44:55"), 403, "no_match"),
        ("stale (-60 s)",       secret, token, lambda: ev(ts_ms=now() - 60_000), 403, "no_match"),
        ("future (+60 s)",      secret, token, lambda: ev(ts_ms=now() + 60_000), 403, "no_match"),
        ("legacy shape, stale", secret, token, lambda: ev(ts_ms=now() - 60_000, legacy=True), 403, "no_match"),
    ]
    if token is not None:
        cases.append(("wrong alarm token",   secret, "0" * len(token), lambda: ev(), 403, "no_match"))
        cases.append(("missing alarm token", secret, None, lambda: ev(), 403, "no_match"))
    if args.key_checked:
        cases.append(("wrong trigger key",   secret, token, lambda: ev(key_="motion"), 403, "no_match"))
    if args.field and args.value is not None:
        cases.append(("other button",        secret, token, lambda: ev(value="not-a-configured-button"), 403, "no_match"))
    if args.field2 and args.value2 is not None:
        cases.append(("other gesture",       secret, token, lambda: ev(value2="not-a-configured-gesture"), 403, "no_match"))

    # Everything valid. With the rule's action unticked in the ACL this is
    # acl_denied; with it ticked it is ack and THE LOCK WILL MOVE. The replay
    # must resend the very same body.
    sent = {}
    def valid():
        sent["valid"] = ev(ts_ms=now(), event_id=uuid.uuid4().hex[:24])
        return sent["valid"]
    # Live: the action is still pending right after the ack, so the board may
    # answer 503 busy (checked before replay/cooldown) instead of 429.
    after_valid = ("cooldown", "busy") if args.live else ("cooldown",)
    after_status = (429, 503) if args.live else (429,)
    cases += [
        ("valid press",       secret, token, valid, 200 if args.live else 403,
                                             "ack" if args.live else "acl_denied"),
        ("replay same press", secret, token, lambda: sent["valid"], after_status, after_valid),
        (f"new press < {cooldown_s:g} s", secret, token, lambda: ev(), after_status, after_valid),
    ]
    if args.token2:
        if args.live:
            print("Note: skipping the --token2 case in live mode (the first action is still pending).")
        else:
            # Cooldown is per rule: another rule right after is accepted (and
            # then refused by the ACL, since its action is unticked too).
            cases.append(("other rule in cooldown", secret, args.token2,
                          lambda: ev(value=args.rule2_value, value2=args.rule2_value2), 403, "acl_denied"))

    failures = 0
    valid_sent_at = None
    for name, sec, tok, body, want_status, want_result in cases:
        want_status = want_status if isinstance(want_status, tuple) else (want_status,)
        want_result = want_result if isinstance(want_result, tuple) else (want_result,)
        if valid_sent_at is not None and time.monotonic() - valid_sent_at > cooldown_s:
            print(f"WARN  more than {cooldown_s:g} s since the valid press; cooldown cases may not apply")
        status, resp = post(url, sec, body(), token=tok)
        if body is valid:
            valid_sent_at = time.monotonic()
        result = resp.get("result") if isinstance(resp, dict) else resp
        ok = status in want_status and result in want_result
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name:<22} -> {status} {result}"
              + ("" if ok else f"   (expected {'/'.join(map(str, want_status))} {'/'.join(want_result)})"))
        time.sleep(0.2)

    print(f"\n{len(cases) - failures}/{len(cases)} passed")
    if failures:
        print("Hints: 503 no_time = board has no NTP time yet; 503 busy = a lock action was still pending; "
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
            s.add_argument("--acl-disabled", action="store_true",
                           help="confirm the rule's action is unticked in Nuki Lock Access Control "
                                "(otherwise you are asked)")
            s.add_argument("--cooldown-ms", type=int, default=10000,
                           help="PROTECT_WEBHOOK_COOLDOWN_MS of the board (default 10000)")
            s.add_argument("--token2", help="token of a second rule, to check per-rule cooldown")
            s.add_argument("--rule2-value", help="button of the second rule (default: --value)")
            s.add_argument("--rule2-value2", help="gesture of the second rule (default: --value2)")
        s.set_defaults(func=func)

    args = p.parse_args()
    global BEARER
    BEARER = getattr(args, "bearer", False)
    sys.exit(args.func(args) or 0)


if __name__ == "__main__":
    main()
