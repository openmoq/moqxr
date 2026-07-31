# Live-Path Content Protection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Detect and signal CMSF content protection on the live publish paths that receive a real CMAF initialization segment, so an encrypted live broadcast publishes `contentProtections` instead of a catalog indistinguishable from clear content.

**Architecture:** No new parsing. Phase 3 already built `parse_track_protection`, `parse_pssh_boxes`, `collect_pssh_systems`, and `attach_content_protection`. This phase connects them at two existing catalog builders — `build_live_catalog` and the DASH CTE `build_catalog_locked` — mirroring the batch path at `src/cmsf_packager.cpp:512-592` exactly.

**Tech Stack:** C++20, CMake, no new dependencies. Tests are single-binary `expect()`-style executables under `tests/`.

## Global Constraints

- C++20, no new dependencies, namespace `openmoq::publisher`.
- Build with BOTH flags: `-DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF -DOPENMOQ_LIBMOQ_SOURCE_DIR=/media/mondain/terrorbyte/workspace/github-moq/moq5`. Without the libmoq flag the suite silently drops from 15 targets to 14 with no error message.
- Baseline at branch start: **15/15 tests, 12 unique compiler warnings** (`grep -E "warning:" | sort -u | wc -l`). No `-Werror`.
- **The publisher never encrypts and never decrypts.** This phase detects and signals protection already present in its input.
- `--input` is required only when `--live-source` is at its `auto` default; `src/cli_options.cpp` exempts `--live-source srt` and `--live-source dash`. The existing tests reflect this: the dash case at `tests/cli_options_test.cpp:257` and the srt case at `:236` both succeed with no `--input`. Match the surrounding tests rather than adding one. Where `--live-source` is left at `auto` and the parse is expected to succeed, `"--input", "sample.mp4"` is required, because these tests call the parser directly and an unexpected throw aborts the binary before any assertion runs.
- No emoji, no "Generated with Claude Code" tagline, no `Co-Authored-By` line.
- Prefer `#include` and unqualified names over fully-qualified spellings.

## File Structure

| File | Responsibility | Task |
| --- | --- | --- |
| `include/openmoq/publisher/cmsf_packager.h` | Declare `collect_pssh_systems`; extend `build_live_catalog` signature | 1, 2 |
| `src/cmsf_packager.cpp` | Un-anonymise `collect_pssh_systems`; attach protection in `build_live_catalog` | 1, 2 |
| `include/openmoq/publisher/live_dash_ingest.h` | Add `pssh_systems` to `RegisteredTrack` | 3 |
| `src/live_dash_ingest.cpp` | Parse systems at registration; attach in `build_catalog_locked` | 3 |
| `src/cli_options.cpp` | Accept `--drm-config` for dash/stdin; correct the SRT message | 4 |
| `tests/cmaf_segmenter_test.cpp` | `build_live_catalog` protection tests | 2 |
| `tests/live_dash_ingest_test.cpp` | DASH catalog protection test | 3 |
| `tests/cli_options_test.cpp` | CLI acceptance and refusal tests | 4 |
| `docs/status.md`, `docs/protocol-mapping.md` | Coverage and remaining limitations | 5 |

---

### Task 1: Expose `collect_pssh_systems`

Promotes an existing helper out of an anonymous namespace so the DASH ingest can reuse it. No behaviour change.

**Files:**
- Modify: `include/openmoq/publisher/cmsf_packager.h`
- Modify: `src/cmsf_packager.cpp:367`

**Interfaces:**
- Produces: `std::vector<CencSystem> collect_pssh_systems(std::span<const std::uint8_t> init_bytes);`

- [ ] **Step 1: Declare it in the header**

`collect_pssh_systems` currently lives in the anonymous namespace that opens at `src/cmsf_packager.cpp:19`, defined at line 367. Add to `include/openmoq/publisher/cmsf_packager.h`, near the existing `build_live_catalog` declaration:

```cpp
// Collects every DRM system described by the pssh boxes directly under moov.
// pssh boxes are siblings of trak, not children of one, so protection applies
// to the initialization segment as a whole and is shared by every track that
// references it (CMSF 4.1.1). Returns empty when there is no moov or no pssh.
std::vector<CencSystem> collect_pssh_systems(std::span<const std::uint8_t> init_bytes);
```

`CencSystem` comes from `openmoq/publisher/mp4_box.h`. Confirm that header is already included by `cmsf_packager.h`; if not, include it.

- [ ] **Step 2: Move the definition out of the anonymous namespace**

In `src/cmsf_packager.cpp`, move the `collect_pssh_systems` definition (line 367 and its body, including the explanatory comment above it at lines 365-366) below the anonymous namespace's closing brace, into `namespace openmoq::publisher`. Do not change its body.

- [ ] **Step 3: Build and run the suite**

```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF -DOPENMOQ_LIBMOQ_SOURCE_DIR=/media/mondain/terrorbyte/workspace/github-moq/moq5
cmake --build build -j"$(nproc)"
ctest --test-dir build
```

Expected: 15/15, 12 unique warnings. This task changes no behaviour, so any test failure means the move broke a call site.

- [ ] **Step 4: Commit**

```bash
git add include/openmoq/publisher/cmsf_packager.h src/cmsf_packager.cpp
git commit -m "Expose collect_pssh_systems for reuse by the live ingest paths"
```

---

### Task 2: Attach protection in `build_live_catalog`

Covers `src/transport/libmoq_publisher.cpp:946` and `:1181`, and `src/transport/moqt_session.cpp:3178` and `:3655`.

**Files:**
- Modify: `include/openmoq/publisher/cmsf_packager.h` (the `build_live_catalog` declaration)
- Modify: `src/cmsf_packager.cpp:727-770`
- Test: `tests/cmaf_segmenter_test.cpp`

**Interfaces:**
- Consumes: `collect_pssh_systems` from Task 1.
- Produces: `build_live_catalog(tracks, init_segment, is_live, drm_systems = {})`.

- [ ] **Step 1: Write the failing tests**

**Do not build new encrypted fixtures.** `tests/cmaf_segmenter_test.cpp` already has everything this task needs, added in Phase 3:

- `make_encrypted_fragmented_test_mp4(bool include_pssh = true)` at line 360 — a complete `ftyp`+`moov`+`moof`+`mdat` file with an `encv` sample entry wrapping `avc1` via `frma`, a `cenc` `schm`, a `schi`/`tenc`, and — when `include_pssh` is true — a `pssh` sibling of `trak` under `moov` carrying the Widevine system ID. `include_pssh = false` is the exact no-`pssh` case this task's refusal test needs, and its comment already records that it models ffmpeg's `-encryption_scheme cenc-aes-ctr`.
- `make_sinf`, `make_schm`, `make_tenc_box`, `make_frma`, `make_pssh`, `widevine_system_id()` — the building blocks, whose byte layouts match what `parse_track_protection` and `parse_pssh_boxes` actually read.
- `expect`, `expect_contains`, `expect_not_contains`, `append_be32`, `concat`, `make_box`, `make_full_box`.
- The existing `build_live_catalog` call at line 1501 uses `(tracks, init_bytes, true)`.

Hand-rolling a `tenc` or `pssh` payload risks a layout the parser rejects, which would make a test fail for the wrong reason or pass while proving nothing. Use the helpers.

Append inside `main()` before `return ok ? 0 : 1;`:

```cpp
    // Phase 5: an encrypted live init segment must produce contentProtections,
    // exactly as the batch path does. CMSF 4.1.2 makes an absent
    // contentProtectionRefIDs mean the track is NOT protected, so a silent
    // omission here is an affirmative false claim about encrypted media.
    {
        const auto encrypted_bytes = make_encrypted_fragmented_test_mp4(true);
        const auto tracks = extract_tracks(parse_mp4_boxes(encrypted_bytes), encrypted_bytes);
        ok &= expect(tracks.size() == 1 && tracks.front().protection.has_value(),
                     "expected the encrypted live fixture to report CENC protection");

        const auto live_catalog = build_live_catalog(tracks, encrypted_bytes, true);
        const std::string text(live_catalog.catalog_payload.begin(), live_catalog.catalog_payload.end());
        ok &= expect_contains(text, "\"contentProtections\"",
                              "expected a live catalog for encrypted input to carry contentProtections");
        ok &= expect_contains(text, "\"contentProtectionRefIDs\"",
                              "expected the protected live track to reference a contentProtections entry");
        ok &= expect(live_catalog.msf_catalog.content_protections.size() == 1,
                     "expected exactly one contentProtections entry for a single-system fixture");
        ok &= expect(live_catalog.msf_catalog.tracks.front().content_protection_ref_ids.size() == 1,
                     "expected the track to reference exactly one entry");
    }

    // A protected track whose init segment carries no pssh cannot be signalled,
    // so it is refused rather than published as clear.
    {
        const auto no_pssh_bytes = make_encrypted_fragmented_test_mp4(false);
        const auto tracks = extract_tracks(parse_mp4_boxes(no_pssh_bytes), no_pssh_bytes);
        ok &= expect(tracks.size() == 1 && tracks.front().protection.has_value(),
                     "expected the no-pssh fixture to still report CENC protection");

        bool refused = false;
        std::string message;
        try {
            (void)build_live_catalog(tracks, no_pssh_bytes, true);
        } catch (const std::runtime_error& error) {
            refused = true;
            message = error.what();
        }
        ok &= expect(refused, "expected a protected live track with no pssh to be refused");
        ok &= expect(message.find("pssh") != std::string::npos,
                     "expected the refusal to name the missing pssh");
        ok &= expect(message.find(tracks.front().track_name) != std::string::npos,
                     "expected the refusal to name the offending track");
    }

    // Regression guard for every existing live user: unencrypted input must
    // produce no contentProtections at all.
    {
        const auto live_catalog = build_live_catalog(multitrack_segmented.tracks, multitrack_init_bytes, true);
        const std::string text(live_catalog.catalog_payload.begin(), live_catalog.catalog_payload.end());
        ok &= expect_not_contains(text, "\"contentProtections\"",
                                  "expected unencrypted live input to carry no contentProtections");
        ok &= expect(live_catalog.msf_catalog.content_protections.empty(),
                     "expected no contentProtections entries for unencrypted live input");
    }

    // --drm-config deployment fields must reach the live catalog.
    {
        const auto encrypted_bytes = make_encrypted_fragmented_test_mp4(true);
        const auto tracks = extract_tracks(parse_mp4_boxes(encrypted_bytes), encrypted_bytes);
        DrmSystemConfig config;
        config.system_id = "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed";
        config.la_url = "https://drm.example/lic";
        config.robustness = "SW_SECURE_DECODE";

        const auto live_catalog = build_live_catalog(tracks, encrypted_bytes, true, {config});
        const std::string text(live_catalog.catalog_payload.begin(), live_catalog.catalog_payload.end());
        ok &= expect_contains(text, "https://drm.example/lic",
                              "expected the configured laURL to reach the live catalog");
        ok &= expect_contains(text, "SW_SECURE_DECODE",
                              "expected the configured robustness to reach the live catalog");
    }
```

Add `#include "openmoq/publisher/publisher_api.h"` to the test file if `DrmSystemConfig` is not already visible.

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build build -j"$(nproc)" && ctest --test-dir build --output-on-failure -R packaging
```

Expected: FAIL on "expected a live catalog for encrypted input to carry contentProtections", and a compile error on the four-argument `build_live_catalog` call.

- [ ] **Step 3: Extend the signature**

In `include/openmoq/publisher/cmsf_packager.h`, change the `build_live_catalog` declaration to add a trailing defaulted parameter, matching the convention the batch entry point already uses at line 67:

```cpp
LiveCatalog build_live_catalog(const std::vector<TrackDescription>& tracks,
                               std::span<const std::uint8_t> init_segment,
                               bool is_live,
                               const std::vector<DrmSystemConfig>& drm_systems = {});
```

The default keeps all four existing call sites compiling unchanged.

- [ ] **Step 4: Attach protection in the body**

In `src/cmsf_packager.cpp`, update the definition to match the new signature. Then, before the per-track loop that begins `for (const auto& track : tracks)` at roughly line 756, add:

```cpp
    const bool any_track_protected = std::any_of(
        tracks.begin(), tracks.end(),
        [](const TrackDescription& track) { return track.protection.has_value(); });
    const std::vector<CencSystem> pssh_systems =
        any_track_protected ? collect_pssh_systems(init_segment) : std::vector<CencSystem>{};
    std::vector<std::string> protected_schemes;
```

Note `init_segment` here is the **full** init segment. Do not pass the per-track segment `build_track_specific_init_segment` produces — that is a synthesised single-track `moov` with no `pssh` siblings, so it would yield no systems and turn every protected track into a refusal.

Inside the loop, after the existing `attach_init_data` call:

```cpp
        if (track.protection.has_value()) {
            // CMSF 4.1.1.4.5 makes pssh only SHOULD-present, so a protected
            // track with no pssh anywhere in the init segment must not fall
            // through silently: CMSF 4.1.2 defines an absent
            // contentProtectionRefIDs as meaning the track is NOT protected,
            // so publishing would affirmatively misdescribe encrypted media.
            if (pssh_systems.empty()) {
                throw std::runtime_error(
                    "track '" + track.track_name +
                    "' is protected (CENC) but no pssh system was found in the "
                    "initialization segment; refusing to publish a catalog with no "
                    "contentProtections entry for encrypted content");
            }
            attach_content_protection(msf_catalog, msf_track, *track.protection, pssh_systems);
            if (std::find(protected_schemes.begin(), protected_schemes.end(), track.protection->scheme) ==
                protected_schemes.end()) {
                protected_schemes.push_back(track.protection->scheme);
            }
        }
```

After the loop closes and before `serialize_catalog`:

```cpp
    for (const auto& scheme : protected_schemes) {
        apply_drm_system_configs(msf_catalog, scheme, pssh_systems, drm_systems);
    }
```

Add `<algorithm>` to the includes if `std::any_of` and `std::find` are not already available.

- [ ] **Step 5: Run the tests to verify they pass**

Expected: 15/15.

- [ ] **Step 6: Prove the refusal and the attachment are real**

Two mutations, run one at a time and restored between:

1. Change the `pssh_systems.empty()` guard to `if (false)`. Expected: the "expected a protected live track with no pssh to be refused" assertion fails. The other three blocks still pass.
2. Remove the `attach_content_protection` call. Expected: the contentProtections assertions fail while the unencrypted regression guard still passes.

Report the exact assertion messages you observed for each.

- [ ] **Step 7: Commit**

```bash
git add include/openmoq/publisher/cmsf_packager.h src/cmsf_packager.cpp tests/cmaf_segmenter_test.cpp
git commit -m "Attach content protection when building live catalogs"
```

---

### Task 3: Attach protection on the DASH CTE path

The DASH ingest builds its catalog independently and does not route through `build_live_catalog`.

**Files:**
- Modify: `include/openmoq/publisher/live_dash_ingest.h:87-90`
- Modify: `src/live_dash_ingest.cpp:386-400`, `:506-527`
- Test: `tests/live_dash_ingest_test.cpp`

**Interfaces:**
- Consumes: `collect_pssh_systems` from Task 1; `attach_content_protection` from `msf_catalog.h`.
- Produces: `RegisteredTrack::pssh_systems`.

- [ ] **Step 1: Write the failing test**

`tests/live_dash_ingest_test.cpp` already builds init segments — it has a 78-byte `visual_header` fixture. Read the existing helpers and reuse them. Add a test that feeds the session an encrypted init segment and asserts the resulting catalog carries `contentProtections`.

The assertion must check the entry exists, not merely that the catalog serialises. This is the test that fails if the per-track base64 is used as the `pssh` source instead of the full init segment:

This file drives a session by constructing `LiveDashIngestSession`, calling
`ingest(path, span)` with an init segment, waiting via `wait_for_tracks`, then
pulling objects with `try_next_object()`. The first object is the catalog. The
idiom is at `tests/live_dash_ingest_test.cpp:295-307`; follow it exactly.

```cpp
    // Phase 5: pssh boxes are siblings of trak under moov, so they live in the
    // full init segment. The per-track init segment the catalog embeds is a
    // synthesised single-track moov with no pssh at all -- reading protection
    // from it would silently find nothing and refuse every protected track.
    {
        LiveDashIngestSession session(8);
        const auto encrypted_init = make_encrypted_dash_init_with_pssh();
        session.ingest("/ingest/enc",
                       std::span<const std::uint8_t>(encrypted_init.data(), encrypted_init.size()));
        ok &= expect(session.wait_for_tracks(std::chrono::milliseconds(1), std::chrono::milliseconds(1)),
                     "expected a track from the encrypted DASH init segment");

        const std::optional<LiveObject> catalog = session.try_next_object();
        ok &= expect(catalog.has_value() && catalog->track_name == "catalog",
                     "expected a catalog object from the encrypted DASH init");
        if (catalog.has_value()) {
            const std::string catalog_text(catalog->payload.begin(), catalog->payload.end());
            ok &= expect(catalog_text.find("\"contentProtections\"") != std::string::npos,
                         "expected the DASH catalog to carry contentProtections for an encrypted init");
            ok &= expect(catalog_text.find("\"contentProtectionRefIDs\"") != std::string::npos,
                         "expected the protected DASH track to reference a contentProtections entry");
        }
    }
```

Build `make_encrypted_dash_init_with_pssh` by porting the CENC fixture helpers from `tests/cmaf_segmenter_test.cpp` — `make_frma`, `make_schm`, `make_tenc_box`, `make_sinf`, `make_pssh`, and `widevine_system_id()`, defined there around lines 320-356. Their byte layouts match what `parse_track_protection` and `parse_pssh_boxes` actually read; do **not** hand-roll replacements, since a wrong `tenc` or `pssh` layout makes a test fail for the wrong reason or pass while proving nothing.

Each test binary owns its fixtures in this codebase, so copy the helpers you need rather than trying to share them across binaries.

The fixture must be an **init segment** — `ftyp` + `moov`, with the `pssh` a sibling of `trak` under `moov` — not a full fragmented file, because the DASH ingest treats `ftyp`/`moov` as the init and expects media in separate `moof`/`mdat` boxes. This file's existing `make_init_segment(...)` shows the expected shape; read it and add the `encv` sample entry with `sinf`, plus the `pssh`, to that shape.

- [ ] **Step 2: Run the test to verify it fails**

Expected: FAIL on "expected the DASH catalog to carry contentProtections for an encrypted init".

- [ ] **Step 3: Store the parsed systems at registration**

In `include/openmoq/publisher/live_dash_ingest.h`, extend `RegisteredTrack`:

```cpp
    struct RegisteredTrack {
        TrackDescription description;
        std::string init_data_base64;
        // Parsed once per path at registration, while the full init segment is
        // still in hand. The per-track init_data_base64 above is a synthesised
        // single-track moov and carries no pssh siblings, so it cannot serve as
        // the source for these.
        std::vector<CencSystem> pssh_systems;
    };
```

`CencSystem` is defined in `openmoq/publisher/mp4_box.h` at line 50. `live_dash_ingest.h` does not include it directly, but reaches it transitively through `cmaf_segmenter.h` (line 16), which includes it at its own line 6 — that is also how the existing `TrackDescription description` field resolves. Add a direct `#include "openmoq/publisher/mp4_box.h"` anyway, so the header states its own dependency rather than relying on a transitive one.

In `src/live_dash_ingest.cpp`, inside `process_box_locked`, after `path_state.tracks = extract_tracks(...)` at line 386 and before the per-track loop, parse once per path:

```cpp
            const bool path_has_protection =
                std::any_of(path_state.tracks.begin(), path_state.tracks.end(),
                            [](const TrackDescription& track) { return track.protection.has_value(); });
            const std::vector<CencSystem> path_pssh_systems =
                path_has_protection ? collect_pssh_systems(path_state.init_bytes)
                                    : std::vector<CencSystem>{};
```

Then in the existing `tracks_.push_back(RegisteredTrack{...})` call, add the field:

```cpp
                tracks_.push_back(RegisteredTrack{.description = track,
                                                  .init_data_base64 = std::move(init_data),
                                                  .pssh_systems = path_pssh_systems});
```

No new includes are needed in `src/live_dash_ingest.cpp`: it already includes `cmsf_packager.h` (line 3, which will declare `collect_pssh_systems` after Task 1), `mp4_box.h` (line 4), `msf_catalog.h` (line 5, for `attach_content_protection`), and `<algorithm>` (line 7, for `std::any_of`).

- [ ] **Step 4: Attach in `build_catalog_locked`**

In `src/live_dash_ingest.cpp`, in the loop at line 517 that already calls `make_msf_track` and `attach_init_data`, add after the `attach_init_data` block:

```cpp
        if (track.protection.has_value()) {
            if (registered.pssh_systems.empty()) {
                throw std::runtime_error(
                    "track '" + track.track_name +
                    "' is protected (CENC) but no pssh system was found in the "
                    "initialization segment; refusing to publish a catalog with no "
                    "contentProtections entry for encrypted content");
            }
            attach_content_protection(msf_catalog, msf_track, *track.protection, registered.pssh_systems);
        }
```

Note `track` here is the local `TrackDescription` copy the loop already makes (`TrackDescription track = registered.description;` with `packaging` coerced to `"cmaf"`), and `registered` is the loop variable over `tracks_`. Use `registered.pssh_systems`, not a re-parse.

This path does not apply `--drm-config` deployment fields; the DASH session has no access to the publisher's `DrmSystemConfig` list. Task 5 records that as a documented limitation.

- [ ] **Step 5: Run the tests to verify they pass**

Expected: 15/15.

- [ ] **Step 6: Prove the attachment and the refusal are load-bearing**

Two mutations, run one at a time and restored between. Report the exact assertion messages you observed for each.

1. Remove the `attach_content_protection` call in `build_catalog_locked`. Expected: the DASH `contentProtections` assertions fail. Every other DASH test still passes.
2. Force `registered.pssh_systems` to be empty at the point of use. Expected: the protected track is refused with the message naming it, proving the refusal path fires rather than being unreachable.

**An earlier version of this step asked for a different mutation — parsing from the per-track init segment, expecting it to fail — on the theory that a synthesised single-track `moov` carries no `pssh`. That theory is false** and was disproved empirically: `build_track_specific_init_segment` (`src/cmsf_packager.cpp:230-268`) copies every `moov` child that is not `trak` or `mvex` verbatim, `pssh` included.

Do not "fix" that by stripping `pssh` from per-track init segments. A subscriber initialising a decoder from a track's `initData` needs the `pssh` to set up DRM; removing it would break the consumer the field exists for. Parsing at registration is still the right design — once per path rather than once per catalog build, no base64 round-trip, and no dependence on incidental copy behaviour — but it is a design preference, not a correctness requirement.

- [ ] **Step 7: Commit**

```bash
git add include/openmoq/publisher/live_dash_ingest.h src/live_dash_ingest.cpp tests/live_dash_ingest_test.cpp
git commit -m "Attach content protection on the DASH CTE ingest path"
```

---

### Task 4: Accept `--drm-config` where detection now works

**Files:**
- Modify: `src/cli_options.cpp:462-495`
- Test: `tests/cli_options_test.cpp`

**Interfaces:**
- Consumes: nothing new.

- [ ] **Step 1: Replace the two existing refusal tests, then add the new ones**

**Three tests already assert the refusals this task removes.** They will fail once the guards are gone, and they must be rewritten rather than deleted — the behaviour they cover still needs coverage, just inverted:

- `tests/cli_options_test.cpp:437-458` asserts `--drm-config` with `--live-source dash` is refused and that the message names `--live-source dash`. **Invert it**: the parse must now succeed and `options.drm_systems` must be non-empty. Reuse its existing flag set verbatim (`--live-source dash`, `--dash-listen 127.0.0.1:8080`, `--endpoint https://relay.example.com:443/moq`) so the new test exercises the same command line the old one did.
- `tests/cli_options_test.cpp:460-482` asserts the same for the live stdin path, using `{"openmoq-publisher", "--input", "-", "--endpoint", "localhost:4443", "--drm-config", ...}`. **Invert it** the same way.
- `tests/cli_options_test.cpp:415-435` asserts the SRT refusal. **Keep it**, but update the message assertion: it currently checks for `--live-source srt`, which the new message still contains, so verify whether it passes unchanged and additionally assert the message mentions `MPEG-TS`.

Each of those blocks ends by removing its temp config file with `std::filesystem::remove(config_path, ec)`. Preserve that cleanup in whatever you write.

`tests/cli_options_test.cpp` has `parse(std::vector<std::string>)` at line 24, `parse_throws(args, fragment, message)` from Phase 4, and `write_drm_config_file(name)` which writes a valid single-system config and returns its path.

```cpp
    // Phase 5: detection now works on the dash and stdin live paths, so
    // --drm-config is accepted there.
    {
        const auto config_path = write_drm_config_file("phase5-dash.json");
        const auto options = parse({"openmoq-publisher", "--live-source", "dash",
                                    "--dash-listen", "127.0.0.1:8080",
                                    "--dash-path", "/ingest",
                                    "--endpoint", "https://relay.example.com:443/moq",
                                    "--drm-config", config_path.string()});
        ok &= expect(!options.drm_systems.empty(),
                     "expected --drm-config to be accepted with --live-source dash");
    }

    // SRT still cannot carry CMAF CENC metadata, and the refusal must say why.
    {
        const auto config_path = write_drm_config_file("phase5-srt.json");
        ok &= parse_throws({"openmoq-publisher", "--live-source", "srt",
                            "--srt-config", "/tmp/foo.json",
                            "--endpoint", "localhost:4443", "--namespace", "ns",
                            "--drm-config", config_path.string()},
                           "MPEG-TS",
                           "expected the SRT refusal to explain that MPEG-TS carries no CENC metadata");
    }
```

The flag forms above are copied from the existing tests at `tests/cli_options_test.cpp:236` (srt) and `:257` (dash). `--live-source dash` requires `--dash-listen`, or the parse throws `--live-source dash requires --dash-listen`.

- [ ] **Step 2: Run the tests to verify they fail**

Expected: FAIL because `--drm-config` with dash currently throws, and because the SRT message does not contain "MPEG-TS".

- [ ] **Step 3: Rewrite the refusal block**

Replace the comment at `src/cli_options.cpp:462-468` and the guard at `:469-495`. The dash and stdin refusals are deleted outright. The SRT refusal stays, with a corrected message:

```cpp
    // Content protection is detected from the CMAF initialization segment's
    // sinf/schm/schi/tenc boxes and the moov-level pssh siblings, so it works
    // on any live path that receives a real init segment: the DASH CTE ingest
    // and the live stdin path both do. SRT does not -- it carries MPEG-TS and
    // the publisher synthesises a CMAF init segment from parsed elementary
    // streams, so there are no CENC boxes to detect and nothing --drm-config
    // could describe.
    if (!options.drm_systems.empty() && live_source_uses_srt) {
        throw std::runtime_error(
            "--drm-config is not supported with --live-source srt: SRT carries MPEG-TS, "
            "which has no CMAF CENC metadata (no sinf, tenc, or pssh boxes) for the "
            "publisher to detect, so there is no content protection to describe");
    }
```

- [ ] **Step 4: Run the tests to verify they pass**

Expected: 15/15.

- [ ] **Step 5: Commit**

```bash
git add src/cli_options.cpp tests/cli_options_test.cpp
git commit -m "Accept --drm-config on the live paths that can detect protection"
```

---

### Task 5: Documentation

**Files:**
- Modify: `docs/status.md`
- Modify: `docs/protocol-mapping.md`

- [ ] **Step 1: Update `docs/status.md`**

Item 6 currently says content protection is "**shipped** for the batch/VOD publish path". Extend it: protection is now also detected and signalled on the live paths that receive a real CMAF initialization segment — the DASH CTE ingest and the live stdin path.

**Six statements across the two documents become false with this phase. Every one must change, or the docs contradict themselves** — the defect the Phase 3 review caught. Locate each by its text, since line numbers will shift as you edit:

In `docs/status.md`:
1. `:96-98` — "Encrypted live input published with no `--drm-config` at all is not refused and still publishes fully unsignalled -- `--drm-config` supplies only optional deployment fields, not protection detection."
2. `:99` — "Wiring content protection into the live paths remains future work (a later phase)."

In `docs/protocol-mapping.md`:
3. `:121` — content protection described as covering "the batch/VOD publish path only".
4. `:129` — "Implemented for the batch/VOD publish path".
5. `:220-224` — "**Not signalled at all today:** the live publish paths -- `MoqtSession::publish_live()` (SRT and stdin ingest) and `publish_live_objects()` (DASH ingest) -- build their catalog through `build_live_catalog`, which never calls `attach_content_protection`."
6. `:237-241` — the library-level note ending "Wiring content protection into the live paths is future work, not part of this phase." Note this passage also claims an SDK consumer combining `PublisherConfig::drm_systems` with a live publish path "gets the same silent behaviour the CLI guard exists to prevent". That is no longer true for real-init paths: they now detect and signal protection regardless of whether `drm_systems` was supplied. Rewrite it to say what remains true — that `drm_systems` supplies only deployment fields, and that SRT still cannot detect protection.

The replacement text must say:

- Detection works on any live path receiving a real init segment.
- SRT is excluded because MPEG-TS carries no CENC metadata, not because the work is unfinished.
- A protected track with no `pssh` is refused on the live paths, matching batch.

After editing, grep **both** files for `live path`, `live publish`, `drm-config`, and `future work`, and read every hit to confirm none still claims live protection is unimplemented. Report the hit counts.

Do not overreach: MoQ Secure Objects encryption fields (MSF 5.2.38-5.2.41), MSF section 12 compression signalling, `clone` delta operations, and the CMSF 4.1.1.4.4 Authorization URL all remain genuinely unimplemented and must stay in their lists.

- [ ] **Step 2: Add limitations to `docs/protocol-mapping.md`**

In the `## CMSF content protection` section, record:

- **The DASH CTE path does not apply `--drm-config` deployment fields.** Protection is detected and signalled there, but `laURL`, `certURL`, and `robustness` are not applied because the ingest session has no access to the publisher's `DrmSystemConfig` list. `build_live_catalog`-based paths do apply them.
- **SRT cannot carry CENC.** The publisher synthesises its init segment from parsed elementary streams, so no `sinf`, `tenc`, or `pssh` exists to detect. This is a property of MPEG-TS, not a gap in the implementation.
- **`pssh` is parsed once per ingest path**, from the full initialization segment held at registration, rather than per catalog build. Note that `build_track_specific_init_segment` does copy moov-level `pssh` into each per-track init segment — every `moov` child that is not `trak` or `mvex` is copied verbatim — so a subscriber's `initData` carries the `pssh` it needs to initialise DRM. Do not describe the per-track segment as `pssh`-free; it is not.

- [ ] **Step 3: Verify the full suite and warning count**

```bash
rm -rf build && cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF -DOPENMOQ_LIBMOQ_SOURCE_DIR=/media/mondain/terrorbyte/workspace/github-moq/moq5 > /dev/null
cmake --build build -j"$(nproc)" 2>&1 | grep -E "warning:" | sort -u | wc -l
ctest --test-dir build
```

Expected: 12 unique warnings, 15/15.

- [ ] **Step 4: Commit**

```bash
git add docs/status.md docs/protocol-mapping.md
git commit -m "Document live-path content protection coverage and limitations"
```
