# OpenMOQ Publisher

[English](README.md) | [Español](README.es.md) | [Français](README.fr.md) | [Italiano](README.it.md) | [日本語](README.ja.md) | [Português](README.pt.md) | [中文](README.zh.md)

`moqxr` is a C++20 OpenMOQ publisher for Linux, macOS, and Windows.

It turns MP4 input into CMSF-style publishable objects, builds draft-aware MOQT publish plans, and can either inspect those plans locally or publish them over picoquic-backed Raw QUIC and WebTransport paths.

## What It Does

- Parses fragmented MP4 input with `ftyp` + `moov` + `moof`/`mdat`.
- Remuxes progressive MP4 input into synthesized fragmented media objects.
- Extracts track metadata and RFC 6381 codec identifiers.
- Preserves HEVC signaling and normalizes `hev1` to `hvc1` when needed.
- Builds publish plans with catalog, optional MSF media timeline and SAP event timeline metadata, and media objects.
- Emits generated objects and catalog metadata to disk for inspection.
- Supports draft-aware MOQT framing for drafts 14, 16, and 18.
- Publishes over Raw QUIC or WebTransport when picoquic and picotls are available.

## Quick Start

Build and test:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

Inspect a publish plan:

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

Emit catalog and media objects:

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

Publish to a relay:

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://relay.example.com:443/moq \
  --namespace media \
  --forward 0 \
  --timeout 10 \
  --paced
```

Live ingest examples (choose one path, not both):

1. SRT ingest path (`--live-source srt`)

Create an SRT config file (example: `/tmp/srt_callers.json`):

```json
{
  "srt_callers": [
    {
      "id": "cam1",
      "srt": {
        "mode": "caller",
        "host": "127.0.0.1",
        "port": 9000,
        "latency_ms": 120
      },
      "mpegts": {
        "auto_detect_program": true,
        "program_number": null,
        "video_pid": null,
        "audio_pid": null
      },
      "cmaf": {
        "fragment_on_keyframe": true,
        "empty_moov": true,
        "default_base_moof": true,
        "separate_moof_per_track": true,
        "target_fragment_duration_ms": 1000
      }
    }
  ]
}
```

Start the publisher (SRT receiver + MoQ publisher):

```bash
./build/openmoq-publisher \
  --live-source srt \
  --srt-config /tmp/srt_callers.json \
  --endpoint 127.0.0.1:4443 \
  --transport raw \
  --namespace live \
  --draft 16 \
  --timeout 120 \
  --forward 0
```

Feed MPEG-TS over SRT from ffmpeg:

```bash
ffmpeg -hide_banner -stream_loop -1 -re \
  -i /home/ubuntu/bbb_sunflower_1080p_30fps_normal.mp4 \
  -filter_complex "[0:v]drawtext=fontcolor=white:fontsize=36:box=1:boxcolor=black@0.45:boxborderw=8:x=w-tw-20:y=20:text='%{localtime\\:%Y-%m-%d %T}\\:%{eif\\:mod(t*1000\\,1000)\\:d\\:3}'[vclock]" \
  -map "[vclock]" -map 0:a:0 \
  -c:v libx265 -preset veryfast -r 30 -g 60 -keyint_min 60 -bf 0 \
  -x265-params "keyint=60:min-keyint=60:scenecut=0:open-gop=0:repeat-headers=1" \
  -c:a aac -b:a 160k -ar 48000 -ac 2 \
  -f mpegts "srt://0.0.0.0:9000?mode=listener&pkt_size=1316"
```

2. stdin fragmented-MP4 path (`--live-source stdin`)

```bash
ffmpeg -i /home/ubuntu/bbb_sunflower_1080p_30fps_normal.mp4 \
  -map 0:v:0 -map 0:a:0 \
  -map_metadata -1 \
  -sn -dn \
  -c:v libx264 -preset medium -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
  -c:a aac -b:a 160k -ar 48000 -ac 2 \
  -movflags +frag_keyframe+empty_moov+default_base_moof+separate_moof \
  -f mp4 - | ./build/openmoq-publisher \
    --live-source stdin \
    --input - \
    --endpoint 127.0.0.1:4443 \
    --transport raw \
    --namespace live \
    --draft 16 \
    --timeout 120 \
    --forward 0
```

`--live-source both` is intentionally not supported.

On Windows, replace `./build/openmoq-publisher` with `build\Release\openmoq-publisher.exe` or the matching build configuration path.

## Documentation

| Topic | Link |
| --- | --- |
| Build and dependencies | [docs/build.md](docs/build.md) |
| CLI quick start | [docs/quickstart.md](docs/quickstart.md) |
| Testing | [docs/testing.md](docs/testing.md) |
| Design overview | [docs/design.md](docs/design.md) |
| FFmpeg input recipes | [docs/ffmpeg.md](docs/ffmpeg.md) |
| Relay interoperability | [docs/relay-interop.md](docs/relay-interop.md) |
| C++ Publisher API | [docs/publisher-api.md](docs/publisher-api.md) |
| Protocol mapping | [docs/protocol-mapping.md](docs/protocol-mapping.md) |
| WebTransport compliance | [docs/webtransport-compliance.md](docs/webtransport-compliance.md) |
| Transport plan | [docs/transport-plan.md](docs/transport-plan.md) |
| Project status and roadmap | [docs/status.md](docs/status.md) |

Localized Publisher API guides are available in [Spanish](docs/publisher-api.es.md), [French](docs/publisher-api.fr.md), [Italian](docs/publisher-api.it.md), [Japanese](docs/publisher-api.ja.md), [Portuguese](docs/publisher-api.pt.md), and [Chinese](docs/publisher-api.zh.md).

## Repository Layout

- `include/openmoq/publisher`: public headers
- `src`: library and CLI implementation
- `tests`: CTest-based unit coverage
- `docs`: protocol notes, integration guides, and design references
- `examples`: example publisher integrations
- `.github/workflows/ci.yml`: Linux, macOS, and Windows CI
- `.github/workflows/release.yml`: release artifact builds

## Current Status

The publisher can generate publish plans, emit inspectable output, and publish over picoquic-backed Raw QUIC and WebTransport transports. Draft 14 is the primary target, draft 16 is maintained as a compatibility profile, and draft 18 support is implemented for version selection, setup/request framing codec paths, and request-stream response correlation while interop hardening continues.

For the detailed roadmap, see [docs/status.md](docs/status.md).
