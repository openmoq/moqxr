# CMSF Content Protection Implementation Plan (Phase 3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Detect CMAF Common Encryption in the publisher's input and signal it in the catalog per `draft-ietf-moq-cmsf-01` section 4, without ever decrypting, and correct `saio` offsets so encrypted content survives the CTE ingest path.

**Architecture:** A new `cenc` module owns CENC box structure and provides a length-walking child-box finder that replaces two unanchored byte scans. `extract_tracks` gains protection data per track and resolves codec strings through `frma`. The catalog model gains a root `contentProtections` array that tracks reference by ID. Finally, `rebuild_moof` corrects `saio` offsets by the moof-size delta.

**Tech Stack:** C++20, CMake, static library `openmoq_publisher_lib`, single-binary tests using a local `expect(bool, message)` helper returning a process exit code.

## Global Constraints

- C++20. No new third-party dependencies. Namespace `openmoq::publisher`. Headers use `#pragma once`.
- **The publisher never decrypts and never encrypts.** It signals protection present in its input and passes encrypted samples through untouched.
- All catalog JSON is produced by `serialize_catalog()` in `src/msf_catalog.cpp`. Never hand-assemble catalog JSON.
- This code parses untrusted media. Every read must be bounds-checked against the real buffer before it happens; malformed input degrades or refuses, never reads out of bounds.
- The project does **NOT** compile warning-free: 12 warnings tree-wide, no `-Werror`. Do not chase pre-existing warnings; do not add avoidable new ones.
- Tests are single-binary programs with local `expect(bool, std::string)` helpers. No test framework, no shared test header.
- **Build with this exact configuration** — without the libmoq flag the suite silently drops from 13 targets to 12 with no error:
  `cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF -DOPENMOQ_LIBMOQ_SOURCE_DIR=/media/mondain/terrorbyte/workspace/github-moq/moq5`
- The suite is **13/13** and must stay 13/13 (14/14 once Task 1 adds a target).
- Commit messages: no emoji, no "Generated with Claude Code" tagline, no Co-Authored-By line.

## Existing state you are building on

Phases 1 and 2 are merged. Relevant facts:

- `include/openmoq/publisher/msf_catalog.h` declares `MsfCatalog`, `MsfTrack`, `MsfBuffers`, `MsfInitData`, `MsfDeltaOp`, `CatalogObject`, `EndBroadcastMode`, `CatalogPublisher`, and free functions `serialize_catalog`, `make_msf_track`, `attach_init_data`, `make_end_broadcast_catalog`, `resolve_bitrate`, `bitrate_is_estimated`.
- `validate_catalog` and `validate_track` in `src/msf_catalog.cpp` enforce catalog invariants and throw `std::runtime_error` naming the offending track.
- `TrackDescription` in `include/openmoq/publisher/mp4_box.h` carries `track_id`, `handler_type`, `codec`, `sample_entry_type`, `track_name`, `packaging`, `event_type`, `mime_type`, `depends`, `timescale`, `width`, `height`, `channel_count`, `sample_rate`, `frame_rate`, `max_bitrate`, `avg_bitrate`, `duration_ms`, `language`, `codec_private`.
- `find_child_box_offset` (`src/mp4_box.cpp:411`) is memory-safe — it clamps to `std::min(offset + size, bytes.size())` and guards `size_t` wrap — but it still advances one byte at a time, so it can match a coincidental four-byte type with a plausible size field.
- `extract_codec_init_data` (`src/cmsf_packager.cpp`) is neither: it bounds by the raw unclamped `sample_entry.span.offset + sample_entry.span.size`.
- `codec_string_from_sample_entry` (`src/mp4_box.cpp`) dispatches on `sample_entry.type` and returns the type itself for anything unrecognised — so `encv` yields the codec string `"encv"`.
- `extract_tracks` already builds an `effective_sample_entry` copy with a rewritten type for the `hev1`-to-`hvc1` normalisation. That is the precedent to follow for `frma`.

## File Structure

| File | Responsibility |
| --- | --- |
| `include/openmoq/publisher/cenc.h` (create) | CENC structs and the length-walking box finder |
| `src/cenc.cpp` (create) | Box walking, `sinf`/`frma`/`schm`/`schi`/`tenc` and `pssh` parsing |
| `tests/cenc_test.cpp` (create) | Box walk, ASAN regression, CENC parsing |
| `include/openmoq/publisher/mp4_box.h` (modify) | `TrackDescription::protection` |
| `src/mp4_box.cpp` (modify) | `frma` codec resolution in `extract_tracks` |
| `src/cmsf_packager.cpp` (modify) | Replace `extract_codec_init_data`'s scan |
| `include/openmoq/publisher/msf_catalog.h` (modify) | `MsfUrlEntry`, `MsfContentProtection`, ref IDs |
| `src/msf_catalog.cpp` (modify) | Serialize and validate protection fields |
| `include/openmoq/publisher/publisher_api.h` (modify) | `DrmSystemConfig`, `PublisherConfig::drm_systems` |
| `src/drm_config.cpp` (create) | Parse the DRM config JSON file |
| `src/cmaf_segmenter.cpp` (modify) | `saio` correction in `rebuild_moof` |
| `CMakeLists.txt` (modify) | Register new sources and the test target |

---

### Task 1: Length-walking box finder, replacing the unclamped scan

**Files:**
- Create: `include/openmoq/publisher/cenc.h`, `src/cenc.cpp`, `tests/cenc_test.cpp`
- Modify: `src/cmsf_packager.cpp` (`extract_codec_init_data`), `CMakeLists.txt`

**Interfaces:**
- Consumes: `Mp4Box`, `ByteSpan` from `openmoq/publisher/mp4_box.h`.
- Produces: `std::optional<ByteSpan> find_child_box_span(std::span<const std::uint8_t> bytes, std::size_t container_offset, std::size_t container_size, std::size_t first_child_offset, std::string_view type)`. Tasks 2, 3, 6, and 7 all use it.

This task fixes a live memory-safety bug. `extract_codec_init_data` bounds its scan by the raw `sample_entry.span.offset + sample_entry.span.size`, where `span.size` is an unvalidated `read_be32` from the file, then builds `bytes.begin() + (cursor + box_size)` from that bound. It is reachable from `build_publish_plan`, so publishing a malformed MP4 hits it.

- [ ] **Step 1: Write the failing test**

Create `tests/cenc_test.cpp`:

```cpp
#include "openmoq/publisher/cenc.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

void append_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

std::vector<std::uint8_t> make_box(const std::string& type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    append_be32(out, static_cast<std::uint32_t>(8 + payload.size()));
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

}  // namespace

int main() {
    using namespace openmoq::publisher;

    bool ok = true;

    // A container holding three children: aaaa, bbbb, cccc.
    std::vector<std::uint8_t> payload;
    const auto a = make_box("aaaa", {1, 2, 3, 4});
    const auto b = make_box("bbbb", {5, 6});
    const auto c = make_box("cccc", {7, 8, 9});
    payload.insert(payload.end(), a.begin(), a.end());
    payload.insert(payload.end(), b.begin(), b.end());
    payload.insert(payload.end(), c.begin(), c.end());

    const auto found_b = find_child_box_span(payload, 0, payload.size(), 0, "bbbb");
    ok &= expect(found_b.has_value(), "expected to find bbbb");
    ok &= expect(found_b->offset == a.size(), "expected bbbb at the offset after aaaa");
    ok &= expect(found_b->size == b.size(), "expected bbbb's full box size");

    const auto missing = find_child_box_span(payload, 0, payload.size(), 0, "zzzz");
    ok &= expect(!missing.has_value(), "expected absent type to return nullopt");

    // A length-walk must NOT match a type string that appears inside another
    // box's PAYLOAD. A sliding byte scan would falsely match here.
    std::vector<std::uint8_t> decoy_payload;
    const auto decoy = make_box("aaaa", {'c', 'c', 'c', 'c', 0, 0, 0, 16});
    decoy_payload.insert(decoy_payload.end(), decoy.begin(), decoy.end());
    const auto not_found = find_child_box_span(decoy_payload, 0, decoy_payload.size(), 0, "cccc");
    ok &= expect(!not_found.has_value(),
                 "expected a length walk to ignore a type appearing inside a payload");

    // A container size larger than the buffer must not drive reads past the
    // end. This is the memory-safety case: the declared size is fabricated.
    const auto oversized = find_child_box_span(payload, 0, 0x40000000, 0, "zzzz");
    ok &= expect(!oversized.has_value(), "expected a fabricated container size to be clamped");

    // A child whose declared length runs past the container terminates the
    // walk rather than being skipped.
    std::vector<std::uint8_t> truncated;
    append_be32(truncated, 0x40000000);
    truncated.insert(truncated.end(), {'a', 'a', 'a', 'a'});
    const auto bad_len = find_child_box_span(truncated, 0, truncated.size(), 0, "aaaa");
    ok &= expect(!bad_len.has_value(), "expected an over-long child length to terminate the walk");

    // A zero or sub-header length must terminate rather than loop forever.
    std::vector<std::uint8_t> zero_len;
    append_be32(zero_len, 0);
    zero_len.insert(zero_len.end(), {'a', 'a', 'a', 'a'});
    const auto zero = find_child_box_span(zero_len, 0, zero_len.size(), 0, "aaaa");
    ok &= expect(!zero.has_value(), "expected a zero length to terminate the walk");

    return ok ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-cenc-tests`
Expected: FAIL — `openmoq/publisher/cenc.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `include/openmoq/publisher/cenc.h`:

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "openmoq/publisher/mp4_box.h"

namespace openmoq::publisher {

// Find a child box by type inside a container, walking the box chain by each
// box's declared length rather than sliding a cursor one byte at a time.
//
// A sliding scan can match four bytes that merely LOOK like a box type inside
// another box's payload -- a real hazard when walking encrypted sample
// entries, where tenc key IDs and PSSH payloads are arbitrary bytes.
//
// container_size is taken from the file and is NOT trusted: it is clamped to
// the real buffer, with a guard against size_t wrap in the sum. A child whose
// declared length is under 8, or which would run past the clamped limit,
// terminates the walk -- a malformed length means the rest cannot be trusted
// to be a box chain at all.
//
// Returns the child's full box span, header included, or nullopt.
std::optional<ByteSpan> find_child_box_span(std::span<const std::uint8_t> bytes,
                                            std::size_t container_offset,
                                            std::size_t container_size,
                                            std::size_t first_child_offset,
                                            std::string_view type);

}  // namespace openmoq::publisher
```

- [ ] **Step 4: Write the implementation**

Create `src/cenc.cpp`:

```cpp
#include "openmoq/publisher/cenc.h"

#include <algorithm>
#include <cstring>

namespace openmoq::publisher {

namespace {

std::uint32_t read_be32_at(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

}  // namespace

std::optional<ByteSpan> find_child_box_span(std::span<const std::uint8_t> bytes,
                                            std::size_t container_offset,
                                            std::size_t container_size,
                                            std::size_t first_child_offset,
                                            std::string_view type) {
    if (type.size() != 4) {
        return std::nullopt;
    }

    // container_size is attacker-controlled. Guard the sum against size_t wrap
    // before clamping to the real buffer.
    std::size_t container_end = container_offset + container_size;
    if (container_end < container_offset) {
        container_end = bytes.size();
    }
    const std::size_t limit = std::min(container_end, bytes.size());

    std::size_t cursor = container_offset + first_child_offset;
    if (cursor < container_offset) {
        return std::nullopt;
    }

    while (cursor + 8 <= limit) {
        const std::uint32_t box_size = read_be32_at(bytes, cursor);
        // A length below the 8-byte header, or one running past the limit,
        // means the remaining bytes are not a valid box chain. Stop rather
        // than skip: skipping is what turns a walk back into a scan.
        if (box_size < 8) {
            return std::nullopt;
        }
        const std::size_t box_end = cursor + box_size;
        if (box_end < cursor || box_end > limit) {
            return std::nullopt;
        }

        if (std::memcmp(bytes.data() + cursor + 4, type.data(), 4) == 0) {
            return ByteSpan{.offset = cursor, .size = box_size};
        }

        cursor = box_end;
    }

    return std::nullopt;
}

}  // namespace openmoq::publisher
```

- [ ] **Step 5: Replace the unclamped scan**

In `src/cmsf_packager.cpp`, add `#include "openmoq/publisher/cenc.h"` and replace the whole body of `extract_codec_init_data` with a length walk over the four codec-config types:

```cpp
std::vector<std::uint8_t> extract_codec_init_data(const Mp4Box& sample_entry,
                                                  std::span<const std::uint8_t> bytes,
                                                  std::size_t child_offset) {
    // Previously a sliding byte scan bounded by the RAW, unvalidated
    // sample_entry.span.size -- a memory-safety bug and a false-positive
    // hazard. find_child_box_span walks by declared length and clamps to the
    // real buffer.
    for (const std::string_view type : {"avcC", "hvcC", "esds", "dOps"}) {
        const auto span = find_child_box_span(bytes,
                                              sample_entry.span.offset,
                                              sample_entry.span.size,
                                              child_offset,
                                              type);
        if (span.has_value()) {
            return std::vector<std::uint8_t>(
                bytes.begin() + static_cast<std::ptrdiff_t>(span->offset),
                bytes.begin() + static_cast<std::ptrdiff_t>(span->offset + span->size));
        }
    }

    throw std::runtime_error("catalog generation could not locate codec initData box in sample entry");
}
```

- [ ] **Step 6: Register in CMake**

Add `src/cenc.cpp` to the `add_library(openmoq_publisher_lib STATIC ...)` list, after `src/cat4moq.cpp` to keep it near the front alphabetically. Then add a test target inside the `if(OPENMOQ_BUILD_TESTS)` block, after the `openmoq-publisher-msf-catalog-tests` block:

```cmake
    add_executable(openmoq-publisher-cenc-tests
        tests/cenc_test.cpp
    )
    target_link_libraries(openmoq-publisher-cenc-tests PRIVATE openmoq_publisher_lib)
    add_test(NAME openmoq-publisher-cenc-tests COMMAND openmoq-publisher-cenc-tests)
```

- [ ] **Step 7: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS, **14/14** (this task adds a target).

- [ ] **Step 8: Prove the memory-safety fix with ASAN**

Build an ASAN copy in the scratch directory, not the repo:

```bash
cmake -S . -B /tmp/cenc-asan -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF \
  -DOPENMOQ_LIBMOQ_SOURCE_DIR=/media/mondain/terrorbyte/workspace/github-moq/moq5 \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -O1 -g -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build /tmp/cenc-asan --target openmoq-publisher-cenc-tests
/tmp/cenc-asan/openmoq-publisher-cenc-tests
```
Expected: exit 0, no sanitizer report.

Then temporarily revert the clamp in `find_child_box_span` — replace `const std::size_t limit = std::min(container_end, bytes.size());` with `const std::size_t limit = container_end;` — rebuild, and rerun. The fabricated-container-size case must produce a heap-buffer-overflow. Restore the clamp, rebuild, confirm clean. Record both observations in the report, then `rm -rf /tmp/cenc-asan`.

- [ ] **Step 9: Commit**

```bash
git add include/openmoq/publisher/cenc.h src/cenc.cpp tests/cenc_test.cpp src/cmsf_packager.cpp CMakeLists.txt
git commit -m "Replace unanchored codec-config scan with a length-walking box finder"
```

---

### Task 2: CENC box parsing

**Files:**
- Modify: `include/openmoq/publisher/cenc.h`, `src/cenc.cpp`, `tests/cenc_test.cpp`

**Interfaces:**
- Consumes: `find_child_box_span` from Task 1.
- Produces: `struct CencTrackProtection`, `struct CencSystem`, `std::optional<CencTrackProtection> parse_track_protection(std::span<const std::uint8_t> bytes, const Mp4Box& sample_entry)`, `std::vector<CencSystem> parse_pssh_boxes(std::span<const std::uint8_t> bytes, const std::vector<Mp4Box>& top_level_boxes)`. Tasks 3, 4, 6, and 7 use these.

- [ ] **Step 1: Write the failing test**

Add to `tests/cenc_test.cpp`. First add these fixture builders to the anonymous namespace:

```cpp
std::vector<std::uint8_t> make_full_box(const std::string& type,
                                        std::uint8_t version,
                                        std::uint32_t flags,
                                        const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    out.push_back(version);
    out.push_back(static_cast<std::uint8_t>((flags >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((flags >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(flags & 0xFFU));
    out.insert(out.end(), payload.begin(), payload.end());
    return make_box(type, out);
}

// 16 distinct bytes so a transposed read is visible.
std::vector<std::uint8_t> sample_kid() {
    return {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
            0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
}

// tenc: version 0, flags 0, reserved(1), reserved(1),
// default_isProtected(1), default_Per_Sample_IV_Size(1), default_KID(16).
std::vector<std::uint8_t> make_tenc(std::uint8_t is_protected, std::uint8_t iv_size) {
    std::vector<std::uint8_t> payload{0, 0, is_protected, iv_size};
    const auto kid = sample_kid();
    payload.insert(payload.end(), kid.begin(), kid.end());
    return make_full_box("tenc", 0, 0, payload);
}

// An encv sample entry: 8-byte header + 70-byte VisualSampleEntry body,
// then children. sinf holds frma, schm, and schi/tenc.
std::vector<std::uint8_t> make_encv_sample_entry(const std::string& original_format,
                                                 const std::string& scheme) {
    std::vector<std::uint8_t> schm_payload;
    schm_payload.insert(schm_payload.end(), scheme.begin(), scheme.end());
    append_be32(schm_payload, 0x00010000);

    std::vector<std::uint8_t> frma_payload(original_format.begin(), original_format.end());

    std::vector<std::uint8_t> schi_payload;
    const auto tenc = make_tenc(1, 8);
    schi_payload.insert(schi_payload.end(), tenc.begin(), tenc.end());

    std::vector<std::uint8_t> sinf_payload;
    const auto frma = make_box("frma", frma_payload);
    const auto schm = make_full_box("schm", 0, 0, schm_payload);
    const auto schi = make_box("schi", schi_payload);
    sinf_payload.insert(sinf_payload.end(), frma.begin(), frma.end());
    sinf_payload.insert(sinf_payload.end(), schm.begin(), schm.end());
    sinf_payload.insert(sinf_payload.end(), schi.begin(), schi.end());

    std::vector<std::uint8_t> body(70, 0);
    const auto sinf = make_box("sinf", sinf_payload);
    body.insert(body.end(), sinf.begin(), sinf.end());
    return make_box("encv", body);
}

std::vector<std::uint8_t> make_pssh(const std::vector<std::uint8_t>& system_id) {
    std::vector<std::uint8_t> payload(system_id.begin(), system_id.end());
    append_be32(payload, 4);
    payload.insert(payload.end(), {0xde, 0xad, 0xbe, 0xef});
    return make_full_box("pssh", 0, 0, payload);
}
```

Then append to `main()`:

```cpp
    // sinf/frma/schm/schi/tenc parsing from an encv sample entry.
    const auto encv = make_encv_sample_entry("avc1", "cenc");
    const Mp4Box encv_box{
        .type = "encv",
        .span = {.offset = 0, .size = encv.size()},
        .payload = {.offset = 8, .size = encv.size() - 8},
        .children = {},
    };
    const auto protection = parse_track_protection(encv, encv_box);
    ok &= expect(protection.has_value(), "expected an encv sample entry to parse as protected");
    ok &= expect(protection->original_format == "avc1", "expected frma to yield avc1");
    ok &= expect(protection->scheme == "cenc", "expected schm to yield cenc");
    ok &= expect(protection->is_protected, "expected tenc default_isProtected");
    ok &= expect(protection->per_sample_iv_size == 8, "expected tenc IV size 8");
    ok &= expect(protection->default_kid == "01234567-89ab-cdef-0123-456789abcdef",
                 "expected the KID rendered as a UUID string");

    // cbcs is the other allowed scheme (CMSF 4.1.1.3).
    const auto encv_cbcs = make_encv_sample_entry("avc1", "cbcs");
    const Mp4Box cbcs_box{
        .type = "encv",
        .span = {.offset = 0, .size = encv_cbcs.size()},
        .payload = {.offset = 8, .size = encv_cbcs.size() - 8},
        .children = {},
    };
    const auto cbcs = parse_track_protection(encv_cbcs, cbcs_box);
    ok &= expect(cbcs.has_value() && cbcs->scheme == "cbcs", "expected cbcs scheme parsed");

    // An unencrypted sample entry has no sinf and must parse as unprotected.
    std::vector<std::uint8_t> plain_body(70, 0);
    const auto plain = make_box("avc1", plain_body);
    const Mp4Box plain_box{
        .type = "avc1",
        .span = {.offset = 0, .size = plain.size()},
        .payload = {.offset = 8, .size = plain.size() - 8},
        .children = {},
    };
    ok &= expect(!parse_track_protection(plain, plain_box).has_value(),
                 "expected an unencrypted sample entry to yield no protection");

    // Two pssh boxes yield two systems, each with its own base64 payload.
    const std::vector<std::uint8_t> widevine{0xed, 0xef, 0x8b, 0xa9, 0x79, 0xd6, 0x4a, 0xce,
                                             0xa3, 0xc8, 0x27, 0xdc, 0xd5, 0x1d, 0x21, 0xed};
    const std::vector<std::uint8_t> playready{0x9a, 0x04, 0xf0, 0x79, 0x98, 0x40, 0x42, 0x86,
                                              0xab, 0x92, 0xe6, 0x5b, 0xe0, 0x88, 0x5f, 0x95};
    std::vector<std::uint8_t> two_pssh;
    const auto p1 = make_pssh(widevine);
    const auto p2 = make_pssh(playready);
    two_pssh.insert(two_pssh.end(), p1.begin(), p1.end());
    two_pssh.insert(two_pssh.end(), p2.begin(), p2.end());
    std::vector<Mp4Box> pssh_boxes{
        Mp4Box{.type = "pssh", .span = {.offset = 0, .size = p1.size()},
               .payload = {.offset = 8, .size = p1.size() - 8}, .children = {}},
        Mp4Box{.type = "pssh", .span = {.offset = p1.size(), .size = p2.size()},
               .payload = {.offset = p1.size() + 8, .size = p2.size() - 8}, .children = {}},
    };
    const auto systems = parse_pssh_boxes(two_pssh, pssh_boxes);
    ok &= expect(systems.size() == 2, "expected two DRM systems");
    ok &= expect(systems[0].system_id == "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed",
                 "expected the Widevine system ID");
    ok &= expect(systems[1].system_id == "9a04f079-9840-4286-ab92-e65be0885f95",
                 "expected the PlayReady system ID");
    ok &= expect(!systems[0].pssh_base64.empty(), "expected base64 pssh bytes");
    ok &= expect(systems[0].pssh_base64 != systems[1].pssh_base64,
                 "expected distinct pssh payloads per system");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-cenc-tests`
Expected: FAIL — `parse_track_protection` and `parse_pssh_boxes` are not declared.

- [ ] **Step 3: Declare the structs and functions**

**The two structs go in `include/openmoq/publisher/mp4_box.h`, NOT in `cenc.h`.** `TrackDescription` gains a `CencTrackProtection` member in Task 3, and `cenc.h` already includes `mp4_box.h` for `Mp4Box` and `ByteSpan` — declaring them in `cenc.h` would make the include circular. They are plain data and belong beside `TrackDescription`.

Add to `include/openmoq/publisher/mp4_box.h`, before `TrackDescription`:

```cpp
// Common Encryption parameters for one track, from sinf/schm/schi/tenc.
struct CencTrackProtection {
    std::string original_format;   // frma, e.g. "avc1" -- the pre-encryption codec
    std::string scheme;            // schm, "cenc" or "cbcs" (CMSF 4.1.1.3)
    std::string default_kid;       // tenc default_KID as a UUID string
    std::uint8_t per_sample_iv_size = 0;
    bool is_protected = false;
};

// One DRM system's initialisation data, from a pssh box.
struct CencSystem {
    std::string system_id;         // UUID string
    std::string pssh_base64;       // the whole pssh box, Base64
};
```

Then add the function declarations to `include/openmoq/publisher/cenc.h`, before its closing namespace:

```cpp
// Parse protection parameters from an encv or enca sample entry. Returns
// nullopt for an unencrypted entry, or when the boxes are absent or
// malformed -- a track whose protection cannot be confirmed must not be
// advertised as protected.
std::optional<CencTrackProtection> parse_track_protection(std::span<const std::uint8_t> bytes,
                                                          const Mp4Box& sample_entry);

// Collect DRM systems from pssh boxes. Boxes with a malformed length or a
// truncated system ID are skipped, not thrown on.
std::vector<CencSystem> parse_pssh_boxes(std::span<const std::uint8_t> bytes,
                                         const std::vector<Mp4Box>& pssh_boxes);
```

- [ ] **Step 4: Implement**

Add to `src/cenc.cpp`. Put the helpers in the existing anonymous namespace:

```cpp
// 16 raw bytes to xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx, the form CMSF
// section 4.1.1.2 requires.
std::string uuid_string(std::span<const std::uint8_t> id) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        out.push_back(kHex[(id[i] >> 4U) & 0x0FU]);
        out.push_back(kHex[id[i] & 0x0FU]);
    }
    return out;
}

std::string base64_encode(std::span<const std::uint8_t> bytes) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t b0 = bytes[i];
        const std::uint32_t b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const std::uint32_t b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const std::uint32_t word = (b0 << 16U) | (b1 << 8U) | b2;
        out.push_back(kAlphabet[(word >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(word >> 12U) & 0x3FU]);
        out.push_back(i + 1 < bytes.size() ? kAlphabet[(word >> 6U) & 0x3FU] : '=');
        out.push_back(i + 2 < bytes.size() ? kAlphabet[word & 0x3FU] : '=');
    }
    return out;
}
```

Then the two public functions:

```cpp
std::optional<CencTrackProtection> parse_track_protection(std::span<const std::uint8_t> bytes,
                                                          const Mp4Box& sample_entry) {
    if (sample_entry.type != "encv" && sample_entry.type != "enca") {
        return std::nullopt;
    }
    // Children begin past the sample entry header: 8 + 70 for visual,
    // 8 + 28 for audio. Same offsets build_track_codec_init_data uses.
    const std::size_t child_offset = sample_entry.type == "encv" ? 8 + 70 : 8 + 28;

    const auto sinf = find_child_box_span(bytes, sample_entry.span.offset,
                                          sample_entry.span.size, child_offset, "sinf");
    if (!sinf.has_value()) {
        return std::nullopt;
    }

    CencTrackProtection out;

    const auto frma = find_child_box_span(bytes, sinf->offset, sinf->size, 8, "frma");
    if (!frma.has_value() || frma->size < 12) {
        return std::nullopt;
    }
    out.original_format.assign(reinterpret_cast<const char*>(bytes.data() + frma->offset + 8), 4);

    // schm is a FullBox: 8-byte header + 4-byte version/flags, then
    // scheme_type(4) and scheme_version(4).
    const auto schm = find_child_box_span(bytes, sinf->offset, sinf->size, 8, "schm");
    if (!schm.has_value() || schm->size < 20) {
        return std::nullopt;
    }
    out.scheme.assign(reinterpret_cast<const char*>(bytes.data() + schm->offset + 12), 4);

    const auto schi = find_child_box_span(bytes, sinf->offset, sinf->size, 8, "schi");
    if (!schi.has_value()) {
        return std::nullopt;
    }
    // tenc is a FullBox: header(8) + version/flags(4) + reserved(1) +
    // reserved(1) + default_isProtected(1) + default_Per_Sample_IV_Size(1)
    // + default_KID(16) = 32 bytes minimum.
    const auto tenc = find_child_box_span(bytes, schi->offset, schi->size, 8, "tenc");
    if (!tenc.has_value() || tenc->size < 32) {
        return std::nullopt;
    }
    out.is_protected = bytes[tenc->offset + 14] != 0;
    out.per_sample_iv_size = bytes[tenc->offset + 15];
    out.default_kid = uuid_string(bytes.subspan(tenc->offset + 16, 16));

    return out;
}

std::vector<CencSystem> parse_pssh_boxes(std::span<const std::uint8_t> bytes,
                                         const std::vector<Mp4Box>& pssh_boxes) {
    std::vector<CencSystem> out;
    for (const auto& box : pssh_boxes) {
        if (box.type != "pssh") {
            continue;
        }
        // header(8) + version/flags(4) + SystemID(16) = 28 bytes minimum.
        if (box.span.size < 28 || box.span.offset + box.span.size > bytes.size()) {
            continue;
        }
        CencSystem system;
        system.system_id = uuid_string(bytes.subspan(box.span.offset + 12, 16));
        system.pssh_base64 = base64_encode(bytes.subspan(box.span.offset, box.span.size));
        out.push_back(std::move(system));
    }
    return out;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS, 14/14.

- [ ] **Step 6: Commit**

```bash
git add include/openmoq/publisher/cenc.h src/cenc.cpp tests/cenc_test.cpp
git commit -m "Parse CENC protection parameters from sinf and pssh boxes"
```

---

### Task 3: Codec strings through `frma`

**Files:**
- Modify: `include/openmoq/publisher/mp4_box.h` (`TrackDescription`), `src/mp4_box.cpp` (`extract_tracks`)
- Modify: `tests/cmaf_segmenter_test.cpp`

**Interfaces:**
- Consumes: `CencTrackProtection`, `parse_track_protection` from Task 2.
- Produces: `TrackDescription::protection` of type `std::optional<CencTrackProtection>`. Task 4 reads it.

This is a correctness fix independent of DRM signalling. `codec_string_from_sample_entry` dispatches on `sample_entry.type`, so an encrypted AVC track currently reports the codec string `"encv"`.

- [ ] **Step 1: Write the failing test**

Add to `tests/cmaf_segmenter_test.cpp`. Build a fragmented MP4 whose video sample entry is `encv` wrapping `avc1`, using the same box builders the file already has, then:

```cpp
    ok &= expect(enc_tracks.size() == 1, "expected one track in the encrypted fixture");
    ok &= expect(enc_tracks.front().sample_entry_type == "encv",
                 "expected the raw sample entry type preserved");
    ok &= expect(enc_tracks.front().codec.rfind("avc1", 0) == 0,
                 "expected the codec string resolved through frma, not left as encv");
    ok &= expect(enc_tracks.front().protection.has_value(),
                 "expected protection parameters on an encrypted track");
    ok &= expect(enc_tracks.front().protection->scheme == "cenc",
                 "expected the cenc scheme recorded");
```

Bind `enc_tracks` with the existing `extract_tracks(parse_mp4_boxes(bytes), bytes)` pattern used elsewhere in the file. The `encv` sample entry must contain BOTH a `sinf` and an `avcC`, since the codec string still needs the `avcC` profile bytes.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-packaging-tests && ctest --test-dir build -R openmoq-publisher-packaging-tests --output-on-failure`
Expected: FAIL — `TrackDescription` has no member `protection`, and the codec string is `"encv"`.

- [ ] **Step 3: Extend TrackDescription**

`CencTrackProtection` is already declared in `include/openmoq/publisher/mp4_box.h` by Task 2, so no new include is needed beyond `<optional>` if it is not already present. Add to `TrackDescription` after `std::string language;`:

```cpp
    // Present when the sample entry is encv or enca and its sinf parsed.
    std::optional<CencTrackProtection> protection;
```

`src/mp4_box.cpp` needs `#include "openmoq/publisher/cenc.h"` to call `parse_track_protection`.

- [ ] **Step 4: Resolve the codec string through `frma`**

In `src/mp4_box.cpp`, inside `extract_tracks`, after `sample_entry` is located and before `codec_string_from_sample_entry` is called, parse protection and build an effective sample entry with the unwrapped type. `extract_tracks` already does exactly this for the `hev1`-to-`hvc1` normalisation — follow that precedent:

```cpp
        std::optional<CencTrackProtection> protection =
            parse_track_protection(bytes, sample_entry);
        std::string effective_type = sample_entry_type;
        if (protection.has_value() && !protection->original_format.empty()) {
            // CMSF 4: the catalog must advertise the pre-encryption codec, not
            // the encv/enca wrapper.
            effective_type = protection->original_format;
        }
```

Then use `effective_type` when constructing the `effective_sample_entry` passed to `codec_string_from_sample_entry`, and add `.protection = std::move(protection),` to the `TrackDescription` aggregate initialiser after `.language = language,`. C++20 designated initialisers must follow declaration order, so `protection` must be declared after `language` and before `codec_private`.

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS, 14/14.

- [ ] **Step 6: Commit**

```bash
git add include/openmoq/publisher/mp4_box.h src/mp4_box.cpp include/openmoq/publisher/cenc.h tests/cmaf_segmenter_test.cpp
git commit -m "Resolve codec strings through frma for encrypted tracks"
```

---

### Task 4: Catalog content-protection signalling

**Files:**
- Modify: `include/openmoq/publisher/msf_catalog.h`, `src/msf_catalog.cpp`, `tests/msf_catalog_test.cpp`

**Interfaces:**
- Consumes: `CencTrackProtection`, `CencSystem`.
- Produces: `MsfUrlEntry`, `MsfContentProtection`, `MsfCatalog::content_protections`, `MsfTrack::content_protection_ref_ids`, and `void attach_content_protection(MsfCatalog&, MsfTrack&, const CencTrackProtection&, const std::vector<CencSystem>&)`. Task 6 configures the URL fields.

- [ ] **Step 1: Write the failing test**

Append to `main()` in `tests/msf_catalog_test.cpp`:

```cpp
    // CMSF 4.1: protection lives at the catalog root; tracks reference it by
    // refID and MUST NOT duplicate it.
    MsfCatalog prot;
    MsfContentProtection cp;
    cp.ref_id = "1";
    cp.default_kids = {"01234567-89ab-cdef-0123-456789abcdef"};
    cp.scheme = "cbcs";
    cp.system_id = "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed";
    cp.la_url = MsfUrlEntry{.url = "https://widevine.example.com/proxy", .type = std::nullopt};
    cp.pssh_base64 = "AAAAP3Bzc2gAAAAA";
    prot.content_protections.push_back(cp);

    MsfTrack prot_video;
    prot_video.name = "video";
    prot_video.packaging = "cmaf";
    prot_video.role = "video";
    prot_video.is_live = true;
    prot_video.codec = "avc1.640028";
    prot_video.bitrate = 5000000;
    prot_video.content_protection_ref_ids = {"1"};
    prot.tracks.push_back(prot_video);

    const std::string prot_json = serialize_catalog(prot);
    ok &= expect_contains(prot_json, "\"contentProtections\"", "expected root contentProtections");
    ok &= expect_contains(prot_json, "\"refID\":\"1\"", "expected refID");
    ok &= expect_contains(prot_json, "\"scheme\":\"cbcs\"", "expected the scheme");
    ok &= expect_contains(prot_json, "\"systemID\":\"edef8ba9-79d6-4ace-a3c8-27dcd51d21ed\"",
                          "expected the system ID inside drmSystem");
    ok &= expect_contains(prot_json, "\"laURL\":{\"url\":\"https://widevine.example.com/proxy\"}",
                          "expected the licence URL object");
    ok &= expect_contains(prot_json, "\"contentProtectionRefIDs\":[\"1\"]",
                          "expected the track's reference array");
    // 4.1.1: protection data MUST NOT be duplicated at track level.
    const std::size_t cp_pos = prot_json.find("\"contentProtections\"");
    const std::size_t tracks_pos = prot_json.find("\"tracks\"");
    ok &= expect(cp_pos != std::string::npos && tracks_pos != std::string::npos &&
                     cp_pos < tracks_pos,
                 "expected contentProtections before tracks, matching the CMSF examples");
    ok &= expect(prot_json.find("\"scheme\"", tracks_pos) == std::string::npos,
                 "expected no scheme duplicated inside the tracks array");

    // A dangling reference throws, mirroring the initRef rule.
    MsfCatalog dangling = prot;
    dangling.tracks[0].content_protection_ref_ids = {"missing"};
    ok &= throws_runtime_error(dangling, "expected a dangling contentProtectionRefID to throw");

    // Duplicate refIDs throw.
    MsfCatalog dup_ref = prot;
    dup_ref.content_protections.push_back(cp);
    ok &= throws_runtime_error(dup_ref, "expected duplicate refIDs to throw");

    // CMSF 4.1.1.3 allows only cenc and cbcs.
    MsfCatalog bad_scheme = prot;
    bad_scheme.content_protections[0].scheme = "rot13";
    ok &= throws_runtime_error(bad_scheme, "expected an unsupported scheme to throw");

    // 4.1.1.2 requires at least one well-formed default KID.
    MsfCatalog no_kid = prot;
    no_kid.content_protections[0].default_kids.clear();
    ok &= throws_runtime_error(no_kid, "expected an empty defaultKID list to throw");

    MsfCatalog bad_kid = prot;
    bad_kid.content_protections[0].default_kids = {"not-a-uuid"};
    ok &= throws_runtime_error(bad_kid, "expected a malformed KID to throw");

    MsfCatalog bad_system = prot;
    bad_system.content_protections[0].system_id = "nope";
    ok &= throws_runtime_error(bad_system, "expected a malformed system ID to throw");

    // An unprotected catalog emits no contentProtections key at all.
    MsfCatalog unprot;
    unprot.tracks.push_back(sapped_video);
    ok &= expect_not_contains(serialize_catalog(unprot), "\"contentProtections\"",
                              "expected no contentProtections when nothing is protected");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-msf-catalog-tests`
Expected: FAIL — `MsfContentProtection` and `MsfUrlEntry` are not declared.

- [ ] **Step 3: Declare the structs**

In `include/openmoq/publisher/msf_catalog.h`, add before `MsfCatalog`:

```cpp
// A URL with an optional type, used by the DRM system fields in CMSF 4.1.1.4.
// Declared before MsfContentProtection, which holds these by value.
//
// Note: CMSF 4.1.1.4.4 describes an Authorization URL but never names its JSON
// key, and the draft's examples show only laURL and certURL. It is therefore
// NOT modelled here -- emitting an invented key would be guessing at the spec,
// and nothing in DrmSystemConfig could populate it anyway.
struct MsfUrlEntry {
    std::string url;
    std::optional<std::string> type;
};

// One DRM system configuration at the catalog root (CMSF 4.1.1). Tracks
// reference these by ref_id; the data is never duplicated on a track.
struct MsfContentProtection {
    std::string ref_id;                       // 4.1.1.1, required
    std::vector<std::string> default_kids;    // 4.1.1.2, required
    std::string scheme;                       // 4.1.1.3, "cenc" or "cbcs"
    std::string system_id;                    // 4.1.1.4.1, required
    std::optional<MsfUrlEntry> la_url;        // 4.1.1.4.2
    std::optional<MsfUrlEntry> cert_url;      // 4.1.1.4.3
    std::optional<std::string> pssh_base64;   // 4.1.1.4.5
    std::optional<std::string> robustness;    // 4.1.1.4.6
};
```

Add to `MsfCatalog`, after `is_complete`:

```cpp
    std::vector<MsfContentProtection> content_protections;   // 4.1.1
```

Add to `MsfTrack`, after `custom_fields`:

```cpp
    std::vector<std::string> content_protection_ref_ids;     // 4.1.2
```

And declare the helper:

```cpp
// Add or reuse a root contentProtections entry for each DRM system carrying
// this track's protection, and point the track at them by refID. Systems
// already present (matched by system_id and scheme) are reused rather than
// duplicated, so two tracks sharing a KID share entries.
void attach_content_protection(MsfCatalog& catalog,
                               MsfTrack& track,
                               const CencTrackProtection& protection,
                               const std::vector<CencSystem>& systems);
```

- [ ] **Step 4: Serialize**

In `src/msf_catalog.cpp`, inside `serialize_catalog`, emit `contentProtections` after `isComplete` and before `tracks`, matching the CMSF examples in sections 5.2 and 5.3. Skip it entirely for a delta catalog, alongside `tracks`:

```cpp
    if (!is_delta && !catalog.content_protections.empty()) {
        seq.separate();
        out << "\"contentProtections\":[";
        JsonSeq cps(out);
        for (const auto& cp : catalog.content_protections) {
            cps.separate();
            out << '{';
            JsonSeq cp_seq(out);
            write_string(out, cp_seq, "refID", cp.ref_id);
            write_string_array(out, cp_seq, "defaultKID", cp.default_kids);
            write_string(out, cp_seq, "scheme", cp.scheme);
            cp_seq.separate();
            out << "\"drmSystem\":{";
            JsonSeq ds(out);
            write_string(out, ds, "systemID", cp.system_id);
            const auto write_url = [&](std::string_view key, const std::optional<MsfUrlEntry>& entry) {
                if (!entry.has_value()) {
                    return;
                }
                ds.separate();
                out << '"' << json_escape(key) << "\":{\"url\":\"" << json_escape(entry->url) << '"';
                if (entry->type.has_value()) {
                    out << ",\"type\":\"" << json_escape(*entry->type) << '"';
                }
                out << '}';
            };
            write_url("laURL", cp.la_url);
            write_url("certURL", cp.cert_url);
            if (cp.pssh_base64.has_value()) {
                write_string(out, ds, "pssh", *cp.pssh_base64);
            }
            if (cp.robustness.has_value()) {
                write_string(out, ds, "robustness", *cp.robustness);
            }
            out << '}';
            out << '}';
        }
        out << ']';
    }
```

In `write_track`, emit the reference array after `custom_fields`:

```cpp
    if (!track.content_protection_ref_ids.empty()) {
        write_string_array(out, seq, "contentProtectionRefIDs", track.content_protection_ref_ids);
    }
```

- [ ] **Step 5: Validate**

Add to `validate_catalog`, after the existing `initDataList` id collection and before the track loop:

```cpp
    std::set<std::string> protection_ids;
    for (const auto& cp : catalog.content_protections) {
        if (!protection_ids.insert(cp.ref_id).second) {
            throw std::runtime_error("CMSF catalog has a duplicate contentProtections refID \"" +
                                     cp.ref_id + "\"");
        }
        // Section 4.1.1.3 defines exactly two schemes.
        if (cp.scheme != "cenc" && cp.scheme != "cbcs") {
            throw std::runtime_error("CMSF contentProtections scheme must be cenc or cbcs, got \"" +
                                     cp.scheme + "\"");
        }
        // Section 4.1.1.2 makes defaultKID required.
        if (cp.default_kids.empty()) {
            throw std::runtime_error("CMSF contentProtections entry \"" + cp.ref_id +
                                     "\" has no defaultKID");
        }
        for (const auto& kid : cp.default_kids) {
            if (!is_uuid_string(kid)) {
                throw std::runtime_error("CMSF defaultKID \"" + kid + "\" is not a UUID string");
            }
        }
        if (!is_uuid_string(cp.system_id)) {
            throw std::runtime_error("CMSF systemID \"" + cp.system_id + "\" is not a UUID string");
        }
    }
```

Inside the existing per-track loop in `validate_catalog`, add:

```cpp
        for (const auto& ref : track.content_protection_ref_ids) {
            if (protection_ids.count(ref) == 0) {
                throw std::runtime_error("CMSF contentProtectionRefID \"" + ref +
                                         "\" has no matching contentProtections entry (track \"" +
                                         track.name + "\")");
            }
        }
```

Add the UUID predicate to the anonymous namespace:

```cpp
// xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx, the form CMSF 4.1.1.2 requires.
bool is_uuid_string(std::string_view value) {
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        const bool is_dash_position = (i == 8 || i == 13 || i == 18 || i == 23);
        if (is_dash_position) {
            if (value[i] != '-') {
                return false;
            }
            continue;
        }
        const char c = value[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}
```

- [ ] **Step 6: Implement `attach_content_protection`**

Add to `src/msf_catalog.cpp`, beside `attach_init_data`:

```cpp
void attach_content_protection(MsfCatalog& catalog,
                               MsfTrack& track,
                               const CencTrackProtection& protection,
                               const std::vector<CencSystem>& systems) {
    for (const auto& system : systems) {
        // Reuse an existing entry for the same system and scheme so two tracks
        // sharing a KID share entries, per CMSF 4.1.1's no-duplication rule.
        auto existing = std::find_if(
            catalog.content_protections.begin(), catalog.content_protections.end(),
            [&](const MsfContentProtection& cp) {
                return cp.system_id == system.system_id && cp.scheme == protection.scheme;
            });

        if (existing == catalog.content_protections.end()) {
            MsfContentProtection cp;
            cp.ref_id = std::to_string(catalog.content_protections.size() + 1);
            cp.default_kids = {protection.default_kid};
            cp.scheme = protection.scheme;
            cp.system_id = system.system_id;
            if (!system.pssh_base64.empty()) {
                cp.pssh_base64 = system.pssh_base64;
            }
            catalog.content_protections.push_back(std::move(cp));
            existing = std::prev(catalog.content_protections.end());
        } else if (std::find(existing->default_kids.begin(), existing->default_kids.end(),
                             protection.default_kid) == existing->default_kids.end()) {
            existing->default_kids.push_back(protection.default_kid);
        }

        if (std::find(track.content_protection_ref_ids.begin(),
                      track.content_protection_ref_ids.end(),
                      existing->ref_id) == track.content_protection_ref_ids.end()) {
            track.content_protection_ref_ids.push_back(existing->ref_id);
        }
    }
}
```

Add `#include <algorithm>` if not already present.

- [ ] **Step 7: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS, 14/14.

- [ ] **Step 8: Commit**

```bash
git add include/openmoq/publisher/msf_catalog.h src/msf_catalog.cpp tests/msf_catalog_test.cpp
git commit -m "Add CMSF contentProtections signalling to the catalog"
```

---

### Task 5: Extract the JSON reader into a shared internal header

**Files:**
- Create: `src/json_reader.h`
- Modify: `src/live_srt_config.cpp`
- Modify: `CMakeLists.txt` only if the build needs the new header listed (it is header-only, so most likely not)

**Interfaces:**
- Consumes: nothing.
- Produces: `openmoq::publisher::internal::JsonValue`, `JsonObject`, `JsonArray`, and the parse entry point plus the accessor helpers currently in `src/live_srt_config.cpp`. Task 6 uses these.

`src/live_srt_config.cpp` contains a complete JSON reader — `JsonValue`, `JsonObject`, `JsonArray`, a parser, and accessors like `read_u32_or_default` — inside an anonymous namespace at `src/live_srt_config.cpp:14`. It is therefore file-local and unreachable from anywhere else.

Task 6 needs to parse a JSON config file. Duplicating a second JSON parser into `src/drm_config.cpp` would recreate exactly the divergence this project spent Phases 1 and 2 eliminating across four catalog emitters. Extract instead.

**This is a pure refactor with no behaviour change.** The existing `live_srt_config` tests must pass completely unchanged — if any assertion needs adjusting, the extraction has altered behaviour and is wrong.

- [ ] **Step 1: Confirm the baseline**

Run: `ctest --test-dir build -R openmoq-publisher-live-srt-config-tests --output-on-failure`
Expected: PASS. Record the output; this is the contract the refactor must preserve.

- [ ] **Step 2: Move the reader into a header**

Create `src/json_reader.h` (a private header beside the sources, not a public one under `include/`, because this is an internal utility rather than part of the library's API):

```cpp
#pragma once

// Minimal JSON reader, extracted from live_srt_config.cpp so more than one
// config parser can use it. Internal to the library: not part of the public
// include/ API.

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace openmoq::publisher::internal {

// ... move JsonValue, JsonObject, JsonArray, the parser, and the accessor
// helpers here VERBATIM from src/live_srt_config.cpp's anonymous namespace,
// changing only the enclosing namespace. Do not reformat, rename, or
// "improve" them during the move -- a behaviour-preserving move is
// reviewable; a move plus edits is not.

}  // namespace openmoq::publisher::internal
```

Move the code verbatim. Resist tidying it in the same commit: the whole value of this task is that the diff is provably behaviour-preserving.

- [ ] **Step 3: Point `live_srt_config.cpp` at the header**

Replace the moved block in `src/live_srt_config.cpp` with `#include "json_reader.h"` and a `using namespace openmoq::publisher::internal;` inside its existing anonymous namespace, or qualify the call sites — whichever produces the smaller diff.

- [ ] **Step 4: Run tests to verify nothing changed**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS, 14/14, with `openmoq-publisher-live-srt-config-tests` passing **without any test modification**. If a test needed changing, revert and investigate — the move was not behaviour-preserving.

- [ ] **Step 5: Commit**

```bash
git add src/json_reader.h src/live_srt_config.cpp
git commit -m "Extract the JSON reader from live_srt_config into a shared header"
```

---

### Task 6: DRM configuration and wiring into the batch path

**Files:**
- Create: `src/drm_config.cpp`, `include/openmoq/publisher/drm_config.h`
- Modify: `include/openmoq/publisher/publisher_api.h`, `include/openmoq/publisher/cli_options.h`, `src/cli_options.cpp`, `src/cmsf_packager.cpp` (`build_publish_plan`), `src/publisher_api.cpp`, `CMakeLists.txt`
- Modify: `tests/cli_options_test.cpp`, `tests/cmaf_segmenter_test.cpp`

**Interfaces:**
- Consumes: `attach_content_protection`, `parse_pssh_boxes`, `TrackDescription::protection`, and the JSON reader from Task 5.
- Produces: `struct DrmSystemConfig`, `PublisherConfig::drm_systems`, `std::vector<DrmSystemConfig> parse_drm_config_file(const std::string& path)`.

- [ ] **Step 1: Write the failing test**

In `tests/cli_options_test.cpp`, following the existing style there, add a case that `--drm-config=<path>` populates `PublisherConfig::drm_systems`, and that a missing file produces a clear parse error rather than a crash.

In `tests/cmaf_segmenter_test.cpp`, add a case building a publish plan from the encrypted fixture from Task 3 and asserting:

```cpp
    ok &= expect_contains(enc_catalog_text, "\"contentProtections\"",
                          "expected an encrypted input to emit contentProtections");
    ok &= expect_contains(enc_catalog_text, "\"contentProtectionRefIDs\"",
                          "expected the protected track to reference an entry");
    ok &= expect_contains(enc_catalog_text, "\"codec\":\"avc1",
                          "expected the frma-resolved codec, not encv");
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: FAIL — `drm_systems` is not a member, and the plan emits no contentProtections.

- [ ] **Step 3: Add the config struct**

In `include/openmoq/publisher/publisher_api.h`, add before `PublisherConfig`:

```cpp
// Deployment configuration for one DRM system, matched to a system found in
// the media by system_id. These fields are not present in the MP4 -- a
// licence URL is deployment data, not media data.
struct DrmSystemConfig {
    std::string system_id;                      // UUID string, the match key
    std::optional<std::string> la_url;
    std::optional<std::string> la_url_type;
    std::optional<std::string> cert_url;
    std::optional<std::string> cert_url_type;
    std::optional<std::string> robustness;
};
```

and to `PublisherConfig`, after `vod`:

```cpp
    // Matched to systems found in the media by system_id. A configured system
    // the media carries no pssh for is IGNORED, not emitted -- a
    // contentProtections entry for an absent system would describe protection
    // that does not exist.
    std::vector<DrmSystemConfig> drm_systems;
```

- [ ] **Step 4: Parse the config file**

Create `include/openmoq/publisher/drm_config.h` declaring:

```cpp
#pragma once

#include <string>
#include <vector>

#include "openmoq/publisher/publisher_api.h"

namespace openmoq::publisher {

// Parse a DRM configuration JSON file. Throws std::runtime_error naming the
// path and the problem when the file is missing or malformed.
//
// Expected shape:
// { "systems": [ { "systemID": "...", "laURL": "...", "laURLType": "...",
//                  "certURL": "...", "certURLType": "...",
//                  "robustness": "..." } ] }
std::vector<DrmSystemConfig> parse_drm_config_file(const std::string& path);

}  // namespace openmoq::publisher
```

Implement it in `src/drm_config.cpp` using the JSON reader Task 5 extracted to `src/json_reader.h`. Do not write a second parser.

```cpp
#include "openmoq/publisher/drm_config.h"

#include "json_reader.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace openmoq::publisher {

namespace {

std::optional<std::string> read_optional_string(const internal::JsonObject& object,
                                                std::string_view key) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        return std::nullopt;
    }
    if (const auto* text = std::get_if<std::string>(&it->second.value)) {
        return *text;
    }
    return std::nullopt;
}

}  // namespace

std::vector<DrmSystemConfig> parse_drm_config_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("drm config: cannot open " + path);
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();

    const auto root = internal::parse_json_object(buffer.str());
    if (!root.has_value()) {
        throw std::runtime_error("drm config: " + path + " is not a JSON object");
    }

    const auto systems_it = root->find("systems");
    if (systems_it == root->end()) {
        throw std::runtime_error("drm config: " + path + " has no \"systems\" array");
    }
    const auto* systems = std::get_if<std::unique_ptr<internal::JsonArray>>(&systems_it->second.value);
    if (systems == nullptr || *systems == nullptr) {
        throw std::runtime_error("drm config: \"systems\" in " + path + " is not an array");
    }

    std::vector<DrmSystemConfig> out;
    for (const auto& entry : **systems) {
        const auto* object = std::get_if<std::unique_ptr<internal::JsonObject>>(&entry.value);
        if (object == nullptr || *object == nullptr) {
            throw std::runtime_error("drm config: a \"systems\" entry in " + path + " is not an object");
        }
        DrmSystemConfig system;
        const auto id = read_optional_string(**object, "systemID");
        if (!id.has_value() || id->empty()) {
            throw std::runtime_error("drm config: a \"systems\" entry in " + path +
                                     " has no systemID");
        }
        system.system_id = *id;
        system.la_url = read_optional_string(**object, "laURL");
        system.la_url_type = read_optional_string(**object, "laURLType");
        system.cert_url = read_optional_string(**object, "certURL");
        system.cert_url_type = read_optional_string(**object, "certURLType");
        system.robustness = read_optional_string(**object, "robustness");
        out.push_back(std::move(system));
    }
    return out;
}

}  // namespace openmoq::publisher
```

Adapt the accessor calls to whatever the extracted reader actually exposes — Task 5 moved the code verbatim, so the exact names of the parse entry point and the variant alternatives come from `src/live_srt_config.cpp`. Read `src/json_reader.h` before writing this and match its real API rather than the illustrative names above. If the extracted parser's entry point is named differently, use the real name and say so in your report.

- [ ] **Step 5: Add the CLI flag**

In `include/openmoq/publisher/cli_options.h` and `src/cli_options.cpp`, add `--drm-config=<path>`, following the existing option-parsing pattern exactly. On parse failure, report the error and exit non-zero rather than publishing with partial configuration.

- [ ] **Step 6: Wire into `build_publish_plan`**

In `src/cmsf_packager.cpp`, `build_publish_plan` gains a `const std::vector<DrmSystemConfig>& drm_systems` parameter, defaulted to an empty vector so existing call sites compile unchanged. For each track with `protection`, call `attach_content_protection` with the systems parsed from the input's `pssh` boxes, then apply any matching `DrmSystemConfig` fields onto the resulting entries.

Thread `config_.drm_systems` from `prepare_file` and `prepare_stream` in `src/publisher_api.cpp`, matching how `config_.vod` is threaded.

- [ ] **Step 7: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS, 14/14.

- [ ] **Step 8: Commit**

```bash
git add include/openmoq/publisher/drm_config.h src/drm_config.cpp include/openmoq/publisher/publisher_api.h include/openmoq/publisher/cli_options.h src/cli_options.cpp src/cmsf_packager.cpp src/publisher_api.cpp tests/ CMakeLists.txt
git commit -m "Add DRM system configuration and wire protection into publish plans"
```

---

### Task 7: `saio` correction on the CTE ingest path

**Files:**
- Modify: `src/cmaf_segmenter.cpp` (`rebuild_moof`, around `:303-330`)
- Modify: `tests/cmaf_segmenter_test.cpp`

**Interfaces:**
- Consumes: `find_child_box_span` from Task 1.
- Produces: nothing consumed by later tasks.

**Order dependency, stated deliberately:** this task must not begin until Tasks 1 and 2 have landed. The spec records that specifying `saio` work against box-parsing code that does not yet exist is the pattern which produced defects in the Phase 2 plan. The controller writes this task's detailed brief from the spec's Part 4 **after** Task 2 merges, using the parser as it actually exists rather than as anticipated.

What the brief must cover, from the spec's Part 4:

- `saio` is a FullBox. After the 8-byte header and 4-byte version/flags: if `flags & 1`, an `aux_info_type` (4) and `aux_info_type_parameter` (4); then `entry_count` (4); then `entry_count` offsets, 32-bit for version 0 and 64-bit for version 1. A version other than 0 or 1 is refused.
- When `rebuild_moof` changes the moof's size by delta *D*, every moof-relative offset shifts by exactly *D*. The existing `trun` `data_offset` recomputation at `:329-330` already does this for its own field.
- Classification, against the ORIGINAL moof size and the following mdat size. An offset below the original moof size points inside the moof at `senc` in the `traf` — correct by adding *D*. An offset within `original_moof_size + mdat_size` is moof-relative into the mdat — correct by adding *D*. Anything larger is an absolute file offset with no meaning in a republished MOQT object — **refuse the fragment with an explicit error naming the offset**, never guess.
- A `saio` present with no `senc` and no `saiz` is malformed and refused, since the auxiliary data it points at cannot be located to validate the classification.

Tests the brief must require:

- Offsets corrected **numerically** against a known delta *D*, for version 0 and version 1, with and without the `aux_info_type` prefix. Asserting merely that offsets changed would pass against an implementation adding the wrong delta.
- A moof-internal offset and an mdat-relative offset are both corrected.
- An absolute-looking offset is refused, with a fixture that is otherwise valid so the refusal is attributable to the classification.
- An unencrypted fragment through the same path is byte-for-byte unchanged, proving the correction is inert when no `saio` is present.

---

### Task 8: Refusals and documentation

**Files:**
- Modify: `src/cmaf_segmenter.cpp` (progressive remux path)
- Modify: `docs/status.md`, `docs/protocol-mapping.md`
- Modify: `tests/cmaf_segmenter_test.cpp`

**Interfaces:**
- Consumes: `TrackDescription::protection`.
- Produces: nothing. Final task.

- [ ] **Step 1: Write the failing test**

Add a case asserting that segmenting a progressive MP4 whose sample entry is `encv` throws, with a message naming the remux path:

```cpp
    bool refused_encrypted_remux = false;
    try {
        (void)segment_for_cmaf(parsed_encrypted_progressive);
    } catch (const std::runtime_error&) {
        refused_encrypted_remux = true;
    }
    ok &= expect(refused_encrypted_remux,
                 "expected encrypted progressive input to be refused rather than remuxed");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target openmoq-publisher-packaging-tests && ctest --test-dir build -R openmoq-publisher-packaging-tests --output-on-failure`
Expected: FAIL — the remux path silently produces synthesized moofs.

- [ ] **Step 3: Refuse encrypted input on the remux path**

In `src/cmaf_segmenter.cpp`, in the progressive-remux path, throw when any track carries `protection`:

```cpp
        if (track.protection.has_value()) {
            // Synthesized moofs cannot carry senc, saiz, or saio, so the
            // output would be undecryptable while appearing valid. Refuse
            // rather than publish media that cannot be played.
            throw std::runtime_error(
                "encrypted input requires fragmented MP4: the progressive remux path cannot "
                "carry CENC auxiliary boxes (track " + track.track_name + ")");
        }
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS, 14/14.

- [ ] **Step 5: Update the documentation**

In `docs/status.md`, replace the roadmap item covering content protection to record that Phase 3 shipped: CENC detection from `sinf`/`schm`/`schi`/`tenc`, `frma` codec resolution, `pssh` extraction, root `contentProtections` with per-track references, DRM configuration via `--drm-config`, and `saio` correction on the CTE ingest path. State plainly that the publisher never decrypts and never encrypts, and that encrypted input on the progressive-remux path is refused.

In `docs/protocol-mapping.md`, add a "CMSF content protection" section recording: protection data lives at the catalog root and is referenced by `refID`, never duplicated per track; the codec string is the `frma` original format, not the `encv`/`enca` wrapper; and `saio` offsets are corrected by the moof-size delta when the CTE path rebuilds a moof, with absolute-looking offsets refused rather than guessed.

Do not claim these are implemented, because they are not: MoQ Secure Objects encryption fields (MSF 5.2.38 to 5.2.41), MSF section 12 compression signalling, MSF URL parsing, or `clone` delta operations.

- [ ] **Step 6: Run the full suite**

Run:
```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF -DOPENMOQ_LIBMOQ_SOURCE_DIR=/media/mondain/terrorbyte/workspace/github-moq/moq5
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: PASS, 14/14.

- [ ] **Step 7: Commit**

```bash
git add src/cmaf_segmenter.cpp docs/status.md docs/protocol-mapping.md tests/cmaf_segmenter_test.cpp
git commit -m "Refuse encrypted progressive input and document CMSF content protection"
```

---

## Out of Scope

- Decryption and encryption. The publisher signals protection present in its input and never holds keys.
- MSF sections 5.2.38 to 5.2.41, MoQ Secure Objects. That is the LOC-packaged end-to-end encryption path; CMSF uses CENC.
- MSF section 12 compression signalling, blocked on transport draft-19 Track and Object Properties.
- Phase 4, MSF URL and fragment parsing.
- `clone` delta operations, deliberately unimplemented in Phase 2.
