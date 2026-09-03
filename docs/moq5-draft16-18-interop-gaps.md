# moq5 Draft-16 and Draft-18 Interoperability Gaps

## Scope

This report records the gaps found while auditing the optional moq5/libmoq
publisher backend after the legacy `MoqtSession` backpressure fixes were fully
tested. The audit used the checked-in draft text in this repository as the
protocol authority:

- `docs/superpowers/specs/draft-ietf-moq-transport-16.txt`
- `docs/superpowers/specs/draft-ietf-moq-transport-18.txt`

The moq5 source snapshot was the clean `openmoq/moq5` checkout at commit
`0cb6ecfd962712626d81d22edfad0c6d6d71d75f`. No moq5 source was changed by
the associated MoQXR work.

## Confirmed Existing Behavior

- `src/transport/libmoq_publisher.cpp` selects only draft 16 or draft 18 and
  maps the requested version explicitly into the libmoq configuration.
- moq5 parses and retains the draft-specific timeout carriers. Draft 16 maps
  its single `DELIVERY_TIMEOUT` into the object and subgroup timeout slots;
  draft 18 retains its independent `OBJECT_DELIVERY_TIMEOUT` and
  `SUBGROUP_DELIVERY_TIMEOUT` values.
- `moq_media_sender_t` has bounded pre-ready and per-track queues and requires
  an explicit backpressure policy.
- Publisher and subscriber priority values are preserved through the public
  facade and subgroup configuration.

These points are necessary, but they do not by themselves prove the timeout
and scheduling behavior required by the drafts.

## Critical: Delivery Timeout Lifecycle

### Subgroup timeout starts at the wrong event

In `core/src/session/session_subgroup.c`, the subgroup delivery deadline is
currently armed when the subgroup is opened. The source comment explicitly
identifies the correct start instant as future work.

Draft 18 section 8 requires the timer to start only when the implementation
becomes aware that all objects in the subgroup have been published. For an
original publisher, this is the application notification corresponding to
subgroup completion, not stream creation.

Starting at open can reset a subgroup while objects are still being produced,
which shortens the negotiated timeout and changes observable wire behavior.

### Subgroup timeout ends before all data is committed

`moq_session_finish_subgroup()` clears the deadline when the FIN action is
queued. Draft 18 requires the timer to remain active until the underlying
transport reports the stream's `all data committed` state. If the deadline
expires first, the stream MUST be reset with `DELIVERY_TIMEOUT`.

The core and each supported transport adapter therefore need an explicit
completion notification that is distinct from enqueueing FIN. Clearing the
deadline when FIN is queued cannot detect data stranded by congestion or
packet loss.

### Object timeout is carried but not enforced at transport admission

The session retains negotiated object timeout values, but
`moq_session_write_object()` does not retain the first-payload-byte time and
does not reject an expired object immediately before handing it to the
transport.

Draft 18 section 8 requires that timestamp to be retained and checked before
transport admission. Draft 16 section 9.2.2.2 likewise requires an expired
object in a subgroup to reset the stream and suppress further delivery on that
subgroup.

This requires object availability timestamps, pre-admission expiry checks,
`DELIVERY_TIMEOUT` resets, and permanent no-reopen state for expired subgroup
streams.

### Required tests

- Virtual-time tests proving that opening a subgroup does not start its
  subgroup timer.
- Tests proving that application subgroup completion starts the timer.
- Adapter tests proving FIN enqueue does not clear the timer and that only
  `all data committed` does.
- Expiry tests for queued objects, including a blocked lower-priority object
  that ages while another object is sent.
- Draft-16 and draft-18 tests proving an expired subgroup is reset and never
  reopened.
- Tests for timeout changes received through `REQUEST_UPDATE`, including the
  independent draft-18 object and subgroup values.

## Important: Scheduling Semantics

moq5 preserves subscriber priority, publisher priority, and group order, but
the publisher facade currently advances its retained objects by cursor order.
The audit did not find a scheduling stage that selects among all eligible
objects using the draft priority tuple.

Drafts 16 and 18 specify subscriber priority first, then publisher priority,
with group order controlling group selection within a subscription. This is a
`SHOULD`, so it is an interoperability-quality gap rather than the same class
of normative failure as the timeout issues.

Required coverage should include competing subscriptions, competing
publisher priorities, ascending and descending group order, blocked
high-priority streams, and `REQUEST_UPDATE` reprioritization without changing
object identity or availability time.

## Important: STOP_SENDING and Renewal Coverage

The core appears to retire stale subgroup handles rather than reopening the
same transport stream. However, the audit did not find end-to-end libmoq tests
covering the complete draft-specific contract:

- Draft 16: `STOP_SENDING` prevents reopening that subgroup.
- Draft 18: the same rule applies, except a later `REQUEST_UPDATE` transition
  from `Forward=0` to `Forward=1` may renew interest and permit a new stream.
- A late `STOP_SENDING` arriving after local FIN must still be consumed without
  retaining connection-lifetime tombstones.

These should be proved at the session and adapter boundaries before declaring
the optional backend fully draft-compliant.

## MoQXR Integration Gap: Unbounded SRT Handoff

This item is in MoQXR, not moq5. In
`src/transport/libmoq_publisher.cpp`, `publish_live_srt_via_libmoq()` places
incoming fragments into an unbounded `std::deque` before they reach the bounded
`moq_media_sender_t` queue. During connection, readiness, demand, or network
stalls, that adapter queue can grow without limit even though moq5's downstream
queue is bounded.

The libmoq adapter needs a separate MoQXR change that applies the same bounded
byte/time policy and multi-video keyframe recovery semantics as the legacy
backend. It must remain compiled only for the opt-in libmoq path and must not
alter non-libmoq builds.

## Media Profile Gap: LOC-02

moq5 currently emits and parses LOC-01 properties for both transport drafts.
Its headers and negotiated-profile tests explicitly reserve LOC-02 as future
work. This does not invalidate generic MOQT draft-18 transport framing, but it
does prevent claiming draft-18 LOC-02 media-profile interoperability.

LOC-02 should be tracked separately from the backpressure work because its
property identifiers and encoding rules require coordinated sender, receiver,
catalog, and negotiated-profile changes.

## Recommended Work Split

1. In moq5, implement object-availability timestamps and pre-transport object
   timeout enforcement for drafts 16 and 18.
2. In moq5 core and adapters, move subgroup timer arming to subgroup completion
   and clear it only on `all data committed`.
3. In moq5, add deterministic priority, group-order, timeout-update, and
   STOP/renewal interoperability tests, then implement any failures they expose.
4. In a separate MoQXR libmoq-only change, bound the SRT handoff and reuse the
   tested multi-video recovery policy without affecting legacy builds.
5. Track LOC-02 as an independent media-profile project.

## Completion Criteria

The optional backend can be called compliant for these areas when:

- draft-16 and draft-18 timeout tests pass against the exact checked-in draft
  semantics;
- every transport adapter reports `all data committed` correctly;
- expired and peer-stopped subgroup streams cannot reopen except for the
  draft-18 Forward renewal case;
- scheduling tests cover priority, group order, and updates under blocked
  transport conditions;
- the MoQXR-to-moq5 live handoff is bounded by objects and bytes; and
- both `OPENMOQ_USE_LIBMOQ_PUBLISHER=OFF` and `ON` build and pass their complete
  test suites independently.
