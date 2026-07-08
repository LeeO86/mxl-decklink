#!/usr/bin/env bash
# Integration smoke test: runs the full binary with the mock DeckLink backend
# against a real MXL domain in tmpfs (IMPLEMENTATION_PLAN.md §6.2).
#
# Usage: tests/integration/smoke.sh [path-to-mxl-decklink] 
set -u -o pipefail

BIN="${1:-build/mxl-decklink}"
HEALTH_PORT="${HEALTH_PORT_OVERRIDE:-19080}"
METRICS_PORT="${METRICS_PORT_OVERRIDE:-19090}"

FAILURES=0
PID=""
DOMAIN=""

say() { echo "[smoke] $*"; }

fail() {
    echo "[smoke] FAIL: $*" >&2
    FAILURES=$((FAILURES + 1))
}

cleanup() {
    if [[ -n "$PID" ]] && kill -0 "$PID" 2>/dev/null; then
        kill -9 "$PID" 2>/dev/null || true
    fi
    [[ -n "$DOMAIN" ]] && rm -rf "$DOMAIN"
}
trap cleanup EXIT

require_cmd() {
    command -v "$1" >/dev/null || { echo "[smoke] missing tool: $1" >&2; exit 2; }
}
require_cmd curl
require_cmd python3

[[ -x "$BIN" ]] || { echo "[smoke] binary not found: $BIN" >&2; exit 2; }

common_env() {
    # HD720p50 keeps the ring footprint small enough for a 64 MiB /dev/shm.
    # exec so that a backgrounded `common_env … &` yields the binary's PID in
    # $! (kill/wait must target the binary, not a wrapper subshell).
    exec env -i \
        PATH="$PATH" \
        LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" \
        MXL_DECKLINK_BACKEND=mock \
        MXL_DECKLINK_CARD_ID=0xa1b2c3d4 \
        MXL_DOMAIN_PATH="$DOMAIN" \
        HEALTH_PORT="$HEALTH_PORT" \
        METRICS_PORT="$METRICS_PORT" \
        LOG_LEVEL=info \
        CH0_DIRECTION=input \
        CH0_SUBDEVICE_INDEX=0 \
        CH0_VIDEO_MODE=HD720p50 \
        CH0_AUDIO_CHANNEL_COUNT=2 \
        CH0_VIDEO_ANC_ENABLE=true \
        CH0_MXL_VIDEO_FLOW_ID=5fbec3b1-1b0f-417d-9059-8b94a47197ed \
        CH0_MXL_AUDIO_FLOW_ID=b3bb5be7-9fe9-4324-a5bb-4c70e1084449 \
        CH0_MXL_ANC_FLOW_ID=db3bd465-2772-484f-8fac-830b0471258b \
        CH0_LABEL=smoke-in \
        CH1_DIRECTION=output \
        CH1_SUBDEVICE_INDEX=0 \
        CH1_VIDEO_MODE=HD720p50 \
        CH1_AUDIO_CHANNEL_COUNT=2 \
        CH1_MXL_VIDEO_FLOW_ID=5fbec3b1-1b0f-417d-9059-8b94a47197ed \
        CH1_MXL_AUDIO_FLOW_ID=b3bb5be7-9fe9-4324-a5bb-4c70e1084449 \
        CH1_LABEL=smoke-out \
        "$@"
}

http_code() {
    curl -s -o /dev/null -w '%{http_code}' --max-time 3 "http://127.0.0.1:$HEALTH_PORT$1" 2>/dev/null
}

wait_for_readyz() {
    local deadline=$(( $(date +%s) + $1 ))
    while (( $(date +%s) < deadline )); do
        [[ "$(http_code /readyz)" == "200" ]] && return 0
        sleep 0.5
    done
    return 1
}

statusz_field() {
    curl -s --max-time 3 "http://127.0.0.1:$HEALTH_PORT/statusz" |
        python3 -c "
import json, sys
data = json.load(sys.stdin)
ch = [c for c in data['channels'] if c['label'] == '$1'][0]
print(ch['$2'])
"
}

# ---------------------------------------------------------------------------
say "test 1: invalid configuration exits 78 (EX_CONFIG)"
env -i PATH="$PATH" LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" "$BIN" >/dev/null 2>&1
rc=$?
[[ $rc == 78 ]] || fail "expected exit 78 for empty env, got $rc"

DOMAIN=$(mktemp -d /dev/shm/mxl-smoke.XXXXXX)
env -i PATH="$PATH" LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" \
    MXL_DECKLINK_CARD_ID=0xa1b2c3d4 MXL_DOMAIN_PATH="$DOMAIN" \
    DIRECTION=input MXL_VIDEO_FLOW_ID=5fbec3b1-1b0f-417d-9059-8b94a47197ed \
    CH0_DIRECTION=input \
    "$BIN" >/dev/null 2>&1
rc=$?
[[ $rc == 78 ]] || fail "expected exit 78 for mixed v1.0/v1.1 env, got $rc"

# ---------------------------------------------------------------------------
say "test 2: end-to-end input+output channels on the mock card"
LOG="/tmp/mxl-smoke-$$.log"
common_env "$BIN" >"$LOG" 2>&1 &
PID=$!

if ! wait_for_readyz 30; then
    fail "/readyz did not reach 200 within 30 s"
    sed -n '1,50p' "$LOG" >&2
else
    say "/readyz is 200"
fi

sleep 3

state_in=$(statusz_field smoke-in state) || state_in=err
state_out=$(statusz_field smoke-out state) || state_out=err
[[ "$state_in" == "healthy" ]] || fail "input channel state: $state_in"
[[ "$state_out" == "healthy" ]] || fail "output channel state: $state_out"

grains1=$(statusz_field smoke-in grains_committed) || grains1=0
sleep 2
grains2=$(statusz_field smoke-in grains_committed) || grains2=0
if (( grains2 <= grains1 )); then
    fail "input grains not advancing: $grains1 → $grains2"
else
    say "input grains advancing at rate: $grains1 → $grains2"
fi
# ~50 fps: expect roughly 100 grains in 2 s (accept 60–140).
delta=$((grains2 - grains1))
if (( delta < 60 || delta > 140 )); then
    fail "grain rate implausible for 50 fps: $delta grains in 2 s"
fi

out_frames1=$(statusz_field smoke-out frames_total) || out_frames1=0
sleep 1
out_frames2=$(statusz_field smoke-out frames_total) || out_frames2=0
(( out_frames2 > out_frames1 )) || fail "output frames not advancing: $out_frames1 → $out_frames2"

# MXL domain must contain the flows.
for flow in 5fbec3b1-1b0f-417d-9059-8b94a47197ed b3bb5be7-9fe9-4324-a5bb-4c70e1084449 db3bd465-2772-484f-8fac-830b0471258b; do
    [[ -e "$DOMAIN/$flow.mxl-flow" || -d "$DOMAIN/$flow" ]] || fail "flow $flow not materialized in domain"
done

metrics=$(curl -s --max-time 3 "http://127.0.0.1:$METRICS_PORT/metrics")
echo "$metrics" | grep -q 'mxl_decklink_frames_total{.*channel_label="smoke-in".*}' || fail "frames_total metric missing"
echo "$metrics" | grep -q 'mxl_grains_committed_total' || fail "grains_committed metric missing"
echo "$metrics" | grep -q 'mxl_flow_grain_commit_latency_seconds_bucket' || fail "commit latency histogram missing"
echo "$metrics" | grep -q 'mxl_active_video_flow_id' || fail "active flow info metric missing"
frames_metric=$(echo "$metrics" | grep 'mxl_decklink_frames_total' | grep 'smoke-in' | awk '{print $NF}')
python3 - "$frames_metric" <<'EOF' || fail "frames_total metric not > 0"
import sys
raise SystemExit(0 if float(sys.argv[1]) > 0 else 1)
EOF

say "test 3: graceful shutdown exits 0 within the grace period"
kill -TERM "$PID"
shutdown_ok=1
for _ in $(seq 1 100); do
    if ! kill -0 "$PID" 2>/dev/null; then
        shutdown_ok=0
        break
    fi
    sleep 0.1
done
if (( shutdown_ok != 0 )); then
    fail "process did not exit within 10 s of SIGTERM"
else
    wait "$PID"
    rc=$?
    [[ $rc == 0 ]] || fail "expected exit 0 on SIGTERM, got $rc"
fi
PID=""

# ---------------------------------------------------------------------------
say "test 4: signal loss degrades /readyz without killing the process"
rm -rf "$DOMAIN"; DOMAIN=$(mktemp -d /dev/shm/mxl-smoke.XXXXXX)
LOG2="/tmp/mxl-smoke-loss-$$.log"
common_env \
    MXL_HEALTH_MIN_HEALTHY_CHANNELS=2 \
    MOCK_SIGNAL_LOSS_AFTER_FRAMES=150 \
    MOCK_SIGNAL_LOSS_DURATION_FRAMES=150 \
    "$BIN" >"$LOG2" 2>&1 &
PID=$!

wait_for_readyz 30 || fail "signal-loss run: /readyz did not reach 200"

# The outage starts after 3 s (150 frames at 50 fps) and lasts 3 s.
degraded=0
deadline=$(( $(date +%s) + 10 ))
while (( $(date +%s) < deadline )); do
    if [[ "$(http_code /readyz)" == "503" ]]; then
        degraded=1
        break
    fi
    sleep 0.2
done
(( degraded == 1 )) || fail "/readyz never degraded to 503 during signal loss"
kill -0 "$PID" 2>/dev/null || fail "process died during signal loss"

recovered=0
deadline=$(( $(date +%s) + 15 ))
while (( $(date +%s) < deadline )); do
    if [[ "$(http_code /readyz)" == "200" ]]; then
        recovered=1
        break
    fi
    sleep 0.2
done
(( recovered == 1 )) || fail "/readyz did not recover to 200 after signal returned"
grep -q '"event":"signal_lost"' "$LOG2" || fail "signal_lost event not logged"

kill -TERM "$PID" 2>/dev/null
wait "$PID" 2>/dev/null
PID=""

# ---------------------------------------------------------------------------
say "test 5: auto format change replaces the video flow with a new UUID (§3.8)"
rm -rf "$DOMAIN"; DOMAIN=$(mktemp -d /dev/shm/mxl-smoke.XXXXXX)
# Short domain history keeps the HD1080p50 auto-start ring inside a 64 MiB /dev/shm.
printf '{"urn:x-mxl:option:history_duration/v1.0": 100000000}' > "$DOMAIN/options.json"
LOG3="/tmp/mxl-smoke-fc-$$.log"
fc_env() {
    exec env -i \
        PATH="$PATH" LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" \
        MXL_DECKLINK_BACKEND=mock MXL_DECKLINK_CARD_ID=0xa1b2c3d4 \
        MXL_DOMAIN_PATH="$DOMAIN" HEALTH_PORT="$HEALTH_PORT" METRICS_PORT="$METRICS_PORT" \
        MOCK_FORMAT_CHANGE_AFTER_FRAMES=100 MOCK_FORMAT_CHANGE_MODE=HD720p50 \
        CH0_DIRECTION=input CH0_SUBDEVICE_INDEX=0 CH0_VIDEO_MODE=auto \
        CH0_AUDIO_CHANNEL_COUNT=2 \
        CH0_MXL_VIDEO_FLOW_ID=5fbec3b1-1b0f-417d-9059-8b94a47197ed \
        CH0_MXL_AUDIO_FLOW_ID=b3bb5be7-9fe9-4324-a5bb-4c70e1084449 \
        CH0_LABEL=smoke-fc \
        "$BIN"
}
fc_env >"$LOG3" 2>&1 &
PID=$!

wait_for_readyz 30 || fail "format-change run: /readyz did not reach 200"
# The change fires after 2 s (100 frames at 50 fps) + 5 s reader grace.
new_flow=""
deadline=$(( $(date +%s) + 20 ))
while (( $(date +%s) < deadline )); do
    flow=$(statusz_field smoke-fc active_video_flow_id 2>/dev/null) || flow=""
    if [[ -n "$flow" && "$flow" != "5fbec3b1-1b0f-417d-9059-8b94a47197ed" ]]; then
        new_flow="$flow"
        break
    fi
    sleep 0.5
done
if [[ -z "$new_flow" ]]; then
    fail "video flow UUID was not replaced after the format change"
else
    say "flow replaced: 5fbec3b1-… → $new_flow"
fi
wait_for_readyz 15 || fail "format-change run: channel did not return to healthy"
grep -q '"event":"video_flow_replaced"' "$LOG3" || fail "video_flow_replaced event not logged"

kill -TERM "$PID" 2>/dev/null
wait "$PID" 2>/dev/null
rc=$?
[[ $rc == 0 ]] || fail "format-change run: expected exit 0 on SIGTERM, got $rc"
PID=""

rm -f "$LOG" "$LOG2" "$LOG3"

# ---------------------------------------------------------------------------
if (( FAILURES > 0 )); then
    echo "[smoke] $FAILURES failure(s)" >&2
    exit 1
fi
say "all smoke tests passed"
