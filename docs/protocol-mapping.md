# Protocol Mapping Notes

This project keeps `draft-ietf-moq-transport-14` as the primary publisher profile and treats `draft-ietf-moq-transport-16` as a secondary compatibility target.

## Draft-14 primary assumptions

- Namespace subscription responses are modeled as the dedicated `SUBSCRIBE_NAMESPACE_OK` and `SUBSCRIBE_NAMESPACE_ERROR` flow.
- Namespace overlap handling is documented against `NAMESPACE_PREFIX_OVERLAP`.
- Publisher-side namespace acceptance is modeled with draft-14 style `PUBLISH_NAMESPACE_OK` and `PUBLISH_NAMESPACE_ERROR`.
- Draft-14 control messages use a `u16` outer `Length` field, including `PUBLISH`, `PUBLISH_OK`, and `PUBLISH_ERROR`; only inner fields explicitly marked `(i)` remain QUIC varints.

## Draft-16 secondary assumptions

- Some request/response handling moved to the generic `REQUEST_OK` and `REQUEST_ERROR` flow.
- `SUBSCRIBE_NAMESPACE` uses a `u16` outer `Length` field and carries `Subscribe Options` before its message parameters. `Subscribe Options` affects whether namespace advertisements, publish advertisements, or both are requested.
- Message parameters in `SUBSCRIBE`, `PUBLISH_OK`, and related draft-16 messages are delta-encoded key-value pairs. Even parameter types carry a varint value directly; odd parameter types carry a varint length followed by bytes.
- `PUBLISH_OK` applies the draft-16 defaults when parameters are omitted: `FORWARD=1`, `SUBSCRIBER_PRIORITY=128`, no explicit `GROUP_ORDER`, and no subscription filter.
- Namespace overlap handling is expressed through the generic request error path rather than the older dedicated response message family.

## CMAF packaging assumptions

- Initialization data is represented either as a binary payload or as a dedicated MOQT track with one group and one object.
- Media objects follow the fast path of `styp? + moof + mdat`, with `moof`/`mdat` reused directly from fragmented MP4 input.
- The current implementation uses one media object per fragment/group, which aligns with the fragment-to-group mapping in the current MOQT CMAF packaging draft.

## MSF v1 catalog

- The catalog is a `draft-ietf-moq-msf-01` version 1 document. `version` is the
  String `"1"`; there is no root `format` field.
- Initialization data lives in the root `initDataList` array, referenced from
  each track by `initRef`, and is serialized after `tracks`.
- Media tracks carry `packaging` of `cmaf` per `draft-ietf-moq-cmsf-01` section
  3.5.1, along with `maxGrpSapStartingType` and `maxObjSapStartingType`.
- All catalog JSON is produced by `serialize_catalog()` in
  `src/msf_catalog.cpp`. Do not hand-assemble catalog JSON.
- The `MsfCatalog::publish_tracks` field models the root `publishTracks` array
  (MSF section 5.1.5), but no emitter populates it yet.
- Catalog track lifecycle (MSF sections 5 and 11.3), owned by
  `CatalogPublisher` in `src/msf_catalog.cpp`:
  - Object 0 of every group holds a full independent catalog; producing an
    independent catalog always starts a new group.
  - Delta updates occupy object IDs 1 and above within the *current* group
    (they never start a new group), and only `add` and `remove` operations
    are emitted -- `clone` is not implemented, since it applies when a new
    track matches an existing one on every field except name, which no
    producer in this project generates. **Scope:** delta updates only happen
    at all inside `MoqtSession`'s two `publish_live()` overloads (the
    SRT-ingest and stdin-ingest live paths), the only callers that ever call
    `CatalogPublisher::publish()` more than once for a session. The batch
    `publish()` plan path and `publish_live_objects()` do not populate
    `CatalogPublisher`, so a broadcast run through either always gets a
    single static, one-shot catalog with no deltas.
    **No delta is emitted on the wire today, even on the two `publish_live()`
    paths:** both build one `LiveCatalog`/`MsfCatalog` snapshot at startup
    and call `catalog_publisher_.publish()` with that same immutable value on
    every SUBSCRIBE and republish tick, so `catalogs_equal()` short-circuits
    after the first call and `publish()` always returns `{}` afterward. The
    add/remove differ and its object-ID/group-ID bookkeeping are exercised
    only from unit tests (`tests/msf_catalog_test.cpp`), not from any
    production code path. Making a delta actually reach the wire needs a
    live producer that detects a track add/remove mid-broadcast (SRT ingest
    noticing a track appear/disappear, or a DASH ingest reconfiguration) and
    calls `catalog_publisher_.publish()` again with an updated `MsfCatalog`;
    wiring that producer is out of scope for the current phase.
  - Every catalog object, independent or delta, maps to MOQT sub-group 0.
  - Section 5.3 freezes a track's attributes once its namespace-and-name
    tuple has been declared: an attribute change on an existing track cannot
    be expressed as a delta, so `CatalogPublisher::publish()` falls back to a
    full independent catalog whenever it detects one, rather than silently
    dropping the change or emitting an invalid delta.
  - `CatalogPublisher::force_independent()` re-emits the last published
    catalog as a fresh independent copy in a new group, for section 5.3's
    "periodically publish a new independent catalog" guidance and for
    section 5's cache-expiry republication; `MoqtSession` calls it on
    `PublisherConfig::catalog_republish_interval` when configured non-zero
    (default zero preserves one-shot delivery). **Scope:** as with deltas,
    this is wired only inside the two `publish_live()` overloads; the batch
    `publish()` path and `publish_live_objects()` never republish.
    **Operator note:** draft-ietf-moq-transport-19 section 10.11 states "A
    sender MUST NOT send PUBLISH_DONE until it has closed all streams it
    will ever open ... for a subscription." `MoqtSession::end_broadcast()`
    can open a further independent-catalog stream on the catalog track's
    alias at any point before the session ends, on every live session,
    regardless of `catalog_republish_interval` -- so `MoqtSession` **always**
    defers PUBLISH_DONE for the catalog subscription, not only when
    republication is enabled. It is sent once, when the `publish_live()`
    polling loop actually winds down (natural end-of-input, or after
    `end_broadcast()` has ended `CatalogPublisher` and no further stream can
    open), never immediately at SUBSCRIBE time. (An earlier revision of this
    document, and of the code, gated the deferral on
    `catalog_republish_interval` being non-zero; that left the default
    configuration -- republication off -- sending PUBLISH_DONE immediately,
    which `end_broadcast()` could then violate by opening one more stream
    afterward. Fixed: the deferral no longer depends on that setting.)
    Operators should expect the catalog subscription to stay open, from the
    receiver's point of view, for the life of the broadcast rather than
    close immediately after the first catalog object.
  - `MoqtSession::end_broadcast()` (MSF section 11.3) uses the same
    `CatalogPublisher` to publish the final independent catalog when a live
    broadcast ends: `isComplete: true` with an empty `tracks` array for
    `kTerminate`, or `isLive: false` (with `trackDuration` for tracks whose
    duration is known) for `kConvertToVod`. This is wired only for the two
    `MoqtSession::publish_live()` overloads, which are the only callers that
    populate `CatalogPublisher` in the first place; a session driven through
    the batch `publish()` plan path or through `publish_live_objects()` does
    not populate it, and `end_broadcast()` correctly skips the final-catalog
    write there rather than guess which track alias is "catalog".
    **Draft conflict on ordering:** MSF section 11.3 lists SUBSCRIBE_DONE
    (i.e. PUBLISH_DONE, in transport terms) before the final catalog object
    in its end-of-broadcast bullet order. This implementation sends the
    final catalog object first and the catalog subscription's PUBLISH_DONE
    afterward -- the reverse of MSF's order -- because MOQT
    draft-ietf-moq-transport-19 section 10.11 forbids sending PUBLISH_DONE
    before every stream a subscription will ever open has closed, and the
    final catalog stream is one such stream. Where the two drafts disagree,
    the transport draft governs what may legally appear on the wire, so its
    ordering wins.
- Not implemented: `clone` delta operations, MSF sections 9/10 log and
  metrics tracks, MSF section 11.1 URL parsing, and MSF section 12
  compression signalling. CMSF section 4 content protection (see the
  dedicated section below) is implemented for the batch/VOD publish path
  only.
- The MSFTS example (`examples/msfts-publisher`) publishes `packaging: "m2ts"`,
  which is not an MSF v1 packaging value; it is defined by
  `draft-gregoire-moq-msfts-00` and is correct only for that draft's tracks.

## CMSF content protection

`draft-ietf-moq-cmsf-01` section 4. Implemented for the batch/VOD publish
path only; the publisher never decrypts and never encrypts anywhere in this
project -- it detects and signals protection already present in its input.

- Protection data lives at the catalog root as `contentProtections` (CMSF
  4.1.1), never duplicated onto a track. Each protected track instead carries
  `contentProtectionRefIDs` (MSF 4.1.2) pointing at the root entries by
  `refID`. `attach_content_protection` (`src/msf_catalog.cpp`) reuses an
  existing root entry when its system ID and scheme already match, rather
  than emitting a duplicate, so multiple tracks sharing a KID share one
  entry.
- A protected track's `codec` string is always the `frma` original format
  (e.g. `avc1.64000C`), not the `encv`/`enca` sample-entry type that wraps
  it. `sample_entry_type` separately keeps the raw `encv`/`enca` type, so a
  consumer can distinguish "this track is protected" from "this track's
  codec". Resolving the codec through `frma` requires reading the profile
  bytes (e.g. `avcC`) via the *effective* sample entry, not a bare
  hard-coded string.
- CENC parameters (`scheme`, `default_KID`, `per_sample_iv_size`,
  `is_protected`) come from `sinf`/`schm`/`schi`/`tenc` inside the encrypted
  sample entry (`cenc.h`'s `parse_track_protection`). A track whose
  protection boxes are absent or malformed is never advertised as protected
  -- protection detection fails closed.
- DRM system init data (`pssh` boxes, siblings of `trak` under `moov`) is
  extracted per system and becomes `contentProtections[].psshBase64`.
  `--drm-config` supplies each system's deployment fields (`laURL`,
  `certURL`, `robustness`) from a JSON file parsed eagerly at CLI startup, so
  a malformed file fails before publishing begins rather than publishing
  with partial configuration.
- **`saio` correction on the CTE ingest path** (`correct_saio_offsets`,
  `src/cmaf_segmenter.cpp`): when the CTE path rebuilds a moof (e.g. to
  materialize `trun` defaults), every offset in a `saio` box is classified
  against the *original* moof size plus the following mdat's size, then
  corrected or refused:
  - An offset less than `original_moof_size + mdat_size` -- i.e. one that
    lands inside the original moof (pointing at `senc`) or inside the mdat
    -- is moof-relative, and is shifted by the same byte delta the rebuild
    applied to the moof's size.
  - An offset at or beyond `original_moof_size + mdat_size` cannot be a
    moof-relative reference within a republished MOQT object; adjusting it
    by the delta would silently point somewhere meaningless and decrypt to
    garbage. It is refused (the fragment is rejected) rather than guessed
    at.
- **Refused, not signalled:** the progressive-remux path (`segment_for_cmaf`'s
  non-fragmented branch) synthesises `moof` boxes from scratch and cannot
  carry `senc`, `saiz`, or `saio`. Encrypted input there (any track with a
  populated `CencTrackProtection`) is refused with an error naming the
  progressive-remux path and the offending track, rather than producing
  output that looks like valid CMAF but cannot be decrypted.
- **Not signalled at all today:** the live publish paths --
  `MoqtSession::publish_live()` (SRT and stdin ingest) and
  `publish_live_objects()` (DASH ingest) -- build their catalog through
  `build_live_catalog` (`src/cmsf_packager.cpp`), which never calls
  `attach_content_protection`. `parse_cli_options` therefore refuses
  `--drm-config` combined with `--live-source srt`, `--live-source dash`
  (when it will actually publish, i.e. not a `--dump-plan` dry run with no
  `--endpoint`), or the default live-stdin path, rather than let a publisher
  emit a catalog with no `contentProtections`/`contentProtectionRefIDs` at
  all for encrypted content -- which would be indistinguishable from
  genuinely unprotected content. Wiring content protection into the live
  paths is future work, not part of this phase.
- **Not modelled:** MoQ Secure Objects encryption fields (MSF 5.2.38-5.2.41)
  are a separate, LOC-packaged end-to-end encryption mechanism; CMSF uses
  CENC instead. The CMSF 4.1.1.4.4 Authorization URL field is also
  deliberately unmodelled -- the draft describes it but never names its JSON
  key.

## Implementation consequence

The code in this repository intentionally separates:

- MP4/CMAF packaging
- draft-version control-plane mapping
- future transport publication

That separation should make it practical to contribute the packaging path first and wire in a concrete OpenMOQ transport session afterwards.
