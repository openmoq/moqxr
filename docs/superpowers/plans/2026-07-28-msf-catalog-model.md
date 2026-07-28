# MSF v1 Catalog Model Implementation Plan (Phase 1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace moqxr's four divergent, pre-draft catalog emitters with one structured `MsfCatalog` model that serializes a catalog conforming to `draft-ietf-moq-msf-01` and `draft-ietf-moq-cmsf-01`.

**Architecture:** A new `msf_catalog` module owns the catalog document: plain structs (`MsfCatalog`, `MsfTrack`, `MsfInitData`) plus a `serialize_catalog()` free function backed by a small internal JSON writer. The four existing emitters — batch publish plan, live catalog, CTE LL-DASH ingest, MSFTS example — shrink to populating those structs. `TrackDescription` and `extract_tracks` gain the track metadata MSF requires but moqxr never parsed: bitrate and language.

**Tech Stack:** C++20, CMake, static library `openmoq_publisher_lib`, hand-rolled single-binary tests using an `expect(bool, message)` helper returning a process exit code.

## Global Constraints

- Language standard: C++20. No new third-party dependencies — the project vendors only picoquic and picotls, and the JSON writer is internal.
- Catalog `version` field is the **String** `"1"`, never a Number.
- No root `format` field. It does not exist in either draft.
- `initDataList` MUST be serialized **after** `tracks` (MSF section 5.1.7).
- Spec field names exactly: `framerate`, `samplerate`, `channelConfig` (String), `initRef`, `bitrate`, `altGroup`, `renderGroup`, `maxGrpSapStartingType`, `maxObjSapStartingType`.
- No `id` field on track objects. It is not in the spec.
- Warnings are enabled (`-Wall -Wextra -Wpedantic` on GCC/Clang, `/W4` on MSVC). Code must compile clean.
- Namespace: `openmoq::publisher`.
- Header guard style: `#pragma once`.
- Build and test: `cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF && cmake --build build && ctest --test-dir build --output-on-failure`
- Commit messages: no emoji, no "Generated with Claude Code" tagline, no Co-Authored-By line.

## File Structure

| File | Responsibility |
| --- | --- |
| `include/openmoq/publisher/msf_catalog.h` (create) | `MsfCatalog`, `MsfTrack`, `MsfBuffers`, `MsfInitData` structs; `serialize_catalog()`, `resolve_bitrate()`, `make_msf_track()` declarations |
| `src/msf_catalog.cpp` (create) | JSON writer, validation, serialization, shared track builder |
| `tests/msf_catalog_test.cpp` (create) | Conformance tests against draft examples |
| `include/openmoq/publisher/mp4_box.h` (modify) | `TrackDescription` gains bitrate/language/duration fields |
| `src/mp4_box.cpp` (modify) | `btrt` parsing, `mdhd` language decoding, computed-bitrate fallback |
| `src/cmsf_packager.cpp` (modify) | Batch and live emitters populate `MsfCatalog` |
| `src/live_dash_ingest.cpp` (modify) | CTE emitter populates `MsfCatalog` |
| `examples/msfts-publisher/msfts_source.cpp` (modify) | MSFTS emitter populates `MsfCatalog` |
| `CMakeLists.txt` (modify) | Register new source and test target |

---

### Task 1: MSF catalog structs and JSON writer

**Files:**
- Create: `include/openmoq/publisher/msf_catalog.h`
- Create: `src/msf_catalog.cpp`
- Create: `tests/msf_catalog_test.cpp`
- Modify: `CMakeLists.txt:254-271` (library sources), `CMakeLists.txt:357` (test target)

**Interfaces:**
- Consumes: nothing.
- Produces: `openmoq::publisher::MsfCatalog`, `MsfTrack`, `MsfInitData`, and `std::string serialize_catalog(const MsfCatalog&)`. Later tasks populate these structs and call this function.

- [ ] **Step 1: Write the failing test**

Create `tests/msf_catalog_test.cpp`:

```cpp
#include "openmoq/publisher/msf_catalog.h"

#include <iostream>
#include <string>
#include <string_view>

namespace {

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

bool expect_contains(std::string_view haystack, std::string_view needle, const std::string& message) {
    return expect(haystack.find(needle) != std::string_view::npos, message);
}

bool expect_not_contains(std::string_view haystack, std::string_view needle, const std::string& message) {
    return expect(haystack.find(needle) == std::string_view::npos, message);
}

}  // namespace

int main() {
    using namespace openmoq::publisher;

    bool ok = true;

    // MSF section 5.1.1: version is a String, not a Number.
    MsfCatalog minimal;
    MsfTrack video;
    video.name = "video";
    video.packaging = "cmaf";
    video.role = "video";
    video.is_live = true;
    video.codec = "avc1.640028";
    video.bitrate = 5000000;
    video.width = 1920;
    video.height = 1080;
    video.framerate = 30.0;
    minimal.tracks.push_back(video);

    const std::string json = serialize_catalog(minimal);
    ok &= expect_contains(json, "\"version\":\"1\"", "expected version as a JSON string");
    ok &= expect_not_contains(json, "\"version\":1", "expected no numeric version");
    ok &= expect_not_contains(json, "\"format\"", "expected no non-spec format field");
    ok &= expect_not_contains(json, "\"id\":", "expected no non-spec track id field");
    ok &= expect_contains(json, "\"packaging\":\"cmaf\"", "expected cmaf packaging");
    ok &= expect_contains(json, "\"bitrate\":5000000", "expected bitrate");
    ok &= expect_contains(json, "\"framerate\":30", "expected spec framerate spelling");
    ok &= expect_not_contains(json, "\"frameRate\"", "expected no legacy frameRate spelling");

    return ok ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-msf-catalog-tests`
Expected: FAIL — `openmoq/publisher/msf_catalog.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `include/openmoq/publisher/msf_catalog.h`:

```cpp
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace openmoq::publisher {

// One entry of the root initDataList (MSF section 5.1.7). Only the "inline"
// type is defined in version 1; data is Base64-encoded initialization data.
struct MsfInitData {
    std::string id;
    std::string type = "inline";
    std::string data;
};

// Target buffer object (MSF section 5.2.9). All keys are optional. Mutually
// exclusive with targetLatency within a single track.
struct MsfBuffers {
    std::optional<std::uint64_t> target_ms;
    std::optional<std::uint64_t> min_ms;
    std::optional<std::uint64_t> max_ms;
};

// A track object (MSF section 5.2). Field names mirror the spec exactly.
// std::optional models absence so the serializer never emits a field the
// drafts say MUST NOT appear.
struct MsfTrack {
    std::string name;                                  // 5.2.3, required
    std::optional<std::string> name_space;             // 5.2.2 ("namespace")
    std::string packaging;                             // 5.2.4, required
    std::optional<std::string> role;                   // 5.2.6
    bool is_live = false;                              // 5.2.7, required
    std::optional<std::uint64_t> target_latency_ms;    // 5.2.8
    std::optional<MsfBuffers> buffers;                 // 5.2.9
    std::optional<std::string> label;                  // 5.2.10
    std::optional<std::uint32_t> render_group;         // 5.2.11
    std::optional<std::uint32_t> alt_group;            // 5.2.12
    std::optional<std::string> init_ref;               // 5.2.13
    std::vector<std::string> depends;                  // 5.2.14
    std::optional<std::string> codec;                  // 5.2.18
    std::optional<std::string> mime_type;              // 5.2.19
    std::optional<double> framerate;                   // 5.2.20
    std::optional<std::uint32_t> timescale;            // 5.2.21
    std::optional<std::uint64_t> bitrate;              // 5.2.22, MUST for a/v
    std::optional<std::uint64_t> avg_bitrate;          // 5.2.23
    std::optional<std::uint64_t> max_gop_duration_ms;  // 5.2.24
    std::optional<std::uint64_t> max_group_duration_ms;// 5.2.25
    std::optional<std::uint32_t> width;                // 5.2.26
    std::optional<std::uint32_t> height;               // 5.2.27
    std::optional<std::uint32_t> samplerate;           // 5.2.28
    std::optional<std::string> channel_config;         // 5.2.29, a String
    std::optional<std::string> lang;                   // 5.2.32
    std::optional<std::uint64_t> track_duration_ms;    // 5.2.35
    std::optional<std::string> event_type;             // 5.2.5, eventtimeline only

    // CMSF section 3.5.2.
    std::optional<std::uint32_t> max_grp_sap_starting_type;
    std::optional<std::uint32_t> max_obj_sap_starting_type;

    // Producer-defined fields. MSF section 5 permits these provided the names
    // do not collide with spec field names; the serializer enforces that.
    // Values are raw JSON, so a string value must arrive already quoted.
    std::map<std::string, std::string> custom_fields;
};

// The root catalog object (MSF section 5.1).
struct MsfCatalog {
    std::string version = "1";                    // 5.1.1, a String
    std::optional<std::uint64_t> generated_at_ms; // 5.1.2
    std::optional<bool> is_complete;              // 5.1.3
    std::vector<MsfTrack> tracks;                 // 5.1.4
    std::vector<MsfTrack> publish_tracks;         // 5.1.5
    std::vector<MsfInitData> init_data_list;      // 5.1.7, emitted after tracks
};

// Serialize to a JSON catalog document. Throws std::runtime_error when a
// draft invariant is violated; the message names the offending track.
std::string serialize_catalog(const MsfCatalog& catalog);

}  // namespace openmoq::publisher
```

- [ ] **Step 4: Write the implementation**

Create `src/msf_catalog.cpp`:

```cpp
#include "openmoq/publisher/msf_catalog.h"

#include <cmath>
#include <iomanip>
#include <ios>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace openmoq::publisher {

namespace {

std::string json_escape(std::string_view value) {
    std::ostringstream out;
    for (const char ch : value) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(ch)) << std::dec;
                } else {
                    out << ch;
                }
                break;
        }
    }
    return out.str();
}

// Emit a JSON number without a trailing ".0" for whole values, so framerate 30
// serializes as 30 rather than 30.000000.
std::string json_number(double value) {
    std::ostringstream out;
    const double rounded = std::round(value);
    if (std::fabs(value - rounded) < 0.0005) {
        out << static_cast<long long>(rounded);
    } else {
        out << std::fixed << std::setprecision(3) << value;
    }
    return out.str();
}

// Comma bookkeeping for a JSON object or array under construction.
class JsonSeq {
public:
    explicit JsonSeq(std::ostringstream& out) : out_(out) {}

    void separate() {
        if (!first_) {
            out_ << ',';
        }
        first_ = false;
    }

private:
    std::ostringstream& out_;
    bool first_ = true;
};

void write_string(std::ostringstream& out, JsonSeq& seq, std::string_view key, std::string_view value) {
    seq.separate();
    out << '"' << json_escape(key) << "\":\"" << json_escape(value) << '"';
}

void write_raw(std::ostringstream& out, JsonSeq& seq, std::string_view key, std::string_view raw) {
    seq.separate();
    out << '"' << json_escape(key) << "\":" << raw;
}

void write_uint(std::ostringstream& out, JsonSeq& seq, std::string_view key, std::uint64_t value) {
    write_raw(out, seq, key, std::to_string(value));
}

void write_bool(std::ostringstream& out, JsonSeq& seq, std::string_view key, bool value) {
    write_raw(out, seq, key, value ? "true" : "false");
}

void write_string_array(std::ostringstream& out,
                        JsonSeq& seq,
                        std::string_view key,
                        const std::vector<std::string>& values) {
    seq.separate();
    out << '"' << json_escape(key) << "\":[";
    JsonSeq inner(out);
    for (const auto& value : values) {
        inner.separate();
        out << '"' << json_escape(value) << '"';
    }
    out << ']';
}

bool is_media_role(const MsfTrack& track) {
    return track.role.has_value() && (*track.role == "video" || *track.role == "audio");
}

// MSF and CMSF invariants that a malformed caller could otherwise publish.
void validate_track(const MsfTrack& track) {
    const std::string where = " (track \"" + track.name + "\")";

    if (track.name.empty()) {
        throw std::runtime_error("MSF catalog track requires a name");
    }
    if (track.packaging.empty()) {
        throw std::runtime_error("MSF catalog requires a packaging value" + where);
    }
    // Section 5.2.22: bitrate MUST be specified for audio and video tracks.
    if (is_media_role(track) && !track.bitrate.has_value()) {
        throw std::runtime_error("MSF catalog requires bitrate for audio and video tracks" + where);
    }
    // Section 5.2.5: eventType is required for, and restricted to, eventtimeline.
    if (track.packaging == "eventtimeline" && !track.event_type.has_value()) {
        throw std::runtime_error("MSF catalog requires eventType for eventtimeline packaging" + where);
    }
    if (track.packaging != "eventtimeline" && track.event_type.has_value()) {
        throw std::runtime_error("MSF catalog forbids eventType outside eventtimeline packaging" + where);
    }
    // Section 5.2.35: trackDuration MUST NOT be included when isLive is true.
    if (track.is_live && track.track_duration_ms.has_value()) {
        throw std::runtime_error("MSF catalog forbids trackDuration on a live track" + where);
    }
    // Section 5.2.28 and 5.2.29: both MUST accompany an audio codec.
    if (track.role.has_value() && *track.role == "audio") {
        if (!track.samplerate.has_value() || !track.channel_config.has_value()) {
            throw std::runtime_error("MSF catalog requires samplerate and channelConfig for audio" + where);
        }
    }
    // Sections 5.2.8 and 5.2.9: targetLatency MUST NOT be present if buffers is.
    if (track.target_latency_ms.has_value() && track.buffers.has_value()) {
        throw std::runtime_error("MSF catalog forbids both targetLatency and buffers" + where);
    }
    // Section 5: custom field names MUST NOT collide with spec field names.
    static const std::set<std::string> kSpecFieldNames = {
        "name", "namespace", "packaging", "eventType", "role", "isLive",
        "targetLatency", "buffers", "label", "renderGroup", "altGroup",
        "initRef", "depends", "codec", "mimeType", "framerate", "timescale",
        "bitrate", "avgBitrate", "maxGopDuration", "maxGroupDuration", "width",
        "height", "samplerate", "channelConfig", "lang", "trackDuration",
        "maxGrpSapStartingType", "maxObjSapStartingType", "temporalId",
        "spatialId", "displayWidth", "displayHeight", "parentName",
        "parentNamespace", "template", "authInfo", "accessibility",
        "encryptionScheme", "cipherSuite", "keyId", "trackBaseKey",
        "connectionURI", "token", "contentProtectionRefIDs",
    };
    for (const auto& [key, value] : track.custom_fields) {
        if (kSpecFieldNames.count(key) != 0) {
            throw std::runtime_error("MSF catalog custom field \"" + key +
                                     "\" collides with a spec field name" + where);
        }
        if (value.empty()) {
            throw std::runtime_error("MSF catalog custom field \"" + key +
                                     "\" has an empty raw JSON value" + where);
        }
    }
}

// Catalog-wide invariants that cannot be checked from a single track.
void validate_catalog(const MsfCatalog& catalog) {
    std::set<std::string> init_ids;
    for (const auto& entry : catalog.init_data_list) {
        if (!init_ids.insert(entry.id).second) {
            throw std::runtime_error("MSF catalog has a duplicate initDataList id \"" + entry.id + "\"");
        }
    }

    // Section 5.2.3: track names MUST be unique per namespace.
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto& track : catalog.tracks) {
        const std::string ns = track.name_space.value_or(std::string{});
        if (!seen.insert({ns, track.name}).second) {
            throw std::runtime_error("MSF catalog has a duplicate track name \"" + track.name +
                                     "\" within one namespace");
        }
        // Section 5.2.13: initRef points at an id in the initDataList.
        if (track.init_ref.has_value() && init_ids.count(*track.init_ref) == 0) {
            throw std::runtime_error("MSF catalog initRef \"" + *track.init_ref +
                                     "\" has no matching initDataList entry (track \"" + track.name + "\")");
        }
    }
}

void write_track(std::ostringstream& out, const MsfTrack& track) {
    validate_track(track);

    out << '{';
    JsonSeq seq(out);

    write_string(out, seq, "name", track.name);
    if (track.name_space.has_value()) {
        write_string(out, seq, "namespace", *track.name_space);
    }
    write_string(out, seq, "packaging", track.packaging);
    if (track.event_type.has_value()) {
        write_string(out, seq, "eventType", *track.event_type);
    }
    if (track.role.has_value()) {
        write_string(out, seq, "role", *track.role);
    }
    write_bool(out, seq, "isLive", track.is_live);
    if (track.target_latency_ms.has_value()) {
        write_uint(out, seq, "targetLatency", *track.target_latency_ms);
    }
    if (track.buffers.has_value()) {
        seq.separate();
        out << "\"buffers\":{";
        JsonSeq buf(out);
        if (track.buffers->target_ms.has_value()) {
            write_uint(out, buf, "target", *track.buffers->target_ms);
        }
        if (track.buffers->min_ms.has_value()) {
            write_uint(out, buf, "min", *track.buffers->min_ms);
        }
        if (track.buffers->max_ms.has_value()) {
            write_uint(out, buf, "max", *track.buffers->max_ms);
        }
        out << '}';
    }
    if (track.label.has_value()) {
        write_string(out, seq, "label", *track.label);
    }
    if (track.render_group.has_value()) {
        write_uint(out, seq, "renderGroup", *track.render_group);
    }
    if (track.alt_group.has_value()) {
        write_uint(out, seq, "altGroup", *track.alt_group);
    }
    if (track.init_ref.has_value()) {
        write_string(out, seq, "initRef", *track.init_ref);
    }
    if (!track.depends.empty()) {
        write_string_array(out, seq, "depends", track.depends);
    }
    if (track.codec.has_value()) {
        write_string(out, seq, "codec", *track.codec);
    }
    if (track.mime_type.has_value()) {
        write_string(out, seq, "mimeType", *track.mime_type);
    }
    if (track.framerate.has_value()) {
        write_raw(out, seq, "framerate", json_number(*track.framerate));
    }
    if (track.timescale.has_value()) {
        write_uint(out, seq, "timescale", *track.timescale);
    }
    if (track.bitrate.has_value()) {
        write_uint(out, seq, "bitrate", *track.bitrate);
    }
    if (track.avg_bitrate.has_value()) {
        write_uint(out, seq, "avgBitrate", *track.avg_bitrate);
    }
    if (track.max_gop_duration_ms.has_value()) {
        write_uint(out, seq, "maxGopDuration", *track.max_gop_duration_ms);
    }
    if (track.max_group_duration_ms.has_value()) {
        write_uint(out, seq, "maxGroupDuration", *track.max_group_duration_ms);
    }
    if (track.width.has_value()) {
        write_uint(out, seq, "width", *track.width);
    }
    if (track.height.has_value()) {
        write_uint(out, seq, "height", *track.height);
    }
    if (track.samplerate.has_value()) {
        write_uint(out, seq, "samplerate", *track.samplerate);
    }
    if (track.channel_config.has_value()) {
        write_string(out, seq, "channelConfig", *track.channel_config);
    }
    if (track.lang.has_value()) {
        write_string(out, seq, "lang", *track.lang);
    }
    if (track.track_duration_ms.has_value()) {
        write_uint(out, seq, "trackDuration", *track.track_duration_ms);
    }
    if (track.max_grp_sap_starting_type.has_value()) {
        write_uint(out, seq, "maxGrpSapStartingType", *track.max_grp_sap_starting_type);
    }
    if (track.max_obj_sap_starting_type.has_value()) {
        write_uint(out, seq, "maxObjSapStartingType", *track.max_obj_sap_starting_type);
    }
    // Section 5: producer-defined fields, emitted last. Values are raw JSON.
    for (const auto& [key, value] : track.custom_fields) {
        write_raw(out, seq, key, value);
    }

    out << '}';
}

void write_track_array(std::ostringstream& out,
                       JsonSeq& seq,
                       std::string_view key,
                       const std::vector<MsfTrack>& tracks) {
    seq.separate();
    out << '"' << json_escape(key) << "\":[";
    JsonSeq inner(out);
    for (const auto& track : tracks) {
        inner.separate();
        write_track(out, track);
    }
    out << ']';
}

}  // namespace

std::string serialize_catalog(const MsfCatalog& catalog) {
    validate_catalog(catalog);

    std::ostringstream out;
    out << '{';
    JsonSeq seq(out);

    write_string(out, seq, "version", catalog.version);
    if (catalog.generated_at_ms.has_value()) {
        write_uint(out, seq, "generatedAt", *catalog.generated_at_ms);
    }
    // Section 5.1.3: this field MUST NOT be included if it is FALSE.
    if (catalog.is_complete.has_value() && *catalog.is_complete) {
        write_bool(out, seq, "isComplete", true);
    }

    write_track_array(out, seq, "tracks", catalog.tracks);

    if (!catalog.publish_tracks.empty()) {
        write_track_array(out, seq, "publishTracks", catalog.publish_tracks);
    }

    // Section 5.1.7: initDataList MUST be located after the tracks array.
    if (!catalog.init_data_list.empty()) {
        seq.separate();
        out << "\"initDataList\":[";
        JsonSeq inner(out);
        for (const auto& entry : catalog.init_data_list) {
            inner.separate();
            out << '{';
            JsonSeq entry_seq(out);
            write_string(out, entry_seq, "id", entry.id);
            write_string(out, entry_seq, "type", entry.type);
            write_string(out, entry_seq, "data", entry.data);
            out << '}';
        }
        out << ']';
    }

    out << '}';
    return out.str();
}

}  // namespace openmoq::publisher
```

- [ ] **Step 5: Register in CMake**

In `CMakeLists.txt`, add `src/msf_catalog.cpp` to the `add_library(openmoq_publisher_lib STATIC ...)` list (currently at line 254), keeping the list alphabetical — insert it immediately after `src/moq_draft.cpp`.

Then add a test target immediately after the `openmoq-publisher-packaging-tests` block (which ends around line 362):

```cmake
    add_executable(openmoq-publisher-msf-catalog-tests
        tests/msf_catalog_test.cpp
    )
    target_link_libraries(openmoq-publisher-msf-catalog-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-msf-catalog-tests COMMAND openmoq-publisher-msf-catalog-tests)
```

- [ ] **Step 6: Run test to verify it passes**

Run:
```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build --target openmoq-publisher-msf-catalog-tests
ctest --test-dir build -R openmoq-publisher-msf-catalog-tests --output-on-failure
```
Expected: PASS

- [ ] **Step 7: Commit**

```bash
git add include/openmoq/publisher/msf_catalog.h src/msf_catalog.cpp tests/msf_catalog_test.cpp CMakeLists.txt
git commit -m "Add MSF v1 catalog model and serializer"
```

---

### Task 2: Draft conformance tests and validation coverage

**Files:**
- Modify: `tests/msf_catalog_test.cpp`

**Interfaces:**
- Consumes: `MsfCatalog`, `MsfTrack`, `MsfInitData`, `serialize_catalog()` from Task 1.
- Produces: nothing consumed by later tasks.

- [ ] **Step 1: Write the failing tests**

Add a `throws_runtime_error` helper inside the anonymous namespace of `tests/msf_catalog_test.cpp`, after `expect_not_contains`:

```cpp
bool throws_runtime_error(const openmoq::publisher::MsfCatalog& catalog, const std::string& message) {
    try {
        (void)openmoq::publisher::serialize_catalog(catalog);
    } catch (const std::runtime_error&) {
        return true;
    }
    return expect(false, message);
}
```

Add `#include "openmoq/publisher/msf_catalog.h"` already present; add `#include <stdexcept>` to the include list.

Then append to `main()`, before `return ok ? 0 : 1;`:

```cpp
    // CMSF section 5.1: simulcast video with a shared altGroup, plus audio,
    // with initialization data carried by initDataList and referenced by initRef.
    MsfCatalog simulcast;
    simulcast.generated_at_ms = 1746104606044ULL;

    for (const auto& quality : {std::string("hd"), std::string("md"), std::string("sd")}) {
        MsfTrack alt;
        alt.name = quality;
        alt.packaging = "cmaf";
        alt.role = "video";
        alt.is_live = true;
        alt.render_group = 1;
        alt.alt_group = 1;
        alt.target_latency_ms = 2000;
        alt.init_ref = "init-" + quality;
        alt.codec = "avc1.640028";
        alt.bitrate = 5000000;
        alt.framerate = 30.0;
        alt.width = 1920;
        alt.height = 1080;
        simulcast.tracks.push_back(alt);
        simulcast.init_data_list.push_back(MsfInitData{.id = "init-" + quality,
                                                       .type = "inline",
                                                       .data = "AAAAHGZ0eXBjbWYy"});
    }

    MsfTrack audio;
    audio.name = "audio";
    audio.packaging = "cmaf";
    audio.role = "audio";
    audio.is_live = true;
    audio.render_group = 1;
    audio.target_latency_ms = 2000;
    audio.init_ref = "init-audio";
    audio.codec = "mp4a.40.5";
    audio.bitrate = 67071;
    audio.samplerate = 48000;
    audio.channel_config = "2";
    simulcast.tracks.push_back(audio);
    simulcast.init_data_list.push_back(MsfInitData{.id = "init-audio",
                                                   .type = "inline",
                                                   .data = "AAAAHGZ0eXBjbWYy"});

    const std::string simulcast_json = serialize_catalog(simulcast);
    ok &= expect_contains(simulcast_json, "\"generatedAt\":1746104606044", "expected generatedAt");
    ok &= expect_contains(simulcast_json, "\"altGroup\":1", "expected shared altGroup");
    ok &= expect_contains(simulcast_json, "\"initRef\":\"init-hd\"", "expected initRef");
    ok &= expect_contains(simulcast_json, "\"channelConfig\":\"2\"", "expected channelConfig as a string");
    ok &= expect_not_contains(simulcast_json, "\"channelCount\"", "expected no legacy channelCount");
    ok &= expect_contains(simulcast_json, "\"samplerate\":48000", "expected spec samplerate spelling");
    ok &= expect_not_contains(simulcast_json, "\"sampleRate\"", "expected no legacy sampleRate spelling");
    ok &= expect_not_contains(simulcast_json, "\"initData\"", "expected no legacy inline initData");

    // MSF section 5.1.7: initDataList MUST be located after the tracks array.
    const std::size_t tracks_pos = simulcast_json.find("\"tracks\"");
    const std::size_t init_list_pos = simulcast_json.find("\"initDataList\"");
    ok &= expect(tracks_pos != std::string::npos && init_list_pos != std::string::npos,
                 "expected both tracks and initDataList present");
    ok &= expect(tracks_pos < init_list_pos, "expected initDataList to follow tracks");

    // MSF section 5.1.3: isComplete MUST NOT be included when false.
    MsfCatalog not_complete = simulcast;
    not_complete.is_complete = false;
    ok &= expect_not_contains(serialize_catalog(not_complete), "\"isComplete\"",
                              "expected isComplete omitted when false");

    // CMSF section 3.6: SAP event timeline track carries eventType and the
    // CMSF max SAP starting types (section 3.5.2).
    MsfCatalog with_sap;
    MsfTrack sap;
    sap.name = "video_sap";
    sap.packaging = "eventtimeline";
    sap.role = "eventtimeline";
    sap.is_live = true;
    sap.event_type = "org.ietf.moq.cmsf.sap";
    sap.mime_type = "application/json";
    sap.depends = {"video"};
    with_sap.tracks.push_back(sap);

    MsfTrack sapped_video;
    sapped_video.name = "video";
    sapped_video.packaging = "cmaf";
    sapped_video.role = "video";
    sapped_video.is_live = true;
    sapped_video.codec = "avc1.640028";
    sapped_video.bitrate = 5000000;
    sapped_video.max_grp_sap_starting_type = 2;
    sapped_video.max_obj_sap_starting_type = 3;
    with_sap.tracks.push_back(sapped_video);

    const std::string sap_json = serialize_catalog(with_sap);
    ok &= expect_contains(sap_json, "\"eventType\":\"org.ietf.moq.cmsf.sap\"", "expected SAP eventType");
    ok &= expect_contains(sap_json, "\"depends\":[\"video\"]", "expected depends array");
    ok &= expect_contains(sap_json, "\"maxGrpSapStartingType\":2", "expected maxGrpSapStartingType");
    ok &= expect_contains(sap_json, "\"maxObjSapStartingType\":3", "expected maxObjSapStartingType");

    // Validation: bitrate is a MUST for audio and video (section 5.2.22).
    MsfCatalog no_bitrate;
    MsfTrack bare_video;
    bare_video.name = "video";
    bare_video.packaging = "cmaf";
    bare_video.role = "video";
    bare_video.is_live = true;
    bare_video.codec = "avc1.640028";
    no_bitrate.tracks.push_back(bare_video);
    ok &= throws_runtime_error(no_bitrate, "expected throw when video track omits bitrate");

    // Validation: eventType outside eventtimeline packaging (section 5.2.5).
    MsfCatalog stray_event_type;
    MsfTrack stray = sapped_video;
    stray.event_type = "com.example.bogus";
    stray_event_type.tracks.push_back(stray);
    ok &= throws_runtime_error(stray_event_type, "expected throw for eventType on cmaf packaging");

    // Validation: trackDuration on a live track (section 5.2.35).
    MsfCatalog live_with_duration;
    MsfTrack durated = sapped_video;
    durated.track_duration_ms = 60000;
    live_with_duration.tracks.push_back(durated);
    ok &= throws_runtime_error(live_with_duration, "expected throw for trackDuration on a live track");

    // Validation: audio requires samplerate and channelConfig (5.2.28, 5.2.29).
    MsfCatalog bare_audio_catalog;
    MsfTrack bare_audio;
    bare_audio.name = "audio";
    bare_audio.packaging = "cmaf";
    bare_audio.role = "audio";
    bare_audio.is_live = true;
    bare_audio.codec = "mp4a.40.2";
    bare_audio.bitrate = 128000;
    bare_audio_catalog.tracks.push_back(bare_audio);
    ok &= throws_runtime_error(bare_audio_catalog, "expected throw when audio omits samplerate");

    // Validation: targetLatency and buffers are mutually exclusive (5.2.8, 5.2.9).
    MsfCatalog both_latency_forms;
    MsfTrack conflicted = sapped_video;
    conflicted.target_latency_ms = 2000;
    conflicted.buffers = MsfBuffers{.target_ms = 1500, .min_ms = std::nullopt, .max_ms = std::nullopt};
    both_latency_forms.tracks.push_back(conflicted);
    ok &= throws_runtime_error(both_latency_forms, "expected throw for targetLatency alongside buffers");

    // Validation: initRef must resolve to an initDataList entry (5.2.13).
    MsfCatalog dangling_ref;
    MsfTrack unresolved = sapped_video;
    unresolved.init_ref = "missing-init";
    dangling_ref.tracks.push_back(unresolved);
    ok &= throws_runtime_error(dangling_ref, "expected throw for initRef with no initDataList entry");

    // Validation: track names must be unique per namespace (5.2.3).
    MsfCatalog duplicated;
    duplicated.tracks.push_back(sapped_video);
    duplicated.tracks.push_back(sapped_video);
    ok &= throws_runtime_error(duplicated, "expected throw for duplicate track names in one namespace");

    // The same name in a different namespace is legal.
    MsfCatalog distinct_namespaces;
    MsfTrack ns_a = sapped_video;
    ns_a.name_space = "example.com/a";
    MsfTrack ns_b = sapped_video;
    ns_b.name_space = "example.com/b";
    distinct_namespaces.tracks.push_back(ns_a);
    distinct_namespaces.tracks.push_back(ns_b);
    ok &= expect_contains(serialize_catalog(distinct_namespaces), "\"namespace\":\"example.com/b\"",
                          "expected identical names in distinct namespaces to serialize");

    // Buffers serialize as an object with only the keys that are set.
    MsfCatalog buffered;
    MsfTrack with_buffers = sapped_video;
    with_buffers.buffers = MsfBuffers{.target_ms = 1500, .min_ms = 800, .max_ms = std::nullopt};
    buffered.tracks.push_back(with_buffers);
    const std::string buffered_json = serialize_catalog(buffered);
    ok &= expect_contains(buffered_json, "\"buffers\":{\"target\":1500,\"min\":800}",
                          "expected buffers object with only the set keys");

    // Section 5: custom fields are permitted and emitted as raw JSON.
    MsfCatalog with_custom;
    MsfTrack custom_track = sapped_video;
    custom_track.custom_fields["m2tsPacketSize"] = "188";
    custom_track.custom_fields["m2tsTimestampMode"] = "\"opaque\"";
    with_custom.tracks.push_back(custom_track);
    const std::string custom_json = serialize_catalog(with_custom);
    ok &= expect_contains(custom_json, "\"m2tsPacketSize\":188", "expected numeric custom field");
    ok &= expect_contains(custom_json, "\"m2tsTimestampMode\":\"opaque\"", "expected quoted custom field");

    // Section 5: custom field names MUST NOT collide with spec field names.
    MsfCatalog colliding;
    MsfTrack collider = sapped_video;
    collider.custom_fields["bitrate"] = "99";
    colliding.tracks.push_back(collider);
    ok &= throws_runtime_error(colliding, "expected throw for a custom field colliding with a spec name");

    // JSON escaping: control characters and quotes must not corrupt the document.
    MsfCatalog escaped;
    MsfTrack quoted = sapped_video;
    quoted.label = "cam \"A\"\n\x01";
    escaped.tracks.push_back(quoted);
    const std::string escaped_json = serialize_catalog(escaped);
    ok &= expect_contains(escaped_json, "\\\"A\\\"", "expected escaped quotes in label");
    ok &= expect_contains(escaped_json, "\\u0001", "expected escaped control character in label");
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build --target openmoq-publisher-msf-catalog-tests && ctest --test-dir build -R openmoq-publisher-msf-catalog-tests --output-on-failure`
Expected: FAIL — validation and ordering assertions fail if Task 1's `validate_track` or `initDataList` placement is wrong. If Task 1 was implemented exactly, these pass immediately; that is an acceptable outcome for a conformance suite over already-written code. If any fail, fix `src/msf_catalog.cpp` before continuing.

- [ ] **Step 3: Fix any failures in the serializer**

If an assertion fails, correct `src/msf_catalog.cpp` — do not weaken the test. The drafts are authoritative; `docs/draft-ietf-moq-msf-01.txt` and `docs/draft-ietf-moq-cmsf-01.txt` are in the repository.

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --test-dir build -R openmoq-publisher-msf-catalog-tests --output-on-failure`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add tests/msf_catalog_test.cpp src/msf_catalog.cpp
git commit -m "Add MSF and CMSF catalog conformance tests"
```

---

### Task 3: Extract bitrate and language from MP4 tracks

**Files:**
- Modify: `include/openmoq/publisher/mp4_box.h:26-43` (`TrackDescription`)
- Modify: `src/mp4_box.cpp:642-720` (`extract_tracks`)
- Modify: `tests/cmaf_segmenter_test.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `TrackDescription::max_bitrate`, `::avg_bitrate`, `::language`, `::duration_ms` — all populated by `extract_tracks`, consumed by Task 4.

- [ ] **Step 1: Write the failing test**

`tests/cmaf_segmenter_test.cpp` already builds synthetic MP4 boxes with `make_box` and `append_be32`. Add this helper inside its anonymous namespace, next to the other box builders:

```cpp
// ISO 14496-12 BitRateBox: bufferSizeDB, maxBitrate, avgBitrate.
std::vector<std::uint8_t> make_btrt(std::uint32_t buffer_size_db,
                                    std::uint32_t max_bitrate,
                                    std::uint32_t avg_bitrate) {
    std::vector<std::uint8_t> payload;
    append_be32(payload, buffer_size_db);
    append_be32(payload, max_bitrate);
    append_be32(payload, avg_bitrate);
    return make_box("btrt", payload);
}
```

Then add to `main()`, before `return ok ? 0 : 1;`. Build a fragmented MP4 whose video sample entry carries a `btrt` and whose `mdhd` declares language `eng`, reusing whichever existing fixture builder in this file produces a single-track fragmented MP4, then assert:

```cpp
    // MSF section 5.2.22 requires bitrate; it comes from btrt when present.
    ok &= expect(btrt_tracks.size() == 1, "expected one track in btrt fixture");
    ok &= expect(btrt_tracks.front().max_bitrate == 5000000,
                 "expected maxBitrate parsed from btrt");
    ok &= expect(btrt_tracks.front().avg_bitrate == 4000000,
                 "expected avgBitrate parsed from btrt");
    // MSF section 5.2.32: lang from the packed mdhd language field.
    ok &= expect(btrt_tracks.front().language == "eng",
                 "expected ISO-639-2 language decoded from mdhd");
```

Bind `btrt_tracks` by parsing the fixture with the existing `extract_tracks(parse_mp4_boxes(bytes), bytes)` call pattern used elsewhere in this file.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-packaging-tests && ctest --test-dir build -R openmoq-publisher-packaging-tests --output-on-failure`
Expected: FAIL — `'struct openmoq::publisher::TrackDescription' has no member named 'max_bitrate'`

- [ ] **Step 3: Extend TrackDescription**

In `include/openmoq/publisher/mp4_box.h`, add to `TrackDescription` after `double frame_rate = 0.0;`:

```cpp
    std::uint64_t max_bitrate = 0;   // bits per second, 0 when unknown
    std::uint64_t avg_bitrate = 0;   // bits per second, 0 when unknown
    std::uint64_t duration_ms = 0;   // track duration, 0 when unknown
    std::string language;            // ISO-639-2/T, empty when unknown or "und"
```

- [ ] **Step 4: Parse btrt, language, and duration**

In `src/mp4_box.cpp`, add these helpers to the anonymous namespace alongside `frame_rate_from_stts`:

```cpp
// ISO 14496-12 BitRateBox inside a sample entry: bufferSizeDB, maxBitrate,
// avgBitrate, each 32-bit big-endian.
struct BitrateInfo {
    std::uint64_t max_bitrate = 0;
    std::uint64_t avg_bitrate = 0;
};

BitrateInfo bitrate_from_sample_entry(const Mp4Box& sample_entry,
                                      std::span<const std::uint8_t> bytes,
                                      std::size_t child_offset) {
    const std::size_t btrt_offset = find_child_box_offset(sample_entry, bytes, child_offset, "btrt");
    if (btrt_offset == 0 || btrt_offset + 20 > bytes.size()) {
        return {};
    }
    return BitrateInfo{
        .max_bitrate = read_be32(bytes, btrt_offset + 12),
        .avg_bitrate = read_be32(bytes, btrt_offset + 16),
    };
}

// mdhd language is three 5-bit values, each offset by 0x60, packed into 16
// bits. Returns an empty string for the "und" (undetermined) code.
std::string language_from_mdhd(const Mp4Box& mdhd, std::span<const std::uint8_t> bytes) {
    if (mdhd.payload.size < 4) {
        return {};
    }
    const std::uint8_t version = bytes[mdhd.payload.offset];
    // v0: version+flags(4) + created(4) + modified(4) + timescale(4) + duration(4)
    // v1: version+flags(4) + created(8) + modified(8) + timescale(4) + duration(8)
    const std::size_t language_offset = mdhd.payload.offset + (version == 1 ? 32 : 20);
    if (language_offset + 2 > bytes.size()) {
        return {};
    }
    const std::uint16_t packed =
        static_cast<std::uint16_t>((bytes[language_offset] << 8U) | bytes[language_offset + 1]);
    std::string language;
    for (int shift = 10; shift >= 0; shift -= 5) {
        const auto code = static_cast<char>(((packed >> shift) & 0x1FU) + 0x60);
        if (code < 'a' || code > 'z') {
            return {};
        }
        language.push_back(code);
    }
    return language == "und" ? std::string{} : language;
}

std::uint64_t duration_ms_from_mdhd(const Mp4Box& mdhd,
                                    std::uint32_t timescale,
                                    std::span<const std::uint8_t> bytes) {
    if (timescale == 0 || mdhd.payload.size < 4) {
        return 0;
    }
    const std::uint8_t version = bytes[mdhd.payload.offset];
    const std::size_t duration_offset = mdhd.payload.offset + (version == 1 ? 24 : 16);
    std::uint64_t duration = 0;
    if (version == 1) {
        if (duration_offset + 8 > bytes.size()) {
            return 0;
        }
        duration = (static_cast<std::uint64_t>(read_be32(bytes, duration_offset)) << 32U) |
                   read_be32(bytes, duration_offset + 4);
    } else {
        if (duration_offset + 4 > bytes.size()) {
            return 0;
        }
        duration = read_be32(bytes, duration_offset);
    }
    return duration * 1000ULL / timescale;
}
```

Note: if `find_child_box_offset` or `read_be32` are declared later in the file than these helpers, move the new helpers below them so they compile.

Then, in `extract_tracks`, after the existing `frame_rate` assignment near `src/mp4_box.cpp:701` and before the `TrackDescription` aggregate initializer at line 719, add:

```cpp
        BitrateInfo bitrate;
        if (sample_entry != nullptr) {
            // Same sample entry header offsets build_track_codec_init_data uses:
            // 8 + 70 past a VisualSampleEntry, 8 + 28 past an AudioSampleEntry.
            const std::size_t child_offset = handler_type == "vide" ? 8 + 70 : 8 + 28;
            bitrate = bitrate_from_sample_entry(*sample_entry, bytes, child_offset);
        }

        std::string language;
        std::uint64_t duration_ms = 0;
        if (mdhd != nullptr) {
            language = language_from_mdhd(*mdhd, bytes);
            duration_ms = duration_ms_from_mdhd(*mdhd, timescale, bytes);
        }
```

Bind `sample_entry` from the `stsd` the surrounding loop already locates; if the existing code does not keep a pointer to the first sample entry, derive it the same way `build_track_codec_init_data` does at `src/cmsf_packager.cpp:443-454`.

Then add these five fields to the aggregate initializer at line 719, after `.frame_rate = frame_rate,`:

```cpp
            .max_bitrate = bitrate.max_bitrate,
            .avg_bitrate = bitrate.avg_bitrate,
            .duration_ms = duration_ms,
            .language = language,
```

C++20 designated initializers must appear in declaration order, so these must follow the order used in `TrackDescription` from Step 3, and `.codec_private` must remain last.

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --target openmoq-publisher-packaging-tests && ctest --test-dir build -R openmoq-publisher-packaging-tests --output-on-failure`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add include/openmoq/publisher/mp4_box.h src/mp4_box.cpp tests/cmaf_segmenter_test.cpp
git commit -m "Extract bitrate, language, and duration from MP4 tracks"
```

---

### Task 4: Bitrate resolution and the shared track builder

**Files:**
- Modify: `include/openmoq/publisher/msf_catalog.h`
- Modify: `src/msf_catalog.cpp`
- Modify: `tests/msf_catalog_test.cpp`

**Interfaces:**
- Consumes: `TrackDescription` fields from Task 3; `MsfTrack` from Task 1.
- Produces, all used by Tasks 5, 6, and 7:
  - `std::uint64_t resolve_bitrate(const TrackDescription&, std::optional<std::uint64_t> configured, std::uint64_t computed)`
  - `bool bitrate_is_estimated(const TrackDescription&, std::optional<std::uint64_t> configured, std::uint64_t computed)`
  - `MsfTrack make_msf_track(const TrackDescription&, bool is_live, std::optional<std::uint64_t> configured_bitrate = std::nullopt, std::uint64_t computed_bitrate = 0)`

This is the task that prevents the four emitters from drifting apart again: every publish path builds its tracks through `make_msf_track`.

- [ ] **Step 1: Write the failing test**

Append to `main()` in `tests/msf_catalog_test.cpp`:

```cpp
    // Bitrate precedence: btrt, then configured override, then computed,
    // then a codec-class default (design decision, Phase 1).
    TrackDescription btrt_track;
    btrt_track.handler_type = "vide";
    btrt_track.max_bitrate = 5000000;
    ok &= expect(resolve_bitrate(btrt_track, 900000, 700000) == 5000000,
                 "expected btrt to win over override and computed");

    TrackDescription no_btrt;
    no_btrt.handler_type = "vide";
    ok &= expect(resolve_bitrate(no_btrt, 900000, 700000) == 900000,
                 "expected configured override to win over computed");
    ok &= expect(resolve_bitrate(no_btrt, std::nullopt, 700000) == 700000,
                 "expected computed bitrate when no btrt or override");
    ok &= expect(resolve_bitrate(no_btrt, std::nullopt, 0) == 2000000,
                 "expected 2 Mbps video default as last resort");

    TrackDescription bare_sound;
    bare_sound.handler_type = "soun";
    ok &= expect(resolve_bitrate(bare_sound, std::nullopt, 0) == 128000,
                 "expected 128 kbps audio default as last resort");
    ok &= expect(bitrate_is_estimated(bare_sound, std::nullopt, 0),
                 "expected the codec-class default to report as estimated");
    ok &= expect(!bitrate_is_estimated(btrt_track, std::nullopt, 0),
                 "expected a btrt-derived bitrate not to report as estimated");

    // make_msf_track maps a parsed MP4 track onto the MSF track object.
    TrackDescription source_video;
    source_video.track_name = "video";
    source_video.handler_type = "vide";
    source_video.packaging = "cmaf";
    source_video.codec = "avc1.640028";
    source_video.width = 1920;
    source_video.height = 1080;
    source_video.frame_rate = 30.0;
    source_video.timescale = 90000;
    source_video.max_bitrate = 5000000;
    source_video.language = "eng";
    source_video.duration_ms = 60000;

    const MsfTrack built_live = make_msf_track(source_video, /*is_live=*/true);
    ok &= expect(built_live.role.value_or("") == "video", "expected vide handler to map to the video role");
    ok &= expect(built_live.bitrate.value_or(0) == 5000000, "expected btrt bitrate on the built track");
    ok &= expect(built_live.lang.value_or("") == "eng", "expected language mapped to lang");
    ok &= expect(built_live.render_group.value_or(0) == 1, "expected media tracks in render group 1");
    ok &= expect(!built_live.track_duration_ms.has_value(),
                 "expected no trackDuration on a live track");

    const MsfTrack built_vod = make_msf_track(source_video, /*is_live=*/false);
    ok &= expect(built_vod.track_duration_ms.value_or(0) == 60000,
                 "expected trackDuration on a non-live track");

    TrackDescription source_audio;
    source_audio.track_name = "audio";
    source_audio.handler_type = "soun";
    source_audio.packaging = "cmaf";
    source_audio.codec = "mp4a.40.2";
    source_audio.sample_rate = 48000;
    source_audio.channel_count = 2;

    const MsfTrack built_audio = make_msf_track(source_audio, /*is_live=*/true);
    ok &= expect(built_audio.role.value_or("") == "audio", "expected soun handler to map to the audio role");
    ok &= expect(built_audio.channel_config.value_or("") == "2",
                 "expected channelConfig rendered as a string");
    ok &= expect(built_audio.samplerate.value_or(0) == 48000, "expected samplerate mapped");
    ok &= expect(built_audio.bitrate.value_or(0) == 128000,
                 "expected the audio codec-class default when no rate is known");
```

Add `#include "openmoq/publisher/mp4_box.h"` to the test's include list.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-msf-catalog-tests`
Expected: FAIL — `'resolve_bitrate' was not declared in this scope`

- [ ] **Step 3: Declare in the header**

In `include/openmoq/publisher/msf_catalog.h`, add `#include "openmoq/publisher/mp4_box.h"` to the includes and declare below `serialize_catalog`:

```cpp
// Resolve the bitrate MSF section 5.2.22 requires for audio and video, in
// precedence order: the track's btrt value, then a configured override, then
// a value computed from sample sizes, then a codec-class default. The default
// keeps the catalog conformant on live paths where no measurement exists; the
// caller is expected to log when it is used.
std::uint64_t resolve_bitrate(const TrackDescription& track,
                              std::optional<std::uint64_t> configured,
                              std::uint64_t computed);

// True when resolve_bitrate would fall through to a codec-class default, so
// callers can warn the operator that the published figure is an estimate.
bool bitrate_is_estimated(const TrackDescription& track,
                          std::optional<std::uint64_t> configured,
                          std::uint64_t computed);

// Build the MSF track object for a parsed MP4 track. Shared by every emitter
// so the four publish paths cannot drift apart again. The caller still sets
// init_ref, alt_group, and the CMSF max SAP starting types, which depend on
// catalog-level and segmentation context this function does not see.
MsfTrack make_msf_track(const TrackDescription& track,
                        bool is_live,
                        std::optional<std::uint64_t> configured_bitrate = std::nullopt,
                        std::uint64_t computed_bitrate = 0);
```

- [ ] **Step 4: Implement**

Append to `src/msf_catalog.cpp`, inside `namespace openmoq::publisher` but outside the anonymous namespace:

```cpp
namespace {

constexpr std::uint64_t kDefaultVideoBitrate = 2000000;
constexpr std::uint64_t kDefaultAudioBitrate = 128000;

std::uint64_t codec_class_default(const TrackDescription& track) {
    return track.handler_type == "soun" ? kDefaultAudioBitrate : kDefaultVideoBitrate;
}

}  // namespace

std::uint64_t resolve_bitrate(const TrackDescription& track,
                              std::optional<std::uint64_t> configured,
                              std::uint64_t computed) {
    if (track.max_bitrate != 0) {
        return track.max_bitrate;
    }
    if (configured.has_value() && *configured != 0) {
        return *configured;
    }
    if (computed != 0) {
        return computed;
    }
    return codec_class_default(track);
}

bool bitrate_is_estimated(const TrackDescription& track,
                          std::optional<std::uint64_t> configured,
                          std::uint64_t computed) {
    return track.max_bitrate == 0 && !(configured.has_value() && *configured != 0) && computed == 0;
}

MsfTrack make_msf_track(const TrackDescription& track,
                        bool is_live,
                        std::optional<std::uint64_t> configured_bitrate,
                        std::uint64_t computed_bitrate) {
    MsfTrack out;
    out.name = track.track_name;
    out.packaging = track.packaging;
    out.is_live = is_live;

    if (track.handler_type == "vide") {
        out.role = "video";
    } else if (track.handler_type == "soun") {
        out.role = "audio";
    } else if (track.packaging == "mediatimeline") {
        out.role = "mediatimeline";
    } else if (track.packaging == "eventtimeline") {
        out.role = "eventtimeline";
    }

    if (!track.codec.empty()) {
        out.codec = track.codec;
    }
    if (!track.mime_type.empty()) {
        out.mime_type = track.mime_type;
    }
    if (!track.event_type.empty()) {
        out.event_type = track.event_type;
    }
    if (!track.depends.empty()) {
        out.depends = track.depends;
    }
    if (!track.language.empty()) {
        out.lang = track.language;
    }
    if (track.timescale != 0) {
        out.timescale = track.timescale;
    }
    // Section 5.2.35 forbids trackDuration on a live track.
    if (!is_live && track.duration_ms != 0) {
        out.track_duration_ms = track.duration_ms;
    }

    if (track.handler_type == "vide") {
        out.render_group = 1;
        out.width = track.width;
        out.height = track.height;
        if (track.frame_rate > 0.0) {
            out.framerate = track.frame_rate;
        }
        out.bitrate = resolve_bitrate(track, configured_bitrate, computed_bitrate);
    } else if (track.handler_type == "soun") {
        out.render_group = 1;
        out.samplerate = track.sample_rate;
        out.channel_config = std::to_string(track.channel_count);
        out.bitrate = resolve_bitrate(track, configured_bitrate, computed_bitrate);
    }

    if (track.avg_bitrate != 0) {
        out.avg_bitrate = track.avg_bitrate;
    }

    return out;
}
```

Note: a second anonymous namespace block in the same translation unit is legal and keeps `codec_class_default` file-local. Place all of this after the existing `serialize_catalog` definition. Add `#include <string>` if it is not already present.

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build --target openmoq-publisher-msf-catalog-tests && ctest --test-dir build -R openmoq-publisher-msf-catalog-tests --output-on-failure`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add include/openmoq/publisher/msf_catalog.h src/msf_catalog.cpp tests/msf_catalog_test.cpp
git commit -m "Add MSF bitrate resolution and the shared track builder"
```

---

### Task 5: Migrate the batch publish plan emitter

**Files:**
- Modify: `src/cmsf_packager.cpp:258-302` (delete `append_catalog_track_json`), `src/cmsf_packager.cpp:544-562` (catalog construction)
- Modify: `tests/cmaf_segmenter_test.cpp:465-475`, `:720-790`

**Interfaces:**
- Consumes: `MsfCatalog`, `MsfTrack`, `MsfInitData`, `serialize_catalog()`, `resolve_bitrate()`.
- Produces: `build_publish_plan` emitting a v1 catalog. `PublishPlan` keeps its existing shape.

- [ ] **Step 1: Write the failing test**

In `tests/cmaf_segmenter_test.cpp`, replace the legacy assertions in the publish-plan section with:

```cpp
    ok &= expect_contains(plan_catalog_text, "\"version\":\"1\"",
                          "expected MSF v1 string version in publish plan catalog");
    ok &= expect_not_contains(plan_catalog_text, "\"format\"",
                              "expected no legacy format field");
    ok &= expect_not_contains(plan_catalog_text, "\"initData\":",
                              "expected initDataList instead of inline initData");
    ok &= expect_contains(plan_catalog_text, "\"initDataList\"",
                          "expected root initDataList");
    ok &= expect_contains(plan_catalog_text, "\"initRef\"",
                          "expected per-track initRef");
    ok &= expect_not_contains(plan_catalog_text, "\"id\":",
                              "expected no non-spec track id field");
```

Bind `plan_catalog_text` with the existing `object_text(...)` helper applied to the plan's catalog object — the same pattern the file already uses at line 465.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-packaging-tests && ctest --test-dir build -R openmoq-publisher-packaging-tests --output-on-failure`
Expected: FAIL — the emitter still writes `"version":1` and `"format":"cmsf"`

- [ ] **Step 3: Replace the emitter**

In `src/cmsf_packager.cpp`, add `#include "openmoq/publisher/msf_catalog.h"` and delete `append_catalog_track_json` (lines 258-302) along with the now-unused `track_role` helper (lines 132-146), whose logic moved into `make_msf_track`.

Replace the `std::ostringstream catalog;` block at lines 544-562 with:

```cpp
    MsfCatalog msf_catalog;
    // Section 5.1.2: generatedAt SHOULD NOT be included when isLive is false,
    // which is always the case for a batch publish plan.
    for (const auto& track : plan.tracks) {
        if (track.track_name == "catalog") {
            continue;
        }
        MsfTrack msf_track = make_msf_track(track, /*is_live=*/false);

        const auto init_it = init_data_by_track.find(track.track_name);
        if (init_it != init_data_by_track.end()) {
            const std::string init_id = track.track_name + "-init";
            msf_track.init_ref = init_id;
            msf_catalog.init_data_list.push_back(MsfInitData{
                .id = init_id,
                .type = "inline",
                .data = init_it->second,
            });
        }

        msf_catalog.tracks.push_back(std::move(msf_track));
    }

    const std::string catalog_text = serialize_catalog(msf_catalog);
```

The existing line `const std::string catalog_text = catalog.str();` is removed, since `catalog_text` is now produced directly. Everything downstream of it — the `catalog_payload` vector and the `plan.objects.push_back` at line 563 — is unchanged.

Keep the file-scope `base64_encode` helper; only the JSON assembly changes.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target openmoq-publisher-packaging-tests && ctest --test-dir build -R openmoq-publisher-packaging-tests --output-on-failure`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/cmsf_packager.cpp tests/cmaf_segmenter_test.cpp
git commit -m "Emit MSF v1 catalog from the batch publish plan"
```

---

### Task 6: Migrate the live catalog emitter

**Files:**
- Modify: `src/cmsf_packager.cpp:694-798` (`build_live_catalog`)
- Modify: `tests/cmaf_segmenter_test.cpp:791-820`

**Interfaces:**
- Consumes: everything from Tasks 1 and 4.
- Produces: `build_live_catalog` keeps its exact signature and `LiveCatalog` return type, so `src/transport/moqt_session.cpp:3061`, `:3437` and `src/transport/libmoq_publisher.cpp:946`, `:1181` need no change.

- [ ] **Step 1: Write the failing test**

In `tests/cmaf_segmenter_test.cpp`, extend the `build_live_catalog` section:

```cpp
    const std::string live_catalog_text(live_catalog.catalog_payload.begin(),
                                        live_catalog.catalog_payload.end());
    ok &= expect_contains(live_catalog_text, "\"version\":\"1\"",
                          "expected MSF v1 string version in live catalog");
    ok &= expect_contains(live_catalog_text, "\"isLive\":true",
                          "expected live tracks marked isLive");
    ok &= expect_contains(live_catalog_text, "\"generatedAt\":",
                          "expected generatedAt on a live catalog");
    ok &= expect_contains(live_catalog_text, "\"initDataList\"",
                          "expected root initDataList in live catalog");
    ok &= expect_not_contains(live_catalog_text, "\"format\"",
                              "expected no legacy format field in live catalog");
    ok &= expect_not_contains(live_catalog_text, "\"initData\":",
                              "expected no inline initData in live catalog");
```

The existing test at line 791 already binds `live_catalog`; keep the surrounding init-segment equality assertions, which remain valid because `track_initializations` is unchanged.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-packaging-tests && ctest --test-dir build -R openmoq-publisher-packaging-tests --output-on-failure`
Expected: FAIL — legacy shape still emitted

- [ ] **Step 3: Replace the emitter body**

In `build_live_catalog`, delete the three local lambdas `local_base64_encode`, `local_json_escape`, and `local_track_role` (lines 698-735) — they duplicate file-scope helpers and are the source of the divergent escaping. Use the file's existing `base64_encode`.

Add `#include <chrono>`. Replace the catalog JSON block at lines 753-795 with:

```cpp
    MsfCatalog msf_catalog;
    if (is_live) {
        // Section 5.1.2: SHOULD NOT be included when isLive is false.
        msf_catalog.generated_at_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    for (const auto& track : tracks) {
        MsfTrack msf_track = make_msf_track(track, is_live);

        const auto init_it = init_data_by_track.find(track.track_name);
        if (init_it != init_data_by_track.end()) {
            const std::string init_id = track.track_name + "-init";
            msf_track.init_ref = init_id;
            msf_catalog.init_data_list.push_back(MsfInitData{
                .id = init_id,
                .type = "inline",
                .data = init_it->second,
            });
        }

        msf_catalog.tracks.push_back(std::move(msf_track));
    }

    const std::string catalog_text = serialize_catalog(msf_catalog);
    result.catalog_payload = std::vector<std::uint8_t>(catalog_text.begin(), catalog_text.end());
    return result;
```

The loop building `init_data_by_track` and `result.track_initializations` at lines 741-751 stays exactly as it is, except that `local_base64_encode` becomes `base64_encode`.

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --test-dir build -R openmoq-publisher-packaging-tests --output-on-failure`
Expected: PASS

- [ ] **Step 5: Update the transport test fixture**

`tests/moqt_session_test.cpp:1292` hardcodes a legacy catalog literal and line 1320 asserts on `"format":"cmsf"`. Replace the literal with:

```cpp
            "{\"version\":\"1\",\"tracks\":[{\"name\":\"vide_1\",\"packaging\":\"cmaf\","
            "\"role\":\"video\",\"isLive\":true,\"initRef\":\"vide_1-init\",\"bitrate\":2000000}],"
            "\"initDataList\":[{\"id\":\"vide_1-init\",\"type\":\"inline\",\"data\":\"AAAA\"}]}";
```

and change the line 1320 assertion to:

```cpp
            ok &= expect(served_catalog.find("\"version\":\"1\"") != std::string::npos,
```

- [ ] **Step 6: Run the full suite**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
git add src/cmsf_packager.cpp tests/cmaf_segmenter_test.cpp tests/moqt_session_test.cpp
git commit -m "Emit MSF v1 catalog from the live catalog builder"
```

---

### Task 7: Migrate the CTE LL-DASH and MSFTS emitters

**Files:**
- Modify: `src/live_dash_ingest.cpp:529-570` (`build_catalog_locked`)
- Modify: `examples/msfts-publisher/msfts_source.cpp:424-449` (`make_catalog`)
- Modify: `tests/live_dash_ingest_test.cpp:455-470`
- Modify: `examples/msfts-publisher/tests/msfts_source_test.cpp:130-146`

**Interfaces:**
- Consumes: everything from Tasks 1 and 4.
- Produces: nothing consumed by later tasks. This is the last emitter migration.

- [ ] **Step 1: Write the failing tests**

In `tests/live_dash_ingest_test.cpp`, replace the `initData` assertions around line 466 with:

```cpp
            ok &= expect(catalog_text.find("\"version\":\"1\"") != std::string::npos,
                         "expected MSF v1 string version in CTE catalog");
            ok &= expect(catalog_text.find("\"initDataList\"") != std::string::npos,
                         "expected root initDataList in CTE catalog");
            ok &= expect(catalog_text.find("\"initRef\"") != std::string::npos,
                         "expected per-track initRef in CTE catalog");
            ok &= expect(catalog_text.find("\"format\"") == std::string::npos,
                         "expected no legacy format field in CTE catalog");
```

In `examples/msfts-publisher/tests/msfts_source_test.cpp`, keep the `"packaging":"m2ts"` assertion at line 132 — `m2ts` is the packaging value from `draft-gregoire-moq-msfts-00` and stays correct. Replace the line 144 `initData` assertion with:

```cpp
        expect(text.find("\"version\":\"1\"") != std::string::npos,
               "expected MSF v1 string version in MSFTS catalog");
        expect(text.find("\"initDataList\"") != std::string::npos,
               "expected root initDataList in MSFTS catalog");
```

- [ ] **Step 2: Run tests to verify they fail**

Run:
```bash
cmake --build build && ctest --test-dir build -R "live-dash|msfts" --output-on-failure
```
Expected: FAIL on both targets — legacy shape still emitted

- [ ] **Step 3: Replace the CTE emitter**

In `src/live_dash_ingest.cpp`, add `#include "openmoq/publisher/msf_catalog.h"` and `#include <chrono>`, then replace the catalog JSON block at lines 530-564 with:

```cpp
    MsfCatalog msf_catalog;
    msf_catalog.generated_at_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    for (const auto& registered : tracks_) {
        const TrackDescription& track = registered.description;
        MsfTrack msf_track = make_msf_track(track, /*is_live=*/true);
        // CTE ingest always produces CMAF regardless of the parsed value.
        msf_track.packaging = "cmaf";

        if (!registered.init_data_base64.empty()) {
            const std::string init_id = track.track_name + "-init";
            msf_track.init_ref = init_id;
            msf_catalog.init_data_list.push_back(MsfInitData{
                .id = init_id,
                .type = "inline",
                .data = registered.init_data_base64,
            });
        }

        msf_catalog.tracks.push_back(std::move(msf_track));
    }

    const std::string payload = serialize_catalog(msf_catalog);
```

The existing `role_for_handler` helper becomes unused here; delete it if nothing else in the file calls it, otherwise leave it alone. Keep the `LiveObject` construction and the `catalog_group_id_++` behavior at line 569 exactly as-is.

- [ ] **Step 4: Replace the MSFTS emitter**

In `examples/msfts-publisher/msfts_source.cpp`, add `#include "openmoq/publisher/msf_catalog.h"` and rewrite `make_catalog`. This source has no `TrackDescription`, so it populates `MsfTrack` directly rather than calling `make_msf_track`:

```cpp
    using openmoq::publisher::MsfCatalog;
    using openmoq::publisher::MsfInitData;
    using openmoq::publisher::MsfTrack;

    MsfCatalog catalog;
    catalog.generated_at_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    MsfTrack track;
    track.name = config_.track_name;
    track.name_space = config_.track_namespace;
    track.packaging = "m2ts";
    track.role = "video";
    track.is_live = true;
    track.mime_type = "video/mp2t";
    track.target_latency_ms = 1000;
    // No measured rate is available from an M2TS source; 2 Mbps matches the
    // codec-class default resolve_bitrate applies elsewhere.
    track.bitrate = 2000000;
    track.init_ref = "m2ts-init";
    catalog.tracks.push_back(std::move(track));

    catalog.init_data_list.push_back(MsfInitData{
        .id = "m2ts-init",
        .type = "inline",
        .data = base64_encode(info_.init_data),
    });
```

The `m2tsPacketSize`, `m2tsPacketsPerObject`, `m2tsProgramNumber`, `m2tsPmtPid`, `m2tsPcrPid`, `m2tsPsiInterval`, `m2tsRandomAccess`, and `m2tsTimestampMode` fields are custom fields from `draft-gregoire-moq-msfts-00`. MSF section 5 permits custom fields provided they do not collide with spec names, and none of these do. Carry them in `MsfTrack::custom_fields`, whose values are raw JSON, so insert them before pushing the track:

```cpp
    track.custom_fields["m2tsPacketSize"] = std::to_string(info_.packet_size);
    track.custom_fields["m2tsPacketsPerObject"] = std::to_string(config_.packets_per_object);
    track.custom_fields["m2tsProgramNumber"] = std::to_string(info_.program_number);
    track.custom_fields["m2tsPmtPid"] = std::to_string(info_.pmt_pid);
    track.custom_fields["m2tsPcrPid"] = std::to_string(info_.pcr_pid);
    track.custom_fields["m2tsPsiInterval"] = std::to_string(kPsiIntervalMs);
    track.custom_fields["m2tsRandomAccess"] = "false";
    if (info_.packet_size == kM2tsPacketSize) {
        // A raw JSON value, so the string must arrive already quoted.
        track.custom_fields["m2tsTimestampMode"] = "\"opaque\"";
    }
    catalog.tracks.push_back(std::move(track));
```

This block replaces the `catalog.tracks.push_back(std::move(track));` line shown above, which moves down to the end. Then:

```cpp
    const std::string json = serialize_catalog(catalog);
    return {json.begin(), json.end()};
```

Add `#include <chrono>` if it is not already present. The inline `initData` field at line 446 is gone, replaced by the `initDataList` entry above.

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build -R "live-dash|msfts" --output-on-failure`
Expected: PASS

- [ ] **Step 6: Run the full suite**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS — all four emitters now produce v1 catalogs

- [ ] **Step 7: Commit**

```bash
git add src/live_dash_ingest.cpp examples/msfts-publisher/msfts_source.cpp tests/live_dash_ingest_test.cpp examples/msfts-publisher/tests/msfts_source_test.cpp
git commit -m "Emit MSF v1 catalog from the CTE and MSFTS sources"
```

---

### Task 8: CMSF max SAP starting types and documentation

**Files:**
- Modify: `src/cmsf_packager.cpp` (`build_publish_plan`)
- Modify: `tests/cmaf_segmenter_test.cpp`
- Modify: `docs/status.md:20-21`
- Modify: `docs/protocol-mapping.md`

**Interfaces:**
- Consumes: everything from Tasks 1 through 7.
- Produces: nothing. Final task of Phase 1.

- [ ] **Step 1: Write the failing test**

Append to the publish-plan section of `tests/cmaf_segmenter_test.cpp`:

```cpp
    // CMSF section 3.5.2: the max SAP type Groups and Objects start with.
    ok &= expect_contains(plan_catalog_text, "\"maxGrpSapStartingType\":",
                          "expected maxGrpSapStartingType on a CMAF media track");
    ok &= expect_contains(plan_catalog_text, "\"maxObjSapStartingType\":",
                          "expected maxObjSapStartingType on a CMAF media track");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-packaging-tests && ctest --test-dir build -R openmoq-publisher-packaging-tests --output-on-failure`
Expected: FAIL — fields not emitted

- [ ] **Step 3: Compute and set the SAP types**

In `build_publish_plan`, before building each media `MsfTrack`, scan `segmented_mp4.fragments` for that track name. Each fragment already carries `has_sap_type` and `sap_type` (threaded into `CmsfObject` at `src/cmsf_packager.cpp:580`). Compute:

- `max_obj_sap_starting_type` = the maximum `sap_type` across all fragments of the track that have `has_sap_type` set.
- `max_grp_sap_starting_type` = the maximum `sap_type` across only those fragments whose `object_id == 0`, i.e. the first Object of each Group.

Set both on the `MsfTrack` only when at least one fragment for that track had `has_sap_type` set; leave them unset otherwise, since both fields are Optional in CMSF section 3.5.2.

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --test-dir build -R openmoq-publisher-packaging-tests --output-on-failure`
Expected: PASS

- [ ] **Step 5: Update the documentation**

In `docs/status.md`, replace roadmap item 5 with:

```markdown
5. MSF/CMSF version 1: the catalog model conforms to `draft-ietf-moq-msf-01`
   and `draft-ietf-moq-cmsf-01`. Remaining phases are catalog track lifecycle
   (delta updates), CMSF content protection, and MSF URL parsing; see
   `docs/superpowers/specs/2026-07-28-msf-cmsf-v1-design.md`. MSF section 12
   compression signaling is blocked on transport draft-19 Track and Object
   Properties.
```

In `docs/protocol-mapping.md`, append a section recording the catalog contract:

```markdown
## MSF v1 catalog

- The catalog is a `draft-ietf-moq-msf-01` version 1 document. `version` is the
  String `"1"`; there is no root `format` field.
- Initialization data lives in the root `initDataList` array, referenced from
  each track by `initRef`, and is serialized after `tracks`.
- Media tracks carry `packaging` of `cmaf` per `draft-ietf-moq-cmsf-01` section
  3.5.1, along with `maxGrpSapStartingType` and `maxObjSapStartingType`.
- All catalog JSON is produced by `serialize_catalog()` in
  `src/msf_catalog.cpp`. Do not hand-assemble catalog JSON.
```

- [ ] **Step 6: Run the full suite**

Run:
```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: PASS

- [ ] **Step 7: Commit**

```bash
git add src/cmsf_packager.cpp tests/cmaf_segmenter_test.cpp docs/status.md docs/protocol-mapping.md
git commit -m "Add CMSF max SAP starting types and document the MSF v1 catalog"
```

---

## Out of Scope for Phase 1

Deferred to their own plans, per `docs/superpowers/specs/2026-07-28-msf-cmsf-v1-design.md`:

- **Phase 2** — catalog track lifecycle: independent catalog at object 0 of every group, `deltaUpdate` add/remove/clone, periodic republish, end-of-broadcast signaling. Needs a JSON reader.
- **Phase 3** — CMSF content protection: `sinf`/`schm`/`schi`/`tenc`/`pssh` parsing, `frma` codec unwrapping, and replacing the unanchored byte scan in `extract_codec_init_data` (`src/cmsf_packager.cpp:396`).
- **Phase 4** — MSF URL and fragment parsing, plus variable substitution.
- **MSF section 12** compression signaling, blocked on transport draft-19.
