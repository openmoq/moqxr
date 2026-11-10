// A client that sets EndpointConfig::idle_timeout_ms notices a relay that
// vanished (killed, rebooted) within that timeout instead of the picoquic
// default of 30 s, and its keepalive still holds a quiet connection open.
#include "openmoq/publisher/transport/picoquic_client.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <picoquic.h>
#include <picoquic_packet_loop.h>
#include <picoquic_internal.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using openmoq::publisher::transport::ConnectionState;
using openmoq::publisher::transport::EndpointConfig;
using openmoq::publisher::transport::PicoquicClient;
using openmoq::publisher::transport::TlsConfig;

constexpr const char* kPicoquicSourceDir = OPENMOQ_PICOQUIC_SOURCE_DIR;
constexpr std::uint32_t kIdleTimeoutMs = 2000;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

// A server that accepts the connection and then only answers at the QUIC
// level. It listens on ::1: some hosts drop large IPv4 loopback datagrams.
struct QuietServer {
    picoquic_quic_t* quic = nullptr;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable condition;
    uint16_t port = 0;
    bool loop_ready = false;
    bool stop_requested = false;
    bool loop_exited = false;
};

int quiet_server_callback(picoquic_cnx_t*, uint64_t, uint8_t*, size_t, picoquic_call_back_event_t, void*, void*) {
    return 0;
}

int quiet_server_loop_callback(picoquic_quic_t*, picoquic_packet_loop_cb_enum cb_mode, void* callback_ctx,
                               void* callback_arg) {
    auto* server = static_cast<QuietServer*>(callback_ctx);
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
            server->port = addr->sa_family == AF_INET6 ? reinterpret_cast<sockaddr_in6*>(addr)->sin6_port
                                                       : reinterpret_cast<sockaddr_in*>(addr)->sin_port;
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

bool start_server(QuietServer& server) {
    const std::string cert_path = std::string(kPicoquicSourceDir) + "/certs/cert.pem";
    const std::string key_path = std::string(kPicoquicSourceDir) + "/certs/key.pem";
    server.quic = picoquic_create(8, cert_path.c_str(), key_path.c_str(), nullptr, "moq-00", quiet_server_callback,
                                  &server, nullptr, nullptr, nullptr, picoquic_current_time(), nullptr, nullptr,
                                  nullptr, 0);
    if (server.quic == nullptr) {
        return false;
    }
    picoquic_set_cookie_mode(server.quic, 2);
    picoquic_set_default_tp_value(server.quic, picoquic_tp_max_datagram_frame_size, PICOQUIC_MAX_PACKET_SIZE);
    server.thread = std::thread([&server] {
        picoquic_packet_loop(server.quic, 0, AF_INET6, 0, 0, 1, quiet_server_loop_callback, &server);
        std::lock_guard<std::mutex> lock(server.mutex);
        server.loop_exited = true;
        server.condition.notify_all();
    });
    std::unique_lock<std::mutex> lock(server.mutex);
    server.condition.wait_for(lock, std::chrono::seconds(5),
                              [&] { return (server.loop_ready && server.port != 0) || server.loop_exited; });
    return server.loop_ready && server.port != 0 && !server.loop_exited;
}

// Ends the server loop without a CONNECTION_CLOSE, as a killed relay does.
void stop_server(QuietServer& server) {
    {
        std::lock_guard<std::mutex> lock(server.mutex);
        server.stop_requested = true;
    }
    const int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd >= 0) {
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(server.port);
        addr.sin6_addr = in6addr_loopback;
        const std::uint8_t nudge = 0;
        for (int attempt = 0; attempt < 20; ++attempt) {
            (void)sendto(fd, &nudge, sizeof(nudge), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            std::unique_lock<std::mutex> lock(server.mutex);
            if (server.condition.wait_for(lock, std::chrono::milliseconds(100), [&] { return server.loop_exited; })) {
                break;
            }
        }
        close(fd);
    }
    if (server.thread.joinable()) {
        server.thread.join();
    }
    if (server.quic != nullptr) {
        PICOQUIC_THREAD_DISABLE_CHECK(server.quic);
        picoquic_free(server.quic);
        server.quic = nullptr;
    }
}

bool connectClient(PicoquicClient& client, const QuietServer& server) {
    EndpointConfig endpoint{
        .host = "::1",
        .port = server.port,
        .alpn = "moq-00",
    };
    endpoint.idle_timeout_ms = kIdleTimeoutMs;
    const TlsConfig tls{.insecure_skip_verify = true};
    return client.configure(endpoint, tls).ok && client.connect().ok;
}

// The keepalive follows the idle timeout: a connection with nothing to send
// stays open for several idle timeouts.
bool quietConnectionStaysOpen() {
    QuietServer server;
    bool ok = expect(start_server(server), "expected the server to start");
    PicoquicClient client;
    ok = ok && expect(connectClient(client, server), "expected the client to connect");
    if (ok) {
        std::this_thread::sleep_for(std::chrono::milliseconds(3 * kIdleTimeoutMs));
        ok &= expect(client.state() == ConnectionState::kConnected,
                     "expected a quiet connection to stay open with the keepalive");
        static_cast<void>(client.close(0));
    }
    stop_server(server);
    return ok;
}

// A server that vanishes is noticed within about the idle timeout.
bool vanishedServerIsNoticed() {
    QuietServer server;
    bool ok = expect(start_server(server), "expected the server to start");
    PicoquicClient client;
    ok = ok && expect(connectClient(client, server), "expected the client to connect");
    stop_server(server);
    if (!ok) {
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(30);
    while (client.state() == ConnectionState::kConnected && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    std::cerr << "vanished server noticed after "
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms\n";
    ok &= expect(client.state() != ConnectionState::kConnected, "expected the client to notice the vanished server");
    ok &= expect(elapsed < std::chrono::milliseconds(2 * kIdleTimeoutMs),
                 "expected the vanished server to be noticed within twice the idle timeout");
    static_cast<void>(client.close(0));
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= quietConnectionStaysOpen();
    ok &= vanishedServerIsNoticed();
    return ok ? 0 : 1;
}
