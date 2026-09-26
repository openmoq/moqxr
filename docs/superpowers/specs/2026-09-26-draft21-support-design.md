# Draft 21 Support Design

## Scope

This change adds `draft-ietf-moq-transport-21` as a fourth selectable
`DraftVersion` alongside the existing drafts 14, 16, and 18, following the
same additive pattern used to add draft 18 (commit `818fd7d`). Drafts 14,
16, and 18 are unaffected: every draft-21 behavior change is gated on
`DraftVersion::kDraft21` and does not alter encode/decode paths, session
logic, or CLI behavior for other drafts.

Drafts 19 and 20 were never implemented in code (only reference text exists
under `docs/`) and remain out of scope; this is a fresh addition comparable
in size to the 16→18 jump, not an incremental delta on a draft-20
implementation.

Full compliance with the draft-21 wire format and session semantics is in
scope, covering every change listed below relative to draft-18 (the last
implemented draft), per the spec's own cumulative changelog (Appendix A,
since-18/-19/-20) — **scoped to the message types this publisher actually
originates or must parse**. This is a publisher-only implementation: GOAWAY
and the FETCH family (FETCH, FETCH_CANCEL, FETCH_OK, FETCH_ERROR) have no
structs or field-level encode/decode today, only opaque uint16-length-frame
byte-skipping in `next_control_message` (`src/transport/moqt_control_messages.cpp`).
There is likewise no `PUBLISH` struct — what this codebase calls
`PublishOk`/`PublishError`/`encode_publish_done_message` corresponds to
MOQT's SUBSCRIBE_OK-family (the publisher responding to an incoming
subscription), not an outbound PUBLISH message the publisher sends. Full
compliance for this work means: (a) for FETCH/GOAWAY, verifying the uint16
frame-length rule still matches the draft-21 wire layout for these types
and leaving them as opaque skip-only frames — no new structs, no field
decode; (b) for message types the publisher does originate or parse
(SETUP, SUBSCRIBE_TRACKS, PUBLISH_OK/PUBLISH_ERROR/PUBLISH_DONE,
REQUEST_UPDATE), implementing every draft-21 field/semantic change in full;
(c) PUBLISH_STATE_NOTIFY and the PUBLISH_BLOCKED→PUBLISH_SKIPPED rename are
implemented only if session logic must dispatch on them (i.e. the publisher
receives them), not as an outbound message the publisher constructs from
scratch with no caller.

## Normative Requirements

The local draft text (`docs/draft-ietf-moq-transport-21.txt`) is
authoritative. Relevant changes since draft-18:

**Negotiation**
- ALPN token for this draft is `moqt-21` (same "moqt-" + draft-number scheme
  already used for drafts 16/18); no new in-band SETUP version-number
  semantics apply (those existed only pre-draft-15).
- SETUP gains option `MAX_REQUEST_UPDATES` (Option Type `0x08`, varint
  value, §9.1.7): caps unacknowledged REQUEST_UPDATEs per request stream;
  0 or absent means unlimited. New session-termination code
  `TOO_MANY_REQUEST_UPDATES` (`0x1B`, §12.2) closes the session if that cap
  is exceeded.
- Session-termination code `0x15` (`VERSION_NEGOTIATION_FAILED` in
  draft-18) is unassigned in draft-21's error table (§12.2) — removed, not
  reused.

**Control messages — in scope (publisher originates or parses these today)**
- PUBLISH_OK (this codebase's `PublishOk`, wire types `0x1e`/aliased `0x07`
  REQUEST_OK): `GROUP_ORDER` (parameter `0x22`) is no longer in the
  parameter allow-list for REQUEST_OK/PUBLISH_OK (§9.3, §9.20.9) — it
  moved to the `PUBLISH` message (§9.8), which this publisher does not
  send. Decoding `GROUP_ORDER` on an incoming PUBLISH_OK-family message
  under draft-21 is therefore a protocol violation, not a value to store.
- SUBSCRIBE_TRACKS (wire type `0x51`): reuses the generic parameter
  registry (§9.20) rather than a dedicated table. `GROUP_ORDER` (`0x22`)
  and Range Filters (a `SUBSCRIPTION_FILTER`-style parameter, `0x21`,
  §3.3.2/§4.3/§8.6) are both valid on SUBSCRIBE_TRACKS in draft-21; this
  codebase's `decode_subscribe_tracks_message` currently only extracts
  the FORWARD parameter and silently skips every other parameter type
  (`src/transport/moqt_control_messages.cpp:1394-1412`) — this needs
  `kDraft21` handling for both.
- PUBLISH_DONE (wire type `0x0b`): status-code table (§12.4) no longer
  has `SUBSCRIPTION_ENDED`; max Stream Count sentinel is `2^64-1` (§9.9).
  This codebase's `encode_publish_done_message` already encodes
  `stream_count` as a full `moqint`/vi64 (not a fixed-width field) and
  takes `status_code` as a caller-supplied raw value with no internal
  enum — verify via test that `kDraft21`'s vi64 encoding round-trips
  `2^64-1`, and audit callers for any hardcoded status code matching
  draft-18's removed `SUBSCRIPTION_ENDED` value.
- REQUEST_UPDATE (wire type `0x02`): confirmed **no field-level change**.
  It already reuses the generic parameter registry, and this codebase's
  `RequestUpdateMessage`/`decode_request_update_message` already carry
  `object_delivery_timeout_ms`, `subgroup_delivery_timeout_ms`,
  `subscriber_priority`, `forward`, and `subscription_filter` — every
  parameter draft-21 allows on REQUEST_UPDATE (§9.20) is already handled.
  No task needed beyond adding `kDraft21` to the draft-iteration test
  lists that already exercise this decoder.

**Control messages — out of scope for field-level decode, framing-only**
- GOAWAY: Request ID field is removed in draft-21. The codebase has never
  decoded GOAWAY fields (only skips its uint16-length payload), so this
  change requires no code change beyond confirming the frame-length rule
  in `next_control_message` still applies to draft-21's GOAWAY layout.
- FETCH family (FETCH, FETCH_CANCEL, FETCH_OK, FETCH_ERROR): "Joining
  FETCH" removal, fill streams, `LOCATION_FILTER`, and `FILL_PARAMETERS`
  are all field-level FETCH changes. Since this publisher has never
  decoded FETCH fields (framing-only today), these remain framing-only
  under draft-21 — no new structs or encode/decode functions.
- PUBLISH (an inbound message from a subscriber-turned-publisher, not one
  this publisher sends): Subscription Parameters and dropped
  `AUTHORIZATION_TOKEN` copy-through are out of scope unless session
  logic is found to require dispatching on this message type for
  draft-21 (verify during Task decomposition in the implementation plan).
- PUBLISH_BLOCKED→PUBLISH_SKIPPED rename and PUBLISH_STATE_NOTIFY: added
  only if session dispatch logic must recognize these types under
  draft-21 (i.e. the publisher receives them from a peer); not
  constructed as new outbound messages with no existing caller.
- Editorial: message "Payload" field renamed "Message Body" — cosmetic,
  no wire impact, no code change.

**Data plane — confirmed not applicable to this codebase**
- OBJECT_DATAGRAM/SUBGROUP_HEADER type-flag bitfield validation
  (draft-21 §11.2/§11.3.1: invalid bit combinations are a
  `PROTOCOL_VIOLATION`) is a receiver-side obligation. This codebase has
  no `decode_subgroup_header`/`decode_object_datagram` function — it only
  encodes outbound headers/objects it sends itself (`encode_subgroup_header`,
  `encode_subgroup_object`), never decodes a peer's incoming header. No
  code path exists to add this validation to; not applicable.
- `OBJECT_DELIVERY_TIMEOUT` measured from the last header byte instead of
  the first payload byte (draft-21 §5.2) applies specifically to objects
  "received from the upstream subscription" (a relay forwarding case).
  This publisher only sends objects it produces itself; its existing
  `object_available_at` timestamp (`src/transport/moqt_session.cpp:3108`)
  already represents "provided by the original publisher application" —
  the other half of the same sentence, unaffected by this clarification.
  Not applicable; no code change.
- "End of Timed-Out Range" object status (fill-timeout expiry) and
  fill-vs-subscription scheduling precedence are both fill-stream/FETCH
  concepts; per the FETCH scope decision above, out of scope.

## Architecture

Extend the existing per-version branching pattern rather than introducing a
version-strategy abstraction. Two alternatives were considered and rejected:
a trait/strategy object to replace scattered `if/switch(draft)` sites (an
unrelated refactor of working code, risking drafts 16/18 regressions for no
requirement of this task), and a fully separate draft-21 code path with no
shared logic (needless duplication, since most fields are unchanged).

Within the existing pattern, `kDraft21` branches are added directly inside
the existing `encode_*`/`decode_*` functions for the in-scope messages
(`PublishOk`, `PublishError`, `encode_publish_done_message`,
`RequestUpdateMessage`, `SubscribeTracksMessage`, `SetupMessage`/
`ServerSetupMessage`), matching how `kDraft18` branches were added
alongside `kDraft14`/`kDraft16` in the same functions — no new files, no
new abstraction layer. FETCH/GOAWAY/PUBLISH stay untouched beyond
confirming their frame-length byte-skipping still matches draft-21 (see
Scope); no encode/decode functions are added for them.

## Components

- `include/openmoq/publisher/moq_draft.h`: add `kDraft21` to
  `enum class DraftVersion`.
- `src/moq_draft.cpp`: add `kDraft21` cases to `draft_profile()`,
  `to_string()`, `default_alpn()` (→ `"moqt-21"`).
- `include/openmoq/publisher/transport/moqt_control_messages.h` /
  `src/transport/moqt_control_messages.cpp`:
  - add `kDraft21Version` wire constant and a `kDraft21` case in
    `draft_version_number()` (symmetry/logging only — draft-21 negotiates
    via ALPN like draft-18, not an in-band SETUP version number);
  - extend `uses_moq_vi64()` to return true for `kDraft21` (draft-21 keeps
    the vi64 integer encoding introduced in draft-17/18);
  - add an optional `max_request_updates` field to `SetupMessage` and
    encode it as SETUP Option `0x08` (varint) in `encode_setup_message`'s
    vi64 branch when present and `draft == kDraft21`;
  - add a `kDraft21` case inside `decode_publish_ok`'s existing
    `kParamGroupOrder` parameter arm that rejects the parameter (returns
    `false`) instead of storing it — GROUP_ORDER is no longer valid on
    PUBLISH_OK/REQUEST_OK in draft-21;
  - `decode_subscribe_tracks_message`: relax the `draft != kDraft18` guard
    to also accept `kDraft21`, add a `group_order` field to
    `SubscribeTracksMessage`, and extend the parameter loop (currently
    only extracts FORWARD, silently skips everything else) to decode
    `kParamGroupOrder` and a Range Filter (`SUBSCRIPTION_FILTER`,
    parameter `0x21`, reusing the same `decode_subscribe_filter` helper
    `decode_publish_ok` already calls) under `kDraft21`;
  - `encode_publish_done_message`/`decode_publish_error`: no field
    changes needed (Stream Count is already a full vi64/moqint, not
    fixed-width); add a `kDraft21` round-trip test asserting
    `stream_count = 2^64-1` survives encode+decode, and audit existing
    callers of `encode_publish_done_message` for a hardcoded status code
    that matches draft-18's removed `SUBSCRIPTION_ENDED` value under
    `kDraft21`;
  - `decode_request_update_message`: no field changes — already carries
    every parameter draft-21 allows on REQUEST_UPDATE; only add `kDraft21`
    to the existing draft-iteration test lists;
  - add a `kDraft21` regression test confirming `next_control_message`
    still frames GOAWAY/FETCH/FETCH_CANCEL/FETCH_OK/FETCH_ERROR as opaque
    uint16-length payloads (pins the "framing-only" scope decision).
- `src/transport/peer_close.h`: `moqt_termination_code_name` needs a
  `DraftVersion` parameter (or an overload) so it can omit `0x15` →
  `VERSION_NEGOTIATION_FAILED` and add `0x1B` → `TOO_MANY_REQUEST_UPDATES`
  for `kDraft21` only, leaving drafts 14/16/18 unchanged.
- `src/transport/moqt_session.cpp`: no PUBLISH_BLOCKED/PUBLISH_SKIPPED or
  PUBLISH_STATE_NOTIFY dispatch case is added — confirmed neither is
  referenced anywhere in this file today, and neither has a message-type
  constant in `moqt_control_messages.cpp`; both stay out of scope per the
  Normative Requirements section above.
- `src/cli_options.cpp`: `parse_draft()` accepts `"21"`; default remains
  `kDraft16`; drafts 14/17/19/20 remain unselectable as today.
- `src/publisher_api.cpp`, `src/transport/libmoq_publisher.cpp`: add
  `kDraft21` arms to existing ALPN/draft checks.
- `README.md` (and translated variants `README.es.md`, `README.fr.md`,
  `README.it.md`, `README.ja.md`, `README.pt.md`, `README.zh.md`): update
  the line documenting which drafts the CLI accepts.
- `CMakeLists.txt`: no changes expected — the draft-18 addition extended
  existing files rather than adding new ones, and this work follows the
  same shape.

## Data Flow

Version selection is unchanged: the CLI's `--draft 21` flag fixes the ALPN
string used at connect time, which fixes which encode/decode branch every
control-message function takes for the life of the session. No
cross-version SETUP negotiation (client offering multiple versions, server
selecting one) exists today and none is added by this work.

## Error Handling

- `src/transport/peer_close.h`'s `moqt_termination_code_name` gains a
  `kDraft21` code path: `0x1B` → `"TOO_MANY_REQUEST_UPDATES"` (new in
  draft-21, paired with the `MAX_REQUEST_UPDATES` SETUP option); `0x15`
  returns unnamed/unknown for `kDraft21` instead of
  `"VERSION_NEGOTIATION_FAILED"` (that code is removed, not reused, in
  draft-21's error table). Drafts 14/16/18 keep their current mapping
  unchanged.
- `decode_publish_ok` rejects (returns `false` from) a `GROUP_ORDER`
  parameter under `kDraft21` instead of storing it, since the parameter
  is no longer valid on PUBLISH_OK/REQUEST_OK in this draft (moved to
  PUBLISH, out of scope — see Normative Requirements).

## Testing

Both test files use a hand-rolled `expect()`-accumulation harness, not
gtest (`tests/moqt_control_messages_test.cpp` has no `TEST(...)` macros;
existing coverage lives in functions like
`test_peer_control_message_decoders_for_all_drafts()` that loop
`for (DraftVersion draft : {kDraft14, kDraft16, kDraft18, ...})`). New
draft-21 coverage extends these loops and functions rather than adding
gtest fixtures.

- `tests/cli_options_test.cpp`: accept/reject matrix including `"21"`.
- `tests/moqt_control_messages_test.cpp`: add `kDraft21` to the existing
  draft-iteration lists; add assertions that `decode_publish_ok` rejects a
  `GROUP_ORDER` parameter under `kDraft21`; add `SubscribeTracksMessage`
  coverage for `GROUP_ORDER` and Range Filter decoding under `kDraft21`;
  add a Stream Count `2^64-1` round-trip test for
  `encode_publish_done_message`; add a SETUP round-trip test for the
  `MAX_REQUEST_UPDATES` option; add one test confirming
  `next_control_message` still frames GOAWAY/FETCH*/PUBLISH as opaque
  uint16-length payloads under `kDraft21` (pins the "stays framing-only"
  decision so a future change can't silently break it). `RequestUpdateMessage`
  needs no new assertions beyond appearing in the extended draft-iteration
  list, since no field changed.
- `tests/peer_close_test.cpp` (or wherever `moqt_termination_code_name` is
  tested today — verify exact file during Task decomposition): `0x1B`
  maps to `TOO_MANY_REQUEST_UPDATES` and `0x15` maps to unknown/empty
  under `kDraft21`, while drafts 14/16/18 are unchanged.

Existing draft-14/16/18 tests must continue to pass unmodified except where
a shared helper's signature gains the new draft as an argument (matching
how the draft-18 addition touched shared test helpers).
