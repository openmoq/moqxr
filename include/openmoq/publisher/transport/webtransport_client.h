#pragma once

#include <cstddef>
#include <memory>
#include <optional>

#include "openmoq/publisher/transport/publisher_transport.h"

namespace openmoq::publisher::transport {

class WebTransportClient final : public PublisherTransport {
public:
    WebTransportClient();
    explicit WebTransportClient(std::size_t media_capacity_for_testing);
    ~WebTransportClient() override;

    TransportStatus configure(const EndpointConfig& endpoint, const TlsConfig& tls) override;
    TransportStatus connect() override;
    ConnectionState state() const override;
    TransportStatus open_stream(StreamDirection direction, std::uint64_t& stream_id) override;
    TransportStatus accept_stream(StreamDirection direction,
                                  std::uint64_t& stream_id,
                                  std::chrono::milliseconds timeout) override;
    TransportStatus write_stream(std::uint64_t stream_id,
                                 std::span<const std::uint8_t> bytes,
                                 bool fin) override;
    ObjectWriteResult try_write_object(std::uint64_t stream_id,
                                       std::span<const std::uint8_t> bytes,
                                       bool fin,
                                       ObjectWriteOptions options) override;
    TransportStatus read_stream(std::uint64_t stream_id,
                                std::vector<std::uint8_t>& bytes,
                                bool& fin,
                                std::chrono::milliseconds timeout) override;
    TransportStatus reset_stream(std::uint64_t stream_id,
                                 std::uint64_t error_code) override;
    std::string connection_id() const override;
    void note_delivery_timeout(std::chrono::milliseconds timeout) override;
    bool media_stream_expired(std::uint64_t stream_id) const override;
    TransportStatus close(std::uint64_t application_error_code) override;

private:
public:
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    std::optional<EndpointConfig> endpoint_;
    std::optional<TlsConfig> tls_;
    ConnectionState state_ = ConnectionState::kIdle;
    std::uint64_t next_bidirectional_stream_id_ = 0;
    std::uint64_t next_unidirectional_stream_id_ = 2;
};

}  // namespace openmoq::publisher::transport
