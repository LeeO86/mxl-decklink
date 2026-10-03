#!/usr/bin/env bash
# Container entrypoint (SPECIFICATION.md §5.1): sanity-checks the runtime
# environment, then execs the binary (which performs full ENV validation and
# exits 78/EX_CONFIG on any violation).
set -u

DOMAIN_PATH="${MXL_OUTPUT_DOMAIN_DIR:-${MXL_DOMAIN_PATH:-/Volumes/mxl/mxl-decklink}}"
BACKEND="${MXL_DECKLINK_BACKEND:-sdk}"

if [[ "$BACKEND" != "mock" ]]; then
    if [[ ! -d /dev/blackmagic ]]; then
        echo '{"level":"error","event":"entrypoint_check_failed","details":"/dev/blackmagic not present; map the DeckLink device nodes (--device or device plugin)"}' >&2
        exit 78
    fi
    if ! ldconfig -p 2>/dev/null | grep -q libDeckLinkAPI && [[ ! -e /usr/lib/libDeckLinkAPI.so ]] && [[ ! -e /usr/local/lib/libDeckLinkAPI.so ]]; then
        echo '{"level":"warn","event":"decklink_lib_missing","details":"libDeckLinkAPI.so not found in the image; expecting a host bind-mount (DECKLINK_LIB_MODE=hostmount)"}' >&2
    fi
fi

DOMAIN_PARENT="$(dirname "$DOMAIN_PATH")"
if [[ ! -d "$DOMAIN_PARENT" ]]; then
    echo "{\"level\":\"error\",\"event\":\"entrypoint_check_failed\",\"details\":\"parent of MXL output domain $DOMAIN_PARENT does not exist; mount the MXL root there\"}" >&2
    exit 78
fi

exec /usr/local/bin/mxl-decklink "$@"
