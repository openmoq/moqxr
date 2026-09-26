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
since-18/-19/-20).

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

**Control messages**
- FETCH: the "Joining FETCH" variant is removed. Fetch is restructured
  around fill streams; range is now carried in a new `LOCATION_FILTER`
  parameter instead of message fields, plus a new `FILL_PARAMETERS`
  parameter.
- SUBSCRIBE_TRACKS: gains Range Filters; gains `GROUP_ORDER` (moved here
  from PUBLISH_OK); multiple concurrent subscriptions per track are now
  permitted.
- PUBLISH: may carry Subscription Parameters; no longer copies
  `AUTHORIZATION_TOKEN` from SUBSCRIBE_TRACKS.
- PUBLISH_OK: loses `GROUP_ORDER` (moved to SUBSCRIBE_TRACKS) and
  subscription parameters (moved to REQUEST_UPDATE).
- PUBLISH_BLOCKED is renamed `PUBLISH_SKIPPED` (message identifier and
  semantics change).
- PUBLISH_DONE: removes the `SUBSCRIPTION_ENDED` status code; max Stream
  Count becomes 2^64-1.
- REQUEST_UPDATE: gains subscription parameters (moved from PUBLISH_OK); an
  unexpected REQUEST_UPDATE is now a session-terminating error.
- GOAWAY: Request ID field is removed.
- PUBLISH_STATE_NOTIFY: new message type, no draft-18 analog.
- Editorial: message "Payload" field is renamed "Message Body" — cosmetic,
  no wire impact.

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

Within the existing pattern, messages that changed structurally (FETCH,
PUBLISH, PUBLISH_OK, PUBLISH_DONE, REQUEST_UPDATE, GOAWAY, and the new
PUBLISH_STATE_NOTIFY) get dedicated encode/decode helper functions selected
by draft, rather than deeper `if` nesting inside the existing large
functions. Messages with only additive/no changes keep their current
single-function-with-branch shape.

## Components

- `include/openmoq/publisher/moq_draft.h`: add `kDraft21` to
  `enum class DraftVersion`.
- `src/moq_draft.cpp`: add `kDraft21` cases to `draft_profile()`,
  `to_string()`, `default_alpn()` (→ `"moqt-21"`).
- `include/openmoq/publisher/transport/moqt_control_messages.h` /
  `src/transport/moqt_control_messages.cpp`:
  - add `kDraft21Version` wire constant and `draft_version_number()` case;
  - add `kDraft21` branches to `encode_client_setup_message`,
    `decode_server_setup_message` for `MAX_REQUEST_UPDATES` and the removal
    of `VERSION_NEGOTIATION_FAILED`;
  - new encode/decode functions for FETCH (fill streams, `LOCATION_FILTER`,
    `FILL_PARAMETERS`), PUBLISH, PUBLISH_OK, PUBLISH_DONE, REQUEST_UPDATE,
    GOAWAY (drop Request ID), and PUBLISH_SKIPPED (renamed from
    PUBLISH_BLOCKED);
  - new PUBLISH_STATE_NOTIFY message struct plus encode/decode functions.
- `src/transport/moqt_session.cpp`: `kDraft21` arms wherever session logic
  branches on draft (request handling, scheduling, timeout classification,
  message dispatch), plus the new session-terminating error path for a
  stray REQUEST_UPDATE and the new PUBLISH_STATE_NOTIFY dispatch case.
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

Mirror the three test files touched by the draft-18 addition:

- `tests/cli_options_test.cpp`: accept/reject matrix including `"21"`.
- `tests/moqt_control_messages_test.cpp`: encode/decode round-trip coverage
  for every changed or new message (FETCH fill streams, PUBLISH,
  PUBLISH_OK, PUBLISH_DONE, REQUEST_UPDATE, GOAWAY, PUBLISH_SKIPPED,
  PUBLISH_STATE_NOTIFY), plus SETUP with `MAX_REQUEST_UPDATES`.
- `tests/moqt_session_test.cpp`: fill-stream FETCH flow, REQUEST_UPDATE
  parameter relocation, GOAWAY without Request ID, session error on a
  stray REQUEST_UPDATE, PUBLISH_STATE_NOTIFY dispatch, and the
  `PROTOCOL_VIOLATION` on an unspecified type-flag bit.

Existing draft-14/16/18 tests must continue to pass unmodified except where
a shared helper's signature gains the new draft as an argument (matching
how the draft-18 addition touched shared test helpers).
