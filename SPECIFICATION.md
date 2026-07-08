# mxl-decklink — Technical Specification

**MXL MediaFunction Container for Blackmagic DeckLink**

| | |
|---|---|
| **Repository** | `github.com/LeeO86/mxl-decklink` |
| **Version** | 1.1 (consolidated) |
| **Date** | July 8, 2026 |
| **Status** | Implementation-ready specification — no reference code |
| **Audience** | Broadcast / software engineers familiar with ST 2110, NMOS, containers, and the DeckLink SDK |

---

## 1. Overview, Goals, Non-Goals

This specification defines a Linux container that acts as a **bidirectional Media Function** between any Blackmagic DeckLink card (SDI or IP) and the **Media eXchange Layer (MXL)** of the EBU / Linux Foundation Dynamic Media Facility (DMF) reference architecture.

A container process **exclusively owns one physical DeckLink card** and serves **1..N channels** on it, in any combination of directions. A *channel* is the smallest unit managed by the container: it is either an input channel (`DeckLink → MXL`) or an output channel (`MXL → DeckLink`) and consists of at least one MXL video flow plus one or more audio flows and, optionally, an ancillary-data (ANC) flow. On bidirectional sub-devices (DeckLink IP 100G), the same sub-device index may be configured twice — once as an input channel and once as an output channel — in which case the container opens both `IDeckLinkInput` and `IDeckLinkOutput` on the same `IDeckLink` object.

The degenerate single-channel configuration (one card, one channel, one direction) is fully supported and corresponds to the original v1.0 deployment model; see §4.4 for the backward-compatibility rules.

**Why this approach is viable now.** MXL reached stable version **v1.0** in February 2026 (v1.0.1 in May 2026; Apache-2.0; hosted by the Linux Foundation) with a frozen flow format (v210, v210a, float32 audio, SMPTE 291 ANC) and an `mxlFlowInfo` header with a fixed 2048-byte layout. Blackmagic Desktop Video 16.0 (March 2026) ships a DeckLink SDK that unifies SDI cards (Duo 2, Quad 2, 8K Pro) and the natively ST 2110-terminating **DeckLink IP 100G** behind identical `IDeckLinkInput` / `IDeckLinkOutput` interfaces. It is therefore possible to build a container that needs to know **neither an ST 2110 stack nor NMOS**, yet serves both worlds.

**Goals.**

1. **Card agnosticism** — a single binary / image for all DeckLink models, using only standard SDK interfaces.
2. **Zero-copy ingest where possible** — v210 → v210 memcpy into the mmapped grain payload.
3. **Precise TAI timestamping** of grains, based on `CLOCK_TAI` or, where available, hardware reference timestamps.
4. **Declarative configuration exclusively via environment variables** for clean twelve-factor container semantics.
5. **Deployability** with Docker/Compose as well as in Kubernetes with PCIe passthrough.
6. **Multi-channel operation** — one container per physical card, serving up to 16 logical channels (e.g. 8 in + 8 out on a DeckLink IP 100G), with per-channel fault isolation.

**Non-goals.**

- The container implements **no ST 2110 stack of its own**: 2110-20/-30/-40 termination is done by the DeckLink IP 100G in firmware, or by an external gateway.
- It implements **no NMOS IS-04/IS-05**: registration and connection management are the responsibility of the card (IP models) or external controllers (VideoIPath, Sony NMOS controller, EBU node controller). The container merely publishes the flow identifiers that a controller binds.
- The container performs **no video format conversion** beyond the minimum required for MXL v1.0 compatibility (see §3.3).
- It is **not** a GPU compositing or codec container.
- It does **not** replace the Blackmagic Desktop Video driver, which must be installed on the host.

---

## 2. System Architecture

### 2.1 Process and Container Model

The execution model is "one process per container". The process remains a single **Media Function** in the DMF sense — the function is "DeckLink Card Bridge" — and exposes 1..N logical channels that are initialized, monitored, and terminated together per card. The channels are internal composition of that function, not separate functions.

The card is addressed by its `BMDDeckLinkPersistentID` (not by the volatile enumeration index). At startup the container enumerates all sub-devices via `IDeckLinkIterator`, filters by the configured card ID, and maps channel configurations onto concrete sub-devices.

This model is the result of a deliberate architecture decision (see §10 for the full analysis summary) driven by three independent constraints: (a) the DeckLink SDK permits multi-process access to different sub-devices, but the card-wide or pair-wise **profile management of SDI cards** makes coordinated changes from multiple processes fragile; (b) the **MXL API is explicitly designed for multi-flow processes** (`mxlFlowSynchronizationGroup`; multiple FlowWriters/FlowReaders per instance are the reference pattern in `mxl-gst-testsrc`); (c) Kubernetes device plugins **model atomic card assignments more cleanly than sub-device scattering** across potentially multiple cards.

### 2.2 Component Diagram (textual)

On the host, the proprietary Blackmagic kernel modules `blackmagic` and `blackmagic-io` run (plus network-related card firmware for DeckLink IP models), providing device nodes under `/dev/blackmagic/`. The container mounts these nodes via `--device` (Docker) or a device plugin (Kubernetes). Inside the container, the dynamically loaded `libDeckLinkAPI.so` from the same Desktop Video package runs; its version must match the host kernel driver.

The container consists of four logical modules:

- **Device Manager** — `IDeckLinkIterator`-based enumeration, selection by `BMDDeckLinkPersistentID` or display name, profile activation via `IDeckLinkProfileManager`, channel-to-sub-device mapping.
- **Streaming** — per channel, either an `IDeckLinkInput` callback path (capture) or an `IDeckLinkOutput` scheduling loop (playback).
- **MXL Bridge** — one shared `mxlInstance` per MXL domain, plus per channel a set of `mxlFlowWriter` (or `mxlFlowReader`) handles for video, audio, and optionally ANC.
- **Ops** — Prometheus metrics, HTTP health endpoints, structured JSON logging.

All modules share a common **configuration snapshot** structure that is validated at startup from environment variables (§4); after that, the configuration is read-only.

### 2.3 Data Flow, Input Path (DeckLink → MXL)

The DeckLink card delivers `VideoInputFrameArrived(videoFrame, audioPacket)` on an SDK-internal callback thread. The Streaming module synchronously reads `videoFrame->GetBytes(&src)`, `GetRowBytes()`, and `GetHardwareReferenceTimestamp()` inside that callback. It computes a TAI timestamp — with `MXL_TIMESTAMP_SOURCE=hardware` by offset-calibrating the hardware timestamp against `clock_gettime(CLOCK_TAI)`, otherwise directly from `CLOCK_TAI`. From the timestamp and the `mxlRational grainRate`, the grain index follows (`mxlTimestampToIndex`).

Then the MXL sequence runs: `mxlFlowWriterOpenGrain(inst, writer, grainIndex, &grainInfo, &dst)` returns an mmapped destination buffer in tmpfs. For compatible pixel formats (DeckLink `bmdFormat10BitYUV` ↔ MXL `video/v210`, byte-wise binary-compatible including 128-byte row alignment), a single `memcpy` suffices. `grainInfo.committedSize = grainInfo.grainSize; mxlFlowWriterCommitGrain(...)` finalizes the grain; the futex word in `flow->state.syncCounter` is incremented and wakes waiting readers.

In parallel, `audioPacket->GetBytes(&pcm)` is read. Because DeckLink delivers interleaved PCM (`bmdAudioSampleType32bitInteger`, 48 kHz) and MXL expects `audio/float32` deinterleaved per channel in a `channels` blob, the bridge converts in flight: for each sample, `float32 = int32 / 2147483648.0f`, deinterleaving into an `mxlMutableWrappedMultiBufferSlice`. The slice structure addresses ring-buffer wraparound via two fragments. `mxlFlowWriterOpenSamples` / `CommitSamples` open and finalize the batch.

For ANC data, `IDeckLinkVideoFrameAncillaryPackets` is iterated and written into a `video/smpte291` flow per RFC 8331 §2 (from the length field onward) when the channel has ANC enabled. The callback must complete well under the frame interval (at 1080p50: <20 ms) — therefore no blocking operations inside it.

### 2.4 Data Flow, Output Path (MXL → DeckLink)

A preroll thread instantiates `mxlFlowReader` handles (video, audio) and waits with `mxlFlowSynchronizationGroupWaitForDataAt(group, targetTai, timeoutNs)` until the configured number of grains (`CHx_OUTPUT_PREROLL_GRAINS`, default 3) is available. It calls `IDeckLinkOutput::CreateVideoFrame(width, height, rowBytes, bmdFormat10BitYUV, bmdFrameFlagDefault, &frame)` for each preroll grain and copies the MXL payload via `mxlFlowReaderGetGrain` into `frame->GetBytes()`. After preroll it calls `BeginAudioPreroll()`, fills the audio ring buffer via `ScheduleAudioSamples`, then `EndAudioPreroll(); StartScheduledPlayback(0, timeScale, 1.0)`.

At runtime, frame production is driven from `IDeckLinkVideoOutputCallback::ScheduledFrameCompleted(frame, result)`: on `bmdOutputFrameCompleted`, the next grain index is computed, a frame filled, and `ScheduleVideoFrame(...)` called. On `bmdOutputFrameDisplayedLate` or `bmdOutputFrameDropped`, the bridge increments a Prometheus counter and, if the rate exceeds a threshold, performs a preroll reset. Audio is pulled symmetrically from `IDeckLinkAudioOutputCallback::RenderAudioSamples(preroll)` via `mxlFlowReaderGetSamples` + `ScheduleAudioSamples`.

### 2.5 Threading Model

The process structures its threads in **three classes**:

- **Streaming threads (hot path).** DeckLink callbacks (`VideoInputFrameArrived`, `ScheduledFrameCompleted`) run on SDK-private threads anyway; the container adds a dedicated **producer/consumer thread per channel** that mediates between the SDK and the MXL flow handles. All streaming threads receive `SCHED_FIFO` at the priority configured via `MXL_REALTIME_PRIORITY` and are pinned via `pthread_setaffinity_np` to CPUs on the NUMA node of the DeckLink card.
- **Control-plane threads.** HTTP health server, metrics exporter, signal handler, log flusher — standard `SCHED_OTHER`, not pinned (the Kubernetes CPU manager `static` policy governs the cpuset).
- **Housekeeping thread.** Monitors signal lock (`bmdDeckLinkStatusVideoInputSignalLocked`), performs watchdog actions, updates per-channel metrics; standard priority.

Each channel owns **exactly one MXL FlowWriter or FlowReader handle per flow**, and that handle is used **exclusively by that channel's producer/consumer thread** — no handle sharing between threads, so the undocumented handle thread-safety assumptions of MXL are never stressed. All channels share **one `mxlInstance`** per MXL domain. For output channels that must consume time-aligned (e.g. multiviewer downstream), the `mxlFlowSynchronizationGroup` API can optionally be enabled.

The CPU pinning strategy follows a fixed rule: **two physical cores per HD channel, four to six per UHD channel**, plus two cores for control plane and housekeeping. The pinning order keeps a channel's streaming threads adjacent so L2/L3 cache sharing between its producer and consumer is maximized. If `MXL_CPU_PIN_LIST` is set, it overrides the Kubernetes CPU-manager assignment — sensible only outside Kubernetes.

A **signal thread** handles SIGTERM/SIGINT and initiates graceful shutdown (`StopStreams` / `StopScheduledPlayback` per channel, MXL writer/reader release, `mxlGarbageCollectFlows`).

### 2.6 Latency Budget

Target end-to-end container latency from SDI/2110 input to MXL grain visibility for a reader: **< 1 frame interval** (at 1080p50 or UHDp50: <20 ms). Breakdown: DeckLink DMA into host buffer 1–4 ms depending on card model, callback overhead <100 µs, format copy for 1080p50 v210 (5.5 MB) ~0.9 ms on DDR4-3200, MXL commit futex wake <50 µs, reader wakeup <100 µs. The remainder is margin. The playback direction inherently adds **preroll grains × frame duration** (default 3 × 20 ms = 60 ms at 1080p50) of buffering.

---
## 3. Functional Requirements

### 3.1 Device Enumeration and Selection

At startup the container calls `CreateDeckLinkIteratorInstance()` and iterates all `IDeckLink` objects, where each sub-device is its own instance (Duo 2 → 4, Quad 2 → 8, 8K Pro → 4, DeckLink IP 100G → up to 8 bidirectional sub-devices). From each instance, the stable **`BMDDeckLinkPersistentID`** is read via `QueryInterface(IID_IDeckLinkProfileAttributes)` and compared against `MXL_DECKLINK_CARD_ID`. All sub-devices belonging to the matched card are collected; each configured channel's `CHx_SUBDEVICE_INDEX` is then resolved against this per-card list (zero-based, in enumeration order within the card).

Selection by display name (`GetDisplayName()`) and by raw iterator index are supported as fallbacks for lab use, with a log warning that they are not reboot-stable.

**Multi-process behavior (informative).** The Blackmagic SDK permits different processes to own different sub-devices of the same card simultaneously; a single sub-device is exclusive to one process. Global configuration objects (`IDeckLinkConfiguration`) are shared across processes via the host-side `DesktopVideoHelper.service`. This specification nevertheless mandates single-process-per-card ownership because of the profile semantics described in §3.9.

**IP-card detection.** The container probes `QueryInterface(IID_IDeckLinkIPExtensions)`; on success it reads the active `IDeckLinkIPFlow` SDP descriptions for logging and health metrics only — the actual 2110 configuration remains with the card (Blackmagic configuration tools / NMOS, outside the container).

### 3.2 Video Mode Detection and Selection

Two modes are supported per channel.

With `CHx_VIDEO_MODE=auto` AND `BMDDeckLinkSupportsInputFormatDetection == true`, the container enables `bmdVideoInputEnableFormatDetection` in `EnableVideoInput`. The `VideoInputFormatChanged(events, newMode, flags)` callback triggers a re-enable sequence: `PauseStreams(); EnableVideoInput(newMode->GetDisplayMode(), pixelFormatFromFlags, bmdVideoInputEnableFormatDetection); FlushStreams(); StartStreams()`. The corresponding MXL flow is **not** live-reconfigured on a format change — MXL flows are bound to one rate and resolution. Instead the container logs the change, marks grains `MXL_GRAIN_FLAG_INVALID` until the writer is recreated, and restarts the writer/flow definition with the new rate (§3.8). `auto` is only valid for input channels on cards with input format detection.

With an explicit `CHx_VIDEO_MODE` (e.g. `HD1080p50`, `4K2160p50`), the container maps to the corresponding `BMDDisplayMode` enum value (§3.2.1). If the card does not confirm the mode via `DoesSupportVideoMode(...)`, the container refuses to start that channel (channel enters `failed` state; process-level behavior per §7).

#### 3.2.1 Video Mode Mapping (`CHx_VIDEO_MODE` → `BMDDisplayMode`)

The container maintains this mapping internally. Essential entries (non-exhaustive): `HD720p50 → bmdModeHD720p50`, `HD720p5994 → bmdModeHD720p5994`, `HD1080i50 → bmdModeHD1080i50`, `HD1080p24 → bmdModeHD1080p24`, `HD1080p25 → bmdModeHD1080p25`, `HD1080p2997 → bmdModeHD1080p2997`, `HD1080p50 → bmdModeHD1080p50`, `HD1080p5994 → bmdModeHD1080p5994`, `4K2160p25 → bmdMode4K2160p25`, `4K2160p50 → bmdMode4K2160p50`, `4K2160p5994 → bmdMode4K2160p5994`, `4K2160p60 → bmdMode4K2160p60`, `8K4320p50 → bmdMode8K4320p50`, `8K4320p60 → bmdMode8K4320p60`. Modes not in the table are rejected.

### 3.3 Pixel Format Mapping

MXL v1.0 supports exactly two video formats: **`video/v210`** (10-bit YUV 4:2:2, Apple/SMPTE v210 layout) and **`video/v210a`** (v210 plus alpha, for fill+key). This is fortunately congruent with the DeckLink standard format `bmdFormat10BitYUV`, which uses the same bit packing (6 pixels in 16 bytes, 128-byte row alignment). The container therefore uses **`bmdFormat10BitYUV` as the default**. `IDeckLinkOutput::RowBytesForPixelFormat` is used to compute row bytes instead of hand-rolled formulas.

All other DeckLink pixel formats (`bmdFormat8BitYUV` UYVY, `bmdFormat10BitRGB` r210, `bmdFormat12BitRGB`, `bmdFormat8BitBGRA`) are incompatible with MXL v1.0 and are rejected with a configuration error, unless `CHx_ALLOW_FORMAT_CONVERSION=true` is set. In that case the container expands 8-bit YUV to v210 by bit expansion (~5% extra CPU at 1080p50, significantly more at UHD — see §8). RGB conversion is explicitly out of scope. For fill+key workflows the container uses `video/v210a` and composes fill (line buffer 1) and key (line buffer 2, 32-bit LE blocks with three 10-bit luma samples each, 4-byte line alignment) in the same grain.

### 3.4 Audio Handling

DeckLink permits `EnableAudioInput/Output` with 2, 8, 16, 32, or 64 channels at a mandatory 48 kHz (`bmdAudioSampleRate48kHz`) and either 16-bit or 32-bit signed integer interleaved. The container uses **32-bit** by default for better conversion accuracy to float32, configurable via `CHx_AUDIO_SAMPLE_TYPE`. The channel count comes from `CHx_AUDIO_CHANNEL_COUNT` (default 16 for broadcast-embedded).

The MXL audio representation is **fundamentally different**: `audio/float32`, **deinterleaved** into `channel_count` separate ring buffers within a single `channels` blob file. The bridge must therefore iterate through the interleaved samples on every `IDeckLinkAudioInputPacket::GetBytes()` and write each channel to its correct ring-buffer offset. `mxlFlowWriterOpenSamples(writer, nextSampleIndex, batchSize, &multiSlice)` returns an `mxlMutableWrappedMultiBufferSlice` with two fragments per channel (for ring-buffer wraparound). The `bufferLength` in `mxlContinuousFlowConfigInfo` MUST be at least `2 × maxBatchSize` so readers can always consistently read half their buffer size (SDK requirement).

An `audio/float32` sample index is based on 48000/1 as `sample_rate`; `nextSampleIndex` is derived deterministically from the TAI timestamp of the audio packet so that video and audio remain time-coherent. Sample-rate drift between the DeckLink hardware clock and system TAI is not compensated (negligible in genlocked operation; otherwise a separate resampler container is required).

### 3.5 Timing and Timestamping

**MXL does not require PTP**, but it does require TAI time relative to the SMPTE ST 2059-1 epoch (identical to the PTP epoch, 1970-01-01 TAI). The container reads `clock_gettime(CLOCK_TAI, &ts)` as its base reference. For exact frame times there are two modes.

In mode **`MXL_TIMESTAMP_SOURCE=hardware`**, the container uses `IDeckLinkVideoInputFrame::GetHardwareReferenceTimestamp(1000000000, &t, &d)` (nanosecond timescale), calibrates the offset `delta = clock_gettime(CLOCK_TAI) - hw_timestamp_now` once at startup, and computes `tai_ns = hw_timestamp + delta` for every frame. This minimizes jitter on genlocked SDI cards and, on the DeckLink IP 100G, is coupled to the card's PTP-locked hardware clock. Caution: the offset does not drift-correct automatically; on non-genlocked systems it can diverge over hours. The container implements a rolling recalibration (every 60 s) with deltas below 1 ms, otherwise a warning log.

In mode **`MXL_TIMESTAMP_SOURCE=host`**, the container takes `clock_gettime(CLOCK_TAI)` at callback entry as the frame timestamp — simpler, but subject to callback jitter (typically ±0.5 ms).

The grain index follows from `mxlTimestampToIndex(&grainRate, tai_ns)`. For signal-loss frames (`bmdFrameHasNoInputSource` set), no grain is committed; a gap in the ring buffer is accepted so readers detect the outage via `MXL_GRAIN_FLAG_INVALID` or the missing sample index.

### 3.6 Behavior on Signal Loss

If `bmdFrameHasNoInputSource` is set or `VideoInputFormatChanged` reports an unknown mode, the affected channel enters the **standby state**. The metrics counter `mxl_decklink_signal_lost_total` is incremented, grains are no longer written, and the channel's contribution to readiness is degraded per §7.2 (`/livez` remains healthy — the process runs but the channel is waiting for signal). After `SIGNAL_LOSS_TIMEOUT_S` seconds (default 30) without recovery, the channel additionally executes `StopStreams(); FlushStreams(); StartStreams()` (the Blackmagic-recommended idiom after a signal → no-signal → signal transition) to reset timing discontinuities in `GetStreamTime`.

On the DeckLink IP 100G, `IDeckLinkStatus::GetFlag(bmdDeckLinkStatusReferenceSignalLocked, &locked)` is additionally used as a proxy for PTP lock; loss of PTP lock is a separate hard health downgrade (readiness → degraded, `mxl_ptp_locked=0`).

### 3.7 Fault Isolation Between Channels

Errors on one channel (signal loss, DeckLink `E_FAIL` on `EnableVideoInput`, MXL `OpenGrain` timeout) are caught in that channel's producer/consumer thread. The thread marks its channel `degraded` or `failed` in a process-global channel state table and attempts **exponential-backoff reconnect** (500 ms → 1 s → 2 s → 5 s → 10 s, then constant 10 s). A channel error **never** terminates the process. Only unrecoverable process events (SIGSEGV, OOM, `libDeckLinkAPI` initialization failure) end the process, letting the orchestrator restart the container.

### 3.8 Format Change

On an auto-detect format change, the container interrupts MXL flow continuity for the affected channel. First, the active writer is released via `mxlReleaseFlowWriter` and the flow is marked complete (grace period 5 s so readers can consume the last valid grain). Then a **new flow with a new UUID** is created — MXL v1.0 does not permit changing rate/resolution within a flow. The container communicates the new flow UUID through two channels: as a structured log event (JSON) and via the Prometheus metric `mxl_active_video_flow_id` as an info label. External NMOS/orchestration systems are responsible for rebinding downstream readers; this is deliberately outside the container scope.

### 3.9 Profile Management (SDI Cards)

Because a container **owns the whole card**, it is the **sole owner of the card profile**. On startup it reads `MXL_DECKLINK_CARD_PROFILE`, calls `IDeckLinkProfileManager::SetDefaultProfile` once, and then opens the sub-devices under the chosen profile. The classic conflict ("two containers concurrently changing the profile") is structurally eliminated.

For multi-card nodes and for the case that external tools (Blackmagic Desktop Video Setup, other applications) nevertheless change the profile, the container **still** implements `IDeckLinkProfileCallback::ProfileChanging` / `ProfileActivated` and treats callbacks as critical events: running streams are stopped cleanly, all sub-devices released, and the process exits with exit code 2 ("profile changed externally"). The orchestrator restarts the pod, which re-applies the desired profile. This fail-fast approach is considerably more robust than attempting on-the-fly reconfiguration.

**Profile scope by card family (informative).** Duo 2 and Quad 2 share profiles **pair-wise per two sub-devices**; the 8K Pro shares **card-wide across four sub-devices**. The **DeckLink IP 100G has no half/full-duplex profile** because it shares no SDI connectors; `MXL_DECKLINK_CARD_PROFILE` is ignored for it. The container probes `QueryInterface(IID_IDeckLinkProfileManager)` — if `E_NOINTERFACE` is returned, the card is treated as profile-less.

### 3.10 Startup, Shutdown, Reconnect

Container startup follows a fixed sequence: (1) ENV validation, (2) MXL instance create, (3) DeckLink enumeration and card match, (4) profile activation, (5) per-channel video-mode verification, (6) per-channel MXL FlowWriter/FlowReader creation, (7) per-channel DeckLink enable video/audio, (8) `SetCallback`, (9) `StartStreams` or preroll + `StartScheduledPlayback`. Card-level failures (steps 1–4) follow retry with exponential backoff (initial 1 s, max 30 s) up to `STARTUP_MAX_RETRIES` (default 10), then exit code 75 (EX_TEMPFAIL) — restart is left to the orchestrator. Channel-level failures (steps 5–9) put only that channel into `failed`/reconnect per §3.7. Invalid configuration exits immediately with code 78 (EX_CONFIG) and a structured error log entry.

Shutdown on SIGTERM: (1) per channel `StopStreams()` / `StopScheduledPlayback()`, (2) wait for outstanding callbacks (`FlushStreams`), (3) `DisableVideoInput/Output`, `DisableAudioInput/Output`, (4) `mxlReleaseFlowWriter/Reader`, (5) `mxlDestroyInstance`, (6) release DeckLink object refs, (7) exit 0. Timeout per stage: 5 s (bounded by `SHUTDOWN_TIMEOUT_S`), then force exit 143.

---
## 4. Configuration Schema (Environment Variables)

All configuration is via environment variables. **No** configuration files inside the container, with one exception: `/etc/blackmagic/BlackmagicPreferences.xml` may be mounted read-only from the host (card-global settings, including NMOS enable on IP cards).

Multi-channel operation uses **indexed prefixes `CHx_…`** (x = 0..15), one prefix per channel. The **number of active channels** is not declared separately; it is derived implicitly from the presence of `CHx_DIRECTION`. Gaps are allowed (`CH0_…` and `CH3_…` without `CH1_`/`CH2_` is valid) — the indices are configuration labels, not sub-device numbers.

**Rationale — indexed prefixes instead of JSON-in-ENV.** Indexed prefixes are far cleaner to edit in Kubernetes ConfigMaps and Docker Compose, easy to generate with Helm `range` templates, and each variable is individually inspectable in `kubectl edit` or GitOps diff reviews. A JSON blob in a single ENV would be more compact but loses validation granularity and diffability.

### 4.1 Global (Card-Wide) Configuration

| Variable | Type | Default | Required | Description |
|---|---|---|---|---|
| `MXL_DECKLINK_CARD_ID` | Hex string (32-bit) | — | one of ID/NAME/INDEX | `BMDDeckLinkPersistentID` of the card — stable across reboots/slots. Recommended. Example `0xa1b2c3d4`. |
| `MXL_DECKLINK_CARD_NAME` | String | — | see above | Human-readable name from `GetDisplayName`, e.g. `"DeckLink Duo 2"`. |
| `MXL_DECKLINK_CARD_INDEX` | Integer ≥0 | — | see above | Fallback: zero-based card index in enumeration order. Not reboot-stable; lab use only. |
| `MXL_DECKLINK_CARD_PROFILE` | enum | — | no | Card profile applied at startup (SDI cards only): `one-full-duplex`, `two-half-duplex`, `four-half-duplex`, `one-half-duplex`. Ignored on profile-less cards (IP 100G). |
| `MXL_DOMAIN_PATH` | Filesystem path | `/dev/shm/mxl` | no | Domain directory; must be a tmpfs mount. Expected to be mounted, not created by the container. |
| `MXL_TIMESTAMP_SOURCE` | enum | `hardware` | no | `hardware` (hardware reference clock) or `host` (`CLOCK_TAI`). |
| `MXL_HUGEPAGE_PATH` | Filesystem path | — | no | Optional HugePages mount used as backing for grain buffers (UHD scale-out; see §6.4). |
| `MXL_CPU_PIN_LIST` | CPU list | — | no | Comma-separated CPU list for pinning; overrides Kubernetes CPU-manager assignment. Use outside Kubernetes only. |
| `MXL_REALTIME_PRIORITY` | Integer 1–99 | `50` | no | `SCHED_FIFO` priority for streaming threads. Requires `RT_SCHED=true`. |
| `RT_SCHED` | bool | `false` | no | Enables `SCHED_FIFO` for streaming threads (requires `CAP_SYS_NICE`). |
| `MXL_PTP_INTERFACE` | Interface name | — | no | Network interface for PTP status correlation (IP cards); informational for health/metrics. |
| `HEALTH_PORT` | Port | `9080` | no | HTTP port for `/livez`, `/readyz`, `/statusz`. |
| `METRICS_PORT` | Port | `9090` | no | HTTP port for `/metrics` (Prometheus). |
| `MXL_HEALTH_MIN_HEALTHY_CHANNELS` | Integer | `1` | no | Readiness threshold; see §7.2. Set to the total configured channel count for strict "all channels up" semantics. |
| `SIGNAL_LOSS_TIMEOUT_S` | Integer | `30` | no | Input channels: window without signal after which a stream reset cycle is triggered. |
| `STARTUP_MAX_RETRIES` | Integer | `10` | no | Retry counter for card-level startup. |
| `SHUTDOWN_TIMEOUT_S` | Integer | `10` | no | Grace period on SIGTERM. |
| `LOG_LEVEL` | enum | `info` | no | `trace`/`debug`/`info`/`warn`/`error`. |
| `LOG_FORMAT` | enum | `json` | no | `json` (structured) or `text`. |
| `DECKLINK_LIB_MODE` | enum | `bundled` | no | `bundled` (libDeckLinkAPI.so from image) or `hostmount` (bind-mounted from host); documentation of the chosen pattern, see §5.1. |

### 4.2 Per-Channel Configuration (Prefix `CHx_`)

| Variable | Type | Default | Required | Description |
|---|---|---|---|---|
| `CHx_DIRECTION` | enum `input`/`output` | — | **yes** — presence activates the channel | Channel direction. Determines whether `IDeckLinkInput` or `IDeckLinkOutput` is opened for this channel. |
| `CHx_SUBDEVICE_INDEX` | Integer 0..7 | — | yes | Zero-based sub-device index within the card. On bidirectional sub-devices (IP 100G), the same index may appear in one input and one output channel. |
| `CHx_VIDEO_MODE` | enum/`auto` | `auto` | no | `auto` (input format detection, input channels on capable cards only) or explicit mode per §3.2.1, e.g. `HD1080p50`, `4K2160p5994`. |
| `CHx_PIXEL_FORMAT` | enum | `10BitYUV` | no | `10BitYUV` (v210, recommended, MXL-native), `10BitYUVA` (v210a fill+key), `8BitYUV` (only with `CHx_ALLOW_FORMAT_CONVERSION`). |
| `CHx_ALLOW_FORMAT_CONVERSION` | bool | `false` | no | Permits 8-bit → 10-bit v210 conversion inside the container. |
| `CHx_VIDEO_ANC_ENABLE` | bool | `false` | no | Creates an additional `video/smpte291` flow for ANC data. |
| `CHx_AUDIO_ENABLE` | bool | `true` | no | If `false`, no audio flow for this channel. |
| `CHx_AUDIO_CHANNEL_COUNT` | enum | `16` | no | 2, 8, 16, 32, 64. Card-dependent. |
| `CHx_AUDIO_SAMPLE_TYPE` | enum | `32bit` | no | `16bit` or `32bit` (DeckLink side). Always converted to MXL float32. |
| `CHx_MXL_VIDEO_FLOW_ID` | UUID | — | yes | UUID of the video flow. Must be stable across deployments (managed externally, e.g. NMOS registry). |
| `CHx_MXL_AUDIO_FLOW_ID` | UUID | — | yes if audio enabled | UUID of the audio flow. |
| `CHx_MXL_ANC_FLOW_ID` | UUID | — | yes if ANC enabled | UUID of the ANC flow. |
| `CHx_MXL_VIDEO_FLOW_LABEL` | String | `"<card>-ch<x>-video"` | no | Human-readable `label` in the flow descriptor. |
| `CHx_MXL_AUDIO_FLOW_LABEL` | String | — | no | Same, audio. |
| `CHx_MXL_GROUP_HINT` | String | Channel label | no | Value for `urn:x-nmos:tag:grouphint/v1.0` — groups V/A/ANC in the NMOS sense. |
| `CHx_MXL_DEVICE_ID` | UUID | — | no | `device_id` in the flow descriptor (for external NMOS binding). |
| `CHx_MXL_SOURCE_ID` | UUID | — | no | `source_id` in the flow descriptor. |
| `CHx_GRAIN_COUNT` | Integer | `12` (HD) / `8` (UHD) | no | Ring-buffer depth in grains for the video flow. |
| `CHx_AUDIO_BUFFER_MS` | Integer | `200` | no | Audio ring-buffer length in ms → `bufferLength = 48 × value`. |
| `CHx_COMMIT_BATCH_HINT` | Integer | `1` (video) / `256` (audio) | no | `maxCommitBatchSizeHint` in the flow options. Higher = fewer futex wakes, more latency. |
| `CHx_OUTPUT_PREROLL_GRAINS` | Integer | `3` | no | Output channels only: grains buffered before `StartScheduledPlayback`. |
| `CHx_READER_TIMEOUT_MS` | Integer | `50` | no | Timeout on `mxlFlowReaderGetGrain` (output channels). |
| `CHx_LABEL` | String | `ch<x>` | no | Human-readable label for logs and metric labels, e.g. `studio-a-cam-1`. |

### 4.3 ENV Validation

All environment variables are validated at startup; invalid values exit with code 78 (EX_CONFIG) and a structured error log entry. Cross-checks include: every `CHx_SUBDEVICE_INDEX` must exist on the matched card; the same sub-device index may appear at most once per direction; `auto` video mode is rejected on output channels; flow UUIDs must be unique across all channels of the container.

### 4.4 Backward Compatibility with Single-Channel (v1.0) Configuration

If **no** `CHx_…` variables are present, but the legacy v1.0 single-channel variables are set (`DIRECTION`, `DECKLINK_DEVICE_ID`/`_NAME`/`_INDEX`, `VIDEO_MODE`, `MXL_VIDEO_FLOW_ID`, `MXL_AUDIO_FLOW_ID`, …), the container interprets them as an implicit channel 0 (`CH0_…`), with the legacy device selector resolving to (card, sub-device) automatically. All v1.0 deployments therefore run unchanged. If v1.0 and v1.1 variables are **mixed**, the container terminates at startup with a clear error.

---

## 5. Container Specification

### 5.1 Base Image and Build

Recommended base image: **`debian:12-slim`** or **`ubuntu:24.04`**, because the Blackmagic Desktop Video Debian packages can be consumed directly. Alternative: RHEL/Rocky/Alma 9 for RPM-based environments. The glibc/libstdc++ in the container must be compatible with the `libDeckLinkAPI.so` being loaded.

The build is **multi-stage**:

- **Stage 1 (`build`)** installs `build-essential`, `cmake`, `git`, `pkg-config`, `libssl-dev`, the unpacked DeckLink SDK (header-only, from `Blackmagic_DeckLink_SDK_16.0/Linux/include/`), and the MXL toolchain prerequisites (`vcpkg` bootstrap). It clones `dmf-mxl/mxl` (tag `v1.0.1` or newer), builds with the CMake preset `Linux-GCC-Release`, and installs to `/opt/mxl`. The application is then linked against `mxl::mxl` (via `find_package(mxl CONFIG REQUIRED)`) and the symbols generated from `DeckLinkAPIDispatch.cpp`. Blackmagic's `libDeckLinkAPI.so` is **not** linked at build time — it is loaded at runtime from stage 2.
- **Stage 2 (`runtime`)** installs the **Blackmagic Desktop Video user-space package** (`desktopvideo_16.0.*_amd64.deb`) via `dpkg -i || apt-get -f install -y`; the kernel-module DKMS part may fail — the modules come from the host. Important: the version installed in the container MUST match the host driver version exactly. Alternatively (recommended for compatibility): bind-mount the host's `libDeckLinkAPI.so` and skip the apt package in the container. Both patterns are permitted; `DECKLINK_LIB_MODE` (`bundled`/`hostmount`) documents the choice.

The application binary lives at `/usr/local/bin/mxl-decklink`. The container entrypoint validates the ENV, checks for `/dev/blackmagic/` and `${MXL_DOMAIN_PATH}`, and starts the binary. The container runs as a **non-root user** (`uid=10001`, group `video` with a GID aligned to the host), with extra capabilities only when `RT_SCHED=true` demands them.

### 5.2 Required Mounts

- **`/dev/blackmagic/`** via `--device` (Docker) or a device plugin (Kubernetes). Plain bind mounts are discouraged because they do not set cgroup device rules.
- **`${MXL_DOMAIN_PATH}`** (default `/dev/shm/mxl`) as a tmpfs bind mount from the host (Docker) or `hostPath` / `emptyDir { medium: Memory }` (Kubernetes).
- **`/etc/blackmagic/BlackmagicPreferences.xml`** optionally read-only, if IP-card configuration (NMOS enable, multicast addresses) is maintained on the host.
- Optional **HugePages mount** for `MXL_HUGEPAGE_PATH` (see §6.4).

### 5.3 Security Context

The container runs **non-privileged**. Standard capture/playback needs only the device mount, plus optionally `CAP_SYS_NICE` and `CAP_IPC_LOCK` for RT scheduling and `mlock`. `--ipc=host` is not required — MXL uses no SysV IPC, only `mmap()` on a shared filesystem path. A read-only rootfs is possible with a tmpfs for `/tmp` and `/var/log`. Seccomp default profile; AppArmor/SELinux labels are host-specific.

### 5.4 Host Prerequisites

On every host that runs the container: Blackmagic Desktop Video ≥ 16.0 with kernel modules `blackmagic` and `blackmagic-io` (verify via `lsmod | grep blackmagic`) and the `DesktopVideoHelper.service` running. Card firmware updated via `DesktopVideoUpdateTool`. Kernel ≥ 5.15 recommended, ≥ 6.6 for the newest hardware; **`PREEMPT_RT` strongly recommended for UHD operation**. For the DeckLink IP 100G: 2× 100 GbE with a working PTP grandmaster in the network (and typically a `ptp4l` instance on the host for correlation). Sufficient RAM for tmpfs (see sizing table §6.3). System time disciplined via `chrony` (stratum ≤ 2) with a correct kernel TAI offset (`chronyc tracking` shows `Leap status: Normal`).

---

## 6. Kubernetes Deployment

### 6.1 Device Access

Recommended: **`generic-device-plugin` with one group per physical card that bundles all of the card's sub-device nodes atomically**. The exposed resource is card-type-specific — e.g. `decklink.mxl.dev/ip100g: 1` or `decklink.mxl.dev/quad2: 1` — so deployments can request the required card type explicitly via `resources.limits`. The group entry lists all sub-device nodes of the card (`/dev/blackmagic/dv0`..`dvN`). With two cards of the same type per node, two groups of the same resource are configured; node capacity rises to 2 and Kubernetes can place two pods per node, each receiving one atomic card assignment. **Individual sub-device assignment is explicitly not supported**, because it introduces the multi-card-node mismatch problem (a pod receiving sub-devices from different cards while profiles are grouped per card).

**`hostPath + privileged` is a bring-up fallback only** for nodes without the device plugin; production clusters with Pod Security Standard "baseline" prohibit it anyway.

**DRA (Dynamic Resource Allocation) — outlook.** DRA is GA since Kubernetes 1.34, and 1.36 adds "partitionable devices". A future DRA driver (roadmap item, not part of this version) would publish the card with attributes (firmware, PCI address, NUMA node, serial) as a `ResourceSlice` and allow both "whole card" and "individual channel" as overlapping partitions — at that point single-channel pods become cleanly schedulable for special cases.

### 6.2 Volumes for the MXL Domain

There is no perfect Kubernetes idiom for cross-pod shared memory; three options:

- **Sidecar model (`emptyDir { medium: Memory }`).** Producer and consumer MediaFunctions deployed as a multi-container pod share an emptyDir memory volume (`sizeLimit` per §6.3). Simple, no root privileges, no node-affinity concerns. Downside: coupled lifecycle. Fits fixed chains (ingest → delay → preview), not loosely coupled routing.
- **`hostPath` onto a shared host tmpfs.** The host prepares `/dev/shm/mxl` (size via `mount -o remount,size=…` or boot parameter). Pods mount it as `hostPath: {path: /dev/shm/mxl, type: DirectoryOrCreate}`. All cooperating pods require pod affinity **to the same node**. The official MXL example deployment uses this pattern with explicit hostname-bound nodeAffinity. Fits node-local media fabrics until the MXL Fabrics API (RDMA) lands.
- **PersistentVolume (hostPath) with `accessModes: [ReadWriteMany]`.** Cluster-admin-managed, cleaner than raw hostPath in the pod spec, identical underneath (cf. `dmf-mxl/mxl/examples/kube-example.yaml`).

### 6.3 Resource Sizing per Card

| Scenario | CPU (requests = limits) | RAM (requests = limits) | tmpfs / HugePages | Notes |
|---|---|---|---|---|
| IP 100G, 8× HD 1080p59.94 full duplex (8 in + 8 out) | 20 cores | 8 GiB | 2 GiB tmpfs (Memory medium) | 16 streams × 8 grains × 5.27 MiB ≈ 674 MiB ring; rest is margin |
| IP 100G, 8× UHD 2160p59.94 full duplex (8 in + 8 out) | 48 cores | 16 GiB | 8 GiB HugePages-1Gi | 16 streams × 8 grains × 21.1 MiB ≈ 2.7 GiB ring; HugePages mandatory |
| Quad 2 SDI, 8× HD half duplex (4 in + 4 out) | 12 cores | 4 GiB | 1 GiB tmpfs | typical v1.0-scale operation |
| Duo 2 SDI, 4× HD half duplex (2 in + 2 out) | 6 cores | 2 GiB | 512 MiB tmpfs | minimal operation |
| Any card, 1× HD single channel (v1.0-compatible) | 4 cores | 4 GiB | 256 MiB tmpfs | legacy configuration |

tmpfs math is based on **v210 frame size**: 1080p60 v210 = 5.27 MiB per frame (row stride 5120 B × 1080 rows), 2160p60 v210 = 21.1 MiB. Default ring depth is 8 grains per video flow (12 for HD single-channel). Audio ring buffers are negligible next to video (~1–2 MiB per flow at 500 ms depth, 16 channels, 48 kHz, float32).

**These figures are engineering estimates with ~30% headroom for UHD; empirical measurement on target hardware is a pre-production requirement (§8).**

### 6.4 Guaranteed QoS and Realtime

For live broadcast workloads the pod MUST achieve **Guaranteed QoS**: `requests == limits`, integer CPU values, memory set. Kubelet with `cpuManagerPolicy: static` and `topologyManagerPolicy: single-numa-node` for NUMA alignment between CPU, memory, and the PCIe device. Host kernel cmdline `isolcpus=… nohz_full=… rcu_nocbs=…` on selected cores; kubelet reserves the rest via `reservedSystemCPUs`. RT scheduling in the container is enabled through `securityContext.capabilities.add: ["SYS_NICE", "IPC_LOCK"]` plus `RT_SCHED=true`.

HugePages are **not required for HD-scale MXL operation** (MXL uses tmpfs mmap, no DPDK-style buffers). For 8× UHD full-duplex, a `emptyDir { medium: HugePages-1Gi }` mount plus `MXL_HUGEPAGE_PATH` is mandated by the sizing table to keep TLB pressure bounded.

### 6.5 Manifest Structure (described, not code)

A `Deployment` with `replicas: 1` (no horizontal scaling — hardware-bound), pod spec with a `nodeSelector` on the card label (manual `hardware/decklink=true`, or Node Feature Discovery auto-label `feature.node.kubernetes.io/pci-1edb.present=true` on the Blackmagic PCI vendor ID, ideally refined with `decklink-model=…`), `resources.limits."decklink.mxl.dev/<model>": 1` plus Guaranteed-QoS CPU/memory, the capabilities above, volume mounts for the MXL domain (and optionally the Blackmagic preferences file and HugePages), env from §4, and probes per §7. The associated `Service` exposes only metrics and health ports; the media data path runs over MXL shared memory, not the network.

For redundant paths, create one Deployment per physical path (not `replicas: 2`), so node and card binding remain deterministic.

---

## 7. Operations: Health, Metrics, Logging

### 7.1 Endpoints

The container exposes three HTTP endpoints on `HEALTH_PORT` plus Prometheus text format on `METRICS_PORT`:

- **`/livez`** returns **200 OK** as long as the process is alive and the housekeeping thread has been active within the last 5 seconds. On deadlock or total failure it times out → Kubernetes kills the pod.
- **`/readyz`** returns **200 OK** exactly when at least `MXL_HEALTH_MIN_HEALTHY_CHANNELS` channels are in state `healthy`. Below the threshold it returns **503 Service Unavailable** with a JSON body listing per-channel state. Default threshold is 1; for critical broadcast setups set it to the total configured channel count — "ready" then strictly means "all channels up".
- **`/statusz`** always returns **200 OK** with a full JSON report: per channel its state, last frame timestamp, signal lock, frame drops, reconnect counter, MXL grain commit count.

Recommended probes: `livenessProbe` initialDelay 30 s, period 10 s, failureThreshold 3, timeout 3 s; `readinessProbe` initialDelay 5 s, period 2 s, failureThreshold 2.

### 7.2 Channel State Model

Each channel is in one of: `init` (0), `healthy` (1), `degraded` (2, e.g. signal loss/standby, PTP unlock, reconnect in progress), `failed` (3, unrecoverable for this channel; reconnect loop continues). Channel states feed `/readyz` per the threshold rule and are exported as the gauge `mxl_decklink_channel_state`.

### 7.3 Metrics

All channel-scoped Prometheus metrics carry the labels `card_id`, `channel_index`, `channel_label`, `direction`, `subdevice_index` (plus `flow_id` where applicable). Core set:

- Counters: `mxl_decklink_frames_total`, `mxl_decklink_frames_dropped_total`, `mxl_decklink_frames_late_total`, `mxl_decklink_signal_lost_total`, `mxl_decklink_format_changes_total`, `mxl_decklink_reconnect_total`, `mxl_grains_committed_total`, `mxl_grains_written_bytes_total`
- Gauges: `mxl_decklink_channel_state`, `mxl_decklink_signal_lock` (0/1), `mxl_ptp_locked` (IP cards), `mxl_ringbuffer_headindex`, `mxl_ringbuffer_reader_lag_grains`, `mxl_buffered_video_frames`, `mxl_buffered_audio_samples`, `mxl_flow_writer_active`, `mxl_active_video_flow_id` (info label)
- Histograms: `mxl_flow_grain_commit_latency_seconds` (buckets 100 µs … 100 ms), `mxl_callback_duration_seconds`

### 7.4 Logging

Structured JSON logging with fields `ts`, `level`, `card_id`, `channel_index`, `channel_label`, `flow_id`, `event`, `details`. All state transitions (signal loss, format change, preroll start, playback start, reset, profile events) are logged. OpenTelemetry traces optional via `OTEL_EXPORTER_OTLP_ENDPOINT`.

---
## 8. Non-Functional Requirements

**Latency.** End-to-end container latency from SDI input to MXL grain visibility: **< 20 ms** at 1080p50/UHDp50; with genlocked cards and `hardware` timestamping, < 5 ms on top of the DeckLink DMA time. Playback preroll with defaults: 3 × frame duration (60 ms at 1080p50).

**Throughput.** Reference data rates, uncompressed 10-bit YUV 4:2:2: 1080p50 ~830 Mbps (~104 MB/s), 1080p59.94 ~1.0 Gbps, 2160p50 ~3.3 Gbps (~415 MB/s), 2160p59.94 ~4.0 Gbps, 8Kp50 ~13 Gbps. v210 row bytes: `ceil(width/48) × 128`; frame bytes: `row_bytes × height`. An 8× UHD full-duplex card moves on the order of 21 GB/s of memory bandwidth through the container (ingest + playback copies); process count is irrelevant to this figure — it is bounded by hardware.

**Reliability.** The container tolerates per-channel signal loss without process restart (§3.6/§3.7). MTBF target: > 30 days continuous operation with a stable reference. Recovery after signal return: < 2 s to first valid grain, < 5 s to auto-format-detection convergence.

**Scalability.** One container = one physical card. A DeckLink IP 100G runs as one pod with up to 16 logical channels (8 in + 8 out on 8 bidirectional sub-devices); a Quad 2 half-duplex as one pod with up to 8 channels; an 8K Pro as one pod with the profile fixed at startup; a Duo 2 half-duplex as one pod with up to 4 channels.

**Security.** Non-root user, minimal capabilities, read-only rootfs possible, no network ports beyond health/metrics. Primary attack surface: DeckLink kernel-module bugs (host responsibility) and corruption of the shared-memory domain by other containers on the same tmpfs (contain via UNIX file permissions per domain directory).

---

## 9. Risks and Open Items

**MXL v1.0 maturity.** MXL first appeared as an alpha in June 2025 and reached v1.0 in February 2026 — young by broadcast-software standards. Production references at broadcasters (CBC/Radio-Canada, BBC R&D, SRG SSR, VRT, ARD) are mostly pilots as of late 2026. Grain-header layout, flow-descriptor semantics, and the still-missing Fabrics API (RDMA multi-host) carry risk. *Mitigation:* pin exact versions in the image, run regular compatibility tests against MXL nightly, participate in the EBU/DMF community.

**Format compatibility.** MXL v1.0 supports only v210 (+ v210a alpha), float32 audio, and SMPTE 291 ANC. RGB (graphics insertion), 12-bit (post), and compressed formats are absent. Card configurations delivering RGB require a separate upstream conversion container. Roadmap v1.1+ adds formats.

**Timing accuracy.** In Kubernetes deployments without a dedicated PTP sidecar (`phc2sys` with `CAP_SYS_TIME`), the container's TAI precision depends on host chrony. For broadcast-grade PTP alignment, run a PTP daemon pod per node disciplining the system clock. On the DeckLink IP 100G the card-internal PTP is independent of this clock; a potential offset between card PTP and host `CLOCK_TAI` is tolerated in `hardware` timestamping mode but can cause ~1 ms grain-timestamp error in `host` mode.

**DeckLink IP 100G sub-device enumeration (pre-go-live check).** The exact sub-device structure of the IP 100G is not spelled out verbatim in the public SDK documentation; the assumption of 8 bidirectional sub-devices is well supported by the IP HD reference (which enumerates as two DeckLink devices) and the product description ("up to 8 channels simultaneous capture and playback"), but a live `DeviceList` test or a query to Blackmagic developer support is mandatory before implementation is finalized.

**MXL handle thread-safety (pre-go-live check).** MXL documents no explicit thread-safety guarantee for sharing a handle between threads. The chosen rule "one handle belongs to exactly one thread" is conservatively sufficient, but should be confirmed with the MXL maintainers via an issue during integration.

**Empirical resource sizing (pre-go-live check).** The per-process overhead of `libDeckLinkAPI.so` with 16 active streams is not published by the vendor; the sizing table in §6.3 is built on experience from comparable projects with ~30% UHD headroom and must be validated by measurement on target hardware.

**Kernel-module version skew.** `libDeckLinkAPI.so` in the image can diverge from the host module version; Blackmagic does not explicitly guarantee backward compatibility. *Mitigation:* `DECKLINK_LIB_MODE=hostmount`, or a CI job building images per Desktop Video release.

**hostPath and Pod Security Standards.** Clusters enforcing PSA `restricted` block `hostPath` and `privileged`. The container itself is PSA-`baseline`-compatible with the generic device plugin; the MXL domain, however, still needs `hostPath` for cross-pod sharing — a clean CSI driver does not yet exist productively.

**External NMOS-controller interaction on rolling updates.** A pod restart interrupts all channels of the card; the controller (e.g. VideoIPath) must compensate via NMOS unregister/register events so routes are not left pointing at dead endpoints. This must be documented operationally.

**Partial live reload (roadmap).** Reconfiguring channel 3 without interrupting channels 1–2 currently requires a pod restart (all channels). A control-plane interface for per-channel live reload (SIGHUP with ENV re-parse, or a UNIX-socket config protocol) is a desirable v-next feature, deliberately excluded here for complexity.

**Multi-node sharing (roadmap).** The MXL Fabrics API (RDMA, libfabric-based) is not part of v1.0. Multi-node deployments must wait or evaluate a sidecar proxy (e.g. `jonasohland/mxl-fabrics-proxy`). Outside container scope, but relevant to deployment architecture.

**DRA driver for DeckLink (roadmap).** Technically the superior device-assignment solution from Kubernetes 1.36 ("partitionable devices"), enabling clean single-channel pods as an alternative deployment mode. Roadmap candidate.

---

## 10. Architecture Decision Record: Multi-Channel vs. Single-Channel (Summary)

Two variants were analyzed in depth before fixing the model in §2.1.

**Variant A — single-channel containers** (one container per sub-device and direction; up to 16 pods per IP 100G). *Pros:* minimal blast radius (a crash takes down exactly one channel), per-channel rolling updates, trivially flat ENV schema, trivially per-channel observability. *Cons:* SDK overhead multiplies per process (estimated 500–800 MB and 20–40 extra threads across 16 pods), the SDI profile-conflict problem is aggravated rather than solved (any pod changing the profile disturbs all peers without Kubernetes noticing), standard device-plugin semantics cannot express "these sub-devices belong to card X" (mismatch risk on multi-card nodes), NUMA placement of independent pods is not guaranteed, NMOS representation fragments into 16 nodes per card.

**Variant B — multi-channel container** (one process per card). *Pros:* SDK overhead once per card, enforced NUMA locality for all channels, trivial profile ownership, ideal atomic Kubernetes card assignment, one NMOS node per card matching the physical model. Industry practice agrees: CasparCG drives N SDI channels in one instance, GStreamer documents multi-element pipelines per process, AJA's NTV2 exposes all frame stores of a card to one application, Intel MTL holds many sessions per process, and MXL's own reference tools (`mxl-gst-testsrc`) write multiple flows from one process. *Cons:* larger crash blast radius (mitigated by in-process channel isolation, §3.7 — but SIGSEGV/OOM remain process-wide), a more complex indexed ENV schema, card-granular rolling updates, and a 30–60-thread process that demands the disciplined pinning strategy of §2.5.

**Decision:** hybrid with **multi-channel per card as the default** and the single-channel configuration retained as a fully backward-compatible degenerate case (§4.4). The decision follows from three independent constraints: the DeckLink profile model wants a single owner per card; the MXL API rewards multi-flow processes with a dedicated synchronization API; Kubernetes device plugins model card assignment more cleanly than sub-device scattering. The "one process per container" principle is preserved because the process implements one Media Function — "DeckLink card X ↔ MXL domain" — whose channels are internal composition.

---

## 11. References

**MXL project.** Repository https://github.com/dmf-mxl/mxl (v1.0.1, May 2026). Architecture https://github.com/dmf-mxl/mxl/blob/main/docs/Architecture.md. Timing https://github.com/dmf-mxl/mxl/blob/main/docs/Timing.md. API usage https://github.com/dmf-mxl/mxl/blob/main/docs/Usage.md. Examples https://github.com/dmf-mxl/mxl/tree/main/examples (incl. `kube-example.yaml`, `mxl-gst-testsrc`). GStreamer plugin https://github.com/dmf-mxl/mxl/blob/main/rust/gst-mxl-rs/README.md. CBC/Radio-Canada hands-on https://github.com/cbcrc/mxl-hands-on. Fabrics proxy https://github.com/jonasohland/mxl-fabrics-proxy.

**EBU / DMF.** Landing page https://tech.ebu.ch/dmf/mxl. DMF reference-architecture white paper https://tech.ebu.ch/publications. Linux Foundation announcement https://www.linuxfoundation.org/press/linux-foundation-announces-intent-to-form-the-media-exchange-layer-project. TAG Video Systems MXL Technical Guide (April 2026) https://tagvs.com/wp-content/uploads/2026/04/MXL-Technical-Guide.pdf. Broadcast Bridge overview https://www.thebroadcastbridge.com/content/entry/21669/broadcast-standards-introducing-mxl-the-media-exchange-layer.

**Blackmagic DeckLink SDK.** SDK manual (March 2026) https://documents.blackmagicdesign.com/UserManuals/DeckLinkSDKManual.pdf. Online SDK docs https://sdk-doc.blackmagicdesign.com/decklink-sdk/. Developer FAQ https://www.blackmagicdesign.com/developer/support/faq/desktop-video-developer-support-faqs. SDK migration guide https://documents.blackmagicdesign.com/UserManuals/DeckLinkSDKMigrationGuide.pdf. Developer portal https://www.blackmagicdesign.com/developer/products/capture-and-playback/sdk-and-software.

**DeckLink hardware.** DeckLink IP product page https://www.blackmagicdesign.com/products/decklinkip and tech specs https://www.blackmagicdesign.com/products/decklinkip/techspecs. Model overview https://www.blackmagicdesign.com/products/decklink/models. Softron connector mapping Duo 2 / Quad 2 / 8K Pro https://softron.zendesk.com/hc/en-us/articles/114093989034.

**Container ecosystem.** squat/generic-device-plugin https://github.com/squat/generic-device-plugin. Kubernetes docs: device plugins https://kubernetes.io/docs/concepts/extend-kubernetes/compute-storage-net/device-plugins/, CPU management policies https://kubernetes.io/docs/tasks/administer-cluster/cpu-management-policies/, HugePages https://kubernetes.io/docs/tasks/manage-hugepages/scheduling-hugepages/, Topology Manager https://kubernetes.io/docs/tasks/administer-cluster/topology-manager/, DRA https://kubernetes.io/blog/2025/09/01/kubernetes-v1-34-dra-updates/. Community DeckLink containers https://github.com/trickkiste/type1tv-docker-decklink, https://github.com/cHunter789/ffmpeg-decklink. NETINT DeckLink setup guide https://docs.netint.com/bitstreams/installation/blackmagic-setup/. C3VOC DeckLink debugger https://github.com/voc/decklink-debugger. CESNET UltraGrid DeckLink setup https://github.com/CESNET/UltraGrid/wiki/DeckLink-Setup.

**Comparable / ecosystem projects.** Intel Media Transport Library https://github.com/OpenVisualCloud/Media-Transport-Library. Intel Tiber Broadcast Suite https://hub.docker.com/r/intel/intel-tiber-broadcast-suite. Sony nmos-cpp https://github.com/sony/nmos-cpp, Easy-NMOS https://github.com/rhastie/easy-nmos. CasparCG server https://github.com/CasparCG/server. GStreamer decklink elements https://gstreamer.freedesktop.org/documentation/decklink/. AJA NTV2 SDK https://sdkdocs.aja.com/public/ntv2/.

---

## 12. Summary

`mxl-decklink` fills a real gap in the 2026 broadcast ecosystem: a deployable building block that connects SDI-era DeckLink hardware and modern ST 2110 / DeckLink IP hardware to the new MXL data plane **under a single API contract**. The key lies in a small number of deliberate constraints — one container exclusively owns one physical card; channels are internal composition of one Media Function; only MXL-native pixel formats — and in the strict separation of data path (in the container) and control plane (NMOS/orchestration outside).

The technical risks are manageable and mostly maturity-driven: MXL v1.0 is stable in API and wire format but young in production references; DeckLink container patterns are community-proven but without vendor support commitments; Kubernetes PCIe passthrough works but requires pragmatic Pod-Security compromises. Three items are marked as mandatory pre-go-live checks: the IP 100G sub-device enumeration, MXL handle thread-safety confirmation, and empirical resource sizing.

The most important next step after this specification is a **prototype against a real DeckLink Duo 2** (SDI) and, when available, a DeckLink IP 100G, so that the timing and compatibility assumptions flagged in §9 can be verified empirically before production deployment investment. The implementation should be published as an open-source contribution to the DMF community so it becomes part of the ecosystem rather than an isolated in-house solution.
