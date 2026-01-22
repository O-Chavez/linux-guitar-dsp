#!/usr/bin/env bash
set -euo pipefail

# Compatibility shim: install script lives in engine/scripts.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$ROOT_DIR/engine/scripts/install_user_service.sh" "$@"
