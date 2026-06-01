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
cmake -S . -B build
cmake --build build --target openmoq-publisher-psychedelic-example
```

## Run

From repo root:

```bash
./build/openmoq-publisher-psychedelic-example
```

Optional flags:

```bash
./build/openmoq-publisher-psychedelic-example \
  --endpoint https://127.0.0.1:4433/moq \
  --namespace live.psychedelic.stream \
  --draft 16 \
  --seconds 20
```

Accepted endpoint forms:

- `host:port` (raw QUIC)
- `moqt://host:port/path` (raw QUIC)
- `https://host:port/path` (WebTransport)

Expected output includes:

- ffmpeg launch line
- publish completion line
- publisher runtime stats JSON

## Notes

- This target links against the CMake target `openmoq_publisher_lib`, which emits the packaged static archive as `libopenmoq_publisher.a` on Linux/macOS or `openmoq_publisher.lib` on Windows.
- The example uses the same public API headers shipped under `include/openmoq/publisher`.
- For local testing, the example sets `insecure_skip_verify=true`.
