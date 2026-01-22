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

cleanup() {
  echo ""
  echo "Stopping DSP engine..."
  "$ROOT_DIR/start_alsa.sh" stop || true
}
trap cleanup EXIT INT TERM

tail_engine_log() {
  local n="${1:-120}"
  local p

  # start_alsa.sh writes here by default (falls back to /tmp if it can't create it).
  for p in "${XDG_STATE_HOME:-$HOME/.local/state}/dsp-engine-v1/dsp_engine_alsa.log" \
           "$HOME/.local/state/dsp-engine-v1/dsp_engine_alsa.log" \
           "/tmp/dsp-engine-v1/dsp_engine_alsa.log"; do
    if [ -f "$p" ]; then
      echo "--- tail $n: $p ---" >&2
      tail -n "$n" "$p" >&2 || true
      echo "--- end log ---" >&2
      return 0
    fi
  done

  echo "(engine log not found; expected under ~/.local/state/dsp-engine-v1 or /tmp/dsp-engine-v1)" >&2
}

wait_for_engine_ready() {
  local deadline now
  deadline=$(( $(date +%s) + WAIT_SOCK_SECS ))

  while :; do
    if ! kill -0 "$ENGINE_WRAPPER_PID" 2>/dev/null; then
      echo "Engine wrapper exited during startup." >&2
      tail_engine_log 200
      return 1
    fi

    if [ "$WAIT_SOCK" = "1" ] && [ -S "$DSP_SOCK_PATH" ]; then
      echo "DSP control socket ready: $DSP_SOCK_PATH"
      return 0
    fi

    now=$(date +%s)
    if [ "$WAIT_SOCK" != "1" ] || [ "$now" -ge "$deadline" ]; then
      if [ "$WAIT_SOCK" = "1" ] && [ ! -S "$DSP_SOCK_PATH" ]; then
        echo "Warning: DSP control socket not found after ${WAIT_SOCK_SECS}s ($DSP_SOCK_PATH). Continuing..." >&2
      fi
      return 0
    fi

    sleep 0.1
  done
}

build_engine_if_needed

echo "Starting DSP engine (ALSA) profile=$ENGINE_PROFILE ..."
"$ROOT_DIR/start_alsa.sh" "$ENGINE_PROFILE" &
ENGINE_WRAPPER_PID=$!

wait_for_engine_ready

# UI stack (installs deps if missing)
"$ROOT_DIR/scripts/dev_app.sh"

# If the app exits, stop the engine via trap.
wait "$ENGINE_WRAPPER_PID" 2>/dev/null || true
