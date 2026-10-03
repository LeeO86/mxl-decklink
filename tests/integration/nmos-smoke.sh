#!/usr/bin/env bash
# Confirms the binary was linked with the BCP-007-03 node and that the Node
# and Connection APIs answer. DNS-SD may log errors when avahi-daemon is absent;
# registration is not required for this check.
#
# Usage: tests/integration/nmos-smoke.sh [path-to-mxl-decklink]
set -u -o pipefail

BIN="${1:-build/mxl-decklink}"
WEB_PORT="${WEB_PORT_OVERRIDE:-19084}"
NMOS_PORT="${NMOS_PORT_OVERRIDE:-13214}"

PID=""
DOMAIN=""
LOG=""

say() { echo "[nmos-smoke] $*"; }

fail() {
    echo "[nmos-smoke] FAIL: $*" >&2
    if [[ -n "$LOG" && -f "$LOG" ]]; then
        echo "[nmos-smoke] --- log ---" >&2
        tail -n 40 "$LOG" >&2 || true
    fi
    exit 1
}

cleanup() {
    if [[ -n "$PID" ]] && kill -0 "$PID" 2>/dev/null; then
        kill -TERM "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    [[ -n "$DOMAIN" ]] && rm -rf "$DOMAIN"
    [[ -n "$LOG" ]] && rm -f "$LOG"
}
trap cleanup EXIT

command -v curl >/dev/null || { echo "[nmos-smoke] missing tool: curl" >&2; exit 2; }
command -v python3 >/dev/null || { echo "[nmos-smoke] missing tool: python3" >&2; exit 2; }
[[ -x "$BIN" ]] || { echo "[nmos-smoke] binary not found: $BIN" >&2; exit 2; }

DOMAIN=$(mktemp -d /dev/shm/mxl-nmos-smoke.XXXXXX)
LOG=$(mktemp /tmp/nmos-smoke.XXXXXX.log)

say "starting mock node on :$NMOS_PORT"
env -i \
    PATH="$PATH" \
    LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" \
    MXL_DECKLINK_BACKEND=mock \
    MXL_DECKLINK_CARD_ID=0xa1b2c3d4 \
    MXL_DOMAIN_PATH="$DOMAIN" \
    MXL_DOMAIN_SCAN_PATH=/dev/shm \
    WEB_PORT="$WEB_PORT" \
    NMOS_ENABLE=true \
    NMOS_PORT="$NMOS_PORT" \
    LOG_LEVEL=info \
    CH0_DIRECTION=input \
    CH0_SUBDEVICE_INDEX=0 \
    CH0_VIDEO_MODE=HD720p50 \
    CH0_AUDIO_CHANNEL_COUNT=2 \
    CH0_MXL_VIDEO_FLOW_ID=5fbec3b1-1b0f-417d-9059-8b94a47197ed \
    CH0_AF0_FLOW_ID=b3bb5be7-9fe9-4324-a5bb-4c70e1084449 \
    CH0_AF0_CHANNEL_COUNT=2 \
    CH0_AF0_MAP=0,1 \
    CH0_LABEL=nmos-smoke-in \
    CH1_DIRECTION=output \
    CH1_SUBDEVICE_INDEX=1 \
    CH1_VIDEO_MODE=HD720p50 \
    CH1_AUDIO_CHANNEL_COUNT=2 \
    CH1_MXL_VIDEO_FLOW_ID=5fbec3b1-1b0f-417d-9059-8b94a47197ed \
    CH1_AF0_FLOW_ID=b3bb5be7-9fe9-4324-a5bb-4c70e1084449 \
    CH1_AF0_CHANNEL_COUNT=2 \
    CH1_AF0_MAP=0,1 \
    CH1_LABEL=nmos-smoke-out \
    "$BIN" >"$LOG" 2>&1 &
PID=$!

deadline=$(( $(date +%s) + 20 ))
ready=0
while (( $(date +%s) < deadline )); do
    if ! kill -0 "$PID" 2>/dev/null; then
        fail "process exited before the Node API was ready"
    fi
    if curl -sf --max-time 2 "http://127.0.0.1:${NMOS_PORT}/x-nmos/node/v1.3/self" >/dev/null; then
        ready=1
        break
    fi
    sleep 0.3
done
[[ "$ready" == 1 ]] || fail "Node API did not answer on :${NMOS_PORT}"

python3 - "$NMOS_PORT" <<'PY' || fail "Node or Connection API did not match BCP-007-03"
import json, sys, urllib.error, urllib.request
port = sys.argv[1]
base = f"http://127.0.0.1:{port}"

def get(path):
    with urllib.request.urlopen(base + path, timeout=3) as resp:
        return resp.status, json.load(resp)

status, self_doc = get("/x-nmos/node/v1.3/self")
assert status == 200, status
versions = self_doc.get("api", {}).get("versions", [])
assert "v1.3" in versions, versions

status, senders = get("/x-nmos/node/v1.3/senders")
assert senders, "no senders"
for sender in senders:
    assert sender.get("transport") == "urn:x-nmos:transport:mxl", sender.get("transport")
    assert sender.get("manifest_href") is None
    assert sender.get("interface_bindings") == []

status, receivers = get("/x-nmos/node/v1.3/receivers")
assert receivers, "no receivers"
for receiver in receivers:
    assert receiver.get("transport") == "urn:x-nmos:transport:mxl"
    media_types = (receiver.get("caps") or {}).get("media_types") or []
    assert media_types, receiver["id"]

req = urllib.request.Request(base + f"/x-nmos/connection/v1.2/single/senders/{senders[0]['id']}/transportfile")
try:
    urllib.request.urlopen(req, timeout=3)
    raise SystemExit("transportfile should be 404")
except urllib.error.HTTPError as exc:
    assert exc.code == 404, exc.code
print("ok", len(senders), "senders", len(receivers), "receivers")
PY

say "Node API v1.3 and Connection API v1.2 answered"
kill -TERM "$PID"
wait "$PID"
rc=$?
PID=""
[[ "$rc" == 143 ]] || fail "expected exit 143 on SIGTERM, got $rc"
say "passed"
