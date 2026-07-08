# syntax=docker/dockerfile:1
# Multi-stage build per SPECIFICATION.md §5.1.
#
# Build args:
#   MXL_REF               dmf-mxl/mxl git ref to build against (default v1.0.1)
#   DESKTOPVIDEO_DEB_URL  optional URL of the Blackmagic Desktop Video
#                         user-space .deb (bundled mode). When empty, the image
#                         expects libDeckLinkAPI.so bind-mounted from the host
#                         (DECKLINK_LIB_MODE=hostmount).

# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS build

ARG MXL_REF=v1.0.1

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git pkg-config curl zip unzip tar \
        ca-certificates bison flex autoconf automake libtool \
    && rm -rf /var/lib/apt/lists/*

# vcpkg for the MXL dependency manifest (spdlog, fmt, stduuid, picojson).
RUN git clone https://github.com/microsoft/vcpkg /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
ENV VCPKG_ROOT=/opt/vcpkg

# MXL v1.0.1 (Apache-2.0), installed to /opt/mxl.
RUN git clone --branch "${MXL_REF}" https://github.com/dmf-mxl/mxl /tmp/mxl \
    && cmake -S /tmp/mxl -B /tmp/mxl/build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF -DBUILD_UTILS=OFF -DBUILD_DOCS=OFF \
        -DCMAKE_INSTALL_PREFIX=/opt/mxl \
    && cmake --build /tmp/mxl/build -j"$(nproc)" \
    && cmake --install /tmp/mxl/build

# The application. The DeckLink interface headers are vendored in-tree;
# libDeckLinkAPI.so is dlopen'ed at runtime, never linked (§5.1).
COPY . /src
RUN cmake -S /src -B /src/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        "-DCMAKE_PREFIX_PATH=/opt/mxl;/tmp/mxl/build/vcpkg_installed/x64-linux" \
        -DMXL_DECKLINK_BUILD_TESTS=ON \
    && cmake --build /src/build -j"$(nproc)" \
    && LD_LIBRARY_PATH=/opt/mxl/lib /src/build/unit-tests

# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

ARG DESKTOPVIDEO_DEB_URL=""

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

# Optional bundled Desktop Video user-space package (§5.1 stage 2). The DKMS
# kernel-module part is expected to fail inside the container — the modules
# come from the host. The installed version MUST match the host driver.
RUN if [ -n "${DESKTOPVIDEO_DEB_URL}" ]; then \
        curl -fsSL -o /tmp/desktopvideo.deb "${DESKTOPVIDEO_DEB_URL}" \
        && (dpkg -i /tmp/desktopvideo.deb || apt-get -f install -y --no-install-recommends) \
        && rm -f /tmp/desktopvideo.deb; \
    fi

COPY --from=build /opt/mxl/lib/ /usr/local/lib/
COPY --from=build /src/build/mxl-decklink /usr/local/bin/mxl-decklink
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN ldconfig && chmod +x /usr/local/bin/entrypoint.sh

# Non-root user (§5.1): uid 10001, group `video` (host-aligned GID 44 on
# Debian/Ubuntu; override with --group-add for other hosts).
RUN useradd --uid 10001 --no-create-home --shell /usr/sbin/nologin mxl \
    && usermod -aG video mxl
USER 10001:10001

EXPOSE 9080 9090
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
