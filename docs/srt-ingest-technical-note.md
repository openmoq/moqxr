# SRT Live Ingest → MoQ Publishing: Technical Note

## Overview
This document describes how `moqxr` receives live MPEG-TS over SRT, demuxes it into elementary stream samples, and publishes the media as MoQ objects. It also compares this to the stdin fragmented-MP4 path.

---

## High-Level Pipeline

```
SRT socket (1316-byte datagrams)
└── MPEG-TS byte stream (buffered)
    └── 188-byte TS packets (sync byte 0x47)
        ├── PAT → discovers PMT PID
        ├── PMT → discovers video PID + audio PID + stream types
        └── PES assembly (per-PID)
            └── EsSample (one complete access unit)
                ├── Annex-B → MP4 length-prefixed NALUs (video)
                │   or ADTS → raw AAC frames (audio)
                └── moof + mdat (one sample per fragment)
                    └── MoQ Object (group_id / object_id)
```

---

## Step-by-Step Flow

### 1. SRT Reception
**File:** `live_srt_ingest.cpp`, worker thread (line ~1340)

```
std::array recv_buf{};
const int received = srt_recv(sock, recv_buf.data(), recv_buf.size());
```

- Each `srt_recv()` returns up to **1316 bytes** — this is the standard SRT payload size (7 × 188-byte TS packets).
- The raw bytes are passed directly to the demuxer: `demuxer.feed(recv_buf.data(), received, sample_sink)`.

---

### 2. Buffering and TS Packet Extraction
**Class:** `TsPesDemuxer::feed()` (line ~898)

```
buffer_.insert(buffer_.end(), data, data + size);

while (buffer_.size() >= 188) {
    if (buffer_[0] != 0x47) {
        // Sync recovery: skip bytes until next 0x47
        auto sync_it = std::find(buffer_.begin() + 1, buffer_.end(), 0x47);
        buffer_.erase(buffer_.begin(), sync_it);
        continue;
    }
    // Extract one 188-byte TS packet
    std::copy_n(buffer_.begin(), 188, packet.begin());
    buffer_.erase(buffer_.begin(), buffer_.begin() + 188);
    parse_packet(packet, sample_sink);
}
```
Incoming 1316-byte SRT payloads are appended to a persistent buffer. The buffer is then consumed 188 bytes at a time, each time verifying the sync byte `0x47`. If sync is lost, bytes are discarded until the next `0x47`.

---

### 3. PAT Parsing
**Method:** `TsPesDemuxer::parse_pat()` (line ~946)

- PID 0x0000 is always the PAT (Program Association Table).
- PAT entries map `program_number` → `PMT PID`.
- The code picks the first program (or the one matching a configured `program_number`).
- Result: `pmt_pid_` is set.

---

### 4. PMT Parsing and PID Discovery
**Method:** `TsPesDemuxer::parse_pmt()` (line ~972)

- When a TS packet arrives on the discovered `pmt_pid_`, the PMT (Program Map Table) is parsed.
- Each elementary stream descriptor has:`stream_type` (0x1B = H.264, 0x24 = HEVC, 0x0F = AAC-ADTS, etc.)
- `elementary_PID`

- First video-type PID found → `video_pid_`, `video_stream_type_`
- First audio-type PID found → `audio_pid_`, `audio_stream_type_`
- After PMT: `pmt_parsed_ = true`

**Stream type mapping:**

Stream Type | Codec
--- | ---
0x02 | MPEG-2 Video
0x1B | H.264/AVC
0x24 | H.265/HEVC
0x0F | AAC (ADTS)
0x11 | AAC (LATM)
0x03/0x04 | MPEG Audio

---

### 5. PES Packet Reconstruction
**Method:** `TsPesDemuxer::parse_pes()` (line ~1010)

TS packets carrying a known video/audio PID are assembled into PES (Packetized Elementary Stream) buffers:

1. When `payload_unit_start` is set → flush the previous PES buffer, start a new one
2. Parse the PES header:Bytes `[0..2]` = start code `00 00 01`
3. Byte 7 = flags (bit 7 = PTS present)
4. Byte 8 = PES header data length
5. If PTS present: extract 33-bit PTS from 5 bytes at offset 9
6. Remaining payload bytes are appended to the PES data buffer
7. Subsequent TS packets (same PID, no `payload_unit_start`) append their payload to the same buffer

**PTS extraction (33-bit timestamp at 90kHz clock):**

```
pes.pts90k = ((payload[9] >> 1) & 0x07) << 30
           | payload[10] << 22
           | ((payload[11] >> 1) & 0x7F) << 15
           | payload[12] << 7
           | ((payload[13] >> 1) & 0x7F);
```

---

### 6. EsSample — The Elementary Stream Access Unit
**Struct:** `EsSample` (line ~405)

```
struct EsSample {
    bool is_video = false;
    std::uint64_t pts90k = 0;      // Presentation timestamp in 90kHz ticks
    std::uint8_t stream_type = 0;  // MPEG-TS stream_type from PMT
    std::vector payload;  // Raw ES data (length-prefixed NALUs for video, raw AAC for audio)
    bool keyframe = false;
    // For audio from ADTS: first 9 bytes of the original ADTS header (for codec discovery).
    std::array<std::uint8_t, 9> adts_header{};
    std::uint8_t adts_header_len = 0;
};
```
When a PES buffer is flushed (`flush_pes`), one or more `EsSample`s are produced:

- **Video:** one `EsSample` per PES (one coded picture in Annex-B format)
- **Audio:** the PES may contain multiple ADTS frames; `flush_pes` splits them into **individual `EsSample`s** with interpolated PTS (see §8)
- `keyframe` is determined by scanning for:
  - H.264: NAL type 5 (IDR)
  - HEVC: NAL types 16–23 (IRAP)

---

### 7. Annex-B → MP4 Length-Prefixed (Video)
**Function:** `annexb_to_avcc()` (line ~355)

MPEG-TS delivers H.264/HEVC in **Annex-B** format (start-code delimited):

```
00 00 00 01 [NAL] 00 00 00 01 [NAL] ...
```
MP4/CMAF requires **length-prefixed** format:

```
[4-byte big-endian length] [NAL] [4-byte length] [NAL] ...
```
The function:

1. Scans for start codes (3-byte `00 00 01` or 4-byte `00 00 00 01`)
2. Measures the NAL unit length (bytes until next start code or end)
3. Writes `[BE32 length][NAL bytes]` for each unit

---

### 8. AAC ADTS Handling
**Inline in:** `flush_pes()` (line ~1050)

AAC in MPEG-TS uses ADTS (Audio Data Transport Stream) framing:

```
[ADTS header (7 or 9 bytes)] [AAC frame] [ADTS header] [AAC frame] ...
```
MP4/CMAF stores **raw AAC frames** without ADTS headers. The ADTS splitting logic in `flush_pes()`:

1. Validates sync word `0xFFF` (`0xFF` + upper nibble `0xF0`) at each frame start
2. Parses `protection_absent` flag → header is 7 bytes (no CRC) or 9 bytes (with CRC)
3. Reads `frame_length` field from ADTS header
4. Strips headers, emits each raw AAC frame as a **separate `EsSample`**
5. Interpolates PTS for each frame using rational arithmetic: `base_pts + (frame_index × 1024 × 90000 + rate/2) / rate`
6. Preserves the first ADTS header in `EsSample::adts_header` for codec discovery

---

### 9. Building avcC / hvcC / esds Init Metadata
During codec discovery (before streaming begins), the code extracts decoder configuration from the first keyframe/ADTS frame:

#### H.264 → avcC box
**Function:** `build_avcc_box()` (line ~590)

1. `extract_h264_sps_pps()` scans the first keyframe's Annex-B data for NAL type 7 (SPS) and type 8 (PPS)
2. Builds an `AVCDecoderConfigurationRecord`:configurationVersion = 1
3. profile/level from SPS bytes [1..3]
4. lengthSizeMinusOne = 3 (4-byte NAL lengths)
5. SPS array, PPS array

#### HEVC → hvcC box
**Function:** `build_hvcc_box()` (line ~632)

1. `extract_hevc_param_sets()` scans for NAL types 32 (VPS), 33 (SPS), 34 (PPS)
2. Builds an `HEVCDecoderConfigurationRecord`:Profile/tier/level parsed from SPS
3. 3 arrays: VPS, SPS, PPS

#### AAC → esds box
**Function:** `build_esds_box()` (line ~728)

1. Parses ADTS header: `profile`, `freq_index`, `channel_config`
2. Builds `AudioSpecificConfig` (2 bytes)
3. Wraps in nested MPEG-4 descriptors:ES_Descriptor (tag 0x03)DecoderConfigDescriptor (tag 0x04, objectType=0x40 = AAC)DecoderSpecificInfo (tag 0x05) = AudioSpecificConfig
4. SLConfigDescriptor (tag 0x06)

---

### 10. Synthetic fMP4 Init Segment
**Function:** `build_init_segment_from_tracks()` (line ~320)

After codec discovery completes (all `codec_private` bytes extracted), a synthetic fMP4 initialization segment is built:

```
ftyp (isom, iso6, mp41)
moov
├── mvhd (movie header, timescale=1000)
├── trak (per track)
│   ├── tkhd (track header, width/height, track_id)
│   └── mdia
│       ├── mdhd (timescale: 90000 for video, 48000 for audio)
│       ├── hdlr ("vide" or "soun")
│       └── minf
│           ├── vmhd/smhd
│           ├── dinf → dref
│           └── stbl
│               ├── stsd → sample entry (avc1/hvc1/mp4a)
│               │           └── codec_private (avcC / hvcC / esds)
│               ├── stts (empty)
│               ├── stsc (empty)
│               ├── stsz (empty)
│               └── stco (empty)
└── mvex
    └── trex (per track, default sample description index = 1)
```
The stbl tables are empty because all timing lives in moof fragments (fragmented MP4).

---

### 11. moof + mdat Generation
**Function:** `build_moof_box()` (line ~817) and `build_fragment_from_sample()` (line ~1103)

Each `EsSample` becomes exactly **one moof+mdat pair** (CMAF per-sample fragment):

```
moof
├── mfhd (sequence_number — globally incrementing)
└── traf
    ├── tfhd (track_id, default-base-is-moof flag)
    ├── tfdt (version=1, 64-bit base_decode_time)
    └── trun (sample_count=1)
        ├── data_offset (points past moof into mdat payload)
        ├── sample_duration
        ├── sample_size
        ├── sample_flags (0x02000000 = sync, 0x00010000 = non-sync)
        └── sample_composition_time_offset (always 0 in this impl)
mdat
└── [sample bytes — length-prefixed NALUs or raw AAC]
```
**Timing model:**

- `base_decode_time` is derived from the sample's PTS relative to a shared origin (`base_pts90k`), converted to the track's timescale. This preserves A/V alignment.
- Audio `sample_duration` is always exactly **1024** (in the audio timescale) — the fixed AAC frame size.
- Video `sample_duration` is computed from PTS deltas: `(current_pts_us - last_pts_us) × timescale / 1,000,000`
- Timescale: 90000 for video, actual sample rate (e.g. 48000) for audio
- tfdt uses version 1 (64-bit) to avoid overflow during long streams

**trun flags = 0x000F01:**

- 0x000001 = data-offset-present
- 0x000100 = sample-duration-present
- 0x000200 = sample-size-present
- 0x000400 = sample-flags-present
- 0x000800 = sample-composition-time-offset-present

---

### 12. MediaFragment → MoQ Object
**Struct:** `MediaFragment` (in `cmaf_segmenter.h`)

```
struct MediaFragment {
    std::size_t group_id;
    std::size_t object_id;
    std::string track_name;      // e.g. "srt1_video" or "srt1_audio"
    std::uint64_t start_time_us;
    std::uint64_t duration_us;
    bool is_video_keyframe;
    PayloadBuffer payload;       // .owned_bytes = moof + mdat concatenated
};
```
The `FragmentSink` callback pushes each `MediaFragment` into a shared queue. The MoQ session's `drain_queue` loop consumes fragments and writes them as MoQ OBJECT messages on subgroup streams:

```
// In moqt_session.cpp drain_queue:
sender.serve(transport_, draft_version, track_alias, send_seq,
             object, /*new_subgroup=*/true, /*fin=*/false, payload);
```
Each fragment's `payload.owned_bytes` (the raw moof+mdat bytes) becomes the **MoQ object payload** — sent as-is on the wire.

---

### 13. MoQ group_id, subgroup_id, and object_id Assignment
**In `build_fragment_from_sample()`** (line ~1113):

```
group_id:
  - Starts at 0
  - Incremented on each VIDEO KEYFRAME (when fragment_on_keyframe=true)
  - All video and audio samples between keyframes share the same group_id
  - A new group = new CMAF segment boundary

subgroup_id:
  - Always 0 (single subgroup per group)

object_id:
  - Per-track counter within a group
  - Reset to 0 at each new group (on video keyframe)
  - Incremented for each sample of that track within the group
  - Video and audio have INDEPENDENT object_id sequences
```
**Example with 30fps video + 48kHz audio (1024-sample AAC frames, ~21ms):**

```
Group 0 (starts at keyframe):
  video object_id: 0, 1, 2, ... 59  (60 frames @ 30fps = 2 seconds)
  audio object_id: 0, 1, 2, ... 92  (~93 AAC frames in 2 seconds)

Group 1 (next keyframe):
  video object_id: 0, 1, 2, ...     (reset)
  audio object_id: 0, 1, 2, ...     (reset)
```

---

### 14. Frames Per MoQ Object
**Exactly 1 frame (or 1 AAC access unit) per MoQ object.**

The SRT path creates one moof+mdat per `EsSample`. Each `EsSample` is one complete access unit:

- **Video:** 1 coded picture (1 frame)
- **Audio:** 1 raw AAC frame (1024 PCM samples ≈ 21.3ms at 48kHz). Even when the encoder packs multiple AAC frames into a single MPEG-TS PES packet, `flush_pes()` splits them into individual `EsSample`s with interpolated timestamps — guaranteeing one access unit per MoQ object.

This is true CMAF "per-sample" fragmentation — the finest granularity possible.

---

## Comparison: SRT Path vs. Stdin fMP4 Path

Aspect | SRT Path | Stdin (fragmented MP4) Path
--- | --- | ---
**Input format** | Raw MPEG-TS over SRT | Fragmented MP4 (e.g. `ffmpeg -f mp4 -movflags frag_keyframe+empty_moov pipe:1`)
**Who creates moof+mdat** | moqxr builds it from scratch | FFmpeg (or other tool) creates it; moqxr forwards as-is
**Init segment** | Synthesized from extracted SPS/PPS/VPS/ADTS | Read directly from stdin (ftyp+moov boxes)
**Demuxing** | Full TS demux: PAT→PMT→PES→ES | MP4 box parser: reads top-level `moof` and `mdat` boxes
**Codec conversion** | Annex-B → length-prefixed; ADTS → raw AAC | None needed (already in MP4 format)
**Granularity** | Always 1 sample per moof+mdat | Depends on ffmpeg fragmentation settings (could be N samples per moof)
**Timing** | Derived from PTS relative to shared origin | Preserved from ffmpeg's trun entries
**group_id** | Incremented on video keyframe | Incremented on video keyframe
**object_id** | Per-track, reset each group | Per-track, reset each group

```
STDIN PATH:
  ffmpeg → [ftyp+moov] [moof+mdat] [moof+mdat] ...
                 │            │
                 │            └── Forwarded as MoQ Object payload (1:1)
                 └── Used as init segment

SRT PATH:
  Encoder → SRT → MPEG-TS → PAT/PMT/PES → EsSample
                                               │
                    ┌──────────────────────────┘
                    ▼
            annexb_to_avcc() / strip_adts()
                    │
                    ▼
            build_moof_box() + build_mdat_box()
                    │
                    ▼
            MoQ Object payload (1 frame per object)
```

---

## Key Data Structures

```
CallerTrackState (per SRT connection):
├── video_track_name / audio_track_name
├── video_track_id / audio_track_id
├── video_timescale (90000) / audio_timescale (48000)
├── group_id (increments on keyframe)
├── first_video_keyframe_seen
├── object_id_by_track (reset per group)
├── last_pts_by_track (for duration calculation)
├── base_pts90k (shared PTS origin for A/V timeline alignment)
├── last_duration_us_by_track (fallback for non-monotonic PTS)
├── moof_sequence (globally incrementing per connection)
└── video_codec (H264 or HEVC)
```

---

## Codec Discovery Phase
Before streaming begins, the system waits up to 5 seconds for:

1. First video frame → detect codec type (H.264 vs HEVC) from stream_type or NAL inspection
2. First video keyframe → extract SPS/PPS/VPS → build avcC or hvcC
3. First audio ADTS frame → extract sample rate / channels → build esds

After discovery:

- Tracks with no `codec_private` (e.g. audio when feed is video-only) are removed
- Init segment is rebuilt with actual codec parameters
- Catalog is generated and published as MoQ object (group=0, object=0 on "catalog" track)
