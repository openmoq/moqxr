# MSF Catalog Track Lifecycle (Phase 2)

Design for the catalog track lifecycle defined by `draft-ietf-moq-msf-01`
sections 5 and 11.3. Phase 2 of the roadmap in
`docs/superpowers/specs/2026-07-28-msf-cmsf-v1-design.md`.

- Status: approved design, pending implementation plan
- Date: 2026-07-29
- Depends on: Phase 1 (merged, commit 7696171)

## Problem

Phase 1 made every catalog moqxr publishes a conformant MSF v1 document. It did
not address when catalogs are published, how they change over a session, or how
a broadcast ends.

Investigation of the current code changed the shape of this phase
substantially, and the findings are recorded here because the Phase 1 design
document described Phase 2 inaccurately.

### The CTE path already satisfies section 5's requirements

`LiveDashIngestSession::build_catalog_locked` (`src/live_dash_ingest.cpp:531`)
emits each republished catalog with `subgroup_id = 0`, `object_id = 0`, and an
incrementing `group_id`. The transport forwards a fresh source catalog whenever
one arrives (`src/transport/moqt_session.cpp:4754`), replacing the retained
copy and sending immediately.

That satisfies every MUST in section 5:

- All catalog updates map to sub-group 0.
- The first Object of any Group holds an independent copy of the catalog.
- An independent update is placed at the start of a new Group.

Section 5 states that a catalog object "MAY be independent of other catalog
objects or it MAY represent a delta update". Delta updates are an optimization,
not a requirement. A publisher that only ever emits independent catalogs, each
in a fresh group, is fully conformant.

### The Phase 1 design document was wrong about the JSON reader

It stated that this phase "requires reading JSON, not just writing it, in order
to diff a prior catalog". That is incorrect. A publisher owns its own catalog
state and holds the previous `MsfCatalog` as a struct in memory, so diffing is
struct-to-struct. Serializing a catalog only to re-parse it would be wasted
work. No JSON reader is built in this phase.

### Actual gaps

| Gap | Spec force | Status |
| --- | --- | --- |
| End-of-broadcast signaling (section 11.3) | Deterministic subscriber signal | Missing entirely |
| Periodic republish on batch and live paths | Section 5 SHOULD (cache expiry) | Missing |
| Delta updates (section 5.1.6) | MAY | Missing, optional |
| Catalog at sub-group 0, object 0, new group | MUST | Already conformant on CTE |

The batch and live paths latch a `catalog_sent` flag
(`src/transport/moqt_session.cpp:3099`, `:3513`) and never republish.

## Decisions

1. **Conformance before optimization.** Part A (end-of-broadcast, periodic
   republish) is the missing behavior and ships first. Part B (delta updates)
   is a bandwidth optimization layered on top. Each is independently testable.
2. **`add` and `remove` only; no `clone`.** No code path removes a track or
   changes a track's attributes today, so `remove` is near-term plausible while
   `clone` is pure speculation — it fires only when a new track matches an
   existing one on every field except `name`, which nothing produces. `clone`
   is recorded as deliberately unimplemented, not silently absent.
3. **No JSON reader.** Diff struct-to-struct, per the finding above.
4. **End-of-broadcast is an explicit Publisher API call.** The application
   decides when a stream ends and whether it becomes VOD or terminates. The
   drafts treat those as materially different signals, so inferring one from
   source EOF would guess at something the publisher cannot know.
5. **Joining FETCH is out of scope.** Section 5's "Subscribers accessing the
   catalog MUST use SUBSCRIBE with a Joining FETCH" binds subscribers. moqxr is
   a publisher. Stated explicitly so the omission is not read as an oversight.
6. **Live is the default; VOD is opt-in.** This publisher is expected to be
   live, or simulating live, unless explicitly configured otherwise. VOD
   semantics are never inferred. See Part A0.

## Part A0: Correct the live-versus-VOD default

Phase 1 got this backwards on the batch path and this phase fixes it.

`build_publish_plan` hardcodes `make_msf_track(track, /*is_live=*/false)`
(`src/cmsf_packager.cpp:440`), so every file publish advertises itself as VOD.
That is wrong for this project: the batch path carries `--paced` and `--loop`
(`include/openmoq/publisher/cli_options.h:52-53`), which exist specifically to
simulate a live broadcast from a file. A looping, paced publish currently
declares `isLive: false`.

Two consequences follow from the flag, and both are wrong for a simulated-live
broadcast:

- `trackDuration` is emitted, which is only legal when `isLive` is false
  (section 5.2.35). A live broadcast has no fixed duration to declare.
- `generatedAt` is suppressed, because section 5.1.2 says it SHOULD NOT be
  included when `isLive` is false. A live broadcast loses its generation
  timestamp, which subscribers use to reason about latency.

The fix:

- `PublisherConfig` gains a `vod` flag, `bool`, defaulting to `false`.
- `build_publish_plan` threads that flag through as `is_live = !vod`, rather
  than hardcoding false.
- When live (the default), `generatedAt` is emitted and `trackDuration` is
  omitted, matching what `build_live_catalog` already does.
- When `vod` is set, the existing behavior is retained.

This changes the batch path's emitted catalog by default. The existing
assertion at `tests/cmaf_segmenter_test.cpp:1038`
(`"expected VOD isLive flag in catalog"`) is updated to expect
`"isLive":true`, with a separate case covering the opt-in VOD path.

`end_broadcast(kConvertToVod)` remains the other route into VOD semantics, and
is likewise never entered implicitly.

## Part A1: End-of-broadcast

`Publisher::end_broadcast(EndBroadcastMode mode)`, with two modes matching
section 11.3.

Both modes first send `SUBSCRIBE_DONE` with status code `0x2 Track Ended` for
all active tracks, then publish one final independent catalog.

**Deviation recorded (final review, finding M6):** the shipped implementation
does the reverse of this ordering -- it writes the final catalog object
first and sends the catalog subscription's PUBLISH_DONE afterward. That is
deliberate, not a bug: MOQT draft-ietf-moq-transport-19 section 10.11
forbids sending PUBLISH_DONE for a subscription until every stream it will
ever open has closed, and the final catalog write is one such stream. MSF
section 11.3's bullet order (`SUBSCRIBE_DONE`/PUBLISH_DONE, then the final
catalog) is therefore not achievable without violating the transport draft's
MUST NOT. Where the two drafts disagree, the transport draft governs what
may legally appear on the wire, so the code follows 10.11's ordering instead
of 11.3's. See `docs/protocol-mapping.md`'s "Draft conflict on ordering"
note for the wire-level writeup.

**`kConvertToVod`** — the live stream becomes a VOD asset. The final catalog
sets `isLive` to false on every track and adds a `trackDuration`.

This interacts with a Phase 1 invariant. `validate_track` throws when a track
has `is_live == true` and `track_duration_ms` set. The transition must
therefore flip `isLive` and add `trackDuration` in the same rebuild. An
implementation that set duration first and cleared `isLive` afterwards would
throw at serialization. This is the most likely defect in Part A1 and gets an
explicit test.

**`kTerminate`** — the broadcast ends permanently. The final catalog sets
`isComplete` to true and carries an empty `tracks` array.

Two Phase 1 behaviors make this work without change. `serialize_catalog` always
writes `tracks` (section 5.1.4, Required), so an empty array serializes
correctly. `isComplete` is suppressed when false (section 5.1.3), so it only
ever appears as `true`. The empty-`tracks` case is the one place where an empty
array is meaningful rather than a defect, so it gets an explicit test.

Section 5.1.3 also states `isComplete` MUST NOT be removed once added, and
section 5.2.7 that a true `isLive` MUST never follow a false one. Both are
one-way transitions. `CatalogPublisher` rejects an attempt to publish after
`end_broadcast`, rather than relying on callers to respect the ordering.

## Part A2: Periodic republish

Section 5: "A catalog object SHOULD be published only when the availability of
tracks changes, or after a period of time has passed such that the catalog
object might fall out of cache in a delivery network."

The batch and live paths publish once and never again. A
`catalog_republish_interval` field is added to `PublisherConfig`
(`include/openmoq/publisher/publisher_api.h`), typed
`std::chrono::seconds` and defaulting to zero, which means disabled. The
current behavior is not wrong, merely not cache-resilient, and enabling
republication by default would change the wire behavior of every existing
deployment.

When enabled, the publisher emits a full independent catalog in a fresh group
at the configured interval, which is what the CTE path already does.

## Part B: Delta updates

A `CatalogPublisher` owns the catalog track's group and object counters and the
last published `MsfCatalog`.

Interface: the publisher is handed a desired catalog state and returns the
objects to send, each a `(group_id, object_id, payload)` triple. The transport
keeps owning the wire; `CatalogPublisher` owns only the decision about what to
emit and where it goes.

- Object 0 of every group holds a full independent catalog.
- Objects with ID 1 and above hold `deltaUpdate` operations.
- Producing an independent catalog forces a new group.
- All catalog objects map to sub-group 0.

Diffing compares the previous and desired catalogs by the namespace and name
tuple, per section 5.3's rule that the tuple defines a fixed set of attributes.

- A tuple present in the desired catalog but not the previous one becomes an
  `add`.
- A tuple present in the previous catalog but not the desired one becomes a
  `remove`. Per section 5.1.6, a remove operation's track object carries the
  track name, may carry the namespace, and MUST NOT hold any other field.
- A tuple present in both but with differing attributes **cannot be expressed
  as a delta**. Section 5.3 freezes attributes once a tuple is declared. The
  differ detects this and falls back to a full independent catalog in a new
  group. Emitting nothing here would strand subscribers on stale attributes,
  which is why the case is called out rather than left to fall through.

A delta catalog carries the `deltaUpdate` field and MUST NOT contain `tracks`
or `version` (section 5.3). Phase 1's `serialize_catalog` does not yet enforce
that combination; Part B extends validation to cover it.

Section 5.3 also advises that producers publishing frequent delta updates
SHOULD periodically publish a new independent catalog to bound the delta
processing a joining subscriber must perform. A maximum-deltas-per-group bound
forces a new independent catalog once exceeded.

## Module boundaries

`CatalogPublisher` lives beside the catalog model, declared in
`include/openmoq/publisher/msf_catalog.h` and defined in
`src/msf_catalog.cpp`. It is catalog policy, not transport, and is testable
with no session — matching how Phase 1's serializer is tested.

The transport changes are confined to replacing the `catalog_sent` latch with
calls into `CatalogPublisher`, and to emitting `SUBSCRIBE_DONE` on
`end_broadcast`.

## Error handling

Consistent with Phase 1: invariant violations are programming errors and throw
`std::runtime_error` naming the offending track or condition.

Validated by `CatalogPublisher`:

- Publishing after `end_broadcast` has been called.
- A delta catalog carrying `tracks` or `version`.
- A remove operation whose track object carries any field beyond name and
  namespace.
- `trackDuration` present on a track whose `isLive` is true (inherited from
  Phase 1's `validate_track`).

## Testing

Tests follow the existing single-binary `expect()` style. Phase 1 left helpers
in `tests/msf_catalog_test.cpp` (`expect`, `expect_contains`,
`expect_not_contains`, `throws_runtime_error`) and in
`tests/cmaf_segmenter_test.cpp` (`no_numeric_id_field`,
`all_init_refs_resolve`) that this phase reuses rather than redefines.

Part A0:

- A batch publish with default configuration emits `isLive: true`, carries a
  `generatedAt`, and carries no `trackDuration` on any track.
- A batch publish with `vod` set emits `isLive: false`, carries a
  `trackDuration`, and carries no `generatedAt`.
- Neither configuration throws at serialization, which is the pairing Phase 1's
  `validate_track` enforces.

Part A1:

- `kConvertToVod` emits `isLive: false` and a `trackDuration` on every track,
  and serializes without throwing — the flip-and-add-together case.
- `kTerminate` emits `isComplete: true` with an empty `tracks` array.
- Publishing after `end_broadcast` throws.
- `isComplete` never appears as `false`.

Part A2:

- With republication disabled, exactly one catalog object is emitted, matching
  today's behavior.
- With it enabled, successive catalogs land in successive groups at object 0.

Part B:

- Adding a track produces a delta with one `add` at object ID 1 of the current
  group, and the emitted delta carries neither `tracks` nor `version`.
- Removing a track produces a `remove` carrying only name and namespace.
- Changing a track's attributes produces a full independent catalog in a new
  group, not a delta. This is the case most likely to be implemented wrongly.
- Exceeding the delta bound forces a new independent catalog.
- A no-op diff emits nothing.

## Out of scope

- `clone` delta operations (decision 2).
- A JSON reader (decision 3).
- Joining FETCH, a subscriber requirement (decision 5).
- MSF section 12 compression signaling, still blocked on transport draft-19
  Track and Object Properties.
- Phases 3 and 4: CMSF content protection, and MSF URL parsing.

## References

- `docs/draft-ietf-moq-msf-01.txt` sections 5, 5.1.3, 5.1.6, 5.2.7, 5.3, 11.3
- `docs/superpowers/specs/2026-07-28-msf-cmsf-v1-design.md`
- Phase 1 merge commit 7696171
