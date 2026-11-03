# Draft-23 Implementation Plan

This plan tracks adding `draft-ietf-moq-transport-23` support to `moqxr`, and making it the default draft.

Reference: [draft-ietf-moq-transport-23.txt](draft-ietf-moq-transport-23.txt) (2 November 2026).

## Scope for a publisher

moqxr only publishes, so only part of the d22 → d23 diff reaches it.

### In scope
- **Control message Length is a vi64** in every message, SETUP included (§9).
  - Before d23 the Length stays 16 bits.
  - An encoder whose payload doesn't fit in 16 bits fails; it no longer truncates.
- **MAX_CONTROL_MESSAGE_SIZE**, Setup Option 0x09 (§9.1.8).
  - The peer's value is parsed; a value below 65535 is an error.
  - moqxr advertises nothing, so its own limit is the 65535 default.
- **FORWARD (0x10) → PAUSED (0x11)**, with the value inverted (§9.19.18).
  - PAUSED is a bare uint8 even though its type is odd.
- **FILL_PARAMETERS (0x23)** is a parameter count followed by the parameters (§9.19.15).
- **NAMESPACE** gains a Parameters block after the suffix (§9.15). moqxr always sends `count = 0`.
- **REQUEST_ERROR**: the inline Redirect is replaced by Error Details, Key-Value-Pairs to the end of the message (§9.4.2).
- **Overlap allowed**: overlapping SUBSCRIBE_NAMESPACE and SUBSCRIBE_TRACKS are accepted (§3.6, §4.2).
  - PREFIX_OVERLAP 0x30 no longer exists.
  - Each matching SUBSCRIBE_TRACKS gets its own PUBLISH, carrying SUBSCRIBE_TRACKS_ID 0x36.
- **SUBSCRIBE_TRACKS** has an explicit parameter list (§9.17).
- **GOAWAY**: only the receiver of a request may send GOAWAY on its stream (§9.2).
- **Empty Track Names** are a PROTOCOL_VIOLATION (§2.4.1).
- **New session close codes** (§12.2): the ones the session can identify without decoder changes.

### Unchanged for moqxr
- **FETCH and TRACK_STATUS** are still answered with REQUEST_ERROR NOT_SUPPORTED. That means FETCH_OK's removal, the FETCH stream re-encoding, Gone and End of Group/Track never come into play.
- **The failed fill stream** (FETCH_HEADER, then a reset) is unchanged.
- **PUBLISH_DONE EXPIRED_AUTH_TOKEN** is unused: moqxr has no token-expiry path.

## Draft predicates
- `is_draft23_or_later` (d23).
- `is_draft22_or_later` (d22, d23): GOAWAY migration, retained discovery streams, NAMESPACE.
- `is_draft21_or_later` (d21+).

The d22 PREFIX_OVERLAP rules stay exact `== kDraft22` checks.

## Known limitations
- Session close codes come from the session's own checks. Decoders still report only success or failure, so a malformed message closes with PROTOCOL_VIOLATION. d23 allows that when no more specific code applies.
- The client offers one ALPN, chosen with `--draft`. A d23 publisher can't fall back to an older draft on a relay that doesn't speak d23.
