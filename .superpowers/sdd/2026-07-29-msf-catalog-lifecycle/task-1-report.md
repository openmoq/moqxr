# Task 1 Report: Live-by-default on the batch path

## Summary
Implemented live-by-default semantics on the batch publish path with VOD opt-in. The batch publish path now correctly marks catalogs as live (`isLive=true`, includes `generatedAt`, omits `trackDuration`) unless explicitly configured for VOD. All 13/13 ctest targets pass.

## Files Modified
1. `include/openmoq/publisher/cmsf_packager.h` - Added `bool vod = false` parameter to `build_publish_plan` signature with documentation
2. `src/cmsf_packager.cpp` - Updated function definition and implementation to emit `generatedAt` when `!vod` and pass `is_live=!vod` to `make_msf_track`
3. `include/openmoq/publisher/publisher_api.h` - Added `bool vod = false` field to `PublisherConfig` struct (after `bool loop`)
4. `src/publisher_api.cpp` - Threaded `config_.vod` through both `prepare_file` and `prepare_stream` call sites
5. `tests/cmaf_segmenter_test.cpp` - Updated test fixture with non-zero `duration_ms=5000`, replaced assertions with bidirectional trackDuration checks, and added VOD opt-in test

## Build Configuration
```bash
cmake -S . -B build -DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF -DOPENMOQ_LIBMOQ_SOURCE_DIR=/media/mondain/terrorbyte/workspace/github-moq/moq5
```

## Build and Test Commands

### Initial Build and Test (Task 1)
```bash
cmake --build build --target openmoq-publisher-packaging-tests && ctest --test-dir build -R openmoq-publisher-packaging-tests --output-on-failure
```
Result: PASS (1/1)

### Full Test Suite (Task 1)
```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Output:
```
100% tests passed, 0 tests failed out of 13

Total Test time (real) =   6.22 sec

Targets:
 1/13 openmoq-publisher-packaging-tests
 2/13 openmoq-publisher-msf-catalog-tests
 3/13 openmoq-publisher-cli-tests
 4/13 openmoq-publisher-live-srt-config-tests
 5/13 openmoq-publisher-live-dash-tests
 6/13 openmoq-publisher-transport-tests
 7/13 openmoq-publisher-webtransport-tests
 8/13 openmoq-publisher-api-tests
 9/13 openmoq-publisher-cat4moq-api-tests
10/13 openmoq-publisher-cat4moq-transport-token-tests
11/13 openmoq-publisher-control-message-tests
12/13 openmoq-publisher-libmoq-translation-tests
13/13 openmoq-publisher-msfts-tests
```

## Fix Round 1: trackDuration Assertions

### Finding 1 Resolution
The original assertion at line 1038 was vacuous—it asserted that `trackDuration` was NOT present, but the test fixture had `duration_ms=0` by default, so the assertion would pass regardless of the implementation correctness.

**Fix applied:**
1. Updated test fixture `TrackDescription` to include `duration_ms=5000`
2. Added bidirectional assertions against the SAME fixture:
   - Live (default) catalog: `expect_not_contains(catalog_text, "\"trackDuration\"")`
   - VOD catalog: `expect_contains(vod_catalog_text, "\"trackDuration\":5000")`

**Inversion Check Results:**
Temporarily inverted the `vod` flag from `true` to `false` in the VOD test. All three assertions failed:
- "expected opt-in VOD to mark tracks not live" (isLive was true instead of false)
- "expected no generatedAt on a VOD catalog" (generatedAt was present when it shouldn't be)
- "expected VOD catalog to include trackDuration with the test fixture value" (trackDuration was absent)

Restored the flag to `true`; all assertions pass. Confirmed that the fixture-based assertions are real and not vacuous.

### Fix Commit
```
abdca5c8 Fix: Add trackDuration assertions in both directions for live/VOD tests
```

### Full Test Suite After Fix
```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Result: PASS, 100% (13/13 targets)

## Warnings
No new warnings added. Project baseline remains at ~10 pre-existing warnings (no `-Werror` enforced).

## Decisions
- Chose `duration_ms=5000` as a clear, testable value for the fixture
- Added explicit MSF section references (5.1.2, 5.2.35) to assertion messages for spec traceability
- Kept VOD test case focused on single-track fixture (as per brief) for clarity

## Concerns
None. Implementation correctly threads `is_live` flag through the pipeline, `trackDuration` is emitted only when `!vod && duration_ms != 0`, and all assertions now validate both positive and negative cases.
