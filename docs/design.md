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
