# LOC-05 additional codecs design

Date: 2026-11-18. Status: approved.

## Intent

`--packaging loc` currently publishes H.264 (`avc1`) and AAC-LC only. Extend
LOC-05 publishing to HEVC, Opus, AV1 and VP9 on every input path where the
codec can arrive. The Codec String property (`0x11`) and the LOC-05 property
IDs (`0x08`, `0x09`, `0x0C`, `0x0D`, `0x0F`, `0x10`, `0x11`) are already
implemented and unchanged.

## Goals

- A `LocCodecProfile` table with one entry per codec: AVC, HEVC, Opus, AV1, VP9.
- Each profile: recognizes the sample entry; validates and extracts extradata
  (Video Config `0x0D` / Audio Config `0x0F`); detects keyframes for the
  frame-marking byte and group boundaries; checks the codec string against
  the config.
- MP4 files, live stdin, DASH and the explicit object API use the table.
- SRT (MPEG-TS) supports H.264, AAC and HEVC only; other codecs fail with a
  clear message because no standard TS mapping exists for them.
- AVC and AAC output stays byte-identical to the current output.

## Non-goals

- Scalable-layer subgroups, datagrams, Secure Objects encryption, LOC-01
  interop mode.
- libmoq backend (still rejected).
- Encrypted input (still rejected).
- AAC variants such as HE-AAC.

## Anti-goals

- Passing unvalidated extradata through.
- Any change to CMAF or LOCMAF bytes.
- A codec string that disagrees with the catalog.

## Constraints

- Draft 18 or later only; one ordered subgroup per GOP for video, one sample
  per group for audio.
- Mid-stream config changes fail explicitly.
- Codec strings follow the WebCodecs registry: `hvc1.`/`hev1.`, `opus`,
  `av01.`, `vp09.`.

## Approaches

- A. Codec profile table (chosen): one profile per codec, shared by all paths.
  Moderate up-front refactor; each later codec is one profile.
- B. Per-codec `if` branches where H.264 is assumed: smaller first diff, but
  checks scatter over five or six files and each codec gets harder.
- C. Pass extradata through unvalidated: rejected; wrong config would reach
  relays and decoders silently, against the existing explicit-rejection design.

## Order and testing

1. Refactor to the profile table; AVC/AAC output byte-identical, proved with
   existing fixtures.
2. HEVC (hvcC; frame marking from IRAP NAL type), including SRT.
3. Opus (OpusHead built from the `dOps` box).
4. AV1 (av1C; keyframe from sequence header/OBU).
5. VP9 (vpcC; keyframe from frame header).

Each codec gets a unit test and an extension of `scripts/test-loc05.mjs` for
independent decoded-frame equivalence. Update `docs/status.md` and
`docs/testing.md`; relay forwarding, cached FETCH and browser playback remain
unverified.

## Decisions made by the author (correctable)

- Frame-marking bits come from the bitstream, not the container sync flag.
- Mid-stream config changes still fail explicitly.

## Left to the builder

- Profile interface shape and file layout.
- Test fixture generation (FFmpeg) per codec.
