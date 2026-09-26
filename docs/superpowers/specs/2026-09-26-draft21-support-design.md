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
- SETUP option list gains `MAX_REQUEST_UPDATES`; new error code
  `TOO_MANY_REQUEST_UPDATES`.
- `VERSION_NEGOTIATION_FAILED` session error is removed for this draft.

**Control messages — in scope (publisher originates or parses these today)**
- SUBSCRIBE_TRACKS: gains Range Filters; gains `GROUP_ORDER` (moved here
  from PUBLISH_OK); multiple concurrent subscriptions per track are now
  permitted.
- PUBLISH_OK: loses `GROUP_ORDER` (moved to SUBSCRIBE_TRACKS) and
  subscription parameters (moved to REQUEST_UPDATE).
- PUBLISH_DONE: removes the `SUBSCRIPTION_ENDED` status code; max Stream
  Count becomes 2^64-1.
- REQUEST_UPDATE: gains subscription parameters (moved from PUBLISH_OK); an
  unexpected REQUEST_UPDATE is now a session-terminating error.

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

**Data plane**
- OBJECT_DATAGRAM / SUBGROUP_HEADER type flags are formally bitfields; an
  unspecified set bit is a `PROTOCOL_VIOLATION`.
- New "End of Timed-Out Range" object status for fill-timeout expiry.
- `OBJECT_DELIVERY_TIMEOUT` is measured from the last header byte instead of
  the first payload byte.
- Explicit scheduling precedence between fill-delivered and
  subscription-delivered objects; datagrams win scheduling ties.

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
  - add `kDraft21Version` wire constant (`kDraft21Version = 0xff000015ULL`,
    following the `0xff0000XX` pattern, though draft-21 negotiates via ALPN
    like draft-18 — the constant exists for symmetry/logging, not in-band
    SETUP negotiation) and a `kDraft21` case in `draft_version_number()`;
  - extend `uses_moq_vi64()` to return true for `kDraft21` (draft-21 keeps
    the vi64 integer encoding introduced in draft-17/18);
  - add `MAX_REQUEST_UPDATES` SETUP option handling in `encode_setup_message`
    / `decode_server_setup_message` / `encode_server_setup_message`, and
    stop emitting `VERSION_NEGOTIATION_FAILED` under `kDraft21`;
  - add `kDraft21` branches inside `decode_publish_ok`, `decode_publish_error`,
    `encode_publish_done_message` (drop `SUBSCRIPTION_ENDED` status code,
    widen max Stream Count), `decode_subscribe_tracks_message` (Range
    Filters, `GROUP_ORDER` parameter), and `decode_request_update_message`
    (subscription-parameter fields moved from `PublishOk`);
  - move `group_order` out of `PublishOk` and subscription-filter/parameter
    fields into `RequestUpdateMessage` for `kDraft21` only — both structs
    gain fields conditionally meaningful per-draft (existing drafts keep
    their current field usage; add a code comment explaining the
    per-draft field-ownership split, following the existing comment style
    on `RequestUpdateMessage` at moqt_control_messages.h:117-119);
  - confirm (add a regression test, not new code) that `next_control_message`
    still correctly frames GOAWAY/FETCH/FETCH_CANCEL/FETCH_OK/FETCH_ERROR
    as opaque uint16-length payloads under `kDraft21`.
- `src/transport/moqt_session.cpp`: `kDraft21` arms wherever session logic
  branches on draft (request handling, scheduling, timeout classification,
  message dispatch), plus the new session-terminating error path for a
  stray REQUEST_UPDATE. PUBLISH_BLOCKED/PUBLISH_SKIPPED and
  PUBLISH_STATE_NOTIFY are not currently referenced anywhere in this file
  (confirmed: no `PUBLISH_BLOCKED` hits, and no message-type constant for
  either exists in `moqt_control_messages.cpp`) — treat both the same as
  FETCH/GOAWAY (framing-only, no new dispatch case) unless a task
  discovers the publisher must actually recognize one of them for
  draft-21 session correctness.
- `src/cli_options.cpp`: `parse_draft()` accepts `"21"`; default remains
  `kDraft16`; drafts 14/17/19/20 remain unselectable as today.
- `src/publisher_api.cpp`, `src/transport/libmoq_publisher.cpp`: add
  `kDraft21` arms to existing ALPN/draft checks.
- `CMakeLists.txt`: add any new source files for the new message
  encode/decode helpers, mirroring the draft-18 addition.
- `README.md` (and translated variants `README.es.md`, `README.fr.md`,
  `README.it.md`, `README.ja.md`, `README.pt.md`, `README.zh.md`): update
  the line documenting which drafts the CLI accepts.

## Data Flow

Version selection is unchanged: the CLI's `--draft 21` flag fixes the ALPN
string used at connect time, which fixes which encode/decode branch every
control-message function takes for the life of the session. No
cross-version SETUP negotiation (client offering multiple versions, server
selecting one) exists today and none is added by this work.

## Error Handling

- Add `TOO_MANY_REQUEST_UPDATES` error code (paired with
  `MAX_REQUEST_UPDATES` SETUP option).
- Remove `VERSION_NEGOTIATION_FAILED` from the draft-21 error-code set only;
  drafts 14/16/18 keep it.
- An unexpected REQUEST_UPDATE becomes a session-terminating error under
  draft-21 only.
- An OBJECT_DATAGRAM/SUBGROUP_HEADER type-flag bit outside the defined set
  is a `PROTOCOL_VIOLATION` under draft-21.

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
  draft-iteration lists and extend the existing round-trip assertions for
  `PublishOk`/`PublishError`/`encode_publish_done_message` (dropped
  `SUBSCRIPTION_ENDED`, widened Stream Count), `SubscribeTracksMessage`
  (Range Filters, `GROUP_ORDER`), `RequestUpdateMessage` (relocated
  subscription-parameter fields), and SETUP (`MAX_REQUEST_UPDATES`
  option). Add one test confirming `next_control_message` still frames
  GOAWAY/FETCH*/PUBLISH as opaque uint16-length payloads under
  `kDraft21` (no new struct needed — this pins the "stays framing-only"
  decision so a future change can't silently break it).
- `tests/moqt_session_test.cpp`: REQUEST_UPDATE parameter relocation,
  session error on a stray REQUEST_UPDATE, and the `PROTOCOL_VIOLATION`
  on an unspecified OBJECT_DATAGRAM/SUBGROUP_HEADER type-flag bit.

Existing draft-14/16/18 tests must continue to pass unmodified except where
a shared helper's signature gains the new draft as an argument (matching
how the draft-18 addition touched shared test helpers).
