# mxl-decklink

**MXL MediaFunction container for Blackmagic DeckLink** — a bidirectional
bridge between DeckLink capture/playback hardware (SDI and IP) and the
[Media eXchange Layer](https://github.com/dmf-mxl/mxl) (MXL) shared-memory
data plane of the EBU/Linux Foundation Dynamic Media Facility.

One container process exclusively owns **one physical DeckLink card** and
serves **1..16 logical channels** on it — input channels (`DeckLink → MXL`)
and output channels (`MXL → DeckLink`) in any combination. See
[`SPECIFICATION.md`](SPECIFICATION.md) for the normative specification and
[`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) for how this codebase maps
onto it (including the few places where the implementation follows the actual
MXL v1.1 API rather than the spec's paraphrase of it).

## Feature summary

- **Input path**: DeckLink v210 frames → MXL `video/v210` grains (single
  `memcpy`), interleaved PCM → deinterleaved `audio/float32` sample batches,
  optional SMPTE 291 ANC → `video/smpte291` grains (RFC 8331 §2 payload).
- **Output path**: MXL grain reader with preroll → `ScheduleVideoFrame`
  completion-driven playback, `audio/float32` → interleaved PCM pull.
- **Timing**: TAI (`CLOCK_TAI`) grain indexing via the MXL time API;
  optional hardware-reference-clock calibration with rolling recalibration.
- **Resilience**: per-channel fault isolation with exponential-backoff
  reconnect; signal-loss standby + stream reset; auto format detection with
  flow replacement (new UUID) on format change; card-profile ownership with
  fail-fast (exit 2) on external profile changes.
- **Ops + web control** (spec §7.1 / §7.5): one consolidated HTTP port
  `WEB_PORT` (default 8080) serves `/livez`, `/readyz`, `/statusz`, Prometheus
  `/metrics`, and — when `WEB_ENABLE=true` — the embedded Vue SPA + REST API
  (dashboard, per-channel forms that adapt to the matched card's sub-devices,
  live DeckLink SDK status, MXL domain/flow browser with flow→output
  assignment and domain creation; domain deletion is not supported).
  Per-channel changes apply at runtime; global changes are flagged
  `restart_required`. Unauthenticated by design — keep it on protected
  networks or set `WEB_ENABLE=false` (health/metrics remain). Structured JSON
  logging.
- **NMOS**: the container image and CI link Sony nmos-cpp. With
  `NMOS_ENABLE=true` the process is an AMWA IS-04 v1.3 / IS-05 v1.2 node for
  MXL ([BCP-007-03](https://specs.amwa.tv/bcp-007-03/)). Input channels are MXL
  senders, output channels are MXL receivers (`urn:x-nmos:transport:mxl`).
  A controller connects them with `mxl_domain_id` and `mxl_flow_id`; there is
  no SDP. This is separate from a DeckLink IP card's own ST 2110 NMOS node.
- **Config**: environment variables, optionally layered over a JSON
  configuration file (`MXL_CONFIG_FILE`, spec §4.5) that the web interface
  persists to. Precedence: env > file > default; env-set keys are shown
  read-only in the UI. Indexed `CHx_*` per-channel blocks; fully backward
  compatible with the v1.0 single-channel variable set. Invalid config exits
  78 (`EX_CONFIG`).

## Building

Requirements: Linux, CMake ≥ 3.24, GCC ≥ 12 or Clang ≥ 16, Node.js ≥ 20
(for the Vue web UI build), and an installed
[MXL](https://github.com/dmf-mxl/mxl) v1.1.0 (`find_package(mxl)`).

```bash
# Build and install MXL v1.1.0 first (uses vcpkg for its dependencies):
git clone --branch v1.1.0 https://github.com/dmf-mxl/mxl
cmake -S mxl -B mxl/build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF \
  -DBUILD_UTILS=OFF -DBUILD_DOCS=OFF -DMXL_ENABLE_FABRICS_OFI=OFF \
  -DCMAKE_INSTALL_PREFIX=/opt/mxl
cmake --build mxl/build -j && sudo cmake --install mxl/build

# Then this project:
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_PREFIX_PATH=/opt/mxl;$(pwd)/../mxl/build/vcpkg_installed/x64-linux"
cmake --build build -j
```

### NMOS node (BCP-007-03)

The published image and CI already link this in. A local build opts in the
same way. It links [sony/nmos-cpp](https://github.com/sony/nmos-cpp) at the
commit in `docker/Dockerfile` (`NMOS_CPP_REF`; that tree contains the MXL
transport, the older Conan Center package does not). Dependencies are Boost,
the C++ REST SDK, OpenSSL, and Avahi's `libdns_sd` compatibility library.
Pass the `Development` directory:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DMXL_DECKLINK_NMOS=ON \
  -DNMOS_CPP_DIR=/path/to/nmos-cpp/Development \
  "-DCMAKE_PREFIX_PATH=/opt/mxl;$(pwd)/../mxl/build/vcpkg_installed/x64-linux"
cmake --build build -j
```

Run with `NMOS_ENABLE=true`. Point `NMOS_REGISTRY_ADDRESS` at an IS-04
registry, or leave it empty to discover one with DNS-SD (Avahi, below).

**Ports.** The web UI, REST API, health, and metrics stay on `WEB_PORT`
(default 8080). That is this process's own HTTP server. The NMOS HTTP APIs
share a second listener, `NMOS_PORT` (default 3212): Node, Connection, and
Events. IS-04/IS-05 subscriptions use a WebSocket listener on `NMOS_PORT + 1`
(3213). nmos-cpp binds those as two sockets. Putting the WebSocket on
`NMOS_PORT` as well drops the HTTP APIs: clients then get `426 Upgrade
Required` from the WebSocket listener. `NMOS_PORT` and `NMOS_PORT + 1` must
not collide with `WEB_PORT`.

**DNS-SD.** Leave `NMOS_REGISTRY_ADDRESS` empty only when an `avahi-daemon`
is reachable on the system D-Bus and mDNS (UDP 5353) can leave the host.
Docker bridge networks and typical Kubernetes CNIs do not forward mDNS, so
the process logs `DNSServiceCreateConnection` errors and does not register.
Unicast registration needs no daemon: set `NMOS_REGISTRY_ADDRESS` (and
`NMOS_REGISTRY_PORT`, default 3210) and keep the published ports above.

Docker, using the host's Avahi (the host must already run `avahi-daemon`):

```yaml
network_mode: host
volumes:
  - /run/dbus:/run/dbus
  - /run/avahi-daemon:/run/avahi-daemon
```

`ports:` is ignored with host networking; the process binds 8080, 3212, and
3213 on the host. A Kubernetes pod needs those same two mounts, plus
`hostNetwork: true` and `dnsPolicy: ClusterFirstWithHostNet`, because the
pod otherwise has no multicast path to the LAN. `deploy/mxl-decklink.yaml`
does not set `hostNetwork`; add `NMOS_REGISTRY_ADDRESS` there for unicast
registration.

Senders write `MXL_OUTPUT_DOMAIN_DIR` (`MXL_DOMAIN_PATH` is the same setting).
Its `domain_def.json` `id` is `mxl_domain_id`. Receivers may read any domain
under `MXL_DOMAIN_SCAN_PATH`, also one created after the start: an activation
that names an unknown domain scans the path again. `master_enable` starts and stops that
leg's MXL writer or reader and is stored in the config file when the
environment does not already pin `CHx_MXL_ACTIVE` / `CHx_AFn_MXL_ACTIVE`.

### Exit codes

| Code | When |
|---|---|
| 0 | The process is still running. SIGTERM does not use 0. |
| 2 | The card profile changed outside this process. |
| 75 | Temporary failure: card retries exhausted, or a TCP port cannot be bound. |
| 78 | Invalid configuration, including a `domain_def.json` id that does not match `MXL_OUTPUT_DOMAIN_ID`. |
| 143 | SIGTERM or SIGINT finished (or the shutdown watchdog fired). |

### HTTP API

Always: `GET /livez`, `GET /readyz`, `GET /statusz`, `GET /metrics`, `GET /api/v1/config/export`, `POST /api/v1/config/import`.

With `WEB_ENABLE=true`: `GET /`, `GET /api/status`, `GET /api/card`, `GET /api/config`, `PUT /api/config`, `GET /api/domains`, `POST /api/domains`, `GET /api/flows`.

The export document is `{"settings":{...}}` of the config file. It contains no secrets (this function stores none). Import replaces that file. Environment variables still win on the next start.

Settings are listed in `SPECIFICATION.md` §4. On the platform, set `NMOS_SEED`, `NMOS_TAGS`, `NMOS_REGISTRY_ADDRESS`, `NMOS_DNS_SD=false`, `MXL_OUTPUT_DOMAIN_DIR`, `MXL_DOMAIN_SCAN_PATH=/Volumes/mxl`, and `MXL_CLEANUP_ON_EXIT=true`. Leave `NMOS_HOST_ADDRESS` unset so the pod IP is announced.


The Blackmagic **DeckLink interface headers** are vendored under
`third_party/decklink/` (the same Blackmagic-licensed copies GStreamer
redistributes); `libDeckLinkAPI.so` is **never linked** — it is `dlopen`ed at
runtime, so the binary builds and runs without Desktop Video installed.

### Prebuilt images

CI publishes the container to GitHub Container Registry
(`ghcr.io/leeo86/mxl-decklink`):

| Tag | Meaning |
|---|---|
| `1.2.3`, `1.2`, `1`, `latest` | releases (git tags `v*.*.*`) |
| `nightly-dev` | latest build from `main` |
| `git-<sha>` | every published build, for pinning |

The image includes the BCP-007-03 node. Set `NMOS_ENABLE=true` to serve it
on `NMOS_PORT` (default 3212) with WebSocket subscriptions on the next port.
See "NMOS node" below for Avahi / DNS-SD.

Or build the container image (multi-stage, builds MXL and nmos-cpp internally):

```bash
docker build -f docker/Dockerfile .
# bundled Desktop Video userland (must match the host driver version):
docker build -f docker/Dockerfile \
  --build-arg DESKTOPVIDEO_DEB_URL=https://…/desktopvideo_16.0_amd64.deb .
```

## Running

Host prerequisites (§5.4): Blackmagic Desktop Video ≥ 16.0 with the
`blackmagic`/`blackmagic-io` kernel modules loaded, a tmpfs MXL domain, and
TAI-disciplined system time (chrony with a correct kernel TAI offset).

Recommended host tmpfs (CBC [`mxl-hands-on`](https://github.com/cbcrc/mxl-hands-on)
pattern — see `docker/docker-compose.yaml` for the full Compose example):

```bash
sudo mkdir -p /Volumes/mxl && sudo chown 1000:1000 /Volumes/mxl
echo 'tmpfs /Volumes/mxl tmpfs defaults,noatime,size=8G,uid=1000,gid=1000,mode=0755 0 0' \
  | sudo tee -a /etc/fstab
sudo mount /Volumes/mxl
mkdir -p /Volumes/mxl/mxl
```

Deployment lessons from field testing (details in SPECIFICATION.md §5):

- **Run as the uid/gid that owns the MXL domain** (e.g. `user: "1000:1000"`)
  and add the host `video` group (`group_add: [video]`) so `/dev/blackmagic`
  (`root:video` 0660) is accessible.
- **Prefer `DECKLINK_LIB_MODE=hostmount`** — bind-mount the host's
  `libDeckLinkAPI.so` read-only (path often
  `/usr/lib/x86_64-linux-gnu/libDeckLinkAPI.so` on Debian/Ubuntu; confirm with
  `find /usr -name 'libDeckLinkAPI.so'`). The bundled `.deb` only works when it
  exactly matches the host driver version.
- **Mount a dedicated MXL tmpfs** (`/Volumes/mxl`), not the host's whole
  `/dev/shm`, so unrelated shared-memory files stay out of the container.
  Mount only one domain subdirectory when sibling discovery is not needed.

Minimal single-channel example (local/CI can keep using `/dev/shm/mxl`):

```bash
MXL_DECKLINK_CARD_ID=0xa1b2c3d4 \
MXL_DOMAIN_PATH=/Volumes/mxl/mxl \
CH0_DIRECTION=input \
CH0_SUBDEVICE_INDEX=0 \
CH0_VIDEO_MODE=auto \
CH0_MXL_VIDEO_FLOW_ID=5fbec3b1-1b0f-417d-9059-8b94a47197ed \
CH0_AF0_FLOW_ID=b3bb5be7-9fe9-4324-a5bb-4c70e1084449 \
CH0_AF0_CHANNEL_COUNT=2 \
CH0_AF0_MAP=0,1 \
./build/mxl-decklink
```

### First deploy (empty config)

No card selector and no `CHx_*` variables are required to bring the process up.
Defaults: `MXL_DECKLINK_CARD_INDEX=0`, `MXL_OUTPUT_DOMAIN_DIR=/Volumes/mxl/mxl-decklink`
(created if missing; `MXL_DOMAIN_PATH` is an alias), zero channels, readiness
threshold clamped to 0. Mount a config volume
and open the web UI to add channels:

```bash
# Absolutely minimal — UI on :8080, configure everything there
MXL_CONFIG_FILE=/config/mxl-decklink.json \
MXL_DECKLINK_BACKEND=mock \
./build/mxl-decklink
```

On real hardware, omit `MXL_DECKLINK_BACKEND` (or set `sdk`). If the card is not
yet visible, the process still starts the UI with a mock-card fallback when no
channels are configured; set `MXL_DECKLINK_CARD_ID` and restart before going live.

The full variable reference is SPECIFICATION.md §4 (global) and §4.2
(per-channel `CHx_*`, including the `CHx_AFn_*` audio routing matrix). Docker Compose and Kubernetes examples live in
[`docker/docker-compose.yaml`](docker/docker-compose.yaml),
[`deploy/mxl-decklink.yaml`](deploy/mxl-decklink.yaml) and
[`deploy/generic-device-plugin.yaml`](deploy/generic-device-plugin.yaml).

Then open the web interface at `http://<host>:8080/` for interactive setup:
mount a config volume and set `MXL_CONFIG_FILE=/config/mxl-decklink.json` so
changes persist. The Channels tab includes an **Open routing matrix…** dialog
for the DeckLink↔audio-flow crosspoints. The process runs as UID 1000 and must
be able to create/rewrite that file — fix ownership once with a throwaway
container (do **not** run the service as root just to chown):

```bash
sudo mkdir -p /var/lib/mxl-decklink/config
docker run --rm -v /var/lib/mxl-decklink/config:/config busybox \
  chown -R 1000:1000 /config
```

The Settings tab renders the effective configuration as a
copyable `KEY=value` block if you prefer to freeze a web-configured setup
back into environment variables (which then override the file and become
read-only in the UI).

### Exit codes

| Code | Meaning |
|---|---|
| 0 | The process is still running. SIGTERM does not use 0. |
| 2 | The card profile changed outside this process. |
| 75 | Temporary failure: card retries exhausted, or a TCP port cannot be bound. |
| 78 | Invalid configuration, including a `domain_def.json` id that does not match `MXL_OUTPUT_DOMAIN_ID`. |
| 143 | SIGTERM or SIGINT finished (or the shutdown watchdog fired). |

## Testing without hardware

The build always contains a deterministic **mock DeckLink backend**
(`MXL_DECKLINK_BACKEND=mock`): a software card with SMPTE-style bars, a frame
counter band, a 1 kHz tone, an ANC test packet, TAI-paced callbacks, and
scriptable fault injection (`MOCK_SIGNAL_LOSS_AFTER_FRAMES`,
`MOCK_FORMAT_CHANGE_AFTER_FRAMES`, `MOCK_SUBDEVICE_COUNT`). This drives the
identical channel/MXL code paths as real hardware.

```bash
# unit tests
LD_LIBRARY_PATH=/opt/mxl/lib ./build/unit-tests
# end-to-end smoke test (mock card + real MXL domain in /dev/shm)
LD_LIBRARY_PATH=/opt/mxl/lib tests/integration/smoke.sh build/mxl-decklink
# BCP-007-03 node (binary built with -DMXL_DECKLINK_NMOS=ON)
LD_LIBRARY_PATH=/opt/mxl/lib tests/integration/nmos-smoke.sh build/mxl-decklink
```

## Known deviations from SPECIFICATION.md

Documented in detail in IMPLEMENTATION_PLAN.md §3:

- **Ring depth** (`CHx_GRAIN_COUNT`, `CHx_AUDIO_BUFFER_MS`): MXL sizes ring
  buffers domain-globally from the `history_duration` option in
  `{domain}/options.json`, not per flow. The container logs a warning when
  the actual depth differs from the requested one and exposes the actual
  value via `/statusz` and logs. It never rewrites a mounted domain's
  `options.json`.
- **Audio batch size** is capped by `mxlFlowWriterGetMaxWriteLengthSamples` /
  `mxlFlowReaderGetMaxReadLengthSamples`. Larger DeckLink packets are split.
- **Output alignment** uses `mxlFlowSynchronizationGroup`: before each video
  grain is read, the channel waits until that grain and the audio samples at
  the same TAI time are available.
- **Grain commit semantics** follow the real API (`validSlices`/`totalSlices`
  and `MXL_GRAIN_FLAG_INVALID`) rather than the spec's `committedSize` field.

## Pre-go-live checks (hardware required, spec §9)

- DeckLink IP 100G sub-device enumeration against a real card. Done 2026-10-03, see below.
- MXL handle thread-safety confirmation with the MXL maintainers.
- Empirical resource sizing on target hardware (the §6.3 table is estimates). One data point below.

### Lab run 2026-10-03: DeckLink IP 100G

Host: Ubuntu 24.04, kernel 6.8, Desktop Video 16.1 (API 16.1, `hostmount`), 2× Xeon Gold 6136, chrony with TAI offset 37 s, image built from this repository (1.0.1).

- Enumeration: one card, persistent id `0x84ef8e60`, eight sub-devices `DeckLink IP 100G (1)`…`(8)` with persistent ids `0x84ef8e60`…`0x84ef8e67` and the same group id. Each reports capture, playback and format detection, no profile manager. PCIe Gen3 x8. `MXL_DECKLINK_CARD_ID=0x84ef8e60` selects it.
- Channels: three 1080p50 inputs (sub-devices 0, 2, 7, format detection, 16 DeckLink audio channels, one stereo flow, ANC) and one 1080p50 output (sub-device 1) fed by mxl-test-player through IS-05 (video and 16-channel audio).
- 3 minutes: 9003 frames per channel (50.0 fps), 0 dropped, 0 late output frames, output reader lag 0. `grain_commit_latency_seconds` mean 1.27 ms, p95 at or below 2.5 ms (`MXL_TIMESTAMP_SOURCE=hardware`). Process 0.62 cores and 1.5 GB RSS.
- The card was not locked to PTP (`reference_locked: false` on every sub-device). The 2110 inputs dropped together every 20–35 s during part of the run (all three `signal_lost` at the same time), and the hardware clock drifted about 5 ms per minute against TAI.
- Spec §3.6 says loss of PTP lock on the IP 100G degrades readiness and sets `mxl_ptp_locked=0`. Neither exists: input channels stay `healthy`, `/readyz` stays 200, and there is no PTP metric. Open.
- An output channel whose `CHx_MXL_VIDEO_FLOW_ID` is set in the environment only finds that flow in this function's own domain (`mxlCreateFlowReader` status 2 for a flow of another domain). Route outputs with IS-05; then the flow is found under `MXL_DOMAIN_SCAN_PATH`.

## License

MIT for this project's code (see [`LICENSE`](LICENSE)). Vendored components:
`third_party/decklink/` under the Blackmagic Design license headers contained
in those files; `third_party/doctest/doctest.h` under MIT. MXL itself is
Apache-2.0.
