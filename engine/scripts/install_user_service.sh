#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UNIT_ALSA_SRC="$ROOT_DIR/systemd/dsp-engine-alsa.service"
UNIT_DST_DIR="$HOME/.config/systemd/user"
UNIT_ALSA_DST="$UNIT_DST_DIR/dsp-engine-alsa.service"

mkdir -p "$UNIT_DST_DIR"
mkdir -p "$HOME/.cache"
cp -f "$UNIT_ALSA_SRC" "$UNIT_ALSA_DST"

echo "Installed user units to:"
echo "  $UNIT_ALSA_DST"

echo "Reloading user units..."
systemctl --user daemon-reload

echo "Disabling any auto-start services (alsa)"
systemctl --user disable --now dsp-engine-alsa.service 2>/dev/null || true

echo "Done. Status:"
systemctl --user status dsp-engine-alsa.service --no-pager -n 5 || true

echo "Logs:"
echo "  engine (alsa): ${XDG_STATE_HOME:-$HOME/.local/state}/dsp-engine-v1/dsp_engine_alsa.log"
echo "  service (alsa): $HOME/.cache/dsp_engine_alsa.service.log"
