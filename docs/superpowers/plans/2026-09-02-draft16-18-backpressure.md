# Draft 16 and 18 Backpressure Compliance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the built-in `MoqtSession` raw QUIC and WebTransport publisher enforce draft-16 and draft-18 delivery timeouts, bounded backpressure, congestion priorities, and live-ingest resource limits.

**Architecture:** Keep control writes on the existing reliable path and add a nonblocking, bounded object-write path. Preserve delivery timeout types through decoding, attach absolute deadlines and priorities to queued objects, let picoquic pull media only when flow control permits, and let `MoqtSession` schedule other eligible work on `WOULD_BLOCK`.

**Tech Stack:** C++20, CMake/CTest, picoquic/picohttp callbacks, existing hand-written test executables.

**Spec:** `docs/superpowers/specs/2026-09-02-draft16-18-backpressure-design.md`

## Global Constraints

- The local `docs/superpowers/specs/draft-ietf-moq-transport-16.txt` and `docs/superpowers/specs/draft-ietf-moq-transport-18.txt` are authoritative.
- Only draft 16 and draft 18 receive new behavior; archived draft profiles must retain existing serialization behavior.
- The default `OPENMOQ_USE_LIBMOQ_PUBLISHER=OFF` build is the implementation target.
- Do not modify `src/transport/libmoq_publisher.cpp`, any libmoq dependency, the libmoq feature gate, or libmoq runtime behavior.
- No libmoq header or symbol may be required by a non-libmoq build.
- Implement critical timeout and byte-budget tasks before priority scheduling and SRT shedding.
- Every production behavior must have an observed failing test before implementation and a passing focused test afterward.
- Use error code `0x02` for `DELIVERY_TIMEOUT` subgroup resets in draft 16 and draft 18.
- A zero timeout means absent; when both endpoints supply a non-zero timeout of the same type, use the smaller value.
- Preserve the user-owned `docs/notes.txt` and unrelated work.

---

### Task 1: Preserve draft-specific delivery timeout values

**Files:**
- Modify: `include/openmoq/publisher/transport/moqt_control_messages.h`
- Modify: `src/transport/moqt_control_messages.cpp`
- Modify: `src/transport/moqt_session.cpp`
- Modify: `tests/moqt_control_messages_test.cpp`

**Interfaces:**
- Produces: `DeliveryTimeouts { object_ms, subgroup_ms }` carried by `SubscribeMessage` and `PublishOk`.
- Consumes: draft-16 parameter `0x02`; draft-18 parameters `0x02` and `0x06`.

- [ ] **Step 1: Write the failing decode tests**

  Replace the draft-18 maximum-value assertion with literal, independent values and add the matching SUBSCRIBE case:

  ```cpp
  expect(message.delivery_timeouts.object_ms == 700,
         "draft-18 object delivery timeout remains independent");
  expect(message.delivery_timeouts.subgroup_ms == 2500,
         "draft-18 subgroup delivery timeout remains independent");
  ```

  Retain draft-16 coverage and assert `object_ms == 1500` and
  `subgroup_ms == 0`.

- [ ] **Step 2: Run RED**

  Run:

  ```bash
  cmake --build build --target openmoq-publisher-control-message-tests -j2
  ./build/openmoq-publisher-control-message-tests
  ```

  Expected: compilation or assertion failure because the model exposes only
  `delivery_timeout_ms` and merges draft-18 values.

- [ ] **Step 3: Implement the timeout model and decoder**

  Add:

  ```cpp
  struct DeliveryTimeouts {
      std::uint64_t object_ms = 0;
      std::uint64_t subgroup_ms = 0;
  };
  ```

  Replace `delivery_timeout_ms` in `SubscribeMessage` and `PublishOk` with
  `delivery_timeouts`. Decode `0x02` into `object_ms`; decode `0x06` only for
  draft 18 into `subgroup_ms`. Keep the existing rejection of on-wire zero.

- [ ] **Step 4: Run GREEN**

  Rebuild and run `openmoq-publisher-control-message-tests`; expect success.

- [ ] **Step 5: Update compile-time consumers without adding enforcement**

  Update callers to pass both values to the existing close-drain bookkeeping,
  preserving its current larger-timeout shutdown bound. Do not implement
  per-object enforcement in this task.

- [ ] **Step 6: Re-run focused control and transport tests**

  ```bash
  cmake --build build --target openmoq-publisher-control-message-tests openmoq-publisher-transport-tests -j2
  ./build/openmoq-publisher-control-message-tests
  ./build/openmoq-publisher-transport-tests
  ```

- [ ] **Step 7: Commit only Task 1 files**

  ```bash
  git add include/openmoq/publisher/transport/moqt_control_messages.h src/transport/moqt_control_messages.cpp tests/moqt_control_messages_test.cpp src/transport/moqt_session.cpp
  git commit -m "Preserve draft delivery timeout semantics"
  ```

---

### Task 2: Add a strict bounded media queue

**Files:**
- Create: `src/transport/pending_media_queue.h`
- Create: `tests/pending_media_queue_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: a transport-internal queue that owns media bytes until the QUIC stack requests them.
- Produces: `MediaAdmission::{kAccepted, kWouldBlock, kOversized}`.
- Produces: deadline-aware per-stream front access and exact `queued_bytes()` accounting.

- [ ] **Step 1: Write the failing queue tests**

  Add a focused executable covering literal budgets:

  ```cpp
  PendingMediaQueue queue(10);
  expect(queue.try_push(make_write(1, 8)) == MediaAdmission::kAccepted);
  expect(queue.try_push(make_write(3, 3)) == MediaAdmission::kWouldBlock);
  expect(queue.queued_bytes() == 8);

  PendingMediaQueue exact(10);
  expect(exact.try_push(make_write(1, 10)) == MediaAdmission::kAccepted);
  expect(exact.queued_bytes() == 10);

  PendingMediaQueue oversized(10);
  expect(oversized.try_push(make_write(1, 11)) == MediaAdmission::kOversized);
  expect(oversized.queued_bytes() == 0);
  ```

  Also test partial consumption, stream clearing, connection clearing, FIFO
  order within a stream, and expiry before the first byte is consumed.

- [ ] **Step 2: Run RED**

  Configure/build the new target. Expected: failure because
  `pending_media_queue.h` and its API do not exist.

- [ ] **Step 3: Implement the minimal queue**

  Define:

  ```cpp
  enum class MediaAdmission { kAccepted, kWouldBlock, kOversized };

  struct PendingMediaWrite {
      std::uint64_t stream_id;
      std::vector<std::uint8_t> bytes;
      std::size_t offset = 0;
      bool fin = false;
      std::uint8_t transport_priority = 255;
      std::optional<std::chrono::steady_clock::time_point> object_deadline;
      std::optional<std::chrono::steady_clock::time_point> subgroup_deadline;
  };
  ```

  `try_push` must use `bytes.size() > capacity - queued_bytes` after checking
  `bytes.size() > capacity`, so size arithmetic cannot overflow. Decrement the
  count only when bytes are consumed or cleared.

- [ ] **Step 4: Run GREEN**

  Build and run `openmoq-publisher-pending-media-queue-tests`; expect success.

- [ ] **Step 5: Commit only Task 2 files**

  ```bash
  git add CMakeLists.txt src/transport/pending_media_queue.h tests/pending_media_queue_test.cpp
  git commit -m "Add bounded media admission queue"
  ```

---

### Task 3: Enforce bounded nonblocking transport admission

**Files:**
- Modify: `include/openmoq/publisher/transport/publisher_transport.h`
- Modify: `include/openmoq/publisher/transport/picoquic_client.h`
- Modify: `include/openmoq/publisher/transport/webtransport_client.h`
- Modify: `src/transport/picoquic_client.cpp`
- Modify: `src/transport/webtransport_client.cpp`
- Modify: `tests/picoquic_smoke_test.cpp`
- Modify: `tests/webtransport_client_test.cpp`
- Modify: `tests/pending_media_queue_test.cpp`

**Interfaces:**
- Consumes: `PendingMediaQueue` from Task 2.
- Produces: `ObjectWriteDisposition::{kAccepted, kWouldBlock, kFailed}` and `ObjectWriteResult`.
- Produces: `try_write_object(stream_id, bytes, fin, ObjectWriteOptions)` on `PublisherTransport`.
- Uses an exact production media-admission budget of `4 * 1024 * 1024` bytes
  per connection for this change; tuning or making it configurable is separate.

- [ ] **Step 1: Write failing transport-contract tests**

  Add tests proving that two writes whose combined size exceeds the configured
  media budget return accepted then would-block without waiting, that control
  `write_stream` remains usable while media is full, and that connection close
  clears queued media and wakes admission. Use a test budget of 10 bytes and
  assert exact dispositions.

- [ ] **Step 2: Run RED**

  Build the picoquic and WebTransport focused targets. Expected: failure
  because `try_write_object` and the observable disposition do not exist.

- [ ] **Step 3: Add the object-write contract**

  Define:

  ```cpp
  struct ObjectWriteOptions {
      std::uint8_t transport_priority = 255;
      std::optional<std::chrono::steady_clock::time_point> object_deadline;
      std::optional<std::chrono::steady_clock::time_point> subgroup_deadline;
  };

  enum class ObjectWriteDisposition { kAccepted, kWouldBlock, kFailed };

  struct ObjectWriteResult {
      ObjectWriteDisposition disposition = ObjectWriteDisposition::kFailed;
      std::string message;
  };
  ```

  Add a virtual `try_write_object` with a compatibility implementation for test
  transports that delegates to `write_stream`. Raw QUIC and WebTransport must
  override it with bounded, nonblocking behavior.

- [ ] **Step 4: Convert raw QUIC media to pull delivery**

  Read picoquic's complete `doc/send_receive_data.md` and the current callback
  implementation before editing. Store accepted media in `PendingMediaQueue`,
  mark the stream active, and serve bytes only from
  `picoquic_callback_prepare_to_send` through
  `picoquic_provide_stream_data_buffer`. Set `is_still_active` only while that
  stream retains queued bytes. Keep control/request writes on the current copy
  path.

- [ ] **Step 5: Convert WebTransport media to provide-data delivery**

  Use `picohttp_callback_provide_data` and
  `picoquic_provide_stream_data_buffer` for application streams. Preserve
  h3zero stream context ownership and existing control-session behavior.

- [ ] **Step 6: Remove the 30-second pseudo-cap path**

  Delete the media use of `kMaxPendingBytes` and `condition.wait_for`. A full
  media queue returns `kWouldBlock` synchronously. Fatal picoquic/h3zero errors
  return `kFailed`; they are never reclassified as transient backpressure.

- [ ] **Step 7: Run GREEN**

  Build and run pending-queue, picoquic smoke, WebTransport, close, and
  close-drain tests. Expect success with no wall-clock 30-second wait.

- [ ] **Step 8: Commit only Task 3 files**

  ```bash
  git add include/openmoq/publisher/transport/publisher_transport.h include/openmoq/publisher/transport/picoquic_client.h include/openmoq/publisher/transport/webtransport_client.h src/transport/picoquic_client.cpp src/transport/webtransport_client.cpp tests/picoquic_smoke_test.cpp tests/webtransport_client_test.cpp tests/pending_media_queue_test.cpp
  git commit -m "Enforce bounded transport backpressure"
  ```

---

### Task 4: Enforce draft-16 and draft-18 delivery deadlines

**Files:**
- Modify: `include/openmoq/publisher/transport/moqt_session.h`
- Modify: `src/transport/moqt_session.cpp`
- Modify: `src/transport/picoquic_client.cpp`
- Modify: `src/transport/webtransport_client.cpp`
- Modify: `tests/moqt_session_test.cpp`
- Modify: `tests/pending_media_queue_test.cpp`

**Interfaces:**
- Consumes: separate `DeliveryTimeouts` and `try_write_object`.
- Produces: injectable `NowFunction` for deterministic session tests.
- Produces: transport-side subgroup deadline monitoring until all data is committed.

- [ ] **Step 1: Write the draft-16 RED test**

  Use `MockTransport`, a manually advanced monotonic clock, and a subscription
  carrying `DELIVERY_TIMEOUT=100`. Stall object admission, advance to 101 ms,
  and assert exactly one reset `(stream_id, 0x02)` plus no later stream reopen
  for the same subgroup.

- [ ] **Step 2: Run the transport test and observe RED**

  Expected: the object is written or retried and no `0x02` reset is recorded.

- [ ] **Step 3: Write the draft-18 independent-deadline RED tests**

  Test `OBJECT_DELIVERY_TIMEOUT=100` with
  `SUBGROUP_DELIVERY_TIMEOUT=500`, then reverse the values. Assert object
  expiry occurs at the object deadline and subgroup expiry occurs only after
  final publication while all-data-committed remains false.

- [ ] **Step 4: Run RED for both draft-18 cases**

  Expected: no per-object/per-subgroup reset behavior exists.

- [ ] **Step 5: Implement session deadline state**

  Inject a defaulted monotonic clock into `MoqtSession`. Retain each object's
  first-availability timestamp while it remains session-owned. Pass absolute
  deadlines into `try_write_object`. On session-owned expiry, reset an open
  stream or mark an unopened subgroup expired; `SubgroupSenderState` must keep
  an expired-subgroup set so it cannot reopen that key.

- [ ] **Step 6: Implement transport deadline state**

  Before providing an object's first byte, expire it and reset its subgroup
  with `0x02`. When final publication is declared, arm the subgroup deadline
  from the application-provided completion time. On the packet-loop thread,
  cancel the timer when picoquic reports the stream fully acknowledged; reset
  with `0x02` if the deadline wins. Clear queued bytes on reset.

- [ ] **Step 7: Run GREEN**

  Run `openmoq-publisher-transport-tests`, pending queue tests, picoquic smoke,
  WebTransport, and close-drain tests. Expect all deadline cases to pass.

- [ ] **Step 8: Commit only Task 4 files**

  ```bash
  git add include/openmoq/publisher/transport/moqt_session.h src/transport/moqt_session.cpp src/transport/picoquic_client.cpp src/transport/webtransport_client.cpp tests/moqt_session_test.cpp tests/pending_media_queue_test.cpp
  git commit -m "Enforce MOQT delivery deadlines"
  ```

---

### Task 5: Schedule eligible objects by draft priority

**Files:**
- Modify: `src/transport/moqt_session.cpp`
- Modify: `src/transport/picoquic_client.cpp`
- Modify: `src/transport/webtransport_client.cpp`
- Modify: `tests/moqt_session_test.cpp`
- Modify: `tests/picoquic_smoke_test.cpp`

**Interfaces:**
- Consumes: nonblocking `try_write_object` and parsed subscription priority/group order.
- Produces: draft ordering comparator and transport priority classes.

- [ ] **Step 1: Write priority RED tests**

  Add independent tests for subscriber priority, publisher priority,
  ascending group order, descending group order, and a would-block
  high-priority stream that allows an eligible lower-priority stream to make
  progress. Derive expected write order as literal request/group/object IDs.

- [ ] **Step 2: Run RED**

  Expected: current media-time/map ordering violates at least subscriber
  priority, descending group order, and blocked-stream progress.

- [ ] **Step 3: Implement the scheduler comparator**

  Select eligible candidates by lower subscriber priority, lower publisher
  priority, requested group direction, lower subgroup ID, then lower object ID.
  Keep media time only for pacing after selection. Treat `kWouldBlock` as
  temporary ineligibility and continue scanning candidates. Apply
  `SUBSCRIBE_UPDATE` to not-yet-admitted candidates.

- [ ] **Step 4: Assign transport priority classes**

  Use distinct classes with lower numbers first: control `0`, draft-18 request
  streams `1`, and object streams starting at `2`. Map object stream priority
  from the scheduler tuple without allowing it to outrank control/request
  traffic.

- [ ] **Step 5: Run GREEN**

  Run transport, picoquic smoke, and WebTransport focused tests.

- [ ] **Step 6: Commit only Task 5 files**

  ```bash
  git add src/transport/moqt_session.cpp src/transport/picoquic_client.cpp src/transport/webtransport_client.cpp tests/moqt_session_test.cpp tests/picoquic_smoke_test.cpp
  git commit -m "Honor MOQT congestion priorities"
  ```

---

### Task 6: Bound live SRT ingestion and recover at a keyframe

**Files:**
- Create: `src/live_media_queue.h`
- Create: `tests/live_media_queue_test.cpp`
- Modify: `src/transport/moqt_session.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: a thread-safe `LiveMediaQueue` with byte and media-duration limits.
- Consumes: `MediaFragment` track, timestamp/group, payload size, and keyframe metadata.
- Integrates with an exact default bound of 16 MiB and 2 seconds of queued
  media; the focused tests inject smaller literal bounds.

- [ ] **Step 1: Write SRT queue RED tests**

  Construct literal video/audio fragments spanning two GOPs. Use a 100-byte,
  2-second test limit. Assert the queue never exceeds either limit, overflow
  removes the old video GOP and its aligned audio, initialization is retained,
  and the first emitted video fragment after recovery is a keyframe.

- [ ] **Step 2: Run RED**

  Expected: failure because the reusable bounded queue does not exist and the
  current SRT path uses an unrestricted deque.

- [ ] **Step 3: Implement bounded GOP-aware admission**

  Track exact payload bytes and media span under one mutex. On overflow, evict
  complete old groups through the next video-keyframe boundary and remove
  audio in the discarded time interval. Return an explicit result indicating
  accepted, shed-to-keyframe, or no-decodable-boundary.

- [ ] **Step 4: Integrate only the built-in SRT path**

  Replace the local unrestricted queue in `MoqtSession::publish_live_srt`.
  Do not touch `libmoq_publisher.cpp`. If no keyframe recovery is possible
  inside the bound, terminate the affected publishing operation with a
  resource-limit/too-far-behind diagnostic.

- [ ] **Step 5: Run GREEN**

  Build and run live-media-queue, live-SRT-config, transport, and publisher API
  tests.

- [ ] **Step 6: Commit only Task 6 files**

  ```bash
  git add CMakeLists.txt src/live_media_queue.h src/transport/moqt_session.cpp tests/live_media_queue_test.cpp
  git commit -m "Bound live SRT buffering"
  ```

---

### Task 7: Verify the default build and freeze the libmoq boundary

**Files:**
- Modify only if a failing test exposes a default-backend regression.
- Do not modify any libmoq source or build gate.

**Interfaces:**
- Consumes: all Tasks 1-6.
- Produces: fresh build/test evidence for `OPENMOQ_USE_LIBMOQ_PUBLISHER=OFF`.

- [ ] **Step 1: Configure a clean non-libmoq build**

  ```bash
  cmake -S . -B /tmp/moqxr-backpressure-build -DOPENMOQ_USE_LIBMOQ_PUBLISHER=OFF -DOPENMOQ_BUILD_EXAMPLES=OFF
  cmake --build /tmp/moqxr-backpressure-build -j2
  ```

  Confirm configure output reports `libmoq available .......... OFF` and the
  legacy `MoqtSession` backend.

- [ ] **Step 2: Run the complete suite**

  ```bash
  ctest --test-dir /tmp/moqxr-backpressure-build --output-on-failure
  ```

  Require zero failures.

- [ ] **Step 3: Run repository checks**

  ```bash
  git diff --check
  git status --short
  ```

  Confirm `src/transport/libmoq_publisher.cpp` and libmoq configuration files
  are absent from the implementation diff.

- [ ] **Step 4: Run C++ review and final reviewer gate**

  Review every changed `.h` and `.cpp` for correctness, concurrency,
  ownership, timeout races, queue accounting, and coverage. Address all
  Critical and Important findings and re-run affected tests.

---

## Post-verification libmoq audit

Only after Task 7 is fully green, perform a read-only comparison of
`src/transport/libmoq_publisher.cpp`, the configured moq5 service/adapter
surface, and the draft-16/18 requirements above. Report which guarantees are
owned by moqxr versus libmoq, identify any compliance gaps, and propose a
separate change set. Do not mix libmoq edits into this branch.
