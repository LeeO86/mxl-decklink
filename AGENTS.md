# AGENTS.md

## Cursor Cloud specific instructions

`mxl-decklink` is a single C++20/CMake service (no package manager, no lockfiles).
It bridges a Blackmagic DeckLink card and an MXL shared-memory domain, and exposes
health (`/livez`, `/readyz`, `/statusz` on `HEALTH_PORT`, default 9080) and
Prometheus metrics (`/metrics` on `METRICS_PORT`, default 9090). A built-in mock
DeckLink backend (`MXL_DECKLINK_BACKEND=mock`) lets you build, test, and run the
full pipeline with no hardware. Canonical build/run/test commands live in
`README.md` ("Building", "Running", "Testing without hardware"), the CI workflow
`.github/workflows/ci.yaml`, and `tests/integration/smoke.sh`.

### Prebuilt toolchain (baked into the VM snapshot)

The external MXL dependency is expensive to build and is prebuilt into the image,
so the startup update script does not rebuild it:

- `/opt/mxl` — MXL v1.0.1 installed (`libmxl.so`, headers, CMake config).
- `/opt/mxl-vcpkg` — MXL's vcpkg dependencies (`fmt`, `spdlog`, `stduuid`,
  `picojson`), needed on `CMAKE_PREFIX_PATH` when building this app.
- `/opt/vcpkg` — bootstrapped vcpkg (only needed to rebuild MXL).

### Compiler gotcha (IMPORTANT)

On this image the default `cc`/`c++` (and `/etc/alternatives`) resolve to **clang**,
which auto-selects the **gcc-14** toolchain whose `libstdc++` dev files are not
installed — so any build using the default compiler fails at link with
`/usr/bin/ld: cannot find -lstdc++`. Always build with **gcc-13** by exporting
`CC=gcc CXX=g++` and passing `-DCMAKE_CXX_COMPILER=g++` to CMake (this is also the
compiler MXL was built with, so ABI matches).

### Build, test, run (mock backend, no hardware)

```bash
export CC=gcc CXX=g++
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++ -DMXL_DECKLINK_BUILD_TESTS=ON \
  "-DCMAKE_PREFIX_PATH=/opt/mxl;/opt/mxl-vcpkg"
cmake --build build -j"$(nproc)"

# unit tests (doctest)
LD_LIBRARY_PATH=/opt/mxl/lib ./build/unit-tests
# end-to-end smoke test (mock card + real MXL domain in /dev/shm)
LD_LIBRARY_PATH=/opt/mxl/lib tests/integration/smoke.sh build/mxl-decklink
```

Run the service directly with the mock backend by setting `MXL_DECKLINK_BACKEND=mock`
plus the `CHx_*` variables from `README.md` §Running (always with
`LD_LIBRARY_PATH=/opt/mxl/lib`). Lint = compiler warnings: the build uses
`-Wall -Wextra`, so a clean `cmake --build` is the lint signal (there is no
separate linter).

### `/dev/shm` is only 64 MiB

The MXL domain is a tmpfs directory. The default `/dev/shm` here is 64 MiB, which
is why `tests/integration/smoke.sh` and the README examples use `HD720p50` and, for
the auto-format case, a short `history_duration` in `options.json`. Higher-res
modes or deeper rings can exhaust `/dev/shm` (grain allocation errors); use a
smaller mode/history or a larger tmpfs.

### If `/opt/mxl` is ever missing

Rebuild it (only needed if the snapshot lost it) following `.github/workflows/ci.yaml`
"Build MXL": clone `dmf-mxl/mxl` at `v1.0.1`, configure with the vcpkg toolchain and
`CC=gcc CXX=g++`, install to `/opt/mxl`, and copy `build/vcpkg_installed/x64-linux`
to `/opt/mxl-vcpkg`.
