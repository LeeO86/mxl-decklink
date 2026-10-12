# Changelog

## 1.0.3

- No segmentation fault when one output channel releases its MXL readers while another output channel creates or releases its flow synchronization group. Output channels that read this function's own output domain (flow ids from the environment, or an IS-05 route to this function's own domain) share its MXL instance, and each kept its sync group on it. MXL removes a released reader from every sync group of the reader's instance (`Instance::releaseReader`, under the instance mutex), but it creates and releases sync groups without that mutex, and a group has no lock of its own (dmf-mxl/mxl@218ddaa `lib/internal/src/Instance.cpp:145-165`, `439-455`). With two or more such output channels, a channel restart (web UI, IS-05 activation, preroll reset, stop) next to another channel's bring-up or teardown could walk freed memory (SIGSEGV in `FlowSynchronizationGroup::removeReader`); the same race crashed mxl-st2110-gateway 1.0.14. Channels that read another function's domain already had an MXL instance of their own and were not affected. Each output channel's sync group now lives on its own MXL instance of the readers' domain, which holds no readers. This costs one more MXL instance per running output channel (an idle inotify watcher thread and a few file descriptors). New unit test: sync groups are created and released while another thread releases readers of the same domain.
- Built on MXL `release/v1.1` at `218ddaa` (was tag `v1.1.0`, `8f3f1096`), the revision mxl-st2110-gateway 1.0.15 and the other media functions use. It adds the writer fix `b8ec4bb6` (a reused ring slot no longer shows the previous grain's `validSlices`) and the reader check `d66dcf3c` (`Instance::getFlowReader` checks the flow id). The shared-memory format is unchanged. The image label `io.dmf.mxl.revision` is `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`; the Docker build and CI clone the branch and check out that commit. No settings change.

## 1.0.2

- MXL receivers accept domains created after the start. The IS-05 constraints listed `mxl_domain_id` as an `enum` of the domains found at startup (nmos-cpp writes a one-element `enum` when there was exactly one), so a PATCH naming a later domain, such as the output domain of a production deployed afterwards, failed with 400 until the pod restarted. The constraint no longer lists domains; an activation (or an `auto` resolution) that names an unknown domain or flow scans `MXL_DOMAIN_SCAN_PATH` again and is refused only if the domain is still not there. No settings change.

## 1.0.1

- Hardware-timestamp recalibration steps to a delta above the 1 ms gate when the next recalibration is again above the gate in the same direction. Before, every such delta was rejected and the timestamps of a card without PTP lock drifted without bound (5 ms per minute on the lab DeckLink IP 100G; 315 s after six weeks). A single jump is still rejected. New log event `hw_clock_recalibration_stepped`.

- SPECIFICATION §3.10 and the Running section of the README now describe a completed SIGTERM as exit 143, matching the process. `v1.0.0` already behaved that way.

## 1.0.0

Stable platform contract for settings, HTTP APIs, and shutdown. A later breaking change needs 2.0.0.

- Output domain: `MXL_OUTPUT_DOMAIN_DIR` (default `/Volumes/mxl/mxl-decklink`) and `MXL_OUTPUT_DOMAIN_ID`. `MXL_DOMAIN_PATH` remains an alias of the directory. An existing `domain_def.json` with a different id is not overwritten (exit 78).
- `MXL_DOMAIN_SCAN_PATH` defaults to `/Volumes/mxl`.
- `MXL_HISTORY_DURATION_MS` is written to `options.json` only when that file does not already exist.
- `NMOS_SEED` derives node, device, source, sender, receiver, and default domain ids with UUIDv5 (`mxl-decklink/<seed>/<role>` under the DNS namespace). Without a seed, ids stay derived from the DeckLink persistent id.
- `NMOS_TAGS` (JSON object) is added to the node and device. Group hints are unchanged.
- `NMOS_DNS_SD` defaults to false: no DNS-SD browse and no mDNS advertisement (`pri` and `highest_pri` at `INT_MAX`). `NMOS_QUERY_ADDRESS` defaults to the registry address; `NMOS_QUERY_PORT` defaults to the registration port + 1.
- `NMOS_HOST_ADDRESS` is the IP literal announced in IS-04 and IS-05. Default is the first non-loopback IPv4. Hostnames, `0.0.0.0`, and loopback are rejected.
- `/readyz` stays 503 until the node is registered when a registry is configured or DNS-SD is on.
- SIGTERM exits **143** after stopping media, asking nmos-cpp to DELETE registry resources, and, when `MXL_CLEANUP_ON_EXIT=true`, removing only this function's output domain directory.
- State file defaults to `$CONFIG_DIR/mxl-decklink.json` (`CONFIG_DIR` default `/config`). `GET /api/v1/config/export` and `POST /api/v1/config/import` read and replace that file. There are no secrets in the document. IS-05 `master_enable` and flow ids are written there when the environment does not already pin those keys (`CHx_MXL_ACTIVE`, `CHx_AFn_MXL_ACTIVE`).
- Prometheus series use the `mxl_decklink_` prefix.
- The container image runs as uid/gid 1000. The `video` group is added only to open `/dev/blackmagic`.
