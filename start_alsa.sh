#!/usr/bin/env bash
set -euo pipefail

echo "========================================="
echo "Guitar Pedal DSP Engine Startup (ALSA)"
echo "========================================="
echo ""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

STATE_DIR_DEFAULT="${XDG_STATE_HOME:-$HOME/.local/state}"
STATE_DIR="$STATE_DIR_DEFAULT/dsp-engine-v1"

if ! mkdir -p "$STATE_DIR" 2>/dev/null; then
	STATE_DIR="/tmp/dsp-engine-v1"
	mkdir -p "$STATE_DIR" 2>/dev/null || true
fi

ENGINE_PID_FILE="$STATE_DIR/dsp_engine_alsa.pid"
ENGINE_PGID_FILE="$STATE_DIR/dsp_engine_alsa.pgid"
START_PID_FILE="$STATE_DIR/start_alsa.pid"
ENGINE_LOG_FILE="$STATE_DIR/dsp_engine_alsa.log"

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

cleanup_engine() {
	local pid pgid
	pid="$(read_pidfile "$ENGINE_PID_FILE")"
	pgid="$(read_pidfile "$ENGINE_PGID_FILE")"

	echo ""
	echo "Stopping DSP engine (ALSA)..."

	local target_pid="${pid:-}"
	if [ -n "$target_pid" ] && ! is_running_pid "$target_pid"; then
		target_pid=""
	fi

	if [ -n "$pgid" ]; then
		kill -TERM -"$pgid" 2>/dev/null || true
	else
		[ -n "$target_pid" ] && kill -TERM "$target_pid" 2>/dev/null || true
	fi

	for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
		if [ -n "$target_pid" ] && ! is_running_pid "$target_pid"; then
			break
		fi
		sleep 0.2 || true
	done

	if [ -n "$pgid" ]; then
		kill -KILL -"$pgid" 2>/dev/null || true
	else
		[ -n "$target_pid" ] && kill -KILL "$target_pid" 2>/dev/null || true
	fi

	remove_pidfile "$ENGINE_PID_FILE"
	remove_pidfile "$ENGINE_PGID_FILE"
	remove_pidfile "$START_PID_FILE"
}

tail_engine_log() {
	local n="${1:-80}"
	if [ -f "$ENGINE_LOG_FILE" ]; then
		tail -n "$n" "$ENGINE_LOG_FILE"
		echo "--- end log ---"
	fi
}

cmd="${1:-start}"
case "$cmd" in
	start-safe)
		# SAFE baseline profile (troubleshooting): 48k / 256 frames / 4 periods.
		# Intentionally overrides any caller-provided period settings.
		export ALSA_RATE="${ALSA_RATE:-48000}"
		export ALSA_PERIOD="256"
		export ALSA_PERIODS="4"
		export ALSA_SAFE_DEFAULTS="1"
		export ALSA_DISABLE_LINK="${ALSA_DISABLE_LINK:-1}"
		export ALSA_LOG_STATS="${ALSA_LOG_STATS:-1}"
		export ALSA_LOG_TIMING="${ALSA_LOG_TIMING:-1}"
		export ALSA_BASELINE="${ALSA_BASELINE:-1}"
		export ALSA_BASELINE_CHAIN_US_MAX="${ALSA_BASELINE_CHAIN_US_MAX:-2000}"
		cmd="start"
		;;
	start-lowlat)
		# LOW_LATENCY baseline profile (target): 48k / 128 frames / 3 periods.
		export ALSA_RATE="${ALSA_RATE:-48000}"
		export ALSA_PERIOD="128"
		export ALSA_PERIODS="3"
		export ALSA_SAFE_DEFAULTS="0"
		export ALSA_DISABLE_LINK="${ALSA_DISABLE_LINK:-1}"
		export ALSA_LOG_STATS="${ALSA_LOG_STATS:-1}"
		export ALSA_LOG_TIMING="${ALSA_LOG_TIMING:-1}"
		export ALSA_BASELINE="${ALSA_BASELINE:-1}"
		export ALSA_BASELINE_CHAIN_US_MAX="${ALSA_BASELINE_CHAIN_US_MAX:-2000}"
		cmd="start"
		;;
	stop)
		cleanup_engine
		exit 0
		;;
	restart)
		cleanup_engine
		cmd="start"
		;;
	status)
		pid="$(read_pidfile "$ENGINE_PID_FILE")"
		if is_running_pid "$pid"; then
			echo "DSP engine (ALSA) running (pid=$pid)."
			exit 0
		else
			echo "DSP engine (ALSA) not running."
			exit 1
		fi
		;;
	start)
		;;
	*)
		echo "Usage: $0 {start|start-safe|start-lowlat|stop|restart|status}"
		exit 2
		;;
esac

existing_pid="$(read_pidfile "$ENGINE_PID_FILE")"
if is_running_pid "$existing_pid"; then
	echo "DSP engine (ALSA) already running (pid=$existing_pid)."
	echo "Use: $0 stop"
	exit 0
fi

write_pidfile "$START_PID_FILE" "$$"

echo "Starting DSP engine (ALSA)..."

rm -f "$ENGINE_LOG_FILE" 2>/dev/null || true

# Stable defaults (low latency).
# We respect caller-provided ALSA_* values by default.
# If you want conservative settings for troubleshooting, set ALSA_SAFE_DEFAULTS=1.
if [ "${ALSA_SAFE_DEFAULTS:-0}" = "1" ]; then
	export ALSA_PERIOD="256"
	export ALSA_PERIODS="4"
	export ALSA_VERBOSE_XRUN="${ALSA_VERBOSE_XRUN:-0}"
	export ALSA_LOG_STATS="${ALSA_LOG_STATS:-0}"
else
	export ALSA_PERIOD="${ALSA_PERIOD:-128}"
	export ALSA_PERIODS="${ALSA_PERIODS:-3}"
	export ALSA_VERBOSE_XRUN="${ALSA_VERBOSE_XRUN:-0}"
	export ALSA_LOG_STATS="${ALSA_LOG_STATS:-0}"
fi

{
	echo "===== dsp_engine_alsa start $(date -Is) ====="
	echo "PWD=$(pwd)"
		echo "ALSA_ENFORCE_RELEASE=${ALSA_ENFORCE_RELEASE:-}"
		echo "ALSA_BASELINE=${ALSA_BASELINE:-}"
		echo "ALSA_BASELINE_CHAIN_US_MAX=${ALSA_BASELINE_CHAIN_US_MAX:-}"
		echo "ALSA_CAPTURE_SANITY_SECS=${ALSA_CAPTURE_SANITY_SECS:-}"
		echo "ALSA_CAPTURE_SILENT_PEAK=${ALSA_CAPTURE_SILENT_PEAK:-}"
	echo "ALSA_DEVICE=${ALSA_DEVICE:-}"
	echo "ALSA_CAPTURE_DEVICE=${ALSA_CAPTURE_DEVICE:-}"
	echo "ALSA_PLAYBACK_DEVICE=${ALSA_PLAYBACK_DEVICE:-}"
	echo "ALSA_RATE=${ALSA_RATE:-}"
	echo "ALSA_CHANNELS=${ALSA_CHANNELS:-}"
	echo "ALSA_CAPTURE_CHANNELS=${ALSA_CAPTURE_CHANNELS:-}"
	echo "ALSA_PLAYBACK_CHANNELS=${ALSA_PLAYBACK_CHANNELS:-}"
	echo "ALSA_PERIOD=${ALSA_PERIOD:-}"
	echo "ALSA_PERIODS=${ALSA_PERIODS:-}"
	echo "ALSA_SAFE_DEFAULTS=${ALSA_SAFE_DEFAULTS:-0}"
	echo "ALSA_VERBOSE_XRUN=${ALSA_VERBOSE_XRUN:-}"
	echo "ALSA_LOG_STATS=${ALSA_LOG_STATS:-}"
	echo "ALSA_LOG_TIMING=${ALSA_LOG_TIMING:-}"
	echo "ALSA_BYPASS_NAM=${ALSA_BYPASS_NAM:-}"
	echo "ALSA_BYPASS_IR=${ALSA_BYPASS_IR:-}"
	echo "ALSA_NAM_PRE_GAIN_DB=${ALSA_NAM_PRE_GAIN_DB:-}"
	echo "NAM_PRE_GAIN_DB=${NAM_PRE_GAIN_DB:-}"
	echo "ALSA_NAM_POST_GAIN_DB=${ALSA_NAM_POST_GAIN_DB:-}"
	echo "ALSA_OUTPUT_GAIN_DB=${ALSA_OUTPUT_GAIN_DB:-}"
} >>"$ENGINE_LOG_FILE"

# Default to disabling ALSA link (can be overridden by env).
export ALSA_DISABLE_LINK="${ALSA_DISABLE_LINK:-1}"

if [ -n "${ALSA_CPU_AFFINITY:-}" ]; then
	setsid -f taskset -c "$ALSA_CPU_AFFINITY" "$SCRIPT_DIR/build/dsp_engine_alsa" >>"$ENGINE_LOG_FILE" 2>&1
else
	setsid -f "$SCRIPT_DIR/build/dsp_engine_alsa" >>"$ENGINE_LOG_FILE" 2>&1
fi

DSP_PID="$(pgrep -n -x dsp_engine_alsa 2>/dev/null || true)"
if [ -z "$DSP_PID" ]; then
	echo "Failed to start DSP engine (ALSA)." >&2
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
	if [ "$cleaned_up" -eq 1 ]; then
		return 0
	fi
	cleaned_up=1
	cleanup_engine
}

handle_int() {
	local pgid
	pgid="$(read_pidfile "$ENGINE_PGID_FILE")"
	if [ -n "$pgid" ]; then
		kill -INT -"$pgid" 2>/dev/null || true
	else
		kill -INT "$DSP_PID" 2>/dev/null || true
	fi
	for _ in 1 2 3 4 5 6 7 8; do
		if ! kill -0 "$DSP_PID" 2>/dev/null; then
			return
		fi
		sleep 0.2 || true
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
}

trap handle_int INT
trap handle_term TERM HUP
trap handle_exit EXIT

echo "DSP engine (ALSA) running (PID: $DSP_PID)"
echo "Press Ctrl+C to stop"
echo ""

while kill -0 "$DSP_PID" 2>/dev/null; do
	sleep 0.2
 done

handle_exit

echo ""
echo "DSP engine stopped."
