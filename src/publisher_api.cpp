#include "openmoq/publisher/publisher_api.h"

#include "openmoq/publisher/cmaf_segmenter.h"
#include "openmoq/publisher/mp4_box.h"
#include "openmoq/publisher/transport/moqt_session.h"
#include "openmoq/publisher/transport/picoquic_client.h"
#include "openmoq/publisher/transport/webtransport_client.h"
#ifdef OPENMOQ_HAS_LIBMOQ
#include "openmoq/publisher/transport/libmoq_publisher.h"
#endif

#include <stdexcept>
#include <sstream>
#include <set>
#include <utility>

namespace openmoq::publisher {

namespace {

std::string webtransport_protocol_offer(DraftVersion version) {
    switch (version) {
        case DraftVersion::kDraft14:
            return "";
        case DraftVersion::kDraft16:
            return "\"moqt-16\"";
        case DraftVersion::kDraft17:
            return "\"moqt-17\"";
        case DraftVersion::kDraft18:
            return "\"moqt-18\"";
    }

    return "";
}

std::string json_escape(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (const char c : input) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

struct BatchPublishStats {
    std::uint64_t bytes_published = 0;
    std::uint64_t objects_published = 0;
    std::uint64_t groups_published = 0;
};

BatchPublishStats summarize_batch_publish_stats(const PublishPlan& plan) {
    BatchPublishStats snapshot;
    std::set<std::pair<std::string, std::uint64_t>> groups;
    for (const auto& object : plan.objects) {
        const std::size_t payload_size = !object.owned_payload.empty() ? object.owned_payload.size()
                                                                        : object.payload.size;
        snapshot.bytes_published += static_cast<std::uint64_t>(payload_size);
        snapshot.objects_published += 1;
        groups.emplace(object.track_name, static_cast<std::uint64_t>(object.group_id));
    }
    snapshot.groups_published = static_cast<std::uint64_t>(groups.size());
    return snapshot;
}

transport::FailureKind publish_failure_kind(
    const transport::TransportStatus& status,
    const transport::PublisherTransport& publisher_transport) {
    if (status.failure_kind != transport::FailureKind::kFatal) {
        return status.failure_kind;
    }
    const transport::ConnectionState state = publisher_transport.state();
    if (state == transport::ConnectionState::kClosed ||
        state == transport::ConnectionState::kFailed) {
        return transport::FailureKind::kRetryable;
    }
    return transport::FailureKind::kFatal;
}

}  // namespace

struct Publisher::ActiveSession {
    std::unique_ptr<transport::PublisherTransport> transport;
    std::unique_ptr<transport::MoqtSession> session;
};

Publisher::Publisher(PublisherConfig config, TransportFactory transport_factory)
    : config_(std::move(config)),
      transport_factory_(std::move(transport_factory)) {
    transport_factory_injected_ = static_cast<bool>(transport_factory_);
    if (!transport_factory_) {
        transport_factory_ = default_transport_factory();
    }
}

const PublisherConfig& Publisher::config() const {
    return config_;
}

void Publisher::set_config(const PublisherConfig& config) {
    config_ = config;
}

PreparedPublish Publisher::prepare_file(const std::filesystem::path& path) const {
    const ParsedMp4 parsed_mp4 = parse_mp4_file(path.string());
    const SegmentedMp4 segmented_mp4 = segment_for_cmaf(parsed_mp4, config_.split_cmaf_chunks ? CmafObjectMode::kSplit
                                                                                               : CmafObjectMode::kCoalesced);
    return PreparedPublish{
        .input_bytes = parsed_mp4.bytes,
        .plan = build_publish_plan(segmented_mp4,
                                   config_.draft_version,
                                   config_.include_sap,
                                   config_.include_msf_timeline,
                                   config_.vod,
                                   config_.drm_systems),
    };
}

PreparedPublish Publisher::prepare_stream(std::istream& input, std::string_view source_name) const {
    const ParsedMp4 parsed_mp4 = parse_mp4_stream(input, source_name);
    const SegmentedMp4 segmented_mp4 = segment_for_cmaf(parsed_mp4, config_.split_cmaf_chunks ? CmafObjectMode::kSplit
                                                                                               : CmafObjectMode::kCoalesced);
    return PreparedPublish{
        .input_bytes = parsed_mp4.bytes,
        .plan = build_publish_plan(segmented_mp4,
                                   config_.draft_version,
                                   config_.include_sap,
                                   config_.include_msf_timeline,
                                   config_.vod,
                                   config_.drm_systems),
    };
}

std::string Publisher::render_plan(const PreparedPublish& prepared) const {
    return render_publish_plan(prepared.plan);
}

void Publisher::emit_objects(const PreparedPublish& prepared, const std::filesystem::path& output_dir) const {
    emit_plan_objects(prepared.plan, prepared.input_bytes, output_dir);
}

transport::TransportStatus Publisher::publish(const PreparedPublish& prepared,
                                              const transport::EndpointConfig& endpoint,
                                              const transport::TlsConfig& tls,
                                              bool endpoint_alpn_overridden) const {
#ifdef OPENMOQ_ENABLE_LIBMOQ_PUBLISHER
    // Batch publishing prefers the libmoq service tier unless a custom
    // TransportFactory was injected (tests still exercise the local path).
    // The live paths (stdin, SRT, LiveObjectSource) route to libmoq in their
    // own methods below; the MoqtSession transport now only serves injected-
    // factory callers and builds without libmoq.
    if (!transport_factory_injected_) {
        const transport::EndpointConfig resolved_endpoint =
            resolve_endpoint(endpoint, endpoint_alpn_overridden);
        const PublishPlan materialized =
            materialize_publish_plan(prepared.plan, prepared.input_bytes);
        transport::LibmoqPublishStats libmoq_stats;
        const transport::TransportStatus status = transport::publish_plan_via_libmoq(
            materialized, config_, resolved_endpoint, tls, libmoq_stats);
        if (!status.ok) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            stats_.active = false;
            stats_.connected = false;
            stats_.publishing_live = false;
            stats_.last_error = status.message;
            return status;
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        stats_.active = false;
        stats_.connected = true;
        stats_.publishing_live = false;
        stats_.transport = resolved_endpoint.transport;
        stats_.host = resolved_endpoint.host;
        stats_.port = resolved_endpoint.port;
        stats_.path = resolved_endpoint.path;
        stats_.bytes_published = libmoq_stats.bytes_published;
        stats_.objects_published = libmoq_stats.objects_published;
        stats_.groups_published = libmoq_stats.groups_published;
        stats_.last_error.clear();
        return status;
    }
#endif
    if (!transport_factory_) {
        return transport::TransportStatus::failure("publisher transport factory is not configured");
    }
    auto active = std::make_shared<ActiveSession>();
    active->transport = transport_factory_(endpoint.transport);
    if (!active->transport) {
        return transport::TransportStatus::failure("failed to create requested transport");
    }

    active->session = std::make_unique<transport::MoqtSession>(
        *active->transport,
        config_.track_namespace,
        config_.forward,
        config_.publish_catalog,
        config_.paced,
        config_.loop,
        config_.subscriber_timeout,
        config_.authorization);
    active->session->set_catalog_republish_interval(config_.catalog_republish_interval);
    active->session->set_preannounce_tracks(config_.preannounce_tracks);

    const transport::EndpointConfig resolved_endpoint = resolve_endpoint(endpoint, endpoint_alpn_overridden);
    set_active_session(active, resolved_endpoint, false);
    transport::TransportStatus status = active->session->connect(resolved_endpoint, tls);
    if (!status.ok) {
        const std::string error = "transport connect failed: " + status.message;
        clear_active_session(active, false, error);
        return transport::TransportStatus::failure(
            error, transport::FailureKind::kRetryable);
    }

    const PublishPlan materialized = materialize_publish_plan(prepared.plan, prepared.input_bytes);
    status = active->session->publish(materialized);
    if (!status.ok) {
        const std::string error = "transport publish failed: " + status.message;
        const transport::FailureKind failure_kind =
            publish_failure_kind(status, *active->transport);
        static_cast<void>(active->session->close(0));
        clear_active_session(active, true, error);
        return transport::TransportStatus::failure(error, failure_kind);
    }
    {
        const auto batch_stats = summarize_batch_publish_stats(materialized);
        std::lock_guard<std::mutex> lock(state_mutex_);
        stats_.bytes_published = batch_stats.bytes_published;
        stats_.objects_published = batch_stats.objects_published;
        stats_.groups_published = batch_stats.groups_published;
    }

    return transport::TransportStatus::success();
}

transport::TransportStatus Publisher::publish_file(const std::filesystem::path& path,
                                                   const transport::EndpointConfig& endpoint,
                                                   const transport::TlsConfig& tls,
                                                   bool endpoint_alpn_overridden) const {
    const PreparedPublish prepared = prepare_file(path);
    return publish(prepared, endpoint, tls, endpoint_alpn_overridden);
}

transport::TransportStatus Publisher::publish_stream(std::istream& input,
                                                     std::string_view source_name,
                                                     const transport::EndpointConfig& endpoint,
                                                     const transport::TlsConfig& tls,
                                                     bool endpoint_alpn_overridden) const {
    const PreparedPublish prepared = prepare_stream(input, source_name);
    return publish(prepared, endpoint, tls, endpoint_alpn_overridden);
}

transport::TransportStatus Publisher::publish_live(std::istream& input,
                                                   const transport::EndpointConfig& endpoint,
                                                   const transport::TlsConfig& tls,
                                                   bool endpoint_alpn_overridden) const {
    LiveIngestConfig ingest;
    ingest.use_stdin = true;
    return publish_live(ingest, &input, endpoint, tls, endpoint_alpn_overridden);
}

transport::TransportStatus Publisher::publish_live(const LiveIngestConfig& ingest,
                                                   std::istream* stdin_input,
                                                   const transport::EndpointConfig& endpoint,
                                                   const transport::TlsConfig& tls,
                                                   bool endpoint_alpn_overridden) const {
#ifdef OPENMOQ_ENABLE_LIBMOQ_PUBLISHER
    // Live stdin and SRT publishing prefer the libmoq service tier unless a
    // custom TransportFactory was injected. (publish_live_objects routes to
    // libmoq in its own method.)
    if (!transport_factory_injected_ && ingest.use_stdin && ingest.srt_callers.empty()) {
        if (stdin_input == nullptr) {
            return transport::TransportStatus::failure("live stdin publish requires an input stream");
        }
        const transport::EndpointConfig resolved_endpoint =
            resolve_endpoint(endpoint, endpoint_alpn_overridden);
        auto live = std::make_shared<transport::LibmoqLiveHandle>();
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            libmoq_live_ = live;
        }
        transport::LibmoqPublishStats libmoq_stats;
        const transport::TransportStatus status = transport::publish_live_stdin_via_libmoq(
            *stdin_input, config_, resolved_endpoint, tls, libmoq_stats, live.get());
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (libmoq_live_ == live) {
            libmoq_live_.reset();
        }
        stats_.active = false;
        stats_.connected = status.ok;
        stats_.publishing_live = false;
        stats_.transport = resolved_endpoint.transport;
        stats_.host = resolved_endpoint.host;
        stats_.port = resolved_endpoint.port;
        stats_.path = resolved_endpoint.path;
        stats_.bytes_published = libmoq_stats.bytes_published;
        stats_.objects_published = libmoq_stats.objects_published;
        stats_.groups_published = libmoq_stats.groups_published;
        stats_.last_error = status.ok ? std::string() : status.message;
        return status;
    }
    // SRT-only live publishing also prefers the libmoq service tier. Mixed
    // stdin+SRT does not match either branch and falls through to the old path
    // below, which rejects it (consistent with existing validation).
    if (!transport_factory_injected_ && !ingest.use_stdin && !ingest.srt_callers.empty()) {
        std::vector<LiveSrtCallerRuntimeConfig> runtime_callers;
        runtime_callers.reserve(ingest.srt_callers.size());
        for (const auto& caller : ingest.srt_callers) {
            LiveSrtCallerRuntimeConfig rc;
            rc.id = caller.id;
            rc.endpoint = caller.endpoint;
            rc.fragment_on_keyframe = caller.fragment_on_keyframe;
            rc.empty_moov = caller.empty_moov;
            rc.default_base_moof = caller.default_base_moof;
            rc.separate_moof_per_track = caller.separate_moof_per_track;
            rc.target_fragment_duration_ms = caller.target_fragment_duration_ms;
            rc.latency_ms = caller.latency_ms;
            rc.auto_detect_program = caller.auto_detect_program;
            rc.program_number = caller.program_number.value_or(0);
            rc.has_program_number = caller.program_number.has_value();
            rc.video_pid = caller.video_pid.value_or(0);
            rc.has_video_pid = caller.video_pid.has_value();
            rc.audio_pid = caller.audio_pid.value_or(0);
            rc.has_audio_pid = caller.audio_pid.has_value();
            runtime_callers.push_back(std::move(rc));
        }
        const transport::EndpointConfig resolved_endpoint =
            resolve_endpoint(endpoint, endpoint_alpn_overridden);
        auto live = std::make_shared<transport::LibmoqLiveHandle>();
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            libmoq_live_ = live;
        }
        transport::LibmoqPublishStats libmoq_stats;
        const transport::TransportStatus status = transport::publish_live_srt_via_libmoq(
            std::move(runtime_callers), config_, resolved_endpoint, tls, libmoq_stats, live.get());
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (libmoq_live_ == live) {
            libmoq_live_.reset();
        }
        stats_.active = false;
        stats_.connected = status.ok;
        stats_.publishing_live = false;
        stats_.transport = resolved_endpoint.transport;
        stats_.host = resolved_endpoint.host;
        stats_.port = resolved_endpoint.port;
        stats_.path = resolved_endpoint.path;
        stats_.bytes_published = libmoq_stats.bytes_published;
        stats_.objects_published = libmoq_stats.objects_published;
        stats_.groups_published = libmoq_stats.groups_published;
        stats_.last_error = status.ok ? std::string() : status.message;
        return status;
    }
#endif
    if (!transport_factory_) {
        return transport::TransportStatus::failure("publisher transport factory is not configured");
    }
    auto active = std::make_shared<ActiveSession>();
    active->transport = transport_factory_(endpoint.transport);
    if (!active->transport) {
        return transport::TransportStatus::failure("failed to create requested transport");
    }

    active->session = std::make_unique<transport::MoqtSession>(
        *active->transport,
        config_.track_namespace,
        config_.forward,
        config_.publish_catalog,
        config_.paced,
        config_.loop,
        config_.subscriber_timeout,
        config_.authorization);
    active->session->set_catalog_republish_interval(config_.catalog_republish_interval);
    active->session->set_preannounce_tracks(config_.preannounce_tracks);

    const transport::EndpointConfig resolved_endpoint = resolve_endpoint(endpoint, endpoint_alpn_overridden);
    set_active_session(active, resolved_endpoint, true);
    transport::TransportStatus status = active->session->connect(resolved_endpoint, tls);
    if (!status.ok) {
        const std::string error = "transport connect failed: " + status.message;
        clear_active_session(active, false, error);
        return transport::TransportStatus::failure(
            error, transport::FailureKind::kRetryable);
    }

    transport::LiveIngestOptions session_ingest;
    session_ingest.use_stdin = ingest.use_stdin;
    session_ingest.srt_callers.reserve(ingest.srt_callers.size());
    for (const auto& caller : ingest.srt_callers) {
        transport::LiveSrtCallerOptions session_caller;
        session_caller.id = caller.id;
        session_caller.endpoint = caller.endpoint;
        session_caller.fragment_on_keyframe = caller.fragment_on_keyframe;
        session_caller.empty_moov = caller.empty_moov;
        session_caller.default_base_moof = caller.default_base_moof;
        session_caller.separate_moof_per_track = caller.separate_moof_per_track;
        session_caller.target_fragment_duration_ms = caller.target_fragment_duration_ms;
        session_caller.latency_ms = caller.latency_ms;
        session_caller.auto_detect_program = caller.auto_detect_program;
        session_caller.program_number = caller.program_number;
        session_caller.video_pid = caller.video_pid;
        session_caller.audio_pid = caller.audio_pid;
        session_ingest.srt_callers.push_back(std::move(session_caller));
    }

    status = active->session->publish_live(session_ingest,
                                           stdin_input,
                                           config_.draft_version,
                                           config_.split_cmaf_chunks,
                                           config_.live_stream_per_object);
    if (!status.ok) {
        const std::string error = "transport live publish failed: " + status.message;
        const transport::FailureKind failure_kind =
            publish_failure_kind(status, *active->transport);
        static_cast<void>(active->session->close(0));
        clear_active_session(active, true, error);
        return transport::TransportStatus::failure(error, failure_kind);
    }
    {
        const auto live_stats = active->session->publish_stats();
        std::lock_guard<std::mutex> lock(state_mutex_);
        stats_.bytes_published = live_stats.bytes_published;
        stats_.objects_published = live_stats.objects_published;
        stats_.groups_published = live_stats.groups_published;
    }

    return transport::TransportStatus::success();
}

transport::TransportStatus Publisher::publish_live_objects(const LiveObjectSource& source,
                                                           const transport::EndpointConfig& endpoint,
                                                           const transport::TlsConfig& tls,
                                                           bool endpoint_alpn_overridden) const {
    const bool source_supplies_catalog =
        source.catalog_mode == LiveCatalogMode::kSourceObject;
    if (source_supplies_catalog) {
        const std::size_t catalog_track_count =
            static_cast<std::size_t>(std::count_if(
                source.tracks.begin(), source.tracks.end(),
                [](const LiveTrack& track) { return track.track_name == "catalog"; }));
        const bool has_media_track =
            std::any_of(source.tracks.begin(), source.tracks.end(),
                        [](const LiveTrack& track) { return track.track_name != "catalog"; });
        if (catalog_track_count != 1 || !has_media_track) {
            return transport::TransportStatus::failure(
                "source-catalog mode requires exactly one catalog track and at least one media track");
        }
    }
#ifdef OPENMOQ_ENABLE_LIBMOQ_PUBLISHER
    // publish_live_objects prefers the libmoq service tier unless a custom
    // TransportFactory was injected (legacy MoqtSession tests). libmoq requires
    // real media metadata per track; a bare/legacy LiveTrack fails with a clear
    // message rather than being faked as a generic media track.
    if (!transport_factory_injected_ && !source_supplies_catalog) {
        for (const auto& track : source.tracks) {
            if (!transport::live_track_has_media_metadata(track)) {
                return transport::TransportStatus::failure(
                    "libmoq-backed publish_live_objects requires media metadata on LiveTrack '" +
                    track.track_name +
                    "' (set media_type, codec, and -- for audio -- sample_rate/channel_count); "
                    "bare object tracks are legacy-only: inject a TransportFactory to use the "
                    "MoqtSession path");
            }
        }
        const transport::EndpointConfig resolved_endpoint =
            resolve_endpoint(endpoint, endpoint_alpn_overridden);
        auto live = std::make_shared<transport::LibmoqLiveHandle>();
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            libmoq_live_ = live;
        }
        transport::LibmoqPublishStats libmoq_stats;
        const transport::TransportStatus status = transport::publish_live_objects_via_libmoq(
            source, config_, resolved_endpoint, tls, libmoq_stats, live.get());
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (libmoq_live_ == live) {
            libmoq_live_.reset();
        }
        stats_.active = false;
        stats_.connected = status.ok;
        stats_.publishing_live = false;
        stats_.transport = resolved_endpoint.transport;
        stats_.host = resolved_endpoint.host;
        stats_.port = resolved_endpoint.port;
        stats_.path = resolved_endpoint.path;
        stats_.bytes_published = libmoq_stats.bytes_published;
        stats_.objects_published = libmoq_stats.objects_published;
        stats_.groups_published = libmoq_stats.groups_published;
        stats_.last_error = status.ok ? std::string() : status.message;
        return status;
    }
#endif
    if (!transport_factory_) {
        return transport::TransportStatus::failure("publisher transport factory is not configured");
    }
    auto active = std::make_shared<ActiveSession>();
    active->transport = transport_factory_(endpoint.transport);
    if (!active->transport) {
        return transport::TransportStatus::failure("failed to create requested transport");
    }

    active->session = std::make_unique<transport::MoqtSession>(
        *active->transport,
        config_.track_namespace,
        config_.forward,
        config_.publish_catalog,
        config_.paced,
        config_.loop,
        config_.subscriber_timeout,
        config_.authorization);
    active->session->set_catalog_republish_interval(config_.catalog_republish_interval);
    active->session->set_preannounce_tracks(config_.preannounce_tracks);

    const transport::EndpointConfig resolved_endpoint = resolve_endpoint(endpoint, endpoint_alpn_overridden);
    set_active_session(active, resolved_endpoint, true);
    transport::TransportStatus status = active->session->connect(resolved_endpoint, tls);
    if (!status.ok) {
        const std::string error = "transport connect failed: " + status.message;
        clear_active_session(active, false, error);
        return transport::TransportStatus::failure(
            error, transport::FailureKind::kRetryable);
    }

    status = active->session->publish_live_objects(source, config_.draft_version);
    if (!status.ok) {
        const std::string error = "transport live object publish failed: " + status.message;
        const transport::FailureKind failure_kind =
            publish_failure_kind(status, *active->transport);
        static_cast<void>(active->session->close(0));
        clear_active_session(active, true, error);
        return transport::TransportStatus::failure(error, failure_kind);
    }
    {
        const auto live_stats = active->session->publish_stats();
        std::lock_guard<std::mutex> lock(state_mutex_);
        stats_.bytes_published = live_stats.bytes_published;
        stats_.objects_published = live_stats.objects_published;
        stats_.groups_published = live_stats.groups_published;
    }

    return transport::TransportStatus::success();
}

transport::TransportStatus Publisher::disconnect(std::uint64_t application_error_code) const {
    std::shared_ptr<ActiveSession> active;
    std::shared_ptr<transport::LibmoqLiveHandle> live;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        active = active_session_;
        live = libmoq_live_;
    }
    // A libmoq-backed live publish does not use active_session_; request_cancel()
    // sets its cancel flag AND interrupts the live endpoint so a blocking wait
    // returns immediately (not just at the next poll).
#ifdef OPENMOQ_HAS_LIBMOQ
    if (live) {
        live->request_cancel();
    }
#endif
    if (!active || !active->session) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        stats_.active = false;
        stats_.connected = false;
        stats_.publishing_live = false;
        return transport::TransportStatus::success();
    }
    const auto status = active->session->close(application_error_code);
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (active_session_ == active) {
        if (active_session_->transport) {
            stats_.connection_id = active_session_->transport->connection_id();
        }
        active_session_.reset();
    }
    stats_.active = false;
    stats_.connected = false;
    stats_.publishing_live = false;
    if (!status.ok) {
        stats_.last_error = "disconnect failed: " + status.message;
    }
    return status;
}

transport::TransportStatus Publisher::end_broadcast(EndBroadcastMode mode) const {
    std::shared_ptr<ActiveSession> active;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        active = active_session_;
    }
    if (!active || !active->session) {
        return transport::TransportStatus::failure("end_broadcast requires an active session");
    }
    // Held without the lock: end_broadcast blocks on the wire, and holding
    // state_mutex_ across a write would deadlock a concurrent stats() call,
    // which is why disconnect() releases it before calling close().
    //
    // config_.draft_version is threaded through explicitly because
    // MoqtSession does not persist the draft used by its in-progress publish
    // call (it is only ever a parameter to publish()/publish_live()); there
    // is nothing for end_broadcast to read it back from otherwise.
    return active->session->end_broadcast(mode, config_.draft_version);
}

PublisherStats Publisher::stats() const {
    StatsSnapshot snapshot;
    bool split_cmaf_chunks = true;
    bool include_sap = false;
    bool include_msf_timeline = false;
    std::shared_ptr<ActiveSession> active;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        snapshot = stats_;
        split_cmaf_chunks = config_.split_cmaf_chunks;
        include_sap = config_.include_sap;
        include_msf_timeline = config_.include_msf_timeline;
        active = active_session_;
    }
    if (active && active->session) {
        const auto live_stats = active->session->publish_stats();
        snapshot.bytes_published = live_stats.bytes_published;
        snapshot.objects_published = live_stats.objects_published;
        snapshot.groups_published = live_stats.groups_published;
    }
    if (active && active->transport) {
        snapshot.connection_id = active->transport->connection_id();
    }

    return PublisherStats{
        .publishing_live = snapshot.publishing_live,
        .bytes_published = snapshot.bytes_published,
        .objects_published = snapshot.objects_published,
        .groups_published = snapshot.groups_published,
        .split_cmaf_chunks = split_cmaf_chunks,
        .include_sap = include_sap,
        .include_msf_timeline = include_msf_timeline,
        .transport = snapshot.transport,
        .host = snapshot.host,
        .port = snapshot.port,
        .path = snapshot.path,
        .connection_id = snapshot.connection_id,
        .last_error = snapshot.last_error,
    };
}

std::string Publisher::stats_json() const {
    StatsSnapshot snapshot;
    bool split_cmaf_chunks = true;
    bool include_sap = false;
    bool include_msf_timeline = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        snapshot = stats_;
        split_cmaf_chunks = config_.split_cmaf_chunks;
        include_sap = config_.include_sap;
        include_msf_timeline = config_.include_msf_timeline;
        if (active_session_ && active_session_->transport) {
            snapshot.connection_id = active_session_->transport->connection_id();
            snapshot.connected = active_session_->transport->state() == transport::ConnectionState::kConnected;
            snapshot.active = true;
        }
    }

    std::ostringstream os;
    os << "{"
       << "\"active\":" << (snapshot.active ? "true" : "false") << ","
       << "\"connected\":" << (snapshot.connected ? "true" : "false") << ","
       << "\"publishingLive\":" << (snapshot.publishing_live ? "true" : "false") << ","
       << "\"bytesPublished\":" << snapshot.bytes_published << ","
       << "\"objectsPublished\":" << snapshot.objects_published << ","
       << "\"groupsPublished\":" << snapshot.groups_published << ","
       << "\"splitCmafChunks\":" << (split_cmaf_chunks ? "true" : "false") << ","
       << "\"includeSap\":" << (include_sap ? "true" : "false") << ","
       << "\"includeMsfTimeline\":" << (include_msf_timeline ? "true" : "false") << ","
       << "\"transport\":\""
       << (snapshot.transport == transport::TransportKind::kWebTransport ? "webtransport" : "raw_quic") << "\","
       << "\"host\":\"" << json_escape(snapshot.host) << "\","
       << "\"port\":" << snapshot.port << ","
       << "\"path\":\"" << json_escape(snapshot.path) << "\","
       << "\"connectionId\":\"" << json_escape(snapshot.connection_id) << "\","
       << "\"lastError\":\"" << json_escape(snapshot.last_error) << "\""
       << "}";
    return os.str();
}

Publisher::TransportFactory Publisher::default_transport_factory() {
    return [](transport::TransportKind kind) -> std::unique_ptr<transport::PublisherTransport> {
        switch (kind) {
            case transport::TransportKind::kRawQuic:
                return std::make_unique<transport::PicoquicClient>();
            case transport::TransportKind::kWebTransport:
                return std::make_unique<transport::WebTransportClient>();
        }
        return nullptr;
    };
}

transport::EndpointConfig Publisher::resolve_endpoint(const transport::EndpointConfig& endpoint,
                                                      bool endpoint_alpn_overridden) const {
    transport::EndpointConfig resolved = endpoint;
    resolved.application_protocol = resolved.transport == transport::TransportKind::kWebTransport
                                        ? webtransport_protocol_offer(config_.draft_version)
                                        : default_alpn(config_.draft_version);

    if (!endpoint_alpn_overridden && resolved.transport == transport::TransportKind::kRawQuic &&
        config_.draft_version != DraftVersion::kDraft14) {
        resolved.alpn = default_alpn(config_.draft_version);
    } else if (!endpoint_alpn_overridden && resolved.transport == transport::TransportKind::kWebTransport) {
        resolved.alpn = "h3";
    }

    return resolved;
}

void Publisher::set_active_session(std::shared_ptr<ActiveSession> active,
                                   const transport::EndpointConfig& endpoint,
                                   bool publishing_live) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    active_session_ = std::move(active);
    stats_.active = true;
    stats_.connected = false;
    stats_.publishing_live = publishing_live;
    stats_.bytes_published = 0;
    stats_.objects_published = 0;
    stats_.groups_published = 0;
    stats_.transport = endpoint.transport;
    stats_.host = endpoint.host;
    stats_.port = endpoint.port;
    stats_.path = endpoint.path;
    stats_.connection_id.clear();
    stats_.last_error.clear();
}

void Publisher::clear_active_session(const std::shared_ptr<ActiveSession>& active,
                                     bool connected,
                                     const std::string& last_error) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (active_session_ == active) {
        if (active_session_ && active_session_->transport) {
            stats_.connection_id = active_session_->transport->connection_id();
        }
        active_session_.reset();
    }
    stats_.active = false;
    stats_.connected = connected;
    stats_.publishing_live = false;
    stats_.last_error = last_error;
}

}  // namespace openmoq::publisher
