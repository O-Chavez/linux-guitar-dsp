#!/usr/bin/env bash
set -euo pipefail

# Ensure we talk to the same PipeWire instance as the (user) engine.
# When run under systemd/user the environment is usually correct, but making
# this explicit prevents silent no-ops when variables are missing.
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export PIPEWIRE_REMOTE="${PIPEWIRE_REMOTE:-}"

SNAPSHOT_ONLY="${SNAPSHOT_ONLY:-0}"

IRIG_IN_DEFAULT='alsa_input.usb-IK_Multimedia_iRig_HD_X_1001073-02.mono-fallback:capture_MONO'
# iRig playback ports depend on the selected profile.
# - pro-audio profile exposes playback as AUX ports
# - analog-stereo profile exposes playback_FL/FR
IRIG_OUT_L_DEFAULT='alsa_output.usb-IK_Multimedia_iRig_HD_X_1001073-02.pro-output-0:playback_AUX0'
IRIG_OUT_R_DEFAULT='alsa_output.usb-IK_Multimedia_iRig_HD_X_1001073-02.pro-output-0:playback_AUX1'
IRIG_OUT_L_FALLBACK='alsa_output.usb-IK_Multimedia_iRig_HD_X_1001073-02.analog-stereo:playback_FL'
IRIG_OUT_R_FALLBACK='alsa_output.usb-IK_Multimedia_iRig_HD_X_1001073-02.analog-stereo:playback_FR'

# PipeWire engine ports (dsp_engine_pw).
# v1 now prefers a single duplex node, but we keep the old 2-stream names as
# fallbacks so we can bisect/regress easily.

# Preferred (single node):
# IMPORTANT: Duplex port naming differs depending on the PipeWire stream factory mode.
# We therefore *discover* the actual ports at runtime.
#
# Convention used in this script:
#   DSP_IN_*  : engine input ports (port.direction=in)  -> we feed these from iRig capture
#   DSP_OUT_* : engine output ports (port.direction=out) -> we feed iRig playback from these
DSP_IN_L=''
DSP_IN_R=''
DSP_OUT_L=''
DSP_OUT_R=''

# Old (2-stream) fallbacks:
DSP_IN_L_OLD='dsp_engine_v1.capture:input_FL'
DSP_IN_R_OLD='dsp_engine_v1.capture:input_FR'
DSP_IN_MONO_OLD='dsp_engine_v1.capture:input_MONO'
DSP_IN_0_OLD='dsp_engine_v1.capture:input_0'
DSP_OUT_L_OLD='dsp_engine_v1.playback:output_0'
DSP_OUT_R_OLD='dsp_engine_v1.playback:output_1'
DSP_OUT_L_OLD_FALLBACK='dsp_engine_v1.playback:output_FL'
DSP_OUT_R_OLD_FALLBACK='dsp_engine_v1.playback:output_FR'
DSP_OUT_L_OLD_MON='dsp_engine_v1.playback:monitor_FL'
DSP_OUT_R_OLD_MON='dsp_engine_v1.playback:monitor_FR'
DSP_MON_L_OLD='dsp_engine_v1.capture:monitor_FL'
DSP_MON_R_OLD='dsp_engine_v1.capture:monitor_FR'

# Per-run debug snapshot path (helps when stdout/stderr is truncated or lost).
STATE_DIR_DEFAULT="${XDG_STATE_HOME:-$HOME/.local/state}"
STATE_DIR="$STATE_DIR_DEFAULT/dsp-engine-v1"
SNAPSHOT_FILE="$STATE_DIR/pw_wire_snapshot.txt"
mkdir -p "$STATE_DIR" 2>/dev/null || true

snapshot_begin() {
  {
    echo "===== pw_wire_irig snapshot $(date -Is) ====="
    echo "XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-}"
    echo "PIPEWIRE_REMOTE=${PIPEWIRE_REMOTE:-}"
  } >"$SNAPSHOT_FILE" 2>/dev/null || true
}

snapshot_kv() {
  # Usage: snapshot_kv key value...
  local k="$1"; shift || true
  echo "$k=$*" >>"$SNAPSHOT_FILE" 2>/dev/null || true
}

snapshot_note() {
  echo "$*" >>"$SNAPSHOT_FILE" 2>/dev/null || true
}

PORTS=""

port_exists() {
  if [ -n "${PORTS:-}" ]; then
	grep -qF -- "$1" <<<"$PORTS"
	return $?
  fi
  pw-link -io 2>/dev/null | grep -qF -- "$1"
}

pwlink_list_ports() {
  # List only the port endpoint names (first column).
  if [ -n "${PORTS:-}" ]; then
	awk '{print $1}' <<<"$PORTS" || true
	return 0
  fi
  pw-link -io 2>/dev/null | awk '{print $1}' || true
}

pwcli_paths_by_dir() {
  # Usage: pwcli_paths_by_dir <regex-on-object.path> <in|out>
  local re="$1" want="$2"
  # NOTE: awk uses POSIX ERE (not PCRE). Ensure callers pass a compatible ERE.
  pw-cli ls Port 2>/dev/null | awk -v re="$re" -v want="$want" '
    BEGIN{RS=""; FS="\n"}
    {
      path=""; dir=""
      for (i=1;i<=NF;i++){
        if ($i ~ /object\.path = "/) { line=$i; sub(/.*object\.path = "/,"",line); sub(/".*/,"",line); path=line }
        if ($i ~ /port\.direction = "/) { line=$i; sub(/.*port\.direction = "/,"",line); sub(/".*/,"",line); dir=line }
      }
      if (path ~ re && dir==want) print path
    }
  ' || true
}

discover_duplex_via_pwlink() {
  # Prefer pw-link's view when available because those names are guaranteed
  # to be linkable via pw-link.
  #
  # On this system (with the current engine build), the duplex node exposes:
  #   inputs : dsp_engine_v1.duplex:playback_FL/FR   (feed these from iRig capture)
  #   outputs: dsp_engine_v1.duplex:capture_FL/FR    (feed iRig playback from these)
  local pbL pbR capL capR
  pbL="$(pwlink_list_ports | grep -Fx 'dsp_engine_v1.duplex:playback_FL' || true)"
  pbR="$(pwlink_list_ports | grep -Fx 'dsp_engine_v1.duplex:playback_FR' || true)"
  capL="$(pwlink_list_ports | grep -Fx 'dsp_engine_v1.duplex:capture_FL' || true)"
  capR="$(pwlink_list_ports | grep -Fx 'dsp_engine_v1.duplex:capture_FR' || true)"

  # Mirror mono if only one channel exists.
  if [ -n "$pbL" ] || [ -n "$pbR" ]; then
    DSP_IN_L="${pbL:-$pbR}"
    DSP_IN_R="${pbR:-$pbL}"
  fi
  if [ -n "$capL" ] || [ -n "$capR" ]; then
    DSP_OUT_L="${capL:-$capR}"
    DSP_OUT_R="${capR:-$capL}"
  fi

  if [ -n "${DSP_IN_L:-}" ] || [ -n "${DSP_OUT_L:-}" ]; then
    return 0
  fi
  return 1
}

pwcli_list_object_paths() {
  # Usage: pwcli_list_object_paths <regex-on-object.path>
  local re="$1"
  pw-cli ls Port 2>/dev/null \
    | awk 'BEGIN{RS=""} $0 ~ /object\.path = "/ {print $0 "\n---"}' \
    | awk -F'"' '/object\.path = "/ {print $2}' \
    | grep -E "$re" \
    || true
}

discover_duplex_all_ports() {
  # Prefer pw-link namespace because those names are guaranteed linkable.
  # We intentionally avoid pw-cli Port parsing here because on some setups
  # it doesn't expose node.name/port.name/object.path reliably.
  discover_duplex_via_pwlink
}

discover_duplex_ports() {
  discover_duplex_all_ports
}

discover_legacy_ports() {
  # Discover legacy two-stream ports via pw-link namespace.
  # Capture inputs (engine capture stream expects input_* ports).
  local cap_fl cap_fr cap_mono cap_0
  cap_fl="$(pwlink_list_ports | grep -Fx 'dsp_engine_v1.capture:input_FL' || true)"
  cap_fr="$(pwlink_list_ports | grep -Fx 'dsp_engine_v1.capture:input_FR' || true)"
  cap_mono="$(pwlink_list_ports | grep -Fx 'dsp_engine_v1.capture:input_MONO' || true)"
  cap_0="$(pwlink_list_ports | grep -Fx 'dsp_engine_v1.capture:input_0' || true)"

  if [ -n "$cap_fl" ] || [ -n "$cap_fr" ]; then
    DSP_IN_L="${cap_fl:-$cap_fr}"
    DSP_IN_R="${cap_fr:-$cap_fl}"
  elif [ -n "$cap_mono" ]; then
    DSP_IN_L="$cap_mono"
    DSP_IN_R="$cap_mono"
  elif [ -n "$cap_0" ]; then
    DSP_IN_L="$cap_0"
    DSP_IN_R="$cap_0"
  fi

  # Playback outputs (engine playback stream exposes monitor_* or output_*).
  local pb_mon_l pb_mon_r pb_out_l pb_out_r pb_out0 pb_out1
  pb_mon_l="$(pwlink_list_ports | grep -Fx 'dsp_engine_v1.playback:monitor_FL' || true)"
  pb_mon_r="$(pwlink_list_ports | grep -Fx 'dsp_engine_v1.playback:monitor_FR' || true)"
  pb_out_l="$(pwlink_list_ports | grep -Fx 'dsp_engine_v1.playback:output_FL' || true)"
  pb_out_r="$(pwlink_list_ports | grep -Fx 'dsp_engine_v1.playback:output_FR' || true)"
  pb_out0="$(pwlink_list_ports | grep -Fx 'dsp_engine_v1.playback:output_0' || true)"
  pb_out1="$(pwlink_list_ports | grep -Fx 'dsp_engine_v1.playback:output_1' || true)"

  if [ -n "$pb_mon_l" ] && [ -n "$pb_mon_r" ]; then
    DSP_OUT_L="$pb_mon_l"
    DSP_OUT_R="$pb_mon_r"
  elif [ -n "$pb_out_l" ] && [ -n "$pb_out_r" ]; then
    DSP_OUT_L="$pb_out_l"
    DSP_OUT_R="$pb_out_r"
  elif [ -n "$pb_out0" ] && [ -n "$pb_out1" ]; then
    DSP_OUT_L="$pb_out0"
    DSP_OUT_R="$pb_out1"
  fi

  if [ -n "${DSP_IN_L:-}" ] || [ -n "${DSP_OUT_L:-}" ]; then
    return 0
  fi
  return 1
}

wait_for_ports() {
  # When started via systemd, the engine may take a moment before pw-link sees ports.
  # Wait until we can discover ports for either the duplex node or legacy streams.
  local i
  for i in {1..200}; do
    PORTS="$(pw-link -io 2>/dev/null || true)"
    DSP_IN_L=''; DSP_IN_R=''; DSP_OUT_L=''; DSP_OUT_R=''
    if discover_duplex_via_pwlink; then
      if [ -n "${DSP_IN_L:-}" ] || [ -n "${DSP_IN_R:-}" ] || [ -n "${DSP_OUT_L:-}" ] || [ -n "${DSP_OUT_R:-}" ]; then
        return 0
      fi
    fi
    # Legacy two-stream ports (capture + playback monitor outputs).
    if discover_legacy_ports; then
      if [ -n "${DSP_IN_L:-}" ] || [ -n "${DSP_IN_R:-}" ] || [ -n "${DSP_OUT_L:-}" ] || [ -n "${DSP_OUT_R:-}" ]; then
        return 0
      fi
    fi
    sleep 0.05
  done
  return 1
}

pick_irig_in() {
  local cand

  # Allow a brief window for the ALSA nodes/ports to appear.
  local tries
  for tries in 1 2 3 4 5; do
    cand=""
    PORTS="$(pw-link -io 2>/dev/null || true)"

    # Prefer AUX2 specifically (confirmed instrument input on this iRig/profile).
    cand="$(pwlink_list_ports | grep -E 'alsa_input\.usb-IK_Multimedia_iRig_HD_X_[^:]+:capture_AUX2' | head -n 1 || true)"
    # Fallback to AUX0 (common on some devices).
    if [ -z "$cand" ]; then
      cand="$(pwlink_list_ports | grep -E 'alsa_input\.usb-IK_Multimedia_iRig_HD_X_[^:]+:capture_AUX0' | head -n 1 || true)"
    fi
    if [ -z "$cand" ]; then
      cand="$(pwlink_list_ports | grep -E 'alsa_input\.usb-IK_Multimedia_iRig_HD_X_[^:]+:capture_AUX[0-9]+' | head -n 1 || true)"
    fi
    if [ -n "$cand" ]; then
      echo "$cand"
      return 0
    fi

    # Fallback: mono-fallback capture_MONO (older profile)
    if port_exists "$IRIG_IN_DEFAULT"; then
      echo "$IRIG_IN_DEFAULT"
      return 0
    fi

    sleep 0.05
  done

  # Nothing found.
  echo "" 
  return 1
}

pick_irig_out() {
  local tries
  for tries in 1 2 3 4 5; do
    # Prefer pro-audio AUX ports.
    if port_exists "$IRIG_OUT_L_DEFAULT" && port_exists "$IRIG_OUT_R_DEFAULT"; then
      echo "$IRIG_OUT_L_DEFAULT" "$IRIG_OUT_R_DEFAULT"
      return 0
    fi

    # Fallback to analog-stereo playback.
    if port_exists "$IRIG_OUT_L_FALLBACK" && port_exists "$IRIG_OUT_R_FALLBACK"; then
      echo "$IRIG_OUT_L_FALLBACK" "$IRIG_OUT_R_FALLBACK"
      return 0
    fi
    sleep 0.05
  done

  echo "" ""
  return 1
}

link_ok() {
  # pw-link prints "failed ... File exists" when the link already exists.
  # Prefer the exit code when it succeeds (new link created).
  local out
  if pw-link "$1" "$2" >/dev/null 2>&1; then
    return 0
  fi
  out="$(pw-link "$1" "$2" 2>&1 || true)"
  # Some environments hard-wrap output (e.g. injected newlines). Normalize whitespace.
  out="$(echo "$out" | tr '\n' ' ' | tr -s ' ')"
  echo "$out" | grep -qiE 'file exists'
}

pwcli_port_id() {
  # Resolve a pw-link endpoint like "node.name:port.name" to PipeWire's numeric Port id.
  # IMPORTANT: On some PipeWire setups `object.path` is not set on Port objects.
  # We therefore match by node.name + port.name, and fall back to object.path.
  local endpoint="$1"
  local node="${endpoint%%:*}"
  local port="${endpoint#*:}"
  [ -n "${endpoint:-}" ] || { echo ""; return 1; }
  [ -n "${node:-}" ] && [ -n "${port:-}" ] || { echo ""; return 1; }

  pw-cli ls Port 2>/dev/null | awk -v want_ep="$endpoint" -v want_node="$node" -v want_port="$port" '
    BEGIN{RS=""; FS="\n"}
    {
      id=""; path=""; nname=""; pname=""; palias=""
      if (match($1,/^[[:space:]]*([0-9]+)/,m)) id=m[1]
      for (i=1;i<=NF;i++){
        if ($i ~ /object\.path = "/) {
          line=$i; sub(/.*object\.path = "/,"",line); sub(/".*/,"",line); path=line
        }
        if ($i ~ /node\.name = "/) {
          line=$i; sub(/.*node\.name = "/,"",line); sub(/".*/,"",line); nname=line
        }
        if ($i ~ /port\.name = "/) {
          line=$i; sub(/.*port\.name = "/,"",line); sub(/".*/,"",line); pname=line
        }
        if ($i ~ /port\.alias = "/) {
          line=$i; sub(/.*port\.alias = "/,"",line); sub(/".*/,"",line); palias=line
        }
      }
      if (nname==want_node && (pname==want_port || palias==want_port)) {print id; exit}
      if (path==want_ep) {print id; exit}
    }
  ' || true
}

snapshot_dump_port_candidates() {
  # Usage: snapshot_dump_port_candidates <endpoint>
  # Emits a compact summary of node/port keys present on this system.
  local endpoint="$1"
  local node="${endpoint%%:*}"
  local port="${endpoint#*:}"
  snapshot_note "dbg: port-id resolution failed for: $endpoint"
  snapshot_note "dbg: looking for node='$node' port='$port'"
  snapshot_note "dbg: keys present (sample):"
  # Show a few representative key lines from the first matching node-name group.
  pw-cli ls Port 2>/dev/null \
    | awk -v n="$node" 'BEGIN{RS=""} $0 ~ ("node.name = \"" n "\"") {print $0"\n---"; c++; if(c>=1) exit}' \
    | grep -E 'node\.name =|node\.id =|port\.name =|port\.alias =|object\.path =|object\.serial =|port\.id ='
  snapshot_note "dbg: candidate node.name values containing 'dsp_engine_v1' (first 10):"
  pw-cli ls Port 2>/dev/null | awk 'BEGIN{RS=""; FS="\n"} {for(i=1;i<=NF;i++) if($i ~ /node\.name = "/){l=$i; sub(/.*node\.name = "/,"",l); sub(/".*/,"",l); print l}}' \
    | grep -F 'dsp_engine_v1' | sort -u | head -n 10 | sed 's/^/  /'
  snapshot_note "dbg: candidate port.name/alias containing 'playback|capture' (first 20):"
  pw-cli ls Port 2>/dev/null | awk 'BEGIN{RS=""; FS="\n"} {for(i=1;i<=NF;i++){if($i ~ /port\.name = "/){l=$i; sub(/.*port\.name = "/,"",l); sub(/".*/,"",l); print l}
    if($i ~ /port\.alias = "/){l=$i; sub(/.*port\.alias = "/,"",l); sub(/".*/,"",l); print l}}}' \
    | grep -E 'playback|capture' | sort -u | head -n 20 | sed 's/^/  /'
}

pwcli_link_exists_by_port_ids() {
  # Usage: pwcli_link_exists_by_port_ids <out_port_id> <in_port_id>
  local out_id="$1" in_id="$2"
  [ -n "${out_id:-}" ] && [ -n "${in_id:-}" ] || return 1
  pw-cli ls Link 2>/dev/null | awk -v o="$out_id" -v i="$in_id" '
    BEGIN{RS=""; FS="\n"}
    {
      op=""; ip=""
      for (k=1;k<=NF;k++){
        if ($k ~ /out\.port\.id = /) { line=$k; sub(/.*out\.port\.id = /,"",line); sub(/[^0-9].*/,"",line); op=line }
        if ($k ~ /in\.port\.id = /)  { line=$k; sub(/.*in\.port\.id = /,"",line);  sub(/[^0-9].*/,"",line); ip=line }
      }
      if (op==o && ip==i) {found=1; exit}
    }
    END{exit(found?0:1)}
  '
}

link_exists_pwlink() {
  # Verify that PipeWire currently has a link from src -> dst using pw-link's own view.
  local src="$1" dst="$2"
  [ -n "${src:-}" ] && [ -n "${dst:-}" ] || return 1
  pw-link -l 2>/dev/null | awk -v dst="$dst" -v src="$src" '
    $0==dst {ins=1; next}
    ins && $0 ~ /^\s*\|<-/ {
      gsub(/^\s*\|<-\s*/,"",$0);
      if ($0==src) {found=1}
      next
    }
    ins && $0 !~ /^\s*\|<-/ && $0!="" {ins=0}
    END {exit(found?0:1)}
  '
}

link_exists() {
  # Validate links using pw-link -l to avoid depending on pw-cli Port metadata,
  # which can be missing in some environments.
  link_exists_pwlink "$1" "$2"
}

link_required() {
  # Like link_optional, but failing to create/verify the link is fatal.
  local src="$1"
  local dst="$2"
  [ -n "${src:-}" ] && [ -n "${dst:-}" ] || { echo "err: missing endpoint src='${src:-}' dst='${dst:-}'" >&2; return 1; }

  if ! port_exists "$src"; then
    echo "err: missing src $src" >&2
    return 1
  fi
  if ! port_exists "$dst"; then
    echo "err: missing dst $dst" >&2
    return 1
  fi

  # Try a few times: PipeWire graph updates can race startup.
  local attempt
  for attempt in 1 2 3 4 5; do
    link_ok "$src" "$dst" >/dev/null 2>&1 || true
    if link_exists "$src" "$dst"; then
      echo "ok: $src -> $dst"
      return 0
    fi
    sleep 0.05
  done

  echo "err: required link not present after retries: $src -> $dst" >&2
  # Debug help: show resolved port ids (if any)
  echo "dbg: src_port_id=$(pwcli_port_id "$src" || true) dst_port_id=$(pwcli_port_id "$dst" || true)" >&2
  snapshot_note "ERR required link missing: $src -> $dst"
  snapshot_kv "src_port_id" "$(pwcli_port_id "$src" || true)"
  snapshot_kv "dst_port_id" "$(pwcli_port_id "$dst" || true)"
  return 1
}

link_optional() {
  local src="$1"
  local dst="$2"
  if [ -z "${src:-}" ] || [ -z "${dst:-}" ]; then
    echo "skip: empty endpoint src='${src:-}' dst='${dst:-}'" >&2
    return 0
  fi
  if ! port_exists "$src"; then
    echo "skip: missing src $src" >&2
    return 0
  fi
  if ! port_exists "$dst"; then
    echo "skip: missing dst $dst" >&2
    return 0
  fi

  # Try linking, then verify it actually exists in the live graph.
  local attempt
  for attempt in 1 2 3; do
    link_ok "$src" "$dst" >/dev/null 2>&1 || true
    if link_exists "$src" "$dst"; then
      echo "ok: $src -> $dst"
      return 0
    fi
    sleep 0.02
  done

  echo "warn: link not present after pw-link retries: $src -> $dst" >&2
  return 1
}

disconnect_sink() {
  local sink="$1"
  if ! port_exists "$sink"; then
    return 0
  fi
  # Disconnect anything feeding this sink.
  pw-link -l 2>/dev/null | awk -v sink="$sink" '
    $0==sink {ins=1; next}
    ins && $0 ~ /^\s*\|<-/ {gsub(/^\s*\|<-\s*/,"",$0); print $0}
    ins && $0 !~ /^\s*\|<-/ && $0!="" {ins=0}
  ' | while read -r src; do
    [ -n "$src" ] && pw-link -d "$src" "$sink" >/dev/null 2>&1 || true
  done
}

rc=0

# Diagnostic mode: only wire DSP -> playback (sink). Do not feed capture into DSP.
# Usage:
#   PW_WIRE_OUTPUT_ONLY=1 ./pw_wire_irig.sh
PW_WIRE_OUTPUT_ONLY="${PW_WIRE_OUTPUT_ONLY:-0}"

snapshot_begin

if ! wait_for_ports; then
  echo "warn: dsp engine ports not found yet (continuing anyway)" >&2
  snapshot_note "WARN dsp engine ports not found yet (continuing anyway)"
  # One more best-effort discovery pass (legacy or duplex).
  DSP_IN_L=''; DSP_IN_R=''; DSP_OUT_L=''; DSP_OUT_R=''
  discover_duplex_via_pwlink || true
  if [ -z "${DSP_IN_L:-}" ] && [ -z "${DSP_OUT_L:-}" ]; then
    discover_legacy_ports || true
  fi
fi

# Cache port list once per run to reduce repeated pw-link calls.
PORTS="$(pw-link -io 2>/dev/null || true)"

IRIG_IN="$(pick_irig_in || true)"
if [ -z "$IRIG_IN" ]; then
  echo "skip: no iRig capture port found" >&2
  snapshot_note "WARN no iRig capture port found"
fi
snapshot_kv "IRIG_IN" "$IRIG_IN"
snapshot_kv "IRIG_IN_visible" "$(port_exists "$IRIG_IN" && echo yes || echo no)"

# Snapshot-only mode: capture discovery info but do not modify the graph.
if [ "$SNAPSHOT_ONLY" = "1" ]; then
  snapshot_note "SNAPSHOT_ONLY=1 (no wiring changes performed)"
  snapshot_kv "DSP_IN_L" "${DSP_IN_L:-}"
  snapshot_kv "DSP_IN_R" "${DSP_IN_R:-}"
  snapshot_kv "DSP_OUT_L" "${DSP_OUT_L:-}"
  snapshot_kv "DSP_OUT_R" "${DSP_OUT_R:-}"
  # Still try to capture playback port visibility for diagnostics.
  read -r IRIG_OUT_L IRIG_OUT_R < <(pick_irig_out || true)
  snapshot_kv "IRIG_OUT_L" "$IRIG_OUT_L"
  snapshot_kv "IRIG_OUT_R" "$IRIG_OUT_R"
  snapshot_kv "IRIG_OUT_L_visible" "$(port_exists "$IRIG_OUT_L" && echo yes || echo no)"
  snapshot_kv "IRIG_OUT_R_visible" "$(port_exists "$IRIG_OUT_R" && echo yes || echo no)"
  if [ -n "${DSP_OUT_L:-}" ] || [ -n "${DSP_OUT_R:-}" ]; then
    snapshot_kv "DSP_OUT_L_visible" "$(port_exists "$DSP_OUT_L" && echo yes || echo no)"
    snapshot_kv "DSP_OUT_R_visible" "$(port_exists "$DSP_OUT_R" && echo yes || echo no)"
  fi
  if [ -n "${DSP_IN_L:-}" ] || [ -n "${DSP_IN_R:-}" ]; then
    snapshot_kv "DSP_IN_L_visible" "$(port_exists "$DSP_IN_L" && echo yes || echo no)"
    snapshot_kv "DSP_IN_R_visible" "$(port_exists "$DSP_IN_R" && echo yes || echo no)"
  fi
  snapshot_note "--- pw-link -l (filtered) ---"
  pw-link -l 2>/dev/null | grep -E "dsp_engine_v1\.duplex|dsp_engine_v1\.capture|dsp_engine_v1\.playback|alsa_(input|output)\.usb-IK_Multimedia_iRig_HD_X_" >>"$SNAPSHOT_FILE" || true
  exit 0
fi

# Refresh cached ports before wiring.
PORTS="$(pw-link -io 2>/dev/null || true)"

# capture mono -> dsp inputs (duplicate to both FL/FR is fine)
# If we found duplex inputs, these are REQUIRED for the engine to ever produce output.
if [ -n "$IRIG_IN" ]; then
  if [ -n "${DSP_IN_L:-}" ] || [ -n "${DSP_IN_R:-}" ]; then
  snapshot_kv "DSP_IN_L" "$DSP_IN_L"
  snapshot_kv "DSP_IN_R" "$DSP_IN_R"
  snapshot_kv "DSP_IN_L_visible" "$(port_exists "$DSP_IN_L" && echo yes || echo no)"
  snapshot_kv "DSP_IN_R_visible" "$(port_exists "$DSP_IN_R" && echo yes || echo no)"
    if [ "$PW_WIRE_OUTPUT_ONLY" != "1" ]; then
      link_required "$IRIG_IN" "$DSP_IN_L" || rc=1
      link_required "$IRIG_IN" "$DSP_IN_R" || rc=1
    else
      echo "diag: PW_WIRE_OUTPUT_ONLY=1 (skipping capture -> dsp links)" >&2
      snapshot_note "diag: output-only wiring (skipping capture -> dsp links)"
    fi
  else
    # Old node fallback (treat as optional; some builds don't have this)
    if [ "$PW_WIRE_OUTPUT_ONLY" != "1" ]; then
      # Legacy capture ports may expose only a single input (e.g. input_FR). Mirror if needed.
      if port_exists "$DSP_IN_L_OLD" && port_exists "$DSP_IN_R_OLD"; then
        link_optional "$IRIG_IN" "$DSP_IN_L_OLD" || true
        link_optional "$IRIG_IN" "$DSP_IN_R_OLD" || true
      elif port_exists "$DSP_IN_L_OLD"; then
        link_optional "$IRIG_IN" "$DSP_IN_L_OLD" || true
        link_optional "$IRIG_IN" "$DSP_IN_L_OLD" || true
      elif port_exists "$DSP_IN_R_OLD"; then
        link_optional "$IRIG_IN" "$DSP_IN_R_OLD" || true
        link_optional "$IRIG_IN" "$DSP_IN_R_OLD" || true
      elif port_exists "$DSP_IN_MONO_OLD"; then
        link_optional "$IRIG_IN" "$DSP_IN_MONO_OLD" || true
      elif port_exists "$DSP_IN_0_OLD"; then
        link_optional "$IRIG_IN" "$DSP_IN_0_OLD" || true
      fi
    else
      echo "diag: PW_WIRE_OUTPUT_ONLY=1 (skipping legacy capture -> dsp links)" >&2
      snapshot_note "diag: output-only wiring (skipping legacy capture -> dsp links)"
    fi
  fi
fi

read -r IRIG_OUT_L IRIG_OUT_R < <(pick_irig_out || true)
if [ -z "${IRIG_OUT_L:-}" ] || [ -z "${IRIG_OUT_R:-}" ]; then
  echo "skip: no iRig playback ports found" >&2
  snapshot_note "ERR no iRig playback ports found"
  exit 1
fi
snapshot_kv "IRIG_OUT_L" "$IRIG_OUT_L"
snapshot_kv "IRIG_OUT_R" "$IRIG_OUT_R"
snapshot_kv "IRIG_OUT_L_visible" "$(port_exists "$IRIG_OUT_L" && echo yes || echo no)"
snapshot_kv "IRIG_OUT_R_visible" "$(port_exists "$IRIG_OUT_R" && echo yes || echo no)"

# dsp outputs -> playback (prefer output_* if present, else monitor_*)
# First, remove any stale links into the playback sinks.
PORTS="$(pw-link -io 2>/dev/null || true)"
disconnect_sink "$IRIG_OUT_L" || true
disconnect_sink "$IRIG_OUT_R" || true

if [ -n "${DSP_OUT_L:-}" ] && [ -n "${DSP_OUT_R:-}" ] && port_exists "$DSP_OUT_L" && port_exists "$DSP_OUT_R"; then
  snapshot_kv "DSP_OUT_L" "$DSP_OUT_L"
  snapshot_kv "DSP_OUT_R" "$DSP_OUT_R"
  snapshot_kv "DSP_OUT_L_visible" "$(port_exists "$DSP_OUT_L" && echo yes || echo no)"
  snapshot_kv "DSP_OUT_R_visible" "$(port_exists "$DSP_OUT_R" && echo yes || echo no)"
  link_required "$DSP_OUT_L" "$IRIG_OUT_L" || rc=1
  link_required "$DSP_OUT_R" "$IRIG_OUT_R" || rc=1
elif port_exists "$DSP_OUT_L_OLD" && port_exists "$DSP_OUT_R_OLD"; then
  link_required "$DSP_OUT_L_OLD" "$IRIG_OUT_L" || rc=1
  link_required "$DSP_OUT_R_OLD" "$IRIG_OUT_R" || rc=1
elif port_exists "$DSP_OUT_L_OLD_FALLBACK" && port_exists "$DSP_OUT_R_OLD_FALLBACK"; then
  link_required "$DSP_OUT_L_OLD_FALLBACK" "$IRIG_OUT_L" || rc=1
  link_required "$DSP_OUT_R_OLD_FALLBACK" "$IRIG_OUT_R" || rc=1
elif port_exists "$DSP_OUT_L_OLD_MON" && port_exists "$DSP_OUT_R_OLD_MON"; then
  link_required "$DSP_OUT_L_OLD_MON" "$IRIG_OUT_L" || rc=1
  link_required "$DSP_OUT_R_OLD_MON" "$IRIG_OUT_R" || rc=1
elif port_exists "$DSP_MON_L_OLD" && port_exists "$DSP_MON_R_OLD"; then
  link_required "$DSP_MON_L_OLD" "$IRIG_OUT_L" || rc=1
  link_required "$DSP_MON_R_OLD" "$IRIG_OUT_R" || rc=1
else
  echo "skip: no dsp outputs found (expected duplex outputs or legacy playback/monitor ports)" >&2
  snapshot_note "ERR no dsp outputs found"
  rc=1
fi

snapshot_kv "rc" "$rc"

exit $rc
