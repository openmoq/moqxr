# MSF and CMSF Version 1 Support

Design for aligning the moqxr publisher with `draft-ietf-moq-msf-01` and
`draft-ietf-moq-cmsf-01` (both stored under `docs/`).

- Status: approved design, pending implementation plan
- Date: 2026-07-28

## Problem

The catalog moqxr publishes today predates both drafts and is not a valid MSF
version 1 catalog. It is generated independently in four places, each with its
own JSON escaping and field set:

| Emitter | Location |
| --- | --- |
| Batch publish plan | `src/cmsf_packager.cpp:544` |
| Live catalog (SRT, stdin, libmoq) | `src/cmsf_packager.cpp:754` |
| CTE LL-DASH ingest | `src/live_dash_ingest.cpp:531` |
| MSFTS example | `examples/msfts-publisher/msfts_source.cpp:430` |

### Root-level defects

- `"version": 1` is a Number. MSF section 5.1.1 requires a String.
- `"format": "cmsf"` is not a field in either draft. Format identity comes from
  the URL fragment type and the per-track `packaging` value.
- No `generatedAt`, `isComplete`, `deltaUpdate`, or `publishTracks`.
- No `initDataList`. Initialization data is inlined per track as `initData`.
  CMSF section 3.1 requires a root `initDataList` referenced by a per-track
  `initRef`, and MSF section 5.1.7 requires that array to follow `tracks`.

### Track-level defects

Emitted name versus required name: `frameRate` versus `framerate`, `sampleRate`
versus `samplerate`, `channelCount` (Number) versus `channelConfig` (String),
`initData` versus `initRef`. A non-spec `id` field is emitted. The required
`bitrate` (section 5.2.22, a MUST for audio and video) is absent, as are
`altGroup`, `maxGopDuration`, `maxGroupDuration`, `timescale`, `lang`,
`trackDuration`, and `targetLatency`/`buffers`. `renderGroup` is hardcoded to 1
for every track, including timeline tracks. CMSF's `maxGrpSapStartingType` and
`maxObjSapStartingType` (section 3.5.2) are missing entirely.

### What already conforms

The packaging layer is in good shape and this work does not disturb it:

- CMAF packaging, per-track single-`trak` init segments, `moof`+`mdat` object
  payloads, and fragment-to-group mapping satisfy CMSF sections 3.3 and 3.4.
- The SAP-type event timeline track uses the correct `eventType` of
  `org.ietf.moq.cmsf.sap` and the correct `{"l":[g,o],"data":[sap,ept_ms]}`
  record shape (`src/cmsf_packager.cpp:198`), matching CMSF section 3.6.1.
- The media timeline track uses the correct `[mediaTimeMs, [g,o], wallclock]`
  triple (`src/cmsf_packager.cpp:222`) with `mimeType: application/json` and a
  `depends` array, satisfying MSF section 7.1.1. Wallclock is hardcoded to 0,
  which the spec permits only for VOD or unknown times; live paths should
  populate it.
- Object numbering follows section 6.2, and each object maps to its own stream
  per section 6.

## Decisions

1. **Clean break.** The legacy catalog shape is replaced outright. No dual
   emission and no compatibility flag. Downstream consumers are updated in
   lockstep.
2. **All four phases are specified here**, in one roadmap document. Each phase
   gets its own implementation plan.
3. **MSF_COMPRESSION is out of scope.** Section 12 rides on MOQT Track and
   Object Properties, which exist only in transport draft-19; `moq_draft.h`
   tops out at `kDraft18`. Omitting the property is spec-legal and means
   uncompressed. This is a documented dependency, not a gap to paper over.
4. **Bitrate is sourced from `btrt`, then computed, then overridden.**
5. **Catalog version string is `"1"`.** Both drafts state "version 1" and every
   example uses `"1"`. Section 5.1.1's `draft-XX` convention is available via
   config for interop testing against draft-tracking peers.

## Phases

The four scope areas are independent subsystems. Phase 1 is a hard gate:
Phases 2 and 3 both write into the catalog model it establishes, and it is
where the breaking change lands. Phase 4 touches nothing else and may be built
at any time, including in parallel.

```
Phase 1 (catalog model) ──┬── Phase 2 (lifecycle)
                          └── Phase 3 (content protection)

Phase 4 (URL parsing) ── independent
```

## Phase 1: Unified MSF v1 catalog model

### Module boundary

New `include/openmoq/publisher/msf_catalog.h` and `src/msf_catalog.cpp`, owning
only the catalog document. No MP4 parsing and no transport concerns.

```
MsfCatalog {
  std::string version = "1";
  std::optional<uint64_t> generated_at_ms;
  std::optional<bool> is_complete;
  std::vector<MsfTrack> tracks;
  std::vector<MsfTrack> publish_tracks;                    // Phase 2
  std::vector<MsfContentProtection> content_protections;   // Phase 3
  std::vector<MsfInitData> init_data_list;                 // id / type / data
  std::vector<MsfDeltaOp> delta_update;                    // Phase 2
}
```

`MsfTrack` carries the spec field names verbatim: `name`, `namespace`,
`packaging`, `role`, `is_live`, `codec`, `bitrate`, `avg_bitrate`, `framerate`,
`timescale`, `samplerate`, `channel_config`, `width`, `height`, `lang`,
`init_ref`, `alt_group`, `render_group`, `depends`, `event_type`, `mime_type`,
`track_duration`, `max_gop_duration`, `max_group_duration`, plus CMSF's
`max_grp_sap_starting_type` and `max_obj_sap_starting_type`.

Optional fields are modeled with `std::optional` so the serializer never emits a
field the drafts say MUST NOT appear.

### Serialization

A small internal JSON writer, not a new dependency. The project vendors only
picoquic and picotls, and the existing code already hand-rolls JSON; a shared
writer also fixes the divergent escaping across the four current emitters (one
escapes control characters, the others do not).

The serializer enforces the drafts' structural rules in one place:

- `initDataList` is emitted after `tracks` (section 5.1.7).
- `eventType` appears only when `packaging` is `eventtimeline` (section 5.2.5).
- A delta catalog carries neither `tracks` nor `version` (section 5.3).
- `targetLatency` and `buffers` are mutually exclusive (sections 5.2.8, 5.2.9).
- `trackDuration` is absent when `isLive` is true (section 5.2.35).

### Call site migration

`cmsf_packager`, `live_dash_ingest`, `live_srt_ingest`, and the MSFTS example
each shrink to populating `MsfCatalog`. `build_live_catalog` keeps its signature
and continues to return `LiveCatalog`, so transport code is untouched.

### Track metadata extraction

`TrackDescription` gains `max_bitrate`, `avg_bitrate`, `language`,
`duration_ms`, and `alt_group`. Extraction extends the existing walk in
`extract_tracks` (`src/mp4_box.cpp:642`):

- **`btrt`**: read from the sample entry, a sibling of `avcC`, `hvcC`, or
  `esds`, giving `maxBitrate` and `avgBitrate` directly. The existing
  `find_child_box_offset(sample_entry, bytes, 8 + 28, ...)` helper already
  navigates past the sample entry header (see the `esds` lookup at
  `src/mp4_box.cpp:492`), so this is a small addition.
- **Computed fallback**: total sample bytes from `stsz` over track duration, for
  batch inputs lacking `btrt`.
- **Live override**: a per-track bitrate in `PublisherConfig`, because live
  catalogs are published before media flows. When neither `btrt` nor an override
  is available on a live path, emit a codec-class default (2 Mbps for video,
  128 kbps for audio) and log a warning naming the track, so the catalog stays
  conformant and the estimate is visible to the operator.
- **`lang`**: the packed 3x5-bit ISO-639-2 code in `mdhd`, immediately after the
  duration field the parser currently skips (`src/mp4_box.cpp:679`).

### CMSF additions

`packaging` is `cmaf` for all media tracks. `maxGrpSapStartingType` and
`maxObjSapStartingType` are computed from the SAP types the segmenter already
carries on each `MediaFragment` (`has_sap_type` and `sap_type` are already
threaded into `CmsfObject` at `src/cmsf_packager.cpp:580`). Tracks in a common
switching set carry a shared `altGroup`.

## Phase 2: Catalog track lifecycle

Depends on Phase 1.

A `CatalogPublisher` owns the group and object state of the catalog track and
implements MSF section 5:

- Object 0 of every group holds a full independent catalog.
- Objects with ID 1 and above hold `deltaUpdate` operations.
- Producing a new independent catalog forces a new group.
- All catalog objects map to subgroup 0.

Delta generation diffs the previous `MsfCatalog` against the current one and
emits `add` and `remove` operations. A `clone` is emitted only when a new track
matches an existing one on every field except `name`. Because the drafts freeze
a track's attributes once its namespace/name tuple is declared, any attribute
change is expressed as an add of a new track followed by a remove of the old.

End-of-broadcast (section 11.3) becomes an explicit `end_broadcast(mode)` call
emitting either the VOD conversion (`isLive: false` plus `trackDuration` on each
track) or the permanent termination (`isComplete: true` with an empty `tracks`
array), after `SUBSCRIBE_DONE` with status `0x2 Track Ended`.

This phase requires reading JSON, not just writing it, in order to diff a prior
catalog. A minimal reader is added here rather than in Phase 1.

Current behavior for reference: the catalog is sent once at group 0, object 0
and never republished (`src/transport/moqt_session.cpp:3099`).
`live_dash_ingest.cpp` already increments a group per emission and is closer to
the target.

## Phase 3: CMSF content protection

Depends on Phase 1. Corresponds to roadmap item 6 in `docs/status.md`.

Two separable pieces.

**MP4 layer.** Parse `sinf`, `schm`, `schi`, and `tenc` within `encv` and `enca`
sample entries to recover `default_KID` and the protection scheme (`cenc` or
`cbcs`), and parse top-level `pssh` boxes for per-system initialization data.
The existing `sample_entry_type` logic must learn that `encv` and `enca` wrap
the original format recorded in `frma`; without this, codec strings break on
protected content. The repository has no CENC box handling today.

**Catalog layer.** Emit the root `contentProtections` array, one entry per DRM
system ID discovered, and per-track `contentProtectionRefIDs`. Per CMSF section
4.1.1, protection information is never duplicated at track level; tracks only
reference root entries. Per section 4.2, the `initDataList` entry for a
protected track must retain its `sinf`/`schm`/`schi`/`tenc` boxes, which the
existing per-track init segment builder must be verified to preserve.

Encrypted samples pass through untouched. moqxr never decrypts.

## Phase 4: MSF URL and fragment parsing

Independent of Phases 1 through 3.

A standalone `msf_url` module parsing
`moqt://host/path?query#msf:ns--name&params` into a session endpoint plus a
track identifier. It implements:

- The namespace-name tuple encoding of section 11.1.2: tuple elements joined by
  a single hyphen, the track name appended after a double hyphen, and all bytes
  other than alphanumerics and underscore percent-encoded as a period followed
  by two lowercase hex digits.
- The reserved fragment parameters of section 11.1.1: `connection` (`q` or
  `wt`), `wallclock-range`, `mediatime-range`, `location-range`, and `c4m`.
  Repeated range parameters resolve to their union.
- Variable substitution (section 5.4): resolving `%name%` references in catalog
  field values from fragment key-value pairs, enforcing the spec's strict
  character allowlist on both names and values to block injection. Query
  parameters are reserved for the server and are never used for substitution.

Usable by both the publisher CLI and any future subscriber.

## Error handling

Catalog serialization failures are programming errors, not runtime conditions.
The builder validates its invariants and throws `std::runtime_error` naming the
offending track, matching how `cmsf_packager` already reports catalog generation
failures (`src/cmsf_packager.cpp:463`).

Validated invariants: `bitrate` present on audio and video tracks; `eventType`
only on event timeline tracks; `targetLatency` and `buffers` mutually exclusive;
`trackDuration` absent when live; every `initRef` resolving to an `initDataList`
entry; track names unique per namespace; `packaging` present on every track.

MP4 extraction failures degrade rather than throw. A missing `btrt` falls back
to computation, and a missing language falls back to omitting `lang`.

## Testing

Tests follow the existing single-binary `expect()` style under `tests/`.

A new `tests/msf_catalog_test.cpp` asserts field-by-field conformance against
catalogs derived from the drafts' own examples: CMSF section 5.1 (simulcast),
CMSF section 5.2 (DRM-protected), CMSF section 5.3 (ClearKey), and MSF sections
5.6.1 through 5.6.3. Negative cases cover each validated invariant.

Per-phase coverage:

- Phase 1: round-trip of each of the four emitters through the new model;
  `btrt` extraction, computed fallback, and override precedence; `lang`
  decoding; `initDataList` and `initRef` correspondence.
- Phase 2: group and object placement of independent versus delta catalogs;
  add, remove, and clone diff generation; both end-of-broadcast modes.
- Phase 3: `sinf`/`tenc`/`pssh` extraction from an encrypted fixture; `frma`
  unwrapping of codec strings; correct root-level protection entries with no
  track-level duplication.
- Phase 4: tuple encode and decode round-trips including period-hex escapes;
  each reserved parameter; range union; substitution allowlist rejection.

Existing assertions must be retargeted from the legacy shape to the v1 shape in
`tests/cmaf_segmenter_test.cpp`, `tests/live_dash_ingest_test.cpp`,
`tests/moqt_session_test.cpp:1292`, and
`examples/msfts-publisher/tests/msfts_source_test.cpp`. This is the visible cost
of the clean break.

## Out of scope

- MSF section 12 compression signaling, blocked on transport draft-19 Track and
  Object Properties.
- MSF sections 5.2.38 through 5.2.41, MoQ Secure Objects encryption fields.
  These are the LOC-packaged end-to-end encryption path; CMSF uses CENC instead,
  which Phase 3 covers.
- MSF sections 9 and 10, log and metrics publish tracks. The `publishTracks`
  field is modeled in Phase 1 but no log or metrics track is produced.
- LOC packaging. moqxr publishes CMAF; `packaging: "loc"` is not emitted.
- Transport draft-19 support generally.

## References

- `docs/draft-ietf-moq-msf-01.txt`
- `docs/draft-ietf-moq-cmsf-01.txt`
- `docs/draft-ietf-moq-transport-19.txt`
- `docs/status.md`, roadmap items 5 and 6
