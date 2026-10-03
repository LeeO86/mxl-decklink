# Changelog

## Unreleased

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
