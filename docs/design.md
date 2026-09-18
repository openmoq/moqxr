# Design Overview

## Fragmented MP4 Fast Path

For already fragmented input:

- `ftyp` + `moov` are reused as the initialization segment
- by default, each fragmented input is split into lower-latency MOQT media objects within the same group when per-sample boundaries can be derived
- `--coalesce-cmaf-chunks` keeps the older one-media-object-per-group behavior
- the pipeline preserves source-byte spans until optional file emission

## Progressive MP4 Remux Path

For non-fragmented input:

- the tool reads `stbl` tables such as `stsz`, `stsc`, `stco` or `co64`, `stts`, and optional `ctts` and `stss`
- it synthesizes a fragmented initialization segment by adding `mvex` and `trex`
- it builds synthetic `moof` + `mdat` payloads from the original sample data
- by default, progressive remux output is split into multiple MOQT objects per group for lower latency

This keeps the project aligned with CMAF-style publication while reusing the same publish-plan model for local inspection and transport-driven publication.

## Optional LOCMAF Output

`MediaPackaging::kCmaf` remains the default. `MediaPackaging::kLocmaf` adds a
conversion stage between CMAF chunk preparation and publication. The batch
path in `src/locmaf_packager.cpp` retains original fragmented chunks and their
auxiliary boxes, preflights each complete track, and then replaces its media
payloads and catalog packaging together. Progressive input is remuxed first;
the LOCMAF path clears the old progressive sample tables in the generated
initialization so they cannot reference stale offsets.

`LocmafEncoder` in `src/locmaf_encoder.cpp` owns per-track encoding state.
Live stdin and SRT sessions and DASH ingest create encoders from their
per-track initialization segments. Production conversion forces full headers
on every object, allowing queue trimming and arbitrary cached-object access
without earlier delta state. The standalone encoder supports deltas, but
publishing them is deferred until recovery and cache behavior are validated.

The default CMAF splitter remains the primary path. LOCMAF constraints,
raw-box handling, and track fallback are documented in
[protocol-mapping.md](protocol-mapping.md#locmaf-packaging).

## LOC Publishing

Implementation branch: `loc`, forked from `locmaf` at
`9d752926576bc46bc5f12ee917f52963cdf69c53` (LOCMAF PR #36).
CMAF remains the primary default; LOCMAF remains a separate opt-in format.

### Draft baseline and compatibility

Use the checked-in [LOC-04 text](draft-ietf-moq-loc-04.txt), published July 20,
2026. IETF Datatracker reported revision `04` as current on September 18,
2026. Download source:
<https://www.ietf.org/archive/id/draft-ietf-moq-loc-04.txt>.
SHA-256: `fb29e2805be0511a188683b60fc830fb7fd3ecf19931968755d60d83707c3b47`.
Use [MSF-01](draft-ietf-moq-msf-01.txt) section 4.1 for catalog signaling and
sample/group mapping. LOC-04 references MOQT-19; the initial implementation
binds its clear-media subset to the existing draft-18 property framing,
with explicit wire tests, rather than imply new MOQT-19 support.

LOC is encoded audio/video frames plus MOQT properties, not a compressed
CMAF chunk. Each encoded sample becomes one object; MP4 box headers are not
part of its media payload. The catalog must say `packaging: "loc"` and must
not include `locmafVersion`. Do not invent a `locVersion` catalog field.

LOC-04 sections 2.3 and 6.1 define:

| Property | ID | Value |
| --- | --- | --- |
| Timestamp | `0x10` | vi64 integer |
| Timescale | `0x08` | vi64 integer |
| Video frame marking | `0x09` | Length-prefixed RFC 9626 bytes |
| Audio level | `0x0C` | vi64 integer containing RFC 6464 bits |
| Video config | `0x0D` | Codec extradata bytes |
| Audio config | `0x0F` | Codec extradata bytes |

Timescale must accompany media-relative timestamps; without it, LOC-04
interprets timestamps as Unix-epoch microseconds. Preserve presentation time
for metadata and decode order for object ordering. Do not substitute decode
time for presentation time on B-frames or label relative time as wall-clock.

Current reference limitations, verified from local sources:

- Playa `2d58206`, `packages/loc/src/property-map.ts`, resolves LOC-01 IDs
  `0x02`, `0x04`, and `0x06`; it is not a LOC-04 semantic oracle. Its transport
  property-block codec is a useful independent framing reference.
- red5-moq-relay `852f9fa`, `org/red5/io/moq/loc/model`, has the same older
  timestamp/frame-marking/audio-level IDs. Opaque property forwarding may
  still work, but must be proved through cached delivery and FETCH.
- The local moq5 `media/loc/src/loc.c` implements LOC-01. Keep libmoq LOC-04
  publishing gated until its profile is implemented and verified.
- The user's publisher is available at `../moq-rs/moq-pub` rather than
  `../moq-pub`; its media catalog currently selects CMAF. Use it as a baseline
  publisher reference, not as proof of LOC-04 compatibility.

### LOC-04 implementation scope

Add explicit `--packaging loc` / `MediaPackaging::kLoc`. Start with clear
H.264 and AAC using the native draft-18 session, then add other codecs only
with verified WebCodecs payload/config mappings. Preserve both existing
packaging modes and their default wire bytes. Reject LOC on unsupported
transport/backend combinations before publication; never silently send the
old LOC-01 property IDs under the LOC-04 implementation.

Cover batch MP4, live stdin, SRT, DASH, and caller-supplied encoded samples
through the same sample-to-object mapping. For the first implementation use
single-layer video, one ordered subgroup per GOP, and per-sample audio
objects. Audio uses one sample per group for both audio-only and A/V publication;
no alternate-track group alignment is advertised.

Defer scalable-layer subgroups, datagrams, Secure Objects encryption/private
properties, and automatic LOC-01 interoperability mode. Reject encrypted
CMAF input for LOC; CENC auxiliary data is not Secure Objects encryption.
Audio level is optional and must be omitted unless actually supplied or
measured. Keep codec configuration available at random-access entry points
so cached groups and late joins can initialize a decoder.

### Initial behavior contract

- `--packaging loc --draft 18` selects LOC-04. Omitted packaging still means
  CMAF. `--packaging locmaf` keeps its existing behavior. Reject LOC with
  draft 16 or libmoq before opening a publishing session. Do not infer LOC
  revision from the transport draft or retry using legacy LOC property IDs.
- Represent common properties as
  `ObjectProperty { uint64_t id; variant<uint64_t, vector<uint8_t>> value; }`.
  Append `vector<ObjectProperty> properties` to public object structs;
  default-empty preserves existing callers. Sort IDs for serialization,
  reject duplicate known LOC fields and parity mismatches, and cap each
  odd-ID value at 65535 bytes per MOQT-18 section 1.4.3. Bound the complete
  block to 64 KiB as a publisher resource policy, not a draft requirement.
- Encode property ID deltas from the previous ID (initially zero), with
  no extra `+1`. Object ID deltas have different rules. Determine value
  parity from the reconstructed absolute property ID, not its delta.
- Set subgroup PROPERTIES bit `0x01` when opening a LOC stream. For each
  object encode Object ID Delta, Properties Length, Properties, Payload
  Length, optional zero-payload Status, then Payload. Do not introduce
  properties into an already-open stream whose header omitted that bit.
- Emit Timestamp and Timescale on every media object. Use sample PTS in
  the source timescale, with checked conversions for scheduling. Preserve DTS for sending order. Omit optional
  audio levels and discardability claims when unavailable from the source.
- Use length-prefixed AVC samples and their `avcC` record; use raw AAC
  access units and AudioSpecificConfig. `TrackDescription::codec_private`
  currently contains entire configuration boxes: strip/parse the box and
  MPEG-4 descriptor wrappers before generating LOC Config properties.
  Reject unsupported sample descriptions and payload layouts explicitly.
- Repeat matching Video Config or Audio Config on every generated media
  object. Reject configuration changes in this initial profile.
  Never retain a dropped dependency chain and resume video on a delta frame.
- Reject `--coalesce-cmaf-chunks` and `--stream-per-object` for this initial
  GOP-stream profile. One video subgroup per GOP is an implementation
  choice, not a LOC-04 prohibition on layered subgroups.
- Emit `*_media.loc` containing the elementary sample and matching
  `*_properties.json` sidecars containing IDs, typed values, and complete
  encoded property-block hex. Integer values in JSON are decimal strings
  to preserve uint64 precision. These are inspection artifacts, not a
  standardized standalone LOC file format.

A minimal draft-18 golden vector for the property writer uses Timescale
1000 (`0x08`) and Timestamp 42 (`0x10`), with payload `aa bb`:

```text
Properties (5 bytes):       08 83 e8 08 2a
First object, ID 0:         00 05 08 83 e8 08 2a 02 aa bb
Header, alias/group 0:      31 00 00   (subgroup zero, no end-of-group bit)
Unchanged no-property obj:  00 02 aa bb
```

The wire test must decode the resulting stream with an independent parser
and assert both properties and unchanged payload. Add odd-ID Config and
Frame Marking vectors separately; the LOC-01 integer frame-marking encoding
must never be emitted as LOC-04.

### Implementation map

- `object_properties.h` and `moqt_control_messages.cpp`: typed properties,
  draft-18 serialization, limits, and fixed subgroup framing.
- `encoded_sample.h` / `encoded_sample.cpp`: lossless sample extraction,
  initialization validation shared by file/live ingest, configuration parsing,
  progressive and fragmented timing. Mixed initial samples plus fragments fail
  explicitly. In-band AVC parameter sets and layered NALs are rejected, except
  the SRT adapter can strip byte-identical copies of advertised SPS/PPS.
- `loc_packager.cpp`: sample-to-object mapping, GOP recovery, source PTS,
  configuration replay, and batch plan preparation.
- `cmsf_packager.cpp`: LOC catalog initialization and inspection files.
- `moqt_session.cpp` / `live_dash_ingest.cpp`: property ownership, native
  publishing, live sample conversion, and whole-group queue recovery.
  `live_srt_ingest.cpp` retains source PES PTS/DTS independently of existing
  CMAF timing; the LOC converter uses those clocks and decode lookahead.
- `scripts/test-loc04.mjs`: independent property decoding and FFmpeg decoded-frame
  equivalence. It uses LOC-04 IDs directly and does not depend on Playa's LOC-01
  semantic decoder.

The acceptance gates below retain the original implementation contract.
Offline validation is recorded in [testing](testing.md#loc-04-tests); real
relay forwarding, cached FETCH, and LOC-04 browser playback remain separate
interoperability gates, not conclusions from a successful build.

### Delivery milestones and acceptance gates

1. **Wire foundation:** property model and draft-18 serialization tests pass;
   existing CMAF and LOCMAF wire vectors remain identical. No CLI LOC mode
   is exposed before a complete sample-to-object path exists.
2. **Batch publishing:** progressive and fragmented H.264/AAC inputs produce
   valid LOC catalogs, one object per sample, and matching inspection
   sidecars. Decode reconstructed samples independently and compare packet
   payload hashes, presentation timing, A/V offset, and frame hashes with
   the source. Compare duration at the sample-extraction boundary; LOC-04
   defines no duration property, so do not invent one on the wire. Include B-frames and multiple samples per chunk.
3. **Live publishing:** stdin, SRT, DASH, and explicit sample-source tests
   prove catalog/config ordering, bounded queues, whole-object drops,
   keyframe recovery, stable object identity on replay, and clean rejection
   of encrypted or unsupported input. A configuration failure must not
   publish a LOC catalog followed by CMAF payloads.
4. **LOC-04 interoperability:** a receiver understands IDs `0x08`, `0x09`,
   `0x0C`, `0x0D`, `0x0F`, and `0x10` with their correct types. Verify real
   relay delivery, late join at a GOP boundary, and relay-served FETCH with
   exact payload/property equality. Record publisher, relay, receiver, and
   transport revisions. Until Playa is upgraded, use an independent LOC-04
   test receiver and report Playa playback as unverified.
5. **Regression and handoff:** full CTest suite, LOCMAF vectors and Playa
   reconstruction, CLI/API negative cases, parser sanitizer checks, and
   documentation examples pass. Review the PR against `locmaf` while PR #36
   is open, or retarget it to `main` after that dependency merges. Do not
   include an implicit transport upgrade or unrelated sibling changes.

Focused verification commands:

```bash
cmake --build build
ctest --test-dir build --output-on-failure \
  -R 'openmoq-publisher-(encoded-sample|loc-publisher|control-message|transport|live-dash|msf-catalog|cli)-tests'
ctest --test-dir build --output-on-failure
bun scripts/test-loc04.mjs build/openmoq-publisher
bun scripts/test-locmaf-playa.mjs build/openmoq-publisher ../moq-playa
```

Each task starts with a failing regression and ends with its focused tests.
Register the new test executables in `CMakeLists.txt`. Run the full existing
suite and LOCMAF golden/Playa checks before calling the LOC addition ready;
CMAF and LOCMAF byte-output comparisons are explicit compatibility gates.
Network setup alone is not delivery proof, and successful old-LOC playback
is not LOC-04 validation.

## Catalog Metadata

The catalog format includes:

- `role` with values such as `video` and `audio`
- RFC 6381 `codec` strings such as `avc1.64000C`, `mp4a.40.2`, and HEVC values like `hvc1.1.6.L90.B0`
- `renderGroup` and `isLive`
- MSF media timeline tracks with `packaging: "mediatimeline"` and `role: "mediatimeline"`
- SAP event timeline tracks with `packaging: "eventtimeline"` and `eventType: "org.ietf.moq.cmsf.sap"`
- `width` and `height` for video tracks
- `sampleRate` and `channelCount` for audio tracks
- base64-encoded per-track CMAF initialization segment (`ftyp` + `moov`) in `initData`

With `--msf-timeline`, `catalog.json` also includes a `timeline` metadata track with:

- `packaging: "mediatimeline"`
- `mimeType: "application/json"`
- `depends` pointing to all media tracks

The corresponding `timeline_g0_o0.json` payload is an explicit MSF media timeline array of `[mediaTimeMs, [groupId, objectId], wallclockMs]` records. VOD output uses `0` for wallclock time.

With `--sap`, `catalog.json` also includes:

- one `*_sap` track per media track
- `packaging: "eventtimeline"`
- `eventType: "org.ietf.moq.cmsf.sap"`
- `depends` pointing back to the corresponding media track

`publish-plan.txt` and `--dump-plan` still print an internal debug `kind=` label for object type (`catalog`, `metadata`, or `media`); that debug label is not part of the catalog spec.

## HEVC Behavior

- compact HEVC RFC 6381 codec strings are derived from the track `hvcC` box
- `hev1` tracks that do not carry in-band VPS, SPS, or PPS NAL units are normalized to `hvc1`
- when in-band HEVC parameter sets are present, the publisher preserves `hev1`
- emitted initialization segments are rewritten to match the normalized sample entry type, so catalog metadata and init segments stay aligned

## Draft Handling

- `draft-ietf-moq-transport-14` is the primary target
- `draft-ietf-moq-transport-16` is represented as a secondary compatibility profile
- `draft-ietf-moq-transport-18` support is implemented for version selection, setup/request framing codec paths, and request-stream response correlation; interop hardening is still in progress
- draft-specific assumptions are documented in [protocol-mapping.md](protocol-mapping.md)

## Transport Notes

The repository includes a transport abstraction and a picoquic-backed client wrapper.

- if local picoquic and picotls source trees are available, CMake can compile the real picoquic transport path into this project
- if those dependencies are not available, the project still builds and tests normally, and the transport layer falls back cleanly
- the session layer uses a draft-aware control-message module instead of ad hoc string formatting
- the optional loopback smoke test is the intended local validation path for real QUIC transport changes
