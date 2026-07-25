# Build

This document covers local builds, optional picoquic transport dependencies, Windows requirements, release builds, and CI build behavior.

## Baseline Build

This is the default path for local development. It works on Linux, macOS, and Windows:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

Using presets with the same `build/` output directory:

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```

On Windows with the Visual Studio generator, the binary lands in `build\Release\` or `build\Debug\` depending on the config passed to `--build`.

The build also produces the reusable publisher API static library:

- Linux/macOS single-config builds: `build/libopenmoq_publisher.a`
- Visual Studio multi-config builds: `build\<config>\openmoq_publisher.lib`

The CMake target remains `openmoq_publisher_lib`, so projects that include this repository with `add_subdirectory(...)` should link that target. Projects that consume the raw archive directly should add `include/` to their include path and link the same transport dependencies used by the build, especially picoquic, picotls, OpenSSL, and platform socket libraries when picoquic transport support is enabled.

## Build with Local Picoquic and Picotls

By default, CMake looks for:

- sibling `../picoquic` and `../picotls` checkouts
- fallback: `third_party/picoquic` and `third_party/picotls`
- fallback: `thirdparty/picoquic` and `thirdparty/picotls`

If you prefer custom paths, clone picoquic and picotls to any convenient location and initialize the picotls submodules:

```bash
git clone https://github.com/private-octopus/picoquic.git /path/to/picoquic
git clone --recurse-submodules https://github.com/private-octopus/picotls.git /path/to/picotls
```

Then point CMake at them:

```bash
cmake -S . -B build \
  -DOPENMOQ_PICOQUIC_SOURCE_DIR=/path/to/picoquic \
  -DOPENMOQ_PICOTLS_SOURCE_DIR=/path/to/picotls \
  -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

## Windows Additional Requirements

picotls requires both `pkg-config` and OpenSSL headers and libraries. On Windows, install both and tell CMake where OpenSSL is:

```powershell
# One-time: install pkg-config shim and OpenSSL (skip if already present)
choco install pkgconfiglite openssl

cmake -S . -B build `
  -DOPENMOQ_PICOQUIC_SOURCE_DIR=C:\path\to\picoquic `
  -DOPENMOQ_PICOTLS_SOURCE_DIR=C:\path\to\picotls `
  -DOPENSSL_ROOT_DIR="C:\Program Files\OpenSSL-Win64" `
  -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

GitHub Actions workflows set `OPENSSL_ROOT_DIR` automatically from the runner's pre-installed OpenSSL, so no manual step is needed there.

## Useful CMake Options

- `-DOPENMOQ_ENABLE_PICOQUIC=ON|OFF`
- `-DOPENMOQ_PICOQUIC_SOURCE_DIR=/path/to/picoquic`
- `-DOPENMOQ_PICOTLS_SOURCE_DIR=/path/to/picotls`
- `-DOPENMOQ_LIBMOQ_SOURCE_DIR=/path/to/moq5`
- `-DOPENMOQ_OPENSSL_ROOT_DIR=/path/to/openssl`
- `-DOPENSSL_ROOT_DIR=/path/to/openssl`
- `-DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=ON|OFF`
- `-DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON|OFF` (default `OFF`)

### Publish backend (temporary migration gate)

moqxr is migrating its publish path from the legacy MoqtSession/moxygen-style
transport onto the sibling **libmoq** service tier. While the migration is being
reviewed, the backend is selectable:

- **libmoq available** — the libmoq integration code (translation, drivers,
  tests) builds and is validated whenever CMake finds a sibling `../moq5`
  checkout. It also accepts `../libmoq`, `third_party/moq5`,
  `thirdparty/moq5`, or an explicit `OPENMOQ_LIBMOQ_SOURCE_DIR`; availability
  is independent of which backend is *selected*.
- **`-DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON`** — the production `Publisher` routes
  batch, live stdin, live SRT, and `LiveObjectSource` publishing through libmoq.
- **default (`OFF`)** — publishing stays on the legacy MoqtSession path.

Configure-time output reports both, e.g.:

```
-- OpenMOQ: libmoq available .......... ON
-- OpenMOQ: publish backend .......... legacy MoqtSession (set -DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON for libmoq)
```

This gate is **temporary** — it will be removed once the libmoq publish path is
accepted as the default. An injected `TransportFactory` always forces the legacy
path regardless of this option.

For a checkout outside the auto-detected locations:

```bash
cmake -S . -B build-libmoq \
  -DOPENMOQ_LIBMOQ_SOURCE_DIR=/path/to/moq5 \
  -DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON
cmake --build build-libmoq
ctest --test-dir build-libmoq --output-on-failure
```

## Release Builds

GitHub Actions publishes release archives for Linux, macOS, and Windows:

- pushing a `v*` tag builds release artifacts and attaches them to the matching GitHub Release
- running the `Release Builds` workflow manually uploads the same archives as workflow artifacts
- manual runs can also publish a GitHub Release when you provide a `release_tag` such as `v0.1.0`
- both CI and release workflows check out `private-octopus/picoquic` plus `private-octopus/picotls`, so published binaries include the picoquic transport path
- Linux and macOS archives are `.tar.gz` and contain `openmoq-publisher`, `libopenmoq_publisher.a`, `include/`, `docs/`, `README.md`, and `LICENSE`
- Windows archives are `.zip` and contain `openmoq-publisher.exe`, `openmoq_publisher.lib`, `include/`, `docs/`, `README.md`, and `LICENSE`

## CI

GitHub Actions builds and tests the project on:

- the default legacy backend on `ubuntu-latest`, `macos-latest`, and
  `windows-latest`
- the opt-in libmoq backend on `ubuntu-latest`, including the
  `openmoq-publisher-libmoq-translation-tests` target

Every lane runs CMake configure, the full default build, a static-library
existence check, and CTest.
