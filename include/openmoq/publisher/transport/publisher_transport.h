#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

struct ObjectWriteOptions {
    std::uint8_t transport_priority = 255;
    std::optional<std::chrono::steady_clock::time_point> object_deadline;
    std::optional<std::chrono::steady_clock::time_point> subgroup_deadline;
};

enum class ObjectWriteDisposition { kAccepted, kWouldBlock, kFailed };

struct ObjectWriteResult {
    ObjectWriteDisposition disposition = ObjectWriteDisposition::kFailed;
    std::string message;
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
    virtual ObjectWriteResult try_write_object(std::uint64_t stream_id,
                                               std::span<const std::uint8_t> bytes,
                                               bool fin,
                                               ObjectWriteOptions options) {
        static_cast<void>(options);
        TransportStatus status = write_stream(stream_id, bytes, fin);
        return {
            .disposition = status.ok ? ObjectWriteDisposition::kAccepted
                                     : ObjectWriteDisposition::kFailed,
            .message = std::move(status.message),
        };
    }
    virtual TransportStatus read_stream(std::uint64_t stream_id,
                                        std::vector<std::uint8_t>& bytes,
                                        bool& fin,
                                        std::chrono::milliseconds timeout) = 0;
    virtual TransportStatus reset_stream(std::uint64_t stream_id,
                                         std::uint64_t error_code) = 0;
    virtual std::string connection_id() const = 0;
    // Reports a delivery timeout negotiated on a subscription (MoQT
    // DELIVERY_TIMEOUT / OBJECT_DELIVERY_TIMEOUT / SUBGROUP_DELIVERY_TIMEOUT).
    // Transports use the largest value reported as the bound on how long
    // close() lets queued and in-flight stream data drain before the
    // connection is torn down (draft -16 9.15 / -18 10.11). Optional: the
    // default ignores it and transports fall back to a built-in bound.
    virtual void note_delivery_timeout(std::chrono::milliseconds timeout) { static_cast<void>(timeout); }
    // Stable, non-destructive notification that the transport has already
    // reset this media stream because a delivery deadline won. Sessions use
    // it to retire the owning subgroup without conflating timeout with the
    // three admission outcomes. Optional transports never report expiry.
    virtual bool media_stream_expired(std::uint64_t stream_id) const {
        static_cast<void>(stream_id);
        return false;
    }
    virtual TransportStatus close(std::uint64_t application_error_code) = 0;
};

}  // namespace openmoq::publisher::transport
