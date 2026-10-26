#!/usr/bin/env python3
"""Prove that a bad firmware update rolls back on a real board.

  scripts/test_ota_rollback.py                  # wifi-camera.local, token from src/secrets.h
  scripts/test_ota_rollback.py -H 192.168.1.50 --token abc12345

Builds a copy of the firmware that crashes shortly before the 20 s OTA confirm
(-DTEST_CRASH_AFTER_BOOT, in a separate build directory, so your normal build
is untouched), uploads it over /ota, and watches /status. Expected: the new
image boots (a different `build` string appears), crashes, and the bootloader
returns to the image that was running before, with ota_pending false.

Exit 0 = rolled back, 1 = it did not, 2 = inconclusive (never saw the crash
image, e.g. polling missed its short window).
The board is left running its previous firmware. Uses the token like /ota does.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from boardtool import add_board_args, http, resolve_token

ROOT = Path(__file__).resolve().parent.parent


def status(host):
    try:
        st, _, body = http(host, "/status", timeout=2)
        if st == 200:
            import json
            return json.loads(body)
    except OSError:
        pass
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    add_board_args(ap)
    ap.add_argument("--env", default="firebeetle2_esp32s3")
    ap.add_argument("--timeout", type=float, default=150, help="seconds to wait for the outcome")
    args = ap.parse_args()
    host, token = args.host, resolve_token(args)

    before = status(host)
    if not before:
        print(f"cannot reach {host}")
        return 1
    if not token or not http(host, "/auth", token=token)[0] == 200:
        print("token missing or wrong — needed for /ota")
        return 1
    if before["ota_pending"]:
        print("the board is still confirming a previous update; wait 20 s and retry")
        return 1
    print(f"running: {before['version']} built {before['build']}")

    with tempfile.TemporaryDirectory(prefix="ota-crash-") as tmp:
        print("building the crashing image…")
        env = dict(os.environ, PLATFORMIO_BUILD_FLAGS="-DTEST_CRASH_AFTER_BOOT=1", PLATFORMIO_BUILD_DIR=tmp)
        r = subprocess.run(["pio", "run", "-e", args.env], cwd=ROOT, env=env)
        image = Path(tmp) / args.env / "firmware.bin"
        if r.returncode or not image.exists():
            print("build failed")
            return 1
        data = image.read_bytes()

    print(f"uploading {len(data) // 1024} KB…")
    code, _, body = http(host, "/ota", "POST", token=token, body=data,
                         headers={"Content-Type": "application/octet-stream"}, timeout=120)
    if code != 200:
        print(f"upload rejected: HTTP {code} {body[:80]!r}")
        return 1

    print("watching…")
    crash_build, went_down, t0, last = None, False, time.time(), None
    while time.time() - t0 < args.timeout:
        s = status(host)
        if s is None:
            went_down = True
        else:
            if s["build"] != before["build"] and crash_build is None:
                crash_build = s["build"]
                print(f"  {time.time() - t0:5.1f} s  crash image up (ota_pending={s['ota_pending']})")
            if crash_build is None and last is not None and s["uptime"] < last["uptime"]:
                went_down = True  # rebooted between two polls
            if s["build"] == before["build"] and (went_down or crash_build):
                if not s["ota_pending"]:
                    print(f"  {time.time() - t0:5.1f} s  back on the previous image "
                          f"({s['version']}, built {s['build']})")
                    if crash_build:
                        print("PASS: rolled back after the crash")
                        return 0
                    print("INCONCLUSIVE: the board rebooted and is back on the old image, but the crash "
                          "image was never seen in /status (it may not have booted at all)")
                    return 2
            last = s
        time.sleep(0.4)
    print("FAIL: the previous image did not come back in time" if crash_build
          else "FAIL: the new image never appeared")
    return 1


if __name__ == "__main__":
    sys.exit(main())
