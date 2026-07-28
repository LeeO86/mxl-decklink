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
MXL v1.0.1 API rather than the spec's paraphrase of it).

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
- **Config**: environment variables, optionally layered over a JSON
  configuration file (`MXL_CONFIG_FILE`, spec §4.5) that the web interface
  persists to. Precedence: env > file > default; env-set keys are shown
  read-only in the UI. Indexed `CHx_*` per-channel blocks; fully backward
  compatible with the v1.0 single-channel variable set. Invalid config exits
  78 (`EX_CONFIG`).

## Building

Requirements: Linux, CMake ≥ 3.24, GCC ≥ 12 or Clang ≥ 16, Node.js ≥ 20
(for the Vue web UI build), and an installed
[MXL](https://github.com/dmf-mxl/mxl) v1.0.1 (`find_package(mxl)`).

```bash
# Build and install MXL v1.0.1 first (uses vcpkg for its dependencies):
git clone --branch v1.0.1 https://github.com/dmf-mxl/mxl
cmake -S mxl -B mxl/build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF \
  -DBUILD_UTILS=OFF -DBUILD_DOCS=OFF -DCMAKE_INSTALL_PREFIX=/opt/mxl
cmake --build mxl/build -j && sudo cmake --install mxl/build

# Then this project:
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_PREFIX_PATH=/opt/mxl;$(pwd)/../mxl/build/vcpkg_installed/x64-linux"
cmake --build build -j
```

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

Or build the container image (multi-stage, builds MXL internally):

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
The full variable reference is SPECIFICATION.md §4 (global) and §4.2
(per-channel `CHx_*`, including the `CHx_AFn_*` audio routing matrix). Docker Compose and Kubernetes examples live in
[`docker/docker-compose.yaml`](docker/docker-compose.yaml),
[`deploy/mxl-decklink.yaml`](deploy/mxl-decklink.yaml) and
[`deploy/generic-device-plugin.yaml`](deploy/generic-device-plugin.yaml).

Then open the web interface at `http://<host>:8080/` for interactive setup:
mount a config volume and set `MXL_CONFIG_FILE=/config/mxl-decklink.json` so
changes persist. The process runs as UID 1000 and must be able to create/rewrite
that file — fix ownership once with a throwaway container (do **not** run the
service as root just to chown):

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
| 0 | clean shutdown (SIGTERM/SIGINT) |
| 2 | card profile changed externally (§3.9 fail-fast) |
| 75 | card-level startup failed after retries (`EX_TEMPFAIL`) |
| 78 | invalid configuration (`EX_CONFIG`) |
| 143 | shutdown grace period exceeded, forced exit |

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
```

## Known deviations from SPECIFICATION.md

Documented in detail in IMPLEMENTATION_PLAN.md §3:

- **Ring depth** (`CHx_GRAIN_COUNT`, `CHx_AUDIO_BUFFER_MS`): MXL v1.0.1 sizes
  ring buffers domain-globally from the `history_duration` option in
  `{domain}/options.json`, not per flow. The container logs a warning when
  the actual depth differs from the requested one and exposes the actual
  value via `/statusz` and logs. It never rewrites a mounted domain's
  `options.json`.
- **`mxlFlowSynchronizationGroup`** does not exist in MXL v1.0.1; output
  alignment uses per-flow readers with TAI pacing instead.
- **Grain commit semantics** follow the real API (`validSlices`/`totalSlices`
  and `MXL_GRAIN_FLAG_INVALID`) rather than the spec's `committedSize` field.

## Pre-go-live checks (hardware required, spec §9)

- DeckLink IP 100G sub-device enumeration against a real card.
- MXL handle thread-safety confirmation with the MXL maintainers.
- Empirical resource sizing on target hardware (the §6.3 table is estimates).

## License

MIT for this project's code (see [`LICENSE`](LICENSE)). Vendored components:
`third_party/decklink/` under the Blackmagic Design license headers contained
in those files; `third_party/doctest/doctest.h` under MIT. MXL itself is
Apache-2.0.
