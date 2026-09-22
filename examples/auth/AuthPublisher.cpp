#include "catapult_client.h"
#include "../common/endpoint.h"

#include "openmoq/publisher/cat4moq.h"
#include "openmoq/publisher/live_object.h"
#include "openmoq/publisher/publisher_api.h"
#include "openmoq/publisher/transport/publisher_transport.h"

#include <charconv>
#include <chrono>
#include <limits>
#include <set>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
    bool insecure_skip_verify = false;
    publisher::DraftVersion draft = publisher::DraftVersion::kDraft16;
    auth_example::TokenEncoding token_encoding = auth_example::TokenEncoding::kAuto;
    std::optional<TokenWrapper> token_wrapper;
    publisher::cat4moq::Profile profile = publisher::cat4moq::Profile::kMoqxCompat;
    std::optional<std::uint64_t> token_type;
    std::optional<std::filesystem::path> token_file;
    std::optional<std::filesystem::path> setup_token_file;
    std::optional<std::filesystem::path> action_token_file;
    std::optional<std::string> catapult_command;
    std::optional<std::filesystem::path> dpop_key_file;
    std::optional<std::uint64_t> dpop_token_type;
    bool print_dpop_jkt = false;
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

publisher::cat4moq::Profile parse_profile(std::string_view value) {
    using publisher::cat4moq::Profile;
    if (value == "c4m-01") return Profile::kC4m01;
    if (value == "moqx-compat") return Profile::kMoqxCompat;
    if (value == "red5-cose-compat") return Profile::kRed5CoseCompat;
    throw std::runtime_error("--auth-profile must be c4m-01, moqx-compat, or red5-cose-compat");
}

std::uint64_t parse_unsigned(std::string_view value, const char* flag) {
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        throw std::runtime_error(std::string(flag) + " requires an unsigned integer");
    }
    return result;
}

Args parse_args(int argc, char** argv) {
    Args args;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (!seen.insert(flag).second) throw std::runtime_error("duplicate example option");
        if (flag == "--endpoint") {
            args.endpoint = require_value(i, argc, argv, "--endpoint");
        } else if (flag == "--namespace") {
            args.track_namespace = require_value(i, argc, argv, "--namespace");
        } else if (flag == "--track") {
            args.track_name = require_value(i, argc, argv, "--track");
        } else if (flag == "--seconds") {
            const auto seconds = parse_unsigned(require_value(i, argc, argv, "--seconds"), "--seconds");
            if (seconds == 0 || seconds > std::numeric_limits<int>::max() / 10) {
                throw std::runtime_error("--seconds is outside the supported object-count range");
            }
            args.seconds = static_cast<int>(seconds);
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
        } else if (flag == "--auth-profile") {
            args.profile = parse_profile(require_value(i, argc, argv, "--auth-profile"));
        } else if (flag == "--auth-token-type") {
            args.token_type = parse_unsigned(require_value(i, argc, argv, "--auth-token-type"), "--auth-token-type");
        } else if (flag == "--token-wrapper") {
            args.token_wrapper = parse_token_wrapper(require_value(i, argc, argv, "--token-wrapper"));
        } else if (flag == "--dpop-key-file") {
            args.dpop_key_file = std::filesystem::path(require_value(i, argc, argv, "--dpop-key-file"));
        } else if (flag == "--dpop-token-type") {
            args.dpop_token_type = parse_unsigned(require_value(i, argc, argv, "--dpop-token-type"), "--dpop-token-type");
        } else if (flag == "--print-dpop-jkt") {
            args.print_dpop_jkt = true;
        } else if (flag == "--help" || flag == "-h") {
            throw std::runtime_error("");
        } else {
            throw std::runtime_error("unknown argument: " + flag);
        }
    }
    if (args.token_file && (args.setup_token_file || args.action_token_file)) {
        throw std::runtime_error("--token-file conflicts with separate setup/action files");
    }
    if (args.catapult_command && (args.token_file || args.setup_token_file || args.action_token_file)) {
        throw std::runtime_error("token files conflict with --catapult-command");
    }
    for (const auto* file : {&args.token_file, &args.setup_token_file, &args.action_token_file}) {
        if (*file && (*file)->empty()) throw std::runtime_error("token file path must not be empty");
    }
    if (args.catapult_command && args.catapult_command->empty()) throw std::runtime_error("token command must not be empty");
    if (args.token_wrapper && (seen.contains("--auth-profile") || args.token_type)) {
        throw std::runtime_error("legacy --token-wrapper conflicts with profile/type options");
    }
    if (args.token_type && (!seen.contains("--auth-profile") || args.profile == publisher::cat4moq::Profile::kC4m01)) {
        throw std::runtime_error("--auth-token-type requires an explicit compatibility profile");
    }
    if (args.dpop_key_file && args.dpop_key_file->empty()) throw std::runtime_error("DPoP key file path must not be empty");
    if (args.dpop_token_type && !args.dpop_key_file) throw std::runtime_error("--dpop-token-type requires --dpop-key-file");
    if (args.print_dpop_jkt && !args.dpop_key_file) throw std::runtime_error("--print-dpop-jkt requires --dpop-key-file");
    if (args.track_name.empty() || args.track_namespace.empty()) throw std::runtime_error("namespace and track must not be empty");
    return args;
}

publisher::cat4moq::DpopSigner load_dpop_signer(const Args& args) {
    std::ifstream input(*args.dpop_key_file, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open DPoP key file");
    std::string pem(publisher::cat4moq::kMaxCredentialBytes + 1, '\0');
    input.read(pem.data(), static_cast<std::streamsize>(pem.size()));
    pem.resize(static_cast<std::size_t>(input.gcount()));
    if (input.bad() || pem.size() > publisher::cat4moq::kMaxCredentialBytes) {
        throw std::runtime_error("DPoP key file cannot be read or exceeds 16384 bytes");
    }
    auto signer = publisher::cat4moq::DpopSigner::from_pem(pem);
    if (args.dpop_token_type) signer.token_type = *args.dpop_token_type;
    return signer;
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
            .subgroup_contains_group_largest = true,
            .final_in_subgroup = (current % kObjectsPerSecond) == (kObjectsPerSecond - 1),
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

publisher::cat4moq::AuthorizationConfig make_authorization(const Args& args) {
    publisher::cat4moq::AuthorizationConfig authorization;
    const auto acquire = [&](bool setup, const std::optional<std::filesystem::path>& file) {
        if (!file && !args.catapult_command) return;
        auto client = make_client(args, file);
        auto bytes = client.issue_token({
            .action = setup ? "client_setup" : "publish_namespace,publish",
            .track_namespace = args.track_namespace,
            .track_name = args.track_name,
            .endpoint = args.endpoint,
        });
        if (args.token_wrapper) {
            auto& target = setup ? authorization.setup_token : authorization.action_token;
            target = wrap_token(std::move(bytes), *args.token_wrapper);
        } else {
            auto& target = setup ? authorization.setup_credential : authorization.action_credential;
            target = publisher::cat4moq::Credential{
                .cwt = std::move(bytes), .profile = args.profile, .token_type = args.token_type,
            };
        }
    };
    acquire(true, args.setup_token_file ? args.setup_token_file : args.token_file);
    acquire(false, args.action_token_file ? args.action_token_file : args.token_file);
    if (!authorization.configured()) throw std::runtime_error("no CAT4MOQ token source configured");
    if (args.dpop_key_file) authorization.dpop_signer = load_dpop_signer(args);
    publisher::cat4moq::validate_authorization(authorization, args.draft);
    return authorization;
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
        << "  --auth-profile c4m-01|moqx-compat|red5-cose-compat  Default: moqx-compat\n"
        << "  --auth-token-type N             Override type with explicit compatibility profile\n"
        << "  --token-wrapper cat|out-of-band|none  Legacy preencoded mode (conflicts with profile/type)\n"
        << "  --dpop-key-file PATH            P-256 PEM key; signs a DPoP proof for a cnf-bound token on every message\n"
        << "  --dpop-token-type N             Token Type of the proof parameter. Default: 17\n"
        << "  --print-dpop-jkt                Print the key's RFC 7638 thumbprint (for the issuer's cnf.jkt) and exit\n"
        << "  --insecure-skip-verify 0|1       Default: 0 (verify TLS)\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Args args = parse_args(argc, argv);
        if (args.print_dpop_jkt) {
            std::cout << load_dpop_signer(args).thumbprint_hex() << '\n';
            return 0;
        }
        const transport::EndpointConfig endpoint = openmoq::examples::parse_endpoint(args.endpoint);

        auto authorization = make_authorization(args);

        publisher::PublisherConfig config;
        config.draft_version = args.draft;
        config.track_namespace = args.track_namespace;
        config.forward = args.forward;
        config.publish_catalog = false;
        config.paced = true;
        config.preannounce_tracks = true;
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
        if (stats.objects_published == 0) throw std::runtime_error("no objects published; check relay subscriber interest");
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
