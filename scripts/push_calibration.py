#!/usr/bin/env python3
"""Send a camera calibration to the board (stored in its flash).

  scripts/push_calibration.py calib.json            # file with the /calibration JSON
  scripts/push_calibration.py --clear               # back to the estimate / calibration.h
  scripts/push_calibration.py                       # show what the board has
  scripts/push_calibration.py -H 192.168.1.50 calib.json

calib.json: {"width":800,"height":600,"fx":..,"fy":..,"cx":..,"cy":..,
             "dist":[k1,k2,p1,p2,k3]}
From OpenCV: fx=K[0][0] fy=K[1][1] cx=K[0][2] cy=K[1][2], dist = first five
dist_coeffs, width/height = the image size you calibrated at.
The token comes from src/secrets.h (API_TOKEN) unless --token is given.
"""
import argparse
import json
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path


def token_from_secrets():
    try:
        text = (Path(__file__).parent.parent / "src" / "secrets.h").read_text()
    except OSError:
        return ""
    m = re.search(r'#define\s+API_TOKEN\s+"(.*)"', text)
    return m.group(1) if m else ""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file", nargs="?", help="calibration JSON to upload")
    ap.add_argument("-H", "--host", default="192.168.4.1", help="board IP (default: its AP)")
    ap.add_argument("--token", default=None)
    ap.add_argument("--clear", action="store_true", help="forget the stored calibration")
    args = ap.parse_args()

    body = None
    method = "GET"
    if args.clear:
        method = "DELETE"
    elif args.file:
        body = json.dumps(json.loads(Path(args.file).read_text())).encode()  # validates the JSON
        method = "POST"

    headers = {"Content-Type": "application/json"}
    if method != "GET":
        headers["X-Token"] = args.token if args.token is not None else token_from_secrets()
    req = urllib.request.Request(f"http://{args.host}/calibration", data=body,
                                 headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            print(json.dumps(json.loads(r.read()), indent=2))
    except urllib.error.HTTPError as e:
        sys.exit(f"{e.code} {e.reason}: {e.read().decode(errors='replace').strip()}")
    except urllib.error.URLError as e:
        sys.exit(f"cannot reach {args.host}: {e.reason}")


if __name__ == "__main__":
    main()
