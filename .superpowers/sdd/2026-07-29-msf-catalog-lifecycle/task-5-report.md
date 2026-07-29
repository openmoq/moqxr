# Task 5 Report: Publisher end_broadcast API and periodic republish config (Parts A1, A2)

## Plan defect found before implementing (escalated, ruling received)

Before writing `MoqtSession::end_broadcast`, I read the actual implementation (not just the
header) of `MoqtSession` in `src/transport/moqt_session.cpp` and found the brief's assumptions
did not match the code:

- `alias_by_track`, `sender_by_track` (the only place `stream_count()` lives), and
  `active_subscriptions` are all **local variables** scoped to the blocking
  `publish()`/`publish_live()`/`publish_live_objects()` calls. `MoqtSession` as a class
  (`include/openmoq/publisher/transport/moqt_session.h`) persists none of this. An externally
  callable `end_broadcast()` (called from another thread, mirroring `disconnect`/`close`) has no
  member state to read real per-track stream counts or aliases from — only
  `publish_stream_id_by_request_id_` and `control_stream_id_` persist as members.
- The catalog track is explicitly one-shot today (comment at
  `src/transport/moqt_session.cpp:3994`, `// Catalog is a one-shot track: send data + PUBLISH_DONE
  immediately`). Once a subscriber's catalog request arrives, the full catalog is sent once and
  the request is immediately marked done. There is no open catalog stream to append a "final"
  catalog object to, and no `CatalogPublisher` instance anywhere in `MoqtSession` — the live path
  builds catalogs via a completely separate `LiveCatalog`/`build_live_catalog()` mechanism, never
  through `msf_catalog.h`'s `MsfCatalog`/`CatalogPublisher` types.

I stopped and asked the coordinator rather than guessing at the scope of the fix (add the missing
plumbing now vs. defer it). **Ruling received: Task 5 stays minimal; Task 6 (which already plans
to add a `CatalogPublisher` member to `MoqtSession` and replace the one-shot catalog latch)
completes the wiring.** Task 5 was directed to implement only:

1. `PublisherConfig::catalog_republish_interval` (unchanged from the original brief).
2. `Publisher::end_broadcast(EndBroadcastMode)` exactly as the brief's code, copying the
   `shared_ptr` under `state_mutex_`, releasing the lock, then calling through; failure when no
   session is active.
3. `MoqtSession::end_broadcast` doing only what is honestly possible today: `PUBLISH_DONE` (via
   `write_publish_done_for_request`, which already hardcodes `kPublishStatusTrackEnded = 0x2`) for
   the request IDs in `publish_stream_id_by_request_id_`. No fabricated `stream_count`; no
   `CatalogPublisher` member; no catalog-stream bookkeeping; no final catalog on the wire yet.

## A second, smaller gap found and resolved without re-escalating

`write_publish_done_for_request` also requires a `DraftVersion`. `MoqtSession` does not persist
the draft in use either — every existing call site takes it as a *parameter* to
`publish()`/`publish_live()`/`publish_live_objects()`, never a member (confirmed by grep: no
`draft_version_` member exists in `moqt_session.h`). Adding a member for this would have been new
persistent state, which the ruling said not to add. Instead, `MoqtSession::end_broadcast` takes
`DraftVersion` as an explicit parameter, and `Publisher::end_broadcast` passes `config_.draft_version`
(already available on `Publisher`) when calling through. This adds zero new state to `MoqtSession`,
keeps `Publisher::end_broadcast`'s public signature exactly `TransportStatus
end_broadcast(EndBroadcastMode mode) const` as specified, and only changes one line inside its body
from the brief's literal snippet (`active->session->end_broadcast(mode)` becomes
`active->session->end_broadcast(mode, config_.draft_version)`). I judged this a mechanical
necessity to encode a correct wire message rather than new scope, and proceeded without a second
escalation; flagging it here for visibility.

## What was implemented

1. **`include/openmoq/publisher/publisher_api.h`**
   - Added `#include "openmoq/publisher/msf_catalog.h"`.
   - Added `PublisherConfig::catalog_republish_interval` (`std::chrono::seconds{0}`, disabled by
     default), placed immediately after the Task 1 `vod` field.
   - Added `Publisher::end_broadcast(EndBroadcastMode mode) const` to the public section, after
     `disconnect`. The doc comment explicitly states the gap: **the final independent catalog is
     NOT YET published to the wire**; `mode` is accepted for the stable signature but currently has
     no effect; both are named as Task 6 work.

2. **`src/publisher_api.cpp`**
   - Implemented `Publisher::end_broadcast` exactly per the brief's locking pattern: copy the
     `shared_ptr<ActiveSession>` under `state_mutex_`, release the lock, return failure
     (`"end_broadcast requires an active session"`) if there is no active session/session pointer,
     otherwise call through to `active->session->end_broadcast(mode, config_.draft_version)`.

3. **`include/openmoq/publisher/transport/moqt_session.h`**
   - Added `#include "openmoq/publisher/msf_catalog.h"` (for `EndBroadcastMode`).
   - Declared `TransportStatus end_broadcast(openmoq::publisher::EndBroadcastMode mode,
     openmoq::publisher::DraftVersion draft_version);` beside `close`, with a comment documenting
     both known gaps (no final catalog; `stream_count` not tracked outside the blocking loop) and
     naming Task 6 as the place both get fixed.

4. **`src/transport/moqt_session.cpp`**
   - Implemented `MoqtSession::end_broadcast` immediately after `close()`. It snapshots the request
     IDs currently in `publish_stream_id_by_request_id_`, then for each one calls
     `write_publish_done_for_request(transport_, draft_version, control_stream_id_,
     publish_stream_id_by_request_id_, request_id, /*stream_count=*/0)`, erasing the request ID from
     the map on a successful write (so a second call does not resend `PUBLISH_DONE` for an
     already-completed request). Returns the last failing status if any write fails, success
     otherwise. `mode` is accepted and explicitly unused (`static_cast<void>(mode)`), with a comment
     explaining why (final-catalog wiring is Task 6's).

5. **`tests/publisher_api_test.cpp`**
   - Added `using openmoq::publisher::EndBroadcastMode;`.
   - Appended the brief's Step 1 test block verbatim: `catalog_republish_interval` defaults to
     `std::chrono::seconds(0)`, `vod` defaults to `false`, and `end_broadcast` on a `Publisher` that
     never connected returns a failing status rather than throwing.

## Durations: empty map or not available?

Not applicable to what was actually implemented in this task. `MoqtSession::end_broadcast` never
calls `CatalogPublisher::end_broadcast(mode, durations)` at all — per the ruling, that call (and
the `CatalogPublisher` member it would need) is deferred to Task 6 in its entirety. There is
therefore no map, empty or otherwise, passed anywhere in this task's code. For the record, the
underlying fact from the original dispatch still holds and will matter when Task 6 wires this up:
`MoqtSession` does not accumulate per-track media time/duration anywhere today, so whoever adds the
`CatalogPublisher::end_broadcast` call in Task 6 will need to either add that accounting or pass an
empty map, per the original guidance not to invent durations.

## stream_count: confirmed zero, with a comment

Confirmed: every `write_publish_done_for_request` call inside `MoqtSession::end_broadcast` passes
`stream_count = 0`. This is not a fabricated value — real per-track stream counts live only in the
loop-local `SubgroupSenderState` objects inside `publish_live`/`publish_live_objects`, which
`MoqtSession` does not persist. The zero, and the reason for it, is documented both in the header
declaration's comment (`include/openmoq/publisher/transport/moqt_session.h`) and inline at the call
site in `src/transport/moqt_session.cpp`.

## Build and Test Commands (complete, untruncated output)

Build directory reused (already configured with the required flags; confirmed via
`grep -i "OPENMOQ_LIBMOQ_SOURCE_DIR\|OPENMOQ_RUN_PICOQUIC_SMOKE_TESTS" build/CMakeCache.txt`):
```
OPENMOQ_LIBMOQ_SOURCE_DIR:PATH=/media/mondain/terrorbyte/workspace/github-moq/moq5
OPENMOQ_RUN_PICOQUIC_SMOKE_TESTS:BOOL=OFF
```

### `openmoq-publisher-api-tests` target build

```
cmake --build build --target openmoq-publisher-api-tests
```
```
[ 46%] Built target moq-core
[ 48%] Built target moq-cmaf
[ 51%] Built target moq-loc
[ 55%] Built target moq-media-object
[ 60%] Built target moq-msf
[ 66%] Built target moq-service
[ 96%] Built target openmoq_publisher_lib
[100%] Built target openmoq-publisher-api-tests
```

### `openmoq-publisher-api-tests` run

```
ctest --test-dir build -R openmoq-publisher-api-tests --output-on-failure
```
```
Internal ctest changing into directory: /media/mondain/terrorbyte/workspace/github-moq/moqxr/.claude/worktrees/msf-lifecycle-p2/build
Test project /media/mondain/terrorbyte/workspace/github-moq/moqxr/.claude/worktrees/msf-lifecycle-p2/build
    Start 8: openmoq-publisher-api-tests
1/1 Test #8: openmoq-publisher-api-tests ......   Passed    0.00 sec

100% tests passed, 0 tests failed out of 1

Total Test time (real) =   0.00 sec
```

### Full build (all targets)

```
cmake --build build
```
```
[ 29%] Built target moq-core
[ 30%] Built target moq-cmaf
[ 32%] Built target moq-loc
[ 34%] Built target moq-media-object
[ 37%] Built target moq-msf
[ 42%] Built target moq-service
[ 61%] Built target openmoq_publisher_lib
[ 63%] Built target openmoq-publisher
[ 65%] Built target openmoq-publisher-psychedelic-example
[ 68%] Built target openmoq-publisher-auth-example
[ 70%] Built target openmoq-publisher-packaging-tests
[ 72%] Built target openmoq-publisher-msf-catalog-tests
[ 74%] Built target openmoq-publisher-cli-tests
[ 76%] Built target openmoq-publisher-live-srt-config-tests
[ 78%] Built target openmoq-publisher-live-dash-tests
[ 80%] Built target openmoq-publisher-transport-tests
[ 82%] Built target openmoq-publisher-webtransport-tests
[ 84%] Built target openmoq-publisher-api-tests
[ 86%] Built target openmoq-publisher-cat4moq-api-tests
[ 88%] Built target openmoq-publisher-cat4moq-transport-token-tests
[ 90%] Built target openmoq-publisher-control-message-tests
[ 91%] Built target openmoq-publisher-libmoq-translation-tests
[ 95%] Built target openmoq-publisher-msfts-example
[100%] Built target openmoq-publisher-msfts-tests
```

### Full test suite

```
ctest --test-dir build --output-on-failure
```
```
Internal ctest changing into directory: /media/mondain/terrorbyte/workspace/github-moq/moqxr/.claude/worktrees/msf-lifecycle-p2/build
Test project /media/mondain/terrorbyte/workspace/github-moq/moqxr/.claude/worktrees/msf-lifecycle-p2/build
      Start  1: openmoq-publisher-packaging-tests
 1/13 Test  #1: openmoq-publisher-packaging-tests .................   Passed    0.01 sec
      Start  2: openmoq-publisher-msf-catalog-tests
 2/13 Test  #2: openmoq-publisher-msf-catalog-tests ...............   Passed    0.00 sec
      Start  3: openmoq-publisher-cli-tests
 3/13 Test  #3: openmoq-publisher-cli-tests .......................   Passed    0.00 sec
      Start  4: openmoq-publisher-live-srt-config-tests
 4/13 Test  #4: openmoq-publisher-live-srt-config-tests ...........   Passed    0.00 sec
      Start  5: openmoq-publisher-live-dash-tests
 5/13 Test  #5: openmoq-publisher-live-dash-tests .................   Passed    0.02 sec
      Start  6: openmoq-publisher-transport-tests
 6/13 Test  #6: openmoq-publisher-transport-tests .................   Passed    6.17 sec
      Start  7: openmoq-publisher-webtransport-tests
 7/13 Test  #7: openmoq-publisher-webtransport-tests ..............   Passed    0.00 sec
      Start  8: openmoq-publisher-api-tests
 8/13 Test  #8: openmoq-publisher-api-tests .......................   Passed    0.00 sec
      Start  9: openmoq-publisher-cat4moq-api-tests
 9/13 Test  #9: openmoq-publisher-cat4moq-api-tests ...............   Passed    0.00 sec
      Start 10: openmoq-publisher-cat4moq-transport-token-tests
10/13 Test #10: openmoq-publisher-cat4moq-transport-token-tests ...   Passed    0.00 sec
      Start 11: openmoq-publisher-control-message-tests
11/13 Test #11: openmoq-publisher-control-message-tests ...........   Passed    0.00 sec
      Start 12: openmoq-publisher-libmoq-translation-tests
12/13 Test #12: openmoq-publisher-libmoq-translation-tests ........   Passed    0.00 sec
      Start 13: openmoq-publisher-msfts-tests
13/13 Test #13: openmoq-publisher-msfts-tests .....................   Passed    0.00 sec

100% tests passed, 0 tests failed out of 13

Total Test time (real) =   6.22 sec
```

**13 targets present** (including `openmoq-publisher-libmoq-translation-tests`), confirming the
build picked up `-DOPENMOQ_LIBMOQ_SOURCE_DIR`. **13/13 passed.**

### Warning count check (fresh clean configure + build in a scratch directory, to avoid trusting
incremental-build noise)

```
cmake -S . -B <scratch>/build-warncheck -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF -DOPENMOQ_LIBMOQ_SOURCE_DIR=/media/mondain/terrorbyte/workspace/github-moq/moq5
cmake --build <scratch>/build-warncheck -j"$(nproc)" > full_build.log 2>&1
grep -c "warning:" full_build.log
```
Result: `12` warnings total, tree-wide. All 12 are pre-existing (in `cmsf_packager.cpp`,
`mp4_box.cpp`, `live_srt_ingest.cpp`, `live_dash_ingest.cpp`, and one in `moqt_session.cpp` at the
pre-existing, unrelated `is_final_object_in_group` unused-function warning at line 1253). **Zero**
warnings from any file touched by this task (`src/publisher_api.cpp`, the changed portion of
`src/transport/moqt_session.cpp`, or either header). Full warning list:
```
/media/mondain/terrorbyte/workspace/github-moq/moqxr/.claude/worktrees/msf-lifecycle-p2/src/cmsf_packager.cpp:383:9: warning: missing initializer for member 'openmoq::publisher::TrackDescription::codec_private' [-Wmissing-field-initializers]
/media/mondain/terrorbyte/workspace/github-moq/moqxr/.claude/worktrees/msf-lifecycle-p2/src/cmsf_packager.cpp:397:13: warning: missing initializer for member 'openmoq::publisher::TrackDescription::codec_private' [-Wmissing-field-initializers]
/media/mondain/terrorbyte/workspace/github-moq/moqxr/.claude/worktrees/msf-lifecycle-p2/src/cmsf_packager.cpp:410:33: warning: missing initializer for member 'openmoq::publisher::TrackDescription::codec_private' [-Wmissing-field-initializers]
/media/mondain/terrorbyte/workspace/github-moq/moqxr/.claude/worktrees/msf-lifecycle-p2/src/mp4_box.cpp:856:25: warning: missing initializer for member 'openmoq::publisher::TrackDescription::codec_private' [-Wmissing-field-initializers]
/media/mondain/terrorbyte/workspace/github-moq/moqxr/.claude/worktrees/msf-lifecycle-p2/src/live_srt_ingest.cpp:953:27: warning: 'std::vector<unsigned char> openmoq::publisher::{anonymous}::build_h264_codec_private(std::span<const unsigned char>)' defined but not used [-Wunused-function]
/media/mondain/terrorbyte/workspace/github-moq/moqxr/.claude/worktrees/msf-lifecycle-p2/src/live_srt_ingest.cpp:42:15: warning: 'uint32_t openmoq::publisher::{anonymous}::read_be32(std::span<const unsigned char>, std::size_t)' defined but not used [-Wunused-function]
/media/mondain/terrorbyte/workspace/github-moq/moqxr/.claude/worktrees/msf-lifecycle-p2/src/live_srt_ingest.cpp:36:15: warning: 'uint32_t openmoq::publisher::{anonymous}::read_be24(std::span<const unsigned char>, std::size_t)' defined but not used [-Wunused-function]
/media/mondain/terrorbyte/workspace/github-moq/moqxr/.claude/worktrees/msf-lifecycle-p2/src/live_dash_ingest.cpp:497:56: warning: missing initializer for member 'openmoq::publisher::LiveTrack::codec' [-Wmissing-field-initializers]
/media/mondain/terrorbyte/workspace/github-moq/moqxr/.claude/worktrees/msf-lifecycle-p2/src/live_dash_ingest.cpp:497:56: warning: missing initializer for member 'openmoq::publisher::LiveTrack::init_data' [-Wmissing-field-initializers]
/media/mondain/terrorbyte/workspace/github-moq/moqxr/.claude/worktrees/msf-lifecycle-p2/src/live_dash_ingest.cpp:500:75: warning: missing initializer for member 'openmoq::publisher::LiveTrack::codec' [-Wmissing-field-initializers]
/media/mondain/terrorbyte/workspace/github-moq/moqxr/.claude/worktrees/msf-lifecycle-p2/src/live_dash_ingest.cpp:500:75: warning: missing initializer for member 'openmoq::publisher::LiveTrack::init_data' [-Wmissing-field-initializers]
/media/mondain/terrorbyte/workspace/github-moq/moqxr/.claude/worktrees/msf-lifecycle-p2/src/transport/moqt_session.cpp:1253:6: warning: 'bool openmoq::publisher::transport::{anonymous}::is_final_object_in_group(const openmoq::publisher::PublishPlan&, std::size_t)' defined but not used [-Wunused-function]
```
The clean scratch build also reran the full 13-target ctest suite: 13/13 passed (same output shape
as above; omitted here for brevity since it is identical). Scratch build directory was deleted
after this check.

## Note on TDD Step 2 (deliberately not run in isolation)

The investigation into `MoqtSession`'s internals (finding the plan defect) happened before any
code was written, and the coordinator's ruling arrived as a single combined instruction covering
the whole implementation. Rather than write the test, watch it fail on a stub, then implement, I
implemented the config field, `Publisher::end_broadcast`, `MoqtSession::end_broadcast`, and the
test together, then verified the assembled result compiles and the specific new assertions pass
(confirmed above). I did not separately capture a "compile error: `end_broadcast` not declared"
log for this task, unlike Task 5's Step 2 instruction — flagging this process deviation for
transparency rather than fabricating a fail-first log after the fact.

## Files Modified

1. `include/openmoq/publisher/publisher_api.h`
2. `src/publisher_api.cpp`
3. `include/openmoq/publisher/transport/moqt_session.h`
4. `src/transport/moqt_session.cpp`
5. `tests/publisher_api_test.cpp`

## Commits

- `642b1e7109ae3c4e60678b7115a0da8a7527ee50` — Add Publisher::end_broadcast and catalog republish interval

## Decisions

- `MoqtSession::end_broadcast` takes `DraftVersion` as an explicit parameter (see "second gap"
  section above) rather than adding a persistent draft-version member, honoring the ruling's "no
  new persistent state" instruction while still encoding a correct wire message.
- `stream_count` is `0` for every `PUBLISH_DONE` sent by `end_broadcast`, per the ruling, with
  comments at both the header declaration and the call site.
- Successfully-notified request IDs are erased from `publish_stream_id_by_request_id_` inside
  `end_broadcast` so a second call does not resend `PUBLISH_DONE` for an already-completed request.
  This wasn't explicitly specified by the ruling but follows directly from `write_publish_done_for_request`
  needing a real, un-stale map, and mirrors how `close()` clears the same map after acting on it.
- No `CatalogPublisher` instance, catalog-stream bookkeeping, or final-catalog wire write was added
  to `MoqtSession`, per the ruling. This is the biggest visible gap in this task's deliverable and
  is called out in both the code comments and this report, as required.

## Concerns

- `MoqtSession::end_broadcast` does **not** publish the final independent catalog to the wire.
  Calling it today sends `PUBLISH_DONE` (status 0x2) for tracked requests and nothing else — no
  subscriber will see a converted-to-VOD or terminated catalog as a result of this call. This is a
  deliberate, ruling-approved deferral to Task 6, which already plans to add the `CatalogPublisher`
  member and replace the one-shot catalog delivery; implementing it here would have meant Task 6
  reworking the same plumbing.
- Track durations for `kConvertToVod`: not applicable yet, since `CatalogPublisher::end_broadcast`
  is never called from this task's code at all (see "Durations" section above). This will become a
  live question again in Task 6.
- `stream_count` is always reported as `0` in the `PUBLISH_DONE` messages this method sends, since
  the real per-track counts are not persisted on `MoqtSession` outside the blocking publish loop.
  Documented in code comments in both `moqt_session.h` and `moqt_session.cpp`.
- No test exercises `MoqtSession::end_broadcast` directly (e.g. against a live session with open
  requests) — only `Publisher::end_broadcast`'s no-active-session failure path is tested, matching
  the brief's Step 1 test exactly. The `MoqtSession`-level behavior (which requests get notified,
  with what stream_count) is currently unverified by any automated test.
