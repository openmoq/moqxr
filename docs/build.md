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

The default build also compiles the MSFTS Publisher API example:

```bash
cmake --build build --target openmoq-publisher-msfts-example
./build/examples/msfts-publisher/openmoq-publisher-msfts-example --help
```

Its CMake target links only `openmoq_publisher_lib`; it does not consume moq5
or picoquic APIs directly.

To omit this and the other example executables (for instance in embedded or
packaging builds), configure with `-DOPENMOQ_BUILD_EXAMPLES=OFF`. See
`docs/dependencies.md` for details, including the interaction with
`OPENMOQ_BUILD_TESTS`.

## Managed Pico Dependencies

By default, CMake owns dependency checkouts under `<build>/_deps` and follows:

- `private-octopus/picoquic` `master`
- canonical `h2o/picotls` `master`, including its recorded submodules
- `openmoq/moq5` `main` when `OPENMOQ_USE_LIBMOQ_PUBLISHER=ON`

The first configure downloads the current branch heads. Later configures use a
successful-check timestamp in that build directory and contact upstream again
after 24 hours. If a required initial download or scheduled refresh cannot
reach upstream, configure fails instead of silently accepting a stale branch.
Configure output records each resolved source directory, tracking branch, and
commit. Run the configure command before an incremental build when you want the
daily dependency check; CI and release builds always configure first.

Set `OPENMOQ_DEPENDENCY_REFRESH_INTERVAL_HOURS` to change the interval. Setting
it to `0` checks on every configure.

## Build with Local Picoquic and Picotls

Explicit source overrides remain available for offline development or testing a
local dependency change. CMake never fetches, checks out, or otherwise modifies
an override directory.

Clone picoquic and canonical picotls to any convenient location and initialize
the picotls submodules:

```bash
git clone https://github.com/private-octopus/picoquic.git /path/to/picoquic
git clone --recurse-submodules https://github.com/h2o/picotls.git /path/to/picotls
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
  -DOPENSSL_ROOT_DIR="C:\Program Files\OpenSSL-Win64" `
  -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

GitHub Actions workflows set `OPENSSL_ROOT_DIR` automatically from the runner's pre-installed OpenSSL, so no manual step is needed there.

## Useful CMake Options

- `-DOPENMOQ_ENABLE_PICOQUIC=ON|OFF`
- `-DOPENMOQ_DEPENDENCY_REFRESH_INTERVAL_HOURS=24`
- `-DOPENMOQ_PICOQUIC_SOURCE_DIR=/path/to/picoquic` (explicit local override)
- `-DOPENMOQ_PICOTLS_SOURCE_DIR=/path/to/picotls` (explicit local override)
- `-DOPENMOQ_LIBMOQ_SOURCE_DIR=/path/to/moq5` (explicit local override)
- `-DOPENMOQ_OPENSSL_ROOT_DIR=/path/to/openssl`
- `-DOPENSSL_ROOT_DIR=/path/to/openssl`
- `-DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=ON|OFF`
- `-DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON|OFF` (default `OFF`)

### Publish backend (temporary migration gate)

moqxr is migrating its publish path from the legacy MoqtSession/moxygen-style
transport onto the **libmoq** service tier. While the migration is being
reviewed, the backend is selectable:

- **`-DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON`** — the production `Publisher` routes
  batch, live stdin, live SRT, and `LiveObjectSource` publishing through a
  managed checkout of `openmoq/moq5` `main`. The libmoq route splits
  `--namespace a/b` into the wire tuple `{a, b}`, resolves the root CA bundle
  the same way the legacy path does (`--ca`, then `SSL_CERT_FILE`, then the
  well-known system bundles) instead of relying on OpenSSL's compiled-in
  default directory, and honours `--paced` and `--loop` for batch input.
- **local libmoq override** — setting `OPENMOQ_LIBMOQ_SOURCE_DIR` builds and
  validates that source tree even when the legacy backend remains selected.
- **Caller-supplied catalog exception** — a `LiveObjectSource` using
  `LiveCatalogMode::kSourceObject` is routed through `MoqtSession` in either
  configuration. This preserves catalog formats such as MSFTS `"m2ts"` that
  libmoq's current RAW/CMAF media sender cannot author.
- **default (`OFF`)** — publishing stays on the legacy MoqtSession path.

Configure-time output reports both, e.g.:

```
-- OpenMOQ: libmoq available .......... ON
-- OpenMOQ: publish backend .......... legacy MoqtSession (set -DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON for libmoq)
```

This gate is **temporary** — it will be removed once the libmoq publish path is
accepted as the default. An injected `TransportFactory` always forces the legacy
path regardless of this option.

For a local checkout override:

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
- CI and release use the managed dependency flow, so each clean job resolves
  picoquic `master` and canonical picotls `master` during configure
- Linux and macOS archives are `.tar.gz` and contain `openmoq-publisher`, `libopenmoq_publisher.a`, `include/`, `docs/`, `README.md`, and `LICENSE`
- Windows archives are `.zip` and contain `openmoq-publisher.exe`, `openmoq_publisher.lib`, `include/`, `docs/`, `README.md`, and `LICENSE`

## CI

GitHub Actions builds and tests the project on:

- the default legacy backend on `ubuntu-latest`, `macos-latest`, and
  `windows-latest`
- the opt-in libmoq backend on `ubuntu-latest`, including the
  `openmoq-publisher-libmoq-translation-tests` target

Every lane runs CMake configure, the full default build, a static-library
existence check, and CTest. The libmoq lane also resolves `openmoq/moq5` `main`
during configure.
