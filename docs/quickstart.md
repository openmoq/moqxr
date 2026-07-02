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
- draft-14 defaults to ALPN `moq-00`
- draft-16 defaults to ALPN `moqt-16`
- draft-18 defaults to ALPN `moqt-18`
- `--alpn` overrides the draft default when targeting a specific relay
