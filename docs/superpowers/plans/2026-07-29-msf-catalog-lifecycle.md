# MSF Catalog Track Lifecycle Implementation Plan (Phase 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the MSF catalog track lifecycle — correct live-versus-VOD defaults, end-of-broadcast signalling, periodic republication, and add/remove delta updates — on top of the Phase 1 catalog model.

**Architecture:** A `CatalogPublisher` owns the catalog track's group and object counters plus the last published `MsfCatalog`, and decides what to emit: a full independent catalog at object 0 of a new group, or a `deltaUpdate` at object ID 1 or above within the current group. It diffs struct-to-struct with no JSON parsing. End-of-broadcast becomes an explicit `Publisher::end_broadcast(mode)` call. The transport keeps owning the wire.

**Tech Stack:** C++20, CMake, static library `openmoq_publisher_lib`, single-binary tests using a local `expect(bool, message)` helper returning a process exit code.

## Global Constraints

- C++20. No new third-party dependencies. Namespace `openmoq::publisher`. Headers use `#pragma once`.
- **This publisher is live, or simulating live, unless explicitly configured otherwise. VOD semantics are never inferred.**
- The project does **NOT** compile warning-free. Roughly 10 pre-existing warnings, no `-Werror`. Do not chase pre-existing warnings; do not add avoidable new ones.
- All catalog JSON is produced by `serialize_catalog()` in `src/msf_catalog.cpp`. Never hand-assemble catalog JSON.
- Catalog `version` is the String `"1"`. `initDataList` serializes after `tracks`.
- Tests are single-binary programs using local `expect(bool, std::string)` helpers. There is no test framework and no shared test header. Follow that convention.
- Build and test: `cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF && cmake --build build && ctest --test-dir build --output-on-failure`
- The suite is currently **13/13** targets passing. It must stay 13/13.
- Commit messages: no emoji, no "Generated with Claude Code" tagline, no Co-Authored-By line.

## Phase 1 state you are building on

Already exists in `include/openmoq/publisher/msf_catalog.h`:

- `MsfCatalog { std::string version; std::optional<std::uint64_t> generated_at_ms; std::optional<bool> is_complete; std::vector<MsfTrack> tracks; std::vector<MsfTrack> publish_tracks; std::vector<MsfInitData> init_data_list; }`
- `MsfTrack` with `name`, `name_space`, `packaging`, `role`, `is_live`, `target_latency_ms`, `buffers`, `label`, `render_group`, `alt_group`, `init_ref`, `depends`, `codec`, `mime_type`, `framerate`, `timescale`, `bitrate`, `avg_bitrate`, `max_gop_duration_ms`, `max_group_duration_ms`, `width`, `height`, `samplerate`, `channel_config`, `lang`, `track_duration_ms`, `event_type`, `max_grp_sap_starting_type`, `max_obj_sap_starting_type`, `custom_fields`. All except `name`, `packaging`, `is_live`, `depends`, `custom_fields` are `std::optional`.
- `MsfBuffers { std::optional<std::uint64_t> target_ms, min_ms, max_ms; }`
- `MsfInitData { std::string id, type, data; }`
- `std::string serialize_catalog(const MsfCatalog&)` — throws `std::runtime_error` on an invariant violation.
- `MsfTrack make_msf_track(const TrackDescription&, bool is_live, std::optional<std::uint64_t> configured_bitrate = std::nullopt, std::uint64_t computed_bitrate = 0)`
- `void attach_init_data(MsfCatalog&, MsfTrack&, std::string_view track_name, std::string base64_data)`
- `std::uint64_t resolve_bitrate(...)`, `bool bitrate_is_estimated(...)`

Invariants `validate_track` already enforces, which constrain this phase:

- Role `video` or `audio` MUST have `bitrate`.
- `is_live == true` and `track_duration_ms` set together **throws**.
- `eventType` only on `eventtimeline` packaging, and required there.
- Audio MUST have `samplerate` and `channel_config`.
- `targetLatency` and `buffers` are mutually exclusive.
- Custom field names MUST NOT collide with spec names.

`validate_catalog` enforces: unique `initDataList` ids; unique namespace+name across `tracks` and `publish_tracks` combined; every `init_ref` resolves.

`serialize_catalog` emits `isComplete` only when it is `true`, and always emits `tracks`.

## Transport state you are building on

- `encode_publish_done_message(draft, request_id, stream_count)` already hardcodes `kPublishStatusTrackEnded = 0x2` (`src/transport/moqt_control_messages.cpp:55,1440`). MSF section 11.3's status-code requirement is already met.
- `write_publish_done_for_request(...)` (`src/transport/moqt_session.cpp:1432`) sends it.
- The live-object path already accepts a fresh source catalog at any time and honours its `group_id` (`src/transport/moqt_session.cpp:4754`).
- The batch and live paths latch a `catalog_sent` bool (`src/transport/moqt_session.cpp:3099`, `:3513`) and never republish.

## File Structure

| File | Responsibility |
| --- | --- |
| `include/openmoq/publisher/msf_catalog.h` (modify) | `MsfDeltaOp`, `EndBroadcastMode`, `CatalogPublisher`, `CatalogObject` declarations |
| `src/msf_catalog.cpp` (modify) | Delta serialization, delta validation, `CatalogPublisher` implementation |
| `include/openmoq/publisher/cmsf_packager.h` (modify) | `build_publish_plan` gains a `vod` parameter |
| `src/cmsf_packager.cpp` (modify) | Thread `vod` through to `make_msf_track`; emit `generatedAt` when live |
| `include/openmoq/publisher/publisher_api.h` (modify) | `PublisherConfig::vod`, `::catalog_republish_interval`; `Publisher::end_broadcast` |
| `src/publisher_api.cpp` (modify) | Pass config through; implement `end_broadcast` |
| `tests/msf_catalog_test.cpp` (modify) | Delta shape, `CatalogPublisher` sequencing, end-of-broadcast catalogs |
| `tests/cmaf_segmenter_test.cpp` (modify) | Batch live-default and VOD opt-in assertions |
| `docs/status.md`, `docs/protocol-mapping.md` (modify) | Record what Phase 2 shipped |

---

### Task 1: Live-by-default on the batch path (Part A0)

**Files:**
- Modify: `include/openmoq/publisher/cmsf_packager.h:46-49`
- Modify: `src/cmsf_packager.cpp:433-441`
- Modify: `include/openmoq/publisher/publisher_api.h` (`PublisherConfig`)
- Modify: `src/publisher_api.cpp:102-106`, `:115-119`
- Modify: `tests/cmaf_segmenter_test.cpp:1038`

**Interfaces:**
- Consumes: `make_msf_track` from Phase 1.
- Produces: `build_publish_plan(..., bool vod = false)`; `PublisherConfig::vod`. Task 4 reads `PublisherConfig`.

- [ ] **Step 1: Write the failing test**

In `tests/cmaf_segmenter_test.cpp`, replace the assertion at line 1038 and add a VOD case. The existing `plan` variable comes from `build_publish_plan(segmented, DraftVersion::kDraft14)` at line 822.

```cpp
    // The publisher is live, or simulating live, unless configured otherwise.
    ok &= expect_contains(catalog_text, "\"isLive\":true",
                          "expected batch publish to default to live");
    ok &= expect_contains(catalog_text, "\"generatedAt\":",
                          "expected generatedAt on a live batch catalog");
    ok &= expect_not_contains(catalog_text, "\"trackDuration\"",
                              "expected no trackDuration on a live batch catalog");

    // Opt-in VOD retains the previous semantics.
    const auto vod_plan = build_publish_plan(segmented, DraftVersion::kDraft14,
                                             /*include_sap=*/false,
                                             /*include_msf_timeline=*/false,
                                             /*vod=*/true);
    const std::string vod_catalog_text = object_text(vod_plan.objects.front());
    ok &= expect_contains(vod_catalog_text, "\"isLive\":false",
                          "expected opt-in VOD to mark tracks not live");
    ok &= expect_not_contains(vod_catalog_text, "\"generatedAt\":",
                              "expected no generatedAt on a VOD catalog (MSF 5.1.2)");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-packaging-tests && ctest --test-dir build -R openmoq-publisher-packaging-tests --output-on-failure`
Expected: FAIL — `build_publish_plan` takes 4 arguments, and the catalog still says `"isLive":false`.

- [ ] **Step 3: Add the parameter**

In `include/openmoq/publisher/cmsf_packager.h`, change the declaration to:

```cpp
// vod: when true the plan describes a Video-On-Demand asset (isLive false,
// trackDuration present, generatedAt omitted per MSF 5.1.2). Defaults to
// false because this publisher is live, or simulating live, unless
// explicitly configured otherwise.
PublishPlan build_publish_plan(const SegmentedMp4& segmented_mp4,
                               DraftVersion version,
                               bool include_sap = false,
                               bool include_msf_timeline = false,
                               bool vod = false);
```

In `src/cmsf_packager.cpp`, update the definition's signature to match, then replace the block at lines 433-441:

```cpp
    MsfCatalog msf_catalog;
    // Section 5.1.2: generatedAt SHOULD NOT be included when isLive is false.
    if (!vod) {
        msf_catalog.generated_at_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }
    for (const auto& track : plan.tracks) {
        if (track.track_name == "catalog") {
            continue;
        }
        MsfTrack msf_track = make_msf_track(track, /*is_live=*/!vod);
```

`<chrono>` is already included in this file (added in Phase 1 for `build_live_catalog`).

- [ ] **Step 4: Thread the config through**

In `include/openmoq/publisher/publisher_api.h`, add to `PublisherConfig` after `bool loop = false;`:

```cpp
    // This publisher is live, or simulating live, unless explicitly told
    // otherwise. VOD semantics are never inferred from the input.
    bool vod = false;
```

In `src/publisher_api.cpp`, add `config_.vod` as the fifth argument to both `build_publish_plan` calls (lines 102-106 and 115-119).

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS, 13/13.

- [ ] **Step 6: Commit**

```bash
git add include/openmoq/publisher/cmsf_packager.h src/cmsf_packager.cpp include/openmoq/publisher/publisher_api.h src/publisher_api.cpp tests/cmaf_segmenter_test.cpp
git commit -m "Default the batch publish path to live, with VOD opt-in"
```

---

### Task 2: End-of-broadcast catalog states (Part A1, catalog layer)

**Files:**
- Modify: `include/openmoq/publisher/msf_catalog.h`
- Modify: `src/msf_catalog.cpp`
- Modify: `tests/msf_catalog_test.cpp`

**Interfaces:**
- Consumes: `MsfCatalog`, `MsfTrack`, `serialize_catalog` from Phase 1.
- Produces: `enum class EndBroadcastMode { kConvertToVod, kTerminate };` and `MsfCatalog make_end_broadcast_catalog(const MsfCatalog& current, EndBroadcastMode mode, const std::map<std::string, std::uint64_t>& track_durations_ms);` — used by Task 3 and Task 6.

- [ ] **Step 1: Write the failing test**

Append to `main()` in `tests/msf_catalog_test.cpp`, before `return ok ? 0 : 1;`. Add `#include <map>` to the includes.

```cpp
    // MSF 11.3: ending a live broadcast.
    MsfCatalog live_now;
    live_now.generated_at_ms = 1751000000000ULL;
    MsfTrack live_v;
    live_v.name = "video";
    live_v.packaging = "cmaf";
    live_v.role = "video";
    live_v.is_live = true;
    live_v.codec = "avc1.640028";
    live_v.bitrate = 5000000;
    live_now.tracks.push_back(live_v);
    MsfTrack live_a;
    live_a.name = "audio";
    live_a.packaging = "cmaf";
    live_a.role = "audio";
    live_a.is_live = true;
    live_a.codec = "mp4a.40.2";
    live_a.bitrate = 128000;
    live_a.samplerate = 48000;
    live_a.channel_config = "2";
    live_now.tracks.push_back(live_a);

    // Converting to VOD: isLive flips to false AND trackDuration is added in
    // the same rebuild. Doing it in two steps would throw, because
    // validate_track rejects is_live together with track_duration_ms.
    const std::map<std::string, std::uint64_t> durations{{"video", 60000}, {"audio", 60000}};
    const MsfCatalog vod_cat =
        make_end_broadcast_catalog(live_now, EndBroadcastMode::kConvertToVod, durations);
    const std::string vod_json = serialize_catalog(vod_cat);
    ok &= expect_contains(vod_json, "\"isLive\":false", "expected VOD conversion to clear isLive");
    ok &= expect_contains(vod_json, "\"trackDuration\":60000", "expected trackDuration on VOD conversion");
    ok &= expect_not_contains(vod_json, "\"isLive\":true", "expected no track left live after conversion");
    ok &= expect_not_contains(vod_json, "\"generatedAt\":",
                              "expected generatedAt dropped on VOD conversion (MSF 5.1.2)");
    ok &= expect_not_contains(vod_json, "\"isComplete\"",
                              "expected no isComplete on a VOD conversion");

    // Terminating permanently: isComplete true and an EMPTY tracks array.
    const MsfCatalog term_cat =
        make_end_broadcast_catalog(live_now, EndBroadcastMode::kTerminate, {});
    const std::string term_json = serialize_catalog(term_cat);
    ok &= expect_contains(term_json, "\"isComplete\":true", "expected isComplete on termination");
    ok &= expect_contains(term_json, "\"tracks\":[]", "expected an empty tracks array on termination");
    ok &= expect_not_contains(term_json, "\"video\"", "expected no track entries after termination");

    // A track with no duration supplied still converts, just without the field.
    const MsfCatalog partial_cat = make_end_broadcast_catalog(
        live_now, EndBroadcastMode::kConvertToVod, {{"video", 60000}});
    const std::string partial_json = serialize_catalog(partial_cat);
    ok &= expect_contains(partial_json, "\"trackDuration\":60000", "expected the known duration");
    ok &= expect_contains(partial_json, "\"isLive\":false", "expected both tracks marked not live");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-msf-catalog-tests`
Expected: FAIL — `make_end_broadcast_catalog` and `EndBroadcastMode` are not declared.

- [ ] **Step 3: Declare in the header**

In `include/openmoq/publisher/msf_catalog.h`, add `#include <map>` and declare after `attach_init_data`:

```cpp
// How a live broadcast ends (MSF section 11.3).
enum class EndBroadcastMode {
    // The stream becomes a VOD asset: isLive false, trackDuration added.
    kConvertToVod,
    // The stream ends permanently: isComplete true, empty tracks array.
    kTerminate,
};

// Build the final independent catalog for a broadcast that is ending.
//
// kConvertToVod clears is_live and applies track_durations_ms in one rebuild.
// Both must change together: validate_track rejects a track that is live and
// also carries a duration, so flipping one field at a time would throw.
// generatedAt is dropped because MSF 5.1.2 says it SHOULD NOT appear when
// isLive is false.
//
// kTerminate returns a catalog with isComplete true and no tracks at all;
// track_durations_ms is ignored.
MsfCatalog make_end_broadcast_catalog(const MsfCatalog& current,
                                      EndBroadcastMode mode,
                                      const std::map<std::string, std::uint64_t>& track_durations_ms);
```

- [ ] **Step 4: Implement**

Append to `src/msf_catalog.cpp` inside `namespace openmoq::publisher`, after `attach_init_data`:

```cpp
MsfCatalog make_end_broadcast_catalog(const MsfCatalog& current,
                                      EndBroadcastMode mode,
                                      const std::map<std::string, std::uint64_t>& track_durations_ms) {
    MsfCatalog out;
    out.version = current.version;

    if (mode == EndBroadcastMode::kTerminate) {
        // Section 11.3: signal isComplete TRUE with an empty Tracks field.
        // Section 5.1.3: isComplete MUST NOT be removed once added.
        out.is_complete = true;
        return out;
    }

    // kConvertToVod. Section 5.1.2: generatedAt SHOULD NOT be included when
    // isLive is false, so it is deliberately not copied.
    out.init_data_list = current.init_data_list;
    out.tracks.reserve(current.tracks.size());
    for (MsfTrack track : current.tracks) {
        track.is_live = false;
        const auto duration_it = track_durations_ms.find(track.name);
        if (duration_it != track_durations_ms.end()) {
            track.track_duration_ms = duration_it->second;
        }
        out.tracks.push_back(std::move(track));
    }
    for (MsfTrack track : current.publish_tracks) {
        track.is_live = false;
        out.publish_tracks.push_back(std::move(track));
    }
    return out;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS, 13/13.

- [ ] **Step 6: Commit**

```bash
git add include/openmoq/publisher/msf_catalog.h src/msf_catalog.cpp tests/msf_catalog_test.cpp
git commit -m "Add end-of-broadcast catalog construction per MSF 11.3"
```

---

### Task 3: CatalogPublisher sequencing (Part B, independent catalogs)

**Files:**
- Modify: `include/openmoq/publisher/msf_catalog.h`
- Modify: `src/msf_catalog.cpp`
- Modify: `tests/msf_catalog_test.cpp`

**Interfaces:**
- Consumes: `MsfCatalog`, `serialize_catalog`, `EndBroadcastMode`, `make_end_broadcast_catalog`.
- Produces: `struct CatalogObject { std::uint64_t group_id; std::uint64_t object_id; std::uint64_t subgroup_id; std::string payload; };` and class `CatalogPublisher` with `std::vector<CatalogObject> publish(const MsfCatalog&)`, `std::vector<CatalogObject> end_broadcast(EndBroadcastMode, const std::map<std::string, std::uint64_t>&)`, `bool ended() const`. Task 4 and Task 5 extend this class.

- [ ] **Step 1: Write the failing test**

Append to `main()` in `tests/msf_catalog_test.cpp`:

```cpp
    // MSF section 5: object 0 of every group holds an independent catalog,
    // and all catalog objects map to sub-group 0.
    CatalogPublisher pub;
    const auto first = pub.publish(live_now);
    ok &= expect(first.size() == 1, "expected one object for the first catalog");
    ok &= expect(first[0].group_id == 0, "expected the first catalog in group 0");
    ok &= expect(first[0].object_id == 0, "expected the first catalog at object 0");
    ok &= expect(first[0].subgroup_id == 0, "expected all catalog objects in sub-group 0");
    ok &= expect_contains(first[0].payload, "\"version\":\"1\"", "expected a serialized catalog");

    // Republishing an unchanged catalog emits nothing.
    const auto unchanged = pub.publish(live_now);
    ok &= expect(unchanged.empty(), "expected no objects when the catalog is unchanged");

    // Ending the broadcast emits one independent catalog in a NEW group.
    const auto ended = pub.end_broadcast(EndBroadcastMode::kTerminate, {});
    ok &= expect(ended.size() == 1, "expected one object for the end-of-broadcast catalog");
    ok &= expect(ended[0].group_id == 1, "expected the final catalog in a new group");
    ok &= expect(ended[0].object_id == 0, "expected the final catalog at object 0");
    ok &= expect_contains(ended[0].payload, "\"isComplete\":true", "expected isComplete on termination");
    ok &= expect(pub.ended(), "expected the publisher to report the broadcast ended");

    // Section 5.1.3 and 5.2.7 are one-way transitions: nothing may follow.
    bool threw_after_end = false;
    try {
        (void)pub.publish(live_now);
    } catch (const std::runtime_error&) {
        threw_after_end = true;
    }
    ok &= expect(threw_after_end, "expected publishing after end_broadcast to throw");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-msf-catalog-tests`
Expected: FAIL — `CatalogPublisher` is not declared.

- [ ] **Step 3: Declare in the header**

Add to `include/openmoq/publisher/msf_catalog.h`:

```cpp
// One catalog object ready for the wire. MSF section 5 requires all catalog
// objects to map to MOQT sub-group 0.
struct CatalogObject {
    std::uint64_t group_id = 0;
    std::uint64_t object_id = 0;
    std::uint64_t subgroup_id = 0;
    std::string payload;
};

// Owns the catalog track's group and object counters and the last published
// catalog, and decides what to emit (MSF section 5).
//
// Object 0 of every group holds a full independent catalog. Producing an
// independent catalog always starts a new group.
class CatalogPublisher {
public:
    // Emit whatever is needed to move subscribers to `desired`. Returns an
    // empty vector when nothing changed. Throws std::runtime_error if the
    // broadcast has already ended.
    std::vector<CatalogObject> publish(const MsfCatalog& desired);

    // Emit the final catalog for a broadcast that is ending (MSF 11.3).
    // After this, publish() throws: section 5.1.3 forbids removing
    // isComplete, and 5.2.7 forbids a true isLive following a false one.
    std::vector<CatalogObject> end_broadcast(
        EndBroadcastMode mode,
        const std::map<std::string, std::uint64_t>& track_durations_ms);

    bool ended() const { return ended_; }

private:
    CatalogObject emit_independent(const MsfCatalog& catalog);

    std::optional<MsfCatalog> last_;
    std::uint64_t next_group_id_ = 0;
    std::uint64_t next_object_id_ = 0;
    bool ended_ = false;
};
```

- [ ] **Step 4: Implement**

Append to `src/msf_catalog.cpp` inside `namespace openmoq::publisher`. The equality helper is file-local; put it in an anonymous namespace above.

```cpp
namespace {

bool tracks_equal(const MsfTrack& a, const MsfTrack& b) {
    // Compare every field that serialize_catalog can emit. A field added to
    // MsfTrack without being added here would make a real change look like a
    // no-op, so keep this in step with write_track.
    return a.name == b.name && a.name_space == b.name_space && a.packaging == b.packaging &&
           a.role == b.role && a.is_live == b.is_live && a.target_latency_ms == b.target_latency_ms &&
           a.label == b.label && a.render_group == b.render_group && a.alt_group == b.alt_group &&
           a.init_ref == b.init_ref && a.depends == b.depends && a.codec == b.codec &&
           a.mime_type == b.mime_type && a.framerate == b.framerate && a.timescale == b.timescale &&
           a.bitrate == b.bitrate && a.avg_bitrate == b.avg_bitrate &&
           a.max_gop_duration_ms == b.max_gop_duration_ms &&
           a.max_group_duration_ms == b.max_group_duration_ms && a.width == b.width &&
           a.height == b.height && a.samplerate == b.samplerate &&
           a.channel_config == b.channel_config && a.lang == b.lang &&
           a.track_duration_ms == b.track_duration_ms && a.event_type == b.event_type &&
           a.max_grp_sap_starting_type == b.max_grp_sap_starting_type &&
           a.max_obj_sap_starting_type == b.max_obj_sap_starting_type &&
           a.custom_fields == b.custom_fields &&
           ((!a.buffers.has_value() && !b.buffers.has_value()) ||
            (a.buffers.has_value() && b.buffers.has_value() &&
             a.buffers->target_ms == b.buffers->target_ms && a.buffers->min_ms == b.buffers->min_ms &&
             a.buffers->max_ms == b.buffers->max_ms));
}

bool catalogs_equal(const MsfCatalog& a, const MsfCatalog& b) {
    if (a.version != b.version || a.is_complete != b.is_complete ||
        a.tracks.size() != b.tracks.size() || a.publish_tracks.size() != b.publish_tracks.size() ||
        a.init_data_list.size() != b.init_data_list.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.tracks.size(); ++i) {
        if (!tracks_equal(a.tracks[i], b.tracks[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.publish_tracks.size(); ++i) {
        if (!tracks_equal(a.publish_tracks[i], b.publish_tracks[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.init_data_list.size(); ++i) {
        if (a.init_data_list[i].id != b.init_data_list[i].id ||
            a.init_data_list[i].type != b.init_data_list[i].type ||
            a.init_data_list[i].data != b.init_data_list[i].data) {
            return false;
        }
    }
    // generated_at_ms is deliberately excluded: a fresh timestamp on an
    // otherwise identical catalog is not a change worth republishing.
    return true;
}

}  // namespace

CatalogObject CatalogPublisher::emit_independent(const MsfCatalog& catalog) {
    CatalogObject object;
    object.group_id = next_group_id_++;
    object.object_id = 0;
    object.subgroup_id = 0;
    object.payload = serialize_catalog(catalog);
    next_object_id_ = 1;
    return object;
}

std::vector<CatalogObject> CatalogPublisher::publish(const MsfCatalog& desired) {
    if (ended_) {
        throw std::runtime_error("MSF catalog publish after end_broadcast");
    }
    if (last_.has_value() && catalogs_equal(*last_, desired)) {
        return {};
    }
    std::vector<CatalogObject> out;
    out.push_back(emit_independent(desired));
    last_ = desired;
    return out;
}

std::vector<CatalogObject> CatalogPublisher::end_broadcast(
    EndBroadcastMode mode,
    const std::map<std::string, std::uint64_t>& track_durations_ms) {
    if (ended_) {
        throw std::runtime_error("MSF catalog end_broadcast called twice");
    }
    const MsfCatalog base = last_.value_or(MsfCatalog{});
    const MsfCatalog final_catalog = make_end_broadcast_catalog(base, mode, track_durations_ms);
    std::vector<CatalogObject> out;
    out.push_back(emit_independent(final_catalog));
    last_ = final_catalog;
    ended_ = true;
    return out;
}
```

Add `#include <optional>` and `#include <vector>` to the header if not already present.

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS, 13/13.

- [ ] **Step 6: Commit**

```bash
git add include/openmoq/publisher/msf_catalog.h src/msf_catalog.cpp tests/msf_catalog_test.cpp
git commit -m "Add CatalogPublisher group and object sequencing"
```

---

### Task 4: Delta updates with add and remove (Part B)

**Files:**
- Modify: `include/openmoq/publisher/msf_catalog.h`
- Modify: `src/msf_catalog.cpp`
- Modify: `tests/msf_catalog_test.cpp`

**Interfaces:**
- Consumes: `CatalogPublisher`, `CatalogObject`, `MsfCatalog` from Task 3.
- Produces: `struct MsfDeltaOp { std::string op; std::vector<MsfTrack> tracks; };`, `MsfCatalog::delta_update`, and `CatalogPublisher::set_max_deltas_per_group(std::size_t)`.

- [ ] **Step 1: Write the failing test**

Append to `main()` in `tests/msf_catalog_test.cpp`:

```cpp
    // Adding a track produces a delta at object ID 1 of the CURRENT group.
    CatalogPublisher dpub;
    (void)dpub.publish(live_now);
    MsfCatalog plus_one = live_now;
    MsfTrack extra;
    extra.name = "video-low";
    extra.packaging = "cmaf";
    extra.role = "video";
    extra.is_live = true;
    extra.codec = "avc1.64000d";
    extra.bitrate = 500000;
    plus_one.tracks.push_back(extra);

    const auto added = dpub.publish(plus_one);
    ok &= expect(added.size() == 1, "expected one object for an add delta");
    ok &= expect(added[0].group_id == 0, "expected the delta to stay in the current group");
    ok &= expect(added[0].object_id == 1, "expected the delta at object ID 1");
    ok &= expect_contains(added[0].payload, "\"deltaUpdate\"", "expected a deltaUpdate field");
    ok &= expect_contains(added[0].payload, "\"op\":\"add\"", "expected an add operation");
    ok &= expect_contains(added[0].payload, "\"video-low\"", "expected the added track name");
    // Section 5.3: a delta MUST NOT contain tracks or version.
    ok &= expect_not_contains(added[0].payload, "\"tracks\":", "expected no tracks field in a delta");
    ok &= expect_not_contains(added[0].payload, "\"version\":", "expected no version field in a delta");

    // Removing a track produces a remove carrying only name (and namespace).
    const auto removed = dpub.publish(live_now);
    ok &= expect(removed.size() == 1, "expected one object for a remove delta");
    ok &= expect(removed[0].object_id == 2, "expected the second delta at object ID 2");
    ok &= expect_contains(removed[0].payload, "\"op\":\"remove\"", "expected a remove operation");
    ok &= expect_contains(removed[0].payload, "\"video-low\"", "expected the removed track name");
    ok &= expect_not_contains(removed[0].payload, "\"bitrate\"",
                              "expected a remove entry to carry no attributes (MSF 5.1.6)");

    // Section 5.3 freezes attributes once a tuple is declared, so an attribute
    // change CANNOT be a delta. It must fall back to a full independent
    // catalog in a new group; emitting nothing would strand subscribers.
    MsfCatalog changed = live_now;
    changed.tracks[0].bitrate = 9000000;
    const auto rebuilt = dpub.publish(changed);
    ok &= expect(rebuilt.size() == 1, "expected one object for an attribute change");
    ok &= expect(rebuilt[0].object_id == 0, "expected an independent catalog, not a delta");
    ok &= expect(rebuilt[0].group_id == 1, "expected the rebuild in a new group");
    ok &= expect_contains(rebuilt[0].payload, "\"tracks\":", "expected a full catalog");
    ok &= expect_contains(rebuilt[0].payload, "\"bitrate\":9000000", "expected the new attribute");

    // Section 5.3: bound the deltas a joining subscriber must process.
    CatalogPublisher bounded;
    bounded.set_max_deltas_per_group(1);
    (void)bounded.publish(live_now);
    MsfCatalog b2 = live_now;
    b2.tracks.push_back(extra);
    const auto b_delta = bounded.publish(b2);
    ok &= expect(b_delta[0].object_id == 1, "expected the first delta at object 1");
    MsfCatalog b3 = b2;
    MsfTrack extra2 = extra;
    extra2.name = "video-mid";
    b3.tracks.push_back(extra2);
    const auto b_forced = bounded.publish(b3);
    ok &= expect(b_forced[0].object_id == 0, "expected the bound to force an independent catalog");
    ok &= expect(b_forced[0].group_id == 1, "expected the forced catalog in a new group");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-msf-catalog-tests`
Expected: FAIL — `set_max_deltas_per_group` is not declared and no delta is emitted.

- [ ] **Step 3: Add the delta types**

In `include/openmoq/publisher/msf_catalog.h`, add above `MsfCatalog`:

```cpp
// One operation in a delta update (MSF section 5.1.6). This version emits
// "add" and "remove" only. "clone" is deliberately unimplemented: it applies
// when a new track matches an existing one on every field except name, which
// no producer in this project generates.
struct MsfDeltaOp {
    std::string op;
    std::vector<MsfTrack> tracks;
};
```

Add to `MsfCatalog`, after `init_data_list`:

```cpp
    // When non-empty this catalog is a DELTA: section 5.3 requires it to
    // carry neither tracks nor version.
    std::vector<MsfDeltaOp> delta_update;
```

Add to `CatalogPublisher`'s public section:

```cpp
    // Section 5.3: producers publishing frequent deltas SHOULD periodically
    // publish a new independent catalog to bound the delta processing a
    // joining subscriber must perform. Default 8.
    void set_max_deltas_per_group(std::size_t max_deltas) { max_deltas_per_group_ = max_deltas; }
```

and to its private section:

```cpp
    std::size_t max_deltas_per_group_ = 8;
```

- [ ] **Step 4: Serialize deltas and validate the delta shape**

In `src/msf_catalog.cpp`, inside `serialize_catalog`, a delta catalog must emit neither `version` nor `tracks`. Restructure the top of the function:

```cpp
std::string serialize_catalog(const MsfCatalog& catalog) {
    validate_catalog(catalog);

    const bool is_delta = !catalog.delta_update.empty();

    std::ostringstream out;
    out << '{';
    JsonSeq seq(out);

    // Section 5.3: a delta update MUST NOT contain a Tracks field or an MSF
    // version field.
    if (!is_delta) {
        write_string(out, seq, "version", catalog.version);
    }
    if (catalog.generated_at_ms.has_value()) {
        write_uint(out, seq, "generatedAt", *catalog.generated_at_ms);
    }
    if (catalog.is_complete.has_value() && *catalog.is_complete) {
        write_bool(out, seq, "isComplete", true);
    }

    if (is_delta) {
        seq.separate();
        out << "\"deltaUpdate\":[";
        JsonSeq ops(out);
        for (const auto& op : catalog.delta_update) {
            ops.separate();
            out << "{\"op\":\"" << json_escape(op.op) << "\",\"tracks\":[";
            JsonSeq op_tracks(out);
            for (const auto& track : op.tracks) {
                op_tracks.separate();
                if (op.op == "remove") {
                    // Section 5.1.6: a remove entry MUST include the track
                    // name, MAY include the namespace, and MUST NOT hold any
                    // other field. The differ builds remove operations from
                    // the previously published tracks, which are complete, so
                    // narrowing to name and namespace HERE is deliberate and
                    // required -- it is not a lossy accident. Validating the
                    // input instead would reject the differ's own output.
                    out << "{\"name\":\"" << json_escape(track.name) << '"';
                    if (track.name_space.has_value()) {
                        out << ",\"namespace\":\"" << json_escape(*track.name_space) << '"';
                    }
                    out << '}';
                } else {
                    write_track(out, track);
                }
            }
            out << "]}";
        }
        out << ']';
    } else {
        write_track_array(out, seq, "tracks", catalog.tracks);
    }
```

The remainder of the function (publish tracks, init data list, closing brace) is unchanged, except that both must be skipped for a delta. Guard them:

```cpp
    if (!is_delta && !catalog.publish_tracks.empty()) {
        write_track_array(out, seq, "publishTracks", catalog.publish_tracks);
    }
    if (!is_delta && !catalog.init_data_list.empty()) {
```

In `validate_catalog`, add at the top:

```cpp
    // Section 5.3: a delta update MUST include at least one operation and
    // MUST NOT carry tracks. Only "add" and "remove" are emitted by this
    // implementation.
    if (!catalog.delta_update.empty()) {
        if (!catalog.tracks.empty()) {
            throw std::runtime_error("MSF delta catalog must not carry a tracks array");
        }
        for (const auto& op : catalog.delta_update) {
            if (op.op != "add" && op.op != "remove") {
                throw std::runtime_error("MSF delta catalog has unsupported operation \"" + op.op + "\"");
            }
            if (op.tracks.empty()) {
                throw std::runtime_error("MSF delta operation \"" + op.op + "\" has no tracks");
            }
        }
        return;
    }
```

Note the early `return`: the remaining catalog-wide checks (unique names, `initRef` resolution) apply to full catalogs, and a delta has neither array to check.

- [ ] **Step 5: Implement the diff**

Replace `CatalogPublisher::publish` in `src/msf_catalog.cpp`:

```cpp
std::vector<CatalogObject> CatalogPublisher::publish(const MsfCatalog& desired) {
    if (ended_) {
        throw std::runtime_error("MSF catalog publish after end_broadcast");
    }
    if (!last_.has_value()) {
        std::vector<CatalogObject> out;
        out.push_back(emit_independent(desired));
        last_ = desired;
        deltas_in_group_ = 0;
        return out;
    }
    if (catalogs_equal(*last_, desired)) {
        return {};
    }

    // Key tracks by the namespace and name tuple, which section 5.3 treats as
    // the identity of a track.
    const auto key_of = [](const MsfTrack& track) {
        return std::pair<std::string, std::string>(track.name_space.value_or(std::string{}), track.name);
    };
    std::map<std::pair<std::string, std::string>, const MsfTrack*> before;
    for (const auto& track : last_->tracks) {
        before.emplace(key_of(track), &track);
    }
    std::map<std::pair<std::string, std::string>, const MsfTrack*> after;
    for (const auto& track : desired.tracks) {
        after.emplace(key_of(track), &track);
    }

    std::vector<MsfTrack> added;
    std::vector<MsfTrack> removed;
    for (const auto& [key, track] : after) {
        const auto it = before.find(key);
        if (it == before.end()) {
            added.push_back(*track);
        } else if (!tracks_equal(*it->second, *track)) {
            // Section 5.3 freezes a track's attributes once its tuple is
            // declared, so an attribute change cannot be expressed as a
            // delta. Fall back to a full independent catalog: emitting
            // nothing would leave subscribers on stale attributes.
            std::vector<CatalogObject> out;
            out.push_back(emit_independent(desired));
            last_ = desired;
            deltas_in_group_ = 0;
            return out;
        }
    }
    for (const auto& [key, track] : before) {
        if (after.find(key) == after.end()) {
            removed.push_back(*track);
        }
    }

    // Anything other than a pure add/remove of whole tracks, including a
    // change to initDataList or publishTracks, needs a full catalog.
    const bool init_data_changed = last_->init_data_list.size() != desired.init_data_list.size();
    if ((added.empty() && removed.empty()) || init_data_changed ||
        deltas_in_group_ >= max_deltas_per_group_) {
        std::vector<CatalogObject> out;
        out.push_back(emit_independent(desired));
        last_ = desired;
        deltas_in_group_ = 0;
        return out;
    }

    MsfCatalog delta;
    if (!added.empty()) {
        delta.delta_update.push_back(MsfDeltaOp{.op = "add", .tracks = std::move(added)});
    }
    if (!removed.empty()) {
        delta.delta_update.push_back(MsfDeltaOp{.op = "remove", .tracks = std::move(removed)});
    }

    CatalogObject object;
    // Stay in the current group. next_group_id_ is at least 1 here: this
    // branch is only reachable when last_ has a value, which means
    // emit_independent already ran and incremented it. Do not hoist this
    // expression above that guard.
    object.group_id = next_group_id_ - 1;
    object.object_id = next_object_id_++;
    object.subgroup_id = 0;
    object.payload = serialize_catalog(delta);

    last_ = desired;
    ++deltas_in_group_;
    return {object};
}
```

Add `std::size_t deltas_in_group_ = 0;` to `CatalogPublisher`'s private section, and reset it to 0 in `emit_independent`. Add `#include <utility>` to `src/msf_catalog.cpp` for `std::pair`.

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS, 13/13.

- [ ] **Step 7: Commit**

```bash
git add include/openmoq/publisher/msf_catalog.h src/msf_catalog.cpp tests/msf_catalog_test.cpp
git commit -m "Add add and remove delta updates to CatalogPublisher"
```

---

### Task 5: Publisher end_broadcast API and periodic republish config (Parts A1, A2)

**Files:**
- Modify: `include/openmoq/publisher/publisher_api.h`
- Modify: `src/publisher_api.cpp`
- Modify: `tests/publisher_api_test.cpp`

**Interfaces:**
- Consumes: `EndBroadcastMode`, `CatalogPublisher` from Tasks 2-4.
- Produces: `PublisherConfig::catalog_republish_interval`, `Publisher::end_broadcast(EndBroadcastMode)`.

- [ ] **Step 1: Write the failing test**

`tests/publisher_api_test.cpp` already uses the project's `expect` style. Append to its `main()`:

```cpp
    // The republish interval is off by default: existing deployments keep
    // their current wire behaviour.
    PublisherConfig default_config;
    ok &= expect(default_config.catalog_republish_interval == std::chrono::seconds(0),
                 "expected catalog republication disabled by default");
    ok &= expect(!default_config.vod, "expected the publisher to default to live");

    // end_broadcast on a publisher that never connected reports failure
    // rather than throwing, matching how disconnect() behaves.
    Publisher idle_publisher;
    const auto end_status = idle_publisher.end_broadcast(EndBroadcastMode::kTerminate);
    ok &= expect(!end_status.ok, "expected end_broadcast without a session to fail cleanly");
```

Add `#include <chrono>` if the file does not already have it.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-api-tests && ctest --test-dir build -R openmoq-publisher-api-tests --output-on-failure`
Expected: FAIL — `catalog_republish_interval` and `end_broadcast` are not declared.

- [ ] **Step 3: Extend the config and API**

In `include/openmoq/publisher/publisher_api.h`, add `#include "openmoq/publisher/msf_catalog.h"` if not already present, then add to `PublisherConfig` after the `vod` field from Task 1:

```cpp
    // Section 5: a catalog SHOULD be republished after enough time has passed
    // that it might fall out of a relay cache. Zero disables republication,
    // which is the historical behaviour.
    std::chrono::seconds catalog_republish_interval{0};
```

Add to `Publisher`'s public section, after `disconnect`:

```cpp
    // End the broadcast per MSF section 11.3: send PUBLISH_DONE with status
    // 0x2 Track Ended for all active tracks, then publish one final
    // independent catalog. kConvertToVod marks every track not live and adds
    // its duration; kTerminate signals isComplete with an empty tracks array.
    // Returns a failure status when no session is active.
    transport::TransportStatus end_broadcast(EndBroadcastMode mode) const;
```

- [ ] **Step 4: Implement**

`Publisher::ActiveSession` (`src/publisher_api.cpp:74-77`) is a thin holder:

```cpp
struct Publisher::ActiveSession {
    std::unique_ptr<transport::PublisherTransport> transport;
    std::unique_ptr<transport::MoqtSession> session;
};
```

`Publisher::disconnect` (`:516`) reaches the session by copying the shared
pointer under `state_mutex_`, releasing the lock, then calling through. Mirror
that exactly. Add to `src/publisher_api.cpp` immediately after `disconnect`:

```cpp
transport::TransportStatus Publisher::end_broadcast(EndBroadcastMode mode) const {
    std::shared_ptr<ActiveSession> active;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        active = active_session_;
    }
    if (!active || !active->session) {
        return transport::TransportStatus::failure("end_broadcast requires an active session");
    }
    // Held without the lock: end_broadcast blocks on the wire, and holding
    // state_mutex_ across a write would deadlock a concurrent stats() call,
    // which is why disconnect() releases it before calling close().
    return active->session->end_broadcast(mode);
}
```

Then add the matching method to `MoqtSession`. Declare in
`include/openmoq/publisher/transport/moqt_session.h`, beside `close`:

```cpp
    // MSF section 11.3. Sends PUBLISH_DONE with status 0x2 Track Ended for
    // every active request, then publishes one final independent catalog.
    TransportStatus end_broadcast(EndBroadcastMode mode);
```

The implementation belongs with the other request-completion helpers in
`src/transport/moqt_session.cpp`, and reuses `write_publish_done_for_request`
(`:1432`), which already encodes `kPublishStatusTrackEnded = 0x2`. The final
catalog comes from `CatalogPublisher::end_broadcast(mode, durations)`, whose
returned `CatalogObject` is sent on the catalog track exactly like any other
catalog object on that path.

Track durations for `kConvertToVod` come from each track's accumulated media
time. If the session does not track that today, pass an empty map: the
resulting catalog still correctly marks every track not live, and
`make_end_broadcast_catalog` simply omits `trackDuration` for tracks with no
entry. Note that in your report so the gap is visible rather than silent.

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS, 13/13.

- [ ] **Step 6: Commit**

```bash
git add include/openmoq/publisher/publisher_api.h src/publisher_api.cpp tests/publisher_api_test.cpp
git commit -m "Add Publisher::end_broadcast and catalog republish interval"
```

---

### Task 6: Wire CatalogPublisher into the transport and document Phase 2

**Files:**
- Modify: `include/openmoq/publisher/msf_catalog.h` (add `force_independent`)
- Modify: `src/msf_catalog.cpp` (implement `force_independent`)
- Modify: `src/transport/moqt_session.cpp:3099-3124`, `:3513-3539`
- Modify: `tests/msf_catalog_test.cpp`
- Modify: `docs/status.md`
- Modify: `docs/protocol-mapping.md`

**Interfaces:**
- Consumes: `CatalogPublisher`, `CatalogObject`, `PublisherConfig::catalog_republish_interval`.
- Produces: nothing. Final task of Phase 2.

- [ ] **Step 1: Write the failing test**

The transport-level republish timing is awkward to test through the mock
transport, because it depends on wall-clock elapsing mid-publish. Test the
sequencing where it lives instead — in `CatalogPublisher`, which owns the
group and object numbering and has no transport dependency.

Add to `tests/msf_catalog_test.cpp`:

```cpp
    // Section 5: cache-expiry republication re-emits the SAME catalog as a
    // fresh independent copy in a new group, at object 0.
    CatalogPublisher rpub;
    const auto r1 = rpub.publish(live_now);
    ok &= expect(r1.size() == 1, "expected the initial catalog");
    ok &= expect(r1[0].group_id == 0 && r1[0].object_id == 0,
                 "expected the initial catalog at group 0 object 0");

    const auto r2 = rpub.force_independent();
    ok &= expect(r2.size() == 1, "expected a republished catalog");
    ok &= expect(r2[0].group_id == 1, "expected the republished catalog in the next group");
    ok &= expect(r2[0].object_id == 0, "expected the republished catalog at object 0");
    ok &= expect(r2[0].subgroup_id == 0, "expected all catalog objects in sub-group 0");
    ok &= expect(r2[0].payload == r1[0].payload,
                 "expected republication to re-send identical content");

    // A republish before anything has been published emits nothing rather
    // than serializing an empty catalog.
    CatalogPublisher empty_pub;
    ok &= expect(empty_pub.force_independent().empty(),
                 "expected no republication before the first publish");

    // force_independent is refused after the broadcast ends, like publish().
    CatalogPublisher done_pub;
    (void)done_pub.publish(live_now);
    (void)done_pub.end_broadcast(EndBroadcastMode::kTerminate, {});
    bool threw_force = false;
    try {
        (void)done_pub.force_independent();
    } catch (const std::runtime_error&) {
        threw_force = true;
    }
    ok &= expect(threw_force, "expected force_independent to throw after end_broadcast");
```

For the transport itself, assert only that the latch is gone: with
`catalog_republish_interval` at its default of zero, exactly one catalog
object is sent, matching today's behaviour. That is a behaviour-preservation
check, and it is what the existing `tests/moqt_session_test.cpp` cases around
line 1330 already exercise — verify they still pass unchanged rather than
adding a timing-dependent case.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-msf-catalog-tests`
Expected: FAIL — `force_independent` is not declared.

- [ ] **Step 3: Replace the catalog_sent latch**

At `src/transport/moqt_session.cpp:3099` and `:3513`, the `catalog_sent` bool prevents any republication. Replace each with a `CatalogPublisher` member plus a `std::chrono::steady_clock::time_point last_catalog_sent_at_`, and have the send path:

- call `publish()` on the first subscription, sending every returned `CatalogObject` at its own `group_id`, `object_id`, and `subgroup_id`;
- when `catalog_republish_interval` is non-zero and that much time has elapsed, call `publish()` again with the same catalog and send whatever it returns.

Because `publish()` returns an empty vector for an unchanged catalog, a republish must force an independent emission rather than relying on the diff. Add a `force_independent()` method to `CatalogPublisher` that emits the last catalog again in a new group, and call that on the interval. Declare it in `include/openmoq/publisher/msf_catalog.h`:

```cpp
    // Re-emit the last published catalog as a fresh independent copy in a new
    // group, for section 5's cache-expiry republication. Returns an empty
    // vector if nothing has been published yet.
    std::vector<CatalogObject> force_independent();
```

and implement it in `src/msf_catalog.cpp`:

```cpp
std::vector<CatalogObject> CatalogPublisher::force_independent() {
    if (ended_) {
        throw std::runtime_error("MSF catalog force_independent after end_broadcast");
    }
    if (!last_.has_value()) {
        return {};
    }
    std::vector<CatalogObject> out;
    out.push_back(emit_independent(*last_));
    return out;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS, 13/13.

- [ ] **Step 5: Update the documentation**

In `docs/status.md`, extend the MSF/CMSF roadmap item to record that Phase 2 shipped: live-by-default with VOD opt-in, end-of-broadcast signalling per section 11.3, optional periodic republication, and add/remove delta updates. State plainly that `clone` delta operations are not implemented, and that Phases 3 and 4 — CMSF content protection and MSF URL parsing — remain.

In `docs/protocol-mapping.md`, extend the "MSF v1 catalog" section with the lifecycle contract: object 0 of every group holds an independent catalog, deltas occupy object IDs 1 and above within the current group, all catalog objects map to sub-group 0, and an attribute change forces a full independent catalog because section 5.3 freezes a track's attributes once its namespace and name tuple is declared.

Do not claim CMSF content protection, MSF URL parsing, log or metrics tracks, compression signalling, or `clone` deltas are implemented. None of them are.

- [ ] **Step 6: Run the full suite**

Run:
```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: PASS, 13/13.

- [ ] **Step 7: Commit**

```bash
git add src/transport/moqt_session.cpp include/openmoq/publisher/msf_catalog.h src/msf_catalog.cpp tests/moqt_session_test.cpp docs/status.md docs/protocol-mapping.md
git commit -m "Wire CatalogPublisher into the transport and document Phase 2"
```

---

## Out of Scope for Phase 2

- `clone` delta operations. No producer generates a track matching another on every field except name.
- A JSON reader. The publisher holds the previous catalog as a struct, so diffing needs no parsing.
- Joining FETCH. Section 5 binds subscribers to use it; moqxr is a publisher.
- MSF section 12 compression signalling, still blocked on transport draft-19 Track and Object Properties.
- Phase 3 (CMSF content protection) and Phase 4 (MSF URL parsing).
