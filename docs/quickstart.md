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

Try the draft-16 compatibility profile:

```bash
./build/openmoq-publisher --input sample.mp4 --draft 16 --dump-plan
```

## Emit Objects to Disk

```bash
./build/openmoq-publisher --input sample.mp4 --emit-dir out/
```

Emit the same plan with SAP metadata enabled:

```bash
./build/openmoq-publisher --input sample.mp4 --draft 14 --sap --emit-dir out/
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
  -f mp4 - | ./build/openmoq-publisher --input - --draft 14 --dump-plan
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

## Output Notes

- default output includes the `catalog` object plus media objects
- `--msf-timeline` additionally creates a `timeline` media timeline track and metadata object
- `--sap` additionally creates `*_sap` metadata tracks and objects
- default packaging emits lower-latency split MOQT objects per group when chunk/sample boundaries are available
- `--coalesce-cmaf-chunks` restores one media object per group
- draft-14 defaults to ALPN `moq-00`
- draft-16 defaults to ALPN `moqt-16`
- draft-18 defaults to ALPN `moqt-18`
- `--alpn` overrides the draft default when targeting a specific relay
