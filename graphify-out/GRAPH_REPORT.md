# Graph Report - .  (2026-07-02)

## Corpus Check
- 78 files · ~291,304 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 1349 nodes · 4061 edges · 64 communities (43 shown, 21 thin omitted)
- Extraction: 95% EXTRACTED · 5% INFERRED · 0% AMBIGUOUS · INFERRED: 190 edges (avg confidence: 0.81)
- Token cost: 194,363 input · 0 output

## Community Hubs (Navigation)
- [[_COMMUNITY_Publisher API Types|Publisher API Types]]
- [[_COMMUNITY_MoQT Control Messages|MoQT Control Messages]]
- [[_COMMUNITY_MoQT Session Core|MoQT Session Core]]
- [[_COMMUNITY_CMAF Segmenter|CMAF Segmenter]]
- [[_COMMUNITY_WebTransport Client|WebTransport Client]]
- [[_COMMUNITY_Picoquic Raw Client|Picoquic Raw Client]]
- [[_COMMUNITY_CMSF Packager|CMSF Packager]]
- [[_COMMUNITY_MP4 Box Parsing|MP4 Box Parsing]]
- [[_COMMUNITY_Publisher API Core|Publisher API Core]]
- [[_COMMUNITY_macOS Socket Shutdown Quirk|macOS Socket Shutdown Quirk]]
- [[_COMMUNITY_Segmenter Tests|Segmenter Tests]]
- [[_COMMUNITY_Auth Example CLI|Auth Example CLI]]
- [[_COMMUNITY_SRT Config JSON|SRT Config JSON]]
- [[_COMMUNITY_MPEG-TS Codec Detection|MPEG-TS Codec Detection]]
- [[_COMMUNITY_Publisher Transport Session|Publisher Transport Session]]
- [[_COMMUNITY_CLI Options|CLI Options]]
- [[_COMMUNITY_SRT fMP4 Box Builders|SRT fMP4 Box Builders]]
- [[_COMMUNITY_Picoquic Smoke Test|Picoquic Smoke Test]]
- [[_COMMUNITY_Catapult Client|Catapult Client]]
- [[_COMMUNITY_Token Encoding Helpers|Token Encoding Helpers]]
- [[_COMMUNITY_DASH Ingest Server Lifecycle|DASH Ingest Server Lifecycle]]
- [[_COMMUNITY_Chunked Body Decoder|Chunked Body Decoder]]
- [[_COMMUNITY_SRT Ingest Technical Note|SRT Ingest Technical Note]]
- [[_COMMUNITY_Video Dimension Parsing|Video Dimension Parsing]]
- [[_COMMUNITY_DASH Ingest Session|DASH Ingest Session]]
- [[_COMMUNITY_SRT Caller Track State|SRT Caller Track State]]
- [[_COMMUNITY_Publisher Docs and Guides|Publisher Docs and Guides]]
- [[_COMMUNITY_CI Tests and Templates|CI Tests and Templates]]
- [[_COMMUNITY_Test Box Helpers|Test Box Helpers]]
- [[_COMMUNITY_CMake Build Dependencies|CMake Build Dependencies]]
- [[_COMMUNITY_CI Workflow Matrix|CI Workflow Matrix]]
- [[_COMMUNITY_FFmpeg Protocol Mapping|FFmpeg Protocol Mapping]]
- [[_COMMUNITY_CMake Presets|CMake Presets]]
- [[_COMMUNITY_CTE LL-DASH Ingest Plan|CTE LL-DASH Ingest Plan]]
- [[_COMMUNITY_CAT4MOQ Auth Example|CAT4MOQ Auth Example]]
- [[_COMMUNITY_SRT Ingest Manager|SRT Ingest Manager]]
- [[_COMMUNITY_CAT4MOQ Token Wrapping|CAT4MOQ Token Wrapping]]
- [[_COMMUNITY_Transport Token Tests|Transport Token Tests]]
- [[_COMMUNITY_Project Overview|Project Overview]]
- [[_COMMUNITY_Transport Status|Transport Status]]
- [[_COMMUNITY_CAT4MOQ API Tests|CAT4MOQ API Tests]]
- [[_COMMUNITY_HEVC Param Sets|HEVC Param Sets]]
- [[_COMMUNITY_SRT Config Tests|SRT Config Tests]]
- [[_COMMUNITY_Auth Example Script|Auth Example Script]]
- [[_COMMUNITY_CAT4MOQ Header|CAT4MOQ Header]]
- [[_COMMUNITY_DASH Ingest Header|DASH Ingest Header]]
- [[_COMMUNITY_MoQ Draft Header|MoQ Draft Header]]
- [[_COMMUNITY_MP4 Box Header|MP4 Box Header]]
- [[_COMMUNITY_Publisher Namespace|Publisher Namespace]]
- [[_COMMUNITY_MoqtSession Node|MoqtSession Node]]
- [[_COMMUNITY_PublisherTransport Node|PublisherTransport Node]]
- [[_COMMUNITY_Reset Stream|Reset Stream]]
- [[_COMMUNITY_Request Stream Wait|Request Stream Wait]]
- [[_COMMUNITY_Code of Conduct|Code of Conduct]]
- [[_COMMUNITY_Cpp20 Requirement|Cpp20 Requirement]]
- [[_COMMUNITY_MoQ Draft 14 Spec|MoQ Draft 14 Spec]]
- [[_COMMUNITY_Fragmented MP4 Fast Path|Fragmented MP4 Fast Path]]
- [[_COMMUNITY_Progressive MP4 Remux|Progressive MP4 Remux]]
- [[_COMMUNITY_Request-Stream Semantics|Request-Stream Semantics]]
- [[_COMMUNITY_MoQ Transport Draft 14|MoQ Transport Draft 14]]
- [[_COMMUNITY_Publisher API Concept|Publisher API Concept]]
- [[_COMMUNITY_Psychedelic Example|Psychedelic Example]]
- [[_COMMUNITY_Raw QUIC ALPN|Raw QUIC ALPN]]
- [[_COMMUNITY_WebTransport HTTP3|WebTransport HTTP3]]

## God Nodes (most connected - your core abstractions)
1. `serve_subscriptions()` - 57 edges
2. `uint8_t` - 51 edges
3. `PicoquicClient()` - 50 edges
4. `MockTransport` - 48 edges
5. `uint8_t` - 46 edges
6. `forward_published_tracks()` - 38 edges
7. `publish_selected_tracks()` - 38 edges
8. `publish_live()` - 37 edges
9. `vector` - 36 edges
10. `publish_live_objects()` - 35 edges

## Surprising Connections (you probably didn't know these)
- `SRT Codec Discovery Phase` --conceptually_related_to--> `build_init_segment_from_tracks()`  [EXTRACTED]
  docs/srt-ingest-technical-note.md → src/live_srt_ingest.cpp
- `publisher()` --shares_data_with--> `PreparedPublish`  [EXTRACTED]
  include/openmoq/publisher/publisher_api.h → docs/publisher-api.md
- `Publisher::stats Publish Summary` --conceptually_related_to--> `publisher()`  [EXTRACTED]
  docs/publisher-api.md → include/openmoq/publisher/publisher_api.h
- `ActiveSubscription` --defines--> `Request Stream ID Retention`  [EXTRACTED]
  src/transport/moqt_session.cpp → docs/superpowers/plans/2026-05-13-request-stream-id-retention.md
- `LiveDashIngestSession()` --references--> `StreamingMp4Reader`  [EXTRACTED]
  src/live_dash_ingest.cpp → docs/ctedash-implementation-plan.md

## Import Cycles
- None detected.

## Hyperedges (group relationships)
- **CTE LL-DASH Ingest Pipeline** — src_live_dash_ingest_chunkedbodydecoder, src_live_dash_ingest_livedashingestsession, src_live_dash_ingest_livedashingestserver, docs_ctedash_implementation_plan_streamingmp4reader, publisher_publisher_api_publish_live_objects, publisher_publisher_api_liveobjectsource [EXTRACTED 1.00]
- **SRT MPEG-TS to MoQ Object Pipeline** — src_live_srt_ingest_tspesdemuxer, src_live_srt_ingest_essample, src_live_srt_ingest_annexb_to_avcc, src_live_srt_ingest_build_moof_box, src_live_srt_ingest_build_fragment_from_sample, publisher_cmaf_segmenter_mediafragment [EXTRACTED 1.00]
- **macOS accept()/shutdown() Fix (commit 65c5a4f)** — docs_macos_accept_shutdown_quirk_bounded_poll_accept_loop, docs_macos_accept_shutdown_quirk_nonblocking_listener, docs_macos_accept_shutdown_quirk_clear_inherited_o_nonblock, src_live_dash_ingest_livedashingestserver [EXTRACTED 1.00]
- **Three-OS CI Build-and-Test Matrix** — workflows_ci_ubuntu_latest_job, workflows_ci_macos_latest_job, workflows_ci_windows_latest_job, workflows_ci_build_and_test_matrix [EXTRACTED 1.00]
- **CAT4MOQ Token Issuance and Verification Flow** — auth_readme_catapult_issuer, auth_readme_token_wrapper, auth_readme_cat4moq_authorization, auth_readme_moqx_relay, cmakelists_auth_example_target [EXTRACTED 1.00]
- **Mutually Exclusive Live Ingest Paths** — readme_srt_ingest, readme_stdin_fmp4_ingest, readme_cte_lldash_ingest [EXTRACTED 1.00]

## Communities (64 total, 21 thin omitted)

### Community 0 - "Publisher API Types"
Cohesion: 0.06
Nodes (128): AuthorizationConfig, Key, LiveIngestOptions, MapLike, ofstream, OpenStream, PublishedObjectSink, PublishStats (+120 more)

### Community 1 - "MoQT Control Messages"
Cohesion: 0.10
Nodes (107): namespace, MaxRequestIdMessage, PublishNamespaceOk, PublishOk, RequestError, ServerSetupMessage, SetupMessage, DraftVersion (+99 more)

### Community 2 - "MoQT Session Core"
Cohesion: 0.06
Nodes (83): atomic, ConnectionState, function, namespace, MoqtSession, OpenEvent, pair, SubscribeMessage (+75 more)

### Community 3 - "CMAF Segmenter"
Cohesion: 0.11
Nodes (77): CmafObjectMode, int32_t, PayloadBuffer, append_ascii(), append_be32(), build_fragmented_init_segment(), build_live_fragment(), build_mvex_box() (+69 more)

### Community 4 - "WebTransport Client"
Cohesion: 0.05
Nodes (63): h3zero_stream_ctx_t, namespace, PendingOpen, picohttp_call_back_event_t, PendingWrite, ReceivedStreamData, condition_variable, ConnectionState (+55 more)

### Community 5 - "Picoquic Raw Client"
Cohesion: 0.05
Nodes (63): AcceptedWrite, namespace, condition_variable, ConnectionState, deque, EndpointConfig, Impl, map (+55 more)

### Community 6 - "CMSF Packager"
Cohesion: 0.10
Nodes (57): CmsfObjectKind, DraftProfile, namespace, LiveCatalog, Mp4Box, ostringstream, publisher(), SegmentedMp4 (+49 more)

### Community 7 - "MP4 Box Parsing"
Cohesion: 0.14
Nodes (56): append(), avc_codec_string(), ChunkMapEntry, first_chunk, samples_per_chunk, codec_from_stsd(), codec_string_from_sample_entry(), compact() (+48 more)

### Community 8 - "Publisher API Core"
Cohesion: 0.10
Nodes (42): ActiveSession, LiveIngestConfig, PreparedPublish, PublishError, PublisherStats, BatchPublishStats, bytes_published, groups_published (+34 more)

### Community 9 - "macOS Socket Shutdown Quirk"
Cohesion: 0.08
Nodes (35): macOS accept()/shutdown() Quirk Note, Bounded poll() Accept Loop, Clear Inherited O_NONBLOCK on Accepted Client fd, macOS CI Hang in openmoq-publisher-live-dash-tests, Non-Blocking Listening Socket, Portable Listener Shutdown Rules, shutdown() Does Not Wake accept() on macOS/BSD, SO_RCVTIMEO Receive Polling Cadence (+27 more)

### Community 10 - "Segmenter Tests"
Cohesion: 0.23
Nodes (35): array, append_ascii(), append_be32(), base64_decode(), be32_bytes(), bytes_equal(), catalog_init_data(), concat() (+27 more)

### Community 11 - "Auth Example CLI"
Cohesion: 0.10
Nodes (35): Args, action_token_file, catapult_command, draft, endpoint, forward, insecure_skip_verify, seconds (+27 more)

### Community 12 - "SRT Config JSON"
Cohesion: 0.15
Nodes (22): namespace, JsonArray, JsonObject, LiveSrtConfig, nullptr_t, publisher(), optional, path (+14 more)

### Community 13 - "MPEG-TS Codec Detection"
Cohesion: 0.10
Nodes (28): PesBuffer, build_h264_codec_private(), function, optional, span, TransportStatus, detect_video_codec_from_annexb(), detect_video_codec_from_stream_type() (+20 more)

### Community 14 - "Publisher Transport Session"
Cohesion: 0.09
Nodes (24): PublisherTransport, PublisherTransport, Publisher::ActiveSession, session, transport, State, ConnectionState, EndpointConfig (+16 more)

### Community 15 - "CLI Options"
Cohesion: 0.12
Nodes (30): InputSource, LiveSourceKind, build_usage(), CliOptions, DraftVersion, EndpointConfig, pair, seconds (+22 more)

### Community 16 - "SRT fMP4 Box Builders"
Cohesion: 0.34
Nodes (28): append_ascii(), append_be16(), append_be32(), build_avcc_box(), build_dinf_box(), build_empty_time_table(), build_esds_box(), build_ftyp_box() (+20 more)

### Community 17 - "Picoquic Smoke Test"
Cohesion: 0.11
Nodes (28): condition_variable, mutex, picoquic_call_back_event_t, picoquic_cnx_t, picoquic_packet_loop_cb_enum, picoquic_quic_t, PublishPlan, size_t (+20 more)

### Community 18 - "Catapult Client"
Cohesion: 0.12
Nodes (22): auth(), CatapultClient(), CatapultClient, namespace, DraftVersion, string, FILE, int_type (+14 more)

### Community 19 - "Token Encoding Helpers"
Cohesion: 0.24
Nodes (22): base64_value(), decode_base64(), decode_hex(), decode_token_bytes(), expand_command(), hex_value(), issue_token(), looks_textual_token() (+14 more)

### Community 20 - "DASH Ingest Server Lifecycle"
Cohesion: 0.18
Nodes (18): LiveTrack, LiveObjectSource, milliseconds, finished(), snapshot_tracks_locked(), source(), wait_for_tracks(), as_string() (+10 more)

### Community 21 - "Chunked Body Decoder"
Cohesion: 0.15
Nodes (21): append(), complete(), map, span, string, uint8_t, vector, fail() (+13 more)

### Community 22 - "SRT Ingest Technical Note"
Cohesion: 0.16
Nodes (19): SRT Live Ingest to MoQ Publishing Technical Note, Accumulation-Based tfdt Timing Model, SRT Codec Discovery Phase, LiveSrtCallerRuntimeConfig, MediaFragment, MediaFragment, annexb_to_avcc(), append_be64() (+11 more)

### Community 23 - "Video Dimension Parsing"
Cohesion: 0.22
Nodes (12): pair, size_t, uint32_t, parse_h264_dimensions(), parse_hevc_dimensions(), RbspBitReader, bit_pos_, byte_pos_ (+4 more)

### Community 24 - "DASH Ingest Session"
Cohesion: 0.18
Nodes (17): namespace, LiveObject, PathState, publisher(), build_catalog_locked(), optional, string_view, enqueue_locked() (+9 more)

### Community 25 - "SRT Caller Track State"
Cohesion: 0.12
Nodes (17): CallerTrackState, audio_timescale, audio_track_id, audio_track_name, base_pts90k, decode_time_by_track, first_video_keyframe_seen, group_id (+9 more)

### Community 26 - "Publisher Docs and Guides"
Cohesion: 0.17
Nodes (15): CAT4MOQ Authorization, Publisher API Guide, Publisher API Guide (Spanish translation), Publisher API Guide (French translation), Publisher API Guide (Portuguese translation), Quick Start Guide, Relay Interoperability Guide, --forward 0/1 Publish Semantics (+7 more)

### Community 27 - "CI Tests and Templates"
Cohesion: 0.15
Nodes (15): CTest Unit Test Suite, openmoq-publisher-live-dash-tests Target, Pull Request Template, Bug Report Issue Template, Feature Request Issue Template, CTE LL-DASH/CMAF Live Ingest, OpenMOQ Publisher README (Spanish), OpenMOQ Publisher README (Italian) (+7 more)

### Community 28 - "Test Box Helpers"
Cohesion: 0.47
Nodes (13): append_ascii(), append_be32(), be32(), concat(), initializer_list, string_view, uint32_t, uint8_t (+5 more)

### Community 29 - "CMake Build Dependencies"
Cohesion: 0.23
Nodes (12): Top-Level CMake Build Configuration, openmoq-publisher CLI Executable Target, Optional libsrt Ingest Support, Build Guide, openmoq_publisher_lib Static Library, picoquic Transport Dependency, picotls TLS Dependency, WebTransport Compliance Notes (+4 more)

### Community 30 - "CI Workflow Matrix"
Cohesion: 0.23
Nodes (12): picoquic Transport Dependency, picotls TLS Dependency, picotls Link-Directory Sanitization Workaround, build-and-test CI Matrix, CI build-and-test (macos-latest), CI picoquic Source Checkout Step, CI picotls Source Checkout Step, CI Workflow (ci.yml) (+4 more)

### Community 31 - "FFmpeg Protocol Mapping"
Cohesion: 0.25
Nodes (11): FFmpeg Input Recipes, Fragmented MP4 (CMAF) Input Fast Path, HEVC hev1-to-hvc1 Normalization, +separate_moof Track-Separated Fragments, Protocol Mapping Notes, CMAF-to-MOQT Packaging Model, MOQT Draft-14 Primary Profile, MOQT Draft-16 Compatibility Profile (+3 more)

### Community 32 - "CMake Presets"
Cohesion: 0.22
Nodes (8): buildPresets, cmakeMinimumRequired, major, minor, patch, configurePresets, testPresets, version

### Community 33 - "CTE LL-DASH Ingest Plan"
Cohesion: 0.33
Nodes (9): CTE LL-DASH Ingest Implementation Plan, CTE LL-DASH Ingest Mode, StreamingMp4Reader, SRT MPEG-TS Ingest Pipeline, LiveObjectSource, Publisher::publish_live_objects, ChunkedBodyDecoder(), size_t (+1 more)

### Community 34 - "CAT4MOQ Auth Example"
Cohesion: 0.43
Nodes (8): CAT4MOQ Auth Example Implementation Plan, Public API Boundary for Auth Types, CAT4MOQ Auth Example README, Catapult CAT4MOQ Token Issuer, moqx Sibling Relay, MoQ AUTHORIZATION_TOKEN Wrapping, openmoq-publisher-auth-example Target, openmoq-publisher-psychedelic-example Target

### Community 35 - "SRT Ingest Manager"
Cohesion: 0.29
Nodes (7): FragmentSink, atomic, string, uint16_t, LiveSrtIngestManager(), make_track(), split_host_port()

### Community 36 - "CAT4MOQ Token Wrapping"
Cohesion: 0.67
Nodes (6): AuthorizationToken, span, uint8_t, wrap_cat_token(), wrap_out_of_band_token(), wrap_token()

### Community 37 - "Transport Token Tests"
Cohesion: 0.52
Nodes (6): contains_subsequence(), uint8_t, vector, main(), test_publish_namespace_includes_auth_token(), test_setup_includes_auth_token()

### Community 38 - "Project Overview"
Cohesion: 0.40
Nodes (5): openmoq_publisher, picoquic, picotls, Publisher C++ API, moqxr

### Community 39 - "Transport Status"
Cohesion: 0.50
Nodes (4): string_view, TransportStatus, failure(), success()

### Community 40 - "CAT4MOQ API Tests"
Cohesion: 0.70
Nodes (4): main(), test_cat_token_wrapper(), test_out_of_band_token_wrapper(), test_publisher_config_auth_defaults()

### Community 41 - "HEVC Param Sets"
Cohesion: 0.50
Nodes (4): HevcParamSets, pps, sps, vps

### Community 42 - "SRT Config Tests"
Cohesion: 0.83
Nodes (3): string, expect(), main()

## Knowledge Gaps
- **345 isolated node(s):** `version`, `major`, `minor`, `patch`, `configurePresets` (+340 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **21 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `array` connect `Segmenter Tests` to `MoQT Control Messages`, `MP4 Box Parsing`, `SRT fMP4 Box Builders`, `Token Encoding Helpers`, `DASH Ingest Server Lifecycle`, `SRT Ingest Technical Note`?**
  _High betweenness centrality (0.213) - this node is a cross-community bridge._
- **Why does `disconnect()` connect `Publisher API Core` to `WebTransport Client`, `Picoquic Raw Client`?**
  _High betweenness centrality (0.166) - this node is a cross-community bridge._
- **Why does `thread` connect `macOS Socket Shutdown Quirk` to `SRT fMP4 Box Builders`, `Publisher API Types`, `MoQT Session Core`, `DASH Ingest Server Lifecycle`?**
  _High betweenness centrality (0.127) - this node is a cross-community bridge._
- **Are the 9 inferred relationships involving `serve_subscriptions()` (e.g. with `encode_subscribe_message()` and `encode_subscribe_namespace_message()`) actually correct?**
  _`serve_subscriptions()` has 9 INFERRED edges - model-reasoned connections that need verification._
- **What connects `version`, `major`, `minor` to the rest of the system?**
  _347 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `Publisher API Types` be split into smaller, more focused modules?**
  _Cohesion score 0.0619939856581078 - nodes in this community are weakly interconnected._
- **Should `MoQT Control Messages` be split into smaller, more focused modules?**
  _Cohesion score 0.09553478712357218 - nodes in this community are weakly interconnected._