#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openmoq::publisher::transport {

enum class TransportKind {
    kRawQuic,
    kWebTransport,
};

enum class StreamDirection {
    kBidirectional,
    kUnidirectional,
};

enum class ConnectionState {
    kIdle,
    kConnecting,
    kConnected,
    kClosed,
    kFailed,
};

struct EndpointConfig {
    TransportKind transport = TransportKind::kRawQuic;
    std::string host;
    std::uint16_t port = 0;
    std::string alpn = "moq-00";
    std::string application_protocol;
    std::string sni;
    std::string path = "/";
    bool path_explicit = false;
};

struct TlsConfig {
    std::string certificate_path;
    std::string private_key_path;
    // PEM CA bundle used to verify the server certificate. When empty and
    // insecure_skip_verify is false, $SSL_CERT_FILE and then the system CA
    // bundle are used; if no usable bundle is found, connect() fails with an
    // actionable error rather than silently skipping verification.
    std::string ca_path;
    // When true, the server certificate is NOT verified. Only use this for
    // local testing against relays with self-signed certificates.
    bool insecure_skip_verify = false;
};

struct TransportStatus {
    bool ok = true;
    std::string message;

    static TransportStatus success();
    static TransportStatus failure(std::string_view error_message);
};

class PublisherTransport {
public:
    virtual ~PublisherTransport() = default;

    virtual TransportStatus configure(const EndpointConfig& endpoint, const TlsConfig& tls) = 0;
    virtual TransportStatus connect() = 0;
    virtual ConnectionState state() const = 0;
    virtual TransportStatus open_stream(StreamDirection direction, std::uint64_t& stream_id) = 0;
    virtual TransportStatus accept_stream(StreamDirection direction,
                                          std::uint64_t& stream_id,
                                          std::chrono::milliseconds timeout) = 0;
    virtual TransportStatus write_stream(std::uint64_t stream_id,
                                         std::span<const std::uint8_t> bytes,
                                         bool fin) = 0;
    virtual TransportStatus read_stream(std::uint64_t stream_id,
                                        std::vector<std::uint8_t>& bytes,
                                        bool& fin,
                                        std::chrono::milliseconds timeout) = 0;
    virtual TransportStatus reset_stream(std::uint64_t stream_id,
                                         std::uint64_t error_code) = 0;
    virtual std::string connection_id() const = 0;
    virtual TransportStatus close(std::uint64_t application_error_code) = 0;
};

}  // namespace openmoq::publisher::transport
