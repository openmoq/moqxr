#pragma once

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
#include "openmoq/publisher/transport/publisher_transport.h"

namespace openmoq::publisher {

struct PublisherConfig {
    DraftVersion draft_version = DraftVersion::kDraft14;
    std::string track_namespace = "media";
    bool forward = false;
    bool publish_catalog = false;
    bool include_sap = false;
    bool include_msf_timeline = false;
    bool split_cmaf_chunks = true;
    bool paced = false;
    bool loop = false;
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
    mutable std::mutex state_mutex_;
    mutable std::shared_ptr<ActiveSession> active_session_;
    mutable StatsSnapshot stats_;
};

}  // namespace openmoq::publisher
