#!/usr/bin/env bash
# Container entrypoint (SPECIFICATION.md §5.1): sanity-checks the runtime
# environment, then execs the binary (which performs full ENV validation and
# exits 78/EX_CONFIG on any violation).
set -u

DOMAIN_PATH="${MXL_DOMAIN_PATH:-/dev/shm/mxl}"
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

if [[ ! -d "$DOMAIN_PATH" ]]; then
    echo "{\"level\":\"error\",\"event\":\"entrypoint_check_failed\",\"details\":\"MXL domain path $DOMAIN_PATH does not exist; mount a tmpfs there\"}" >&2
    exit 78
fi

exec /usr/local/bin/mxl-decklink "$@"
