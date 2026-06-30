#include "catapult_client.h"

#include "openmoq/publisher/cat4moq.h"
#include "openmoq/publisher/live_object.h"
#include "openmoq/publisher/publisher_api.h"
#include "openmoq/publisher/transport/publisher_transport.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace auth_example = openmoq::publisher::examples::auth;
namespace publisher = openmoq::publisher;
namespace transport = openmoq::publisher::transport;

enum class TokenWrapper {
    kCat,
    kOutOfBand,
    kNone,
};

struct Args {
    std::string endpoint = "https://127.0.0.1:4433/moq";
    std::string track_namespace = "cat4moq.example";
    std::string track_name = "video";
    int seconds = 3;
    bool forward = true;
    bool insecure_skip_verify = true;
    publisher::DraftVersion draft = publisher::DraftVersion::kDraft16;
    auth_example::TokenEncoding token_encoding = auth_example::TokenEncoding::kAuto;
    TokenWrapper token_wrapper = TokenWrapper::kCat;
    std::optional<std::filesystem::path> token_file;
    std::optional<std::filesystem::path> setup_token_file;
    std::optional<std::filesystem::path> action_token_file;
    std::optional<std::string> catapult_command;
};

std::string require_value(int& index, int argc, char** argv, const char* flag) {
    if (index + 1 >= argc) {
        throw std::runtime_error(std::string("missing value for ") + flag);
    }
    ++index;
    return argv[index];
}

bool parse_bool(std::string_view value, const char* flag) {
    if (value == "1" || value == "true" || value == "yes") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no") {
        return false;
    }
    throw std::runtime_error(std::string(flag) + " must be 0 or 1");
}

publisher::DraftVersion parse_draft(std::string_view value) {
    if (value == "14") {
        return publisher::DraftVersion::kDraft14;
    }
    if (value == "16") {
        return publisher::DraftVersion::kDraft16;
    }
    if (value == "17") {
        return publisher::DraftVersion::kDraft17;
    }
    if (value == "18") {
        return publisher::DraftVersion::kDraft18;
    }
    throw std::runtime_error("--draft must be one of: 14, 16, 17, 18");
}

TokenWrapper parse_token_wrapper(std::string_view value) {
    if (value == "cat") {
        return TokenWrapper::kCat;
    }
    if (value == "out-of-band") {
        return TokenWrapper::kOutOfBand;
    }
    if (value == "none") {
        return TokenWrapper::kNone;
    }
    throw std::runtime_error("--token-wrapper must be cat, out-of-band, or none");
}

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--endpoint") {
            args.endpoint = require_value(i, argc, argv, "--endpoint");
        } else if (flag == "--namespace") {
            args.track_namespace = require_value(i, argc, argv, "--namespace");
        } else if (flag == "--track") {
            args.track_name = require_value(i, argc, argv, "--track");
        } else if (flag == "--seconds") {
            args.seconds = std::stoi(require_value(i, argc, argv, "--seconds"));
            if (args.seconds <= 0) {
                throw std::runtime_error("--seconds must be > 0");
            }
        } else if (flag == "--draft") {
            args.draft = parse_draft(require_value(i, argc, argv, "--draft"));
        } else if (flag == "--forward") {
            args.forward = parse_bool(require_value(i, argc, argv, "--forward"), "--forward");
        } else if (flag == "--insecure-skip-verify") {
            args.insecure_skip_verify =
                parse_bool(require_value(i, argc, argv, "--insecure-skip-verify"), "--insecure-skip-verify");
        } else if (flag == "--token-file") {
            args.token_file = std::filesystem::path(require_value(i, argc, argv, "--token-file"));
        } else if (flag == "--setup-token-file") {
            args.setup_token_file = std::filesystem::path(require_value(i, argc, argv, "--setup-token-file"));
        } else if (flag == "--action-token-file") {
            args.action_token_file = std::filesystem::path(require_value(i, argc, argv, "--action-token-file"));
        } else if (flag == "--catapult-command") {
            args.catapult_command = require_value(i, argc, argv, "--catapult-command");
        } else if (flag == "--token-encoding") {
            args.token_encoding = auth_example::parse_token_encoding(require_value(i, argc, argv, "--token-encoding"));
        } else if (flag == "--token-wrapper") {
            args.token_wrapper = parse_token_wrapper(require_value(i, argc, argv, "--token-wrapper"));
        } else if (flag == "--help" || flag == "-h") {
            throw std::runtime_error("");
        } else {
            throw std::runtime_error("unknown argument: " + flag);
        }
    }
    return args;
}

transport::EndpointConfig parse_endpoint(const std::string& raw) {
    transport::EndpointConfig endpoint;
    endpoint.transport = transport::TransportKind::kRawQuic;
    endpoint.path = "/";
    endpoint.path_explicit = false;

    std::string authority = raw;
    const auto consume_scheme = [&](std::string_view prefix) {
        if (authority.rfind(prefix, 0) == 0) {
            authority = authority.substr(prefix.size());
            return true;
        }
        return false;
    };

    const bool webtransport = consume_scheme("https://");
    const bool raw_quic = consume_scheme("moqt://");
    if (webtransport || raw_quic) {
        endpoint.transport = webtransport ? transport::TransportKind::kWebTransport : transport::TransportKind::kRawQuic;
        const std::size_t slash = authority.find('/');
        if (slash != std::string::npos) {
            endpoint.path = authority.substr(slash);
            endpoint.path_explicit = true;
            authority = authority.substr(0, slash);
        } else if (webtransport) {
            endpoint.path = "/moq";
            endpoint.path_explicit = true;
        }
    }

    const std::size_t colon = authority.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= authority.size()) {
        throw std::runtime_error("endpoint must be host:port, moqt://host:port/path, or https://host:port/path");
    }
    endpoint.host = authority.substr(0, colon);
    const int port = std::stoi(authority.substr(colon + 1));
    if (port <= 0 || port > 65535) {
        throw std::runtime_error("endpoint port must be between 1 and 65535");
    }
    endpoint.port = static_cast<std::uint16_t>(port);
    return endpoint;
}

publisher::cat4moq::AuthorizationToken wrap_token(std::vector<std::uint8_t> bytes, TokenWrapper wrapper) {
    if (wrapper == TokenWrapper::kCat) {
        return publisher::cat4moq::wrap_cat_token(bytes);
    }
    if (wrapper == TokenWrapper::kOutOfBand) {
        return publisher::cat4moq::wrap_out_of_band_token(bytes);
    }
    return publisher::cat4moq::AuthorizationToken{.bytes = std::move(bytes)};
}

publisher::LiveObjectSource make_source(std::string track_name, int seconds) {
    constexpr int kObjectsPerSecond = 10;
    const int object_count = seconds * kObjectsPerSecond;
    auto index = std::make_shared<int>(0);

    publisher::LiveObjectSource source;
    source.tracks.push_back(publisher::LiveTrack{.track_name = track_name});
    source.next_object = [track_name = std::move(track_name), object_count, index]() mutable
        -> std::optional<publisher::LiveObject> {
        if (*index >= object_count) {
            return std::nullopt;
        }
        const int current = (*index)++;
        const std::string payload_text = "cat4moq-auth-frame-" + std::to_string(current);
        std::vector<std::uint8_t> payload(payload_text.begin(), payload_text.end());
        return publisher::LiveObject{
            .track_name = track_name,
            .group_id = static_cast<std::size_t>(current / kObjectsPerSecond),
            .subgroup_id = 0,
            .object_id = static_cast<std::size_t>(current % kObjectsPerSecond),
            .media_time_us = static_cast<std::uint64_t>(current) * 100000,
            .media_duration_us = 100000,
            .payload = std::move(payload),
            .subgroup_contains_group_largest = (current % kObjectsPerSecond) == (kObjectsPerSecond - 1) ||
                                               current == (object_count - 1),
            .final_in_subgroup = true,
        };
    };
    return source;
}

auth_example::CatapultClient make_client(const Args& args, std::optional<std::filesystem::path> token_file) {
    auth_example::CatapultClientOptions options;
    options.token_file = std::move(token_file);
    options.command = args.catapult_command;
    options.encoding = args.token_encoding;
    return auth_example::CatapultClient(std::move(options));
}

void print_usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [options]\n"
        << "  --endpoint URL                  Default: https://127.0.0.1:4433/moq\n"
        << "  --namespace NAME                Default: cat4moq.example\n"
        << "  --track NAME                    Default: video\n"
        << "  --draft 14|16|17|18             Default: 16\n"
        << "  --seconds N                     Default: 3\n"
        << "  --forward 0|1                   Default: 1\n"
        << "  --token-file PATH               Use one token for setup and action requests\n"
        << "  --setup-token-file PATH         Setup token source\n"
        << "  --action-token-file PATH        Action token source\n"
        << "  --catapult-command COMMAND      Token command; supports {action}, {namespace}, {track}, {endpoint}\n"
        << "  --token-encoding auto|raw|base64|hex\n"
        << "  --token-wrapper cat|out-of-band|none\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Args args = parse_args(argc, argv);
        const transport::EndpointConfig endpoint = parse_endpoint(args.endpoint);

        publisher::cat4moq::AuthorizationConfig authorization;
        const std::optional<std::filesystem::path> setup_file =
            args.setup_token_file.has_value() ? args.setup_token_file : args.token_file;
        const std::optional<std::filesystem::path> action_file =
            args.action_token_file.has_value() ? args.action_token_file : args.token_file;

        if (setup_file.has_value() || args.catapult_command.has_value()) {
            auth_example::CatapultClient client = make_client(args, setup_file);
            authorization.setup_token = wrap_token(
                client.issue_token(auth_example::CatapultTokenRequest{
                    .action = "client_setup",
                    .track_namespace = args.track_namespace,
                    .track_name = args.track_name,
                    .endpoint = args.endpoint,
                }),
                args.token_wrapper);
        }
        if (action_file.has_value() || args.catapult_command.has_value()) {
            auth_example::CatapultClient client = make_client(args, action_file);
            authorization.action_token = wrap_token(
                client.issue_token(auth_example::CatapultTokenRequest{
                    .action = "publish",
                    .track_namespace = args.track_namespace,
                    .track_name = args.track_name,
                    .endpoint = args.endpoint,
                }),
                args.token_wrapper);
        }

        if (!authorization.setup_token.has_value() && !authorization.action_token.has_value()) {
            throw std::runtime_error("no CAT4MOQ token source configured");
        }

        publisher::PublisherConfig config;
        config.draft_version = args.draft;
        config.track_namespace = args.track_namespace;
        config.forward = args.forward;
        config.publish_catalog = false;
        config.paced = false;
        config.subscriber_timeout = std::chrono::seconds(2);
        config.authorization = std::move(authorization);

        transport::TlsConfig tls;
        tls.insecure_skip_verify = args.insecure_skip_verify;

        publisher::Publisher auth_publisher(config);
        const transport::TransportStatus status =
            auth_publisher.publish_live_objects(make_source(args.track_name, args.seconds), endpoint, tls);
        if (!status.ok) {
            throw std::runtime_error("publish_live_objects failed: " + status.message);
        }

        const transport::TransportStatus close_status = auth_publisher.disconnect(0);
        if (!close_status.ok) {
            throw std::runtime_error("disconnect failed: " + close_status.message);
        }

        const auto stats = auth_publisher.stats();
        std::cout << "[cat4moq-auth] published bytes=" << stats.bytes_published
                  << " objects=" << stats.objects_published
                  << " groups=" << stats.groups_published << '\n';
        return 0;
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).empty()) {
            print_usage(argv[0]);
            return 0;
        }
        std::cerr << "error: " << error.what() << '\n';
        print_usage(argv[0]);
        return 1;
    }
}
