#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_DIR="$ROOT_DIR/app/neural-pedal-interface"

if [ ! -d "$APP_DIR" ]; then
  echo "Missing app directory: $APP_DIR" >&2
  exit 1
fi

pushd "$APP_DIR" >/dev/null
if [ ! -d node_modules ]; then
  echo "Installing app dependencies (npm workspaces)..."
  npm install
fi

npm run dev
popd >/dev/null
