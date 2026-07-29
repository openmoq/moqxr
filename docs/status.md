# Project Status and Roadmap

## Current Status

Current transport and packaging work is past the initial prototype stage. The publisher can generate publish plans, publish over picoquic-backed Raw QUIC and WebTransport paths, and exercise the supported MOQT draft profiles.

Draft status:

- draft 14 is the primary target
- draft 16 is maintained as a secondary compatibility profile
- draft 17 is selectable and has VI64, request-stream, control-codec, ALPN, and WebTransport protocol-offer support; relay interop coverage remains limited
- draft 18 support includes version selection, setup/request framing, request-stream response correlation, fragmented subscriber-interest reads, and same-stream `SUBSCRIBE_OK` responses
- draft 19 is archived as `docs/superpowers/specs/draft-ietf-moq-transport-19.txt` for later review and is not selectable
- CTE LL-DASH regressions cover FFmpeg-style multi-representation paths and draft-16 await-subscribe delivery

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
   for them rather than guess at scope it does not have. See
   `docs/protocol-mapping.md` for the full wire-level contract and per-path
   breakdown, and
   `docs/superpowers/specs/2026-07-28-msf-cmsf-v1-design.md` for the original
   design. `clone` delta operations are not implemented -- no producer in
   this project generates a track matching another on every field except
   name -- and Phases 3 and 4, CMSF content protection and MSF URL parsing,
   remain. MSF section 12 compression signaling is blocked on transport
   draft-19 Track and Object Properties. Bitrate (MSF 5.2.22) currently
   resolves from the `btrt` box when present, else a codec-class default
   (with an operator warning); the stsz-based computed fallback described in
   the design document is not yet implemented.
6. Add DRM/CENC-aware packaging support: detect and preserve encrypted CMAF boxes such as `sinf`, `tenc`, `pssh`, `saiz`, `saio`, and `senc`, expose the needed catalog signaling, and validate encrypted sample forwarding without attempting decryption.
7. Create an M2TS packaging example based on `draft-gregoire-moq-msfts-00`, using the draft's `m2ts` packaging value to carry MPEG-2 Transport Stream or M2TS source packets directly over MOQT.
8. Keep Linux, macOS, and Windows CI/release builds green, including the psychedelic FFmpeg live-publisher example.
