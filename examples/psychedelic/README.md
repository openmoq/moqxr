# Psychedelic Example

This example (`Psychedelic.cpp`) demonstrates real live integration with the `moqxr` C++ Publisher API.

It now:

- launches one `ffmpeg` pipeline with video + audio inputs
- maps them into separate tracks in one fragmented MP4 stream
- publishes via one `Publisher::publish_live(...)` session
- uses one namespace containing separate video/audio tracks
- performs explicit graceful teardown with `disconnect(0)`
- prints the structured post-publish `Publisher::stats()` summary

## Prerequisites

- `ffmpeg` installed and available in `PATH`
- running MOQT relay endpoint

## Build

From repo root:

```bash
cmake -S . -B build -DOPENMOQ_BUILD_EXAMPLES=ON
cmake --build build --target openmoq-publisher-psychedelic-example
```

## Run

From repo root:

```bash
./build/openmoq-publisher-psychedelic-example --insecure
```

Optional flags:

```bash
./build/openmoq-publisher-psychedelic-example \
  --endpoint https://127.0.0.1:4433/moq \
  --namespace live.psychedelic.stream \
  --draft 16 \
  --seconds 20 \
  --insecure
```

Accepted endpoint forms:

- `host:port` (raw QUIC)
- `moqt://host:port/path` (raw QUIC)
- `https://host:port/path` (WebTransport)

Ports must be in the range 1–65535. Bracket IPv6 literals, for example
`https://[::1]:4433/moq`. Run with `--help` to print usage without starting
FFmpeg or connecting to the relay.

Expected output includes:

- ffmpeg launch line
- publish completion line
- publisher byte, object, and group counters

## Notes

- This target links against the CMake target `openmoq_publisher_lib`, which emits the packaged static archive as `libopenmoq_publisher.a` on Linux/macOS or `openmoq_publisher.lib` on Windows.
- The example uses the same public API headers shipped under `include/openmoq/publisher`.
- TLS certificates are verified by default. The commands above use `--insecure`
  for a local relay with a self-signed certificate; omit it for a trusted relay.
