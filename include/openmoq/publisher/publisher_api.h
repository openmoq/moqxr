#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/cat4moq.h"
#include "openmoq/publisher/live_object.h"
#include "openmoq/publisher/moq_draft.h"
#include "openmoq/publisher/msf_catalog.h"
#include "openmoq/publisher/transport/publisher_transport.h"

namespace openmoq::publisher {

namespace transport {
// Defined in transport/libmoq_publisher.h (libmoq builds only). Held here by
// shared_ptr so disconnect() can interrupt an in-flight libmoq live publish; the
// member stays null when libmoq is not compiled in.
struct LibmoqLiveHandle;
}  // namespace transport

// Deployment configuration for one DRM system, matched to a system found in
// the media by system_id. These fields are not present in the MP4 -- a
// licence URL is deployment data, not media data.
struct DrmSystemConfig {
    std::string system_id;                      // UUID string, the match key
    std::optional<std::string> la_url;
    std::optional<std::string> la_url_type;
    std::optional<std::string> cert_url;
    std::optional<std::string> cert_url_type;
    std::optional<std::string> robustness;
};

struct PublisherConfig {
    DraftVersion draft_version = DraftVersion::kDraft16;
    std::string track_namespace = "media";
    bool forward = false;
    bool publish_catalog = false;
    // Send a PUBLISH for each track immediately after PUBLISH_NAMESPACE, before any
    // subscriber exists. Relays differ here: some accept tracks that way and never
    // forward a SUBSCRIBE upstream, so a publisher waiting for one would stall,
    // while others resolve the track namespace only once a subscriber appears and
    // are disturbed by an early PUBLISH. Off by default because the path that
    // sends it does not process the PUBLISH_OK it would receive, so the request
    // would be made and its answer ignored.
    bool preannounce_tracks = false;
    bool include_sap = false;
    bool include_msf_timeline = false;
    bool split_cmaf_chunks = true;
    bool live_stream_per_object = false;
    bool paced = false;
    bool loop = false;
    // QUIC stack for the libmoq publish route (see transport::LibmoqBackend).
    transport::LibmoqBackend libmoq_backend = transport::LibmoqBackend::kAuto;
    // This publisher is live, or simulating live, unless explicitly told
    // otherwise. VOD semantics are never inferred from the input.
    bool vod = false;
    // Matched to systems found in the media by system_id. A configured system
    // the media carries no pssh for is IGNORED, not emitted -- a
    // contentProtections entry for an absent system would describe protection
    // that does not exist.
    //
    // Content-protection detection and signalling work on the live publish
    // paths (publish_live()'s stdin ingest and publish_live_objects()'s DASH
    // ingest) wherever a real CMAF initialization segment reaches the
    // publisher: the live catalog builders do call attach_content_protection,
    // from the init segment's sinf/schm/schi/tenc boxes and the moov-level
    // pssh siblings, exactly as the batch path does. That detection is
    // independent of drm_systems -- it happens whether or not this field is
    // populated. `parse_cli_options` (src/cli_options.cpp) refuses only
    // `--drm-config` combined with `--live-source srt`: SRT carries MPEG-TS,
    // and the publisher synthesises a CMAF init segment from parsed
    // elementary streams, so there is no sinf/schm/schi/tenc or pssh box for
    // the publisher ever to detect on that path -- a property of the
    // container, not unfinished work. drm_systems itself supplies only
    // optional deployment fields (laURL/certURL/robustness), never protection
    // detection; on the default build (MoqtSession backend, i.e. without
    // -DOPENMOQ_USE_LIBMOQ_PUBLISHER=ON), no live path applies those
    // deployment fields to the catalog even when detection succeeds and this
    // field is populated -- only the batch/VOD path does (see
    // docs/status.md and docs/protocol-mapping.md for the full picture).
    // This holds on both backends: the libmoq stdin path passes drm_systems
    // through but discards the built catalog, so nothing reaches the wire.
    std::vector<DrmSystemConfig> drm_systems;
    // Section 5: a catalog SHOULD be republished after enough time has passed
    // that it might fall out of a relay cache. Zero disables republication,
    // which is the historical behaviour.
    std::chrono::seconds catalog_republish_interval{0};
    std::chrono::seconds subscriber_timeout = std::chrono::seconds(30);
    cat4moq::AuthorizationConfig authorization;
};

struct PreparedPublish {
    std::vector<std::uint8_t> input_bytes;
    PublishPlan plan;
};

struct PublisherStats {
    bool publishing_live = false;
    std::uint64_t bytes_published = 0;
    std::uint64_t objects_published = 0;
    std::uint64_t groups_published = 0;
    bool split_cmaf_chunks = true;
    bool include_sap = false;
    bool include_msf_timeline = false;
    transport::TransportKind transport = transport::TransportKind::kRawQuic;
    std::string host;
    std::uint16_t port = 0;
    std::string path;
    std::string connection_id;
    std::string last_error;
};

struct LiveSrtCaller {
    std::string id;
    std::string endpoint;
    bool fragment_on_keyframe = true;
    bool empty_moov = true;
    bool default_base_moof = true;
    bool separate_moof_per_track = true;
    std::uint32_t target_fragment_duration_ms = 1000;
    std::uint32_t latency_ms = 120;
    bool auto_detect_program = true;
    std::optional<std::uint32_t> program_number;
    std::optional<std::uint32_t> video_pid;
    std::optional<std::uint32_t> audio_pid;
};

struct LiveIngestConfig {
    bool use_stdin = false;
    std::vector<LiveSrtCaller> srt_callers;
};

class Publisher {
public:
    using TransportFactory = std::function<std::unique_ptr<transport::PublisherTransport>(transport::TransportKind)>;

    explicit Publisher(PublisherConfig config = {}, TransportFactory transport_factory = {});

    const PublisherConfig& config() const;
    void set_config(const PublisherConfig& config);

    PreparedPublish prepare_file(const std::filesystem::path& path) const;
    PreparedPublish prepare_stream(std::istream& input, std::string_view source_name) const;

    std::string render_plan(const PreparedPublish& prepared) const;
    void emit_objects(const PreparedPublish& prepared, const std::filesystem::path& output_dir) const;

    transport::TransportStatus publish(const PreparedPublish& prepared,
                                       const transport::EndpointConfig& endpoint,
                                       const transport::TlsConfig& tls = {},
                                       bool endpoint_alpn_overridden = false) const;
    transport::TransportStatus publish_file(const std::filesystem::path& path,
                                            const transport::EndpointConfig& endpoint,
                                            const transport::TlsConfig& tls = {},
                                            bool endpoint_alpn_overridden = false) const;
    transport::TransportStatus publish_stream(std::istream& input,
                                              std::string_view source_name,
                                              const transport::EndpointConfig& endpoint,
                                              const transport::TlsConfig& tls = {},
                                              bool endpoint_alpn_overridden = false) const;
    transport::TransportStatus publish_live(std::istream& input,
                                            const transport::EndpointConfig& endpoint,
                                            const transport::TlsConfig& tls = {},
                                            bool endpoint_alpn_overridden = false) const;
    transport::TransportStatus publish_live(const LiveIngestConfig& ingest,
                                            std::istream* stdin_input,
                                            const transport::EndpointConfig& endpoint,
                                            const transport::TlsConfig& tls = {},
                                            bool endpoint_alpn_overridden = false) const;
    transport::TransportStatus publish_live_objects(const LiveObjectSource& source,
                                                    const transport::EndpointConfig& endpoint,
                                                    const transport::TlsConfig& tls = {},
                                                    bool endpoint_alpn_overridden = false) const;
    transport::TransportStatus disconnect(std::uint64_t application_error_code = 0) const;
    // End the broadcast per MSF section 11.3: publish the final independent
    // catalog (kConvertToVod marks every track not live and adds
    // trackDuration where known; kTerminate marks isComplete true with an
    // empty tracks array), then send PUBLISH_DONE with status 0x2 Track
    // Ended for all requests this session still tracks. Returns a failure
    // status when no session is active. The final catalog write only
    // happens for a session driven through publish_live(): a session driven
    // through the batch publish() plan path or publish_live_objects() never
    // populates MoqtSession's CatalogPublisher, so end_broadcast() skips that
    // write rather than guess which track alias is "catalog".
    transport::TransportStatus end_broadcast(EndBroadcastMode mode) const;
    PublisherStats stats() const;
    [[deprecated("Use stats(); live polling is not supported by the blocking publish API.")]]
    std::string stats_json() const;

private:
    struct ActiveSession;
    struct StatsSnapshot {
        bool active = false;
        bool connected = false;
        bool publishing_live = false;
        std::uint64_t bytes_published = 0;
        std::uint64_t objects_published = 0;
        std::uint64_t groups_published = 0;
        transport::TransportKind transport = transport::TransportKind::kRawQuic;
        std::string host;
        std::uint16_t port = 0;
        std::string path;
        std::string connection_id;
        std::string last_error;
    };

    static TransportFactory default_transport_factory();
    transport::EndpointConfig resolve_endpoint(const transport::EndpointConfig& endpoint,
                                               bool endpoint_alpn_overridden) const;
    void set_active_session(std::shared_ptr<ActiveSession> active,
                            const transport::EndpointConfig& endpoint,
                            bool publishing_live) const;
    void clear_active_session(const std::shared_ptr<ActiveSession>& active,
                              bool connected,
                              const std::string& last_error) const;

    PublisherConfig config_;
    TransportFactory transport_factory_;
    // True when the caller injected a custom TransportFactory (e.g. a test
    // mock). When false and libmoq is available, batch publishing prefers the
    // libmoq service-tier path over the local MoqtSession transport.
    bool transport_factory_injected_ = false;
    mutable std::mutex state_mutex_;
    mutable std::shared_ptr<ActiveSession> active_session_;
    // Shared handle for an in-progress libmoq-backed live publish (cancel flag +
    // the live endpoint). disconnect() calls request_cancel() on it so a running
    // driver stops promptly AND its blocking wait is interrupted (the libmoq
    // paths do not use active_session_). Guarded by state_mutex_.
    mutable std::shared_ptr<transport::LibmoqLiveHandle> libmoq_live_;
    mutable StatsSnapshot stats_;
};

}  // namespace openmoq::publisher
