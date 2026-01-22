#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE_CMD=("$ROOT_DIR/start_alsa.sh" start-lowlat)
ENGINE_STOP=("$ROOT_DIR/start_alsa.sh" stop)
APP_DIR="$ROOT_DIR/app/neural-pedal-interface"

if [ ! -d "$APP_DIR" ]; then
  echo "Missing app directory: $APP_DIR" >&2
  exit 1
fi

cleanup() {
  echo ""
  echo "Stopping DSP engine..."
  "${ENGINE_STOP[@]}" || true
}
trap cleanup EXIT INT TERM

pushd "$ROOT_DIR" >/dev/null
echo "Starting DSP engine (ALSA)..."
"${ENGINE_CMD[@]}" &
ENGINE_WRAPPER_PID=$!
popd >/dev/null

pushd "$APP_DIR" >/dev/null
if [ ! -d node_modules ]; then
  echo "Installing app dependencies (npm workspaces)..."
  npm install
fi

echo "Starting app dev (shared + server + ui)..."
npm run dev
popd >/dev/null

# If the app exits, stop the engine via trap.
wait "$ENGINE_WRAPPER_PID" 2>/dev/null || true
