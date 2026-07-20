# FFmpeg Input Recipes

The publisher's preferred fast path is already fragmented MP4 input. You can generate that with `ffmpeg` by copying compatible AAC-LC, H.264, or HEVC streams and enabling CMAF-style fragmentation flags.

## Fragment an Existing MP4

```bash
ffmpeg -i input.mp4 \
  -map 0:v -map 0:a \
  -map_metadata -1 \
  -sn -dn \
  -c:v copy \
  -c:a copy \
  -movflags +frag_keyframe+empty_moov+default_base_moof+separate_moof \
  -f mp4 fragmented.mp4
```

## Re-Encode to Compatible H.264 or HEVC

If the source codecs are not already compatible, re-encode instead of copying.

H.264:

```bash
ffmpeg -i bbb_sunflower_2160p_60fps_normal.mp4 \
  -map 0:v:0 -map 0:a:0 \
  -map_metadata -1 \
  -sn -dn \
  -c:v libx264 -preset medium -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
  -c:a aac -b:a 160k -ar 48000 -ac 2 \
  -movflags +frag_keyframe+empty_moov+default_base_moof+separate_moof \
  -f mp4 sunflower-frag.mp4
```

HEVC:

```bash
ffmpeg -i bbb_sunflower_2160p_60fps_normal.mp4 \
  -map 0:v:0 -map 0:a:0 \
  -map_metadata -1 \
  -sn -dn \
  -c:v libx265 -preset medium -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
  -c:a aac -b:a 160k -ar 48000 -ac 2 \
  -movflags +frag_keyframe+empty_moov+default_base_moof+separate_moof \
  -f mp4 sunflower265-frag.mp4
```

## Practical Notes

- `-map 0:v -map 0:a` keeps only video and audio streams, excluding subtitle and other non-A/V tracks
- `-map 0:v:0 -map 0:a:0` uses the first audio stream if multiple exist
- `-sn -dn` explicitly disables subtitle and data or text streams
- `-map_metadata -1` drops container-level metadata from the output
- `+frag_keyframe` starts a new fragment on keyframes
- `+empty_moov` writes initialization metadata up front
- `+default_base_moof` and `+separate_moof` produce a layout that is easier for fragmented-MP4 pipelines to consume
- omit `+separate_moof` only if you are sure downstream tooling can parse interleaved multi-track fragments
- when audio appears in the catalog but no audio media objects are sent, regenerate with `+separate_moof`
- for HEVC, prefer streams that are already `hvc1`-compatible
- if a source is tagged `hev1` but keeps VPS/SPS/PPS only in the init segment, the publisher normalizes the advertised codec and emitted init segment to `hvc1`
- if HEVC samples include in-band parameter sets, the publisher preserves `hev1`
- if you start from a progressive MP4, this project can remux it internally, but pre-fragmented input is simpler and more efficient

## Live Fragmented MP4 Stdin Publishing

For live encoder pipelines, the publisher can consume fragmented MP4 directly from standard input.

This live path expects ffmpeg to emit track-separated fragments, where each `moof` + `mdat` pair belongs to a single media track. Use `+separate_moof` when generating the stream. Without `+separate_moof`, audio and video may be carried inside the same `moof`, which is not the intended input layout for the current live parser.

```bash
ffmpeg -stream_loop -1 -re -i bbb_sunflower_1080p_30fps_normal.mp4 \
  -map 0:v:0 -map 0:a:0 \
  -c:v libx264 -preset medium -r 30 -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 \
  -c:a aac -b:a 160k -ar 48000 -ac 2 \
  -movflags +frag_keyframe+empty_moov+default_base_moof+separate_moof \
  -f mp4 - | ./build/openmoq-publisher \
    --input - \
    --endpoint moqt://relay.example.com:443/moq \
    --namespace live/demo \
    --timeout 120
```

## CTE LL-DASH HTTP Ingest

The DASH live path lets FFmpeg send CMAF/fMP4 over HTTP/1.1 chunked requests instead of piping fragmented MP4 through stdin. Start the publisher first:

```bash
./build/openmoq-publisher \
  --live-source dash \
  --dash-listen 0.0.0.0:8080 \
  --dash-path /ingest \
  --endpoint https://127.0.0.1:4433/moq \
  --transport webtransport \
  --namespace live \
  --draft 18 \
  --publish-catalog \
  --forward 1 \
  --insecure
```

Then push a live DASH feed to the ingest prefix:

```bash
ffmpeg -re \
  -f lavfi -i "testsrc2=size=1280x720:rate=25" \
  -f lavfi -i "anullsrc=r=48000:cl=stereo" \
  -filter_complex "[0:v]split=2[v1][v2];[v1]scale=1280:720[v720];[v2]scale=640:360[v360]" \
  -map "[v720]" -c:v:0 libx264 -b:v:0 1500k -g 50 -keyint_min 50 -sc_threshold 0 \
  -map "[v360]" -c:v:1 libx264 -b:v:1 500k -g 50 -keyint_min 50 -sc_threshold 0 \
  -map 1:a -c:a aac -b:a 128k \
  -f dash -seg_duration 2 -use_template 1 -use_timeline 0 \
  -init_seg_name 'video$RepresentationID$' \
  -media_seg_name 'video$RepresentationID$' \
  -adaptation_sets "id=0,streams=v id=1,streams=a" \
  -multiple_requests 1 -streaming 1 -remove_at_exit 0 \
  -window_size 20 -extra_window_size 20 \
  http://127.0.0.1:8080/ingest/
```

With this naming pattern, FFmpeg sends representation requests under the ingest prefix, such as `/ingest/video0`, `/ingest/video1`, and `/ingest/video2`. The publisher treats each representation path independently, discovers tracks from init segments sent on those paths, and emits catalog plus media objects for subscribers.

For relay smoke testing, use `--forward 1` so objects are forwarded immediately. Use `--forward 0` when the relay should wait for subscriber interest before media delivery. In that mode, `connection_id=` confirms transport and MOQT setup only; it does not mean the relay accepted the namespace or forwarded a subscription. Draft-16 subscriptions arrive on the control stream, while draft-17/18 subscriptions arrive on bidirectional request streams and are acknowledged with `SUBSCRIBE_OK` on the same stream. Use `--timeout` to bound the initial wait and `OPENMOQ_PICOQUIC_TRACE=1` to inspect relay control traffic.
