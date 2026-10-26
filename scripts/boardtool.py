"""Shared bits for the scripts that talk to a running board."""
import argparse
import re
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


def add_board_args(ap: argparse.ArgumentParser):
    ap.add_argument("-H", "--host", default="wifi-camera.local",
                    help="board address (default: wifi-camera.local; its AP is 192.168.4.1)")
    ap.add_argument("--token", default=None,
                    help="API token (default: API_TOKEN from src/secrets.h)")


def resolve_token(args):
    return args.token if args.token is not None else token_from_secrets()


def http(host, path, method="GET", token=None, body=None, headers=None, timeout=6):
    """(status, headers, body bytes) — HTTP errors are returned, not raised."""
    h = dict(headers or {})
    if token:
        h["X-Token"] = token
    req = urllib.request.Request(f"http://{host}{path}", data=body, method=method, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.headers, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read()
