#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
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
    echo "Recommended: use nvm and the pinned version in app/neural-pedal-interface/.nvmrc" >&2
    echo "  cd app/neural-pedal-interface && nvm install && nvm use" >&2
    echo "Override (not recommended): ALLOW_UNSUPPORTED_NODE=1" >&2
    exit 2
  fi
fi

if [ ! -d "$APP_DIR" ]; then
  echo "Missing app directory: $APP_DIR" >&2
  exit 1
fi

pushd "$APP_DIR" >/dev/null

needs_reinstall=0

# If the app folder was copied with preexisting node_modules (e.g. from a zip),
# executable bits on node_modules/.bin shims can be lost, causing "Permission denied".
for bin in \
  "apps/ui/node_modules/.bin/vite" \
  "apps/server/node_modules/.bin/ts-node-dev" \
  "packages/shared/node_modules/.bin/tsc"; do
  if [ -e "$bin" ] && [ ! -x "$bin" ]; then
    needs_reinstall=1
    break
  fi
done

if [ "${FORCE_NPM_INSTALL:-0}" = "1" ]; then
  needs_reinstall=1
fi

if [ ! -d node_modules ] || [ "$needs_reinstall" = "1" ]; then
  if [ "$needs_reinstall" = "1" ]; then
    echo "Detected non-executable workspace bin shims; cleaning node_modules for a fresh install..."
    rm -rf node_modules apps/*/node_modules packages/*/node_modules
  fi

  echo "Installing app dependencies (npm workspaces)..."
  if [ -f package-lock.json ]; then
    if ! npm ci --include=optional; then
      echo "npm ci failed; retrying with npm install (optional deps included)..." >&2
      rm -rf node_modules apps/*/node_modules packages/*/node_modules
      npm install --include=optional
    fi
  else
    npm install --include=optional
  fi
fi

npm run dev
popd >/dev/null
