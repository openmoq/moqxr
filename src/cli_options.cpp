#include "openmoq/publisher/cli_options.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "openmoq/publisher/msf_url.h"
#include "openmoq/publisher/version.h"

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

std::size_t parse_retry_count(std::string_view value) {
    const int retries = parse_strict_int(value, "--retry");
    if (retries < 0) {
        throw std::runtime_error("--retry must be zero or greater");
    }
    return static_cast<std::size_t>(retries);
}

}  // namespace

CliOptions parse_cli_options(int argc, char** argv) {
    CliOptions options;
    bool dash_path_set = false;
    bool dash_queue_depth_set = false;
    bool transport_set = false;
    bool retry_set = false;
    // Tracks whether --endpoint / --url were themselves given on the command
    // line, as opposed to options.endpoint simply having a value (--alpn and
    // --sni also construct an EndpointConfig when none exists yet). Guarding
    // mutual exclusion on has_value() would misfire on a plain
    // "--alpn moq-99 --endpoint h:443" that never mentions --url.
    bool endpoint_set = false;
    bool url_set = false;
    // Tracks whether --namespace was itself given, mirroring endpoint_set /
    // url_set above, so --url and --namespace can be refused as mutually
    // exclusive in either order rather than one silently overwriting the
    // other's track_namespace.
    bool namespace_set = false;
    // Recorded, not applied, inside the loop: --transport can come either
    // before or after --url, so the conflict/agreement check must happen
    // once after the loop rather than only when --transport is seen first.
    std::optional<transport::TransportKind> url_required_transport;

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
            transport_set = true;
            options.transport_explicit = true;
        } else if (argument == "--endpoint") {
            if (url_set) {
                throw std::runtime_error("--url and --endpoint are mutually exclusive");
            }
            // If --alpn/--sni already constructed an EndpointConfig, update the
            // four fields parse_endpoint owns in place rather than replacing
            // the whole struct. Replacing it discarded an earlier --alpn while
            // leaving endpoint_alpn_overridden true, so resolve_endpoint also
            // skipped its own draft-appropriate selection: the connection went
            // out with a struct default nobody chose. Mirrors what --url does.
            const transport::EndpointConfig parsed = parse_endpoint(require_value("--endpoint"));
            transport::EndpointConfig configured = parsed;
            if (options.endpoint.has_value()) {
                configured.alpn = options.endpoint->alpn;
                configured.sni = options.endpoint->sni;
            }
            options.endpoints.push_back(configured);
            options.endpoint = options.endpoints.front();
            endpoint_set = true;
        } else if (argument == "--url") {
            if (url_set) {
                throw std::runtime_error("--url was already given; only one --url is supported");
            }
            if (endpoint_set) {
                throw std::runtime_error("--url and --endpoint are mutually exclusive");
            }
            if (namespace_set) {
                throw std::runtime_error("--url and --namespace are mutually exclusive");
            }
            const auto url = parse_msf_url(require_value("--url"));

            // track_namespace is a flat string the transport layer splits on
            // '/', so a namespace tuple element containing a literal slash
            // cannot survive the round-trip.
            std::string flattened;
            for (std::size_t element_index = 0; element_index < url.track.namespace_tuple.size();
                 ++element_index) {
                const std::string& element = url.track.namespace_tuple[element_index];
                if (element.find('/') != std::string::npos) {
                    throw std::runtime_error(
                        "--url namespace tuple element \"" + element +
                        "\" contains a slash, which this publisher's flat namespace cannot represent");
                }
                if (element_index != 0) {
                    flattened.push_back('/');
                }
                flattened += element;
            }
            options.track_namespace = flattened;

            // The fragment is explicitly never sent (draft line ~3320), but the
            // query carries key-value parameters intended for the server at
            // connection init, so it must survive into the endpoint just like
            // --endpoint's own query handling (parse_endpoint keeps a query
            // inside `path` because it never splits on '?' at all). Fold it in
            // the same way here rather than dropping it.
            std::string endpoint_path = url.path;
            bool endpoint_path_explicit = url.path_explicit;
            if (!url.query.empty()) {
                endpoint_path += "?" + url.query;
                endpoint_path_explicit = true;
            }

            // If --alpn/--sni already constructed an EndpointConfig, update
            // the fields --url owns in place rather than replacing the whole
            // struct, so an earlier --alpn/--sni value survives.
            if (options.endpoint.has_value()) {
                options.endpoint->host = url.host;
                options.endpoint->port = url.port;
                options.endpoint->path = endpoint_path;
                options.endpoint->path_explicit = endpoint_path_explicit;
            } else {
                transport::EndpointConfig endpoint;
                endpoint.host = url.host;
                endpoint.port = url.port;
                endpoint.path = endpoint_path;
                endpoint.path_explicit = endpoint_path_explicit;
                options.endpoint = endpoint;
            }
            options.endpoints = {*options.endpoint};
            endpoint_set = true;
            url_set = true;

            if (url.connection != ConnectionRequirement::kAny) {
                // Recorded here and resolved once after the loop (see
                // url_required_transport below), so the outcome does not
                // depend on whether --transport appears before or after --url.
                url_required_transport = url.connection == ConnectionRequirement::kRawQuic
                                             ? transport::TransportKind::kRawQuic
                                             : transport::TransportKind::kWebTransport;
            }

            if (url.c4m_token.has_value()) {
                options.msf_c4m_token = *url.c4m_token;
                std::cerr << "msf_c4m_token present; this publisher does not consume CAT tokens"
                          << std::endl;
            }
            if (url.track.track_name != "catalog") {
                std::cerr << "--url track name \"" << url.track.track_name
                          << "\" ignored; publisher track names come from the media" << std::endl;
            }
        } else if (argument == "--alpn") {
            if (!options.endpoint.has_value()) {
                options.endpoint = transport::EndpointConfig{};
            }
            options.endpoint->alpn = std::string(require_value("--alpn"));
            for (auto& endpoint : options.endpoints) {
                endpoint.alpn = options.endpoint->alpn;
            }
            options.endpoint_alpn_overridden = true;
        } else if (argument == "--sni") {
            if (!options.endpoint.has_value()) {
                options.endpoint = transport::EndpointConfig{};
            }
            options.endpoint->sni = std::string(require_value("--sni"));
            for (auto& endpoint : options.endpoints) {
                endpoint.sni = options.endpoint->sni;
            }
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
            if (url_set) {
                throw std::runtime_error("--url and --namespace are mutually exclusive");
            }
            options.track_namespace = std::string(require_value("--namespace"));
            namespace_set = true;
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
        } else if (argument == "--retry") {
            options.retry_count = parse_retry_count(require_value("--retry"));
            retry_set = true;
        } else if (argument == "--preannounce-tracks") {
            options.preannounce_tracks = true;
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
        } else if (argument == "--print-msf-urls") {
            options.print_msf_urls = true;
        } else if (argument == "--version" || argument == "-V") {
            options.show_version = true;
            return options;
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
    if (retry_set && options.endpoints.empty()) {
        throw std::runtime_error("--retry requires --endpoint or --url");
    }

    // Resolve any --url connection requirement against --transport exactly
    // once, regardless of which flag appeared first on the command line.
    if (url_required_transport.has_value()) {
        if (transport_set && options.transport != *url_required_transport) {
            throw std::runtime_error(
                "--url specifies a connection type that conflicts with --transport");
        }
        options.transport = *url_required_transport;
        options.transport_explicit = true;
    }

    if (options.endpoint.has_value()) {
        options.endpoint->transport = options.transport;
        for (auto& endpoint : options.endpoints) {
            endpoint.transport = options.transport;
        }
    }
    if (options.transport == transport::TransportKind::kWebTransport) {
        if (!options.endpoint.has_value()) {
            throw std::runtime_error("--transport webtransport requires --endpoint");
        }
        const bool all_paths_explicit =
            std::all_of(options.endpoints.begin(), options.endpoints.end(),
                        [](const transport::EndpointConfig& endpoint) {
                            return endpoint.path_explicit;
                        });
        if (!all_paths_explicit) {
            if (options.endpoints.size() == 1) {
                throw std::runtime_error("--transport webtransport requires an endpoint path such as https://host:port/moq");
            }
            throw std::runtime_error(
                "all WebTransport endpoints require an explicit path such as https://host:port/moq");
        }
    }
    if (options.track_namespace.empty()) {
        throw std::runtime_error("--namespace must not be empty");
    }

    // Content protection is detected from the CMAF initialization segment's
    // sinf/schm/schi/tenc boxes and the moov-level pssh siblings, so it works
    // on any live path that receives a real init segment: the DASH CTE ingest
    // and the live stdin path both do. SRT does not -- it carries MPEG-TS and
    // the publisher synthesises a CMAF init segment from parsed elementary
    // streams, so there are no CENC boxes to detect and nothing --drm-config
    // could describe.
    if (!options.drm_systems.empty() && live_source_uses_srt) {
        throw std::runtime_error(
            "--drm-config is not supported with --live-source srt: SRT carries MPEG-TS, "
            "which has no CMAF CENC metadata (no sinf, tenc, or pssh boxes) for the "
            "publisher to detect, so there is no content protection to describe");
    }

    return options;
}

std::string build_version_banner() {
    return "openmoq-publisher " + std::string(version_full()) + " (commit " +
           std::string(git_commit()) + ")";
}

std::string build_usage(const char* argv0) {
    return build_version_banner() + "\nUsage: " + argv0 +
           " --input <mp4|-> [--live-source auto|stdin|srt|dash] [--srt-config <path>]"
           " [--dash-listen host:port] [--dash-path <prefix>] [--dash-queue-depth <count>]"
           " [--transport raw|webtransport] [--draft 14|16|17|18] [--namespace <value>] [--forward 0|1] [--timeout <seconds>]"
           " [--publish-catalog] [--sap] [--msf-timeline] [--coalesce-cmaf-chunks] [--stream-per-object] [--paced] [--loop] [--preannounce-tracks] [--dump-plan] [--print-msf-urls] [--emit-dir <dir>]"
           " [--vod] [--catalog-republish-interval <seconds>] [--drm-config <path>]"
           " [--endpoint host:port|moqt://host:port/path|https://host:port/path]... [--url moqt://host/path#msf:ns--track] [--alpn value] [--sni value]"
           " [--retry <count>]"
           " [--cert file] [--key file] [--ca file] [--insecure]"
           " [--version] [--help]";
}

}  // namespace openmoq::publisher
