# CMSF Content Protection (Phase 3)

Design for CMAF Common Encryption support defined by `draft-ietf-moq-cmsf-01`
section 4. Phase 3 of the roadmap in
`docs/superpowers/specs/2026-07-28-msf-cmsf-v1-design.md`.

- Status: approved design, pending implementation plan
- Date: 2026-07-29
- Depends on: Phase 1 (merged, 7696171) and Phase 2 (merged, 8a1a770)

## Problem

The publisher has no Common Encryption support at all. A grep for `sinf`,
`tenc`, `pssh`, `frma`, `encv`, and `enca` across `src/` and `include/` returns
nothing. Encrypted CMAF input is neither detected nor signalled, and its codec
strings come out wrong.

Investigation before writing this design surfaced three findings that shape it.

### A live memory-safety bug, third of its family

`extract_codec_init_data` (`src/cmsf_packager.cpp`) bounds its scan by the raw
`sample_entry.span.offset + sample_entry.span.size`, where `span.size` comes
from an unvalidated `read_be32` of attacker-controlled file bytes. It then
constructs `bytes.begin() + (cursor + box_size)` from that unclamped bound.

Phase 1's final review found and fixed exactly this pattern in
`find_child_box_offset` and in `mpeg4_audio_codec_string`. This sibling was
missed. It is reachable at `src/cmsf_packager.cpp:423` from
`build_publish_plan`, so publishing a malformed MP4 file reaches it.

It is doubly relevant here: the scan is unanchored, so once encrypted content
is parsed it sweeps across `sinf`, `tenc`, and PSSH bytes, where a coincidental
four-byte type match with a plausible size field yields a false positive. That
risk was recorded in the Phase 1 design document and never closed.

The project owner ruled that Phase 3 replaces the scan wholesale rather than
patching it separately. It is therefore the first task, with an ASAN regression
fixture, because the bug is live on `main` until this phase merges.

### Encrypted content survives only one of three publish paths

| Path | `moof` handling | CENC viability |
| --- | --- | --- |
| Fragmented MP4, batch | referenced verbatim by span (`src/cmaf_segmenter.cpp:1156`) | safe; `senc`, `saiz`, `saio` untouched |
| Progressive MP4, remux | synthesized (`src/cmaf_segmenter.cpp:775-821`) | impossible; no CENC boxes exist to carry |
| CTE LL-DASH ingest | rebuilt with normalized `trun` (`src/cmaf_segmenter.cpp:1395`) | broken; `saio` offsets copied while the moof's size changes |

`rebuild_moof` (`src/cmaf_segmenter.cpp:303-327`) copies every `traf` child
verbatim except `trun`, which it normalizes because FFmpeg's DASH muxer emits
minimal `trun` entries. The rebuild changes the moof's size — the code already
recomputes `trun`'s `data_offset` at `:329-330` for precisely that reason — but
`saio` receives no equivalent correction. `saio` points at sample auxiliary
information, so a decryptor reading those offsets gets the wrong bytes and
produces garbage rather than an error.

The project owner ruled that Phase 3 fixes `saio` for the CTE path rather than
refusing there.

### `frma` unwrapping is a correctness fix, not a feature

`codec_string_from_sample_entry` keys off `sample_entry_type`. For encrypted
tracks that value is `encv` or `enca`, so the catalog advertises a codec string
of `"encv"`. That is wrong independently of whether protection is signalled at
all, and it is wrong today for any encrypted input.

## Decisions

1. **Replace the unanchored scan, do not patch it.** A proper child-box walk
   that iterates by length fields from a clamped limit. Task 1, with an ASAN
   fixture.
2. **File-derived protection data, with optional configuration.** `defaultKID`,
   `scheme`, `systemID`, and `pssh` come from the media. `laURL`, `certURL`,
   authorization, and `robustness` come from `PublisherConfig` and a CLI flag,
   keyed by system ID. A catalog built from the file alone is conformant; a
   configured one is usable for real playback, since a subscriber cannot
   acquire a licence without `laURL`.
3. **Correct `saio` on the CTE path; refuse when the reference base is
   ambiguous.** See the classification rule below. Guessing reintroduces silent
   corruption.
4. **Refuse encrypted input on the progressive-remux path.** Synthesized moofs
   cannot carry CENC boxes, so signalling protection there would describe media
   that is not actually decryptable.
5. **No decryption, ever.** The publisher passes encrypted samples through
   untouched. It never holds keys and never decrypts.
6. **ClearKey needs no special code.** CMSF section 4.3 ClearKey is a
   well-known system ID plus a `laURL`, both of which fall out of decisions 2.

## Part 1: CENC box parsing

New module: `include/openmoq/publisher/cenc.h` and `src/cenc.cpp`. It owns CENC
box structure only — separate from `mp4_box` (generic box parsing) and
`msf_catalog` (the catalog document model).

### The child-box walk

```
std::optional<Mp4Box> find_child_box(std::span<const std::uint8_t> bytes,
                                     std::size_t container_offset,
                                     std::size_t container_end,
                                     std::size_t first_child_offset,
                                     std::string_view type);
```

It advances by each box's declared length rather than by one byte, and bounds
every read against `std::min(container_end, bytes.size())`. A box whose length
is under 8, or which would extend past the clamped limit, terminates the walk
rather than being skipped — a malformed length means the remaining bytes cannot
be trusted to be a box chain at all.

This replaces `extract_codec_init_data`'s sliding scan. The fixed
`VisualSampleEntry` and `AudioSampleEntry` header offsets (`8 + 70` and
`8 + 28`) become the `first_child_offset` argument.

### What is extracted

From a sample entry whose type is `encv` or `enca`:

- `sinf` → `frma`: the original four-character codec format. This replaces the
  sample entry type for codec-string purposes.
- `sinf` → `schm`: the protection scheme, `cenc` or `cbcs`. CMSF section 4.1.1.3
  allows only those two; any other value means the track is protected by a
  scheme this publisher does not understand, and is refused.
- `sinf` → `schi` → `tenc`: `default_isProtected`, `default_Per_Sample_IV_Size`,
  and `default_KID` (16 bytes, rendered as a UUID string).

From `moov` and from the file's top level:

- `pssh`: system ID (16 bytes as a UUID string) and the full box bytes,
  Base64-encoded for the catalog's `pssh` field. A file may carry several, one
  per DRM system.

### Structures

```
struct CencTrackProtection {
    std::string original_format;      // from frma, e.g. "avc1"
    std::string scheme;               // "cenc" or "cbcs"
    std::string default_kid;          // UUID string
    std::uint8_t per_sample_iv_size = 0;
    bool is_protected = false;
};

struct CencSystem {
    std::string system_id;            // UUID string
    std::string pssh_base64;
};
```

## Part 2: Codec strings for protected tracks

`extract_tracks` gains the protection information for each track. When
`CencTrackProtection::original_format` is set, codec-string derivation uses it
in place of the `encv`/`enca` sample entry type, so an encrypted AVC track
reports `avc1.640028` rather than `encv`.

`TrackDescription` gains a `protection` field holding the parsed
`CencTrackProtection`, so downstream consumers can tell a protected track from
an unprotected one without re-parsing.

## Part 3: Catalog signalling

CMSF section 4.1 places protection data at the catalog root and has tracks
reference it. Data is never duplicated at track level.

`MsfCatalog` gains:

```
// Declared before MsfContentProtection, which holds these by value.
struct MsfUrlEntry {
    std::string url;
    std::optional<std::string> type;
};

struct MsfContentProtection {
    std::string ref_id;                          // 4.1.1.1
    std::vector<std::string> default_kids;       // 4.1.1.2
    std::string scheme;                          // 4.1.1.3
    std::string system_id;                       // 4.1.1.4.1
    std::optional<MsfUrlEntry> la_url;           // 4.1.1.4.2
    std::optional<MsfUrlEntry> cert_url;         // 4.1.1.4.3
    std::optional<MsfUrlEntry> auth_url;         // 4.1.1.4.4
    std::optional<std::string> pssh_base64;      // 4.1.1.4.5
    std::optional<std::string> robustness;       // 4.1.1.4.6
};

std::vector<MsfContentProtection> content_protections;
```

Emitted between `generatedAt` and `tracks`, matching the layout of the CMSF
draft's own examples in sections 5.2 and 5.3.

`MsfTrack` gains `std::vector<std::string> content_protection_ref_ids`
(section 4.1.2).

One `contentProtections` entry per distinct system ID found in the media. Every
protected track references the entries whose scheme and KID match its own.

Serialization ordering follows the existing writer: `contentProtections` is a
root field emitted before `tracks`, since `initDataList` is already required to
follow `tracks` and nothing else constrains root ordering.

### Configuration

`PublisherConfig` gains:

```
struct DrmSystemConfig {
    std::string system_id;                       // UUID string, the key
    std::optional<std::string> la_url;
    std::optional<std::string> la_url_type;
    std::optional<std::string> cert_url;
    std::optional<std::string> cert_url_type;
    std::optional<std::string> robustness;
};

std::vector<DrmSystemConfig> drm_systems;
```

Entries are matched to file-derived systems by `system_id`. A configured system
not present in the media is ignored rather than emitted, because a
`contentProtections` entry for a system the media carries no PSSH for would
describe protection that does not exist.

A CLI flag supplies these. Given the number of fields, the flag takes a path to
a small JSON file rather than a long inline string, matching how
`live_srt_config` already handles structured configuration in this project.

### Validation

Added to `validate_catalog`:

- Every `content_protection_ref_ids` entry resolves to a `contentProtections`
  `ref_id`. A dangling reference throws, mirroring the existing `initRef` rule.
- `ref_id` values are unique.
- `scheme` is `cenc` or `cbcs` (CMSF section 4.1.1.3).
- `default_kids` entries are well-formed UUID strings, in the
  `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` form CMSF section 4.1.1.2 requires.
- `system_id` is a well-formed UUID string.
- A `contentProtections` entry carries at least one `default_kid`, since
  section 4.1.1.2 makes the field Required.

## Part 4: `saio` correction on the CTE path

Depends on Part 1's box parsing.

When `rebuild_moof` (`src/cmaf_segmenter.cpp:303`) changes the moof's size by
delta *D*, every moof-relative `saio` offset shifts by exactly *D*.

`saio` is a FullBox (ISO/IEC 14496-12 section 8.7.13):

```
if (flags & 1) { unsigned int(32) aux_info_type;
                 unsigned int(32) aux_info_type_parameter; }
unsigned int(32) entry_count;
if (version == 0) unsigned int(32) offset[entry_count];
else              unsigned int(64) offset[entry_count];
```

The correction rewrites `saio` in place within the rebuilt `traf`, adding *D*
to each offset. Both version variants and the optional `aux_info_type` prefix
must be handled; a version other than 0 or 1 is refused.

### Reference-base classification

This is the riskiest judgment in this design, and it refuses rather than
guesses. For each offset, against the ORIGINAL moof size and the following
mdat size:

- `offset < original_moof_size` — points inside the moof, at `senc` in the
  `traf`. Moof-relative. **Correct by adding *D*.**
- `original_moof_size <= offset < original_moof_size + mdat_size` — points into
  the mdat, moof-relative. **Correct by adding *D*.**
- otherwise — an absolute file offset, which has no meaning in a republished
  MOQT object regardless of the rebuild. **Refuse the fragment with an
  explicit error naming the offset.**

A `saio` present with no `senc` and no `saiz` is treated as malformed and
refused, since the auxiliary data it points at cannot be located to validate
the classification.

### Refusals elsewhere

Encrypted input on the progressive-remux path is refused with a message naming
the path and the reason, rather than published with synthesized moofs that
carry no CENC boxes.

## Error handling

Consistent with Phases 1 and 2: invariant violations throw
`std::runtime_error` naming the offending track or box.

Degrade rather than throw: a missing or malformed `pssh`, an absent `tenc`, a
`sinf` without `schm`. These yield "not protected" and the track publishes
unencrypted, which is accurate because a track the publisher cannot confirm is
protected should not be advertised as protected.

Refuse rather than degrade, because degrading would publish wrong data: an
unrecognised `schm` scheme; a `saio` offset that classifies as absolute; a
`saio` version other than 0 or 1; encrypted input on the progressive-remux
path; a `contentProtections` reference that does not resolve.

## Testing

Tests follow the existing single-binary `expect()` convention. This phase needs
synthetic encrypted fixtures the repository does not have, built with the box
builders already in `tests/`:

- an `encv` sample entry containing `sinf` with `frma`, `schm`, and
  `schi`/`tenc`
- `pssh` boxes for two distinct DRM systems
- a `traf` carrying `senc`, `saiz`, and `saio` in both version 0 and version 1
  forms, with and without the `aux_info_type` prefix

Coverage:

- The child-box walk: an ASAN regression fixture with a sample entry declaring
  a size far larger than the buffer, asserting no out-of-bounds read. This must
  fail if the clamp is reverted.
- `frma` unwrapping: an encrypted AVC track reports `avc1.…`, not `encv`.
- Catalog: `contentProtections` at root only, never duplicated on tracks;
  `contentProtectionRefIDs` resolving; one entry per system for a two-system
  file.
- Configuration: a configured `laURL` reaches the catalog; a configured system
  absent from the media is not emitted.
- Validation: each refusal case throws, with fixtures that are otherwise valid
  so the throw is attributable to the condition under test.
- `saio`: offsets corrected **numerically** against a known delta *D*, for v0
  and v1, with and without the `aux_info_type` prefix. Asserting merely that
  offsets changed would pass against an implementation adding the wrong delta.
- `saio` classification: a moof-internal offset and an mdat-relative offset are
  both corrected; an absolute-looking offset is refused.

## Risks

**Part 4 is specified against box-parsing code that does not exist yet.** That
is the pattern which produced defects in the Phase 2 plan, where a task was
ordered before the plumbing it depended on. The implementation plan must order
all of Part 1 before any of Part 4, and Part 4's brief must be written after
Part 1 has landed rather than up front.

**The `saio` reference-base classification is heuristic.** It is defensible —
the three cases are exhaustive for well-formed input — but a muxer writing
offsets in a form not anticipated here would hit the refusal path. That is the
intended failure mode, and refusals are visible where corruption is not.

## Out of scope

- Decryption. The publisher never holds keys.
- Encryption. The publisher signals protection present in its input; it does
  not add protection.
- MSF sections 5.2.38 to 5.2.41, MoQ Secure Objects. That is the LOC-packaged
  end-to-end encryption path; CMSF uses CENC, which this phase covers.
- MSF section 12 compression signalling, still blocked on transport draft-19
  Track and Object Properties.
- Phase 4, MSF URL and fragment parsing.

## References

- `docs/draft-ietf-moq-cmsf-01.txt` sections 3.5.1, 4, 4.1, 4.2, 4.3
- ISO/IEC 23001-7, Common Encryption
- ISO/IEC 14496-12 sections 8.7.12 (`saiz`) and 8.7.13 (`saio`)
- `docs/superpowers/specs/2026-07-28-msf-cmsf-v1-design.md`
