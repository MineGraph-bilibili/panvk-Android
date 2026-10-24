#!/bin/sh
# build-all.sh — host tools + android + glibc for a profile
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROFILE="${1:-}"; [ -n "$PROFILE" ] || { echo "usage: build-all.sh --profile <p>" >&2; exit 2; }
"$ROOT/scripts/bootstrap-host-tools.sh"
"$ROOT/scripts/build-android.sh" --profile "$2"
"$ROOT/scripts/build-glibc.sh" --profile "$2"
