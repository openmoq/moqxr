#include "openmoq/publisher/cli_options.h"

#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace openmoq::publisher {

namespace {

InputSource parse_input_source(std::string_view value) {
    if (value == "-") {
        return InputSource{.kind = InputSourceKind::kStdin, .path = {}};
    }
    return InputSource{.kind = InputSourceKind::kFile, .path = std::filesystem::path(value)};
}

DraftVersion parse_draft(std::string_view value) {
    if (value == "16") {
        return DraftVersion::kDraft16;
    }
    if (value == "18") {
        return DraftVersion::kDraft18;
    }
    // Draft-14 and draft-17 are no longer user-selectable; only draft-16 and
    // draft-18 are supported going forward.
    if (value == "14" || value == "17") {
        throw std::runtime_error(
            "draft " + std::string(value) +
            " is no longer supported; only draft 16 and 18 are available");
    }

    throw std::runtime_error("unsupported draft value: expected 16 or 18");
}

transport::TransportKind parse_transport_kind(std::string_view value) {
    if (value == "raw") {
        return transport::TransportKind::kRawQuic;
    }
    if (value == "webtransport") {
        return transport::TransportKind::kWebTransport;
    }

    throw std::runtime_error("unsupported --transport value: expected raw or webtransport");
}

transport::EndpointConfig parse_endpoint(std::string_view value) {
    transport::EndpointConfig endpoint;
    std::string_view authority = value;

    constexpr std::string_view kScheme = "moqt://";
    constexpr std::string_view kHttpsScheme = "https://";
    if (authority.starts_with(kScheme)) {
        authority.remove_prefix(kScheme.size());
        const std::size_t slash = authority.find('/');
        if (slash == std::string_view::npos) {
            endpoint.path = "/";
        } else {
            endpoint.path = std::string(authority.substr(slash));
            endpoint.path_explicit = true;
            authority = authority.substr(0, slash);
        }
    } else if (authority.starts_with(kHttpsScheme)) {
        authority.remove_prefix(kHttpsScheme.size());
        const std::size_t slash = authority.find('/');
        if (slash == std::string_view::npos) {
            endpoint.path = "/";
        } else {
            endpoint.path = std::string(authority.substr(slash));
            endpoint.path_explicit = true;
            authority = authority.substr(0, slash);
        }
    }

    const std::size_t colon = authority.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= authority.size()) {
        throw std::runtime_error("endpoint must be in host:port, moqt://host:port/path, or https://host:port/path form");
    }

    endpoint.host = std::string(authority.substr(0, colon));
    const std::string port_text(authority.substr(colon + 1));
    const int port = std::stoi(port_text);
    if (port <= 0 || port > 65535) {
        throw std::runtime_error("endpoint port must be between 1 and 65535");
    }
    endpoint.port = static_cast<std::uint16_t>(port);
    return endpoint;
}

bool parse_forward_flag(std::string_view value) {
    if (value == "0") {
        return false;
    }
    if (value == "1") {
        return true;
    }
    throw std::runtime_error("unsupported --forward value: expected 0 or 1");
}

std::chrono::seconds parse_timeout(std::string_view value) {
    const int timeout = std::stoi(std::string(value));
    if (timeout < 0) {
        throw std::runtime_error("subscriber timeout must be zero or greater");
    }
    return std::chrono::seconds(timeout);
}

LiveSourceKind parse_live_source(std::string_view value) {
    if (value == "auto") {
        return LiveSourceKind::kAuto;
    }
    if (value == "stdin") {
        return LiveSourceKind::kStdin;
    }
    if (value == "srt") {
        return LiveSourceKind::kSrt;
    }
    if (value == "dash") {
        return LiveSourceKind::kDash;
    }
    if (value == "both") {
        throw std::runtime_error("--live-source 'both' is not supported; use either 'stdin' or 'srt'");
    }
    throw std::runtime_error("unsupported --live-source value: expected auto, stdin, srt, or dash");
}

int parse_strict_int(std::string_view value, std::string_view option_name) {
    // Reject empty, non-numeric, or trailing-garbage input with a clear,
    // option-specific message rather than a bare std::stoi("stoi") exception,
    // and so "80abc" is not silently accepted as 80.
    if (value.empty()) {
        throw std::runtime_error(std::string(option_name) + " requires a numeric value");
    }
    std::size_t consumed = 0;
    int parsed = 0;
    try {
        parsed = std::stoi(std::string(value), &consumed);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string(option_name) + " must be a valid integer");
    }
    if (consumed != value.size()) {
        throw std::runtime_error(std::string(option_name) + " must be a valid integer");
    }
    return parsed;
}

std::pair<std::string, std::uint16_t> parse_host_port(std::string_view value, std::string_view option_name) {
    const std::size_t colon = value.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= value.size()) {
        throw std::runtime_error(std::string(option_name) + " must be in host:port form");
    }
    const std::string host(value.substr(0, colon));
    const int port = parse_strict_int(value.substr(colon + 1), std::string(option_name) + " port");
    if (port <= 0 || port > 65535) {
        throw std::runtime_error(std::string(option_name) + " port must be between 1 and 65535");
    }
    return {host, static_cast<std::uint16_t>(port)};
}

std::size_t parse_queue_depth(std::string_view value) {
    const int depth = parse_strict_int(value, "--dash-queue-depth");
    if (depth <= 0) {
        throw std::runtime_error("--dash-queue-depth must be greater than zero");
    }
    return static_cast<std::size_t>(depth);
}

std::chrono::seconds parse_catalog_republish_interval(std::string_view value) {
    const int seconds = parse_strict_int(value, "--catalog-republish-interval");
    if (seconds < 0) {
        throw std::runtime_error("--catalog-republish-interval must be zero or greater");
    }
    return std::chrono::seconds(seconds);
}

}  // namespace

CliOptions parse_cli_options(int argc, char** argv) {
    CliOptions options;
    bool dash_path_set = false;
    bool dash_queue_depth_set = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];

        auto require_value = [&](const char* flag) -> std::string_view {
            if (index + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + flag);
            }
            ++index;
            return argv[index];
        };

        if (argument == "--input") {
            options.input_source = parse_input_source(require_value("--input"));
        } else if (argument == "--live-source") {
            options.live_source = parse_live_source(require_value("--live-source"));
        } else if (argument == "--srt-config") {
            options.srt_config_path = std::filesystem::path(require_value("--srt-config"));
        } else if (argument == "--dash-listen") {
            const std::string_view value = require_value("--dash-listen");
            const auto [host, port] = parse_host_port(value, "--dash-listen");
            options.dash_listen = std::string(value);
            options.dash_listen_host = host;
            options.dash_listen_port = port;
        } else if (argument == "--dash-path") {
            options.dash_path_prefix = std::string(require_value("--dash-path"));
            dash_path_set = true;
        } else if (argument == "--dash-queue-depth") {
            options.dash_queue_depth = parse_queue_depth(require_value("--dash-queue-depth"));
            dash_queue_depth_set = true;
        } else if (argument == "--transport") {
            options.transport = parse_transport_kind(require_value("--transport"));
        } else if (argument == "--endpoint") {
            options.endpoint = parse_endpoint(require_value("--endpoint"));
        } else if (argument == "--alpn") {
            if (!options.endpoint.has_value()) {
                options.endpoint = transport::EndpointConfig{};
            }
            options.endpoint->alpn = std::string(require_value("--alpn"));
            options.endpoint_alpn_overridden = true;
        } else if (argument == "--sni") {
            if (!options.endpoint.has_value()) {
                options.endpoint = transport::EndpointConfig{};
            }
            options.endpoint->sni = std::string(require_value("--sni"));
        } else if (argument == "--cert") {
            options.tls.certificate_path = std::string(require_value("--cert"));
        } else if (argument == "--key") {
            options.tls.private_key_path = std::string(require_value("--key"));
        } else if (argument == "--ca") {
            options.tls.ca_path = std::string(require_value("--ca"));
        } else if (argument == "--insecure") {
            options.tls.insecure_skip_verify = true;
        } else if (argument == "--draft") {
            options.draft_version = parse_draft(require_value("--draft"));
        } else if (argument == "--namespace") {
            options.track_namespace = std::string(require_value("--namespace"));
        } else if (argument == "--forward") {
            options.forward = parse_forward_flag(require_value("--forward"));
        } else if (argument == "--publish-catalog") {
            options.publish_catalog = true;
        } else if (argument == "--sap") {
            options.include_sap = true;
        } else if (argument == "--msf-timeline") {
            options.include_msf_timeline = true;
        } else if (argument == "--coalesce-cmaf-chunks") {
            options.split_cmaf_chunks = false;
        } else if (argument == "--stream-per-object") {
            options.stream_per_object = true;
        } else if (argument == "--timeout") {
            options.subscriber_timeout = parse_timeout(require_value("--timeout"));
        } else if (argument == "--paced") {
            options.paced = true;
        } else if (argument == "--loop") {
            options.loop = true;
        } else if (argument == "--vod") {
            options.vod = true;
        } else if (argument == "--catalog-republish-interval") {
            options.catalog_republish_interval =
                parse_catalog_republish_interval(require_value("--catalog-republish-interval"));
        } else if (argument == "--drm-config") {
            // Parsed eagerly, not stored as a path: a malformed DRM config
            // must fail before publishing starts rather than publish with
            // partial configuration.
            options.drm_systems = parse_drm_config_file(std::string(require_value("--drm-config")));
        } else if (argument == "--emit-dir") {
            options.emit_dir = std::filesystem::path(require_value("--emit-dir"));
        } else if (argument == "--dump-plan") {
            options.dump_plan = true;
        } else if (argument == "--help" || argument == "-h") {
            throw std::runtime_error("");
        } else {
            throw std::runtime_error(std::string("unknown argument: ") + std::string(argument));
        }
    }

    const bool live_source_uses_stdin =
        options.live_source == LiveSourceKind::kAuto ||
        options.live_source == LiveSourceKind::kStdin;
    if (live_source_uses_stdin &&
        options.input_source.kind == InputSourceKind::kFile &&
        options.input_source.path.empty()) {
        throw std::runtime_error("missing required --input argument");
    }

    const bool live_source_uses_srt =
        options.live_source == LiveSourceKind::kSrt;
    const bool live_source_uses_dash =
        options.live_source == LiveSourceKind::kDash;
    if (live_source_uses_srt && !options.srt_config_path.has_value()) {
        throw std::runtime_error("--live-source srt requires --srt-config");
    }
    if (live_source_uses_srt && !options.endpoint.has_value()) {
        throw std::runtime_error("--live-source srt requires --endpoint");
    }
    if (!live_source_uses_srt && options.srt_config_path.has_value()) {
        throw std::runtime_error("--srt-config requires --live-source srt");
    }
    if (live_source_uses_dash && !options.dash_listen.has_value()) {
        throw std::runtime_error("--live-source dash requires --dash-listen");
    }
    if (live_source_uses_dash && !options.endpoint.has_value() && !options.dump_plan) {
        throw std::runtime_error("--live-source dash requires --endpoint or --dump-plan");
    }
    if (!live_source_uses_dash && options.dash_listen.has_value()) {
        throw std::runtime_error("--dash-listen requires --live-source dash");
    }
    if (!live_source_uses_dash && dash_path_set) {
        throw std::runtime_error("--dash-path requires --live-source dash");
    }
    if (!live_source_uses_dash && dash_queue_depth_set) {
        throw std::runtime_error("--dash-queue-depth requires --live-source dash");
    }
    if (!options.dash_path_prefix.empty() && options.dash_path_prefix.front() != '/') {
        throw std::runtime_error("--dash-path must start with /");
    }

    if (options.endpoint.has_value() && options.endpoint->host.empty()) {
        throw std::runtime_error("--alpn and --sni require --endpoint to be provided first");
    }
    if (options.endpoint.has_value()) {
        options.endpoint->transport = options.transport;
    }
    if (options.transport == transport::TransportKind::kWebTransport) {
        if (!options.endpoint.has_value()) {
            throw std::runtime_error("--transport webtransport requires --endpoint");
        }
        if (!options.endpoint->path_explicit) {
            throw std::runtime_error("--transport webtransport requires an endpoint path such as https://host:port/moq");
        }
    }
    if (options.track_namespace.empty()) {
        throw std::runtime_error("--namespace must not be empty");
    }

    // build_live_catalog (src/cmsf_packager.cpp) never calls
    // attach_content_protection, so a live publish path today would produce a
    // catalog with no contentProtections and no contentProtectionRefIDs at
    // all -- indistinguishable from unprotected content. Refuse the
    // combination outright rather than publish a misleading catalog; wiring
    // content protection into the live paths is out of scope here (see
    // docs/status.md).
    if (!options.drm_systems.empty()) {
        if (live_source_uses_srt) {
            throw std::runtime_error(
                "--drm-config is not supported with --live-source srt: the live publish path "
                "does not yet attach content protection to the catalog, so publishing would "
                "produce an unprotected-looking catalog for encrypted content");
        }
        // A dash dry run (--dump-plan without --endpoint) never builds a live
        // catalog at all -- main.cpp just prints raw object info locally --
        // so there is no misleading catalog for this guard to prevent.
        if (live_source_uses_dash && options.endpoint.has_value()) {
            throw std::runtime_error(
                "--drm-config is not supported with --live-source dash: the live publish path "
                "does not yet attach content protection to the catalog, so publishing would "
                "produce an unprotected-looking catalog for encrypted content");
        }
        const bool live_stdin_default = live_source_uses_stdin &&
                                        options.input_source.kind == InputSourceKind::kStdin &&
                                        options.endpoint.has_value();
        if (live_stdin_default) {
            throw std::runtime_error(
                "--drm-config is not supported with the live stdin publish path: the live "
                "publish path does not yet attach content protection to the catalog, so "
                "publishing would produce an unprotected-looking catalog for encrypted content");
        }
    }

    return options;
}

std::string build_usage(const char* argv0) {
    return std::string("Usage: ") + argv0 +
           " --input <mp4|-> [--live-source auto|stdin|srt|dash] [--srt-config <path>]"
           " [--dash-listen host:port] [--dash-path <prefix>] [--dash-queue-depth <count>]"
           " [--transport raw|webtransport] [--draft 14|16|17|18] [--namespace <value>] [--forward 0|1] [--timeout <seconds>]"
           " [--publish-catalog] [--sap] [--msf-timeline] [--coalesce-cmaf-chunks] [--stream-per-object] [--paced] [--loop] [--dump-plan] [--emit-dir <dir>]"
           " [--vod] [--catalog-republish-interval <seconds>] [--drm-config <path>]"
           " [--endpoint host:port|moqt://host:port/path|https://host:port/path] [--alpn value] [--sni value]"
           " [--cert file] [--key file] [--ca file] [--insecure]";
}

}  // namespace openmoq::publisher
