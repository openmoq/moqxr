#include "msfts_options.h"

#include <charconv>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace openmoq::examples::msfts {

namespace {

std::uint64_t parse_unsigned(std::string_view value,
                             std::uint64_t maximum,
                             const char* option) {
    std::uint64_t parsed = 0;
    const auto result =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (value.empty() || result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() || parsed > maximum) {
        throw std::runtime_error(std::string(option) +
                                 " requires an unsigned integer no greater than " +
                                 std::to_string(maximum));
    }
    return parsed;
}

publisher::DraftVersion parse_draft(const std::string& value) {
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

}  // namespace

MsftsPublisherOptions parse_msfts_options(
    const std::vector<std::string>& arguments) {
    MsftsPublisherOptions options;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];
        const auto require_value = [&](const char* option) -> const std::string& {
            if (index + 1 >= arguments.size()) {
                throw std::runtime_error(std::string("missing value for ") + option);
            }
            return arguments[++index];
        };

        if (argument == "--endpoint") {
            options.endpoint = require_value("--endpoint");
        } else if (argument == "--input") {
            options.input_path = require_value("--input");
        } else if (argument == "--namespace") {
            options.track_namespace = require_value("--namespace");
        } else if (argument == "--track") {
            options.track_name = require_value("--track");
        } else if (argument == "--program") {
            options.program = static_cast<std::uint16_t>(
                parse_unsigned(require_value("--program"),
                               std::numeric_limits<std::uint16_t>::max(),
                               "--program"));
        } else if (argument == "--packets-per-object") {
            options.packets_per_object = static_cast<std::size_t>(
                parse_unsigned(require_value("--packets-per-object"),
                               std::numeric_limits<std::size_t>::max(),
                               "--packets-per-object"));
            if (options.packets_per_object == 0) {
                throw std::runtime_error("--packets-per-object must be greater than zero");
            }
        } else if (argument == "--draft") {
            options.draft = parse_draft(require_value("--draft"));
        } else if (argument == "--insecure") {
            options.insecure = true;
        } else if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else {
            throw std::runtime_error("unknown argument: " + argument);
        }
    }

    if (!options.help && options.input_path.empty()) {
        throw std::runtime_error("--input is required");
    }
    if (options.track_namespace.empty()) {
        throw std::runtime_error("--namespace must not be empty");
    }
    if (options.track_name.empty()) {
        throw std::runtime_error("--track must not be empty");
    }
    return options;
}

publisher::transport::EndpointConfig parse_msfts_endpoint(
    const std::string& value) {
    using publisher::transport::EndpointConfig;
    using publisher::transport::TransportKind;

    EndpointConfig endpoint;
    std::string authority = value;
    if (authority.starts_with("https://")) {
        endpoint.transport = TransportKind::kWebTransport;
        authority.erase(0, 8);
    } else if (authority.starts_with("moqt://")) {
        endpoint.transport = TransportKind::kRawQuic;
        authority.erase(0, 7);
    } else {
        endpoint.transport = TransportKind::kRawQuic;
    }

    const std::size_t slash = authority.find('/');
    if (slash != std::string::npos) {
        endpoint.path = authority.substr(slash);
        endpoint.path_explicit = true;
        authority.erase(slash);
    } else if (endpoint.transport == TransportKind::kWebTransport) {
        endpoint.path = "/";
        endpoint.path_explicit = true;
    }

    std::size_t port_separator = std::string::npos;
    if (authority.starts_with('[')) {
        const std::size_t bracket = authority.find(']');
        if (bracket == std::string::npos || bracket + 1 >= authority.size() ||
            authority[bracket + 1] != ':') {
            throw std::runtime_error(
                "endpoint IPv6 literals must use [address]:port");
        }
        endpoint.host = authority.substr(1, bracket - 1);
        port_separator = bracket + 1;
    } else {
        port_separator = authority.rfind(':');
        if (port_separator == std::string::npos) {
            throw std::runtime_error("endpoint must include host:port");
        }
        endpoint.host = authority.substr(0, port_separator);
    }
    if (endpoint.host.empty() || port_separator + 1 >= authority.size()) {
        throw std::runtime_error("endpoint must include a non-empty host and port");
    }
    endpoint.port = static_cast<std::uint16_t>(
        parse_unsigned(std::string_view(authority).substr(port_separator + 1),
                       std::numeric_limits<std::uint16_t>::max(),
                       "endpoint port"));
    if (endpoint.port == 0) {
        throw std::runtime_error("endpoint port must be greater than zero");
    }
    return endpoint;
}

std::string msfts_usage(const char* executable) {
    return std::string("Usage: ") + executable +
           " --input FILE [--endpoint URL] [--namespace NAME] [--track NAME]\n"
           "       [--program NUMBER] [--packets-per-object COUNT]\n"
           "       [--draft 14|16|17|18] [--insecure]\n";
}

}  // namespace openmoq::examples::msfts
