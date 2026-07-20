# Testing

## Packaging and Session Tests

For routine development, run the packaging, CLI, and transport tests from the default build directory:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

This covers:

- fragmented MP4 packaging
- progressive MP4 remux into CMAF-style objects
- CLI option parsing and validation
- MOQT setup and control-message encoding/decoding for drafts 14, 16, 17, and 18
- binary namespace announcement plus subscribe-serving or forward-publish control/object sequencing
- draft-18 fragmented request-stream reads and same-stream `SUBSCRIBE_OK` responses
- draft-16 LL-DASH await-subscribe behavior with catalog plus multiple FFmpeg-style representation paths
- multitrack subscribe serving in publish-plan/media-time order rather than draining one track at a time
- paced send scheduling against fragment media timestamps
- QUIC varint boundary coverage

Publish-plan numbering notes:

- `group_id` is allocated per track, not across all tracks, so interleaved audio and video fragments can both use `0, 1, 2, ...`
- by default, `object_id` advances within a group when CMAF content is split into multiple MOQT objects for lower latency
- `--coalesce-cmaf-chunks` forces `object_id = 0` for the current one-object-per-group fallback
- MSF media timeline tracks are disabled by default; add `--msf-timeline` when you want a `timeline` metadata track and object
- SAP event timeline tracks are disabled by default; add `--sap` when you want catalog and metadata objects for `*_sap` tracks

If you want the packaging and transport tests without the CLI target, use the secondary build tree:

```bash
cmake -S . -B build-nosmoke -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build-nosmoke
ctest --test-dir build-nosmoke --output-on-failure
```

## Picoquic Loopback Smoke Test

When you want to exercise the live QUIC path locally, enable the smoke test target:

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=ON
cmake --build build --target openmoq-publisher-picoquic-smoke-tests
./build/openmoq-publisher-picoquic-smoke-tests
```

For transport debugging, run it with tracing enabled:

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher-picoquic-smoke-tests
```

The smoke test requires an environment that permits real UDP sockets. Keep it disabled for routine packaging or session work, and run the smoke binary directly when validating transport changes.

## CLI Dry-Run Testing

Input source notes:

- `--input <path>` reads an MP4 from a regular file
- `--input -` reads the MP4 byte stream from standard input
- fragmented and progressive MP4 inputs are both supported through either source type
- MSF media timeline tracks are disabled by default; add `--msf-timeline` to include the `timeline` metadata track and object in the publish plan
- SAP event timeline tracks are disabled by default; add `--sap` to include `*_sap` metadata tracks and objects in the publish plan

Use `--dump-plan` to inspect the generated publish plan without touching the network:

```bash
./build/openmoq-publisher --input sample.mp4 --draft 14 --dump-plan
```

Include SAP event timeline output explicitly:

```bash
./build/openmoq-publisher --input sample.mp4 --draft 14 --sap --dump-plan
```

Include MSF media timeline output explicitly:

```bash
./build/openmoq-publisher --input sample.mp4 --draft 14 --msf-timeline --dump-plan
```

Use stdin when the source is already being produced by another command:

```bash
cat sample.mp4 | ./build/openmoq-publisher --input - --draft 14 --dump-plan
```

## CTE DASH Ingest Dry-Run Testing

`--dump-plan` also works with `--live-source dash`, replacing the otherwise required `--endpoint`. The publisher starts the chunked-transfer ingest listener, waits for tracks, and prints the live plan (track names followed by one line per live object) instead of publishing to a relay:

```bash
./build/openmoq-publisher \
  --live-source dash \
  --dash-listen 127.0.0.1:8099 \
  --dash-path /ingest \
  --draft 18 \
  --publish-catalog \
  --dump-plan
```

Push CMAF segments with HTTP/1.1 chunked transfer encoding (curl or the FFmpeg DASH recipe in `docs/quickstart.md`) at `http://127.0.0.1:8099/ingest/...`. Like a live publish, the dry run keeps draining objects until it is interrupted; wrap it in `timeout` for scripted checks.

Run the focused DASH ingest and subscriber-interest regressions with:

```bash
cmake --build build --target openmoq-publisher-live-dash-tests openmoq-publisher-transport-tests
./build/openmoq-publisher-live-dash-tests
./build/openmoq-publisher-transport-tests
```

The DASH test covers separate init and media requests on `/ingest/video0`, `/ingest/video1`, and `/ingest/video2`. The transport test verifies that a draft-16 live publisher advertises those tracks, waits for `SUBSCRIBE`, acknowledges it, and sends only the catalog plus the subscribed representation.
