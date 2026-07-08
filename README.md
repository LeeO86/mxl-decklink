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
- **Ops**: `/livez`, `/readyz`, `/statusz` on `HEALTH_PORT` (default 9080),
  Prometheus `/metrics` on `METRICS_PORT` (default 9090), structured JSON
  logging.
- **Config**: environment variables only — global `MXL_*`/ops variables plus
  indexed `CHx_*` per-channel blocks; fully backward compatible with the
  v1.0 single-channel variable set. Invalid config exits 78 (`EX_CONFIG`).

## Building

Requirements: Linux, CMake ≥ 3.24, GCC ≥ 12 or Clang ≥ 16, and an installed
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
`blackmagic`/`blackmagic-io` kernel modules loaded, a tmpfs MXL domain
(default `/dev/shm/mxl`), and TAI-disciplined system time (chrony with a
correct kernel TAI offset).

Minimal single-channel example:

```bash
MXL_DECKLINK_CARD_ID=0xa1b2c3d4 \
MXL_DOMAIN_PATH=/dev/shm/mxl \
CH0_DIRECTION=input \
CH0_SUBDEVICE_INDEX=0 \
CH0_VIDEO_MODE=auto \
CH0_MXL_VIDEO_FLOW_ID=5fbec3b1-1b0f-417d-9059-8b94a47197ed \
CH0_MXL_AUDIO_FLOW_ID=b3bb5be7-9fe9-4324-a5bb-4c70e1084449 \
./build/mxl-decklink
```

The full variable reference is SPECIFICATION.md §4 (global) and §4.2
(per-channel `CHx_*`). Docker Compose and Kubernetes examples live in
[`docker/docker-compose.yaml`](docker/docker-compose.yaml),
[`deploy/mxl-decklink.yaml`](deploy/mxl-decklink.yaml) and
[`deploy/generic-device-plugin.yaml`](deploy/generic-device-plugin.yaml).

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
