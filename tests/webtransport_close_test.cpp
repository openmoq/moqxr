// Regression test: WebTransportClient::close() must return even when the
// peer never tears down the underlying QUIC connection.
//
// The publisher's batch path finishes by calling Publisher::disconnect(),
// which joins the WebTransport packet-loop thread. That loop only exits once
// the picoquic connection reaches the disconnected state. A relay that keeps
// the QUIC connection open after the CLOSE_WEBTRANSPORT_SESSION capsule
// therefore left the process hanging forever. This test stands up a local
// h3zero WebTransport server that accepts CONNECT and then does nothing, and
// asserts close() completes in bounded time.

#include "openmoq/publisher/transport/webtransport_client.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <picoquic.h>
#include <picoquic_packet_loop.h>
#include <picoquic_internal.h>
#include <h3zero_common.h>
#include <pico_webtransport.h>
#include <picoquic_set_textlog.h>

#include <cstdlib>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using openmoq::publisher::transport::ConnectionState;
using openmoq::publisher::transport::EndpointConfig;
using openmoq::publisher::transport::ObjectWriteDisposition;
using openmoq::publisher::transport::ObjectWriteOptions;
using openmoq::publisher::transport::StreamDirection;
using openmoq::publisher::transport::TlsConfig;
using openmoq::publisher::transport::TransportKind;
using openmoq::publisher::transport::WebTransportClient;

constexpr const char* kPicoquicSourceDir = OPENMOQ_PICOQUIC_SOURCE_DIR;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

struct SilentServer {
    picoquic_quic_t* quic = nullptr;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable condition;
    uint16_t port = 0;
    bool loop_ready = false;
    bool stop_requested = false;
    bool loop_exited = false;
    bool connect_accepted = false;
    std::size_t stream_bytes_received = 0;
    std::size_t request_stream_bytes_received = 0;
    bool stream_fin_received = false;
    std::set<std::uint64_t> stream_fins;
    std::map<std::uint64_t, std::uint64_t> stream_reset_errors;
    std::set<std::uint64_t> stop_sending_stream_ids;
    std::set<std::uint64_t> stop_sending_sent_streams;
    picohttp_server_path_item_t path_item{};
    picohttp_server_parameters_t params{};
};

// WebTransport path callback for the server. Accepts the CONNECT and then
// ignores everything: no session close, no QUIC close.
int silent_path_callback(picoquic_cnx_t* cnx,
                         uint8_t* bytes,
                         size_t length,
                         picohttp_call_back_event_t event,
                         h3zero_stream_ctx_t* stream_ctx,
                         void* path_app_ctx) {
    static_cast<void>(bytes);
    static_cast<void>(length);
    auto* server = static_cast<SilentServer*>(path_app_ctx);
    if (server == nullptr) {
        return -1;
    }
    if (event == picohttp_callback_connect) {
        auto* h3_ctx = static_cast<h3zero_callback_ctx_t*>(picoquic_get_callback_context(cnx));
        if (h3_ctx == nullptr || stream_ctx == nullptr) {
            return -1;
        }
        stream_ctx->ps.stream_state.control_stream_id = stream_ctx->stream_id;
        if (h3zero_declare_stream_prefix(h3_ctx, stream_ctx->stream_id, silent_path_callback, server) != 0) {
            return -1;
        }
        stream_ctx->path_callback = silent_path_callback;
        stream_ctx->path_callback_ctx = server;
        std::lock_guard<std::mutex> lock(server->mutex);
        server->connect_accepted = true;
        server->condition.notify_all();
    } else if (event == picohttp_callback_post_data || event == picohttp_callback_post_fin) {
        // Count application stream bytes so the drain scenario can verify
        // everything written before close() actually arrived.
        std::lock_guard<std::mutex> lock(server->mutex);
        server->stream_bytes_received += length;
        if (stream_ctx != nullptr && length != 0 &&
            server->stop_sending_stream_ids.contains(stream_ctx->stream_id) &&
            !server->stop_sending_sent_streams.contains(stream_ctx->stream_id)) {
            server->stop_sending_sent_streams.insert(stream_ctx->stream_id);
            if (picoquic_stop_sending(cnx, stream_ctx->stream_id, 0x01) != 0) {
                return -1;
            }
        }
        if (stream_ctx != nullptr && stream_ctx->stream_id != 0 &&
            (stream_ctx->stream_id & 0x2) == 0) {
            server->request_stream_bytes_received += length;
        }
        if (event == picohttp_callback_post_fin) {
            server->stream_fin_received = true;
            if (stream_ctx != nullptr) {
                server->stream_fins.insert(stream_ctx->stream_id);
            }
        }
        server->condition.notify_all();
    } else if (event == picohttp_callback_reset && stream_ctx != nullptr) {
        std::lock_guard<std::mutex> lock(server->mutex);
        server->stream_reset_errors.insert_or_assign(
            stream_ctx->stream_id,
            picoquic_get_remote_stream_error(cnx, stream_ctx->stream_id));
        server->condition.notify_all();
    }
    return 0;
}

int silent_server_loop_callback(picoquic_quic_t* quic,
                                picoquic_packet_loop_cb_enum cb_mode,
                                void* callback_ctx,
                                void* callback_arg) {
    static_cast<void>(quic);
    auto* server = static_cast<SilentServer*>(callback_ctx);
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
            // picoquic_store_loopback_addr writes the port in host order.
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

bool start_server(SilentServer& server, bool hold_unidirectional_data = false) {
    const std::string cert_path = std::string(kPicoquicSourceDir) + "/certs/cert.pem";
    const std::string key_path = std::string(kPicoquicSourceDir) + "/certs/key.pem";

    server.path_item.path = "/moq";
    server.path_item.path_length = 4;
    server.path_item.path_callback = silent_path_callback;
    server.path_item.path_app_ctx = &server;
    server.params.path_table = &server.path_item;
    server.params.path_table_nb = 1;

    server.quic = picoquic_create(8, cert_path.c_str(), key_path.c_str(), nullptr, "h3",
                                  h3zero_callback, &server.params, nullptr, nullptr, nullptr,
                                  picoquic_current_time(), nullptr, nullptr, nullptr, 0);
    if (server.quic == nullptr) {
        return false;
    }
    picoquic_set_cookie_mode(server.quic, 2);
    // WebTransport clients (picowt) require the peer to advertise datagram
    // support and reset_stream_at before they will send the CONNECT.
    picoquic_tp_t tp = *picoquic_get_default_tp(server.quic);
    tp.max_datagram_frame_size = PICOQUIC_MAX_PACKET_SIZE;
    tp.is_reset_stream_at_enabled = 1;
    if (hold_unidirectional_data) {
        tp.initial_max_stream_data_uni = 0;
    }
    picoquic_set_default_tp(server.quic, &tp);
    if (std::getenv("OPENMOQ_PICOQUIC_TRACE") != nullptr) {
        picoquic_set_textlog(server.quic, "-");
    }

    server.thread = std::thread([&server] {
        const int ret =
            picoquic_packet_loop(server.quic, 0, AF_INET, 0, 0, 1, silent_server_loop_callback, &server);
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

void stop_server(SilentServer& server) {
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

}  // namespace

int main() {
    bool ok = true;

    SilentServer server;
    ok &= expect(start_server(server), "expected local webtransport server to start");
    if (!ok) {
        stop_server(server);
        return 1;
    }

    const EndpointConfig endpoint{
        .transport = TransportKind::kWebTransport,
        .host = "127.0.0.1",
        .port = server.port,
        .alpn = "h3",
        .application_protocol = "moq-00",
        .sni = {},
        .path = "/moq",
        .path_explicit = true,
    };
    const TlsConfig tls{
        .certificate_path = {},
        .private_key_path = {},
        .ca_path = {},
        .insecure_skip_verify = true,
    };

    auto* client = new WebTransportClient();
    ok &= expect(client->configure(endpoint, tls).ok, "expected configure to succeed");
    const auto connect_status = client->connect();
    if (!connect_status.ok) {
        std::cerr << "connect error: " << connect_status.message << '\n';
    }
    ok &= expect(connect_status.ok, "expected webtransport CONNECT to a silent local server to succeed");
    ok &= expect(client->state() == ConnectionState::kConnected, "expected connected state after connect");

    std::uint64_t priority_control_stream_id = 0;
    std::uint64_t priority_request_stream_id = 0;
    ok &= expect(client->open_stream(StreamDirection::kUnidirectional,
                                     priority_control_stream_id).ok,
                 "expected WebTransport priority control stream open to succeed");
    ok &= expect(client->open_stream(StreamDirection::kBidirectional,
                                     priority_request_stream_id).ok,
                 "expected WebTransport priority request stream open to succeed");
    ok &= expect(client->set_reliable_stream_priority(priority_control_stream_id, 0).ok &&
                     client->set_reliable_stream_priority(priority_request_stream_id, 1).ok,
                 "expected WebTransport backend to accept explicit reliable priorities");
    const auto priority_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < priority_deadline &&
           (client->applied_reliable_stream_priority_for_testing(priority_control_stream_id) !=
                std::optional<std::uint8_t>{0} ||
            client->applied_reliable_stream_priority_for_testing(priority_request_stream_id) !=
                std::optional<std::uint8_t>{1})) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ok &= expect(
        client->applied_reliable_stream_priority_for_testing(priority_control_stream_id) ==
                std::optional<std::uint8_t>{0} &&
            client->applied_reliable_stream_priority_for_testing(priority_request_stream_id) ==
                std::optional<std::uint8_t>{1},
        "expected WebTransport packet loop to apply explicit classes 0 and 1");
    constexpr std::size_t kReliablePriorityHistoryCapacity = 256;
    constexpr std::size_t kReliablePriorityChurnCount = 272;
    const auto reliable_priority_churn_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (std::size_t index = 0; index < kReliablePriorityChurnCount; ++index) {
        const std::uint8_t priority =
            static_cast<std::uint8_t>(index + 2);
        ok &= expect(client->set_reliable_stream_priority(
                         priority_request_stream_id, priority).ok,
                     "expected WebTransport reliable-priority churn update to queue");
        while (std::chrono::steady_clock::now() <
                   reliable_priority_churn_deadline &&
               client->applied_reliable_stream_priority_for_testing(
                   priority_request_stream_id) !=
                   std::optional<std::uint8_t>{priority}) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    const auto reliable_priority_history =
        client->applied_reliable_stream_priorities_for_testing(
            priority_request_stream_id);
    ok &= expect(
        reliable_priority_history.size() ==
                kReliablePriorityHistoryCapacity &&
            reliable_priority_history.front() ==
                static_cast<std::uint8_t>(
                    kReliablePriorityChurnCount -
                    kReliablePriorityHistoryCapacity + 2) &&
            reliable_priority_history.back() ==
                static_cast<std::uint8_t>(
                    kReliablePriorityChurnCount - 1 + 2),
        "expected WebTransport reliable-priority history to retain only the latest 256 FIFO values");

    {
        std::unique_lock<std::mutex> lock(server.mutex);
        ok &= expect(server.condition.wait_for(lock, std::chrono::seconds(5), [&] { return server.connect_accepted; }),
                     "expected server to observe the CONNECT");
    }

    // The peer never closes QUIC. close() must still complete in bounded time.
    const auto started = std::chrono::steady_clock::now();
    auto close_future = std::async(std::launch::async, [client] { return client->close(0); });
    const bool completed = close_future.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    ok &= expect(completed, "expected close() to return within 10s when the peer keeps the QUIC connection open");
    if (completed) {
        const auto close_status = close_future.get();
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        std::cerr << "close completed in " << elapsed_ms << " ms\n";
        ok &= expect(close_status.ok, "expected close() to succeed");
        ok &= expect(client->state() == ConnectionState::kClosed, "expected closed state after close");
        delete client;
    } else {
        // Leak the client on purpose: its destructor would block forever too.
        std::cerr << "close() is hung; leaking client and exiting\n";
        stop_server(server);
        std::_Exit(1);
    }

    // Drain scenario: data written with FIN right before close() must reach
    // the peer before the QUIC connection is closed (MoQT wind-down: stream
    // FIN -> PUBLISH_DONE -> let delivery complete, -16 9.15 / -18 10.11).
    {
        constexpr std::size_t kPayloadBytes = 512 * 1024;
        auto* drain_client = new WebTransportClient();
        ok &= expect(drain_client->configure(endpoint, tls).ok, "expected drain client configure to succeed");
        const auto drain_connect = drain_client->connect();
        if (!drain_connect.ok) {
            std::cerr << "drain connect error: " << drain_connect.message << '\n';
        }
        ok &= expect(drain_connect.ok, "expected drain client CONNECT to succeed");
        if (!ok) {
            stop_server(server);
            return 1;
        }

        std::uint64_t ack_stream_id = 0;
        ok &= expect(drain_client->open_stream(StreamDirection::kUnidirectional,
                                               ack_stream_id).ok,
                     "expected ACK-wins webtransport stream open to succeed");
        ObjectWriteOptions first_priority_options;
        first_priority_options.transport_priority = 220;
        const std::vector<std::uint8_t> first_priority_payload = {0x79};
        const auto first_priority_result = drain_client->try_write_object(
            ack_stream_id, first_priority_payload, false,
            first_priority_options);
        ObjectWriteOptions ack_wins_options;
        ack_wins_options.transport_priority = 7;
        ack_wins_options.subgroup_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        const std::vector<std::uint8_t> ack_payload = {0x7A};
        const auto ack_result = drain_client->try_write_object(
            ack_stream_id, ack_payload, true, ack_wins_options);
        ok &= expect(first_priority_result.disposition ==
                             ObjectWriteDisposition::kAccepted &&
                         ack_result.disposition ==
                             ObjectWriteDisposition::kAccepted,
                     "expected both same-stream webtransport priority objects to be admitted");
        {
            std::unique_lock<std::mutex> lock(server.mutex);
            ok &= expect(server.condition.wait_for(lock, std::chrono::seconds(3), [&] {
                             return server.stream_fins.contains(ack_stream_id);
                         }),
                         "expected the webtransport peer to receive the ACK-wins FIN");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        const auto media_priorities =
            drain_client->applied_media_stream_priorities_for_testing(
                ack_stream_id);
        ok &= expect(media_priorities.size() >= 2 &&
                         media_priorities.front() == 220 &&
                         media_priorities.back() == 7,
                     "expected the WebTransport callback to apply the next queued object's current priority");
        ok &= expect(!drain_client->media_stream_expired(ack_stream_id),
                     "expected h3zero stream free after ACK to cancel the subgroup timer");
        std::size_t bytes_before_drain = 0;
        {
            std::lock_guard<std::mutex> lock(server.mutex);
            ok &= expect(!server.stream_reset_errors.contains(ack_stream_id),
                         "expected no webtransport timeout reset after the acknowledged deadline-bearing FIN");
            bytes_before_drain = server.stream_bytes_received;
        }

        std::uint64_t peer_stopped_stream_id = 0;
        ok &= expect(drain_client->open_stream(StreamDirection::kUnidirectional,
                                               peer_stopped_stream_id).ok,
                     "expected peer-stop WebTransport stream open to succeed");
        std::size_t bytes_before_stop = 0;
        {
            std::lock_guard<std::mutex> lock(server.mutex);
            bytes_before_stop = server.stream_bytes_received;
            server.stream_reset_errors.erase(peer_stopped_stream_id);
            server.stop_sending_stream_ids.insert(peer_stopped_stream_id);
            server.stop_sending_sent_streams.erase(peer_stopped_stream_id);
        }
        const std::vector<std::uint8_t> peer_stopped_first(
            2 * 1024 * 1024, 0x61);
        const std::vector<std::uint8_t> peer_stopped_second(
            2 * 1024 * 1024, 0x62);
        ok &= expect(drain_client->try_write_object(peer_stopped_stream_id,
                                                    peer_stopped_first,
                                                    false,
                                                    {}).disposition ==
                         ObjectWriteDisposition::kAccepted &&
                         drain_client->try_write_object(peer_stopped_stream_id,
                                                       peer_stopped_second,
                                                       true,
                                                       {}).disposition ==
                             ObjectWriteDisposition::kAccepted,
                     "expected FIN-retired WebTransport media before peer STOP_SENDING");
        {
            std::unique_lock<std::mutex> lock(server.mutex);
            ok &= expect(server.condition.wait_for(lock, std::chrono::seconds(5), [&] {
                             return server.stream_reset_errors.contains(
                                 peer_stopped_stream_id);
                         }),
                         "expected WebTransport publisher to answer STOP_SENDING with a reset");
            const auto reset =
                server.stream_reset_errors.find(peer_stopped_stream_id);
            if (reset != server.stream_reset_errors.end()) {
                ok &= expect(reset->second == 0x01,
                             "expected WebTransport peer-stop reset error CANCELLED 0x01");
            }
            ok &= expect(server.stream_bytes_received - bytes_before_stop <
                             peer_stopped_first.size() +
                                 peer_stopped_second.size(),
                         "expected WebTransport STOP_SENDING to discard queued media");
        }
        ok &= expect(drain_client->media_stream_peer_stopped(
                         peer_stopped_stream_id),
                     "expected WebTransport peer-stop state to remain queryable");
        const auto peer_stop_snapshot =
            drain_client->media_stream_peer_stop_events();
        ok &= expect(std::find(peer_stop_snapshot.begin(),
                              peer_stop_snapshot.end(),
                              peer_stopped_stream_id) !=
                         peer_stop_snapshot.end(),
                     "expected WebTransport snapshot to retain STOP_SENDING received after FIN admission");
        const std::vector<std::uint8_t> after_stop_payload = {0x63};
        const auto after_peer_stop = drain_client->try_write_object(
            peer_stopped_stream_id, after_stop_payload, true, {});
        ok &= expect(after_peer_stop.disposition ==
                             ObjectWriteDisposition::kFailed &&
                         after_peer_stop.message.find("stopped by peer") !=
                             std::string::npos,
                     "expected WebTransport to reject later admission on a peer-stopped stream");
        drain_client->acknowledge_media_stream_peer_stopped(
            peer_stopped_stream_id);
        ok &= expect(!drain_client->media_stream_peer_stopped(
                             peer_stopped_stream_id) &&
                         drain_client->peer_stopped_media_stream_count_for_testing() == 0,
                     "expected WebTransport acknowledgement to release retained peer-stop state");

        constexpr std::size_t kPeerStopStressCount = 24;
        std::vector<std::uint64_t> peer_stop_stress_streams;
        peer_stop_stress_streams.reserve(kPeerStopStressCount);
        for (std::size_t index = 0; index < kPeerStopStressCount; ++index) {
            std::uint64_t stress_stream_id = 0;
            ok &= expect(drain_client->open_stream(
                             StreamDirection::kUnidirectional,
                             stress_stream_id).ok,
                         "expected WebTransport stress peer-stop stream open to succeed");
            peer_stop_stress_streams.push_back(stress_stream_id);
        }
        {
            std::lock_guard<std::mutex> lock(server.mutex);
            server.stop_sending_stream_ids.insert(
                peer_stop_stress_streams.begin(),
                peer_stop_stress_streams.end());
        }
        std::size_t acknowledged_peer_stops = 0;
        std::thread peer_stop_consumer([&] {
            std::vector<bool> consumed(peer_stop_stress_streams.size(), false);
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (acknowledged_peer_stops < peer_stop_stress_streams.size() &&
                   std::chrono::steady_clock::now() < deadline) {
                for (const std::uint64_t stopped_stream_id :
                     drain_client->media_stream_peer_stop_events()) {
                    const auto stopped_it = std::find(
                        peer_stop_stress_streams.begin(),
                        peer_stop_stress_streams.end(),
                        stopped_stream_id);
                    if (stopped_it != peer_stop_stress_streams.end()) {
                        const std::size_t index = static_cast<std::size_t>(
                            std::distance(peer_stop_stress_streams.begin(),
                                          stopped_it));
                        if (!consumed[index]) {
                            drain_client
                                ->acknowledge_media_stream_peer_stopped(
                                    stopped_stream_id);
                            consumed[index] = true;
                            ++acknowledged_peer_stops;
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        for (std::size_t index = 0;
             index < peer_stop_stress_streams.size(); ++index) {
            const std::array<std::uint8_t, 1> payload = {
                static_cast<std::uint8_t>(index)};
            ok &= expect(drain_client->try_write_object(
                             peer_stop_stress_streams[index], payload, false, {})
                             .disposition == ObjectWriteDisposition::kAccepted,
                         "expected WebTransport stress peer-stop object admission");
        }
        peer_stop_consumer.join();
        ok &= expect(acknowledged_peer_stops == peer_stop_stress_streams.size() &&
                         drain_client->peer_stopped_media_stream_count_for_testing() == 0,
                     "expected concurrent WebTransport consumption to bound retained peer-stop state");
        constexpr std::size_t kExpiryStressCount = 24;
        for (std::size_t index = 0; index < kExpiryStressCount; ++index) {
            std::uint64_t stress_stream_id = 0;
            ok &= expect(drain_client->open_stream(
                             StreamDirection::kUnidirectional,
                             stress_stream_id).ok,
                         "expected WebTransport stress-expiry stream open to succeed");
            ObjectWriteOptions stress_options;
            stress_options.object_deadline =
                std::chrono::steady_clock::now() -
                std::chrono::milliseconds(1);
            const std::array<std::uint8_t, 1> payload = {
                static_cast<std::uint8_t>(index)};
            ok &= expect(drain_client->try_write_object(
                             stress_stream_id, payload, false,
                             stress_options).disposition ==
                             ObjectWriteDisposition::kAccepted,
                         "expected WebTransport stress-expiry object admission");
            const auto expiry_deadline =
                std::chrono::steady_clock::now() +
                std::chrono::seconds(1);
            while (std::chrono::steady_clock::now() < expiry_deadline &&
                   !drain_client->media_stream_expired(stress_stream_id)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const auto expiry_events =
                drain_client->media_stream_expiry_events();
            ok &= expect(std::find(expiry_events.begin(),
                                   expiry_events.end(),
                                   stress_stream_id) != expiry_events.end(),
                         "expected WebTransport stress expiry to remain losslessly observable");
            drain_client->acknowledge_media_stream_expired(
                stress_stream_id);
        }
        ok &= expect(
            drain_client->timed_out_media_stream_count_for_testing() == 0,
            "expected repeated WebTransport expiry acknowledgement to keep retained state bounded");
        {
            std::lock_guard<std::mutex> lock(server.mutex);
            bytes_before_drain = server.stream_bytes_received;
        }

        std::uint64_t stream_id = 0;
        ok &= expect(drain_client->open_stream(StreamDirection::kUnidirectional, stream_id).ok,
                     "expected open_stream to succeed");
        const std::vector<std::uint8_t> payload(kPayloadBytes, 0xAB);
        const auto media_result = drain_client->try_write_object(stream_id, payload, true, {});
        ok &= expect(media_result.disposition == ObjectWriteDisposition::kAccepted,
                     "expected try_write_object with FIN to succeed");

        const auto drain_started = std::chrono::steady_clock::now();
        auto drain_close = std::async(std::launch::async, [drain_client] { return drain_client->close(0); });
        const bool drain_completed = drain_close.wait_for(std::chrono::seconds(15)) == std::future_status::ready;
        ok &= expect(drain_completed, "expected close() after a large FIN write to return within 15s");
        if (!drain_completed) {
            std::cerr << "close() is hung; leaking client and exiting\n";
            stop_server(server);
            std::_Exit(1);
        }
        const auto drain_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - drain_started)
                                          .count();
        std::cerr << "drain close completed in " << drain_elapsed_ms << " ms\n";
        ok &= expect(drain_close.get().ok, "expected drain close() to succeed");
        delete drain_client;

        std::unique_lock<std::mutex> lock(server.mutex);
        server.condition.wait_for(lock, std::chrono::seconds(5), [&] {
            return server.stream_fins.contains(stream_id) &&
                   server.stream_bytes_received >= bytes_before_drain + kPayloadBytes;
        });
        std::cerr << "server received " << server.stream_bytes_received - bytes_before_drain
                  << " of " << kPayloadBytes
                  << " bytes, fin=" << (server.stream_fin_received ? "yes" : "no") << '\n';
        ok &= expect(server.stream_bytes_received == bytes_before_drain + kPayloadBytes,
                     "expected the server to receive every byte written before close()");
        ok &= expect(server.stream_fins.contains(stream_id),
                     "expected the server to receive the stream FIN before close()");
    }

    // Timer-wins uses a separate 10-byte client on the normally flowing
    // loopback connection. A one-byte probe lets h3zero establish the stream
    // context; the already-due final admission is then reset before provide
    // data, and a full-size replacement proves the queue was cleared.
    {
        WebTransportClient deadline_client(10);
        ok &= expect(deadline_client.configure(endpoint, tls).ok,
                     "expected timer-wins webtransport client configure to succeed");
        ok &= expect(deadline_client.connect().ok,
                     "expected timer-wins webtransport client connect to succeed");
        std::uint64_t timeout_stream_id = 0;
        ok &= expect(deadline_client.open_stream(StreamDirection::kUnidirectional,
                                                 timeout_stream_id).ok,
                     "expected timer-wins webtransport stream open to succeed");
        std::size_t bytes_before_timer_stream = 0;
        {
            std::lock_guard<std::mutex> lock(server.mutex);
            bytes_before_timer_stream = server.stream_bytes_received;
        }
        const std::vector<std::uint8_t> prefix_probe = {0xE1};
        ok &= expect(deadline_client.try_write_object(timeout_stream_id,
                                                      prefix_probe,
                                                      false,
                                                      {}).disposition == ObjectWriteDisposition::kAccepted,
                     "expected webtransport timer stream probe to be admitted");
        {
            std::unique_lock<std::mutex> lock(server.mutex);
            ok &= expect(server.condition.wait_for(lock, std::chrono::seconds(3), [&] {
                             return server.stream_bytes_received >=
                                    bytes_before_timer_stream + prefix_probe.size();
                         }),
                         "expected the server to establish the timer stream context");
        }
        ObjectWriteOptions timer_wins_options;
        timer_wins_options.subgroup_deadline =
            std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
        const std::vector<std::uint8_t> timed_media(10, 0xE2);
        ok &= expect(deadline_client.try_write_object(timeout_stream_id,
                                                      timed_media,
                                                      true,
                                                      timer_wins_options).disposition ==
                         ObjectWriteDisposition::kAccepted,
                     "expected already-due webtransport subgroup to enter bounded admission");
        {
            std::unique_lock<std::mutex> lock(server.mutex);
            ok &= expect(server.condition.wait_for(lock, std::chrono::seconds(3), [&] {
                             return server.stream_reset_errors.contains(timeout_stream_id);
                         }),
                         "expected the webtransport subgroup timer to reset its stream");
            const auto reset = server.stream_reset_errors.find(timeout_stream_id);
            if (reset != server.stream_reset_errors.end()) {
                ok &= expect(reset->second == 0x02,
                             "expected webtransport timer-wins reset error 0x02");
            }
        }
        ok &= expect(deadline_client.media_stream_expired(timeout_stream_id),
                     "expected webtransport timer expiry to remain queryable by stream ID");
        const auto timeout_snapshot =
            deadline_client.media_stream_expiry_events();
        ok &= expect(std::find(timeout_snapshot.begin(),
                               timeout_snapshot.end(),
                               timeout_stream_id) != timeout_snapshot.end(),
                     "expected WebTransport timer expiry in the stable event snapshot");
        deadline_client.acknowledge_media_stream_expired(timeout_stream_id);
        ok &= expect(!deadline_client.media_stream_expired(timeout_stream_id) &&
                         deadline_client.timed_out_media_stream_count_for_testing() == 0,
                     "expected WebTransport expiry acknowledgement to release retained stream state");
        std::uint64_t replacement_stream_id = 0;
        ok &= expect(deadline_client.open_stream(StreamDirection::kUnidirectional,
                                                 replacement_stream_id).ok,
                     "expected replacement webtransport timer stream open to succeed");
        const std::vector<std::uint8_t> exact_budget(10, 0xE3);
        std::size_t bytes_before_replacement = 0;
        {
            std::lock_guard<std::mutex> lock(server.mutex);
            bytes_before_replacement = server.stream_bytes_received;
        }
        ok &= expect(deadline_client.try_write_object(replacement_stream_id,
                                                      exact_budget,
                                                      true,
                                                      {}).disposition == ObjectWriteDisposition::kAccepted,
                     "expected webtransport timeout reset to release the full admission budget");
        {
            std::unique_lock<std::mutex> lock(server.mutex);
            ok &= expect(server.condition.wait_for(lock, std::chrono::seconds(3), [&] {
                             return server.stream_bytes_received >=
                                    bytes_before_replacement + exact_budget.size();
                         }),
                         "expected replacement WebTransport object to drain before close-order test");
        }

        std::uint64_t close_expired_stream_id = 0;
        ok &= expect(deadline_client.open_stream(StreamDirection::kUnidirectional,
                                                 close_expired_stream_id).ok,
                     "expected activation-expiry WebTransport stream open to succeed");
        std::size_t bytes_before_close_expiry = 0;
        {
            std::lock_guard<std::mutex> lock(server.mutex);
            bytes_before_close_expiry = server.stream_bytes_received;
            server.stream_reset_errors.erase(close_expired_stream_id);
        }
        const std::vector<std::uint8_t> close_prefix = {0xE4};
        ok &= expect(deadline_client.try_write_object(close_expired_stream_id,
                                                      close_prefix,
                                                      false,
                                                      {}).disposition ==
                         ObjectWriteDisposition::kAccepted,
                     "expected WebTransport prefix before activation expiry");
        {
            std::unique_lock<std::mutex> lock(server.mutex);
            ok &= expect(server.condition.wait_for(lock, std::chrono::seconds(3), [&] {
                             return server.stream_bytes_received >=
                                    bytes_before_close_expiry + close_prefix.size();
                         }),
                         "expected WebTransport activation-expiry stream context");
        }
        ObjectWriteOptions close_expired_options;
        close_expired_options.object_deadline =
            std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
        const std::vector<std::uint8_t> close_expired_payload(9, 0xE5);
        ok &= expect(deadline_client.try_write_object(close_expired_stream_id,
                                                      close_expired_payload,
                                                      false,
                                                      close_expired_options).disposition ==
                         ObjectWriteDisposition::kAccepted,
                     "expected already-expired WebTransport object to enter activation");
        deadline_client.note_delivery_timeout(std::chrono::milliseconds(1));
        ok &= expect(deadline_client.close(0).ok,
                     "expected timer-wins webtransport client close to succeed");
        {
            std::lock_guard<std::mutex> lock(server.mutex);
            const auto reset =
                server.stream_reset_errors.find(close_expired_stream_id);
            ok &= expect(reset != server.stream_reset_errors.end() &&
                             reset->second == 0x02,
                         "expected activation-time WebTransport expiry reset before connection close");
        }
    }

    {
        auto* reset_failure_client = new WebTransportClient();
        ok &= expect(reset_failure_client->configure(endpoint, tls).ok,
                     "expected reset-failure WebTransport client configure to succeed");
        ok &= expect(reset_failure_client->connect().ok,
                     "expected reset-failure WebTransport client connect to succeed");
        constexpr std::uint64_t kInvalidResetStreamId =
            std::numeric_limits<std::uint64_t>::max();
        ok &= expect(reset_failure_client->reset_stream(
                         kInvalidResetStreamId, 0x02).ok,
                     "expected invalid WebTransport reset to enter packet-loop processing");
        auto close_future = std::async(std::launch::async, [reset_failure_client] {
            return reset_failure_client->close(0);
        });
        const bool close_completed =
            close_future.wait_for(std::chrono::seconds(5)) ==
            std::future_status::ready;
        ok &= expect(close_completed,
                     "expected failed WebTransport reset processing to terminate without hanging close");
        if (!close_completed) {
            std::cerr << "reset-failure close is hung; leaking client and exiting\n";
            stop_server(server);
            std::_Exit(1);
        }
        static_cast<void>(close_future.get());
        ok &= expect(!reset_failure_client->connection_close_sent_for_testing(),
                     "expected WebTransport reset failure to prevent connection close overtaking the reset");
        delete reset_failure_client;
    }

    stop_server(server);

    // Deterministic admission contract: zero peer credit keeps media queued,
    // while bidirectional request traffic remains independently writable.
    SilentServer capacity_server;
    ok &= expect(start_server(capacity_server, true),
                 "expected capacity-test webtransport server to start");
    if (!ok) {
        stop_server(capacity_server);
        return 1;
    }
    EndpointConfig capacity_endpoint = endpoint;
    capacity_endpoint.port = capacity_server.port;
    WebTransportClient capacity_client(10);
    ok &= expect(capacity_client.configure(capacity_endpoint, tls).ok,
                 "expected capacity client configure to succeed");
    ok &= expect(capacity_client.connect().ok,
                 "expected capacity client CONNECT to succeed");

    std::uint64_t media_stream_id = 0;
    ok &= expect(capacity_client.open_stream(StreamDirection::kUnidirectional,
                                             media_stream_id).ok,
                 "expected capacity media stream open to succeed");
    const std::vector<std::uint8_t> oversized_media(11, 0xC1);
    const std::vector<std::uint8_t> first_media(8, 0xC2);
    const std::vector<std::uint8_t> second_media(3, 0xC3);
    const auto admission_started = std::chrono::steady_clock::now();
    const auto oversized_result =
        capacity_client.try_write_object(media_stream_id, oversized_media, false, {});
    const auto first_result =
        capacity_client.try_write_object(media_stream_id, first_media, false, {});
    const auto second_result =
        capacity_client.try_write_object(media_stream_id, second_media, true, {});
    const auto admission_elapsed = std::chrono::steady_clock::now() - admission_started;
    ok &= expect(oversized_result.disposition == ObjectWriteDisposition::kFailed,
                 "expected oversized webtransport media to report a resource-limit failure");
    ok &= expect(!oversized_result.message.empty() &&
                     oversized_result.message.find("budget") != std::string::npos,
                 "expected oversized webtransport failure to explain the admission budget");
    ok &= expect(first_result.disposition == ObjectWriteDisposition::kAccepted,
                 "expected webtransport media within the 10-byte budget to be accepted");
    ok &= expect(second_result.disposition == ObjectWriteDisposition::kWouldBlock,
                 "expected combined webtransport media over the 10-byte budget to would-block");
    ok &= expect(admission_elapsed < std::chrono::seconds(1),
                 "expected full webtransport admission to return synchronously");

    std::uint64_t request_stream_id = 0;
    ok &= expect(capacity_client.open_stream(StreamDirection::kBidirectional,
                                             request_stream_id).ok,
                 "expected request stream open while webtransport media is full");
    const std::vector<std::uint8_t> request_bytes = {0x5A};
    ok &= expect(capacity_client.write_stream(request_stream_id, request_bytes, true).ok,
                 "expected reliable request write while webtransport media is full");
    {
        std::unique_lock<std::mutex> lock(capacity_server.mutex);
        ok &= expect(capacity_server.condition.wait_for(lock, std::chrono::seconds(5), [&] {
                         return capacity_server.request_stream_bytes_received >= request_bytes.size();
                     }),
                     "expected reliable request bytes while webtransport media is full");
    }

    ok &= expect(capacity_client.reset_stream(media_stream_id, 0).ok,
                 "expected webtransport media reset to succeed");
    std::uint64_t replacement_stream_id = 0;
    ok &= expect(capacity_client.open_stream(StreamDirection::kUnidirectional,
                                             replacement_stream_id).ok,
                 "expected replacement webtransport media stream open to succeed");
    const std::vector<std::uint8_t> exact_budget(10, 0xC4);
    ok &= expect(capacity_client.try_write_object(replacement_stream_id,
                                                  exact_budget,
                                                  true,
                                                  {}).disposition == ObjectWriteDisposition::kAccepted,
                 "expected webtransport reset to release the full admission budget");
    capacity_client.note_delivery_timeout(std::chrono::milliseconds(1));
    ok &= expect(capacity_client.close(0).ok,
                 "expected capacity client close to succeed");

    ok &= expect(capacity_client.configure(capacity_endpoint, tls).ok,
                 "expected webtransport reconfigure after close to succeed");
    ok &= expect(capacity_client.connect().ok,
                 "expected webtransport reconnect after close to succeed");
    std::uint64_t post_close_stream_id = 0;
    ok &= expect(capacity_client.open_stream(StreamDirection::kUnidirectional,
                                             post_close_stream_id).ok,
                 "expected post-close webtransport media stream open to succeed");
    ok &= expect(capacity_client.try_write_object(post_close_stream_id,
                                                  exact_budget,
                                                  true,
                                                  {}).disposition == ObjectWriteDisposition::kAccepted,
                 "expected webtransport close to clear the media admission budget");
    capacity_client.note_delivery_timeout(std::chrono::milliseconds(1));
    ok &= expect(capacity_client.close(0).ok,
                 "expected reconnected capacity client close to succeed");
    stop_server(capacity_server);
    return ok ? 0 : 1;
}
