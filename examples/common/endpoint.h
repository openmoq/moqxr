#pragma once

#include "openmoq/publisher/transport/publisher_transport.h"

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace openmoq::examples {

inline publisher::transport::EndpointConfig parse_endpoint(
    const std::string& value,
    const std::string& default_webtransport_path = "/moq") {
    using publisher::transport::EndpointConfig;
    using publisher::transport::TransportKind;

    if (value.find_first_of(" \t\r\n#") != std::string::npos) {
        throw std::runtime_error("endpoint must not contain whitespace or a fragment");
    }
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
        if (authority.find("://") != std::string::npos) {
            throw std::runtime_error("endpoint scheme must be https:// or moqt://");
        }
    }

    const auto path_start = authority.find_first_of("/?");
    if (path_start != std::string::npos) {
        endpoint.path = authority[path_start] == '?'
                            ? "/" + authority.substr(path_start)
                            : authority.substr(path_start);
        endpoint.path_explicit = true;
        authority.erase(path_start);
    } else if (endpoint.transport == TransportKind::kWebTransport) {
        endpoint.path = default_webtransport_path;
        endpoint.path_explicit = true;
    }

    std::size_t separator = std::string::npos;
    if (authority.starts_with('[')) {
        const auto bracket = authority.find(']');
        if (bracket == std::string::npos || bracket + 1 >= authority.size() ||
            authority[bracket + 1] != ':') {
            throw std::runtime_error("endpoint IPv6 literals must use [address]:port");
        }
        endpoint.host = authority.substr(1, bracket - 1);
        separator = bracket + 1;
    } else {
        separator = authority.find(':');
        if (separator == std::string::npos || separator != authority.rfind(':')) {
            throw std::runtime_error("endpoint must include host:port; bracket IPv6 addresses");
        }
        endpoint.host = authority.substr(0, separator);
    }
    if (endpoint.host.empty() || endpoint.host.find_first_of("@[]") != std::string::npos) {
        throw std::runtime_error("endpoint must include a non-empty host without user information");
    }
    const std::string_view port = std::string_view(authority).substr(separator + 1);
    std::uint32_t parsed_port = 0;
    const auto result = std::from_chars(port.data(), port.data() + port.size(), parsed_port);
    if (port.empty() || result.ec != std::errc{} || result.ptr != port.data() + port.size() ||
        parsed_port == 0 || parsed_port > 65535) {
        throw std::runtime_error("endpoint port must be an integer from 1 through 65535");
    }
    endpoint.port = static_cast<std::uint16_t>(parsed_port);
    return endpoint;
}

}  // namespace openmoq::examples
