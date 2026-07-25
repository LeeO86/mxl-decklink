# AGENTS.md

## Cursor Cloud specific instructions

`mxl-decklink` is a single C++20/CMake service (Vue web UI under `web/`,
built via npm at configure/build time). It bridges a Blackmagic DeckLink card
and an MXL shared-memory domain, and exposes health (`/livez`, `/readyz`,
`/statusz`), Prometheus metrics (`/metrics`), and the web UI/API on one
consolidated `WEB_PORT` (default 8080). A built-in mock DeckLink backend
(`MXL_DECKLINK_BACKEND=mock`) lets you build, test, and run the full pipeline
with no hardware. Canonical build/run/test commands live in `README.md`
("Building", "Running", "Testing without hardware"), the CI workflow
`.github/workflows/ci.yaml`, and `tests/integration/smoke.sh`.

### Prebuilt toolchain (baked into the VM snapshot)

The external MXL dependency is expensive to build and is prebuilt into the image,
so the startup update script does not rebuild it:

- `/opt/mxl` — MXL v1.0.1 installed (`libmxl.so`, headers, CMake config).
- `~/mxl/build/vcpkg_installed/x64-linux` — MXL's vcpkg dependencies (`fmt`,
  `spdlog`, `stduuid`, `picojson`); put this on `CMAKE_PREFIX_PATH` when building
  this app.
- `~/vcpkg` — bootstrapped vcpkg (only needed to rebuild MXL).

`libstdc++-14-dev` is installed. This matters: the default `cc`/`c++` resolve to
clang, which auto-selects the gcc-14 toolchain — without `libstdc++-14-dev` that
link fails with `/usr/bin/ld: cannot find -lstdc++`. With the package present, the
default toolchain works and no `CC`/`CXX` override is needed (CMake picks up
`g++`/gcc-13 by default here anyway).

### Build, test, run (mock backend, no hardware)

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DMXL_DECKLINK_BUILD_TESTS=ON \
  "-DCMAKE_PREFIX_PATH=/opt/mxl;$HOME/mxl/build/vcpkg_installed/x64-linux"
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
"Build MXL": clone `dmf-mxl/mxl` at `v1.0.1`, configure with the `~/vcpkg` toolchain,
install to `/opt/mxl`, and keep `build/vcpkg_installed/x64-linux` for
`CMAKE_PREFIX_PATH`.
