# Project Status and Roadmap

## Current Status

Current transport and packaging work is past the initial prototype stage. The publisher can generate publish plans, publish over picoquic-backed Raw QUIC and WebTransport paths, and exercise the supported MOQT draft profiles.

Draft status:

- draft 22 is the default; draft 23 is the newest profile and is selectable with `--draft 23`
- draft 14 is retained for API callers and draft 16 is maintained as a compatibility profile
- draft 17 is retained for API callers and the MSFTS example (the main CLI rejects `--draft 17`) and has VI64, request-stream, control-codec, ALPN, and WebTransport protocol-offer support; relay interop coverage remains limited
- WebTransport publishing to moqx is blocked: moqx omits the `reset_stream_at` transport parameter that WebTransport over HTTP/3 requires of servers, so picoquic withholds the CONNECT and the publisher names the missing parameter ([openmoq/moqx#752](https://github.com/openmoq/moqx/issues/752)) unless `--webtransport-allow-missing-reset-stream-at` is given; raw QUIC to moqx and WebTransport to Red5 are unaffected. A local moqx 0.3.5-99 build advertised the parameter and accepted draft-16 WebTransport publishes (the opt-out was not exercised), so newer builds may no longer be affected
- draft 18 support includes version selection, setup/request framing, request-stream response correlation, fragmented subscriber-interest reads, and same-stream `SUBSCRIBE_OK` responses
- draft 21 (`--draft 21`, native backend only) runs on the draft-18 request-stream model and adds LOCATION_FILTER, INCLUDE_PROPERTIES, FILL_PARAMETERS, and TOO_MANY_REQUEST_UPDATES handling. A subscription is not ended at the end of its LOCATION_FILTER (draft-21 has no SUBSCRIPTION_ENDED, #1833): plan publishing keeps it until the track's last object is available, never while the plan loops, and then ends it with PUBLISH_DONE TRACK_ENDED. Draft 21 is verified end to end over WebTransport through red5-moq-relay 1.8.0 (`moqt-21`), which relays it to a draft-18 moq-playa subscriber. Known limitations:
  - a LOCATION_FILTER relative start of one or more groups starts at the next object, because live delivery keeps no history
  - fill fetch streams always fail: a REQUEST_UPDATE ack that reports LARGEST_OBJECT with Forward=1 opens a stream, writes FETCH_HEADER, and resets it with INTERNAL_ERROR. The reset uses RESET_STREAM, not RESET_STREAM_AT, so the header may not reach the subscriber. A fill stream that cannot be opened is logged and the subscription continues. The exception is a live (stdin or SRT) catalog SUBSCRIBE whose fill range includes the current catalog object: on drafts 21 and 22 that object is sent on the fill fetch stream, which ends with FIN
  - SUBSCRIBE_OK reports LARGEST_OBJECT from draft 18 on, as the largest object the publisher has produced on the track. The batch and `publish_live_objects()` paths omit it for the catalog track, because they re-send the retained catalog at its existing Location. `publish_live()` (stdin and SRT) reports it, because it sends a later catalog subscriber a new catalog group. A SUBSCRIBE with FILL_PARAMETERS on a non-catalog track still gets the open-then-reset fill failure once anything was sent. Drafts 14 and 16 still omit LARGEST_OBJECT from SUBSCRIBE_OK. TRACK_STATUS is answered on drafts 18 and later: a published track gets TRACK_STATUS_OK (a REQUEST_OK carrying LARGEST_OBJECT once it has objects, no Track Properties) and an unknown track gets DOES_NOT_EXIST, each followed by a FIN. On draft 18 a Joining FETCH (relative or absolute) of a subscription to the catalog track is served with FETCH_OK and the retained catalog object on a FETCH_HEADER stream, even when that subscription has already completed; a standalone FETCH, a Joining FETCH of any other track and FETCH on drafts 21 and later get NOT_SUPPORTED (drafts 21 and later cover the late catalog subscriber with the fill), an unknown Joining Request ID gets INVALID_JOINING_REQUEST_ID, and a Joining FETCH before any catalog exists gets INVALID_RANGE. On draft 16, FETCH and TRACK_STATUS on the control stream get NOT_SUPPORTED instead of being skipped
  - on draft 18, the batch path answers REQUEST_UPDATE with the track's final Largest Object from the publish plan, and only when the update extends the end, while its SUBSCRIBE_OK reports the largest object already sent; the two can differ for one track. The live paths answer both from the same record
  - a STOP_SENDING on the request stream of a PUBLISH-initiated subscription ends it (draft 22 sections 3.1 and 3.1.2, draft 23 the same): the PUBLISH's Forward State stops counting, the track's open subgroup streams are reset with CANCELLED unless another subscription forwards it, and no PUBLISH_DONE is sent. This holds on the batch, stdin, SRT and publish_live_objects paths. Only publish_live_objects has a test, because the others take objects from a reader thread
  - a write picoquic refuses on a stream the peer stopped (STOP_SENDING), that was reset, or that is already released is dropped, not retried, on both the raw QUIC and WebTransport clients (a retry could never succeed and held back every later control write, so responses stopped for the rest of the session, #125). Other refusals are still treated as back-pressure and deferred. The drop is traced on both clients
  - the moq5 backend ignores the retry resume state. A publisher that restarts a Track from Group 0 risks reusing Locations of Objects it already published (draft 22 section 2.5); resuming above the used Groups needs a starting Group ID in the moq5 API. On draft 22 the moq5 sender starts its Groups at the wallclock milliseconds, which is above a previous instance's only while the clock moves forward and no instance numbers Groups faster than it
  - SUBSCRIBE_TRACKS with a LOCATION_FILTER other than Next Object is declined with REQUEST_ERROR NOT_SUPPORTED; its GROUP_ORDER is validated but not applied
  - range filters (0x25-0x29) are rejected because MAX_FILTER_RANGES is never advertised
- draft 22 (`--draft 22`) is available on the native and libmoq backends. The native implementation keeps the draft-21 publishing semantics, with explicit LOCATION_FILTER types, GOAWAY migration with resume above the groups already sent, NAMESPACE notifications on SUBSCRIBE_NAMESPACE, and PREFIX_OVERLAP rejection of overlapping namespace and track subscriptions. Libmoq builds route draft 22 through moq5 unless a custom TransportFactory or caller-supplied catalog selects the native path; that backend has unit-test and closed-port coverage but has not been verified against a draft-22 relay
- draft 23 (`--draft 23`, native backend only) keeps the draft-22 behaviour on the draft-23 wire format: a varint control message Length, PAUSED in place of FORWARD, count-prefixed FILL_PARAMETERS, NAMESPACE Parameters, REQUEST_ERROR Error Details, SUBSCRIBE_TRACKS_ID on the PUBLISHes that answer a SUBSCRIBE_TRACKS, and the draft-23 session termination codes where the session identifies the condition. See [draft23-implementation-plan.md](draft23-implementation-plan.md). Known limitations:
  - an overlapping SUBSCRIBE_TRACKS is declined with REQUEST_ERROR NOT_SUPPORTED instead of getting a second set of PUBLISHes, because each track has one Track Alias and one set of data streams; overlapping SUBSCRIBE_NAMESPACE is accepted
  - control messages are limited to 65535 bytes: moqxr never advertises a larger MAX_CONTROL_MESSAGE_SIZE and never sends past the default
  - malformed input closes the session with the code section 12.2 names (INVALID_PARAMETER, INVALID_TRACK_NAMESPACE, MALFORMED_KEY_VALUE, MALFORMED_MESSAGE, INVALID_FIELD, ROLE_VIOLATION, the two overflow codes, and the data-plane codes for datagrams and subgroup headers); PROTOCOL_VIOLATION remains for a failure the decoder cannot attribute, such as a message cut short at an unnamed field
  - a second answer to a PUBLISH or PUBLISH_NAMESPACE, a REQUEST_UPDATE from the receiving side, and any other message that does not belong on a request stream close with UNEXPECTED_MESSAGE; PUBLISH_DONE finishes the request stream with a FIN
  - PUBLISH does not carry LARGEST_OBJECT, as on earlier drafts
  - not yet verified against a draft-23 relay
- drafts 19 and 20 are archived under `docs/` for later review and are not selectable
- CTE LL-DASH regressions cover FFmpeg-style multi-representation paths and draft-16 await-subscribe delivery
- `--moq-impl-id` sends the MOQT_IMPLEMENTATION Setup Option (`openmoq-publisher/<version>`) in SETUP on every draft (Setup Option 0x07; draft 14 text assigns 0x05 to it, which draft 15 corrected). The drafts say SHOULD, so it is a candidate to become the default; it is off for now because the version string can fingerprint the endpoint (draft 23 section 15.8, Implementation Identification Fingerprinting)
- the main CLI supports ordered relay failover through repeated `--endpoint` values and `--retry N` same-endpoint retries; transport/connectivity failures are retryable, relay namespace/track rejections skip directly to the next endpoint, and fatal or cancelled work stops

## LOC Status

The native backend implements the clear H.264/AAC subset of LOC-05 with
`--packaging loc --draft 18`. CMAF remains the primary default and LOCMAF keeps
its separate packaging path. LOC objects carry source presentation timing,
timescale, frame marking, codec extradata, and the codec string; video uses
GOP groups and audio uses one sample per group. Unsupported input and
configuration changes fail explicitly. The libmoq backend does not publish this LOC profile.

Independent offline validation covers 217 encoded samples and decoded-frame
equivalence, alongside property-wire and live-ingest regressions. Real relay
forwarding, cached FETCH, and LOC-05 browser playback remain unverified; the
reference Playa LOC decoder still uses LOC-01 IDs. See the
[quickstart](quickstart.md#opt-in-to-loc) and [tests](testing.md#loc-05-tests).

## LOCMAF Status

LOCMAF-01, packaging version `0.3`, is implemented as an opt-in addition;
CMAF remains the primary default. File, live stdin, SRT, and CTE DASH input
use the default backend's conversion path. Published objects use independent
full headers; automatic delta-header publication and libmoq LOCMAF publishing
are not enabled. Multiplexed fragments require demuxing first.

Golden-vector and offline moq-playa reconstruction checks pass. Relay-to-player
LOCMAF delivery remains to be validated. See the [quickstart](quickstart.md#opt-in-to-locmaf),
[protocol details](protocol-mapping.md#locmaf-packaging), and
[validation record](testing.md#locmaf-tests).

## Roadmap

1. Keep draft-14, draft-16, draft-17, and draft-18 message/subgroup serde tests aligned with the current relay matrix so wire placement regressions are caught in CI.
2. Continue draft-17/18 request-stream interop validation, especially stream retention, close/reset behavior, fragmented delivery, and relay-specific response timing.
3. Continue Red5 relay and player interop validation, including catalog publication, downstream subscription discovery, and Red5 Pro playback behavior.
4. Expand CMAF packaging coverage for fragmented and progressive inputs, including Opus, AAC, H.264, HEVC, edit lists, and less common sample-table layouts.
5. MSF/CMSF version 1: the catalog model conforms to `draft-ietf-moq-msf-01`
   and `draft-ietf-moq-cmsf-01`. Phase 2, catalog track lifecycle, has
   shipped: live-by-default catalogs with an explicit VOD opt-in, end-of-
   broadcast signalling per MSF section 11.3 (`isComplete` for a terminated
   broadcast, `isLive: false` with `trackDuration` for a VOD conversion),
   optional periodic republication of an independent catalog (disabled by
   default, see `PublisherConfig::catalog_republish_interval`), and `add`/
   `remove` delta updates bounded by a configurable max-deltas-per-group.
   **Scope qualifier:** all three of those (end-of-broadcast signalling,
   periodic republication, and delta updates) are wired only inside
   `MoqtSession`'s two `publish_live()` overloads (the SRT-ingest and
   stdin-ingest live paths) -- they are the only callers that populate the
   session's `CatalogPublisher`. `publish_live_objects()`, also a live
   streaming entry point, and the batch `publish()` plan path do not use
   `CatalogPublisher` at all today: a broadcast run through either of those
   still gets a static, one-shot catalog with none of Phase 2's lifecycle
   behavior, and `end_broadcast()` correctly no-ops its final-catalog write
   for them rather than guess at scope it does not have.
   **Delta updates specifically do not fire on the wire today, on either
   `publish_live()` path:** `add`/`remove` delta support is fully implemented
   and unit-tested in `CatalogPublisher::publish()`, but both call sites
   build one `LiveCatalog`/`MsfCatalog` snapshot up front and pass that same
   immutable value to `catalog_publisher_.publish()` on every SUBSCRIBE and
   republish tick. Nothing in the live-ingest paths ever mutates it after
   startup, so `catalogs_equal()` matches on every call after the first and
   `publish()` always returns an empty vector -- never a delta. Reaching a
   delta on the wire needs a live producer that detects a track add/remove
   mid-broadcast (e.g. SRT ingest noticing a track appear or disappear, or a
   DASH ingest reconfiguration) and calls `catalog_publisher_.publish()`
   again with an updated `MsfCatalog` reflecting that change; no such
   producer exists yet. See
   `docs/protocol-mapping.md` for the full wire-level contract and per-path
   breakdown, and
   `docs/superpowers/specs/2026-07-28-msf-cmsf-v1-design.md` for the original
   design. `clone` delta operations are not implemented -- no producer in
   this project generates a track matching another on every field except
   name. Phase 4, MSF URL support, has shipped: MSF sections 11.1 (URL
   structure), 11.1.1 (reserved fragment parameters), and 11.1.2
   (namespace-name tuple encoding) are implemented in `src/msf_url.cpp` and
   `include/openmoq/publisher/msf_url.h`, both parse and build directions.
   `--url` configures the session endpoint, track namespace, and transport
   from one MSF URL; `--print-msf-urls` emits the broadcast's catalog URL
   only, since the catalog is the discovery entry point and a client learns
   every media track from it. MSF section 5.4 is implemented only as
   emit-side validation -- a `%` in a catalog field value is refused unless
   it forms a well-formed `%name%` reference -- because resolution is
   client-side per 5.4.2 and this repository has no subscriber. See
   `docs/protocol-mapping.md` for the full deviation list, including the
   fields exempt from the 5.4 rule and the parameters that are parsed but
   not acted on. (Phase 3, CMSF content
   protection, has shipped for the batch/VOD publish path and is now also
   detected and signalled on the live paths that receive a real CMAF
   initialization segment -- see item 6 below for what that covers and what
   it does not.) MSF section 12
   compression signaling is blocked on transport
   draft-19 Track and Object Properties. Bitrate (MSF 5.2.22) currently
   resolves from the `btrt` box when present, else a codec-class default
   (with an operator warning); the stsz-based computed fallback described in
   the design document is not yet implemented.
6. CMSF content protection (`draft-ietf-moq-cmsf-01` section 4), Phase 3: **shipped** for the batch/VOD publish path, and now also detected and signalled on the live paths that receive a real CMAF initialization segment -- the DASH CTE ingest and the live stdin path. CENC protection is detected from an encrypted sample entry's `sinf`/`schm`/`schi`/`tenc` boxes; the reported codec string is resolved through `frma` so an encrypted track advertises its real pre-encryption codec (e.g. `avc1.64000C`) rather than the bare `encv`/`enca` wrapper type, and its geometry (width/height, or samplerate/channelConfig) is read through the same effective, frma-resolved sample entry rather than gated on the raw `encv`/`enca` type. `pssh` boxes sibling to `trak` under `moov` are extracted per DRM system. The catalog signals this at the root as `contentProtections`, one entry per distinct system (keyed by system ID and scheme, so tracks sharing a KID share one entry), with each protected track pointing at its entries by `contentProtectionRefIDs` -- protection data is never duplicated onto the track itself. A protected track whose init segment carries no `pssh` at all (legal -- CMSF 4.1.1.4.5 makes `pssh` only SHOULD-present, and e.g. ffmpeg's `-encryption_scheme cenc-aes-ctr` omits it) is refused rather than published as an unprotected-looking catalog, since CMSF 4.1.2 defines an absent `contentProtectionRefIDs` as meaning the track is not protected. `--drm-config` supplies deployment configuration (`laURL`, `certURL`, `robustness`) per DRM system, parsed eagerly at CLI startup so a malformed file fails before publishing begins. On the CTE (fragmented, moof-preserving) ingest path, `saio` (Sample Auxiliary Information Offsets) entries are corrected by the moof-size delta when a moof is rebuilt, but only for offsets that actually move (at or after the original `trun`'s end -- see `docs/protocol-mapping.md` for the exact classification rule, including the multi-`traf` limitation); an offset that cannot be a moof-relative reference is refused rather than guessed at, since a wrong offset would decrypt to garbage. **The publisher never decrypts and never encrypts anywhere in this project** -- it only detects and signals protection already present in its input.
   **Not implemented, and refused or unsignalled rather than silently wrong:**
   the progressive-remux path (`segment_for_cmaf`'s non-fragmented branch,
   `src/cmaf_segmenter.cpp`) synthesises `moof` boxes from scratch and cannot
   carry `senc`/`saiz`/`saio`, so encrypted input there is refused with an
   error naming the path and the track rather than producing output that
   looks valid but cannot be decrypted. The live publish paths detect and
   signal content protection wherever a real CMAF initialization segment
   reaches the publisher: `publish_live()`'s stdin ingest and
   `publish_live_objects()`'s DASH ingest both build their catalog through
   paths that call `attach_content_protection` (`build_live_catalog` in
   `src/cmsf_packager.cpp` for stdin, and `build_catalog_locked` in
   `src/live_dash_ingest.cpp` for DASH), and detection there does not depend
   on `--drm-config` being supplied at all -- it comes from the init
   segment's `sinf`/`schm`/`schi`/`tenc` boxes and moov-level `pssh`
   siblings. A protected track with no `pssh` is refused on these paths,
   matching batch, per CMSF 4.1.2. `--drm-config` combined with
   `--live-source srt` is still refused outright by `parse_cli_options`
   (`src/cli_options.cpp`): SRT carries MPEG-TS, and the publisher
   synthesises its CMAF init segment from parsed elementary streams, so
   there are no CENC boxes to detect and nothing `--drm-config` could
   describe -- this is a property of the container, not unfinished work.
   `--drm-config` itself supplies only optional deployment fields (`laURL`,
   `certURL`, `robustness`), never protection detection. No live path in the
   default build (`-DOPENMOQ_USE_LIBMOQ_PUBLISHER=OFF`, the `MoqtSession`
   backend) applies those deployment fields: `MoqtSession` has no access to
   `PublisherConfig::drm_systems` at all, so protection is detected and
   signalled on the stdin and DASH live paths but those deployment fields are
   not applied there. Only the batch/VOD path applies them.
   Also not implemented: MoQ Secure Objects
   encryption fields (MSF 5.2.38-5.2.41, a different LOC-packaged end-to-end
   mechanism than CMSF's CENC), MSF section 12 compression signalling
   (blocked on transport draft-19 Track and Object Properties), `clone`
   delta operations (Phase 2), and the
   CMSF 4.1.1.4.4 Authorization URL field (the draft never names its JSON
   key, so it is deliberately unmodelled).
7. The M2TS packaging example (`examples/msfts-publisher`) has **shipped**: it publishes `packaging: "m2ts"` per `draft-gregoire-moq-msfts`, carrying MPEG-2 Transport Stream or M2TS source packets directly over MOQT, and is covered by `openmoq-publisher-msfts-tests`. The catalog it emits carries eight of the draft's nine `m2ts*` track fields -- `m2tsPacketSize`, `m2tsPacketsPerObject`, `m2tsProgramNumber`, `m2tsPmtPid`, `m2tsPcrPid`, `m2tsPsiInterval`, `m2tsRandomAccess`, and `m2tsTimestampMode`. Not implemented: `m2tsScte35Pid` (draft section 6.10), since the example does not parse SCTE-35 splice signalling out of the source stream.
8. Keep Linux, macOS, and Windows CI/release builds green, including the psychedelic FFmpeg live-publisher example.
