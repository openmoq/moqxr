# CTE LL-DASH Ingest Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add CTE LL-DASH as a live publishing mechanism by accepting multiple concurrent HTTP/1.1 chunked CMAF ingest paths and forwarding resulting live objects through the existing MoQ publisher.

**Architecture:** Implement a small HTTP/1.1 ingest server that accepts `POST`/`PUT` requests with `Transfer-Encoding: chunked`, decodes request bodies incrementally, and feeds decoded bytes into a CMAF live adapter. The adapter reuses `StreamingMp4Reader`, `extract_tracks`, `build_live_catalog`, `build_live_fragment`, and `Publisher::publish_live_objects(...)`, keeping the new surface limited to one live ingest module and CLI wiring.

**Tech Stack:** C++20, POSIX sockets on non-Windows builds, existing MP4/CMAF parser and MoQ publisher API, CMake tests.

---

## File Structure

- Create `include/openmoq/publisher/live_dash_ingest.h`
  - Public config for the DASH ingest listener.
  - Queue-backed live object source builder.
  - Testable chunked-transfer decoder and CMAF stream adapter.
- Create `src/live_dash_ingest.cpp`
  - HTTP request parser.
  - Chunked transfer decoder.
  - Multi-path connection handler.
  - CMAF init/media object production.
- Create `tests/live_dash_ingest_test.cpp`
  - Unit tests for chunked decoding, CMAF object production, and concurrent path behavior.
- Modify `include/openmoq/publisher/cli_options.h`
  - Add `LiveSourceKind::kDash` and DASH listener options.
- Modify `src/cli_options.cpp`
  - Parse `--live-source dash`, `--dash-listen`, `--dash-path`, and `--dash-queue-depth`.
- Modify `src/main.cpp`
  - Start DASH ingest mode and call `Publisher::publish_live_objects(...)`.
- Modify `CMakeLists.txt`
  - Compile the new module and add the new test executable.
- Modify `docs/quickstart.md` and `README.md`
  - Document the new ingest mode after implementation.

## Task 1: Plan and Baseline

- [ ] **Step 1: Save this plan**

Create `docs/ctedash-implementation-plan.md` with this content.

- [ ] **Step 2: Verify baseline branch**

Run:

```bash
git status -sb
```

Expected: branch is `feature/ctedash`; only this plan file is new before implementation starts.

## Task 2: Chunked Transfer Decoder

**Files:**
- Create: `include/openmoq/publisher/live_dash_ingest.h`
- Create: `src/live_dash_ingest.cpp`
- Test: `tests/live_dash_ingest_test.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing tests**

Add tests that feed chunked bytes split across arbitrary input boundaries:

```cpp
ok &= expect_chunked_decodes("4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n", "Wikipedia");
ok &= expect_chunked_decodes("4;token=value\r\nWiki\r\n0\r\n\r\n", "Wiki");
ok &= expect_chunked_rejects("FFFFFFFFFFFFFFFFF\r\nx\r\n0\r\n\r\n");
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build --target openmoq-publisher-live-dash-tests
./build/openmoq-publisher-live-dash-tests
```

Expected: build or test fails because the new module does not exist yet.

- [ ] **Step 3: Implement minimal decoder**

Implement `ChunkedBodyDecoder::append(...)` and `ChunkedBodyDecoder::take_decoded()` with explicit states for size line, data, data CRLF, trailers, complete, and error. Enforce a configurable max chunk size and reject integer overflow.

- [ ] **Step 4: Run test to verify it passes**

Run the same test command. Expected: all chunked decoder tests pass.

## Task 3: CMAF Live Object Adapter

**Files:**
- Modify: `include/openmoq/publisher/live_dash_ingest.h`
- Modify: `src/live_dash_ingest.cpp`
- Test: `tests/live_dash_ingest_test.cpp`

- [ ] **Step 1: Write failing tests**

Use existing MP4 test helpers to build:

- one init segment containing `ftyp+moov`
- two `moof+mdat` media pairs on path `/ingest/video`
- two `moof+mdat` media pairs on path `/ingest/audio`

Assert:

- no media object is emitted before init is available
- a catalog object is emitted first
- media objects retain path-specific track names
- path queues can interleave without blocking each other

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build --target openmoq-publisher-live-dash-tests
./build/openmoq-publisher-live-dash-tests
```

Expected: tests fail because the adapter does not exist.

- [ ] **Step 3: Implement adapter**

Implement a queue-backed `LiveDashIngestSession`:

- `ingest(path, bytes)` appends bytes to that path's `StreamingMp4Reader`.
- `ftyp` and `moov` are buffered as init bytes.
- tracks are extracted after `moov`.
- `moof` is held until the following `mdat`.
- each complete `moof+mdat` becomes a `LiveObject`.
- group IDs increment independently per track/path.
- `source()` returns a `LiveObjectSource` with all known tracks plus `catalog`.

- [ ] **Step 4: Run test to verify it passes**

Run the same test command. Expected: adapter tests pass.

## Task 4: HTTP Server and Multiple Concurrent Paths

**Files:**
- Modify: `src/live_dash_ingest.cpp`
- Test: `tests/live_dash_ingest_test.cpp`

- [ ] **Step 1: Write failing tests**

Start the ingest server on loopback port `0`, open two client sockets, and send chunked `PUT` requests to `/ingest/video` and `/ingest/audio`. Assert both paths produce objects and that a malformed request gets a `400` response without killing the listener.

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build --target openmoq-publisher-live-dash-tests
./build/openmoq-publisher-live-dash-tests
```

Expected: tests fail because server handling is not implemented.

- [ ] **Step 3: Implement server**

Implement `LiveDashIngestServer`:

- bind/listen on configured host/port
- expose actual bound port for tests
- accept each connection in its own worker thread
- parse request line and headers
- allow only `POST` and `PUT`
- require path prefix match
- decode `Transfer-Encoding: chunked`
- feed decoded bytes into the shared session
- send `204 No Content` on clean completion and `400`/`405` on invalid input
- stop cleanly when requested

- [ ] **Step 4: Run test to verify it passes**

Run the same test command. Expected: HTTP server tests pass.

## Task 5: CLI and Publisher Wiring

**Files:**
- Modify: `include/openmoq/publisher/cli_options.h`
- Modify: `src/cli_options.cpp`
- Modify: `src/main.cpp`
- Test: `tests/cli_options_test.cpp`

- [ ] **Step 1: Write failing CLI tests**

Assert:

```cpp
--live-source dash --dash-listen 127.0.0.1:8080 --dash-path /ingest --endpoint https://relay.example.com:443/moq
```

parses successfully, while missing `--dash-listen` or missing `--endpoint` fails.

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build --target openmoq-publisher-cli-tests
./build/openmoq-publisher-cli-tests
```

Expected: tests fail because the CLI mode is not implemented.

- [ ] **Step 3: Implement CLI and main wiring**

Add:

- `LiveSourceKind::kDash`
- `--dash-listen <host:port>`
- `--dash-path <prefix>` defaulting to `/ingest`
- `--dash-queue-depth <count>` defaulting to `128`

In `main.cpp`, start `LiveDashIngestServer`, then publish `server.source()` with `Publisher::publish_live_objects(...)`.

- [ ] **Step 4: Run test to verify it passes**

Run the CLI tests. Expected: all CLI tests pass.

## Task 6: Docs and Final Verification

**Files:**
- Modify: `README.md`
- Modify: `docs/quickstart.md`

- [ ] **Step 1: Document usage**

Add a short CTE LL-DASH section with the publisher command and a chunked HTTP producer example.

- [ ] **Step 2: Run complete verification**

Run:

```bash
cmake --build build
ctest --test-dir build --output-on-failure
git diff --check
```

Expected: build succeeds, tests pass, and whitespace check passes.

- [ ] **Step 3: Review C++ changes**

Review `git diff HEAD` for correctness, concurrency, ownership, parser overflow handling, and test coverage before commit.

