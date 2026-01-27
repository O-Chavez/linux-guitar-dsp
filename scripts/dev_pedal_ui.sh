#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE_CMD=("$ROOT_DIR/start_alsa.sh" start-lowlat)
ENGINE_STOP=("$ROOT_DIR/start_alsa.sh" stop)
APP_DIR="$ROOT_DIR/app/neural-pedal-interface"

if ! command -v npm >/dev/null 2>&1; then
  echo "Error: npm not found in PATH." >&2
  echo "Install Node.js + npm (recommended Node 18/20 LTS), then re-run." >&2
  echo "Ubuntu/Debian: sudo apt-get update && sudo apt-get install -y nodejs npm" >&2
  echo "Or use nvm: https://github.com/nvm-sh/nvm" >&2
  exit 127
fi

# UI tooling requires Node >= 20.19.0 (Vite/rolldown-vite).
NODE_VERSION="$(node -v 2>/dev/null | sed 's/^v//' || true)"
NODE_MAJOR="$(printf '%s' "$NODE_VERSION" | cut -d. -f1)"
NODE_MINOR="$(printf '%s' "$NODE_VERSION" | cut -d. -f2)"
if [ "${ALLOW_UNSUPPORTED_NODE:-0}" != "1" ]; then
  if [ -z "$NODE_VERSION" ]; then
    echo "Error: node not found in PATH." >&2
    exit 127
  fi
  if [ "${NODE_MAJOR:-0}" -lt 20 ] || { [ "${NODE_MAJOR:-0}" -eq 20 ] && [ "${NODE_MINOR:-0}" -lt 19 ]; }; then
    echo "Error: Node v$NODE_VERSION detected; UI requires Node >= 20.19.0 (or 22.12+)." >&2
    echo "Recommended: cd app/neural-pedal-interface && nvm install && nvm use" >&2
    echo "Override (not recommended): ALLOW_UNSUPPORTED_NODE=1" >&2
    exit 2
  fi
fi

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
