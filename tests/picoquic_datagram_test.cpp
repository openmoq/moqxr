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
#include <picoquic_internal.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef OPENMOQ_TEST_WRAP_TRANSPORT_PARAMETERS
// picoquic automatically offers DATAGRAM on servers when the client offers it.
// Suppress that convenience only for the negative peer fixture, so the client
// actually receives a handshake with no DATAGRAM transport parameter.
extern "C" int __real_picoquic_prepare_transport_extensions(
    picoquic_cnx_t*, int, std::uint8_t*, std::size_t, std::size_t*);
extern "C" int __wrap_picoquic_prepare_transport_extensions(
    picoquic_cnx_t* cnx, int mode, std::uint8_t* bytes, std::size_t size, std::size_t* consumed) {
    const auto offered = cnx->remote_parameters.max_datagram_frame_size;
    if (mode == 1 && cnx->local_parameters.max_datagram_frame_size == 0) {
        cnx->remote_parameters.max_datagram_frame_size = 0;
    }
    const int result = __real_picoquic_prepare_transport_extensions(cnx, mode, bytes, size, consumed);
    cnx->remote_parameters.max_datagram_frame_size = offered;
    return result;
}
#endif

namespace {

using openmoq::publisher::transport::EndpointConfig;
using openmoq::publisher::transport::PicoquicClient;
using openmoq::publisher::transport::StreamDirection;
using openmoq::publisher::transport::TlsConfig;

constexpr const char* kPicoquicSourceDir = OPENMOQ_PICOQUIC_SOURCE_DIR;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

struct DatagramServer {
    picoquic_quic_t* quic = nullptr;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable condition;
    uint16_t port = 0;
    std::string alpn = "moqt-21";
    bool advertise = true;
    bool peer_advertised = false;
    bool datagram_acked = false;
    std::uint64_t close_error = 0;
    std::vector<std::uint8_t> datagram;
    bool connection_closed = false;
    bool loop_ready = false;
    bool stop_requested = false;
    bool loop_exited = false;
};

int datagram_server_callback(picoquic_cnx_t* cnx,
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
    auto* server = static_cast<DatagramServer*>(callback_ctx);
    if (server == nullptr) {
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }
    switch (event) {
        case picoquic_callback_ready: {
            std::lock_guard<std::mutex> lock(server->mutex);
            const auto* tp = picoquic_get_transport_parameters(cnx, 0);
            server->peer_advertised = tp && tp->max_datagram_frame_size > 0;
            server->condition.notify_all();
            return 0;
        }
        case picoquic_callback_stream_fin:
            // Sending only after the client writes avoids racing connect().
            return picoquic_queue_datagram_frame(cnx, server->datagram.size(), server->datagram.data());
        case picoquic_callback_datagram_acked: {
            std::lock_guard<std::mutex> lock(server->mutex);
            server->datagram_acked = true;
            server->condition.notify_all();
            return 0;
        }
        case picoquic_callback_close:
        case picoquic_callback_application_close:
        case picoquic_callback_stateless_reset: {
            std::lock_guard<std::mutex> lock(server->mutex);
            server->connection_closed = true;
            std::uint64_t local = 0, remote = 0, local_app = 0;
            picoquic_get_close_reasons(cnx, &local, &remote, &local_app, &server->close_error);
            server->condition.notify_all();
            return 0;
        }
        default:
            return 0;
    }
}

int datagram_server_loop_callback(picoquic_quic_t* quic,
                               picoquic_packet_loop_cb_enum cb_mode,
                               void* callback_ctx,
                               void* callback_arg) {
    static_cast<void>(quic);
    auto* server = static_cast<DatagramServer*>(callback_ctx);
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

bool start_server(DatagramServer& server) {
    const std::string cert_path = std::string(kPicoquicSourceDir) + "/certs/cert.pem";
    const std::string key_path = std::string(kPicoquicSourceDir) + "/certs/key.pem";
    server.quic = picoquic_create(8, cert_path.c_str(), key_path.c_str(), nullptr, server.alpn.c_str(),
                                  datagram_server_callback, &server, nullptr, nullptr, nullptr,
                                  picoquic_current_time(), nullptr, nullptr, nullptr, 0);
    if (server.quic == nullptr) {
        return false;
    }
    picoquic_set_cookie_mode(server.quic, 2);
    picoquic_set_default_tp_value(server.quic, picoquic_tp_max_datagram_frame_size,
                                  server.advertise ? PICOQUIC_MAX_PACKET_SIZE : 0);
    server.thread = std::thread([&server] {
        const int ret = picoquic_packet_loop(server.quic, 0, AF_INET, 0, 0, 1, datagram_server_loop_callback, &server);
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

void stop_server(DatagramServer& server) {
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
        // The loop thread is gone; disarm the cross-thread check (no-op
        // unless built with PICOQUIC_WITH_THREAD_CHECK) before freeing.
        PICOQUIC_THREAD_DISABLE_CHECK(server.quic);
        picoquic_free(server.quic);
        server.quic = nullptr;
    }
}

bool scenario(std::vector<std::uint8_t> datagram, bool valid, bool advertise = true,
              bool custom_alpn = false) {
    DatagramServer server;
    server.datagram = std::move(datagram);
    server.advertise = advertise;
    server.alpn = custom_alpn ? "moq-00" : "moqt-21";
    if (!expect(start_server(server), "server started")) {
        stop_server(server);
        return false;
    }
    PicoquicClient client;
    bool ok = client.configure(EndpointConfig{.host = "127.0.0.1", .port = server.port,
                                               .alpn = server.alpn,
                                               .application_protocol = custom_alpn ? "moqt-21" : ""},
                               TlsConfig{.insecure_skip_verify = true}).ok;
    const auto status = client.connect();
    if (!advertise) {
        ok &= expect(!status.ok && status.message.find("DATAGRAM") != std::string::npos,
                     "peer without DATAGRAM rejected with diagnostic: " + status.message);
    } else {
        ok &= expect(status.ok, "DATAGRAM peer connected: " + status.message);
        if (status.ok) {
            std::uint64_t stream = 0;
            ok &= client.open_stream(StreamDirection::kBidirectional, stream).ok;
            ok &= client.write_stream(stream, std::vector<std::uint8_t>{0}, true).ok;
            std::unique_lock<std::mutex> lock(server.mutex);
            server.condition.wait_for(lock, std::chrono::seconds(3), [&] {
                return server.connection_closed || (valid && server.datagram_acked);
            });
            ok &= expect(server.peer_advertised, "client advertised DATAGRAM on wire");
            if (valid) {
                ok &= expect(server.datagram_acked && !server.connection_closed,
                             "valid unknown-alias object or padding discarded without closing");
            } else {
                ok &= expect(server.connection_closed && server.close_error == 0x3,
                             "invalid datagram closed with PROTOCOL_VIOLATION");
            }
        }
    }
    client.close(0);
    stop_server(server);
    return ok;
}
}  // namespace

int main() {
    bool ok = true;
#ifdef OPENMOQ_TEST_WRAP_TRANSPORT_PARAMETERS
    ok &= scenario({}, false, false);
#else
    std::cerr << "SKIP: missing-DATAGRAM peer fixture requires Linux with static picoquic\n";
#endif
    ok &= scenario({0x0c, 1, 2, 0x42}, true); // unknown alias; zero ID/default priority
    ok &= scenario({0xf0, 0x13, 0x2b, 0x3e, 0x29, 0, 0}, true); // vi64 padding
    ok &= scenario({0xf0, 0x13, 0x2b, 0x3e, 0x29}, true, true, true); // ALPN override retains draft 21
    ok &= scenario({0x10}, false); // reserved type flag
    ok &= scenario({0x22}, false); // STATUS + END_OF_GROUP
    ok &= scenario({0x40}, false); // unknown type
    ok &= scenario({}, false);
    ok &= scenario({0x0c, 0x80}, false); // truncated alias
    ok &= scenario({0x0d, 1, 2, 0}, false); // zero properties length
    ok &= scenario({0x2d, 1, 2, 2, 2, 0, 3}, false); // properties with non-Normal status
    return ok ? 0 : 1;
}
