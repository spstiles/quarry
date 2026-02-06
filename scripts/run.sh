#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"

if [[ ! -x "$BUILD_DIR/quarry" ]]; then
  echo "Executable not found at: $BUILD_DIR/quarry" >&2
  echo "Build first: $ROOT_DIR/scripts/build.sh" >&2
  exit 1
fi

if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
  echo "No GUI session detected (DISPLAY/WAYLAND_DISPLAY not set)." >&2
  echo "Run this from a desktop session, or over SSH with X11/Wayland forwarding." >&2
fi

exec "$BUILD_DIR/quarry" "$@"
