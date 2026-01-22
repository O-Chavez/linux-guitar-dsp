#!/usr/bin/env bash
set -euo pipefail

# Compatibility shim: the real launcher lives in engine/start_alsa.sh.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/engine/start_alsa.sh" "$@"
