#!/bin/bash

# Legacy PipeWire routing launcher.
#
# The primary backend is now ALSA-direct (see start_alsa.sh). This script is
# kept for legacy PipeWire setups only.

set -euo pipefail

echo "========================================="
echo "Guitar Pedal DSP Engine Startup (LEGACY PipeWire)"
echo "========================================="
echo ""

# This file lives under deprecated/, but it still needs to reference the repo root
# for build outputs and scripts.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}" )/.." && pwd)"

# Prefer a persistent, per-user runtime/cache directory.
# This survives reboots and is less likely to be wiped than /tmp.
STATE_DIR_DEFAULT="${XDG_STATE_HOME:-$HOME/.local/state}"
STATE_DIR="$STATE_DIR_DEFAULT/dsp-engine-v1"

# Fallback to /tmp if the state dir can't be created (e.g., read-only home).
if ! mkdir -p "$STATE_DIR" 2>/dev/null; then
	STATE_DIR="/tmp/dsp-engine-v1"
	mkdir -p "$STATE_DIR" 2>/dev/null || true
fi

ENGINE_PID_FILE="$STATE_DIR/dsp_engine_pw.pid"
ENGINE_PGID_FILE="$STATE_DIR/dsp_engine_pw.pgid"
START_PID_FILE="$STATE_DIR/start_dsp.pid"
ENGINE_LOG_FILE="$STATE_DIR/dsp_engine_pw.log"

is_running_pid() {
	local pid="$1"
	[ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

read_pidfile() {
	local f="$1"
	[ -f "$f" ] && cat "$f" 2>/dev/null || true
}

write_pidfile() {
	local f="$1"
	local pid="$2"
	echo "$pid" >"$f"
}

remove_pidfile() {
	local f="$1"
	rm -f "$f" 2>/dev/null || true
}

disconnect_irig_playback_feeds() {
	# Ensure nothing is still feeding the iRig outputs.
	# Support both older analog-stereo profile ports and pro-output ports.
	for p in \
		alsa_output.usb-IK_Multimedia_iRig_HD_X_1001073-02.analog-stereo:playback_FL \
		alsa_output.usb-IK_Multimedia_iRig_HD_X_1001073-02.analog-stereo:playback_FR \
		alsa_output.usb-IK_Multimedia_iRig_HD_X_1001073-02.pro-output-0:playback_AUX0 \
		alsa_output.usb-IK_Multimedia_iRig_HD_X_1001073-02.pro-output-0:playback_AUX1
	do
		pw-link -l 2>/dev/null | awk -v sink="$p" '
			$0==sink {ins=1; next}
			ins && $0 ~ /^\s*\|<-/ {gsub(/^\s*\|<-\s*/,"",$0); print $0}
			ins && $0 !~ /^\s*\|<-/ && $0!="" {ins=0}
		' | while read -r src; do
			[ -n "$src" ] && pw-link -d "$src" "$p" >/dev/null 2>&1 || true
		done
	done
}

cleanup_engine() {
	snapshot_pw

	local pid pgid
	pid="$(read_pidfile "$ENGINE_PID_FILE")"
	pgid="$(read_pidfile "$ENGINE_PGID_FILE")"

	echo ""
	echo "Stopping DSP engine and disconnecting audio..."

	# Prefer killing the engine process group, if known.
	# For dump captures, we want to give the engine time to shut down cleanly and flush files.
	local target_pid="${pid:-}"
	if [ -n "$target_pid" ] && ! is_running_pid "$target_pid"; then
		target_pid=""
	fi

	if [ -n "$pgid" ]; then
		# Negative PID => process group.
		kill -TERM -"$pgid" 2>/dev/null || true
	else
		[ -n "$target_pid" ] && kill -TERM "$target_pid" 2>/dev/null || true
	fi

	# Wait up to ~5s for clean exit.
	for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
		if [ -n "$target_pid" ] && ! kill -0 "$target_pid" 2>/dev/null; then
			break
		fi
		sleep 0.2 || true
	done

	# If still running, force kill.
	if [ -n "$pgid" ]; then
		kill -KILL -"$pgid" 2>/dev/null || true
	else
		[ -n "$target_pid" ] && kill -KILL "$target_pid" 2>/dev/null || true
	fi

	# Fallback hard stop (should rarely be needed).
	pkill -x dsp_engine_pw 2>/dev/null || true
	pkill -x dsp_engine_v1 2>/dev/null || true

	disconnect_irig_playback_feeds

	remove_pidfile "$ENGINE_PID_FILE"
	remove_pidfile "$ENGINE_PGID_FILE"
	remove_pidfile "$START_PID_FILE"
}

snapshot_pw() {
	SNAPSHOT_ONLY=1 "$SCRIPT_DIR/deprecated/scripts/pw_wire_irig.sh" || true
	local SNAPSHOT_FILE="$STATE_DIR/pw_wire_snapshot.txt"
	if [ -f "$SNAPSHOT_FILE" ]; then
		echo "" >>"$ENGINE_LOG_FILE"
		echo "--- pw_wire_snapshot: $SNAPSHOT_FILE ---" >>"$ENGINE_LOG_FILE"
		cat "$SNAPSHOT_FILE" >>"$ENGINE_LOG_FILE" || true
		echo "--- end pw_wire_snapshot ---" >>"$ENGINE_LOG_FILE"
	fi
}

resolve_dsp_node_id() {
	local id
	if command -v jq >/dev/null 2>&1; then
		id="$(pw-dump 2>/dev/null | jq -r '
			.[] | select(.type=="PipeWire:Interface:Node") |
			select(.info.props["node.name"]=="dsp_engine_v1.duplex") |
			.id' | head -n 1)"
	else
		id="$(pw-cli ls Node 2>/dev/null | awk 'BEGIN{RS=""; FS="\n"} /node.name = "dsp_engine_v1\.duplex"/ {for(i=1;i<=NF;i++) if($i ~ /^id /){split($i,a," "); gsub(/,/,"",a[2]); print a[2]; exit}}')"
	fi
	echo "$id"
}

tail_engine_log() {
	local n="${1:-80}"
	if [ -f "$ENGINE_LOG_FILE" ]; then
		echo ""
		echo "--- dsp_engine_pw log (last $n lines): $ENGINE_LOG_FILE ---"
		tail -n "$n" "$ENGINE_LOG_FILE" || true
		echo "--- end log ---"
	fi
}

cmd="${1:-start}"
case "$cmd" in
	stop)
		cleanup_engine
		tail_engine_log 120
		echo "DSP engine stopped."
		exit 0
		;;
	status)
		pid="$(read_pidfile "$ENGINE_PID_FILE")"
		pgid="$(read_pidfile "$ENGINE_PGID_FILE")"
		if is_running_pid "$pid"; then
			echo "RUNNING pid=$pid pgid=${pgid:-unknown}"
			echo "log=$ENGINE_LOG_FILE"
			echo "state_dir=$STATE_DIR"
			exit 0
		fi
		echo "STOPPED"
		tail_engine_log 80
		echo "state_dir=$STATE_DIR"
		exit 1
		;;
	start)
		;;
	*)
		echo "Usage: $0 [start|stop|status]" >&2
		exit 2
		;;
esac

# If something is already running, don't start a second copy.
existing_pid="$(read_pidfile "$ENGINE_PID_FILE")"
if is_running_pid "$existing_pid"; then
	echo "DSP engine already running (pid=$existing_pid)."
	echo "Use: $0 stop"
	exit 0
fi

# Record that this script instance is the owner.
write_pidfile "$START_PID_FILE" "$$"

echo "Starting DSP engine (PipeWire native)..."

# Reset the log for this run.
rm -f "$ENGINE_LOG_FILE" 2>/dev/null || true

# Write a small header so we can correlate runs.
{
	echo "===== dsp_engine_pw start $(date -Is) ====="
	echo "PWD=$(pwd)"
	echo "DUMP_NAM_IN_WAV=${DUMP_NAM_IN_WAV:-}"
	echo "DUMP_NAM_OUT_WAV=${DUMP_NAM_OUT_WAV:-}"
	echo "DUMP_NAM_SECONDS=${DUMP_NAM_SECONDS:-}"
	echo "NAM_REFERENCE_MODE=${NAM_REFERENCE_MODE:-}"
	echo "PW_USE_LEGACY_STREAMS=${PW_USE_LEGACY_STREAMS:-}"
	echo "PW_CAPTURE_CHANNELS=${PW_CAPTURE_CHANNELS:-}"
	echo "PW_WANT_DRIVER=${PW_WANT_DRIVER:-}"
	echo "PW_CAPTURE_PASSIVE=${PW_CAPTURE_PASSIVE:-}"
	echo "PW_CAPTURE_NO_FORCE=${PW_CAPTURE_NO_FORCE:-}"
} >>"$ENGINE_LOG_FILE"

# Preserve any debug env vars passed to this script when launching the engine.
# (e.g. DUMP_NAM_IN_WAV / DUMP_NAM_OUT_WAV / DUMP_NAM_SECONDS)
export DUMP_NAM_IN_WAV="${DUMP_NAM_IN_WAV:-}"
export DUMP_NAM_OUT_WAV="${DUMP_NAM_OUT_WAV:-}"
export DUMP_NAM_SECONDS="${DUMP_NAM_SECONDS:-}"
export PW_USE_LEGACY_STREAMS="${PW_USE_LEGACY_STREAMS:-}"
export PW_CAPTURE_CHANNELS="${PW_CAPTURE_CHANNELS:-}"
export PW_WANT_DRIVER="${PW_WANT_DRIVER:-}"
export PW_CAPTURE_PASSIVE="${PW_CAPTURE_PASSIVE:-}"
export PW_CAPTURE_NO_FORCE="${PW_CAPTURE_NO_FORCE:-}"

# Pin the engine streams to the iRig's nodes so PipeWire schedules us in the
# hardware clock domain (and we don't end up timer/freewheel ~60Hz).
# Allow manual override by exporting PW_TARGET_CAPTURE/PW_TARGET_PLAYBACK.
if [ -z "${PW_TARGET_CAPTURE:-}" ]; then
	PW_TARGET_CAPTURE="$(pw-cli ls Node 2>/dev/null | awk -F '"' '/node.name = "alsa_input\.usb-IK_Multimedia_iRig_HD_X_/ {print $2; exit}' || true)"
	export PW_TARGET_CAPTURE
fi
if [ -z "${PW_TARGET_PLAYBACK:-}" ]; then
	PW_TARGET_PLAYBACK="$(pw-cli ls Node 2>/dev/null | awk -F '"' '/node.name = "alsa_output\.usb-IK_Multimedia_iRig_HD_X_/ {print $2; exit}' || true)"
	export PW_TARGET_PLAYBACK
fi

# Optional hints (can help keep streams grouped with the ALSA graph). Keep them
# overridable by the environment.
export PW_CLOCK_NAME="${PW_CLOCK_NAME:-}"
export PW_NODE_GROUP="${PW_NODE_GROUP:-}"

{
	echo "PW_TARGET_CAPTURE=${PW_TARGET_CAPTURE:-}"
	echo "PW_TARGET_PLAYBACK=${PW_TARGET_PLAYBACK:-}"
	echo "PW_CLOCK_NAME=${PW_CLOCK_NAME:-}"
	echo "PW_NODE_GROUP=${PW_NODE_GROUP:-}"
} >>"$ENGINE_LOG_FILE"

# Start the engine in a new session so it gets its own process group.
# This lets us reliably forward Ctrl+C/TERM to the whole group (engine + any children)
# and avoids orphaned dsp_engine_pw processes.
set +m 2>/dev/null || true
setsid -f "$SCRIPT_DIR/build/dsp_engine_pw" >>"$ENGINE_LOG_FILE" 2>&1

ENGINE_PID_FILE="$STATE_DIR/dsp_engine_pw.pid"
ENGINE_PGID_FILE="$STATE_DIR/dsp_engine_pw.pgid"
START_PID_FILE="$STATE_DIR/start_dsp.pid"
ENGINE_LOG_FILE="$STATE_DIR/dsp_engine_pw.log"

is_running_pid() {
	local pid="$1"
	[ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

read_pidfile() {
	local f="$1"
	[ -f "$f" ] && cat "$f" 2>/dev/null || true
}

write_pidfile() {
	local f="$1"
	local pid="$2"
	echo "$pid" >"$f"
}

remove_pidfile() {
	local f="$1"
	rm -f "$f" 2>/dev/null || true
}

disconnect_irig_playback_feeds() {
	# Ensure nothing is still feeding the iRig outputs.
	# Support both older analog-stereo profile ports and pro-output ports.
	for p in \
		alsa_output.usb-IK_Multimedia_iRig_HD_X_1001073-02.analog-stereo:playback_FL \
		alsa_output.usb-IK_Multimedia_iRig_HD_X_1001073-02.analog-stereo:playback_FR \
		alsa_output.usb-IK_Multimedia_iRig_HD_X_1001073-02.pro-output-0:playback_AUX0 \
		alsa_output.usb-IK_Multimedia_iRig_HD_X_1001073-02.pro-output-0:playback_AUX1
	do
		pw-link -l 2>/dev/null | awk -v sink="$p" '
			$0==sink {ins=1; next}
			ins && $0 ~ /^\s*\|<-/ {gsub(/^\s*\|<-\s*/,"",$0); print $0}
			ins && $0 !~ /^\s*\|<-/ && $0!="" {ins=0}
		' | while read -r src; do
			[ -n "$src" ] && pw-link -d "$src" "$p" >/dev/null 2>&1 || true
		done
	done
}

cleanup_engine() {
			# Append wiring snapshot (authoritative port IDs + link verification results).
			# This makes debugging possible even when stdout/stderr is truncated.
			SNAPSHOT_FILE="$STATE_DIR/pw_wire_snapshot.txt"
			if [ -f "$SNAPSHOT_FILE" ]; then
				echo "" >>"$ENGINE_LOG_FILE"
				echo "--- pw_wire_snapshot: $SNAPSHOT_FILE ---" >>"$ENGINE_LOG_FILE"
				cat "$SNAPSHOT_FILE" >>"$ENGINE_LOG_FILE" || true
				echo "--- end pw_wire_snapshot ---" >>"$ENGINE_LOG_FILE"
			fi

	local pid pgid
	pid="$(read_pidfile "$ENGINE_PID_FILE")"
	pgid="$(read_pidfile "$ENGINE_PGID_FILE")"

	echo ""
	echo "Stopping DSP engine and disconnecting audio..."

	# Prefer killing the engine process group, if known.
	# For dump captures, we want to give the engine time to shut down cleanly and flush files.
	local target_pid="${pid:-}"
	if [ -n "$target_pid" ] && ! is_running_pid "$target_pid"; then
		target_pid=""
	fi

	if [ -n "$pgid" ]; then
		# Negative PID => process group.
		kill -TERM -"$pgid" 2>/dev/null || true
	else
		[ -n "$target_pid" ] && kill -TERM "$target_pid" 2>/dev/null || true
	fi

	# Wait up to ~5s for clean exit.
	for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
		if [ -n "$target_pid" ] && ! kill -0 "$target_pid" 2>/dev/null; then
			break
		fi
		sleep 0.2 || true
	done

	# If still running, force kill.
	if [ -n "$pgid" ]; then
		kill -KILL -"$pgid" 2>/dev/null || true
	else
		[ -n "$target_pid" ] && kill -KILL "$target_pid" 2>/dev/null || true
	fi

	# Fallback hard stop (should rarely be needed).
	pkill -x dsp_engine_pw 2>/dev/null || true
	pkill -x dsp_engine_v1 2>/dev/null || true

	disconnect_irig_playback_feeds

	remove_pidfile "$ENGINE_PID_FILE"
	remove_pidfile "$ENGINE_PGID_FILE"
	remove_pidfile "$START_PID_FILE"
}

snapshot_pw() {
	SNAPSHOT_ONLY=1 "$SCRIPT_DIR/scripts/pw_wire_irig.sh" || true
	local SNAPSHOT_FILE="$STATE_DIR/pw_wire_snapshot.txt"
	if [ -f "$SNAPSHOT_FILE" ]; then
		echo "" >>"$ENGINE_LOG_FILE"
		echo "--- pw_wire_snapshot: $SNAPSHOT_FILE ---" >>"$ENGINE_LOG_FILE"
		cat "$SNAPSHOT_FILE" >>"$ENGINE_LOG_FILE" || true
		echo "--- end pw_wire_snapshot ---" >>"$ENGINE_LOG_FILE"
	fi
}

resolve_dsp_node_id() {
	local id
	if command -v jq >/dev/null 2>&1; then
		id="$(pw-dump 2>/dev/null | jq -r '
			.[] | select(.type=="PipeWire:Interface:Node") |
			select(.info.props["node.name"]=="dsp_engine_v1.duplex") |
			.id' | head -n 1)"
	else
		id="$(pw-cli ls Node 2>/dev/null | awk 'BEGIN{RS=""; FS="\n"} /node.name = "dsp_engine_v1\.duplex"/ {for(i=1;i<=NF;i++) if($i ~ /^id /){split($i,a," "); gsub(/,/,"",a[2]); print a[2]; exit}}')"
	fi
	echo "$id"
}

tail_engine_log() {
	local n="${1:-80}"
	if [ -f "$ENGINE_LOG_FILE" ]; then
		echo ""
		echo "--- dsp_engine_pw log (last $n lines): $ENGINE_LOG_FILE ---"
		tail -n "$n" "$ENGINE_LOG_FILE" || true
		echo "--- end log ---"
	fi
}

cmd="${1:-start}"
case "$cmd" in
	stop)
		cleanup_engine
		tail_engine_log 120
		echo "DSP engine stopped."
		exit 0
		;;
	status)
		pid="$(read_pidfile "$ENGINE_PID_FILE")"
		pgid="$(read_pidfile "$ENGINE_PGID_FILE")"
		if is_running_pid "$pid"; then
			echo "RUNNING pid=$pid pgid=${pgid:-unknown}"
			echo "log=$ENGINE_LOG_FILE"
			echo "state_dir=$STATE_DIR"
			exit 0
		fi
		echo "STOPPED"
		tail_engine_log 80
		echo "state_dir=$STATE_DIR"
		exit 1
		;;
	start)
		;;
	*)
		echo "Usage: $0 [start|stop|status]" >&2
		exit 2
		;;
esac

# If something is already running, don't start a second copy.
existing_pid="$(read_pidfile "$ENGINE_PID_FILE")"
if is_running_pid "$existing_pid"; then
	echo "DSP engine already running (pid=$existing_pid)."
	echo "Use: $0 stop"
	exit 0
fi

# Record that this script instance is the owner.
write_pidfile "$START_PID_FILE" "$$"

echo "Starting DSP engine (PipeWire native)..."

# Reset the log for this run.
rm -f "$ENGINE_LOG_FILE" 2>/dev/null || true

# Write a small header so we can correlate runs.
{
	echo "===== dsp_engine_pw start $(date -Is) ====="
	echo "PWD=$(pwd)"
	echo "DUMP_NAM_IN_WAV=${DUMP_NAM_IN_WAV:-}"
	echo "DUMP_NAM_OUT_WAV=${DUMP_NAM_OUT_WAV:-}"
	echo "DUMP_NAM_SECONDS=${DUMP_NAM_SECONDS:-}"
	echo "NAM_REFERENCE_MODE=${NAM_REFERENCE_MODE:-}"
	echo "PW_USE_LEGACY_STREAMS=${PW_USE_LEGACY_STREAMS:-}"
	echo "PW_CAPTURE_CHANNELS=${PW_CAPTURE_CHANNELS:-}"
	echo "PW_WANT_DRIVER=${PW_WANT_DRIVER:-}"
	echo "PW_CAPTURE_PASSIVE=${PW_CAPTURE_PASSIVE:-}"
	echo "PW_CAPTURE_NO_FORCE=${PW_CAPTURE_NO_FORCE:-}"
} >>"$ENGINE_LOG_FILE"

# Preserve any debug env vars passed to this script when launching the engine.
# (e.g. DUMP_NAM_IN_WAV / DUMP_NAM_OUT_WAV / DUMP_NAM_SECONDS)
export DUMP_NAM_IN_WAV="${DUMP_NAM_IN_WAV:-}"
export DUMP_NAM_OUT_WAV="${DUMP_NAM_OUT_WAV:-}"
export DUMP_NAM_SECONDS="${DUMP_NAM_SECONDS:-}"
export PW_USE_LEGACY_STREAMS="${PW_USE_LEGACY_STREAMS:-}"
export PW_CAPTURE_CHANNELS="${PW_CAPTURE_CHANNELS:-}"
export PW_WANT_DRIVER="${PW_WANT_DRIVER:-}"
export PW_CAPTURE_PASSIVE="${PW_CAPTURE_PASSIVE:-}"
export PW_CAPTURE_NO_FORCE="${PW_CAPTURE_NO_FORCE:-}"

# Pin the engine streams to the iRig's nodes so PipeWire schedules us in the
# hardware clock domain (and we don't end up timer/freewheel ~60Hz).
# Allow manual override by exporting PW_TARGET_CAPTURE/PW_TARGET_PLAYBACK.
if [ -z "${PW_TARGET_CAPTURE:-}" ]; then
	PW_TARGET_CAPTURE="$(pw-cli ls Node 2>/dev/null | awk -F '"' '/node.name = "alsa_input\.usb-IK_Multimedia_iRig_HD_X_/ {print $2; exit}' || true)"
	export PW_TARGET_CAPTURE
fi
if [ -z "${PW_TARGET_PLAYBACK:-}" ]; then
	PW_TARGET_PLAYBACK="$(pw-cli ls Node 2>/dev/null | awk -F '"' '/node.name = "alsa_output\.usb-IK_Multimedia_iRig_HD_X_/ {print $2; exit}' || true)"
	export PW_TARGET_PLAYBACK
fi

# Optional hints (can help keep streams grouped with the ALSA graph). Keep them
# overridable by the environment.
export PW_CLOCK_NAME="${PW_CLOCK_NAME:-}"
export PW_NODE_GROUP="${PW_NODE_GROUP:-}"

{
	echo "PW_TARGET_CAPTURE=${PW_TARGET_CAPTURE:-}"
	echo "PW_TARGET_PLAYBACK=${PW_TARGET_PLAYBACK:-}"
	echo "PW_CLOCK_NAME=${PW_CLOCK_NAME:-}"
	echo "PW_NODE_GROUP=${PW_NODE_GROUP:-}"
} >>"$ENGINE_LOG_FILE"

# Start the engine in a new session so it gets its own process group.
# This lets us reliably forward Ctrl+C/TERM to the whole group (engine + any children)
# and avoids orphaned dsp_engine_pw processes.
set +m 2>/dev/null || true
setsid -f "$SCRIPT_DIR/build/dsp_engine_pw" >>"$ENGINE_LOG_FILE" 2>&1

# Find the newest dsp_engine_pw and treat it as "our" child.
# (Assumption: only one instance is started by this script; we still refuse to start when pidfile says running.)
DSP_PID="$(pgrep -n -x dsp_engine_pw 2>/dev/null || true)"
if [ -z "$DSP_PID" ]; then
	echo "Failed to start DSP engine." >&2
	tail_engine_log 200
	exit 1
fi

DSP_PGID="$(ps -o pgid= -p "$DSP_PID" 2>/dev/null | tr -d ' ' || true)"
write_pidfile "$ENGINE_PID_FILE" "$DSP_PID"
if [ -n "$DSP_PGID" ]; then
	write_pidfile "$ENGINE_PGID_FILE" "$DSP_PGID"
fi

cleaned_up=0
handle_exit() {
	# Avoid double-run (signal trap + EXIT).
	if [ "$cleaned_up" -eq 1 ]; then
		return 0
	fi
	cleaned_up=1
	cleanup_engine
}

handle_int() {
	# On Ctrl+C, forward SIGINT to the engine process group but don't immediately
	# kill the group or disconnect links. Let the engine shut down cleanly so
	# it can flush dump WAVs.
	local pgid
	pgid="$(read_pidfile "$ENGINE_PGID_FILE")"
	if [ -n "$pgid" ]; then
		kill -INT -"$pgid" 2>/dev/null || true
	else
		kill -INT "$DSP_PID" 2>/dev/null || true
	fi
	# Wait briefly for clean exit; if it doesn't exit, cleanup will happen on EXIT.
	for _ in 1 2 3 4 5 6 7 8 9 10; do
		if ! kill -0 "$DSP_PID" 2>/dev/null; then
			break
		fi
		sleep 0.2
	done
}

handle_term() {
	local pgid
	pgid="$(read_pidfile "$ENGINE_PGID_FILE")"
	if [ -n "$pgid" ]; then
		kill -TERM -"$pgid" 2>/dev/null || true
	else
		kill -TERM "$DSP_PID" 2>/dev/null || true
	fi
	handle_exit
	exit 0
}

trap handle_int INT
trap handle_term TERM HUP
trap handle_exit EXIT

# Wait for it to initialize and create PipeWire ports.
# A fixed sleep is racey: sometimes WirePlumber creates the nodes and ports later.
echo "Waiting for DSP to create PipeWire ports..."
ports_ready=0
for _ in $(seq 1 60); do
	# If the engine already died, bail out.
	if ! kill -0 "$DSP_PID" 2>/dev/null; then
		echo "DSP engine exited during startup."
		tail_engine_log 200
		handle_exit
		exit 1
	fi

	# The ports we expect the wiring script to connect.
	if pw-link -io 2>/dev/null | grep -qE '^dsp_engine_v1\.duplex:playback_(FL|FR)$' \
		&& pw-link -io 2>/dev/null | grep -qE '^dsp_engine_v1\.duplex:capture_(FL|FR)$'; then
		ports_ready=1
		break
	fi

	# Legacy 2-stream fallback.
	if pw-link -io 2>/dev/null | grep -qE '^dsp_engine_v1\.capture:input_(FL|FR|MONO|0)$' \
		&& pw-link -io 2>/dev/null | grep -qE '^dsp_engine_v1\.playback:(output_(0|1|FL|FR)|monitor_(FL|FR))$'; then
		ports_ready=1
		break
	fi
	sleep 0.1
done

if [ "$ports_ready" -ne 1 ]; then
	echo "Timed out waiting for DSP ports to appear. Continuing anyway..." >&2
fi

# Resolve the current dsp_engine_v1.duplex node id for accurate diagnostics.
DSP_NODE_ID="$(resolve_dsp_node_id)"
echo "DSP_NODE_ID=${DSP_NODE_ID:-}" >>"$ENGINE_LOG_FILE"

echo "Connecting audio using PipeWire (pw-link)..."

# Debug: capture what the service environment can actually see.
# This is critical because some tools (notably `pw-link -io`) may omit ports
# depending on session/policy, while `pw-cli` is authoritative.
{
	echo ""
	echo "===== pw debug $(date -Is) ====="
	echo "which pw-cli: $(command -v pw-cli || echo 'MISSING')"
	echo "which pw-link: $(command -v pw-link || echo 'MISSING')"
	echo "XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-}"
	echo "PIPEWIRE_REMOTE=${PIPEWIRE_REMOTE:-}"
	echo "-- pw-cli ls Node (dsp_engine_v1.duplex + iRig) --"
	pw-cli ls Node 2>/dev/null | grep -E '^(\s*id\s+[0-9]+,)|\s*node\.name = "(dsp_engine_v1\.duplex|alsa_(input|output)\.usb-IK_Multimedia_iRig_HD_X_)' || true
	echo "-- pw-cli ls Port (dsp_engine_v1.duplex playback ports) --"
	pw-cli ls Port 2>/dev/null | awk 'BEGIN{RS=""} /object\.path = "dsp_engine_v1\.duplex:playback_/ {print $0 "\n---"}' | head -n 200 || true
	echo "-- pw-link -io (snapshot) --"
	pw-link -io 2>/dev/null | head -n 200 || true
	echo "===== end pw debug ====="
} >>"$ENGINE_LOG_FILE" 2>&1

echo "Selected iRig capture port (from pw-link -io):"
pw-link -io 2>/dev/null | grep -E "alsa_input\.usb-IK_Multimedia_iRig_HD_X_[^:]+:capture_(AUX[0-9]+|MONO)" || true

wire_ok=0
for _ in 1 2 3 4 5 6 7 8 9 10; do
	if "$SCRIPT_DIR/scripts/pw_wire_irig.sh"; then
		wire_ok=1
		break
	fi
	sleep 0.2
done

if [ "$wire_ok" -ne 1 ]; then
	echo "Warning: wiring did not fully succeed (ports may be missing)." >&2
fi

if [ "$wire_ok" -eq 1 ]; then
	snapshot_pw
	sleep 2
	snapshot_pw
fi

echo ""
echo "Active links (engine + iRig):"
pw-link -l 2>/dev/null | grep -E "dsp_engine_v1\.(capture|playback)|alsa_(input|output)\.usb-IK_Multimedia_iRig_HD_X_" || true

# Authoritative: show actual PipeWire Link objects for the duplex node
# to diagnose cases where script output says "ok" but links don't persist.
if [ -z "${DSP_NODE_ID:-}" ]; then
	DSP_NODE_ID="$(resolve_dsp_node_id)"
fi
echo "DSP_NODE_ID(after)=${DSP_NODE_ID:-}" >>"$ENGINE_LOG_FILE"
{
	echo ""
	if [ -n "${DSP_NODE_ID:-}" ]; then
		echo "-- pw-cli ls Link (node ${DSP_NODE_ID}) --"
		pw-cli ls Link 2>/dev/null | awk -v nid="$DSP_NODE_ID" 'BEGIN{RS=""} $0 ~ "link\\.output\\.node = \"" nid "\"" || $0 ~ "link\\.input\\.node = \"" nid "\"" {print; print "---"}' | head -n 200 || true
		echo "-- pw-cli info ${DSP_NODE_ID} --"
		pw-cli info "$DSP_NODE_ID" 2>/dev/null | sed -n '1,120p' || true
	else
		echo "-- pw-cli ls Link (node ?) --"
		echo "DSP_NODE_ID not resolved; skipping link/node info."
	fi
} >>"$ENGINE_LOG_FILE" 2>&1

echo ""
echo "DSP engine running (PID: $DSP_PID)"
echo "Press Ctrl+C to stop"
echo ""

# Wait for the DSP process
while kill -0 "$DSP_PID" 2>/dev/null; do
	sleep 0.2
done

# If we get here, the process exited normally.
handle_exit

# If it exits, show why
echo ""
echo "DSP engine stopped."
