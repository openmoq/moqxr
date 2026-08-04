# OpenMOQ Publisher

[English](README.md) | [Español](README.es.md) | [Français](README.fr.md) | [Italiano](README.it.md) | [日本語](README.ja.md) | [Português](README.pt.md) | [中文](README.zh.md)

`moqxr` is a C++20 OpenMOQ publisher for Linux, macOS, and Windows.

It packages file and live media for Media over QUIC Transport (MOQT), builds MSF/CMSF catalogs and publish plans, and publishes through picoquic-backed Raw QUIC or WebTransport connections.

## Capabilities

- Parses fragmented MP4 (`ftyp` + `moov` + `moof`/`mdat`) and remuxes progressive MP4 into fragmented media objects.
- Extracts track metadata and RFC 6381 codec identifiers, including HEVC signaling and `hev1` to `hvc1` normalization.
- Builds MSF/CMSF version 1 catalogs, initialization data, optional media timelines, and SAP event timelines.
- Detects and signals existing CMAF CENC content protection for batch input, live fragmented MP4 on stdin, and CTE LL-DASH ingest. It does not encrypt or decrypt media.
- Emits catalog, initialization, media, probe, and publish-plan files for local inspection.
- Publishes with the main CLI's supported MOQT draft profiles: draft 16 (default) and draft 18.
- Publishes over Raw QUIC or WebTransport when picoquic and picotls are available.
- Accepts live fragmented MP4 from stdin, MPEG-TS over SRT when libsrt is available, and CMAF over HTTP/1.1 chunked CTE LL-DASH ingest.
- Parses MSF URLs with `--url` and prints the catalog discovery URL with `--print-msf-urls`.
- Provides C++ Publisher API examples for FFmpeg-generated live media, CAT4MOQ authorization, and MPEG-2 TS/M2TS packaging.
- Optionally routes publishing through the [moq5](https://github.com/openmoq/moq5) C11 Media-over-QUIC library for drafts 16 and 18.

## Quick Start

Build and test:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

The default build creates the `openmoq-publisher` executable and the static Publisher library: `build/libopenmoq_publisher.a` on Linux/macOS, or `build\<config>\openmoq_publisher.lib` with Visual Studio generators on Windows.

Inspect a publish plan:

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

Emit the catalog and packaged media objects:

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

Publish to a relay with the default draft-16 profile:

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://relay.example.com:443/moq \
  --namespace media \
  --draft 16 \
  --forward 0 \
  --timeout 10 \
  --paced
```

`--forward 1` sends objects immediately. `--forward 0` waits for the relay to forward subscriber interest. A printed `connection_id=` confirms transport and MOQT setup only; it does not confirm namespace acceptance or a downstream subscription.

On Windows, replace `./build/openmoq-publisher` with `build\Release\openmoq-publisher.exe` or the path for the selected build configuration.

## Live Ingest

The CLI exposes one live source at a time:

| Source | CLI selection | Input | Notes |
| --- | --- | --- | --- |
| Fragmented MP4 | `--live-source stdin --input -` | CMAF/fMP4 on standard input | Available on all supported platforms |
| SRT | `--live-source srt --srt-config FILE` | MPEG-TS over SRT | Requires libsrt; CENC metadata is unavailable in this path |
| CTE LL-DASH | `--live-source dash --dash-listen HOST:PORT` | Chunked CMAF `POST` or `PUT` requests | Listener currently requires a Unix-like platform |

### SRT ingest

The publisher is an SRT caller. Create `/tmp/srt_callers.json` with the SRT listener address and MPEG-TS/CMAF settings:

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

In the first terminal, start an FFmpeg SRT listener that sends MPEG-TS after the publisher connects:

```bash
ffmpeg -hide_banner -stream_loop -1 -re \
  -i input.mp4 \
  -map 0:v:0 -map 0:a:0 \
  -c:v libx264 -preset veryfast -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
  -c:a aac -b:a 160k -ar 48000 -ac 2 \
  -f mpegts "srt://0.0.0.0:9000?mode=listener&pkt_size=1316"
```

In the second terminal, start the SRT caller and MoQ publisher:

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

The only supported SRT mode is `caller`; the configured host and port must identify an existing SRT listener. Use `--forward 1` for an immediate relay smoke test, or keep `--forward 0` to wait for subscriber interest.

### CTE LL-DASH ingest

Start the publisher with an HTTP/1.1 chunked CMAF listener and a MoQ relay target:

```bash
./build/openmoq-publisher \
  --live-source dash \
  --dash-listen 0.0.0.0:8080 \
  --dash-path /ingest \
  --endpoint https://127.0.0.1:4433/moq \
  --transport webtransport \
  --namespace live \
  --draft 18 \
  --publish-catalog \
  --forward 1 \
  --insecure
```

Send an existing CMAF/fMP4 stream with HTTP/1.1 chunked transfer encoding:

```bash
curl -X PUT \
  -H 'Transfer-Encoding: chunked' \
  -H 'Content-Type: video/iso.segment' \
  --data-binary @live-video.cmaf \
  http://127.0.0.1:8080/ingest/video
```

FFmpeg can instead create two video representations plus audio and push them directly to the ingest prefix:

```bash
ffmpeg -re \
  -f lavfi -i "testsrc2=size=1280x720:rate=25" \
  -f lavfi -i "anullsrc=r=48000:cl=stereo" \
  -filter_complex "[0:v]split=2[v1][v2];[v1]scale=1280:720[v720];[v2]scale=640:360[v360]" \
  -map "[v720]" -c:v:0 libx264 -b:v:0 1500k -g 50 -keyint_min 50 -sc_threshold 0 \
  -map "[v360]" -c:v:1 libx264 -b:v:1 500k -g 50 -keyint_min 50 -sc_threshold 0 \
  -map 1:a -c:a aac -b:a 128k \
  -f dash -seg_duration 2 -use_template 1 -use_timeline 0 \
  -init_seg_name 'video$RepresentationID$' \
  -media_seg_name 'video$RepresentationID$' \
  -adaptation_sets "id=0,streams=v id=1,streams=a" \
  -multiple_requests 1 -streaming 1 -remove_at_exit 0 \
  -window_size 20 -extra_window_size 20 \
  http://127.0.0.1:8080/ingest/
```

Each path below `/ingest` maintains independent parser state and produces path-prefixed MoQ track names. Use `--forward 1` to send objects immediately, or `--forward 0` to wait for subscriber interest. The DASH listener currently requires a Unix-like platform.

See the [CLI quick start](docs/quickstart.md), [FFmpeg recipes](docs/ffmpeg.md), and [SRT technical note](docs/srt-ingest-technical-note.md) for additional details.

## Publishing via moq5

The opt-in [moq5](https://github.com/openmoq/moq5) backend routes batch, live stdin, live SRT, and live-object publishing through moq5's service tier. The service tier handles catalog publication, CMSF/CMAF validation, subscriber-demand gating, bounded backpressure, and graceful transport draining.

CMake fetches current `openmoq/moq5` `main` when this backend is enabled. Set
`OPENMOQ_LIBMOQ_SOURCE_DIR` only to use a local or offline source override:

```bash
cmake -S . -B build-libmoq \
  -DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON
cmake --build build-libmoq
ctest --test-dir build-libmoq --output-on-failure
```

The default build keeps the built-in transport path. See [docs/build.md](docs/build.md) for backend status, dependency discovery, and configuration details.

## Examples

| Example | Target | Purpose |
| --- | --- | --- |
| Psychedelic live publisher | `openmoq-publisher-psychedelic-example` | Runs one FFmpeg audio/video pipeline through `Publisher::publish_live(...)` |
| CAT4MOQ authorization | `openmoq-publisher-auth-example` | Publishes deterministic live objects with token files or a Catapult token command |
| MSFTS publisher | `openmoq-publisher-msfts-example` | Publishes packet-aligned MPEG-2 TS or M2TS objects through `Publisher::publish_live_objects(...)` |

The MSFTS example follows the local text draft in `examples/msfts-publisher/docs/`, discovers PAT/PMT data, selects one program, filters unrelated PIDs, and emits an MSF version 1 catalog with `packaging: "m2ts"`.

```bash
./build/examples/msfts-publisher/openmoq-publisher-msfts-example \
  --input sample.m2ts \
  --endpoint https://relay.example.com:443/moq \
  --namespace media.msfts \
  --track transport \
  --draft 17
```

Add `--program NUMBER` to select a program, `--packets-per-object COUNT` to change object sizing, or `--insecure` only when the relay certificate is intentionally untrusted.

## Documentation

| Topic | Link |
| --- | --- |
| Build and dependencies | [docs/build.md](docs/build.md) |
| CLI and live-ingest quick start | [docs/quickstart.md](docs/quickstart.md) |
| Testing | [docs/testing.md](docs/testing.md) |
| Design overview | [docs/design.md](docs/design.md) |
| FFmpeg input recipes | [docs/ffmpeg.md](docs/ffmpeg.md) |
| SRT ingest technical note | [docs/srt-ingest-technical-note.md](docs/srt-ingest-technical-note.md) |
| Relay interoperability | [docs/relay-interop.md](docs/relay-interop.md) |
| C++ Publisher API | [docs/publisher-api.md](docs/publisher-api.md) |
| CAT4MOQ auth example | [examples/auth/README.md](examples/auth/README.md) |
| MSFTS text draft | [examples/msfts-publisher/docs/draft-gregoire-moq-msfts.txt](examples/msfts-publisher/docs/draft-gregoire-moq-msfts.txt) |
| Protocol mapping | [docs/protocol-mapping.md](docs/protocol-mapping.md) |
| WebTransport compliance | [docs/webtransport-compliance.md](docs/webtransport-compliance.md) |
| Local MSF version 1 draft | [docs/draft-ietf-moq-msf-01.txt](docs/draft-ietf-moq-msf-01.txt) |
| Local CMSF version 1 draft | [docs/draft-ietf-moq-cmsf-01.txt](docs/draft-ietf-moq-cmsf-01.txt) |
| macOS DASH shutdown behavior | [docs/macos-accept-shutdown-quirk.txt](docs/macos-accept-shutdown-quirk.txt) |
| Project status and roadmap | [docs/status.md](docs/status.md) |

Localized Publisher API guides are available in [Spanish](docs/publisher-api.es.md), [French](docs/publisher-api.fr.md), [Italian](docs/publisher-api.it.md), [Japanese](docs/publisher-api.ja.md), [Portuguese](docs/publisher-api.pt.md), and [Chinese](docs/publisher-api.zh.md).

## Repository Layout

- `include/openmoq/publisher`: public C++ headers
- `src`: static library and CLI implementation
- `tests`: CTest-based unit and integration coverage
- `docs`: local draft text, protocol notes, integration guides, and design references
- `examples`: Publisher API integrations
- `.github/workflows/ci.yml`: Linux, macOS, and Windows CI
- `.github/workflows/release.yml`: CLI, header, and static-library release artifacts

## Current Status

The main `openmoq-publisher` CLI accepts drafts 16 and 18; draft 16 remains the default while draft 18 provides the newer request-stream profile. Text for drafts 14, 17, and 19 remains in `docs/` for implementation history and protocol review, but those versions are not selectable in the main CLI. The separate MSFTS example retains draft 14/16/17/18 selection for draft-specific testing.

The default picoquic backend and the opt-in moq5 backend are both under active interoperability testing. For detailed feature coverage, limitations, and roadmap work, see [docs/status.md](docs/status.md) and [docs/protocol-mapping.md](docs/protocol-mapping.md).
