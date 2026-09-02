#include "openmoq/publisher/transport/webtransport_client.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using openmoq::publisher::transport::ConnectionState;
using openmoq::publisher::transport::EndpointConfig;
using openmoq::publisher::transport::ObjectWriteDisposition;
using openmoq::publisher::transport::ObjectWriteOptions;
using openmoq::publisher::transport::StreamDirection;
using openmoq::publisher::transport::TlsConfig;
using openmoq::publisher::transport::TransportKind;
using openmoq::publisher::transport::WebTransportClient;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    bool ok = true;

    {
        WebTransportClient client(10);
        const std::vector<std::uint8_t> bytes(8, 0x11);
        const auto started = std::chrono::steady_clock::now();
        const auto result = client.try_write_object(2, bytes, true, ObjectWriteOptions{});
        const auto elapsed = std::chrono::steady_clock::now() - started;
        ok &= expect(result.disposition == ObjectWriteDisposition::kFailed,
                     "expected media admission on a disconnected WebTransport client to fail");
        ok &= expect(!result.message.empty(),
                     "expected failed WebTransport media admission to explain the failure");
        ok &= expect(elapsed < std::chrono::seconds(1),
                     "expected failed WebTransport media admission to return synchronously");

        const auto status = client.connect();
        ok &= expect(!status.ok, "expected connect without configure to fail");
        ok &= expect(status.message == "webtransport transport is not configured",
                     "expected connect without configure to report missing config");
        ok &= expect(client.state() == ConnectionState::kFailed,
                     "expected failed state after connect without configure");
    }

    {
        WebTransportClient client;
        EndpointConfig endpoint{
            .transport = TransportKind::kWebTransport,
            .host = "127.0.0.1",
            .port = 443,
            .alpn = "h3",
            .application_protocol = "moq-00",
            .sni = {},
            .path = "",
            .path_explicit = false,
        };
        const auto configure_status = client.configure(endpoint, TlsConfig{});
        ok &= expect(configure_status.ok, "expected configure to succeed");

        std::uint64_t stream_id = 0;
        auto status = client.open_stream(StreamDirection::kBidirectional, stream_id);
        ok &= expect(!status.ok, "expected open_stream before connect to fail");
        ok &= expect(status.message == "transport is not connected",
                     "expected open_stream before connect to report disconnected transport");

        status = client.connect();
        ok &= expect(!status.ok, "expected WebTransport connect without path to fail");
        ok &= expect(status.message == "webtransport endpoint path must not be empty",
                     "expected empty WebTransport path to be rejected");
        ok &= expect(client.state() == ConnectionState::kFailed,
                     "expected failed state after invalid WebTransport connect");
    }

    {
        WebTransportClient client;
        const auto status = client.close(0);
        ok &= expect(status.ok, "expected close on idle client to succeed");
        ok &= expect(client.state() == ConnectionState::kClosed,
                     "expected close on idle client to transition to closed");
    }

    return ok ? 0 : 1;
}
