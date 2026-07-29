#pragma once

#include <filesystem>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "openmoq/publisher/drm_config.h"
#include "openmoq/publisher/moq_draft.h"
#include "openmoq/publisher/transport/publisher_transport.h"

namespace openmoq::publisher {

enum class InputSourceKind {
    kFile,
    kStdin,
};

struct InputSource {
    InputSourceKind kind = InputSourceKind::kFile;
    std::filesystem::path path;
};

enum class LiveSourceKind {
    kAuto,
    kStdin,
    kSrt,
    kDash,
};

struct CliOptions {
    InputSource input_source;
    LiveSourceKind live_source = LiveSourceKind::kAuto;
    std::optional<std::filesystem::path> srt_config_path;
    std::optional<std::string> dash_listen;
    std::string dash_listen_host = "127.0.0.1";
    std::uint16_t dash_listen_port = 8080;
    std::string dash_path_prefix = "/ingest";
    std::size_t dash_queue_depth = 128;
    std::optional<std::filesystem::path> emit_dir;
    std::optional<transport::EndpointConfig> endpoint;
    transport::TransportKind transport = transport::TransportKind::kRawQuic;
    transport::TlsConfig tls;
    DraftVersion draft_version = DraftVersion::kDraft16;
    std::string track_namespace = "media";
    bool endpoint_alpn_overridden = false;
    bool forward = false;
    bool publish_catalog = false;
    bool include_sap = false;
    bool include_msf_timeline = false;
    bool split_cmaf_chunks = true;
    bool stream_per_object = false;
    bool paced = false;
    bool loop = false;
    bool dump_plan = false;
    std::chrono::seconds subscriber_timeout = std::chrono::seconds(30);
    // This publisher is live, or simulating live, unless explicitly told
    // otherwise (PublisherConfig::vod's own rule). --vod is the only way to
    // opt into VOD from the moqxr binary.
    bool vod = false;
    // Section 5: a catalog SHOULD be republished after enough time has
    // passed that it might fall out of a relay cache. Zero (the default)
    // disables republication.
    std::chrono::seconds catalog_republish_interval{0};
    // Parsed eagerly by parse_cli_options so a malformed --drm-config file is
    // reported before publishing begins, rather than publishing with partial
    // DRM configuration.
    std::vector<DrmSystemConfig> drm_systems;
};

CliOptions parse_cli_options(int argc, char** argv);
std::string build_usage(const char* argv0);

}  // namespace openmoq::publisher
