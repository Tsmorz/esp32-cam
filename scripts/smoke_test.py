#!/usr/bin/env python3
"""Check a running board after an OTA (or any change).

  scripts/smoke_test.py                       # esp32cam.local, token from src/secrets.h
  scripts/smoke_test.py -H 192.168.1.50 --token abc12345

Checks: dashboard and /status, token handling, state changes are POST-only,
no CORS headers, /detections timestamps are plausible (capture → result
latency is printed), a UDP subscription gets frames with an advancing `seq`
and a wrong token gets none. Exits 1 if anything failed.
"""
import argparse
import json
import socket
import sys
import time

from boardtool import add_board_args, http, resolve_token

results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"  {'ok  ' if ok else 'FAIL'} {name}" + (f" — {detail}" if detail else ""))
    return ok


def jget(host, path, token=None):
    st, _, body = http(host, path, token=token)
    return st, (json.loads(body) if st == 200 else None)


def udp_frames(host, hello: bytes, seconds: float):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.5)
    board = (host, 5005)
    frames, last_hello = [], 0.0
    end = time.time() + seconds
    while time.time() < end:
        if time.time() - last_hello > 5 or not last_hello:
            s.sendto(hello, board)
            last_hello = time.time()
        try:
            frames.append(json.loads(s.recvfrom(2048)[0]))
        except socket.timeout:
            pass
        except json.JSONDecodeError:
            frames.append(None)
    s.sendto(b"bye " + hello[4:], board)
    s.close()
    return frames


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    add_board_args(ap)
    ap.add_argument("--seconds", type=float, default=4, help="how long to listen for UDP frames")
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
    check("GET /detector/set refused", http(host, "/detector/set?enabled=1")[0] == 405)
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
    check("/tag page", http(host, "/tag?id=0")[0] == 200)

    det = status.get("detect", {})
    if not det.get("ready"):
        check("detector ready", False, "detect.ready is false")
    elif not det.get("enabled"):
        print("  note detection is off; frame checks skipped")
    else:
        st, d = jget(host, "/detections")
        if check("/detections", st == 200 and d is not None):
            need = {"seq", "t_us", "now_us", "age_ms", "w", "h", "count", "tags"}
            check("/detections fields", need <= d.keys(), f"missing {sorted(need - d.keys())}" if need - d.keys() else "")
            if d["seq"]:
                total = (d["now_us"] - d["t_us"]) / 1000
                pipeline = total - d["age_ms"]
                check("capture time plausible", 0 < pipeline < 3000,
                      f"capture → result {pipeline:.0f} ms (t_us may be the grab-time fallback if ~0)")
            else:
                print("  note no frame processed yet")
        if token:
            frames = udp_frames(host, b"sub " + token.encode(), args.seconds)
            good = [f for f in frames if f]
            check("UDP frames arrive", len(good) > 0, f"{len(good)} in {args.seconds:.0f} s")
            seqs = [f["seq"] for f in good]
            check("seq advances", len(seqs) > 1 and seqs == sorted(seqs) and seqs[-1] > seqs[0],
                  f"{seqs[0]}…{seqs[-1]}" if seqs else "no frames")
            if len(good) > 1:
                dt = (good[-1]["now_us"] - good[0]["now_us"]) / 1e6
                if dt > 0:
                    print(f"       pushed rate {(seqs[-1] - seqs[0]) / dt:.1f} Hz")
            bad = udp_frames(host, b"sub wrong-token-x", 2)
            check("UDP with a wrong token gets nothing", len(bad) == 0, f"{len(bad)} frames")

    failed = results.count(False)
    print(f"{len(results) - failed}/{len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
