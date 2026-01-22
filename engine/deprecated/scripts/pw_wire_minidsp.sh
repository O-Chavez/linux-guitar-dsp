#!/usr/bin/env bash
set -euo pipefail

log() { echo "[pw_wire_minidsp] $*" >&2; }

die() { log "ERROR: $*"; exit 1; }

need() { command -v "$1" >/dev/null 2>&1 || die "missing dependency: $1"; }

need pw-link
need pw-dump
need jq

MIN_NODE_NAME=${MIN_NODE_NAME:-dsp_engine_min.stream}
SINK_NODE_NAME=${SINK_NODE_NAME:-}

# iRig output heuristics (same style as pw_wire_irig.sh but minimal).
IRIG_SINK_GREP=${IRIG_SINK_GREP:-alsa_output.*iRig.*}

min_L="$MIN_NODE_NAME:input_FL"
min_R="$MIN_NODE_NAME:input_FR"

have_port() { pw-link -io 2>/dev/null | grep -Fqx "$1"; }

have_port_pwcli() {
  # Fallback when pw-link doesn't list ports yet.
  pw-cli ls Port 2>/dev/null | awk -v want="$1" 'BEGIN{RS="";FS="\n"} {
    path=""; n=""; p="";
    for (i=1;i<=NF;i++) {
      line=$i
      if (line ~ /object\.path = "/) { sub(/.*object\.path = "/,"",line); sub(/".*/,"",line); path=line }
      if (line ~ /node\.name = "/) { sub(/.*node\.name = "/,"",line); sub(/".*/,"",line); n=line }
      if (line ~ /port\.name = "/) { sub(/.*port\.name = "/,"",line); sub(/".*/,"",line); p=line }
    }
    if (path == want || (n ":" p) == want) { exit 0 }
  } END{ exit 1 }'
}

list_ports_pwcli() {
  # Return endpoints in node.name:port.name form when available.
  pw-cli ls Port 2>/dev/null | awk 'BEGIN{RS="";FS="\n"} /node\.name = "'"$MIN_NODE_NAME"'"/ {
    n=""; p=""; path="";
    for (i=1;i<=NF;i++) {
      line=$i
      if (line ~ /node\.name = "/) { sub(/.*node\.name = "/,"",line); sub(/".*/,"",line); n=line }
      if (line ~ /port\.name = "/) { sub(/.*port\.name = "/,"",line); sub(/".*/,"",line); p=line }
      if (line ~ /object\.path = "/) { sub(/.*object\.path = "/,"",line); sub(/".*/,"",line); path=line }
    }
    if (n != "" && p != "") print n ":" p;
    else if (path != "") print path;
  }' || true
}

pick_min_ports() {
  # Prefer output_* ports (playback stream), then monitor_*, then input_*.
  local outL="$MIN_NODE_NAME:output_FL"
  local outR="$MIN_NODE_NAME:output_FR"
  local out0="$MIN_NODE_NAME:output_0"
  local monL="$MIN_NODE_NAME:monitor_FL"
  local monR="$MIN_NODE_NAME:monitor_FR"
  local mon0="$MIN_NODE_NAME:monitor_0"
  local inL="$MIN_NODE_NAME:input_FL"
  local inR="$MIN_NODE_NAME:input_FR"
  local in0="$MIN_NODE_NAME:input_0"
  if have_port "$outL" && have_port "$outR"; then
    min_L="$outL"
    min_R="$outR"
    return 0
  fi
  if have_port_pwcli "$out0"; then
    min_L="$out0"
    min_R="$out0"
    return 0
  fi
  if have_port "$out0"; then
    min_L="$out0"
    min_R="$out0"
    return 0
  fi
  if have_port "$monL" && have_port "$monR"; then
    min_L="$monL"
    min_R="$monR"
    return 0
  fi
  if have_port_pwcli "$mon0"; then
    min_L="$mon0"
    min_R="$mon0"
    return 0
  fi
  if have_port "$mon0"; then
    min_L="$mon0"
    min_R="$mon0"
    return 0
  fi
  if have_port "$inL" && have_port "$inR"; then
    min_L="$inL"
    min_R="$inR"
    return 0
  fi
  if have_port_pwcli "$in0"; then
    min_L="$in0"
    min_R="$in0"
    return 0
  fi
  if have_port "$in0"; then
    min_L="$in0"
    min_R="$in0"
    return 0
  fi
  # Last-resort: pick the first pw-cli port and mirror it.
  local any
  any="$(list_ports_pwcli | head -n 1 || true)"
  if [[ -n "$any" ]]; then
    min_L="$any"
    min_R="$any"
    return 0
  fi
  min_L="$inL"
  min_R="$inR"
  return 0
}

wait_for_port() {
  local p="$1"
  local tries=40
  while (( tries-- > 0 )); do
    if have_port "$p" || have_port_pwcli "$p"; then
      return 0
    fi
    sleep 0.05
  done
  return 1
}

resolve_sink_from_env_or_irig() {
  if [[ -n "$SINK_NODE_NAME" ]]; then
    echo "$SINK_NODE_NAME"
    return 0
  fi

  # Try to find an iRig output node.name.
  # Prefer a Sink node with playback_FL/FR ports.
  local candidate
  candidate=$(pw-dump | jq -r --arg re "$IRIG_SINK_GREP" '
    [.[]
      | select(.type=="PipeWire:Interface:Node")
      | select((.info.props["media.class"]//"")|test("Audio/Sink"))
      | select((.info.props["node.name"]//"")|test($re))
      | .info.props["node.name"]
    ][0] // empty
  ')

  if [[ -n "$candidate" ]]; then
    echo "$candidate"
    return 0
  fi

  die "could not auto-detect sink. Set SINK_NODE_NAME explicitly or relax IRIG_SINK_GREP."
}

link_once() {
  local src="$1" dst="$2"
  pw-link "$src" "$dst" >/dev/null 2>&1 || true
}

verify_link() {
  local src="$1" dst="$2"
  pw-link -l | grep -Fq "$src |-> $dst"
}

main() {
  # Wait for any minidsp port to appear, then choose monitor vs input.
  log "waiting for any minidsp port..."
  local tries=40
  while (( tries-- > 0 )); do
    if pw-link -io 2>/dev/null | grep -Fq "$MIN_NODE_NAME:"; then
      break
    fi
    if pw-cli ls Port 2>/dev/null | grep -Fq "object.path = \"$MIN_NODE_NAME:"; then
      break
    fi
    sleep 0.05
  done
  pick_min_ports
  log "waiting for minidsp ports: $min_L / $min_R"
  wait_for_port "$min_L" || die "minidsp port not found: $min_L"
  wait_for_port "$min_R" || die "minidsp port not found: $min_R"

  local sink
  sink=$(resolve_sink_from_env_or_irig)
  log "using sink node.name: $sink"

  local sink_L="$sink:playback_FL"
  local sink_R="$sink:playback_FR"

  log "waiting for sink ports: $sink_L / $sink_R"
  wait_for_port "$sink_L" || die "sink port not found: $sink_L"
  wait_for_port "$sink_R" || die "sink port not found: $sink_R"

  log "linking: $min_L -> $sink_L"
  link_once "$min_L" "$sink_L"

  log "linking: $min_R -> $sink_R"
  link_once "$min_R" "$sink_R"

  verify_link "$min_L" "$sink_L" || die "link missing: $min_L -> $sink_L"
  verify_link "$min_R" "$sink_R" || die "link missing: $min_R -> $sink_R"

  log "OK: links verified"
}

main "$@"
