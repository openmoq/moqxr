# WebTransport Compliance Notes

This note is the guardrail for MoQ over WebTransport work in this tree.
It is intentionally limited to the draft variants we support today:

- MoQ Transport draft 14
- MoQ Transport draft 16
- MoQ Transport draft 17
- MoQ Transport draft 18
- WebTransport over HTTP/3 draft 14

## Normative Model

### 1. Transport split

There are two distinct connection models:

- Native QUIC MoQ
- MoQ over WebTransport over HTTP/3

They are not interchangeable at session setup time.

For WebTransport:

- QUIC ALPN is `h3`
- session establishment uses HTTPS extended CONNECT
- the WebTransport resource is identified by `authority` and `path`
- MoQ draft/version negotiation happens after CONNECT

For native QUIC:

- QUIC ALPN identifies the MoQ draft directly
- `authority` and `path` are carried in the MoQ SETUP parameters

### 2. WebTransport CONNECT requirements

For WebTransport over HTTP/3:

- `:scheme` must be `https`
- `:protocol` must be `webtransport`
- the URI authority and path identify the session
- the connection requires the H3/WebTransport settings and transport parameters

In this codebase, picoquic's `picowt_prepare_client_cnx()` already configures the required transport parameters and H3 callback path.

### 3. WT protocol negotiation

MoQ-over-WebTransport draft selection is not QUIC ALPN.  QUIC ALPN is
`h3`; the MoQ application protocol identifier is advertised on the
WebTransport CONNECT exchange and the MoQ wire version is then confirmed by
the MoQ setup flow.

It is carried in:

- `WT-Available-Protocols`
- `WT-Protocol`

These are Structured Fields strings on the CONNECT exchange.  The
`WT-Available-Protocols` header value is therefore a structured string list:
send `"moqt-16"`, not bare `moqt-16`.  The WebTransport draft allows a server
to omit `WT-Protocol`, but if it sends one it must choose a value from the
client's offer.  MoQ Transport draft-18 still expects clients to include MOQT
protocol identifiers in `WT-Available-Protocols`.

Current offers in this repo:

- draft 14: offer no WebTransport subprotocol for legacy compatibility
- draft 16: offer `"moqt-16"` only in `WT-Available-Protocols`
- draft 17: offer `"moqt-17"` only in `WT-Available-Protocols`
- draft 18: offer `"moqt-18"` only in `WT-Available-Protocols`

Red5's current picoquic/h3zero server path no longer exposes
`WT-Available-Protocols` to the application on CONNECT accept.  Red5 therefore
accepts the WebTransport session without returning draft-specific
`WT-Protocol`, and draft selection proceeds through MoQ setup.  Treat this as
an interop compatibility mode, not proof that CONNECT-level subprotocol
selection works.

### 4. MoQ SETUP parameters over WebTransport

For WebTransport:

- `AUTHORITY` must not appear in MoQ SETUP
- `PATH` must not appear in MoQ SETUP

Those parameters are only for native QUIC mode.

### 5. WebTransport application stream format

WebTransport bidi application streams are not raw application bytes on the wire.

Each application bidi stream begins with:

- signal value `0x41`
- Session ID varint
- application payload

The application should only see the stream body after the stack strips the WT preamble.

The CONNECT stream is different:

- it is the HTTP/3 stream that established the WT session
- it can carry capsule traffic
- it is not the MoQ control stream

## What picoquic already does for us

The current picoquic WebTransport helpers cover a large part of the protocol surface:

- `picowt_prepare_client_cnx()` sets the H3 callback and WT transport parameters
- `h3zero` emits H3 settings including CONNECT protocol, H3 datagram, and WT session settings
- `picowt_set_control_stream()` creates the CONNECT stream context
- `picowt_connect()` formats the CONNECT request
- `picowt_create_local_stream()` creates WT app streams and writes the required preamble

That means this client should not manually reproduce those pieces unless there is a verified picoquic bug.

## Current code status

Areas that are aligned with the drafts:

- WT mode uses QUIC ALPN `h3` by default
- CONNECT uses the endpoint `authority` and `path`
- WT protocol offer is sent separately from QUIC ALPN
- MoQ setup omits `AUTHORITY` and `PATH` in WT mode: `CLIENT_SETUP` for
  draft-14/draft-16, `SETUP` for draft-18
- local WT app streams are created through `picowt_create_local_stream()`
- WebTransport app-stream writes use picoquic's callback-driven provide-data path instead of direct stream injection

Areas that should be treated as suspect until proven:

- anything that interprets bytes arriving on the CONNECT stream as MoQ data
- any logic that tries to recreate WT app-stream context or preamble outside picoquic helpers
- any assumptions that the first MoQ response can arrive on the CONNECT stream

## Debugging rules

Before changing wire behavior, verify each of these:

1. CONNECT succeeded and the WebTransport protocol offer matches the intended draft:
   draft-14 offers no WebTransport subprotocol, draft-16 offers `"moqt-16"`,
   draft-17 offers `"moqt-17"`, and draft-18 offers `"moqt-18"` as structured
   `WT-Available-Protocols` strings.  If the relay omits `WT-Protocol`, verify
   the negotiated MoQ setup version before treating the session as draft-specific.
2. The first client MoQ bytes are sent only on a WT application stream, not the CONNECT stream.
3. The WT application stream gets exactly one WT preamble.
4. Incoming CONNECT-stream bytes are logged as WT control/capsule traffic, not parsed as MoQ.
5. Incoming application-stream bytes are delivered without the WT preamble before MoQ parsing.
6. Draft-14, draft-16, and draft-18 transport sessions are tested separately; draft-17 control-codec coverage is maintained alongside them.

## Current risk focus

The current WebTransport path is interoperating with the tested moqx and Red5
draft-16/draft-18 relay paths. Draft-17 is selectable and codec-tested, but has
less relay coverage. Remaining risk is now concentrated in broader
coverage rather than initial session establishment:

- higher object volume and backpressure behavior
- live subscriber timing across multiple tracks
- relay-specific resource paths
- draft-specific control message drift as the MOQT drafts continue to change

## Current interoperability state

Observed behavior as of May 16, 2026, with the Red5 relay result refreshed on
September 4, 2026:

- `<moqx-la-relay-host>:4433/moq-relay`
  - draft-16 CONNECT succeeds with verified TLS
  - `PUBLISH_NAMESPACE_OK` arrives on the WT control stream as expected
  - with `--forward 0`, the relay may remain idle afterward until a downstream subscriber appears
- `<moqx-ord-relay-host>:4433/moq-relay`
  - draft-16 CONNECT succeeds with verified TLS
  - `PUBLISH_NAMESPACE_OK` arrives on the WT control stream as expected
  - with `--forward 0`, the relay may remain idle afterward until a downstream subscriber appears
- `moq-relay.red5.net:4433/moq`
  - draft-16 CONNECT succeeds with verified TLS
  - `PUBLISH_NAMESPACE_OK` arrives on the WT control stream as expected
  - a live `SUBSCRIBE` for `catalog` was observed and served successfully
  - draft-14 and draft-18 psychedelic publisher flows have both connected and
    published against the Red5 relay during local interop checks
  - a September 4 draft-18 CTE LL-DASH run published namespace `live/dasher`
    from simulated FFmpeg input with two H.264 representations and AAC audio
  - FFmpeg opened `/ingest/video0`, `/ingest/video1`, and `/ingest/video2`; the
    publisher received the catalog subscription and emitted more than 2,000
    media objects across 12 shared media groups without a transport or protocol
    error
- `moq-relay.red5.net:4433/moq-relay`
  - WebTransport CONNECT is rejected with HTTP `404`
  - use `/moq` for the Red5 relay on port 4433
- Historical: `draft-14.cloudflare.mediaoverquic.com:443/moq`
  - CONNECT succeeds
  - `SERVER_SETUP` arrives on the first WT bidi application stream
  - `PUBLISH_NAMESPACE_OK` arrives
  - the relay may remain idle afterward until a downstream subscriber appears
- Historical: `fb.mvfst.net:9448`
  - the tested resource paths still return HTTP `404` during CONNECT
  - that is currently treated as a server resource-path issue, not a post-CONNECT MoQ framing issue

## Idle subscriber behavior

For `--forward 0`, once `PUBLISH_NAMESPACE_OK` has been received:

- the publisher waits up to `--timeout` seconds for downstream `SUBSCRIBE`
- if no `SUBSCRIBE` arrives before that timeout, the session is treated as idle rather than failed
- the publisher emits `PUBLISH_NAMESPACE_DONE` and exits successfully

This behavior is intentional. It keeps interoperability probes from being reported as publish failures when the relay accepted the namespace but no subscriber appeared during the configured wait window.

Automated regressions cover the reported live DASH shape with catalog plus
multiple representation paths. They verify shared audio/video keyframe groups,
explicit `trun` sample metadata derived from FFmpeg `tfhd` defaults, subgroup
stream reuse within a group, and stream closure at a group transition. The
draft-16 session test also verifies that source objects are not consumed before
`SUBSCRIBE`, that `SUBSCRIBE_OK` is returned, and that the catalog remains
available while waiting for media interest. Draft-18 session coverage
separately verifies that a `SUBSCRIBE` split across multiple request-stream
reads is reassembled and answered on that request stream.
