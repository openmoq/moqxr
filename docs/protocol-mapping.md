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
    producer in this project generates.
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
    (default zero preserves one-shot delivery).
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
- Not implemented: `clone` delta operations, MSF sections 9/10 log and
  metrics tracks, MSF section 11.1 URL parsing, MSF section 12 compression
  signalling, and CMSF section 4 content protection.
- The MSFTS example (`examples/msfts-publisher`) publishes `packaging: "m2ts"`,
  which is not an MSF v1 packaging value; it is defined by
  `draft-gregoire-moq-msfts-00` and is correct only for that draft's tracks.

## Implementation consequence

The code in this repository intentionally separates:

- MP4/CMAF packaging
- draft-version control-plane mapping
- future transport publication

That separation should make it practical to contribute the packaging path first and wire in a concrete OpenMOQ transport session afterwards.
