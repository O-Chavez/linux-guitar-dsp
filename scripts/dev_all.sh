#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_DIR="$ROOT_DIR/app/neural-pedal-interface"
BUILD_DIR="$ROOT_DIR/build"
ENGINE_BIN="$BUILD_DIR/dsp_engine_alsa"

ENGINE_PROFILE="${ENGINE_PROFILE:-start-lowlat}"  # start-lowlat | start-safe
SKIP_BUILD="${SKIP_BUILD:-0}"
REBUILD="${REBUILD:-0}"

# Engine control socket (readiness signal for the UI/server).
DSP_SOCK_PATH="${DSP_CONTROL_SOCK:-/tmp/pedal-dsp.sock}"
WAIT_SOCK_SECS="${WAIT_SOCK_SECS:-5}"
WAIT_SOCK="${WAIT_SOCK:-1}"

usage() {
  cat <<EOF
Usage: $0 [--no-build] [--rebuild] [--safe] [--no-wait-sock]

Runs full dev stack:
  1) (optional) CMake configure/build into ./build
  2) start DSP engine (ALSA)
  3) start Node/React dev stack (npm workspaces)

Env:
  ENGINE_PROFILE=start-lowlat|start-safe  (default: start-lowlat)
  SKIP_BUILD=1                           (same as --no-build)
  REBUILD=1                              (same as --rebuild)
  DSP_CONTROL_SOCK=/path/to.sock         (default: /tmp/pedal-dsp.sock)
  WAIT_SOCK=0                            (same as --no-wait-sock)
  WAIT_SOCK_SECS=5                       (wait timeout in seconds)
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --no-build)
      SKIP_BUILD=1
      shift
      ;;
    --rebuild)
      REBUILD=1
      shift
      ;;
    --safe)
      ENGINE_PROFILE="start-safe"
      shift
      ;;
    --no-wait-sock)
      WAIT_SOCK=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      usage
      exit 2
      ;;
  esac
done

if [ ! -d "$APP_DIR" ]; then
  echo "Missing app directory: $APP_DIR" >&2
  exit 1
fi

build_engine_if_needed() {
  if [ "$SKIP_BUILD" = "1" ]; then
    return 0
  fi

  if [ "$REBUILD" = "1" ]; then
    echo "Rebuilding engine: removing $BUILD_DIR ..."
    rm -rf "$BUILD_DIR"
  fi

  if [ -x "$ENGINE_BIN" ]; then
    return 0
  fi

  echo "Engine binary missing; building to $BUILD_DIR ..."
  cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD_DIR" -j
}

wait_for_socket() {
  if [ "$WAIT_SOCK" != "1" ]; then
    return 0
  fi

  # Wait briefly for the engine to expose its control socket before starting the UI.
  # This reduces race conditions where the server/UI boots before the engine is ready.
  local deadline now
  deadline=$(( $(date +%s) + WAIT_SOCK_SECS ))
  while :; do
    if [ -S "$DSP_SOCK_PATH" ]; then
      echo "DSP control socket ready: $DSP_SOCK_PATH"
      return 0
    fi
    now=$(date +%s)
    if [ "$now" -ge "$deadline" ]; then
      echo "Warning: DSP control socket not found after ${WAIT_SOCK_SECS}s ($DSP_SOCK_PATH). Continuing..." >&2
      return 0
    fi
    sleep 0.1
  done
}

cleanup() {
  echo ""
  echo "Stopping DSP engine..."
  "$ROOT_DIR/start_alsa.sh" stop || true
}
trap cleanup EXIT INT TERM

build_engine_if_needed

echo "Starting DSP engine (ALSA) profile=$ENGINE_PROFILE ..."
"$ROOT_DIR/start_alsa.sh" "$ENGINE_PROFILE" &
ENGINE_WRAPPER_PID=$!

wait_for_socket

# UI stack (installs deps if missing)
"$ROOT_DIR/scripts/dev_app.sh"

# If the app exits, stop the engine via trap.
wait "$ENGINE_WRAPPER_PID" 2>/dev/null || true
