#!/usr/bin/env bash
# Confirms the binary was linked with the BCP-007-03 node and that the Node
# and Connection APIs answer. DNS-SD may log errors when avahi-daemon is absent;
# registration is not required for this check. Then routes the output's audio
# receiver to another function's domain and parks its video receiver over
# IS-05, restarts the process, and checks that both routes are restored
# (state/connections.json, SPECIFICATION.md §7.7).
#
# Usage: tests/integration/nmos-smoke.sh [path-to-mxl-decklink]
set -u -o pipefail

BIN="${1:-build/mxl-decklink}"
WEB_PORT="${WEB_PORT_OVERRIDE:-19084}"
NMOS_PORT="${NMOS_PORT_OVERRIDE:-13214}"

PID=""
DOMAIN=""
OTHER=""
CONFIG=""
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
    [[ -n "$OTHER" ]] && rm -rf "$OTHER"
    [[ -n "$CONFIG" ]] && rm -rf "$CONFIG"
    [[ -n "$LOG" ]] && rm -f "$LOG"
}
trap cleanup EXIT

command -v curl >/dev/null || { echo "[nmos-smoke] missing tool: curl" >&2; exit 2; }
command -v python3 >/dev/null || { echo "[nmos-smoke] missing tool: python3" >&2; exit 2; }
[[ -x "$BIN" ]] || { echo "[nmos-smoke] binary not found: $BIN" >&2; exit 2; }

DOMAIN=$(mktemp -d /dev/shm/mxl-nmos-smoke.XXXXXX)
# Another function's output domain (the audio of the output comes from there).
OTHER=$(mktemp -d /dev/shm/mxl-nmos-smoke-other.XXXXXX)
OTHER_ID=7d3c2b1a-0f9e-4d8c-8b7a-6f5e4d3c2b1a
printf '{"id":"%s","label":"nmos-smoke other function"}\n' "$OTHER_ID" >"$OTHER/domain_def.json"
CONFIG=$(mktemp -d /tmp/nmos-smoke-config.XXXXXX)
LOG=$(mktemp /tmp/nmos-smoke.XXXXXX.log)

start_node() {
env -i \
    PATH="$PATH" \
    LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" \
    MXL_DECKLINK_BACKEND=mock \
    MXL_DECKLINK_CARD_ID=0xa1b2c3d4 \
    MXL_DOMAIN_PATH="$DOMAIN" \
    MXL_DOMAIN_SCAN_PATH=/dev/shm \
    CONFIG_DIR="$CONFIG" \
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
    "$BIN" >>"$LOG" 2>&1 &
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
}

stop_node() {
    kill -TERM "$PID"
    wait "$PID"
    rc=$?
    PID=""
    [[ "$rc" == 143 ]] || fail "expected exit 143 on SIGTERM, got $rc"
}

say "starting mock node on :$NMOS_PORT"
start_node

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

# IS-05: the output's audio from the other domain, its video parked.
python3 - "$NMOS_PORT" "$OTHER_ID" <<'PY' || fail "IS-05 activation of the output receivers failed"
import json, sys, urllib.request
port, other = sys.argv[1], sys.argv[2]
base = f"http://127.0.0.1:{port}"
with urllib.request.urlopen(base + "/x-nmos/node/v1.3/receivers", timeout=3) as resp:
    receivers = json.load(resp)
def receiver(fmt):
    return next(r for r in receivers if r["format"] == fmt and r["label"].startswith("nmos-smoke-out"))
def patch(rid, body):
    req = urllib.request.Request(base + f"/x-nmos/connection/v1.2/single/receivers/{rid}/staged",
                                 data=json.dumps(body).encode(), method="PATCH",
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=5) as resp:
        assert resp.status == 200, resp.status
audio = receiver("urn:x-nmos:format:audio")
video = receiver("urn:x-nmos:format:video")
patch(audio["id"], {"master_enable": True, "sender_id": "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d",
                    "activation": {"mode": "activate_immediate"},
                    "transport_params": [{"mxl_domain_id": other, "mxl_flow_id": "c0ffee00-1111-4222-8333-444455556666"}]})
patch(video["id"], {"master_enable": False, "activation": {"mode": "activate_immediate"}})
print(audio["id"], video["id"])
PY

deadline=$(( $(date +%s) + 10 ))
until [[ $(grep -c '"event":"nmos_connection_saved"' "$LOG") -ge 2 ]]; do
    (( $(date +%s) < deadline )) || fail "the activations were not saved"
    sleep 0.2
done
[[ -f "$CONFIG/state/connections.json" ]] || fail "no $CONFIG/state/connections.json"
say "activations saved to state/connections.json; restarting"
stop_node
start_node

python3 - "$NMOS_PORT" "$OTHER_ID" <<'PY' || fail "the IS-05 routes were not restored after the restart"
import json, sys, time, urllib.request
port, other = sys.argv[1], sys.argv[2]
base = f"http://127.0.0.1:{port}"
def get(path):
    with urllib.request.urlopen(base + path, timeout=3) as resp:
        return json.load(resp)
receivers = get("/x-nmos/node/v1.3/receivers")
def receiver(fmt):
    return next(r for r in receivers if r["format"] == fmt and r["label"].startswith("nmos-smoke-out"))
audio = get(f"/x-nmos/connection/v1.2/single/receivers/{receiver('urn:x-nmos:format:audio')['id']}/active")
video = get(f"/x-nmos/connection/v1.2/single/receivers/{receiver('urn:x-nmos:format:video')['id']}/active")
assert audio["master_enable"] is True, audio
assert audio["sender_id"] == "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d", audio
params = audio["transport_params"][0]
assert params["mxl_domain_id"] == other, params
assert params["mxl_flow_id"] == "c0ffee00-1111-4222-8333-444455556666", params
assert video["master_enable"] is False, video
print("restored", json.dumps(params))
PY
grep -q '"event":"nmos_connection_restored"' "$LOG" || fail "no nmos_connection_restored log"
say "IS-05 routes restored after the restart"
stop_node
say "passed"
