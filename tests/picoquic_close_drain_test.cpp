// Regression test: PicoquicClient::close() must let already-written stream
// data (and its FIN) reach the peer before it emits CONNECTION_CLOSE.
//
// MoQT's wind-down is stream FIN -> PUBLISH_DONE -> let delivery complete
// (draft-ietf-moq-transport-16 section 9.15 / -18 section 10.11: "stream
// state can persist until delivery completes ... only deleting it when all
// such streams have received ACK of the FIN"). picoquic_close() moves the
// connection straight to disconnecting without flushing queued stream data,
// so a close issued right behind the last write used to truncate the tail of
// the last subgroup. This test writes a large stream with FIN, calls close()
// immediately, and asserts the server received every byte and the FIN.

#include "openmoq/publisher/transport/picoquic_client.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <picoquic.h>
#include <picoquic_packet_loop.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using openmoq::publisher::transport::ConnectionState;
using openmoq::publisher::transport::EndpointConfig;
using openmoq::publisher::transport::PicoquicClient;
using openmoq::publisher::transport::StreamDirection;
using openmoq::publisher::transport::TlsConfig;

constexpr const char* kPicoquicSourceDir = OPENMOQ_PICOQUIC_SOURCE_DIR;
constexpr std::size_t kPayloadBytes = 512 * 1024;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

struct DrainServer {
    picoquic_quic_t* quic = nullptr;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable condition;
    uint16_t port = 0;
    std::size_t bytes_received = 0;
    bool fin_received = false;
    bool connection_closed = false;
    bool loop_ready = false;
    bool stop_requested = false;
    bool loop_exited = false;
};

int drain_server_callback(picoquic_cnx_t* cnx,
                          uint64_t stream_id,
                          uint8_t* bytes,
                          size_t length,
                          picoquic_call_back_event_t event,
                          void* callback_ctx,
                          void* stream_ctx) {
    static_cast<void>(cnx);
    static_cast<void>(stream_id);
    static_cast<void>(bytes);
    static_cast<void>(stream_ctx);
    auto* server = static_cast<DrainServer*>(callback_ctx);
    if (server == nullptr) {
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }
    switch (event) {
        case picoquic_callback_stream_data:
        case picoquic_callback_stream_fin: {
            std::lock_guard<std::mutex> lock(server->mutex);
            server->bytes_received += length;
            if (event == picoquic_callback_stream_fin) {
                server->fin_received = true;
            }
            server->condition.notify_all();
            return 0;
        }
        case picoquic_callback_close:
        case picoquic_callback_application_close:
        case picoquic_callback_stateless_reset: {
            std::lock_guard<std::mutex> lock(server->mutex);
            server->connection_closed = true;
            server->condition.notify_all();
            return 0;
        }
        default:
            return 0;
    }
}

int drain_server_loop_callback(picoquic_quic_t* quic,
                               picoquic_packet_loop_cb_enum cb_mode,
                               void* callback_ctx,
                               void* callback_arg) {
    static_cast<void>(quic);
    auto* server = static_cast<DrainServer*>(callback_ctx);
    if (server == nullptr) {
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }
    switch (cb_mode) {
        case picoquic_packet_loop_ready: {
            auto* options = static_cast<picoquic_packet_loop_options_t*>(callback_arg);
            if (options != nullptr) {
                options->do_time_check = 1;
            }
            std::lock_guard<std::mutex> lock(server->mutex);
            server->loop_ready = true;
            server->condition.notify_all();
            return 0;
        }
        case picoquic_packet_loop_port_update: {
            auto* addr = static_cast<sockaddr*>(callback_arg);
            std::lock_guard<std::mutex> lock(server->mutex);
            if (addr->sa_family == AF_INET) {
                server->port = reinterpret_cast<sockaddr_in*>(addr)->sin_port;
            } else {
                server->port = reinterpret_cast<sockaddr_in6*>(addr)->sin6_port;
            }
            server->condition.notify_all();
            return 0;
        }
        case picoquic_packet_loop_after_receive:
        case picoquic_packet_loop_after_send:
        case picoquic_packet_loop_time_check: {
            if (cb_mode == picoquic_packet_loop_time_check) {
                auto* time_check = static_cast<packet_loop_time_check_arg_t*>(callback_arg);
                if (time_check != nullptr && time_check->delta_t > 10000) {
                    time_check->delta_t = 10000;
                }
            }
            std::lock_guard<std::mutex> lock(server->mutex);
            return server->stop_requested ? PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP : 0;
        }
        default:
            return 0;
    }
}

bool start_server(DrainServer& server) {
    const std::string cert_path = std::string(kPicoquicSourceDir) + "/certs/cert.pem";
    const std::string key_path = std::string(kPicoquicSourceDir) + "/certs/key.pem";
    server.quic = picoquic_create(8, cert_path.c_str(), key_path.c_str(), nullptr, "moq-00",
                                  drain_server_callback, &server, nullptr, nullptr, nullptr,
                                  picoquic_current_time(), nullptr, nullptr, nullptr, 0);
    if (server.quic == nullptr) {
        return false;
    }
    picoquic_set_cookie_mode(server.quic, 2);
    server.thread = std::thread([&server] {
        const int ret = picoquic_packet_loop(server.quic, 0, AF_INET, 0, 0, 1, drain_server_loop_callback, &server);
        static_cast<void>(ret);
        std::lock_guard<std::mutex> lock(server.mutex);
        server.loop_exited = true;
        server.condition.notify_all();
    });
    std::unique_lock<std::mutex> lock(server.mutex);
    const bool started = server.condition.wait_for(lock, std::chrono::seconds(5), [&] {
        return (server.loop_ready && server.port != 0) || server.loop_exited;
    });
    return started && server.loop_ready && server.port != 0 && !server.loop_exited;
}

void stop_server(DrainServer& server) {
    {
        std::lock_guard<std::mutex> lock(server.mutex);
        server.stop_requested = true;
    }
    if (server.port != 0) {
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd >= 0) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(server.port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            const std::uint8_t nudge = 0;
            for (int attempt = 0; attempt < 20; ++attempt) {
                (void)sendto(fd, &nudge, sizeof(nudge), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
                std::unique_lock<std::mutex> lock(server.mutex);
                if (server.condition.wait_for(lock, std::chrono::milliseconds(100),
                                              [&] { return server.loop_exited; })) {
                    break;
                }
            }
            close(fd);
        }
    }
    if (server.thread.joinable()) {
        server.thread.join();
    }
    if (server.quic != nullptr) {
        picoquic_free(server.quic);
        server.quic = nullptr;
    }
}

}  // namespace

// Second scenario: the peer vanishes after the write, so nothing is ever
// acknowledged. close() must give up after the drain bound instead of
// hanging. The bound is the negotiated MoQT delivery timeout, which the
// session normally reports via note_delivery_timeout(); here it is set
// directly to keep the test fast.
bool run_timeout_scenario(const TlsConfig& tls) {
    bool ok = true;
    DrainServer server;
    ok &= expect(start_server(server), "expected timeout-scenario server to start");
    if (!ok) {
        stop_server(server);
        return false;
    }
    const EndpointConfig endpoint{
        .host = "127.0.0.1",
        .port = server.port,
        .alpn = "moq-00",
    };
    PicoquicClient client;
    ok &= expect(client.configure(endpoint, tls).ok, "expected timeout-scenario configure to succeed");
    ok &= expect(client.connect().ok, "expected timeout-scenario connect to succeed");
    client.note_delivery_timeout(std::chrono::milliseconds(500));
    if (!ok) {
        stop_server(server);
        return false;
    }

    std::uint64_t stream_id = 0;
    ok &= expect(client.open_stream(StreamDirection::kUnidirectional, stream_id).ok, "expected open_stream to succeed");
    const std::vector<std::uint8_t> payload(kPayloadBytes, 0xCD);
    ok &= expect(client.write_stream(stream_id, payload, true).ok, "expected write_stream with FIN to succeed");

    // Kill the peer so the queued data can never be acknowledged.
    stop_server(server);

    const auto started = std::chrono::steady_clock::now();
    const auto close_status = client.close(0);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    std::cerr << "timeout-scenario close completed in " << elapsed_ms << " ms\n";
    ok &= expect(close_status.ok, "expected close() to succeed after the drain timeout");
    ok &= expect(elapsed >= std::chrono::milliseconds(400),
                 "expected close() to hold for the drain timeout while data is unacknowledged");
    ok &= expect(elapsed < std::chrono::seconds(8), "expected close() to return in bounded time after the peer vanished");
    return ok;
}

int main() {
    bool ok = true;

    DrainServer server;
    ok &= expect(start_server(server), "expected local picoquic server to start");
    if (!ok) {
        stop_server(server);
        return 1;
    }

    const EndpointConfig endpoint{
        .host = "127.0.0.1",
        .port = server.port,
        .alpn = "moq-00",
    };
    const TlsConfig tls{
        .certificate_path = {},
        .private_key_path = {},
        .ca_path = {},
        .insecure_skip_verify = true,
    };

    PicoquicClient client;
    ok &= expect(client.configure(endpoint, tls).ok, "expected configure to succeed");
    const auto connect_status = client.connect();
    if (!connect_status.ok) {
        std::cerr << "connect error: " << connect_status.message << '\n';
    }
    ok &= expect(connect_status.ok, "expected connect to succeed");
    if (!ok) {
        stop_server(server);
        return 1;
    }

    std::uint64_t stream_id = 0;
    ok &= expect(client.open_stream(StreamDirection::kUnidirectional, stream_id).ok, "expected open_stream to succeed");
    const std::vector<std::uint8_t> payload(kPayloadBytes, 0xAB);
    ok &= expect(client.write_stream(stream_id, payload, true).ok, "expected write_stream with FIN to succeed");

    // Close immediately behind the write: the data is still queued locally or
    // in flight. close() must drain it before CONNECTION_CLOSE goes out.
    const auto started = std::chrono::steady_clock::now();
    const auto close_status = client.close(0);
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::cerr << "close completed in " << elapsed_ms << " ms\n";
    ok &= expect(close_status.ok, "expected close() to succeed");
    ok &= expect(client.state() == ConnectionState::kClosed, "expected closed state after close");

    {
        std::unique_lock<std::mutex> lock(server.mutex);
        server.condition.wait_for(lock, std::chrono::seconds(5), [&] { return server.connection_closed; });
        std::cerr << "server received " << server.bytes_received << " of " << kPayloadBytes
                  << " bytes, fin=" << (server.fin_received ? "yes" : "no") << '\n';
        ok &= expect(server.bytes_received == kPayloadBytes,
                     "expected the server to receive every byte written before close()");
        ok &= expect(server.fin_received, "expected the server to receive the stream FIN before close()");
    }

    stop_server(server);

    ok &= run_timeout_scenario(tls);
    return ok ? 0 : 1;
}
