# Live-Path Content Protection — Design

Phase 5. Wires CMSF content protection into the live publish paths, closing the
gap Phase 3 left: protection is detected and signalled on the batch/VOD path
only, while this publisher is expected to be live by default.

Implements no new draft surface. Every primitive already exists from Phase 3.

## Goal

An encrypted live broadcast whose input carries real CMAF initialization
segments publishes a catalog with `contentProtections` and per-track
`contentProtectionRefIDs`, exactly as the batch path already does. An encrypted
live broadcast that cannot be signalled is refused rather than published as
clear.

## The finding that shapes this phase

The three live sources are not equivalent, and the distinction that matters is
not SRT versus DASH versus stdin. It is whether a **real initialization segment
reaches the publisher**:

| Path | Init segment | Protection detectable |
| --- | --- | --- |
| DASH CTE ingest | real, from the encoder (`PathState::init_bytes`) | yes |
| stdin live | real, parsed from the stream | yes |
| session real-init path | real | yes |
| SRT | synthesised by `build_init_segment_from_tracks` from parsed elementary streams | no |

`extract_tracks` already populates `TrackDescription::protection` wherever a real
init segment is parsed. The DASH path has called `extract_tracks` since before
this phase (`src/live_dash_ingest.cpp:386`). The protection data is present and
simply never reaches the catalog.

SRT carries MPEG-TS. TS has no CMAF CENC metadata — no `sinf`, no `tenc`, no
`pssh`. There is nothing to detect, and mapping TS scrambling control bits onto
CMSF's CENC-shaped model is undefined by the drafts. SRT is therefore out of
scope for detection, and its refusal message is corrected to say so.

## Architecture

Two attachment points, both of which already receive everything required.

**1. `build_live_catalog`** (`src/cmsf_packager.cpp:727`) — the convergence point
for four call sites: `src/transport/libmoq_publisher.cpp:946` and `:1181`, and
`src/transport/moqt_session.cpp:3178` and `:3655`. It already takes
`const std::vector<TrackDescription>& tracks` and
`std::span<const std::uint8_t> init_segment`.

**2. `LiveDashIngestSession::build_catalog_locked`**
(`src/live_dash_ingest.cpp:506`) — the DASH CTE path builds its catalog
independently and does not route through `build_live_catalog`.

### Why the synthetic-init paths need no branch

Two of `build_live_catalog`'s four callers pass a synthesised init segment. No
path-type flag is needed: those tracks never passed through a `sinf`, so
`TrackDescription::protection` is empty and `collect_pssh_systems` finds nothing.
Attachment is inert because the data says so. Adding a branch on path type would
encode the same fact twice and invite the two copies to disagree.

## What each attachment point does

Mirror the batch path at `src/cmsf_packager.cpp:515-591` exactly. It is already
correct, already reviewed, and divergence between the two is precisely the kind
of defect this project has repeatedly found at whole-branch review.

Parse the systems once, outside the per-track loop:

```cpp
const bool any_track_protected = std::any_of(
    tracks.begin(), tracks.end(),
    [](const TrackDescription& track) { return track.protection.has_value(); });
const std::vector<CencSystem> pssh_systems =
    any_track_protected ? collect_pssh_systems(init_segment) : std::vector<CencSystem>{};
```

Then inside the loop, after `attach_init_data`:

```cpp
if (track.protection.has_value()) {
    if (pssh_systems.empty()) {
        throw std::runtime_error(
            "track '" + track.track_name +
            "' is protected (CENC) but no pssh system was found in the "
            "initialization segment; refusing to publish a catalog with no "
            "contentProtections entry for encrypted content");
    }
    attach_content_protection(msf_catalog, msf_track, *track.protection, pssh_systems);
    // record the scheme for apply_drm_system_configs, hoisted below the loop
}
```

And after the loop, one `apply_drm_system_configs` call per distinct scheme —
hoisted out of the loop, matching the fix Phase 3's review already applied to
the batch path.

## `pssh` must be read from the full init segment

`pssh` boxes are siblings of `trak` under `moov`, not children of a `trak`. They
belong to the initialization segment as a whole.

`build_live_catalog` already receives the full `init_segment`, so it needs no
change here — note only that `collect_pssh_systems` must be given that span and
**not** the per-track segment `build_track_specific_init_segment` synthesises.

The DASH path does need a change. `LiveDashIngestSession::RegisteredTrack`
(`include/openmoq/publisher/live_dash_ingest.h:87-90`) holds a
`TrackDescription` and a base64 per-track init string, but not the raw init
bytes. `build_catalog_locked` therefore has no access to the `pssh` boxes.
`PathState::init_bytes` holds them at registration time
(`src/live_dash_ingest.cpp:376`).

**Resolution:** parse the systems at registration time, while
`PathState::init_bytes` is still in hand, and store the small parsed result —
`std::vector<CencSystem>` — on each `RegisteredTrack`. Registration already
happens once per path, so `collect_pssh_systems` runs once per path rather than
once per catalog build.

Storing the parsed systems is preferred over retaining the raw init bytes: an
init segment is kilobytes and would be duplicated across every track registered
from that path, whereas a parsed system list is a handful of small structs.

**Correction, established empirically during Task 3.** An earlier draft of this
spec claimed the per-track init segment carries no `pssh` and therefore could
not serve as the parse source. That is false in this codebase:
`build_track_specific_init_segment` (`src/cmsf_packager.cpp:230-268`) copies
every `moov` child that is not `trak` or `mvex` verbatim, `pssh` included, so a
per-track init segment does retain the moov-level `pssh` boxes.

That behaviour is correct and must not be changed. A subscriber initialising a
decoder from a track's `initData` needs the `pssh` to set up DRM; stripping it
would break exactly the consumer the field exists for.

Parsing at registration from `PathState::init_bytes` remains the right design
for different reasons: it runs once per path rather than once per catalog build,
it avoids a base64 decode round-trip, and it does not depend on
`build_track_specific_init_segment`'s copy-everything-else behaviour, which is
incidental rather than contractual.

This requires promoting `collect_pssh_systems` from `cmsf_packager.cpp`'s
anonymous namespace (it is declared at line 367) to `cmsf_packager.h`, so the
DASH ingest can call it. That is the same shared-helper extraction Phase 3
applied to the JSON reader, and it keeps one implementation of "find the `pssh`
boxes under `moov` and parse them" rather than two.

## Deployment configuration

`--drm-config` supplies `laURL`, `certURL`, and `robustness`. With detection
wired, those fields must reach the live catalog too.

`build_live_catalog` gains a trailing defaulted parameter matching the
convention `cmsf_packager.h:67` already uses for the batch entry point:

```cpp
LiveCatalog build_live_catalog(const std::vector<TrackDescription>& tracks,
                               std::span<const std::uint8_t> init_segment,
                               bool is_live,
                               const std::vector<DrmSystemConfig>& drm_systems = {});
```

The default keeps all four existing call sites compiling unchanged; only the
sites with access to the publisher's configuration pass it.

## CLI changes

`src/cli_options.cpp:469` currently refuses `--drm-config` for every live
source. That becomes:

- **DASH and stdin live:** accepted. Detection now works on these paths.
- **SRT:** still refused, with a corrected message. The current text says the
  live path "does not yet attach content protection to the catalog", which
  becomes false for two of three sources and misleading about why SRT differs.
  The replacement states that SRT carries MPEG-TS, which has no CMAF CENC
  metadata to detect, so `--drm-config` cannot describe anything.

## Error handling

**A protected track with no `pssh` throws, terminating the broadcast.** This
matches the batch path and CMSF §4.1.2: an absent `contentProtectionRefIDs`
*means* the content is not protected, so publishing encrypted media without it
is an affirmative false claim, not an omission.

In practice this fires at the first catalog build, at or just before stream
start. On the DASH path a track registering mid-broadcast could terminate a
running stream. That is accepted: publishing encrypted media advertised as clear
is worse than a failed broadcast, and a subscriber that cannot decrypt has no
recovery either.

Everything else degrades as it already does. A track with no protection attaches
nothing.

## Delta catalogs

No work required. MSF §5.3 forbids `contentProtections` on a delta catalog, and
`validate_catalog` already returns early for deltas before reaching the
content-protection checks. Republished full catalogs carry protection because
they run the same construction path.

## Testing

Tests follow the existing single-binary `expect()` style under `tests/`.

**`build_live_catalog`:**

- An encrypted fixture with `sinf`/`tenc` and a `pssh` produces a catalog with a
  root `contentProtections` entry and a matching `contentProtectionRefIDs` on the
  track, asserted against exact values rather than key presence.
- The same fixture with the `pssh` removed is refused, with the message naming
  the track. The fixture must be valid in every other respect so the refusal is
  attributable to the missing `pssh` and not to an unrelated defect.
- An unencrypted fixture produces a catalog with no `contentProtections` at all —
  the regression guard for every existing live user.
- A synthesised init segment carrying no CENC boxes produces no
  `contentProtections` and does not throw, proving the synthetic-init paths stay
  inert.
- `--drm-config` fields reach the live catalog's `laURL`/`certURL`/`robustness`.

**DASH CTE path:**

- A registered track whose init segment carries protection yields a catalog with
  `contentProtections`, proving the full init bytes survived to catalog
  construction. This is the test that would fail if the per-track base64 were
  used as the `pssh` source, so it must assert the entry exists rather than
  merely that the catalog serialises.

**CLI:**

- `--drm-config` with `--live-source dash` is accepted.
- `--drm-config` with `--live-source srt` is refused, and the message mentions
  MPEG-TS rather than claiming the feature is unimplemented.

Each new refusal gets a mutation check: revert the guard, observe the specific
named assertion fail, restore.

## Constraints

- C++20, no new dependencies, namespace `openmoq::publisher`.
- Build with `-DOPENMOQ_RUN_PICOQUIC_SMOKE_TESTS=OFF -DOPENMOQ_LIBMOQ_SOURCE_DIR=<path to moq5>`;
  without the libmoq flag the suite silently drops a target with no error.
- The suite must stay green and the tree-wide compiler warning count must stay
  at its 12-warning baseline (unique lines, `sort -u`). There is no `-Werror`.
- **The publisher never encrypts and never decrypts.** This phase detects and
  signals protection already present in its input, exactly as Phase 3 does.
