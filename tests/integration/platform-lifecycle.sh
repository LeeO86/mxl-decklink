#!/usr/bin/env bash
# Platform lifecycle: start → /readyz (registry registration) → SIGTERM →
# exit 143, Registration DELETE, own output domain removed.
set -u -o pipefail

BIN="${1:-build/mxl-decklink}"
ROOT=$(mktemp -d /dev/shm/mxl-life.XXXXXX)
DOMAIN="$ROOT/instance"
REG_PORT="${REG_PORT_OVERRIDE:-18110}"
QUERY_PORT="${QUERY_PORT_OVERRIDE:-18111}"
WEB_PORT="${WEB_PORT_OVERRIDE:-18180}"
NMOS_PORT="${NMOS_PORT_OVERRIDE:-18112}"
LOG=$(mktemp /tmp/mxl-life.XXXXXX.log)
MOCK_LOG=$(mktemp /tmp/mxl-life-reg.XXXXXX.log)
PID=""
MOCK_PID=""

fail() {
    echo "[lifecycle] FAIL: $*" >&2
    echo "[lifecycle] --- app ---" >&2
    tail -n 80 "$LOG" >&2 || true
    echo "[lifecycle] --- registry ---" >&2
    tail -n 40 "$MOCK_LOG" >&2 || true
    exit 1
}

cleanup() {
    if [[ -n "$PID" ]] && kill -0 "$PID" 2>/dev/null; then
        kill -TERM "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    if [[ -n "$MOCK_PID" ]] && kill -0 "$MOCK_PID" 2>/dev/null; then
        kill -TERM "$MOCK_PID" 2>/dev/null || true
        wait "$MOCK_PID" 2>/dev/null || true
    fi
    rm -rf "$ROOT"
    rm -f "$LOG" "$MOCK_LOG"
}
trap cleanup EXIT

[[ -x "$BIN" ]] || { echo "[lifecycle] binary not found: $BIN" >&2; exit 2; }
mkdir -p "$DOMAIN" "$ROOT/config"

python3 - "$REG_PORT" "$QUERY_PORT" "$MOCK_LOG" <<'PY' &
import json, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
reg_port, query_port, log_path = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
logf = open(log_path, "w", buffering=1)
resources = {}

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, fmt, *args):
        logf.write("%s %s\n" % (self.command, self.path))
        logf.flush()
    def _read(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        raw = self.rfile.read(n) if n else b""
        if self.command == "POST" and self.path.rstrip("/").endswith("/resource") and raw:
            try:
                doc = json.loads(raw.decode())
                data = doc.get("data") or {}
                if isinstance(data, dict) and "id" in data:
                    resources[data["id"]] = data
            except Exception:
                pass
        return raw
    def _send(self, code, body=b""):
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)
    def do_GET(self):
        if "/nodes/" in self.path:
            node_id = self.path.rstrip("/").split("/")[-1]
            data = resources.get(node_id)
            self._send(200 if data is not None else 404, json.dumps(data or {}).encode())
            return
        self._send(200, b"[]")
    def do_POST(self):
        self._read()
        if "/health/nodes/" in self.path:
            node_id = self.path.rstrip("/").split("/")[-1]
            self._send(200 if node_id in resources else 404, b"{}")
            return
        self._send(201, b"{}")
    def do_DELETE(self):
        self._read()
        self._send(204, b"")

def serve(port):
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()

threading.Thread(target=serve, args=(reg_port,), daemon=True).start()
serve(query_port)
PY
MOCK_PID=$!
sleep 0.4

env -i PATH="$PATH" LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" \
    MXL_DECKLINK_BACKEND=mock \
    MXL_DECKLINK_CARD_ID=0xa1b2c3d4 \
    MXL_DOMAIN_SCAN_PATH="$ROOT" \
    MXL_OUTPUT_DOMAIN_DIR="$DOMAIN" \
    MXL_OUTPUT_DOMAIN_ID=11111111-1111-4111-8111-111111111111 \
    MXL_CLEANUP_ON_EXIT=true \
    CONFIG_DIR="$ROOT/config" \
    WEB_PORT="$WEB_PORT" \
    NMOS_ENABLE=true \
    NMOS_DNS_SD=false \
    NMOS_SEED=lifecycle \
    NMOS_PORT="$NMOS_PORT" \
    NMOS_REGISTRY_ADDRESS=127.0.0.1 \
    NMOS_REGISTRY_PORT="$REG_PORT" \
    NMOS_QUERY_ADDRESS=127.0.0.1 \
    NMOS_QUERY_PORT="$QUERY_PORT" \
    NMOS_HOST_ADDRESS=10.255.0.10 \
    LOG_LEVEL=info \
    CH0_DIRECTION=input \
    CH0_SUBDEVICE_INDEX=0 \
    CH0_VIDEO_MODE=HD720p50 \
    CH0_AUDIO_ENABLE=false \
    CH0_MXL_VIDEO_FLOW_ID=5fbec3b1-1b0f-417d-9059-8b94a47197ed \
    CH0_LABEL=life-in \
    "$BIN" >"$LOG" 2>&1 &
PID=$!

deadline=$(( $(date +%s) + 25 ))
ready=0
while (( $(date +%s) < deadline )); do
    if ! kill -0 "$PID" 2>/dev/null; then
        fail "process exited before /readyz"
    fi
    code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:${WEB_PORT}/readyz" || true)
    if [[ "$code" == "200" ]]; then
        ready=1
        break
    fi
    sleep 0.4
done
[[ "$ready" == 1 ]] || fail "/readyz did not become 200"
[[ -f "$DOMAIN/domain_def.json" ]] || fail "output domain was not created"

kill -TERM "$PID"
wait "$PID"
rc=$?
PID=""
[[ "$rc" == 143 ]] || fail "expected exit 143, got $rc"
[[ ! -d "$DOMAIN" ]] || fail "own output domain was not removed"
if ! grep -q 'DELETE ' "$MOCK_LOG"; then
    fail "registry did not see a DELETE"
fi
echo "[lifecycle] passed"
MOCK_PID_SAVE=$MOCK_PID
MOCK_PID=""
kill -TERM "$MOCK_PID_SAVE" 2>/dev/null || true
wait "$MOCK_PID_SAVE" 2>/dev/null || true
trap - EXIT
rm -rf "$ROOT"
rm -f "$LOG" "$MOCK_LOG"
