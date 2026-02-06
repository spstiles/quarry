#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [--debug|--release|--relwithdebinfo] [--clean]

Environment:
  BUILD_DIR   Build directory (default: $ROOT_DIR/build)
  BUILD_TYPE  CMake build type (default: RelWithDebInfo)

Examples:
  $0
  $0 --debug
  BUILD_DIR=build-clang $0 --clean
EOF
}

clean=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --debug) BUILD_TYPE=Debug ;;
    --release) BUILD_TYPE=Release ;;
    --relwithdebinfo) BUILD_TYPE=RelWithDebInfo ;;
    --clean) clean=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage; exit 2 ;;
  esac
  shift
done

if [[ "$clean" -eq 1 ]]; then
  rm -rf -- "$BUILD_DIR"
fi

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" -j

