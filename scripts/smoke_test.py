#!/usr/bin/env python3
"""Check a running board after an OTA (or any change).

  scripts/smoke_test.py                       # wifi-camera.local, token from src/secrets.h
  scripts/smoke_test.py -H 192.168.1.50 --token abc12345

Checks: dashboard and /status, token handling, state changes are POST-only,
no CORS headers, /snapshot, /camera/status and /pose work, and a request for
another host is redirected (captive portal). Exits 1 if anything failed.
"""
import argparse
import json
import sys
from http.client import HTTPConnection

from boardtool import add_board_args, http, resolve_token

results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"  {'ok  ' if ok else 'FAIL'} {name}" + (f" — {detail}" if detail else ""))
    return ok


def jget(host, path, token=None):
    st, _, body = http(host, path, token=token)
    return st, (json.loads(body) if st == 200 else None)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    add_board_args(ap)
    args = ap.parse_args()
    host, token = args.host, resolve_token(args)

    print(f"board {host}")
    try:
        st, status = jget(host, "/status")
    except OSError as e:
        print(f"  FAIL cannot reach the board: {e}")
        return 1
    if not check("/status", st == 200 and status is not None, f"HTTP {st}"):
        return 1
    print(f"       version {status['version']}, built {status['build']}, "
          f"up {status['uptime']} s, ota_pending={status['ota_pending']}")
    check("dashboard", http(host, "/")[0] == 200)
    check("no CORS header", "Access-Control-Allow-Origin" not in http(host, "/status")[1])
    check("GET /camera/set refused", http(host, "/camera/set?var=quality&val=12")[0] == 405)
    check("unauthenticated POST refused", http(host, "/camera/set?var=quality&val=12", "POST")[0] == 401)

    if not status.get("auth"):
        print("  note the board has no token yet (claim one in the dashboard); token checks skipped")
    elif not token:
        print("  note no token given (--token or src/secrets.h); authenticated checks skipped")
    else:
        check("wrong token refused", http(host, "/auth", token="wrong-token-x")[0] == 401)
        check("token accepted", http(host, "/auth", token=token)[0] == 200)

    code, hdrs, body = http(host, "/snapshot", timeout=10)
    check("/snapshot is a JPEG", code == 200 and body[:2] == b"\xff\xd8", f"HTTP {code}, {len(body)} B")
    st, cam = jget(host, "/camera/status")
    check("/camera/status", st == 200 and cam is not None and "viewers" in cam, f"HTTP {st}")
    check("/status.wifi.ap_on", "ap_on" in status.get("wifi", {}))
    st, pose = jget(host, "/pose")
    check("/pose", st == 200 and pose is not None and {"q", "imu", "mag"} <= pose.keys(), f"HTTP {st}")
    if pose and pose.get("imu") == "none":
        print("  note no IMU board attached; orientation checks skipped")
    # A phone's connectivity check for another host is redirected to the
    # dashboard (urllib would follow the redirect, so ask without it)
    c = HTTPConnection(host, 80, timeout=6)
    c.request("GET", "/hotspot-detect.html", headers={"Host": "captive.apple.com"})
    r = c.getresponse()
    check("captive portal redirect", r.status == 302, f"HTTP {r.status} → {r.getheader('Location')}")
    c.close()

    failed = results.count(False)
    print(f"{len(results) - failed}/{len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
