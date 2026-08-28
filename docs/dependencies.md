# Build Dependencies

This document lists the tools, libraries, and source checkouts used to build
moqxr. Dependencies are grouped by the CMake flag that enables them. Unless a
source override is set, managed source dependencies are downloaded into
`<build>/_deps` during CMake configuration.

## Required for Every Build

| Dependency | Requirement | Notes |
| --- | --- | --- |
| CMake | 3.20 or newer | CMake configures the project and provides CTest when tests are enabled. |
| C compiler | Compatible with the selected dependencies | The project and its managed transport dependencies contain C sources. |
| C++ compiler | C++20 support | The public publisher target requires C++20. |
| Build tool | One supported by the selected CMake generator | The `default` preset uses Ninja. Unix Makefiles and Visual Studio generators are also supported. |

Git and HTTPS access are also required when CMake must clone or refresh a
managed dependency. They are not required when every selected source dependency
is already cached and its refresh interval has not expired, or when explicit
local source overrides are used.

## `OPENMOQ_ENABLE_PICOQUIC`

Default: `ON`

This flag enables the picoquic QUIC and WebTransport transport. The following
dependencies are required when it is enabled:

| Dependency | How it is supplied | Purpose |
| --- | --- | --- |
| [picoquic](https://github.com/private-octopus/picoquic) | Managed `master` checkout, or `OPENMOQ_PICOQUIC_SOURCE_DIR` | QUIC, HTTP/3, and WebTransport transport |
| [picotls](https://github.com/h2o/picotls) | Managed `master` checkout with submodules, or `OPENMOQ_PICOTLS_SOURCE_DIR` | TLS implementation used by picoquic |
| OpenSSL development headers and libraries | System installation selected by CMake | Required by the picoquic build's default OpenSSL backend |
| pkg-config | System installation | Required while configuring picotls; on Windows, CI uses `pkgconfiglite` |
| Platform thread library | Located by CMake's `Threads` package | Required by picoquic |

The project adds required platform libraries automatically. Windows builds link
`bcrypt`, `ws2_32`, and `iphlpapi`; these are provided by the Windows SDK.
Picotls can use Brotli when both Brotli pkg-config modules are present, but
Brotli is optional and is not required for a moqxr build. Picoquic's optional
AEGIS, Mbed TLS, and io_uring modes are not enabled by moqxr.

Use `OPENMOQ_OPENSSL_ROOT_DIR` to force picotls, picoquic, and libmoq to use the
same OpenSSL installation. The standard `OPENSSL_ROOT_DIR` hint is also
accepted. On Windows, the documented setup is:

```powershell
choco install pkgconfiglite openssl

cmake -S . -B build `
  -DOPENMOQ_OPENSSL_ROOT_DIR="C:\Program Files\OpenSSL-Win64"
```

Set `-DOPENMOQ_ENABLE_PICOQUIC=OFF` to build without picoquic, picotls,
OpenSSL, pkg-config, or the picoquic thread dependency. The publisher library
and non-network packaging code still build, but the picoquic transport is not
available.

## `OPENMOQ_ENABLE_SRT`

Default: `ON`

SRT ingest requires the libsrt development package:

| Dependency | Detection | Purpose |
| --- | --- | --- |
| libsrt headers (`srt/srt.h`) | `find_path` | SRT API declarations |
| libsrt library (`srt`) | `find_library` | SRT ingest implementation |

This is a soft dependency. If the flag is `ON` but either the header or library
is missing, CMake reports that libsrt was not found and continues with SRT
ingest disabled. Set `-DOPENMOQ_ENABLE_SRT=OFF` to skip detection explicitly.

## `OPENMOQ_USE_LIBMOQ_PUBLISHER`

Default: `OFF`

Enabling this flag selects the libmoq publisher backend and adds:

| Dependency | How it is supplied | Purpose |
| --- | --- | --- |
| [moq5/libmoq](https://github.com/openmoq/moq5) | Managed `main` checkout, or `OPENMOQ_LIBMOQ_SOURCE_DIR` | MOQT service, MSF, and media-object tiers |

When `OPENMOQ_ENABLE_PICOQUIC=ON`, libmoq reuses the picoquic, picotls,
OpenSSL, and thread dependencies already configured by moqxr and builds its raw
QUIC and picoquic WebTransport adapters. When picoquic is disabled, libmoq's
core and service tiers build without those adapters.

Setting `OPENMOQ_LIBMOQ_SOURCE_DIR` configures and links that local libmoq tree
even when `OPENMOQ_USE_LIBMOQ_PUBLISHER=OFF`. This validates the integration
while leaving the legacy publisher backend selected. CMake does not fetch or
modify an explicit source override.

## `OPENMOQ_BUILD_TESTS`

Default: `ON`

The maintained tests use CTest and small in-repository test executables. They do
not require an external unit-test framework or FFmpeg. The MP4 regression
fixture is stored in the repository, so FFmpeg is not needed to build or run
that test.

Set `-DOPENMOQ_BUILD_TESTS=OFF` to omit all moqxr test executables and CTest
registrations. This flag does not disable the example executables built outside
the test block; use `OPENMOQ_BUILD_EXAMPLES` for those.

## `OPENMOQ_BUILD_EXAMPLES`

Default: `OFF`

The default build compiles the example executables under `examples/`
(`openmoq-publisher-psychedelic-example`, `openmoq-publisher-auth-example`, and
`openmoq-publisher-msfts-example`). Set `-DOPENMOQ_BUILD_EXAMPLES=OFF` to omit
all of them, for instance when consuming this repository via
`add_subdirectory(...)` and only the `openmoq_publisher_lib` target and CLI are
wanted.

The MSFTS example's companion test (`openmoq-publisher-msfts-tests`) compiles
example sources, so it is built and registered with CTest only when both
`OPENMOQ_BUILD_EXAMPLES=ON` and `OPENMOQ_BUILD_TESTS=ON`. The project defines
no `install()` rules, so disabling examples also guarantees they never appear
in downstream packaging steps that stage built artifacts.

## `OPENMOQ_RUN_PICOQUIC_SMOKE_TESTS`

Default: `OFF`

This flag adds the picoquic loopback and TLS-verification smoke-test
executables. It is effective only when both `OPENMOQ_BUILD_TESTS=ON` and
`OPENMOQ_ENABLE_PICOQUIC=ON`; its dependency set is therefore the complete
`OPENMOQ_ENABLE_PICOQUIC` set above. Running the tests also requires permission
to create local network sockets.

## Dependency Source and Refresh Flags

These cache variables change where dependencies come from; they do not enable
features by themselves.

| CMake variable | Default | Effect |
| --- | --- | --- |
| `OPENMOQ_DEPENDENCY_REFRESH_INTERVAL_HOURS` | `24` | Minimum age before CMake checks managed branch heads again; `0` checks on every configure. |
| `OPENMOQ_PICOQUIC_SOURCE_DIR` | Empty | Uses a local picoquic source tree instead of the managed checkout. |
| `OPENMOQ_PICOTLS_SOURCE_DIR` | Empty | Uses a local picotls source tree instead of the managed checkout. The checkout must include its required submodules. |
| `OPENMOQ_LIBMOQ_SOURCE_DIR` | Empty | Uses and configures a local moq5/libmoq source tree. |
| `OPENMOQ_OPENSSL_ROOT_DIR` | Empty | Pins one OpenSSL prefix for picotls, picoquic, and libmoq. |
| `OPENSSL_ROOT_DIR` | Environment or CMake default | Standard CMake OpenSSL location hint when the moqxr-specific prefix is not set. |

An explicit source override must contain a `CMakeLists.txt`. The build never
fetches, checks out, or modifies files inside an override directory.

## Tools That Are Not Build Dependencies

FFmpeg is used by live-media examples and end-to-end ingest testing, but it is
not required to compile moqxr or run its maintained CTest suite. Catapult token
helpers and external relays are runtime integration tools, not build
dependencies.
