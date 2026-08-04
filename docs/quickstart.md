# Quick Start

If you already have a sample MP4 and want to see what the publisher does, these are the most useful first commands.

On Windows, replace `./build/openmoq-publisher` with `build\Release\openmoq-publisher.exe`. For trace-enabled examples, use `set OPENMOQ_PICOQUIC_TRACE=1` in `cmd.exe` or `$env:OPENMOQ_PICOQUIC_TRACE=1` in PowerShell instead of the shell prefix form.

## Inspect a Publish Plan

```bash
./build/openmoq-publisher --input sample.mp4 --dump-plan
```

Inspect the same input with SAP event timeline metadata enabled:

```bash
./build/openmoq-publisher --input sample.mp4 --sap --dump-plan
```

Inspect the same input with an MSF media timeline track enabled:

```bash
./build/openmoq-publisher --input sample.mp4 --msf-timeline --dump-plan
```

Explicitly select the default draft-16 profile:

```bash
./build/openmoq-publisher --input sample.mp4 --draft 16 --dump-plan
```

## Emit Objects to Disk

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

Emit the same plan with SAP metadata enabled:

```bash
./build/openmoq-publisher --input sample.mp4 --draft 16 --sap --emit-dir out/
```

The output directory should contain:

- `catalog.json`
- one `*_init.mp4` file per media track
- one `*_media.mp4` file per emitted media object
- one `*_probe.mp4` file per emitted media object for direct `ffprobe` use
- `publish-plan.txt`

When `--msf-timeline` is enabled, the output directory also contains `timeline_g0_o0.json` with explicit MSF media timeline records. When `--sap` is enabled, the output directory also contains one `*_sap_g*_o*.json` file per emitted SAP event timeline object.

## Use Standard Input

Stream input over stdin instead of reading it from a file path:

```bash
cat sample.mp4 | ./build/openmoq-publisher --input - --dump-plan
```

Inspect an ffmpeg-produced fragmented stream without writing an intermediate file:

```bash
ffmpeg -i input.mp4 \
  -map 0:v -map 0:a \
  -c:v copy \
  -c:a copy \
  -movflags +frag_keyframe+empty_moov+default_base_moof+separate_moof \
  -f mp4 - | ./build/openmoq-publisher --input - --draft 16 --dump-plan
```

## Publish to a Relay

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://relay.example.com:443/moq \
  --namespace interop \
  --forward 0 \
  --timeout 10 \
  --paced
```

Transport-oriented CLI flags:

```bash
./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint localhost:4433 \
  --namespace media \
  --forward 0 \
  --timeout 3 \
  --paced \
  --insecure
```

The same CLI accepts stdin for transport publishing:

```bash
cat sample.mp4 | ./build/openmoq-publisher \
  --input - \
  --endpoint localhost:4433 \
  --namespace media \
  --forward 0 \
  --timeout 3 \
  --paced \
  --insecure
```

## SRT Live Ingest

The SRT ingest path receives MPEG-TS from one or more configured SRT listeners, discovers the selected MPEG-TS program, converts the elementary streams to CMAF objects, and publishes them to the relay. The publisher supports SRT `caller` mode, so each configured host and port must already be listening.

Create `/tmp/srt_callers.json`:

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

In the first terminal, start an FFmpeg listener that sends MPEG-TS when the SRT caller connects:

```bash
ffmpeg -hide_banner -stream_loop -1 -re \
  -i input.mp4 \
  -map 0:v:0 -map 0:a:0 \
  -c:v libx264 -preset veryfast -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
  -c:a aac -b:a 160k -ar 48000 -ac 2 \
  -f mpegts "srt://0.0.0.0:9000?mode=listener&pkt_size=1316"
```

In the second terminal, start the publisher:

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

The `mpegts` object can either auto-detect the first program or pin `program_number`, `video_pid`, and `audio_pid`. The `cmaf` object controls keyframe fragmentation and generated CMAF layout. Use `--forward 1` for an immediate relay smoke test, or keep `--forward 0` to wait for subscriber interest. SRT ingest requires a build in which libsrt was detected; see [build.md](build.md) and [srt-ingest-technical-note.md](srt-ingest-technical-note.md).

## CTE LL-DASH Live Ingest

Start an HTTP/1.1 chunked CMAF ingest endpoint and publish the resulting live objects to a MoQ relay:

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

Each concurrent path below `--dash-path` is accepted independently:

```bash
curl -X PUT \
  -H 'Transfer-Encoding: chunked' \
  -H 'Content-Type: video/iso.segment' \
  --data-binary @live-video.cmaf \
  http://127.0.0.1:8080/ingest/video
```

The server accepts `PUT` or `POST` requests with HTTP/1.1 chunked transfer encoding. Every path under the prefix keeps its own MP4 parser state, so independent representations can send init segments and media fragments on separate request paths. Track names in the MoQ catalog are prefixed with the final path component; for example, `/ingest/video0` becomes track names such as `video0_vide_1`.

FFmpeg DASH output can push directly to the ingest endpoint:

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

For FFmpeg DASH input, the ingest adapter expands `tfhd` sample defaults into
the outgoing `trun` entries before publishing CMAF objects. Video keyframes
start shared media groups, and audio objects join the active video group;
media received before the first video keyframe is discarded. This keeps the
representations independently addressable while preserving aligned render
groups for downstream playback.

Useful DASH ingest flags:

- `--dash-listen <host:port>` chooses the local HTTP listener address
- `--dash-path <prefix>` limits accepted request paths to that prefix
- `--dash-queue-depth <count>` controls how many pending live objects are retained when publishers run ahead of subscribers
- `--publish-catalog` publishes the CMSF catalog track
- `--forward 1` forwards media objects immediately after publishing to the relay
- `--forward 0` waits for subscriber interest before media is sent

When `--forward 0` is used, `connection_id=` confirms that the transport connection and MOQT setup completed. It is printed before namespace acceptance and does not indicate subscriber interest. The publisher completes the draft-specific namespace and track signaling, then withholds media until the relay forwards a subscription for a catalog or media track. Draft 16 carries that interest on the control stream; draft 18 uses a bidirectional request stream, which may deliver the request in multiple reads. The publisher reassembles a draft-18 request and sends `SUBSCRIBE_OK` on the same request stream.

`--timeout` bounds the initial wait for subscriber interest. Reaching the timeout with no subscription is an idle, successful exit rather than a transport failure. Use `--forward 1` for a smoke test that should send objects without waiting for a subscriber. Set `OPENMOQ_PICOQUIC_TRACE=1` when diagnosing whether the relay accepted the namespace and delivered the subscriber request.

The DASH ingest listener is currently supported on Unix-like platforms. Windows builds compile the CLI but report DASH ingest server startup as unsupported. The listener and live stdin reader use bounded polling so shutdown does not hang on idle input; see [macos-accept-shutdown-quirk.txt](macos-accept-shutdown-quirk.txt) for the macOS listener details.

## CAT4MOQ Auth Example

Build the CAT4MOQ auth example when testing a relay that requires MoQ authorization tokens:

```bash
cmake --build build --target openmoq-publisher-auth-example
```

Run it with a token file:

```bash
CAT4MOQ_TOKEN_FILE=/tmp/publish-token.cwt \
CAT4MOQ_ENDPOINT='https://127.0.0.1:4433/moq-relay' \
./examples/auth/run-cat4moq-auth-example.sh
```

Or run it with moqx as the Catapult/CAT4MOQ issuer command:

```bash
CATAPULT_CAT4MOQ_COMMAND='../moqx/build/moqx issue-cat-token --config /tmp/moqx-auth.yaml --auth-service live --auth-key-id cat-dev --auth-actions client_setup,publish_namespace,publish --auth-namespace {namespace} --auth-track {track}' \
CAT4MOQ_ENDPOINT='https://127.0.0.1:4433/moq-relay' \
./examples/auth/run-cat4moq-auth-example.sh
```

See [examples/auth/README.md](../examples/auth/README.md) for the moqx auth
config, token generation, relay connection, and focused-test workflow.

## Output Notes

- default output includes the `catalog` object plus media objects
- `--msf-timeline` additionally creates a `timeline` media timeline track and metadata object
- `--sap` additionally creates `*_sap` metadata tracks and objects
- default packaging emits lower-latency split MOQT objects per group when chunk/sample boundaries are available
- `--coalesce-cmaf-chunks` restores one media object per group
- draft-16 defaults to ALPN `moqt-16`
- draft-18 defaults to ALPN `moqt-18`
- drafts 14, 17, and 19 are retained as local text or implementation-history references but are not selectable in the main CLI
- `--alpn` overrides the draft default when targeting a specific relay
