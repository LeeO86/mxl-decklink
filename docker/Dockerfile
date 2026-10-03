# syntax=docker/dockerfile:1
# Multi-stage build per SPECIFICATION.md §5.1.
#
# Build args:
#   MXL_REF               dmf-mxl/mxl git ref to build against (default v1.1.0)
#   NMOS_CPP_REF          sony/nmos-cpp commit with the MXL transport (BCP-007-03).
#                         Keep in sync with .github/workflows/ci.yaml.
#   DESKTOPVIDEO_DEB_URL  optional URL of the Blackmagic Desktop Video
#                         user-space .deb (bundled mode). When empty, the image
#                         expects libDeckLinkAPI.so bind-mounted from the host
#                         (DECKLINK_LIB_MODE=hostmount). Pass via
#                         --build-arg or the DESKTOPVIDEO_DEB_URL repository
#                         secret / variable in the container workflow.

# ---------------------------------------------------------------------------
# Vue web UI (§7.5) — builds a single-file dist/index.html for embedding.
FROM node:22-bookworm AS webui
WORKDIR /web
COPY web/package.json web/package-lock.json ./
RUN npm ci --no-fund --no-audit
COPY web/ ./
RUN npm run build

# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS build

ARG MXL_REF=v1.1.0
ARG NMOS_CPP_REF=fe303849527394b03bdedc8f161f377fe458bb62

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git pkg-config curl zip unzip tar \
        ca-certificates bison flex autoconf automake libtool \
        libboost-system-dev libboost-thread-dev libboost-chrono-dev \
        libboost-date-time-dev libboost-regex-dev libboost-random-dev \
        libboost-filesystem-dev libcpprest-dev nlohmann-json3-dev \
        libssl-dev libavahi-compat-libdnssd-dev \
    && rm -rf /var/lib/apt/lists/*

# vcpkg for the MXL dependency manifest (spdlog, fmt, stduuid, picojson, ada-url).
RUN git clone https://github.com/microsoft/vcpkg /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
ENV VCPKG_ROOT=/opt/vcpkg

# MXL v1.1.0 (Apache-2.0), installed to /opt/mxl.
RUN git clone --branch "${MXL_REF}" https://github.com/dmf-mxl/mxl /tmp/mxl \
    && cmake -S /tmp/mxl -B /tmp/mxl/build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF -DBUILD_UTILS=OFF -DBUILD_DOCS=OFF \
        -DMXL_ENABLE_FABRICS_OFI=OFF \
        -DCMAKE_INSTALL_PREFIX=/opt/mxl \
    && cmake --build /tmp/mxl/build -j"$(nproc)" \
    && cmake --install /tmp/mxl/build

# Sony nmos-cpp (the MXL transport lives on this commit, not in the Conan package).
RUN git -c init.defaultBranch=main init /opt/nmos-cpp \
    && git -C /opt/nmos-cpp remote add origin https://github.com/sony/nmos-cpp.git \
    && git -C /opt/nmos-cpp fetch --depth 1 origin "${NMOS_CPP_REF}" \
    && git -C /opt/nmos-cpp -c advice.detachedHead=false checkout FETCH_HEAD

# The application. The DeckLink interface headers are vendored in-tree;
# libDeckLinkAPI.so is dlopen'ed at runtime, never linked (§5.1).
COPY . /src
COPY --from=webui /web/dist /src/web/dist
RUN cmake -S /src -B /src/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        "-DCMAKE_PREFIX_PATH=/opt/mxl;/tmp/mxl/build/vcpkg_installed/x64-linux" \
        -DMXL_DECKLINK_BUILD_TESTS=ON \
        -DMXL_DECKLINK_NMOS=ON \
        -DNMOS_CPP_DIR=/opt/nmos-cpp/Development \
        -DMXL_DECKLINK_USE_PREBUILT_WEBUI=ON \
    && cmake --build /src/build -j"$(nproc)" \
    && LD_LIBRARY_PATH=/opt/mxl/lib /src/build/unit-tests

# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

ARG DESKTOPVIDEO_DEB_URL=""

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates curl \
        libcpprest2.10 libboost-chrono1.83.0t64 libboost-thread1.83.0 \
        libssl3t64 libavahi-compat-libdnssd1 \
    && rm -rf /var/lib/apt/lists/*

# Optional bundled Desktop Video user-space package (§5.1 stage 2). The DKMS
# kernel-module part is expected to fail inside the container — the modules
# come from the host. The installed version MUST match the host driver.
# Blackmagic packages are not freely downloadable without credentials, so CI
# only bakes this in when DESKTOPVIDEO_DEB_URL is provided (build-arg / secret).
RUN if [ -n "${DESKTOPVIDEO_DEB_URL}" ]; then \
        curl -fsSL -o /tmp/desktopvideo.deb "${DESKTOPVIDEO_DEB_URL}" \
        && (dpkg -i /tmp/desktopvideo.deb || apt-get -f install -y --no-install-recommends) \
        && rm -f /tmp/desktopvideo.deb; \
    fi

COPY --from=build /opt/mxl/lib/ /usr/local/lib/
COPY --from=build /src/build/mxl-decklink /usr/local/bin/mxl-decklink
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN ldconfig && chmod +x /usr/local/bin/entrypoint.sh

# Non-root (platform uid/gid 1000). Ubuntu 24.04 already has uid/gid 1000
# (user ubuntu). The video group is only so the process can open
# /dev/blackmagic (root:video). No root, no host IPC.
ARG MXL_REVISION=unknown
LABEL org.opencontainers.image.source="https://github.com/LeeO86/mxl-decklink" \
      org.opencontainers.image.licenses="MIT" \
      io.dmf.mxl.revision="${MXL_REVISION}"
RUN if ! getent group 1000 >/dev/null; then groupadd --gid 1000 mxl; fi \
    && if ! getent passwd 1000 >/dev/null; then \
         useradd --uid 1000 --gid 1000 --no-create-home --shell /usr/sbin/nologin mxl; \
       fi \
    && usermod -aG video "$(getent passwd 1000 | cut -d: -f1)"
USER 1000:1000

EXPOSE 8080 3212 3213
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
