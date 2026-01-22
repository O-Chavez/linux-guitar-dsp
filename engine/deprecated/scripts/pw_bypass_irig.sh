#!/usr/bin/env bash
set -euo pipefail

# Direct monitor / bypass wiring:
# iRig capture -> iRig playback (bypasses dsp_engine_pw entirely)
#
# Usage:
#   scripts/pw_bypass_irig.sh
#
# Notes:
# - We prefer AUX2 (confirmed instrument input) when available, else MONO.
# - We explicitly disconnect anything feeding the iRig playback sinks first.

IRIG_IN_MONO='alsa_input.usb-IK_Multimedia_iRig_HD_X_1001073-02.mono-fallback:capture_MONO'
IRIG_OUT_L='alsa_output.usb-IK_Multimedia_iRig_HD_X_1001073-02.analog-stereo:playback_FL'
IRIG_OUT_R='alsa_output.usb-IK_Multimedia_iRig_HD_X_1001073-02.analog-stereo:playback_FR'

port_exists() {
  pw-link -io 2>/dev/null | grep -qF "$1"
}

pick_irig_in() {
  # Prefer AUX2 specifically (confirmed instrument input on this iRig/profile).
  local cand
  cand="$(pw-link -io 2>/dev/null | grep -oE 'alsa_input\.usb-IK_Multimedia_iRig_HD_X_[^:]+:capture_AUX2' | head -n 1 || true)"
  if [ -n "$cand" ]; then
    echo "$cand"
    return 0
  fi

  # Fallback to MONO.
  if port_exists "$IRIG_IN_MONO"; then
    echo "$IRIG_IN_MONO"
    return 0
  fi

  # Last resort: any AUX.
  cand="$(pw-link -io 2>/dev/null | grep -oE 'alsa_input\.usb-IK_Multimedia_iRig_HD_X_[^:]+:capture_AUX[0-9]+' | head -n 1 || true)"
  if [ -n "$cand" ]; then
    echo "$cand"
    return 0
  fi

  echo ""
  return 1
}

disconnect_sink() {
  local sink="$1"
  if ! port_exists "$sink"; then
    return 0
  fi
  pw-link -l 2>/dev/null | awk -v sink="$sink" '
    $0==sink {ins=1; next}
    ins && $0 ~ /^\s*\|<-/ {gsub(/^\s*\|<-\s*/,"",$0); print $0}
    ins && $0 !~ /^\s*\|<-/ && $0!="" {ins=0}
  ' | while read -r src; do
    [ -n "$src" ] && pw-link -d "$src" "$sink" >/dev/null 2>&1 || true
  done
}

link_optional() {
  local src="$1"
  local dst="$2"
  if ! port_exists "$src"; then
    echo "skip: missing src $src" >&2
    return 0
  fi
  if ! port_exists "$dst"; then
    echo "skip: missing dst $dst" >&2
    return 0
  fi
  if pw-link "$src" "$dst" >/dev/null 2>&1; then
    echo "ok: $src -> $dst"
    return 0
  fi
  local out
  out="$(pw-link "$src" "$dst" 2>&1 || true)"
  out="$(echo "$out" | tr '\n' ' ' | tr -s ' ')"
  if echo "$out" | grep -qiE 'file exists'; then
    echo "ok: $src -> $dst (already)"
    return 0
  fi
  echo "err: failed to link $src -> $dst" >&2
  echo "     $out" >&2
  return 1
}

IRIG_IN="$(pick_irig_in || true)"
if [ -z "$IRIG_IN" ]; then
  echo "error: no iRig capture port found" >&2
  exit 1
fi

echo "Bypass wiring (direct DI monitor):"
echo "  IN : $IRIG_IN"
echo "  OUT: $IRIG_OUT_L / $IRIG_OUT_R"

# Make sure only our bypass feeds the iRig playback sinks.
disconnect_sink "$IRIG_OUT_L" || true
disconnect_sink "$IRIG_OUT_R" || true

# Feed capture mono into both playback channels.
link_optional "$IRIG_IN" "$IRIG_OUT_L"
link_optional "$IRIG_IN" "$IRIG_OUT_R"

# Print the active links for sanity.
echo
echo "Active links (iRig playback sinks):"
pw-link -l 2>/dev/null | awk '
  /alsa_output\.usb-IK_Multimedia_iRig_HD_X_.*:playback_FL/ {show=1}
  show {print}
  show && $0=="" {show=0}
  /alsa_output\.usb-IK_Multimedia_iRig_HD_X_.*:playback_FR/ {show=1}
  show {print}
  show && $0=="" {show=0}
' | sed -n '1,40p'
