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
   name -- and Phase 4, MSF URL parsing, remains. (Phase 3, CMSF content
   protection, has shipped for the batch/VOD publish path -- see item 6
   below for what that covers and what it does not.) MSF section 12
   compression signaling is blocked on transport
   draft-19 Track and Object Properties. Bitrate (MSF 5.2.22) currently
   resolves from the `btrt` box when present, else a codec-class default
   (with an operator warning); the stsz-based computed fallback described in
   the design document is not yet implemented.
6. CMSF content protection (`draft-ietf-moq-cmsf-01` section 4), Phase 3: **shipped** for the batch/VOD publish path. CENC protection is detected from an encrypted sample entry's `sinf`/`schm`/`schi`/`tenc` boxes; the reported codec string is resolved through `frma` so an encrypted track advertises its real pre-encryption codec (e.g. `avc1.64000C`) rather than the bare `encv`/`enca` wrapper type, and its geometry (width/height, or samplerate/channelConfig) is read through the same effective, frma-resolved sample entry rather than gated on the raw `encv`/`enca` type. `pssh` boxes sibling to `trak` under `moov` are extracted per DRM system. The catalog signals this at the root as `contentProtections`, one entry per distinct system (keyed by system ID and scheme, so tracks sharing a KID share one entry), with each protected track pointing at its entries by `contentProtectionRefIDs` -- protection data is never duplicated onto the track itself. A protected track whose init segment carries no `pssh` at all (legal -- CMSF 4.1.1.4.5 makes `pssh` only SHOULD-present, and e.g. ffmpeg's `-encryption_scheme cenc-aes-ctr` omits it) is refused rather than published as an unprotected-looking catalog, since CMSF 4.1.2 defines an absent `contentProtectionRefIDs` as meaning the track is not protected. `--drm-config` supplies deployment configuration (`laURL`, `certURL`, `robustness`) per DRM system, parsed eagerly at CLI startup so a malformed file fails before publishing begins. On the CTE (fragmented, moof-preserving) ingest path, `saio` (Sample Auxiliary Information Offsets) entries are corrected by the moof-size delta when a moof is rebuilt, but only for offsets that actually move (at or after the original `trun`'s end -- see `docs/protocol-mapping.md` for the exact classification rule, including the multi-`traf` limitation); an offset that cannot be a moof-relative reference is refused rather than guessed at, since a wrong offset would decrypt to garbage. **The publisher never decrypts and never encrypts anywhere in this project** -- it only detects and signals protection already present in its input.
   **Not implemented, and refused or unsignalled rather than silently wrong:**
   the progressive-remux path (`segment_for_cmaf`'s non-fragmented branch,
   `src/cmaf_segmenter.cpp`) synthesises `moof` boxes from scratch and cannot
   carry `senc`/`saiz`/`saio`, so encrypted input there is refused with an
   error naming the path and the track rather than producing output that
   looks valid but cannot be decrypted. The live publish paths
   (`publish_live()`'s SRT/stdin ingest and `publish_live_objects()`'s DASH
   ingest, via `build_live_catalog` in `src/cmsf_packager.cpp`) never call
   `attach_content_protection` at all, so `--drm-config` combined with
   `--live-source srt`, `--live-source dash`, or the default live-stdin path
   is refused outright by `parse_cli_options` (`src/cli_options.cpp`) --
   publishing would otherwise produce a catalog with no
   `contentProtections`/`contentProtectionRefIDs`, indistinguishable from
   genuinely unprotected content. **This refusal is narrower than it may
   look:** it fires only when `--drm-config` is actually supplied, since
   detecting protection at CLI-parse time (before any media is read) is not
   possible. Encrypted live input published with no `--drm-config` at all is
   not refused and still publishes fully unsignalled -- `--drm-config`
   supplies only optional deployment fields, not protection detection.
   Wiring content protection into the live paths remains future work (Phase
   4 or later). Also not implemented: MoQ Secure Objects
   encryption fields (MSF 5.2.38-5.2.41, a different LOC-packaged end-to-end
   mechanism than CMSF's CENC), MSF section 12 compression signalling
   (blocked on transport draft-19 Track and Object Properties), MSF URL and
   fragment parsing (Phase 4), `clone` delta operations (Phase 2), and the
   CMSF 4.1.1.4.4 Authorization URL field (the draft never names its JSON
   key, so it is deliberately unmodelled).
7. Create an M2TS packaging example based on `draft-gregoire-moq-msfts-00`, using the draft's `m2ts` packaging value to carry MPEG-2 Transport Stream or M2TS source packets directly over MOQT.
8. Keep Linux, macOS, and Windows CI/release builds green, including the psychedelic FFmpeg live-publisher example.
