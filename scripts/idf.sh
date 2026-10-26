#!/usr/bin/env bash
# Run idf.py with ESP-IDF v5.5.5, loading its environment first if this shell
# hasn't (`. $IDF_PATH/export.sh`). The Taskfile and the scripts call this.
#   scripts/idf.sh build
#   IDF_PATH=/elsewhere/esp-idf scripts/idf.sh build
set -euo pipefail

IDF_WANT=v5.5.5

if ! command -v idf.py >/dev/null 2>&1; then
  : "${IDF_PATH:=$HOME/esp/$IDF_WANT/esp-idf}"
  if [[ ! -f "$IDF_PATH/export.sh" ]]; then
    echo "ESP-IDF not found at $IDF_PATH — install $IDF_WANT (see README) or set IDF_PATH" >&2
    exit 1
  fi
  # shellcheck disable=SC1091
  . "$IDF_PATH/export.sh" >/dev/null
fi

have=$(cd "$IDF_PATH" && git describe --tags 2>/dev/null || echo unknown)
[[ "$have" == "$IDF_WANT" ]] || echo "WARNING: ESP-IDF $have at $IDF_PATH, this project uses $IDF_WANT" >&2

cd "$(dirname "$0")/.."
exec idf.py "$@"
