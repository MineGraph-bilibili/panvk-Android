#!/bin/sh
# fetch-mesa.sh — clone/fetch the exact pinned Mesa commit from sources.lock
# Usage: ./scripts/fetch-mesa.sh [--work work] [--checkout-only]
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${1:-$ROOT/work}"
MESA_DIR="$WORK/mesa"
REPO="$(python3 -c "import json;print(json.load(open('$ROOT/sources.lock'))['mesaRepo'])")"
COMMIT="$(python3 -c "import json;print(json.load(open('$ROOT/sources.lock'))['mesaCommit'])")"
mkdir -p "$WORK"
if [ ! -d "$MESA_DIR/.git" ]; then
  git clone "$REPO" "$MESA_DIR"
fi
git -C "$MESA_DIR" fetch origin "$COMMIT" --depth 1
git -C "$MESA_DIR" checkout --detach "$COMMIT"
git -C "$MESA_DIR" reset --hard "$COMMIT"
git -C "$MESA_DIR" clean -fdq -- src/ meson.build meson.options || true
git -C "$MESA_DIR" rev-parse HEAD
git -C "$MESA_DIR" describe --tags 2>/dev/null || true
echo "OK mesa=$COMMIT dir=$MESA_DIR"
