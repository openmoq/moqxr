# Draft 16 and 18 Backpressure Compliance Design

## Scope

This change makes the built-in `MoqtSession` publisher and its raw QUIC and
WebTransport transports enforce the backpressure, priority, delivery-timeout,
and resource-exhaustion rules needed by the current draft-16 and draft-18
interop targets.

The opt-in libmoq publisher is explicitly outside this implementation. No
libmoq header, symbol, compile definition, configuration default, or runtime
behavior may be changed by this work. After the built-in backend is complete
and fully verified, libmoq will receive a separate compliance audit and, if
necessary, a separate implementation change.

Drafts 14, 17, and 19 remain outside the behavior changes and acceptance
matrix. Existing serialization tests for those archived profiles must continue
to pass where they are still built.

## Normative Requirements

The local draft text is authoritative.

For draft 16:

- Section 7 defines an object blocked by transport flow control as not
  schedulable and recommends subscriber priority, publisher priority, group
  order, then subgroup/object order.
- Section 9.2.2.2 requires a subgroup stream to be reset with
  `DELIVERY_TIMEOUT` when an object exceeds its negotiated delivery timeout and
  recommends not reopening the subgroup.
- Section 12.1 requires a stream, preferably the lowest-priority stream, to be
  cancelled after a resource limit is reached.

For draft 18:

- Section 7 retains the scheduling requirements and recommends control streams
  before bidirectional request streams before subscribed objects.
- Section 8 defines independent `OBJECT_DELIVERY_TIMEOUT` and
  `SUBGROUP_DELIVERY_TIMEOUT` values. Object age is checked before handing the
  object to the transport. A subgroup timer begins once all objects are known
  to have been published and runs until the transport reports all data
  committed. Expiry resets the subgroup stream with `DELIVERY_TIMEOUT`.
- Section 13.5 retains the resource-limit cancellation requirement.

## Root Causes

The current control-message model collapses draft-18 object and subgroup
timeouts into one maximum value. `MoqtSession` forwards that value only to the
connection close-drain bound, so it has no per-object timestamp, no subgroup
completion timer, and no path that resets an expired subgroup with error code
`0x02`.

The raw QUIC and WebTransport clients call picoquic's copy-based stream API.
That API accepts and copies application data even while the peer's flow-control
window is closed. The existing 4 MiB check covers only the short-lived
cross-thread queue, excludes the incoming write from its calculation, and
ignores expiration of its 30-second wait. It therefore neither reports useful
backpressure nor bounds total retained media.

The session scheduler orders objects by loop cycle and media time. Although it
parses subscriber priority and group order, it does not use either field when
selecting the next object. Object streams also retain a single default
transport priority.

The SRT producer appends fragments to an unrestricted deque. A stalled network
consumer can therefore allow ingest memory and media latency to grow without a
limit.

## Transport Contract

Control and request messages keep the existing reliable `write_stream`
operation. Object data uses a nonblocking transport-admission operation with
three observable outcomes: accepted, would block, and failed. An accepted
object is owned by the transport; a would-block object remains owned by the
session and can be scheduled again or expired; a failure terminates the
current publishing operation.

Each object admission carries:

- the target stream and encoded bytes;
- whether the write completes the subgroup stream;
- subscriber and publisher priority;
- the absolute object-delivery deadline, when present; and
- the absolute subgroup-delivery deadline for a final subgroup write, when
  present.

The transport maintains a strict connection-wide queued-media byte budget.
Admission checks `currently queued bytes + incoming bytes` with overflow-safe
arithmetic. An individual object larger than the budget is rejected as a
resource-limit failure instead of being admitted. The configured budget covers
all media bytes retained by the application transport layer. This change keeps
the existing 4 MiB value as the exact per-connection media-admission budget;
making it configurable is outside scope.

Raw QUIC uses picoquic's active-stream/prepare-to-send path so bytes remain in
the bounded application queue until picoquic can put them on the wire. The
WebTransport implementation uses the corresponding provide-data callback.
Neither implementation moves an entire stalled media backlog into picoquic's
copy queue.

Before providing the first byte of an object to picoquic, the transport checks
its object deadline. If it has expired, every queued object for that subgroup
is discarded, the stream is reset with `DELIVERY_TIMEOUT (0x02)`, and the
session is notified that the subgroup is closed. The subgroup cannot be
reopened.

When a final subgroup write is admitted, its subgroup deadline is armed from
the time the application declared the subgroup complete. The packet-loop
thread cancels the deadline when the stream reaches all-data-committed state;
otherwise it resets the stream with `0x02` at expiry. This timer is independent
of the connection close-drain timeout.

Control capacity is reserved independently from the media budget so a full
media queue cannot prevent the messages needed to establish aliases, update
requests, or terminate subscriptions.

## Session Scheduling

`MoqtSession` retains the time at which each object first becomes available
from the application. The negotiated effective timeout is computed per
timeout type. If both publisher and subscriber provide a non-zero value, the
smaller value is used; zero means that side supplied no limit.

The next eligible object is selected using this tuple:

1. subscriber priority, lower numeric value first;
2. publisher priority, lower numeric value first;
3. requested group order within the subscription;
4. subgroup ID; and
5. object ID.

Media time remains the pacing clock but is not a congestion priority. A stream
whose transport admission reports would-block is temporarily ineligible; the
scheduler considers other streams instead of blocking the session thread.
`SUBSCRIBE_UPDATE` changes the subscriber priority for objects that have not
yet been admitted.

The transport priority assigned to a subgroup reflects its subscriber and
publisher priority as far as picoquic's single priority value permits. Control
streams receive the highest transport priority, draft-18 request streams the
next priority class, and object streams follow. Exact ordering among object
streams is retained in the session scheduler.

If an object deadline expires while the object remains session-owned, the
session resets the already-open subgroup with `0x02`, or records the subgroup
as expired without opening a stream. Later objects from the expired subgroup
are skipped.

## Resource-Limit Policy

Batch and VOD publishing remain lossless while capacity is available. If the
strict media budget is exhausted and no other object can make progress before
the applicable timeout or transport resource deadline, the lowest-priority
affected subgroup is reset. The subscription is completed with
`TOO_FAR_BEHIND` where the selected draft and message flow permit that signal.

Live SRT ingestion is bounded by both bytes and media duration. On overflow it
drops the oldest incomplete live media through the next video keyframe, along
with audio belonging to the discarded time range, so publication resumes from
a decodable and A/V-aligned boundary. Initialization and catalog state are not
dropped. The built-in SRT path uses exact default limits of 16 MiB and 2
seconds; focused tests inject smaller limits. If a decodable boundary cannot be found within the configured bound,
the affected live subscription is terminated rather than allowing unbounded
growth.

Existing bounded stdin and DASH queues keep their current policies unless a
test demonstrates that they bypass the new transport contract.

## Error and Shutdown Behavior

- `WOULD_BLOCK` is scheduling state, not a fatal transport error.
- `DELIVERY_TIMEOUT (0x02)` closes one subgroup stream and prevents it from
  reopening; it does not close the connection.
- Connection closure wakes all blocked producers and releases queued media.
- Fatal transport failures remain failures and must not be retried as
  backpressure.
- The close-drain bound remains a connection-shutdown safeguard and does not
  substitute for object or subgroup delivery timers.

## Test Strategy

Every behavior change follows an observed red/green cycle. Tests use an
injectable monotonic clock and deterministic transport capacity instead of
wall-clock sleeps.

Critical tests are implemented first:

1. Draft-16 `DELIVERY_TIMEOUT` expiry resets the subgroup with `0x02` and later
   objects do not reopen it.
2. Draft-18 decoding preserves independent object and subgroup timeout values.
3. Draft-18 object expiry and subgroup all-data-committed expiry are exercised
   independently.
4. Queue admission rejects `queued + incoming` beyond the exact budget,
   rejects an oversized first object, and never admits an object merely because
   a wait elapsed.
5. A flow-control-stalled transport retains no more than its configured media
   budget and still admits reserved control traffic.

Important tests follow only after all critical tests are green:

6. Subscriber priority wins between eligible subscriptions.
7. Publisher priority and ascending/descending group order break ties as the
   drafts specify.
8. A flow-control-blocked high-priority stream does not prevent an eligible
   lower-priority stream from progressing.
9. Draft-18 control, request, and object streams receive distinct transport
   priority classes.
10. SRT queue overflow remains within its byte/time bounds and resumes at a
    video keyframe with aligned audio.

Focused test binaries run after each red/green cycle. Completion requires a
fresh default-backend build, the complete CTest suite, `git diff --check`, and
a C++ review of every changed `.h` and `.cpp` file.

## Acceptance Criteria

- Draft-16 and draft-18 negotiated delivery deadlines cause the required
  subgroup reset behavior under deterministic stalled-transport tests.
- Draft-18 object and subgroup deadlines remain separate throughout decoding,
  negotiation, session state, and transport enforcement.
- Retained media never exceeds the configured application byte budget.
- Flow-control-blocked streams do not stall unrelated eligible streams or
  control traffic.
- Congestion ordering uses the draft priority tuple and honors group order.
- Live SRT buffering is bounded and recovers only at a decodable A/V-aligned
  boundary.
- All existing default-backend tests pass.
- A build with libmoq unavailable or disabled continues to compile and test
  without any libmoq dependency.
- The libmoq backend is audited only after these criteria are met, and any
  resulting change is isolated in a separate implementation and verification
  cycle.
