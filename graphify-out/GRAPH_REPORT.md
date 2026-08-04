# Graph Report - .  (2026-08-04)

## Corpus Check
- 90 files · ~538,039 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 1744 nodes · 5385 edges · 75 communities (45 shown, 30 thin omitted)
- Extraction: 96% EXTRACTED · 4% INFERRED · 0% AMBIGUOUS · INFERRED: 242 edges (avg confidence: 0.8)
- Token cost: 348,265 input · 13,432 output

## Community Hubs (Navigation)
- [[_COMMUNITY_Publisher Session Core|Publisher Session Core]]
- [[_COMMUNITY_SRT Ingest Pipeline|SRT Ingest Pipeline]]
- [[_COMMUNITY_MOQT Control Messages|MOQT Control Messages]]
- [[_COMMUNITY_Libmoq Translation|Libmoq Translation]]
- [[_COMMUNITY_MOQT Session Tests|MOQT Session Tests]]
- [[_COMMUNITY_CMAF Segment Generation|CMAF Segment Generation]]
- [[_COMMUNITY_CMAF Segment Tests|CMAF Segment Tests]]
- [[_COMMUNITY_MSF Catalog Model|MSF Catalog Model]]
- [[_COMMUNITY_WebTransport Client Internals|WebTransport Client Internals]]
- [[_COMMUNITY_Picoquic Client Internals|Picoquic Client Internals]]
- [[_COMMUNITY_CLI and Config Parsing|CLI and Config Parsing]]
- [[_COMMUNITY_MP4 Track Parsing|MP4 Track Parsing]]
- [[_COMMUNITY_Auth Example CLI|Auth Example CLI]]
- [[_COMMUNITY_MSF URL Handling|MSF URL Handling]]
- [[_COMMUNITY_CMSF Catalog Packaging|CMSF Catalog Packaging]]
- [[_COMMUNITY_Publisher API Runtime|Publisher API Runtime]]
- [[_COMMUNITY_MSFTS CLI Options|MSFTS CLI Options]]
- [[_COMMUNITY_MSFTS Live Source|MSFTS Live Source]]
- [[_COMMUNITY_CENC Protection|CENC Protection]]
- [[_COMMUNITY_DASH Stream Reader|DASH Stream Reader]]
- [[_COMMUNITY_Publisher API Tests|Publisher API Tests]]
- [[_COMMUNITY_DASH HTTP Ingest|DASH HTTP Ingest]]
- [[_COMMUNITY_Token Encoding|Token Encoding]]
- [[_COMMUNITY_MP4 Box Builders|MP4 Box Builders]]
- [[_COMMUNITY_Picoquic Smoke Server|Picoquic Smoke Server]]
- [[_COMMUNITY_Picoquic Smoke Tests|Picoquic Smoke Tests]]
- [[_COMMUNITY_CMSF Protection Architecture|CMSF Protection Architecture]]
- [[_COMMUNITY_DASH HTTP Tests|DASH HTTP Tests]]
- [[_COMMUNITY_Live Object Queue|Live Object Queue]]
- [[_COMMUNITY_CAT4MOQ Example Design|CAT4MOQ Example Design]]
- [[_COMMUNITY_Dependency Build Overview|Dependency Build Overview]]
- [[_COMMUNITY_CMake Presets|CMake Presets]]
- [[_COMMUNITY_Live Ingest Architecture|Live Ingest Architecture]]
- [[_COMMUNITY_macOS Listener Portability|macOS Listener Portability]]
- [[_COMMUNITY_Publisher Transport Lifecycle|Publisher Transport Lifecycle]]
- [[_COMMUNITY_CAT4MOQ Token Wrapping|CAT4MOQ Token Wrapping]]
- [[_COMMUNITY_CAT4MOQ Transport Tests|CAT4MOQ Transport Tests]]
- [[_COMMUNITY_Catalog and Compliance|Catalog and Compliance]]
- [[_COMMUNITY_Draft Version Handling|Draft Version Handling]]
- [[_COMMUNITY_Streaming MP4 Reader|Streaming MP4 Reader]]
- [[_COMMUNITY_HTTP Request Parsing|HTTP Request Parsing]]
- [[_COMMUNITY_Transport Status Helpers|Transport Status Helpers]]
- [[_COMMUNITY_CAT4MOQ API Tests|CAT4MOQ API Tests]]
- [[_COMMUNITY_SRT Config Tests|SRT Config Tests]]
- [[_COMMUNITY_CAT4MOQ Run Script|CAT4MOQ Run Script]]
- [[_COMMUNITY_CAT4MOQ Public Header|CAT4MOQ Public Header]]
- [[_COMMUNITY_CMAF Segment Header|CMAF Segment Header]]
- [[_COMMUNITY_DRM Config Header|DRM Config Header]]
- [[_COMMUNITY_Draft Version Header|Draft Version Header]]
- [[_COMMUNITY_MSF URL Header|MSF URL Header]]
- [[_COMMUNITY_Publisher Authorization API|Publisher Authorization API]]
- [[_COMMUNITY_Publisher Namespace|Publisher Namespace]]
- [[_COMMUNITY_Broadcast End API|Broadcast End API]]
- [[_COMMUNITY_Publisher Transport Class|Publisher Transport Class]]
- [[_COMMUNITY_Stream Reset Handling|Stream Reset Handling]]
- [[_COMMUNITY_Request Stream Synchronization|Request Stream Synchronization]]
- [[_COMMUNITY_Code of Conduct|Code of Conduct]]
- [[_COMMUNITY_C++20 Requirement|C++20 Requirement]]
- [[_COMMUNITY_MOQT Draft 14|MOQT Draft 14]]
- [[_COMMUNITY_Fragmented MP4 Path|Fragmented MP4 Path]]
- [[_COMMUNITY_Progressive MP4 Remux|Progressive MP4 Remux]]
- [[_COMMUNITY_Request Stream Semantics|Request Stream Semantics]]
- [[_COMMUNITY_FFmpeg Recipes|FFmpeg Recipes]]
- [[_COMMUNITY_Pull Request Template|Pull Request Template]]
- [[_COMMUNITY_Bug Report Template|Bug Report Template]]
- [[_COMMUNITY_Feature Request Template|Feature Request Template]]
- [[_COMMUNITY_Project Roadmap|Project Roadmap]]
- [[_COMMUNITY_Protocol Mapping|Protocol Mapping]]
- [[_COMMUNITY_C++ Publisher API|C++ Publisher API]]
- [[_COMMUNITY_Psychedelic Example|Psychedelic Example]]
- [[_COMMUNITY_Publisher API Guide|Publisher API Guide]]
- [[_COMMUNITY_Quick Start Guide|Quick Start Guide]]
- [[_COMMUNITY_Raw MOQT QUIC|Raw MOQT QUIC]]
- [[_COMMUNITY_HTTP3 WebTransport|HTTP3 WebTransport]]

## God Nodes (most connected - your core abstractions)
1. `serve_subscriptions()` - 58 edges
2. `uint8_t` - 51 edges
3. `PicoquicClient()` - 49 edges
4. `MockTransport` - 49 edges
5. `uint8_t` - 46 edges
6. `main()` - 46 edges
7. `publish_live()` - 42 edges
8. `vector` - 42 edges
9. `uint8_t` - 41 edges
10. `MsfCatalog` - 38 edges

## Surprising Connections (you probably didn't know these)
- `WebTransport Compliance Notes` --conceptually_related_to--> `MoqtSession`  [INFERRED]
  docs/webtransport-compliance.md → src/moqt_session.cpp
- `Testing` --references--> `src/cmsf_packager.cpp`  [INFERRED]
  docs/testing.md → src/cmsf_packager.cpp
- `ActiveSubscription` --defines--> `Request Stream ID Retention`  [EXTRACTED]
  src/transport/moqt_session.cpp → docs/superpowers/plans/2026-05-13-request-stream-id-retention.md
- `openmoq/publisher/cenc.h` --references--> `picoquic`  [EXTRACTED]
  include/openmoq/publisher/cenc.h → CMakeLists.txt
- `openmoq/publisher/cenc.h` --references--> `moq5 (libmoq)`  [EXTRACTED]
  include/openmoq/publisher/cenc.h → CMakeLists.txt

## Import Cycles
- None detected.

## Communities (75 total, 30 thin omitted)

### Community 0 - "Publisher Session Core"
Cohesion: 0.06
Nodes (143): AuthorizationConfig, Key, LiveIngestOptions, MapLike, ofstream, OpenStream, PublishedObjectSink, PublishStats (+135 more)

### Community 1 - "SRT Ingest Pipeline"
Cohesion: 0.05
Nodes (113): FragmentSink, PesBuffer, annexb_to_avcc(), append_ascii(), append_be16(), append_be32(), append_be64(), build_avcc_box() (+105 more)

### Community 2 - "MOQT Control Messages"
Cohesion: 0.09
Nodes (109): namespace, MaxRequestIdMessage, encode_namespace_name, PublishOk, RequestError, ServerSetupMessage, SetupMessage, DraftVersion (+101 more)

### Community 3 - "Libmoq Translation"
Cohesion: 0.06
Nodes (101): namespace, LibmoqDemandOps, LibmoqDemandOutcome, LibmoqLiveHandle, LibmoqObjectTranslation, LibmoqPlanTranslation, LibmoqPublishStats, LibmoqReadyOps (+93 more)

### Community 4 - "MOQT Session Tests"
Cohesion: 0.06
Nodes (88): OpenEvent, PublishNamespaceOk, SubscribeTracksMessage, all_init_refs_resolve(), append_be16(), append_be32(), append_bytes(), bytes_equal() (+80 more)

### Community 5 - "CMAF Segment Generation"
Cohesion: 0.11
Nodes (89): CmafObjectMode, PayloadBuffer, uint32_t, read_be32_at(), append_ascii(), append_be32(), append_coalesced_fragments(), build_fragmented_init_segment() (+81 more)

### Community 6 - "CMAF Segment Tests"
Cohesion: 0.13
Nodes (70): all_init_refs_resolve(), append_ascii(), append_be32(), append_be64(), base64_decode(), be32_bytes(), bytes_equal(), concat() (+62 more)

### Community 7 - "MSF Catalog Model"
Cohesion: 0.10
Nodes (61): namespace, EndBroadcastMode, MsfContentProtection, MsfTrack, ostringstream, publisher(), attach_content_protection(), attach_init_data() (+53 more)

### Community 8 - "WebTransport Client Internals"
Cohesion: 0.05
Nodes (63): h3zero_stream_ctx_t, namespace, PendingOpen, picohttp_call_back_event_t, PendingWrite, ReceivedStreamData, condition_variable, ConnectionState (+55 more)

### Community 9 - "Picoquic Client Internals"
Cohesion: 0.05
Nodes (63): AcceptedWrite, namespace, condition_variable, ConnectionState, deque, EndpointConfig, Impl, map (+55 more)

### Community 10 - "CLI and Config Parsing"
Cohesion: 0.06
Nodes (60): namespace, InputSource, JsonArray, JsonValue, LiveSourceKind, LiveSrtConfig, publisher(), build_usage() (+52 more)

### Community 11 - "MP4 Track Parsing"
Cohesion: 0.13
Nodes (61): append(), avc_codec_string(), bcp47_from_iso639_2(), bitrate_from_sample_entry(), BitrateInfo, avg_bitrate, max_bitrate, ChunkMapEntry (+53 more)

### Community 12 - "Auth Example CLI"
Cohesion: 0.06
Nodes (55): Args, action_token_file, catapult_command, draft, endpoint, forward, insecure_skip_verify, seconds (+47 more)

### Community 13 - "MSF URL Handling"
Cohesion: 0.09
Nodes (53): Callable, ConnectionRequirement, build_msf_url, decode_namespace_name, MsfTrackIdentifier, MsfUrl, parse_msf_url, MsfLocation (+45 more)

### Community 14 - "CMSF Catalog Packaging"
Cohesion: 0.11
Nodes (54): CmsfObjectKind, namespace, RegisteredTrack, LiveCatalog, CatalogObject, CatalogPublisher, MsfCatalog, MsfDeltaOp (+46 more)

### Community 15 - "Publisher API Runtime"
Cohesion: 0.09
Nodes (45): ActiveSession, namespace, namespace, LiveIngestConfig, PreparedPublish, PublishError, PublisherStats, BatchPublishStats (+37 more)

### Community 16 - "MSFTS CLI Options"
Cohesion: 0.13
Nodes (38): DraftVersion, EndpointConfig, string, string_view, uint64_t, vector, namespace, class (+30 more)

### Community 17 - "MSFTS Live Source"
Cohesion: 0.16
Nodes (31): bitset, optional, size_t, string, uint16_t, uint32_t, uint8_t, unique_ptr (+23 more)

### Community 18 - "CENC Protection"
Cohesion: 0.17
Nodes (29): base64_encode(), ByteSpan, CencSystem, CencTrackProtection, Mp4Box, optional, size_t, span (+21 more)

### Community 19 - "DASH Stream Reader"
Cohesion: 0.15
Nodes (26): PathState, append(), complete(), milliseconds, span, StreamingBoxResult, string, string_view (+18 more)

### Community 20 - "Publisher API Tests"
Cohesion: 0.11
Nodes (19): PublisherTransport, State, ConnectionState, EndpointConfig, milliseconds, shared_ptr, span, StreamDirection (+11 more)

### Community 21 - "DASH HTTP Ingest"
Cohesion: 0.09
Nodes (28): LiveDashIngestConfig, bound_port(), ChunkedBodyDecoder(), close(), close_fd(), mutex, set, size_t (+20 more)

### Community 22 - "Token Encoding"
Cohesion: 0.24
Nodes (22): token_encoding, base64_value(), decode_base64(), decode_hex(), decode_token_bytes(), expand_command(), hex_value(), issue_token() (+14 more)

### Community 23 - "MP4 Box Builders"
Cohesion: 0.33
Nodes (23): append_ascii(), append_be32(), be32(), be64(), concat(), initializer_list, string_view, uint32_t (+15 more)

### Community 24 - "Picoquic Smoke Server"
Cohesion: 0.11
Nodes (18): picoquic_quic_t, condition_variable, mutex, size_t, SmokeServer, bytes_received, condition, control_bytes (+10 more)

### Community 25 - "Picoquic Smoke Tests"
Cohesion: 0.25
Nodes (14): picoquic_call_back_event_t, picoquic_cnx_t, picoquic_packet_loop_cb_enum, PublishPlan, string, thread, expect(), main() (+6 more)

### Community 26 - "CMSF Protection Architecture"
Cohesion: 0.18
Nodes (10): src/cmaf_segmenter.cpp, CMSF Content Protection, src/cmsf_packager.cpp, namespace, class, namespace, src/live_dash_ingest.cpp, publisher() (+2 more)

### Community 27 - "DASH HTTP Tests"
Cohesion: 0.28
Nodes (13): as_string(), close_socket(), size_t, span, string, uint16_t, expect(), expect_chunked_decodes() (+5 more)

### Community 28 - "Live Object Queue"
Cohesion: 0.21
Nodes (11): build_catalog_locked(), LiveObject, LiveObjectSource, LiveTrack, optional, vector, finished(), next_object_blocking() (+3 more)

### Community 29 - "CAT4MOQ Example Design"
Cohesion: 0.31
Nodes (10): CAT4MOQ Auth Example Implementation Plan, Public API Boundary for Auth Types, CAT4MOQ Authorization, CAT4MOQ Auth Example README, Catapult CAT4MOQ Token Issuer, moqx Sibling Relay, MoQ AUTHORIZATION_TOKEN Wrapping, Relay Interoperability Guide (+2 more)

### Community 30 - "Dependency Build Overview"
Cohesion: 0.31
Nodes (9): Build Documentation, openmoq/publisher/cenc.h, moq5 (libmoq), picoquic, picotls, CMSF: a CMAF compliant implementation of MOQT Streaming Format, MOQT Streaming Format, Media over QUIC Transport (+1 more)

### Community 31 - "CMake Presets"
Cohesion: 0.22
Nodes (8): buildPresets, cmakeMinimumRequired, major, minor, patch, configurePresets, testPresets, version

### Community 32 - "Live Ingest Architecture"
Cohesion: 0.25
Nodes (8): CTE LL-DASH Ingest Implementation Plan, CTE LL-DASH Ingest Mode, StreamingMp4Reader, SRT Live Ingest to MoQ Publishing Technical Note, Accumulation-Based tfdt Timing Model, SRT Codec Discovery Phase, SRT MPEG-TS Ingest Pipeline, MediaFragment

### Community 33 - "macOS Listener Portability"
Cohesion: 0.39
Nodes (8): macOS accept()/shutdown() Quirk Note, Bounded poll() Accept Loop, Clear Inherited O_NONBLOCK on Accepted Client fd, macOS CI Hang in openmoq-publisher-live-dash-tests, Non-Blocking Listening Socket, Portable Listener Shutdown Rules, shutdown() Does Not Wake accept() on macOS/BSD, SO_RCVTIMEO Receive Polling Cadence

### Community 34 - "Publisher Transport Lifecycle"
Cohesion: 0.25
Nodes (8): namespace, MoqtSession, PublisherTransport, unique_ptr, Publisher::ActiveSession, session, transport, transport()

### Community 35 - "CAT4MOQ Token Wrapping"
Cohesion: 0.67
Nodes (6): AuthorizationToken, span, uint8_t, wrap_cat_token(), wrap_out_of_band_token(), wrap_token()

### Community 36 - "CAT4MOQ Transport Tests"
Cohesion: 0.52
Nodes (6): contains_subsequence(), uint8_t, vector, main(), test_publish_namespace_includes_auth_token(), test_setup_includes_auth_token()

### Community 37 - "Catalog and Compliance"
Cohesion: 0.33
Nodes (6): CatalogPublisher, MoqtSession, src/msf_catalog.cpp, openmoq/publisher/msf_catalog.h, MSF v1 Catalog, WebTransport Compliance Notes

### Community 38 - "Draft Version Handling"
Cohesion: 0.53
Nodes (5): DraftProfile, DraftVersion, string, default_alpn(), to_string()

### Community 39 - "Streaming MP4 Reader"
Cohesion: 0.40
Nodes (4): class, namespace, publisher(), StreamingMp4Reader()

### Community 40 - "HTTP Request Parsing"
Cohesion: 0.40
Nodes (5): map, ParsedRequest, headers, method, path

### Community 41 - "Transport Status Helpers"
Cohesion: 0.50
Nodes (4): string_view, TransportStatus, failure(), success()

### Community 42 - "CAT4MOQ API Tests"
Cohesion: 0.70
Nodes (4): main(), test_cat_token_wrapper(), test_out_of_band_token_wrapper(), test_publisher_config_auth_defaults()

### Community 43 - "SRT Config Tests"
Cohesion: 0.83
Nodes (3): string, expect(), main()

## Knowledge Gaps
- **424 isolated node(s):** `version`, `major`, `minor`, `patch`, `configurePresets` (+419 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **30 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `src/cmsf_packager.cpp` connect `CMSF Protection Architecture` to `MOQT Control Messages`, `Libmoq Translation`, `MOQT Session Tests`, `Publisher Transport Lifecycle`, `CMAF Segment Tests`, `CMSF Catalog Packaging`, `DASH Stream Reader`, `Picoquic Smoke Tests`, `Dependency Build Overview`?**
  _High betweenness centrality (0.204) - this node is a cross-community bridge._
- **Why does `src/cmaf_segmenter.cpp` connect `CMSF Protection Architecture` to `Publisher Session Core`, `CMAF Segment Generation`, `CMAF Segment Tests`, `CMSF Catalog Packaging`, `Publisher API Runtime`?**
  _High betweenness centrality (0.098) - this node is a cross-community bridge._
- **Why does `main()` connect `CLI and Config Parsing` to `CMSF Protection Architecture`, `MSF URL Handling`?**
  _High betweenness centrality (0.066) - this node is a cross-community bridge._
- **Are the 9 inferred relationships involving `serve_subscriptions()` (e.g. with `encode_subscribe_message()` and `encode_subscribe_namespace_message()`) actually correct?**
  _`serve_subscriptions()` has 9 INFERRED edges - model-reasoned connections that need verification._
- **What connects `version`, `major`, `minor` to the rest of the system?**
  _430 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `Publisher Session Core` be split into smaller, more focused modules?**
  _Cohesion score 0.05554002422886963 - nodes in this community are weakly interconnected._
- **Should `SRT Ingest Pipeline` be split into smaller, more focused modules?**
  _Cohesion score 0.05050872093023256 - nodes in this community are weakly interconnected._