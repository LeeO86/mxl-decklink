# mxl-decklink — Implementation Plan

This document describes how `SPECIFICATION.md` (v1.1) is implemented in this repository:
module boundaries, technology choices, deliberate deviations (with reasons), and the
verification strategy. It was written before the code and kept in sync with it.

## 1. Ground rules and technology choices

- **Language / toolchain:** C++20, CMake ≥ 3.24, GCC ≥ 12 or Clang ≥ 16. No exceptions
  crossing module boundaries on the hot path; `mxlStatus`/`HRESULT`-style result codes
  internally where it matters.
- **MXL:** built from `dmf-mxl/mxl` tag `v1.0.1` and consumed via
  `find_package(mxl CONFIG REQUIRED)`. Only the public C API (`mxl/mxl.h`, `mxl/flow.h`,
  `mxl/time.h`) is used.
- **DeckLink SDK:** the Linux interface headers plus `DeckLinkAPIDispatch.cpp`
  (`third_party/decklink/`, the Blackmagic-licensed copies that GStreamer redistributes,
  API version 12.1). `libDeckLinkAPI.so` is **never linked**; it is `dlopen`ed at runtime
  by the dispatch shim, so the binary builds and runs (with the mock backend, see §4)
  on machines without Desktop Video installed.
- **Zero third-party runtime dependencies** beyond `libmxl` and (optionally, at runtime)
  `libDeckLinkAPI.so`. Logging, the HTTP server, the Prometheus registry, JSON emission,
  env parsing, v210/PCM/ANC conversion are all implemented in-tree. Rationale: the hot
  path needs none of them, the control plane is small, and every avoided dependency
  simplifies the container image and the security review.
- **Tests:** doctest (single vendored header) for unit tests; a shell-driven integration
  smoke test that runs the full binary with the mock DeckLink backend against a real MXL
  domain in tmpfs and validates grains/samples with `mxl-info`-style checks.

## 2. Source layout

```
src/
  main.cpp                 — startup sequence §3.10, signal handling, shutdown
  version.hpp
  config/
    config.hpp/.cpp        — env schema §4 (global + CHx_), validation §4.3, legacy §4.4
    videomodes.hpp/.cpp    — mode table §3.2.1 (name ⇄ BMDDisplayMode, dims, rate)
  util/
    logging.hpp/.cpp       — structured JSON/text logger §7.4 (lock-free-ish, level filter)
    taiclock.hpp/.cpp      — CLOCK_TAI reads, hardware-offset calibration §3.5
    v210.hpp/.cpp          — row-bytes math, UYVY→v210 expansion §3.3
    audioconv.hpp/.cpp     — int16/int32 interleaved → float32 deinterleaved §3.4
    anc.hpp/.cpp           — SMPTE 291 packets → RFC 8331 §2 payload §2.3
    uuid.hpp/.cpp          — UUID parse/format/validate
    threading.hpp/.cpp     — SCHED_FIFO, affinity pinning §2.5
  mxlbridge/
    domain.hpp/.cpp        — one shared mxlInstance per process §2.2, GC timer
    flowdef.hpp/.cpp       — NMOS-style flow-descriptor JSON builders (video/audio/anc)
    videowriter.hpp/.cpp   — grain open/copy/commit, invalid-grain marking
    videoreader.hpp/.cpp   — grain fetch with timeout (output path)
    audiowriter.hpp/.cpp   — OpenSamples/CommitSamples batches, ring wraparound
    audioreader.hpp/.cpp   — GetSamples (output path)
    ancwriter.hpp/.cpp     — smpte291 data grains
  decklink/
    types.hpp              — backend-neutral value types (modes, frames, packets)
    device.hpp             — abstract Device / CaptureSession / PlaybackSession API
    devicemanager.hpp/.cpp — enumeration, card matching §3.1, profile mgmt §3.9
    sdk/…                  — real backend on the Blackmagic COM interfaces
    mock/…                 — deterministic software card (bars + tone) for tests/CI
  channel/
    state.hpp              — channel state machine §7.2 (init/healthy/degraded/failed)
    input_channel.hpp/.cpp — DeckLink→MXL §2.3 (+ signal loss §3.6, format change §3.8)
    output_channel.hpp/.cpp— MXL→DeckLink §2.4 (preroll, scheduled playback)
    channel_manager.hpp/.cpp — per-channel threads, backoff reconnect §3.7
  ops/
    metrics.hpp/.cpp       — Prometheus text registry §7.3
    httpserver.hpp/.cpp    — minimal HTTP/1.1 server (poll loop, no deps)
    health.hpp/.cpp        — /livez /readyz /statusz §7.1
    housekeeping.hpp/.cpp  — liveness heartbeat, lock polling, watchdog §2.5
tests/
  unit/…                   — doctest binaries (config, modes, v210, audio, anc, metrics…)
  integration/smoke.sh     — end-to-end mock-card run against a tmpfs MXL domain
docker/                    — Dockerfile (multi-stage §5.1), entrypoint.sh, compose example
deploy/                    — Kubernetes manifests §6 (deployment, device-plugin config)
third_party/decklink/      — DeckLink API headers + dispatch (Blackmagic license header)
third_party/doctest/       — vendored test framework header
```

## 3. Mapping spec → implementation (and verified MXL v1.0.1 facts)

The MXL v1.0.1 sources were reviewed before design; the implementation is built on the
**actual** API, not on the spec's paraphrase of it. Differences that matter:

| Spec says | MXL v1.0.1 reality | Implementation |
|---|---|---|
| `mxlFlowWriterOpenGrain(inst, writer, index, …)`; `grainInfo.committedSize` | Signature is `mxlFlowWriterOpenGrain(writer, index, &grainInfo, &payload)`; completion is tracked via `grainInfo.validSlices == totalSlices`, not `committedSize` | Writers set `validSlices = totalSlices` (full-frame commit) and use `MXL_GRAIN_FLAG_INVALID` for gaps |
| `CHx_GRAIN_COUNT` / `CHx_AUDIO_BUFFER_MS` size the rings per flow | Ring depth is **domain-global**: `grainCount = history_duration × rate` from `{domain}/options.json` (`urn:x-mxl:option:history_duration/v1.0`, default 200 ms). The public API cannot set a per-flow depth | The container derives the *requested* history duration from these variables and, when creating a flow yields a different actual depth, logs a structured warning and exports the actual depth via `/statusz` + metrics. It never rewrites a mounted domain's `options.json` (the domain is shared infrastructure per §4.1) |
| `CHx_COMMIT_BATCH_HINT` → `maxCommitBatchSizeHint` | Passed as writer-options JSON `{"maxCommitBatchSizeHint": N}` to `mxlCreateFlowWriter` | Implemented exactly so |
| `mxlFlowSynchronizationGroup` for output alignment | **Does not exist in v1.0.1** (it is post-1.0 roadmap) | Output preroll uses `mxlFlowReaderGetGrain` with timeouts + `mxlGetNsUntilIndex` pacing; the sync-group hook is isolated in `output_channel.cpp` for later adoption |
| Audio sample index semantics | `mxlFlowWriterOpenSamples(writer, index, count, …)` addresses the `count` samples **ending at** `index` (same convention as `mxlFlowReaderGetSamples`) | `audiowriter` computes `index = mxlTimestampToIndex(48000/1, tai_first_sample) + count` |
| ANC flow "per RFC 8331 §2 from the length field onward" | MXL only fixes the container: `format: data`, `media_type: video/smpte291`, 4096-byte grains, 1-byte slices | `anc.cpp` serializes `Length(16) ANC_Count(8) F(2) reserved(22)` followed by 10-bit-packed ANC data packets (C, Line, HO, S, StreamNum, DID, SDID, DC, UDW, CS, word-aligned) — the RFC 8331 payload after the RTP-specific fields |
| Timestamps via `CLOCK_TAI` | `mxlGetTime()` **is** `CLOCK_TAI`-based; all index math uses 128-bit rounding | The container uses `mxlGetTime`/`mxlTimestampToIndex` for all index math so container and readers can never disagree; hardware calibration (§3.5) is an offset applied before index conversion, recalibrated every 60 s with a 1 ms sanity gate |

Everything else follows the spec directly:

- **§3.1 enumeration** — iterate `IDeckLinkIterator`, read `BMDDeckLinkPersistentID` via
  `IDeckLinkProfileAttributes`, group sub-devices per card, resolve `CHx_SUBDEVICE_INDEX`
  within the matched card; name/index fallbacks log reboot-stability warnings.
- **§3.2 modes** — table-driven; `auto` requires input direction + card detection support,
  drives the re-enable sequence on `VideoInputFormatChanged`; explicit modes are verified
  with `DoesSupportVideoMode` before enabling, else the channel fails (not the process).
- **§3.3 pixel formats** — `bmdFormat10BitYUV` default (v210 bit-identical, single
  `memcpy` per frame); `8BitYUV` only with `CHx_ALLOW_FORMAT_CONVERSION=true` (scalar
  expansion loop, unit-tested against reference vectors); RGB rejected at config time;
  `10BitYUVA` → `video/v210a` two-plane grains for fill+key.
- **§3.4 audio** — DeckLink 32-bit (or 16-bit) interleaved 48 kHz →
  deinterleaved float32 into the `mxlMutableWrappedMultiBufferSlice` fragments
  (`int / 2147483648.0f`, resp. `/ 32768.0f`); batch size = frames-per-packet.
- **§3.6/§3.7 resilience** — `bmdFrameHasNoInputSource` ⇒ standby + counter + no commit;
  `SIGNAL_LOSS_TIMEOUT_S` ⇒ stream reset cycle; every channel error is contained in its
  thread with 500 ms → 10 s capped exponential backoff; process exit only for
  card-level/`EX_CONFIG`/profile events.
- **§3.8 format change** — writer released, 5 s reader grace, **new flow UUID**
  (UUIDv5-style derivation from the configured UUID + format signature, so restarts with
  the same detected format produce the same replacement UUID), structured log event +
  `mxl_active_video_flow_id` info metric.
- **§3.9 profiles** — applied once at startup when configured; `IDeckLinkProfileCallback`
  registered; external `ProfileActivated` ⇒ clean stop + **exit code 2**. Cards without
  `IDeckLinkProfileManager` (IP 100G) skip both steps.
- **§3.10 lifecycle** — exact startup order; card-level retries (1 s → 30 s backoff,
  `STARTUP_MAX_RETRIES`) then **exit 75**; config errors **exit 78**; SIGTERM runs the
  staged shutdown bounded by `SHUTDOWN_TIMEOUT_S`, then **exit 143**.
- **§4 configuration** — full schema incl. legacy v1.0 mapping (`DIRECTION`,
  `DECKLINK_DEVICE_ID/_NAME/_INDEX`, `VIDEO_MODE`, `MXL_VIDEO_FLOW_ID`, … → implicit
  `CH0_`), mixing rejected; cross-checks: sub-device exists on card, ≤1 use per
  direction, no `auto` on outputs, flow-UUID uniqueness.
- **§7 ops** — `/livez` (housekeeping heartbeat < 5 s), `/readyz`
  (`MXL_HEALTH_MIN_HEALTHY_CHANNELS` threshold, JSON body on 503), `/statusz` (full
  report), `/metrics` with the complete §7.3 metric set and label conventions.

## 4. The mock backend (test strategy, not spec scope creep)

Real DeckLink hardware cannot exist in CI. The `decklink/` layer therefore has two
implementations of one small internal interface:

- `sdk` — the production backend on the real COM interfaces (compiled always, loaded
  when `libDeckLinkAPI.so` is present).
- `mock` — a deterministic software card enabled with `MXL_DECKLINK_BACKEND=mock`
  (documented in the README as a test facility): N sub-devices, SMPTE bars in v210 with a
  frame counter, 1 kHz −18 dBFS tone, optional ANC test packet, wall-clock-paced
  callbacks, scriptable signal-loss and format-change injection
  (`MOCK_SIGNAL_LOSS_AFTER_FRAMES`, `MOCK_FORMAT_CHANGE_AFTER_FRAMES`), and a playback
  side that consumes scheduled frames at rate and reports completions.

The abstraction is intentionally thin (value types + 3 interfaces) so the hot path stays
"callback → convert → memcpy → commit" with zero extra allocation.

## 5. Containers and deployment

- **Dockerfile** (multi-stage, `ubuntu:24.04`): stage 1 builds MXL v1.0.1 (vcpkg
  manifest, `Linux-GCC-Release` preset) then this application; stage 2 is the slim
  runtime with a non-root user (uid 10001, group `video`), the entrypoint validating
  `/dev/blackmagic` + `${MXL_DOMAIN_PATH}`, and optional Desktop Video installation via
  build-arg (`DECKLINK_LIB_MODE=bundled`) or host bind-mount (`hostmount`).
- **docker-compose.yaml** — single-card example with device mapping, `/dev/shm/mxl`
  bind, health/metrics ports.
- **deploy/** — Deployment with Guaranteed QoS, `generic-device-plugin` ConfigMap
  grouping all `/dev/blackmagic/dv*` nodes per card, hostPath tmpfs domain volume,
  probes per §7.1.

## 6. Verification plan

1. **Unit tests** (no hardware, no MXL domain): config parsing/validation matrix incl.
   legacy mapping and all §4.3 cross-checks; video-mode table; v210 row-bytes math and
   8→10-bit expansion golden vectors; PCM conversion (including wraparound fragment
   split); RFC 8331 serialization round-trip; metrics text rendering; TAI/hardware
   offset calibration logic.
2. **Integration smoke test**: build MXL, run `mxl-decklink` with 1 input + 1 output
   mock channel on one domain, assert via a small `mxl-verify` helper that (a) video
   grains advance at the configured rate with plausible indices, (b) audio samples are
   readable and non-silent, (c) `/readyz` flips to 200, (d) `/metrics` exposes
   non-zero frame counters, (e) SIGTERM exits 0 within the grace period, and
   (f) signal-loss injection degrades `/readyz` without killing the process.
3. **Config-failure tests**: invalid env exits 78 with a structured error line; mixed
   v1.0/v1.1 env exits 78.

Pre-go-live items from §9 (IP 100G enumeration, MXL handle thread-safety confirmation,
empirical sizing) remain hardware tasks and are tracked in the README.
