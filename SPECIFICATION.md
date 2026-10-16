# mxl-decklink — Technical Specification

**MXL MediaFunction Container for Blackmagic DeckLink**

| | |
|---|---|
| **Repository** | `github.com/LeeO86/mxl-decklink` |
| **Version** | 1.2 (adds §4.5 configuration file, §7.5 web control interface, §7.6 domain/flow discovery; deployment field notes in §5) |
| **Date** | July 25, 2026 |
| **Status** | Implementation-ready specification — no reference code |
| **Audience** | Broadcast / software engineers familiar with ST 2110, NMOS, containers, and the DeckLink SDK |

---

## 1. Overview, Goals, Non-Goals

This specification defines a Linux container that acts as a **bidirectional Media Function** between any Blackmagic DeckLink card (SDI or IP) and the **Media eXchange Layer (MXL)** of the EBU / Linux Foundation Dynamic Media Facility (DMF) reference architecture.

A container process **exclusively owns one physical DeckLink card** and serves **1..N channels** on it, in any combination of directions. A *channel* is the smallest unit managed by the container: it is either an input channel (`DeckLink → MXL`) or an output channel (`MXL → DeckLink`) and consists of at least one MXL video flow plus one or more audio flows and, optionally, an ancillary-data (ANC) flow. On bidirectional sub-devices (DeckLink IP 100G), the same sub-device index may be configured twice — once as an input channel and once as an output channel — in which case the container opens both `IDeckLinkInput` and `IDeckLinkOutput` on the same `IDeckLink` object.

The degenerate single-channel configuration (one card, one channel, one direction) is fully supported and corresponds to the original v1.0 deployment model; see §4.4 for the backward-compatibility rules.

**Why this approach is viable now.** MXL reached stable version **v1.0** in February 2026 and **v1.1.0** in September 2026 (Apache-2.0; hosted by the Linux Foundation). The v1.0 flow format (v210, v210a, float32 audio, SMPTE 291 ANC) and the 2048-byte `mxlFlowInfo` header stay compatible. v1.1 adds flow synchronization groups and inter-host fabrics. Blackmagic Desktop Video 16.0 (March 2026) ships a DeckLink SDK that unifies SDI cards (Duo 2, Quad 2, 8K Pro) and the natively ST 2110-terminating **DeckLink IP 100G** behind identical `IDeckLinkInput` / `IDeckLinkOutput` interfaces. It is therefore possible to build a container that needs to know **neither an ST 2110 stack nor NMOS**, yet serves both worlds.

**Goals.**

1. **Card agnosticism** — a single binary / image for all DeckLink models, using only standard SDK interfaces.
2. **Zero-copy ingest where possible** — v210 → v210 memcpy into the mmapped grain payload.
3. **Precise TAI timestamping** of grains, based on `CLOCK_TAI` or, where available, hardware reference timestamps.
4. **Declarative configuration exclusively via environment variables** for clean twelve-factor container semantics.
5. **Deployability** with Docker/Compose as well as in Kubernetes with PCIe passthrough.
6. **Multi-channel operation** — one container per physical card, serving up to 16 logical channels (e.g. 8 in + 8 out on a DeckLink IP 100G), with per-channel fault isolation.

**Non-goals.**

- The container implements **no ST 2110 stack of its own**: 2110-20/-30/-40 termination is done by the DeckLink IP 100G in firmware, or by an external gateway.
- **NMOS IS-04/IS-05 is included in the container image.** The image and CI link Sony nmos-cpp. With `NMOS_ENABLE=true` the process is a BCP-007-03 node: each MXL writer is an IS-04 Source/Flow/Sender and each MXL reader is an IS-04 Receiver, both with transport `urn:x-nmos:transport:mxl`. IS-05 `master_enable` starts and stops the MXL read or write; `mxl_domain_id` and `mxl_flow_id` select the domain and flow. The DeckLink IP card's own ST 2110 NMOS node, when present, stays a separate node. A local binary built without `MXL_DECKLINK_NMOS` rejects `NMOS_ENABLE=true`. Each receiver has its own `mxl_domain_id`, so the video and the audio of one output can come from different domains. The last activation of every sender and receiver is restored after a restart (§7.7).
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
- **Control** — the embedded web interface and its REST API (§7.5), the configuration store (§4.5), and MXL domain/flow discovery (§7.6).

All modules share a common **configuration snapshot** structure that is validated at startup (§4). The **global (card-wide) part is immutable for the lifetime of the process**; the **per-channel part can be changed at runtime** through the web interface / REST API with per-channel apply semantics (§7.5.3). Every accepted change is validated against the complete §4.3 rule set before it takes effect.

### 2.3 Data Flow, Input Path (DeckLink → MXL)

The DeckLink card delivers `VideoInputFrameArrived(videoFrame, audioPacket)` on an SDK-internal callback thread. The Streaming module synchronously reads `videoFrame->GetBytes(&src)`, `GetRowBytes()`, and `GetHardwareReferenceTimestamp()` inside that callback. It computes a TAI timestamp — with `MXL_TIMESTAMP_SOURCE=hardware` by offset-calibrating the hardware timestamp against `clock_gettime(CLOCK_TAI)`, otherwise directly from `CLOCK_TAI`. From the timestamp and the `mxlRational grainRate`, the grain index follows (`mxlTimestampToIndex`).

Then the MXL sequence runs: `mxlFlowWriterOpenGrain(inst, writer, grainIndex, &grainInfo, &dst)` returns an mmapped destination buffer in tmpfs. For compatible pixel formats (DeckLink `bmdFormat10BitYUV` ↔ MXL `video/v210`, byte-wise binary-compatible including 128-byte row alignment), a single `memcpy` suffices. `grainInfo.committedSize = grainInfo.grainSize; mxlFlowWriterCommitGrain(...)` finalizes the grain; the futex word in `flow->state.syncCounter` is incremented and wakes waiting readers.

In parallel, `audioPacket->GetBytes(&pcm)` is read. Because DeckLink delivers interleaved PCM (`bmdAudioSampleType32bitInteger`, 48 kHz) and MXL expects `audio/float32` deinterleaved per channel in a `channels` blob, the bridge converts in flight: for each sample, `float32 = int32 / 2147483648.0f`, deinterleaving into an `mxlMutableWrappedMultiBufferSlice`. The slice structure addresses ring-buffer wraparound via two fragments. `mxlFlowWriterOpenSamples` / `CommitSamples` open and finalize the batch.

For ANC data, `IDeckLinkVideoFrameAncillaryPackets` is iterated and written into a `video/smpte291` flow per RFC 8331 §2 (from the length field onward) when the channel has ANC enabled (one grain per field for interlaced modes, §3.2.2). The callback must complete well under the frame interval (at 1080p50: <20 ms) — therefore no blocking operations inside it.

### 2.4 Data Flow, Output Path (MXL → DeckLink)

A preroll thread instantiates `mxlFlowReader` handles (video, audio) and waits with `mxlFlowSynchronizationGroupWaitForDataAt(group, targetTai, timeoutNs)` until the configured number of grains (`CHx_OUTPUT_PREROLL_GRAINS`, default 3) is available. It calls `IDeckLinkOutput::CreateVideoFrame(width, height, rowBytes, bmdFormat10BitYUV, bmdFrameFlagDefault, &frame)` for each preroll grain and copies the MXL payload via `mxlFlowReaderGetGrain` into `frame->GetBytes()`. After preroll it calls `BeginAudioPreroll()`, fills the audio ring buffer via `ScheduleAudioSamples`, then `EndAudioPreroll(); StartScheduledPlayback(0, timeScale, 1.0)`.

At runtime, frame production is driven from `IDeckLinkVideoOutputCallback::ScheduledFrameCompleted(frame, result)`: on `bmdOutputFrameCompleted`, the next grain index is computed, a frame filled, and `ScheduleVideoFrame(...)` called. On `bmdOutputFrameDisplayedLate` or `bmdOutputFrameDropped`, the bridge increments a Prometheus counter and, if the rate exceeds a threshold, performs a preroll reset. Audio is pulled symmetrically from `IDeckLinkAudioOutputCallback::RenderAudioSamples(preroll)` via `mxlFlowReaderGetSamples` + `ScheduleAudioSamples`.

**Re-created flows.** A writer that removes and re-creates a flow under the same id gives it a new data file; MXL then answers a read past the old head (a grain read, a sample read, or the sync-group wait) with `MXL_ERR_FLOW_INVALID`. The callback takes that reader out of the sync group and stops reading it, and the channel's supervisor releases it and opens it again for the same flow id right away, then after 100 ms, 200 ms, 500 ms and every 1 s while the flow is missing. Until then video repeats the last frame and that audio flow is silent, so the SDI output never stops. These repeated frames count in `mxl_decklink_frames_repeated_total{reason="flow_reopen"}`, never towards the late/drop preroll reset. Each re-created flow increments `mxl_decklink_flow_reopens_total` and logs `mxl_flow_recreated` once. A flow missing for more than 1 s makes the channel `degraded` until it is open again. A re-created flow with another format, grain rate, line size or channel count restarts the channel (as a preroll reset does).

**Domains and MXL instances.** Each IS-05 receiver of an output has its own domain (§7.7): the video flow and each audio flow may come from a different domain, that is from a different function. An output opens its readers on MXL instances of its own, one per domain it reads, also for this function's own domain: MXL hands out its cached reader of a flow id per instance while any holder keeps it, so a shared instance could not re-open a re-created flow for one output alone. The sync group lives on a further instance (no readers) and takes the readers of all of them. An output never creates another function's domain directory; a route to one that does not exist yet fails the bring-up, which retries with backoff (§3.7) until it exists.

**Read deadline.** When a frame completes, the card still holds `CHx_OUTPUT_PREROLL_GRAINS` − 1 frames. A read in the completion callback (the sync-group wait and the grain) waits at most `CHx_READER_TIMEOUT_MS` and never past the time the frame being scheduled is shown, less a third of a frame (at least 8 ms). That time comes from the card's own playback clock (`GetScheduledStreamTime`): the frame scheduled as the n-th is shown at n frame durations; whatever delays the completion calls (the card's call latency, queued callbacks, clock drift) is accounted for. Without that clock (a backend that does not report it) the estimate below is used. Each bring-up samples the clock for 1.5 frames and logs `stream_time_granularity` (`whole_frames` or `sub_frame`, `step_us`, `frame_us`, `margin_us`); a clock that steps in whole frames lags the real position by up to a frame, so then the earlier of the clock-based and the estimated deadline counts. The completion time is the time of the call when it came while no completion callback ran; a call that came as the previous callback returned (it was queued behind it) completed one frame after the previous completion. So waits in consecutive callbacks share one deadline, and a run of long waits does not drift past it. The audio callback waits at most until the same deadline: it holds the same lock as the completion callback. A grain that comes later than that is not waited for: the frame is repeated (`source_late`) instead of being shown late.

**Preroll.** The preroll starts `CHx_OUTPUT_PREROLL_GRAINS` + `CHx_READ_OFFSET_GRAINS` grains behind the current time, but no later than the source's newest grain allows (its preroll grains end at the newest grain less the offset): a mirrored source arrives a grain or two late, and a mirror created by the route has no history yet. A grain older than the newest one that the source never wrote (a gap, or before a new mirror's first grain) is passed over at once; one it has not written yet is waited for. The audio starts with the grain of the first frame shown. A new session never repeats the previous session's last frame. A flow without any grain yet (an idle mirror the route woke up; replicating its first grain can take seconds) is first waited for in state `init`, up to 10 s: `preroll_no_grain_yet` (info, `waited_ms`) is logged once after 1 s and `preroll_first_grain` (`waited_ms`) when it comes; only with no grain after 10 s does the bring-up fail (`preroll_no_grain`, warning) and retry. Then the preroll also waits until the source has written its first grain + `CHx_READ_OFFSET_GRAINS` + `CHx_OUTPUT_PREROLL_GRAINS` (bounded by that many frames + 3), so it starts at grains the source has and the read offset is in place from the start, not built up by late repeats. Only when nothing can be shown within `CHx_READER_TIMEOUT_MS` + (preroll + 3) frames does the bring-up fail (`preroll_grain_unavailable` with `grain_index`, `head_index`, `read_offset_grains`) and retry with backoff. Every bring-up logs `read_offset_applied` (`requested`, `effective`).

**Read position.** An output reads grain *n* when frame *n* − 1 has completed, `CHx_READ_OFFSET_GRAINS` (default 0) grains behind the newest grain: set 1–2 for a mirrored source that arrives with fabrics lag, so its grains are there when read. The position follows the source, not only the card's clock:

- *Source later than before.* A grain missed because the source writes it after the read deadline (`source_late`) is read again at the next frame: the read position slips one frame behind. A source whose latency grows (a mirror back from an origin restart with more buffering, a card a few ppm faster than the source) therefore costs one repeated frame per frame of extra latency and keeps playing. Without the slip the read would stay ahead of the source and repeat the last frame for good, with no late card frame to trigger a reset. Late repeats less than 1 s apart form one run: it is logged with `source_late` (warning, `late_frames`, the `action` taken) once it has 3 repeats and with `source_late_ended` (`frames_repeated`, `read_position_slipped_grains`) when no late repeat came for 1 s. The channel is `degraded` while no grain came in time for 1 s (late repeats only); a source that is late now and then stays `healthy`.
- *Source earlier than before.* When the newest grain stays more than `CHx_READ_OFFSET_GRAINS` + 2 grains ahead of the grain just read for 1 s in a row (the latency shrank again, or the card runs slower than the source), the next read skips ahead to one grain behind the newest plus the offset. A burst of completions after an output-side delay reads behind the head for a few frames only and skips nothing. A source that jumped ahead of the read position is followed at once, and its newest grain (less the offset) is shown in the same frame: after a stall, when the source resumed at the current time and left a gap of grains it never wrote (they read as invalid), and when a read is older than the flow's history (`MXL_ERR_OUT_OF_RANGE_TOO_LATE`). When the newest grain is not complete yet (a writer that commits slice by slice), a read out of the history still moves on, to one grain behind the newest. Catch-ups are logged with `read_position_caught_up` (`catch_ups`, `skipped_grains`, `reason` `behind_the_source` or `source_jumped_ahead`; at most one line a second). The preroll is exempt from the 1 s rule; a jump to the source's newest grain can also happen in the preroll (a mirror whose first grain lands then), and the audio starts with the grain of the first frame shown. A grain older than the newest one that did not come within the wait is read once more without waiting before it counts as a gap.
- *Lip sync.* The audio read position moves with the video's: a slip plays one audio block again, a skip of *k* grains skips *k* blocks, so the audio block stays (video grain − (`CHx_OUTPUT_PREROLL_GRAINS` − 1)) × samples per frame. An audio read older than the audio history re-aligns to that position.
- *Offset cap.* At bring-up the offset is capped so that `CHx_OUTPUT_PREROLL_GRAINS` + offset stays inside the readable history: video grain count − 3 (MXL keeps grain count − 2 readable) and, for each audio flow, buffer length / 2 / samples per frame − 1. A capped offset logs `read_offset_capped` with `requested`, `effective`, `video_grain_count` and `audio_buffer_length`. The history is the domain's `history_duration` (200 ms by default: 10 grains at 50 Hz, so at most 4 grains of offset with a preroll of 3).

**Source stalls.** A source can stop writing while its flow stays (a fabrics mirror whose remote origin restarts, a writer that hangs). A read that finds nothing from a source whose head index has not moved for two frame periods (timed across calls, so a live writer that writes a frame's worth at a time is not taken for stalled) is stalled: it leaves the sync group and is read without waiting, video repeats the last frame and the stalled audio flow's DeckLink channels play silence, for as long as the stall lasts. Samples the source wrote before it stopped are still played (the audio read position trails the video). The stall ends as soon as the source's head index moves; the next read waits as usual, and an audio block the source has not written yet is read again, not played as silence. One audio block is scheduled per audio callback, silent where nothing could be read, so the audio stays on the video's TAI grid and is in sync when the source resumes. A live audio source that is only behind the read position (head moving) skips the block as before. A stall never triggers the preroll reset: that counts only frames the card displayed late or dropped, which is output-side trouble. A stall longer than 1 s marks the channel `degraded` until it ends. Repeated frames count in `mxl_decklink_frames_repeated_total{reason}` (`source_stall`, `source_late`, `source_invalid`, `flow_reopen`, `read_error`), not in `mxl_decklink_frames_dropped_total`; each stall counts in `mxl_decklink_source_stalls_total{essence}`. One that lasts 200 ms is logged when it starts (`source_stall`, warning); every stall is logged when it ends (`source_stall_ended`, info, with `stalls`, the last one's `duration_ms` and the `frames_repeated` since the previous line; at most one line a second, except the end of a stall whose start was logged, which is logged at once).

**Other repeats and lost frames.** A grain the source marked invalid (no picture for it, such as a player's gap grain after a freeze) repeats the frame with reason `source_invalid` and is logged at info level (`source_invalid_grain`, `frames_repeated`; at most one line a second). A repeat for another read error is logged as `grain_read_error` (warning, `frames_repeated`, `last` reason; at most one line a second). Frames the card showed late or dropped, and frames that could not be scheduled, are logged as `output_frames_lost` (warning, `displayed_late`, `dropped_by_card`, `not_scheduled`; at most one line a second).

### 2.5 Threading Model

The process structures its threads in **three classes**:

- **Streaming threads (hot path).** DeckLink callbacks (`VideoInputFrameArrived`, `ScheduledFrameCompleted`) run on SDK-private threads anyway; the container adds a dedicated **producer/consumer thread per channel** that mediates between the SDK and the MXL flow handles. All streaming threads receive `SCHED_FIFO` at the priority configured via `MXL_REALTIME_PRIORITY` and are pinned via `pthread_setaffinity_np` to CPUs on the NUMA node of the DeckLink card.
- **Control-plane threads.** HTTP health server, metrics exporter, signal handler, log flusher — standard `SCHED_OTHER`, not pinned (the Kubernetes CPU manager `static` policy governs the cpuset).
- **Housekeeping thread.** Monitors signal lock (`bmdDeckLinkStatusVideoInputSignalLocked`), performs watchdog actions, updates per-channel metrics; standard priority.

Each channel owns **exactly one MXL FlowWriter or FlowReader handle per flow**, and that handle is used **exclusively by that channel's producer/consumer thread** — no handle sharing between threads, so the undocumented handle thread-safety assumptions of MXL are never stressed. All channels share **one `mxlInstance`** per MXL domain for their writers. Each output channel opens its readers on an **`mxlInstance` of its own**: MXL keeps one reader per flow id and instance and returns that same reader while any holder keeps it, so a reader of a re-created flow shared by two channels could not be re-opened (§2.4). Every output channel registers its video reader and active audio readers with an `mxlFlowSynchronizationGroup` and, before each grain read, waits on that grain's TAI timestamp so the video grain and the audio sample at the same time are available together. That group lives on a **private `mxlInstance`** of the readers' domain that holds no readers: MXL removes a released reader from every sync group of the reader's instance, but creates and releases groups without the instance lock, and a group has no lock (dmf-mxl/mxl@218ddaa `lib/internal/src/Instance.cpp`), so a group on the shared instance could be walked by another channel's reader release while it is created or released.

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

**IP-card detection.** The container probes `QueryInterface(IID_IDeckLinkIPExtensions)`; on success it reads the active `IDeckLinkIPFlow` SDP descriptions for logging and health metrics only — the actual 2110 configuration remains with the card (Blackmagic configuration tools / the card's own NMOS node, outside the container). The optional BCP-007-03 node described above controls MXL flows, not the card's RTP senders.

### 3.2 Video Mode Detection and Selection

Two modes are supported per channel.

With `CHx_VIDEO_MODE=auto` AND `BMDDeckLinkSupportsInputFormatDetection == true`, the container enables `bmdVideoInputEnableFormatDetection` in `EnableVideoInput`. The `VideoInputFormatChanged(events, newMode, flags)` callback triggers a re-enable sequence: `PauseStreams(); EnableVideoInput(newMode->GetDisplayMode(), pixelFormatFromFlags, bmdVideoInputEnableFormatDetection); FlushStreams(); StartStreams()`. The corresponding MXL flow is **not** live-reconfigured on a format change — MXL flows are bound to one rate and resolution. Instead the container logs the change, marks grains `MXL_GRAIN_FLAG_INVALID` until the writer is recreated, and restarts the writer/flow definition with the new rate (§3.8). `auto` is only valid for input channels on cards with input format detection.

With an explicit `CHx_VIDEO_MODE` (e.g. `HD1080p50`, `4K2160p50`), the container maps to the corresponding `BMDDisplayMode` enum value (§3.2.1). If the card does not confirm the mode via `DoesSupportVideoMode(...)`, the container refuses to start that channel (channel enters `failed` state; process-level behavior per §7).

#### 3.2.1 Video Mode Mapping (`CHx_VIDEO_MODE` → `BMDDisplayMode`)

The container maintains this mapping internally. Essential entries (non-exhaustive): `HD720p50 → bmdModeHD720p50`, `HD720p5994 → bmdModeHD720p5994`, `HD1080i50 → bmdModeHD1080i50`, `HD1080p24 → bmdModeHD1080p24`, `HD1080p25 → bmdModeHD1080p25`, `HD1080p2997 → bmdModeHD1080p2997`, `HD1080p50 → bmdModeHD1080p50`, `HD1080p5994 → bmdModeHD1080p5994`, `4K2160p25 → bmdMode4K2160p25`, `4K2160p50 → bmdMode4K2160p50`, `4K2160p5994 → bmdMode4K2160p5994`, `4K2160p60 → bmdMode4K2160p60`, `8K4320p50 → bmdMode8K4320p50`, `8K4320p60 → bmdMode8K4320p60`. Modes not in the table are rejected.

#### 3.2.2 Interlaced Modes (`PAL`, `NTSC`, `HD1080i50`, `HD1080i5994`)

The container follows the shared convention of the MXL media functions (MXL `218ddaa`). The flow definition keeps `grain_rate` = the frame rate (25/1 or 30000/1001) and the real `interlace_mode`; MXL then stores **one field per grain** (height / 2 lines) at twice that rate. Grain 2k is the first field in time of TAI frame k (k = `mxlTimestampToIndex` at the frame rate), grain 2k + 1 the second. `interlaced_tff`: the first field is the top field, the even frame lines (0, 2, 4, …); `interlaced_bff`: the bottom field, the odd lines. A reader weaves grain i into the frame lines of parity `(i & 1) ^ bff`. MXL has no per-grain field marker, so a reader cannot detect a writer that puts the first field on an odd index.

- **Field order** comes from the mode table: `NTSC` is lower field first (`bmdLowerFieldFirst` in `IDeckLinkDisplayMode::GetFieldDominance`), so its flow is `interlaced_bff`; `PAL`, `HD1080i50` and `HD1080i5994` are `interlaced_tff`. The MXL flow definition and the NMOS flow and receiver caps carry it.
- **Counters:** `mxl_decklink_grains_committed_total` (and `grains_committed` in `/statusz`) counts MXL grains: two per frame for an interlaced input, one for a progressive input. `mxl_decklink_frames_total` counts frames in both.
- **Input:** k comes from the frame's start, one frame period before its timestamp (§3.5; the frame arrives when it is complete). The bridge writes grain 2k from the first field's lines and grain 2k + 1 from the other lines of the woven DeckLink frame, by row (v210 directly, 8-bit YUV after the expansion of §3.3; for `video/v210a` the fill lines).
- **ANC** of an interlaced input runs at the **field rate**: the `video/smpte291` flow is declared at twice the frame rate (50/1 for 1080i50, 60000/1001 for 1080i59.94). The packets of field 1 go to grain 2k and those of field 2 to grain 2k + 1, the index of the video field, with the RFC 8331 `F` field 0b10 (field 1) or 0b11 (field 2); progressive grains keep `F` = 0b00. When only one field of a frame has packets, the other field gets an empty grain; a frame without ANC packets writes no ANC grain. The field follows from the packet's SMPTE line number: 1080i lines 564–1125, 625-line lines 313–625 and 525-line lines 266–525 and 1–3 are field 2; line 0 (unknown) counts as field 1. The NMOS ANC flow and the ANC receiver caps carry the same field rate as the MXL flow definition (video keeps the frame rate). Progressive ANC stays at the frame rate.
- **Output:** a flow whose `interlace_mode` is `interlaced_tff` or `interlaced_bff` is read as frames: for frame k the channel reads grains 2k and 2k + 1 and weaves them into the DeckLink frame. Every frame count of §2.4 (read position, preroll, read offset, catch-up, slips, the sync-group wait, which waits for the second field's time) is in frames, and the audio per frame is 48000 / frame rate samples (1920 at 25 frames/s). A frame with an invalid field is repeated like an invalid grain (the second field is not waited for when the first is invalid). A flow definition that cannot be read or parsed is read as progressive with the warning `mxl_flow_def_unreadable`. The output does not read ANC.

### 3.3 Pixel Format Mapping

MXL v1.0 supports exactly two video formats: **`video/v210`** (10-bit YUV 4:2:2, Apple/SMPTE v210 layout) and **`video/v210a`** (v210 plus alpha, for fill+key). This is fortunately congruent with the DeckLink standard format `bmdFormat10BitYUV`, which uses the same bit packing (6 pixels in 16 bytes, 128-byte row alignment). The container therefore uses **`bmdFormat10BitYUV` as the default**. `IDeckLinkOutput::RowBytesForPixelFormat` is used to compute row bytes instead of hand-rolled formulas.

All other DeckLink pixel formats (`bmdFormat8BitYUV` UYVY, `bmdFormat10BitRGB` r210, `bmdFormat12BitRGB`, `bmdFormat8BitBGRA`) are incompatible with MXL v1.0 and are rejected with a configuration error, unless `CHx_ALLOW_FORMAT_CONVERSION=true` is set. In that case the container expands 8-bit YUV to v210 by bit expansion (~5% extra CPU at 1080p50, significantly more at UHD — see §8). RGB conversion is explicitly out of scope. For fill+key workflows the container uses `video/v210a` and composes fill (line buffer 1) and key (line buffer 2, 32-bit LE blocks with three 10-bit luma samples each, 4-byte line alignment) in the same grain.

### 3.4 Audio Handling

DeckLink permits `EnableAudioInput/Output` with 2, 8, 16, 32, or 64 channels at a mandatory 48 kHz (`bmdAudioSampleRate48kHz`) and either 16-bit or 32-bit signed integer interleaved. The container uses **32-bit** by default for better conversion accuracy to float32, configurable via `CHx_AUDIO_SAMPLE_TYPE`. The channel count comes from `CHx_AUDIO_CHANNEL_COUNT` (default 16 for broadcast-embedded).

The MXL audio representation is **fundamentally different**: `audio/float32`, **deinterleaved** into `channel_count` separate ring buffers within a single `channels` blob file. The bridge must therefore iterate through the interleaved samples on every `IDeckLinkAudioInputPacket::GetBytes()` and write each channel to its correct ring-buffer offset. `mxlFlowWriterOpenSamples(writer, nextSampleIndex, batchSize, &multiSlice)` returns an `mxlMutableWrappedMultiBufferSlice` with two fragments per channel (for ring-buffer wraparound). One `OpenSamples` or `GetSamples` call is limited to the length reported by `mxlFlowWriterGetMaxWriteLengthSamples` / `mxlFlowReaderGetMaxReadLengthSamples` (half the ring on the POSIX backend). Larger DeckLink packets are split into consecutive batches that each end at their own head index. The `bufferLength` in `mxlContinuousFlowConfigInfo` MUST be at least `2 × maxBatchSize` so readers can always consistently read half their buffer size (SDK requirement).

An `audio/float32` sample index is based on 48000/1 as `sample_rate`; `nextSampleIndex` is derived deterministically from the TAI timestamp of the audio packet so that video and audio remain time-coherent. Sample-rate drift between the DeckLink hardware clock and system TAI is not compensated (negligible in genlocked operation; otherwise a separate resampler container is required).

### 3.5 Timing and Timestamping

**MXL does not require PTP**, but it does require TAI time relative to the SMPTE ST 2059-1 epoch (identical to the PTP epoch, 1970-01-01 TAI). The container reads `clock_gettime(CLOCK_TAI, &ts)` as its base reference. For exact frame times there are two modes.

In mode **`MXL_TIMESTAMP_SOURCE=hardware`**, the container uses `IDeckLinkVideoInputFrame::GetHardwareReferenceTimestamp(1000000000, &t, &d)` (nanosecond timescale), calibrates the offset `delta = clock_gettime(CLOCK_TAI) - hw_timestamp_now` once at startup, and computes `tai_ns = hw_timestamp + delta` for every frame. This minimizes jitter on genlocked SDI cards and, on the DeckLink IP 100G, is coupled to the card's PTP-locked hardware clock. Caution: the offset does not drift-correct automatically; on non-genlocked systems it can diverge over hours. The container implements a rolling recalibration (every 60 s) with deltas below 1 ms, otherwise a warning log.

In mode **`MXL_TIMESTAMP_SOURCE=host`**, the container takes `clock_gettime(CLOCK_TAI)` at callback entry as the frame timestamp — simpler, but subject to callback jitter (typically ±0.5 ms).

The grain index follows from `mxlTimestampToIndex(&grainRate, tai_ns)`; interlaced modes write two field grains from the frame index of the frame's start (§3.2.2). For signal-loss frames (`bmdFrameHasNoInputSource` set), no grain is committed; a gap in the ring buffer is accepted so readers detect the outage via `MXL_GRAIN_FLAG_INVALID` or the missing sample index.

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

Shutdown on SIGTERM or SIGINT, within `SHUTDOWN_TIMEOUT_S`: (1) stop each channel (`StopStreams` / `StopScheduledPlayback`, flush callbacks, disable video and audio, release MXL writers and readers), (2) remove this node's NMOS resources so the registry receives DELETEs, (3) stop the web server, (4) `mxlDestroyInstance`, (5) when `MXL_CLEANUP_ON_EXIT=true`, remove only this function's output domain directory, (6) release DeckLink object refs, (7) exit 143. If that grace period is exceeded, the watchdog exits 143 as well.

---
## 4. Configuration Schema (Environment Variables)

All configuration is via environment variables. **No** configuration files inside the container, with one exception: `/etc/blackmagic/BlackmagicPreferences.xml` may be mounted read-only from the host (card-global settings, including NMOS enable on IP cards).

Multi-channel operation uses **indexed prefixes `CHx_…`** (x = 0..15), one prefix per channel. The **number of active channels** is not declared separately; it is derived implicitly from the presence of `CHx_DIRECTION`. Gaps are allowed (`CH0_…` and `CH3_…` without `CH1_`/`CH2_` is valid) — the indices are configuration labels, not sub-device numbers.

**Rationale — indexed prefixes instead of JSON-in-ENV.** Indexed prefixes are far cleaner to edit in Kubernetes ConfigMaps and Docker Compose, easy to generate with Helm `range` templates, and each variable is individually inspectable in `kubectl edit` or GitOps diff reviews. A JSON blob in a single ENV would be more compact but loses validation granularity and diffability.

### 4.1 Global (Card-Wide) Configuration

| Variable | Type | Default | Required | Description |
|---|---|---|---|---|
| `MXL_DECKLINK_CARD_ID` | Hex string (32-bit) | — | no† | `BMDDeckLinkPersistentID` of the card — stable across reboots/slots. Recommended. Example `0xa1b2c3d4`. |
| `MXL_DECKLINK_CARD_NAME` | String | — | no† | Human-readable name from `GetDisplayName`, e.g. `"DeckLink Duo 2"`. |
| `MXL_DECKLINK_CARD_INDEX` | Integer ≥0 | `0` | no† | Zero-based card index in enumeration order. **Default when no selector is set** (first-deploy friendly; not reboot-stable — prefer `CARD_ID` in production). |
| `MXL_DECKLINK_CARD_PROFILE` | enum | — | no | Card profile applied at startup (SDI cards only): `one-full-duplex`, `two-half-duplex`, `four-half-duplex`, `one-half-duplex`. Ignored on profile-less cards (IP 100G). |
| `MXL_OUTPUT_DOMAIN_DIR` | Filesystem path | `/Volumes/mxl/mxl-decklink` | no | This function's own output domain. Created when missing. `MXL_DOMAIN_PATH` is an alias. |
| `MXL_TIMESTAMP_SOURCE` | enum | `hardware` | no | `hardware` (hardware reference clock) or `host` (`CLOCK_TAI`). |
| `MXL_HUGEPAGE_PATH` | Filesystem path | — | no | Optional HugePages mount used as backing for grain buffers (UHD scale-out; see §6.4). |
| `MXL_CPU_PIN_LIST` | CPU list | — | no | Comma-separated CPU list for pinning; overrides Kubernetes CPU-manager assignment. Use outside Kubernetes only. |
| `MXL_REALTIME_PRIORITY` | Integer 1–99 | `50` | no | `SCHED_FIFO` priority for streaming threads. Requires `RT_SCHED=true`. |
| `RT_SCHED` | bool | `false` | no | Enables `SCHED_FIFO` for streaming threads (requires `CAP_SYS_NICE`). |
| `MXL_PTP_INTERFACE` | Interface name | — | no | Network interface for PTP status correlation (IP cards); informational for health/metrics. |
| `WEB_ENABLE` | bool | `true` | no | Enables the embedded web interface and its mutating REST API (§7.5). Health endpoints and `/metrics` are always served. |
| `WEB_PORT` | Port | `8080` | no | Consolidated HTTP port for the web UI, `/api/…`, `/livez` `/readyz` `/statusz`, and `/metrics` (§7.1). |
| `MXL_HEALTH_MIN_HEALTHY_CHANNELS` | Integer | `1` | no | Readiness threshold; see §7.2. Auto-clamped to `0` when no channels are configured yet. Set to the total configured channel count for strict "all channels up" semantics. |
| `SIGNAL_LOSS_TIMEOUT_S` | Integer | `30` | no | Input channels: window without signal after which a stream reset cycle is triggered. |
| `STARTUP_MAX_RETRIES` | Integer | `10` | no | Retry counter for card-level startup. |
| `SHUTDOWN_TIMEOUT_S` | Integer | `10` | no | Grace period on SIGTERM. A completed SIGTERM exits 143. |
| `LOG_LEVEL` | enum | `info` | no | `trace`/`debug`/`info`/`warn`/`error`. |
| `LOG_FORMAT` | enum | `json` | no | `json` (structured) or `text`. |
| `DECKLINK_LIB_MODE` | enum | `bundled` | no | `bundled` (libDeckLinkAPI.so from image) or `hostmount` (bind-mounted from host); documentation of the chosen pattern, see §5.1. |
| `CONFIG_DIR` | Filesystem path | `/config` | no | Directory for state this process writes. Default file is `$CONFIG_DIR/mxl-decklink.json`. |
| `MXL_CONFIG_FILE` | Filesystem path | `$CONFIG_DIR/mxl-decklink.json` | no | JSON configuration file (§4.5). Overrides the `CONFIG_DIR` default. Env-only. |
| `MXL_DOMAIN_SCAN_PATH` | Filesystem path | `/Volumes/mxl` | no | Parent of domain directories (§7.6), including `mirror-*` siblings. |
| `MXL_OUTPUT_DOMAIN_ID` | UUID | seed or path-stable | no | Id written into `domain_def.json` when the file is created. An existing file with a different id is not overwritten; startup exits 78. |
| `MXL_HISTORY_DURATION_MS` | Integer | — | no | `history_duration` written to `options.json` only when that file does not exist. |
| `MXL_CLEANUP_ON_EXIT` | bool | `false` | no | On SIGTERM, remove this function's own output domain directory. |
| `NMOS_ENABLE` | bool | `false` | no | Serve the BCP-007-03 node. The image is built with nmos-cpp. |
| `NMOS_PORT` | Port | `3212` | no | NMOS HTTP APIs. WebSocket subscriptions listen on `NMOS_PORT+1`. |
| `NMOS_LABEL` | String | card name | no | Node label and device-label prefix. |
| `NMOS_SEED` | String | — | no | UUIDv5 name for node, device, sources, senders, receivers, and the default domain id. |
| `NMOS_TAGS` | JSON | — | no | Object of tag name to array of strings, added to the node and device. |
| `NMOS_REGISTRY_ADDRESS` | String | — | no | Unicast Registration API host. |
| `NMOS_REGISTRY_PORT` | Port | `3210` | no | Registration API port. |
| `NMOS_QUERY_ADDRESS` | String | registry address | no | Query API host used when checking registration. |
| `NMOS_QUERY_PORT` | Port | registry port + 1 | no | Query API port. |
| `NMOS_DNS_SD` | bool | `false` | no | When false, no DNS-SD browse and no mDNS advertisement. |
| `NMOS_RESUME_CONNECTIONS` | bool | `true` | no | Save every accepted IS-05 activation to `state/connections.json` next to the configuration file and apply them again at the next start (§7.7). |
| `NMOS_HOST_ADDRESS` | IP literal | first non-loopback IPv4 | no | Address announced in IS-04 and IS-05. Never a hostname. |

† At most one of `CARD_ID` / `CARD_NAME` / `CARD_INDEX` may be set. If none are set, `CARD_INDEX=0` is assumed. Channels (`CHx_DIRECTION`, …) are **optional at startup** — an empty channel list starts the process and web UI so operators can finish configuration interactively (with `MXL_CONFIG_FILE` mounted). If the DeckLink card cannot be opened and no channels are configured, the process falls back to the mock card so the UI stays reachable; set a real card selector and restart before enabling live channels.

### 4.2 Per-Channel Configuration (Prefix `CHx_`)

| Variable | Type | Default | Required | Description |
|---|---|---|---|---|
| `CHx_DIRECTION` | enum `input`/`output` | — | **yes** — presence activates the channel | Channel direction. Determines whether `IDeckLinkInput` or `IDeckLinkOutput` is opened for this channel. |
| `CHx_SUBDEVICE_INDEX` | Integer 0..7 | — | yes | Zero-based sub-device index within the card. On bidirectional sub-devices (IP 100G), the same index may appear in one input and one output channel. |
| `CHx_VIDEO_MODE` | enum/`auto` | `auto` | no | `auto` (input format detection, input channels on capable cards only) or explicit mode per §3.2.1, e.g. `HD1080p50`, `4K2160p5994`. |
| `CHx_PIXEL_FORMAT` | enum | `10BitYUV` | no | `10BitYUV` (v210, recommended, MXL-native), `10BitYUVA` (v210a fill+key), `8BitYUV` (only with `CHx_ALLOW_FORMAT_CONVERSION`). |
| `CHx_ALLOW_FORMAT_CONVERSION` | bool | `false` | no | Permits 8-bit → 10-bit v210 conversion inside the container. |
| `CHx_VIDEO_ANC_ENABLE` | bool | `false` | no | Creates an additional `video/smpte291` flow for ANC data. |
| `CHx_AUDIO_ENABLE` | bool | `true` | no | If `false`, no MXL audio flows for this channel. |
| `CHx_AUDIO_CHANNEL_COUNT` | enum | `16` | no | DeckLink interleaved PCM width: 2, 8, 16, 32, 64. Card-dependent. This is the routing-matrix source/sink size — not an MXL flow channel count. |
| `CHx_AUDIO_SAMPLE_TYPE` | enum | `32bit` | no | `16bit` or `32bit` (DeckLink side). Always converted to MXL float32. |
| `CHx_MXL_VIDEO_FLOW_ID` | UUID | — | yes | UUID of the video flow. Must be stable across deployments (managed externally, e.g. NMOS registry). On **output** channels the nil UUID (`00000000-0000-0000-0000-000000000000`) means unassigned (channel stays down until a flow is set). |
| `CHx_AFn_FLOW_ID` | UUID | — | yes if audio enabled (`n` contiguous from 0) | UUID of audio flow `n`. Inputs require a non-nil UUID. Outputs may use the nil UUID for an unassigned slot (mapped DeckLink channels stay silent). |
| `CHx_AFn_CHANNEL_COUNT` | Integer 1..64 | — | yes with `FLOW_ID` | MXL `audio/float32` channel count for this flow (any count; not limited to named layouts). |
| `CHx_AFn_MAP` | comma-separated ints | — | yes with `FLOW_ID` | DeckLink channel indices mapped into this flow; length must equal `CHANNEL_COUNT`. Indices are in `0 .. AUDIO_CHANNEL_COUNT-1`. Unmapped DeckLink channels are silence both ways. On **output**, each DeckLink index may appear in at most one map (no mixing). On **input**, the same DeckLink index may fan out to multiple flows. |
| `CHx_AFn_LABEL` | String | `ch<x>-audio<n+1>` | no | Human-readable audio flow label. |
| `CHx_MXL_ANC_FLOW_ID` | UUID | — | yes if ANC enabled | UUID of the ANC flow. |
| `CHx_MXL_VIDEO_FLOW_LABEL` | String | `"<card>-ch<x>-video"` | no | Human-readable `label` in the flow descriptor. |
| `CHx_MXL_GROUP_HINT` | String | Channel label | no | Value for `urn:x-nmos:tag:grouphint/v1.0` — groups V/A/ANC in the NMOS sense. |
| `CHx_MXL_DEVICE_ID` | UUID | — | no | `device_id` in the flow descriptor (for external NMOS binding). |
| `CHx_MXL_SOURCE_ID` | UUID | — | no | `source_id` in the flow descriptor. |
| `CHx_GRAIN_COUNT` | Integer | `12` (HD) / `8` (UHD) | no | Ring-buffer depth in grains for the video flow. |
| `CHx_AUDIO_BUFFER_MS` | Integer | `200` | no | Audio ring-buffer length in ms → `bufferLength = 48 × value`. |
| `CHx_COMMIT_BATCH_HINT` | Integer | `1` (video) / `256` (audio) | no | `maxCommitBatchSizeHint` in the flow options. Higher = fewer futex wakes, more latency. |
| `CHx_OUTPUT_PREROLL_GRAINS` | Integer | `3` | no | Output channels only: grains buffered before `StartScheduledPlayback`. During playback the card holds this many frames less one, which bounds how long a read may wait (§2.4). |
| `CHx_READER_TIMEOUT_MS` | Integer | `50` | no | Output channels: longest wait for a grain or audio block in a DeckLink callback; never longer than the read deadline of §2.4. |
| `CHx_SDI_LEVEL` | enum `A`/`B`/`driver` | `A` | no | Output channels: 3G-SDI SMPTE level set before the output starts (§7.1 SDI output signalling). `A` also selects the single link output for HD modes; `driver` leaves the card's setting. |
| `CHx_READ_OFFSET_GRAINS` | Integer 0..64 | `0` | no | Output channels: read this many grains behind the newest grain (§2.4 read position). 1–2 for mirrored sources with fabrics lag. Adds that many frames of latency. Capped at bring-up to the flows' history (log `read_offset_capped`). |
| `CHx_LABEL` | String | `ch<x>` | no | Human-readable label for logs and metric labels, e.g. `studio-a-cam-1`. |

### 4.3 ENV Validation

All environment variables are validated at startup; invalid values exit with code 78 (EX_CONFIG) and a structured error log entry. Cross-checks include: every `CHx_SUBDEVICE_INDEX` must exist on the matched card; the same sub-device index may appear at most once per direction; `auto` video mode is rejected on output channels; non-nil flow UUIDs must be unique across all channels of the same role (writers vs readers); audio-flow maps must be contiguous from `AF0` and, on outputs, must not mix multiple sources onto one DeckLink channel.

### 4.4 Backward Compatibility with Single-Channel (v1.0) Configuration

If **no** `CHx_…` variables are present, but the legacy v1.0 single-channel variables are set (`DIRECTION`, `DECKLINK_DEVICE_ID`/`_NAME`/`_INDEX`, `VIDEO_MODE`, `MXL_VIDEO_FLOW_ID`, `AF0_FLOW_ID` / `AF0_CHANNEL_COUNT` / `AF0_MAP`, …), the container interprets them as an implicit channel 0 (`CH0_…`), with the legacy device selector resolving to (card, sub-device) automatically. The former single-flow keys `MXL_AUDIO_FLOW_ID` / `MXL_AUDIO_FLOW_LABEL` (and their `CHx_` forms) are **rejected** — use `CHx_AFn_*` instead. If v1.0 and v1.1 variables are **mixed**, the container terminates at startup with a clear error.

### 4.5 Configuration File and Precedence

Pure environment-variable configuration is complete but tedious for interactive setup work (many `CHx_…` blocks per card). The container therefore supports an optional **JSON configuration file** as a second configuration layer, designed for a mounted config volume:

- **Location**: `MXL_CONFIG_FILE` (e.g. `/config/mxl-decklink.json`). Absent variable ⇒ no file layer; the file not existing yet is not an error (it is created on first save from the web interface).
- **Format**: one flat JSON object whose keys are **exactly the §4.1/§4.2 environment-variable names** and whose values are strings, e.g. `{"CH0_DIRECTION": "input", "CH0_VIDEO_MODE": "auto", …}`. This keeps a single schema, a single validator, full GitOps diffability, and a trivial mapping for operators who later want to freeze a web-configured setup into environment variables.
- **Precedence**: **environment > file > built-in default.** An environment variable always wins over the same key in the file; the web interface renders such settings read-only with a "set via environment" marker (§7.5.2).
- **Scope**: the file layer may carry any configuration key. Changes to **per-channel keys** apply at runtime (§7.5.3). Changes to **global keys** (card selection, profile, domain path, ports, RT settings, …) are persisted but only take effect on the next process start; the API response and the web interface flag them as `restart_required`.
- **Validation**: the merged effective configuration (env over file) must pass the complete §4.3 validation. A save request producing an invalid merge is rejected atomically — the file is never left in an invalid state. At startup, an invalid file is a configuration error (exit 78) exactly like invalid environment variables.
- **Legacy interaction**: the file uses v1.1 (`CHx_…`) keys exclusively. Legacy v1.0 environment variables combined with a file that defines channels are a configuration error (§4.4 mixing rule).
- **Persistence semantics**: writes are atomic (temp file + rename) so cooperating tooling never observes a torn file.

---

## 5. Container Specification

### 5.1 Base Image and Build

Recommended base image: **`debian:12-slim`** or **`ubuntu:24.04`**, because the Blackmagic Desktop Video Debian packages can be consumed directly. Alternative: RHEL/Rocky/Alma 9 for RPM-based environments. The glibc/libstdc++ in the container must be compatible with the `libDeckLinkAPI.so` being loaded.

The build is **multi-stage**:

- **Stage 1 (`build`)** installs `build-essential`, `cmake`, `git`, `pkg-config`, `libssl-dev`, the unpacked DeckLink SDK (header-only, from `Blackmagic_DeckLink_SDK_16.0/Linux/include/`), and the MXL toolchain prerequisites (`vcpkg` bootstrap). It clones `dmf-mxl/mxl` branch `MXL_REF` (`release/v1.1`), checks out `MXL_REVISION` (`218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`, which must be on that branch; also the image label `io.dmf.mxl.revision`), builds with the CMake preset `Linux-GCC-Release`, and installs to `/opt/mxl`. It also clones `sony/nmos-cpp` at `NMOS_CPP_REF` and links the BCP-007-03 node. The application is then linked against `mxl::mxl` (via `find_package(mxl CONFIG REQUIRED)`) and the symbols generated from `DeckLinkAPIDispatch.cpp`. Blackmagic's `libDeckLinkAPI.so` is **not** linked at build time — it is loaded at runtime from stage 2. Stage 2 installs the nmos-cpp shared libraries (cpprestsdk, Boost, OpenSSL, Avahi `libdns_sd`).
- **Stage 2 (`runtime`)** installs the **Blackmagic Desktop Video user-space package** (`desktopvideo_16.0.*_amd64.deb`) via `dpkg -i || apt-get -f install -y`; the kernel-module DKMS part may fail — the modules come from the host. Important: the version installed in the container MUST match the host driver version exactly. Alternatively (**recommended — see field note below**): bind-mount the host's `libDeckLinkAPI.so` and skip the apt package in the container. Both patterns are permitted; `DECKLINK_LIB_MODE` (`bundled`/`hostmount`) documents the choice.

**Field note on `bundled` vs `hostmount` (v1.1 deployment experience).** The bundled pattern is fragile in practice: any skew between the library baked into the image and the host kernel driver makes `libDeckLinkAPI.so` fail to communicate with the driver (device enumeration returns nothing or the API refuses to initialize). Unless the image build is strictly pinned to each host's Desktop Video release, **`DECKLINK_LIB_MODE=hostmount` with a read-only bind mount of the host's `libDeckLinkAPI.so` is the recommended default** — it matches the host driver by definition. The host path for that library varies by distro (commonly `/usr/lib/x86_64-linux-gnu/libDeckLinkAPI.so` on Debian/Ubuntu, sometimes `/usr/lib/libDeckLinkAPI.so`); locate it with `find /usr -name 'libDeckLinkAPI.so'` and mount it to `/usr/lib/libDeckLinkAPI.so` in the container. At startup the container logs the loaded DeckLink API version as structured event `decklink_api_version` (via `IDeckLinkAPIInformation`) so mismatches are diagnosable.

The application binary lives at `/usr/local/bin/mxl-decklink`. The container entrypoint validates the ENV, checks for `/dev/blackmagic/` and `${MXL_DOMAIN_PATH}`, and starts the binary. The container runs as a **non-root user** (`uid=1000`, `gid=1000`, plus the host `video` group so `/dev/blackmagic` can be opened), with extra capabilities only when `RT_SCHED=true` demands them.

### 5.2 Required Mounts

- **`/dev/blackmagic/`** via `--device` (Docker) or a device plugin (Kubernetes). Plain bind mounts are discouraged because they do not set cgroup device rules. Device nodes are typically `root:video` mode `0660` — the container must be in the host `video` group (`group_add: [video]` in Compose, or matching `fsGroup` / supplemental groups in Kubernetes) in addition to running as the domain-owner uid/gid.
- **A dedicated host tmpfs for MXL domains**, following the CBC/Radio-Canada [`mxl-hands-on`](https://github.com/cbcrc/mxl-hands-on) preparation pattern: mount a tmpfs at **`/Volumes/mxl`** (via `/etc/fstab`, sized per §6.3), create domain directories under it (e.g. `/Volumes/mxl/mxl`), and bind-mount **the whole `/Volumes/mxl` tree** into the container. `${MXL_DOMAIN_PATH}` points at one domain directory inside that mount; `${MXL_DOMAIN_SCAN_PATH}` (default `/Volumes/mxl` in the Compose example) is the discovery root (§7.6). Mounting the dedicated volume — rather than the host's whole `/dev/shm` — avoids exposing unrelated host shared-memory files while still allowing sibling-domain discovery and domain creation (plain `mkdir` plus `domain_def.json`/`options.json`; the MXL SDK itself only opens existing domain directories). A **single-domain** bind mount (e.g. only `/Volumes/mxl/mxl`) remains valid for locked-down deployments that do not need discovery. Built-in process defaults remain `/dev/shm/mxl` and `/dev/shm` so local/CI runs without fstab prep still work.
- **`/etc/blackmagic/BlackmagicPreferences.xml`** optionally read-only, if IP-card configuration (NMOS enable, multicast addresses) is maintained on the host.
- Optional **HugePages mount** for `MXL_HUGEPAGE_PATH` (see §6.4).

### 5.3 Security Context

The container runs **non-privileged**. Standard capture/playback needs only the device mount, plus optionally `CAP_SYS_NICE` and `CAP_IPC_LOCK` for RT scheduling and `mlock`. `--ipc=host` is not required — MXL uses no SysV IPC, only `mmap()` on a shared filesystem path. A read-only rootfs is possible with a tmpfs for `/tmp` and `/var/log`. Seccomp default profile; AppArmor/SELinux labels are host-specific.

**MXL domain ownership (field note).** MXL flows are plain files in the shared tmpfs; every cooperating media-function container must be able to read **and write** them (the reader side updates access metadata and takes shared locks). All containers sharing a domain must therefore agree on the file ownership — in practice: run every MXL container with the **same uid/gid (or a common group) as the owner of the domain directory**. The stock image user is `uid=1000`/`gid=1000`, which matches the platform and the `mxl-hands-on` domain owner. The `video` group is added only for DeckLink device nodes.

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
- **`hostPath` onto a shared host tmpfs.** Prefer a dedicated MXL tmpfs such as `/Volumes/mxl` (CBC `mxl-hands-on` `/etc/fstab` pattern; size per §6.3) rather than remounting the node's whole `/dev/shm`. Pods mount it as `hostPath: {path: /Volumes/mxl, type: Directory}` (or a single domain subdirectory for locked-down deployments). All cooperating pods require pod affinity **to the same node**. The official MXL example deployment uses this pattern with explicit hostname-bound nodeAffinity. Fits node-local media fabrics until the MXL Fabrics API (RDMA) lands.
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

The container exposes a **single consolidated HTTP port** `WEB_PORT` (default 8080). On that port:

- **`/livez`** returns **200 OK** as long as the process is alive and the housekeeping thread has been active within the last 5 seconds. On deadlock or total failure it times out → Kubernetes kills the pod.
- **`/readyz`** returns **200 OK** exactly when at least `MXL_HEALTH_MIN_HEALTHY_CHANNELS` channels are in state `healthy`. Below the threshold it returns **503 Service Unavailable** with a JSON body listing per-channel state. Default threshold is 1; for critical broadcast setups set it to the total configured channel count — "ready" then strictly means "all channels up".
- **`/statusz`** always returns **200 OK** with a full JSON report: per channel its state, last frame timestamp, signal lock, frame drops (`frames_dropped`) and, for outputs, repeated frames (`frames_repeated`, §2.4), the read offset in effect (`read_offset_grains`, §2.4) and the card's SDI output signalling read at bring-up (`sdi_output`, see below), reconnect counter, MXL grain commit count, and an `audio` summary (enable, DeckLink width, per-AF map/UUID, output buffered audio sample frames when available).
- **`/metrics`** serves Prometheus text format.
- When `WEB_ENABLE=true` (default): the embedded web UI (`/`) and the `/api/…` REST API (§7.5). When `WEB_ENABLE=false`, UI and mutating API are absent; health and metrics remain.

**SDI output signalling.** Before an output starts, `CHx_SDI_LEVEL` (§4.2, default `A`) is applied: `A` sets `bmdDeckLinkConfigSMPTELevelAOutput` and, for HD modes (up to 1080 lines), the single link output (`bmdDeckLinkConfigSDIOutputLinkConfiguration` = `bmdLinkConfigurationSingleLink`): a dual-link configuration sends an HD mode as two 1.5G links, which receivers show as interlaced. `B` clears the Level A flag and leaves the link configuration. `driver` changes nothing. A card that does not support Level A (`BMDDeckLinkSupportsSMPTELevelAOutput` false), or a setting the driver refuses, keeps the driver's setting: log `sdi_level_not_set` (warning) with the reason. Level A set but the single link refused: `sdi_level_partial` (warning). Otherwise `sdi_level_set` (`link_set_to_single`). The output holds the `IDeckLinkConfiguration` interface for the whole playback session and releases it at teardown (the SDK may drop changes when the last reference goes); the settings are not written to the Desktop Video preferences, so they apply to this process's session only. After the output is enabled the values are read back and logged (`sdi_output_config`), so a check on the card shows whether they stuck. For every sub-device `GET /api/card` reports `status.sdi_output`, and `/statusz` reports it per output channel as read back after the setting at its last bring-up (plus `requested_level`): `smpte_level` (`"A"` or `"B"`, from `bmdDeckLinkConfigSMPTELevelAOutput`; 3G-SDI 1080p50 Level B is shown as 1080i50 by some receivers), `supports_smpte_level_a` (`BMDDeckLinkSupportsSMPTELevelAOutput`), `output_1080p_as_psf` (`bmdDeckLinkConfigOutput1080pAsPsF`) and `link_configuration` (`single`, `dual`, `quad`; `bmdDeckLinkConfigSDIOutputLinkConfiguration`), `null` when the card does not report a value. Each output bring-up logs them (`sdi_output_config`). With `CHx_SDI_LEVEL=driver` they are the driver's settings (Desktop Video Setup / preferences).

`HEALTH_PORT` / `METRICS_PORT` from earlier revisions are **removed**; setting them is a configuration error.

Recommended probes: `livenessProbe` initialDelay 30 s, period 10 s, failureThreshold 3, timeout 3 s; `readinessProbe` initialDelay 5 s, period 2 s, failureThreshold 2.

### 7.2 Channel State Model

Each channel is in one of: `init` (0), `healthy` (1), `degraded` (2, e.g. signal loss/standby, PTP unlock, reconnect in progress), `failed` (3, unrecoverable for this channel; reconnect loop continues). Channel states feed `/readyz` per the threshold rule and are exported as the gauge `mxl_decklink_channel_state`.

### 7.3 Metrics

All channel-scoped Prometheus metrics carry the labels `card_id`, `channel_index`, `channel_label`, `direction`, `subdevice_index` (plus `flow_id` where applicable). Core set:

- Counters: `mxl_decklink_frames_total`, `mxl_decklink_frames_dropped_total` (inputs: frames lost; outputs: frames the card dropped or that could not be scheduled, output-side trouble only), `mxl_decklink_frames_late_total` (outputs: frames the card displayed late), `mxl_decklink_frames_repeated_total` (outputs, label `reason`: `source_stall` the source stopped writing, `source_late` its grain came after the read deadline and the read position slipped one frame, `flow_reopen` the flow is being re-opened, `read_error` an invalid grain or another read error; §2.4), `mxl_decklink_source_stalls_total` (outputs, label `essence` `video`/`audio`: a reader's source stopped writing, §2.4), `mxl_decklink_signal_lost_total`, `mxl_decklink_format_changes_total`, `mxl_decklink_reconnect_total`, `mxl_decklink_flow_reopens_total` (output: flows re-created by their writer whose reader was re-opened, §2.4), `mxl_grains_committed_total`, `mxl_grains_written_bytes_total` (inputs: per MXL grain, so two per frame for an interlaced mode, one per frame for a progressive mode; `mxl_decklink_frames_total` counts frames in both cases)
- Gauges: `mxl_decklink_channel_state`, `mxl_decklink_signal_lock` (0/1), `mxl_ptp_locked` (IP cards), `mxl_ringbuffer_headindex`, `mxl_ringbuffer_reader_lag_grains`, `mxl_buffered_video_frames`, `mxl_buffered_audio_samples`, `mxl_flow_writer_active`, `mxl_active_video_flow_id` (info label); `mxl_decklink_read_offset_grains` (outputs: `CHx_READ_OFFSET_GRAINS` in effect after the cap, §2.4)
- Histograms: `mxl_flow_grain_commit_latency_seconds` (buckets 100 µs … 100 ms), `mxl_callback_duration_seconds`

### 7.4 Logging

Structured JSON logging with fields `ts`, `level`, `card_id`, `channel_index`, `channel_label`, `flow_id`, `event`, `details`. All state transitions (signal loss, format change, preroll start, playback start, reset, re-created MXL flows, source stalls, restored IS-05 activations, profile events) are logged. OpenTelemetry traces optional via `OTEL_EXPORTER_OTLP_ENDPOINT`.

### 7.5 Web Control Interface

The container embeds a web interface — a **Vue 3** single-page application built to one HTML file and embedded in the binary (no external assets at runtime) — plus a JSON REST API on the same `WEB_PORT` as health and metrics (§7.1), gated by `WEB_ENABLE` (default true). It is a **control plane for one container/card** — it does not replace NMOS or facility orchestration, and it carries no media data.

#### 7.5.1 Structure

The UI is organized in tabs:

- **Dashboard (index)** — general status: card identity, per-channel state/direction/mode/flow with live counters (frames, drops, reconnects, grain rate), MXL domain in use, process health (uptime, version, MXL SDK version, DeckLink API version when available).
- **Channels** — the full per-channel configuration (§4.2) as forms. The tab **adapts dynamically to the matched card**: it offers exactly the sub-devices the card exposes (2 for a Duo 2, 8 for a Quad 2 / IP 100G, …) with their capture/playback capabilities, and allows adding, editing, and removing channels within those bounds. For output channels, the video/audio flows can be picked directly from the MXL browser (§7.5.4).
- **Card** — everything the DeckLink SDK reports per sub-device: detected input video mode (`bmdDeckLinkStatusDetectedVideoInputMode`), current input/output modes and pixel formats, input-signal and reference lock, busy state, PCIe link width/speed, device temperature, profile information.
- **MXL** — domain and flow browser (§7.6): all domains under `MXL_DOMAIN_SCAN_PATH`, all flows of a selected domain with their descriptors and liveness, plus domain creation. Domain **deletion is not supported** (remove unused domains on the host); the UI states this explicitly.
- **Settings** — the global (card-wide) configuration and the **environment-variable view**: the effective configuration rendered as a copyable `KEY=value` block for operators who want to persist the current setup as environment variables instead of the config file.

#### 7.5.2 Configuration editing rules

Every setting displays its **provenance** (default / file / environment). Settings supplied by an environment variable are **read-only in the UI** and visibly marked ("set via environment variable — unset it to edit here"), implementing the §4.5 precedence. Edits are validated server-side against the full §4.3 rule set; invalid submissions are rejected with a per-field error message and change nothing.

#### 7.5.3 Apply semantics (dynamic reconfiguration)

- **Per-channel changes** (add/edit/remove a channel, including flow assignment) apply **at runtime**: only the affected channel is stopped and restarted with the new configuration (a brief interruption of that channel; all other channels are untouched — the §3.7 isolation domain is reused). This supersedes the "partial live reload" roadmap item of v1.1.
- **Global changes** are persisted to the config file but flagged `restart_required`; the Dashboard shows a persistent banner until the process is restarted.
- Every applied change is written to the config file (§4.5) before it is acted upon, so a crash directly after apply cannot lose the configuration.

#### 7.5.4 Flow-to-output assignment

The MXL tab allows selecting any **video flow** (and matching audio flow) discovered in the attached domain and assigning it to an output channel. The UI derives the required `CHx_VIDEO_MODE` from the flow descriptor (`frame_width`/`frame_height`/`grain_rate`/`interlace_mode`; an interlaced flow's `grain_rate` is its frame rate, so 1080 lines at 25/1 map to `HD1080i50`, 30000/1001 to `HD1080i5994`, 576 lines at 25/1 to `PAL` and 486 lines at 30000/1001 to `NTSC`) and warns when no DeckLink mode of the card matches the flow geometry. The assignment is a normal per-channel configuration change (§7.5.3).

#### 7.5.5 Security

The web interface implements **no authentication** — like the health and metrics endpoints it is intended for protected operations networks. Deployments exposing it beyond that must front it with an authenticating reverse proxy / ingress. When `WEB_ENABLE=false`, the UI and mutating `/api/…` routes are not registered (health and metrics stay on `WEB_PORT`). The API never returns environment-variable **values** of settings that look like credentials (none exist in the current schema, but the rule is future-proofing).

#### 7.5.6 REST API (consumed by the UI, usable for automation)

| Endpoint | Method | Purpose |
|---|---|---|
| `/api/status` | GET | Dashboard summary (process, card, channels, domain). Each channel includes an `audio` object: enable/DeckLink width/sample type, `flows[]` (AF index, map, label, UUID), MXL join (`mxl_active` / `mxl_head_index` from the domain), and for outputs `decklink_buffered_audio_frames` (`IDeckLinkOutput::GetBufferedAudioSampleFrameCount`). |
| `/api/card` | GET | Card + per-sub-device capabilities and live SDK status |
| `/api/config` | GET | All settings with value, provenance, editability, env-var name |
| `/api/config` | PUT | Partial update `{key: value \| null}`; validates, persists, applies; reports `restart_required` |
| `/api/domains` | GET | Scan `MXL_DOMAIN_SCAN_PATH` for domains (§7.6) |
| `/api/domains` | POST | Create a domain (§7.6) |
| `/api/flows?domain=…` | GET | List flows of a domain with descriptor + liveness |

### 7.6 MXL Domain and Flow Discovery

- **Domain scan.** A directory under `MXL_DOMAIN_SCAN_PATH` (recursive, bounded depth) is recognized as an MXL domain when it contains a `domain_def.json` marker (the convention established by the CBC/Radio-Canada `mxl-hands-on` ecosystem: `{id, label, description}`), an MXL `options.json`, or at least one `*.mxl-flow` entry. The scan reports path, label/description/id when available, tmpfs backing (via `mxlIsTmpFs`), flow count, and whether it is the domain this container currently uses.
- **Flow listing.** Flows are enumerated from the domain's `{uuid}.mxl-flow/` directories; each entry reports the flow ID, the parsed `flow_def.json` descriptor (format, media type, label, group hint, geometry/rate or channel count), **liveness** (`mxlIsFlowActive`), and — for discrete flows — the current head index and last write time from the flow's runtime info.
- **Domain creation.** MXL deliberately only *opens* existing domain directories (`mxlCreateInstance` fails on a missing directory). Creating a domain is therefore a container-side filesystem operation exposed via `POST /api/domains`: create the directory (must lie under `MXL_DOMAIN_SCAN_PATH`; must be tmpfs-backed or the response carries a warning), write `domain_def.json` (generated `id`, user-supplied `label`/`description`) for ecosystem discoverability, and optionally write `options.json` with a user-supplied history duration (`urn:x-mxl:option:history_duration/v1.0`). Switching the container to the new domain is a global configuration change (`MXL_DOMAIN_PATH`, `restart_required` per §7.5.3). **Domain deletion is intentionally not offered** — operators remove unused domain directories on the host.

### 7.7 IS-05 Connection Persistence

`NMOS_RESUME_CONNECTIONS` (default `true`): every IS-05 activation the node accepts and applies is saved to `state/connections.json` in the directory of the configuration file (`$CONFIG_DIR/state/connections.json` by default; written atomically and durably: temp file, fsync, rename, fsync of the directory; not part of config export/import). Per sender or receiver id it holds `master_enable`, `mxl_domain_id`, the domain's directory at activation time, `mxl_flow_id`, and `sender_id` (receivers) or `receiver_id` (senders). Log `nmos_connection_saved`; a failed write logs `route_state_write_failed` and changes nothing else.

At the next start, before the channels start, the node applies the saved activations and shows them in IS-05 `/active` and `/staged` and in the IS-04 `subscription` of the receiver (`sender_id`, `active`) or sender (`receiver_id`, `active`), so the outputs come back on their routes without a controller (as mxl-st2110-gateway's `node.resume_connections`):

- Saved routes win over the flow ids of the environment and the config file.
- Receivers keep their own domain each (§2.4). A domain found by id after a fresh scan of `MXL_DOMAIN_SCAN_PATH` is read at its current directory. A domain not found yet (the function that writes it starts later) is read at its saved directory once that exists; a flow not there yet is waited for: the output's bring-up retries with backoff (§3.7). Both log `nmos_connection_restored` with a `waiting` reason.
- A saved activation that the activation checks reject (for example no flow id with `master_enable` true, or a domain neither found nor saved) comes up with `master_enable` false and its flow id kept: log `restored_activation_disabled` with the reason. One for a sender or receiver that the current configuration no longer has is skipped (`restored_activation_skipped`).
- Senders restore `master_enable` only. Their flow ids are this function's own (configuration, or the input's runtime flow).
- A missing file is a first start. An empty or invalid file is renamed to `connections.json.bad` and logged (`route_state_invalid` with `moved_to`): nothing is restored, and the next activation writes a new file instead of overwriting the broken one with a single route.

With `NMOS_RESUME_CONNECTIONS=false` nothing is saved or restored; receivers start from the configuration (unassigned outputs parked until a controller activates them).

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

**Partial live reload — implemented via the web interface (v1.2).** Reconfiguring channel 3 without interrupting channels 1–2 no longer requires a pod restart: per-channel changes through the REST API/web interface (§7.5.3) stop and recreate only the affected channel. Global (card-wide) settings still require a process restart, which the API flags as `restart_required`.

**Web interface exposure.** The embedded web interface (§7.5) is unauthenticated by design (operations-network assumption, like `/metrics`). Exposing `WEB_PORT` beyond a protected network without an authenticating reverse proxy is a deployment error. `WEB_ENABLE=false` removes the UI and mutating API while leaving health and metrics on the same port.

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

**MXL project.** Repository https://github.com/dmf-mxl/mxl (v1.1.0, September 2026). Architecture https://github.com/dmf-mxl/mxl/blob/main/docs/Architecture.md. Timing https://github.com/dmf-mxl/mxl/blob/main/docs/Timing.md. API usage https://github.com/dmf-mxl/mxl/blob/main/docs/Usage.md. Examples https://github.com/dmf-mxl/mxl/tree/main/examples (incl. `kube-example.yaml`, `mxl-gst-testsrc`). GStreamer plugin https://github.com/dmf-mxl/mxl/blob/main/rust/gst-mxl-rs/README.md. CBC/Radio-Canada hands-on https://github.com/cbcrc/mxl-hands-on. Fabrics proxy https://github.com/jonasohland/mxl-fabrics-proxy.

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
