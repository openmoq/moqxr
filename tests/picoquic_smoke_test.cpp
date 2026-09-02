#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/moq_draft.h"
#include "openmoq/publisher/transport/moqt_control_messages.h"
#include "openmoq/publisher/transport/moqt_session.h"
#include "openmoq/publisher/transport/picoquic_client.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <picoquic.h>
#include <picoquic_packet_loop.h>
#include <picoquic_internal.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using openmoq::publisher::ByteSpan;
using openmoq::publisher::CmsfObject;
using openmoq::publisher::CmsfObjectKind;
using openmoq::publisher::DraftVersion;
using openmoq::publisher::PublishPlan;
using openmoq::publisher::TrackDescription;
using openmoq::publisher::draft_profile;
using openmoq::publisher::materialize_publish_plan;
using openmoq::publisher::transport::EndpointConfig;
using openmoq::publisher::transport::MoqtSession;
using openmoq::publisher::transport::ObjectWriteDisposition;
using openmoq::publisher::transport::ObjectWriteOptions;
using openmoq::publisher::transport::PicoquicClient;
using openmoq::publisher::transport::ServerSetupMessage;
using openmoq::publisher::transport::TlsConfig;
using openmoq::publisher::transport::decode_varint;
using openmoq::publisher::transport::encode_server_setup_message;
using openmoq::publisher::transport::encode_varint;
using openmoq::publisher::transport::next_control_message;

constexpr const char* kPicoquicSourceDir = OPENMOQ_PICOQUIC_SOURCE_DIR;

struct SmokeServer {
    picoquic_quic_t* quic = nullptr;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable condition;
    uint16_t port = 0;
    std::size_t bytes_received = 0;
    std::size_t request_stream_bytes_received = 0;
    std::vector<std::uint8_t> media_stream_bytes;
    bool media_stream_fin_received = false;
    std::set<std::uint64_t> media_stream_fins;
    std::map<std::uint64_t, std::uint64_t> media_stream_reset_errors;
    std::vector<std::uint8_t> control_bytes;
    std::size_t publish_response_count = 0;
    bool loop_ready = false;
    bool stop_requested = false;
    bool loop_exited = false;
    int loop_return_code = 0;
    bool setup_response_sent = false;
    bool namespace_response_sent = false;
    bool publish_responses_sent = false;
};

bool trace_enabled() {
    static const bool enabled = std::getenv("OPENMOQ_PICOQUIC_TRACE") != nullptr;
    return enabled;
}

void trace(const std::string& message) {
    if (trace_enabled()) {
        std::cerr << "[picoquic-smoke] " << message << std::endl;
    }
}

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

PublishPlan make_span_backed_plan() {
    return {
        .draft = draft_profile(DraftVersion::kDraft14),
        .tracks = {TrackDescription{.track_id = 1, .handler_type = "vide", .codec = "avc1", .track_name = "vide_1"}},
        .objects = {
            CmsfObject{
                .kind = CmsfObjectKind::kInitialization,
                .track_name = "init",
                .group_id = 0,
                .object_id = 0,
                .payload = ByteSpan{.offset = 0, .size = 4},
            },
            CmsfObject{
                .kind = CmsfObjectKind::kMedia,
                .track_name = "vide_1",
                .group_id = 1,
                .object_id = 0,
                .payload = ByteSpan{.offset = 4, .size = 3},
            },
        },
    };
}

std::vector<std::uint8_t> encode_publish_namespace_ok(std::uint64_t request_id) {
    const std::vector<std::uint8_t> payload = encode_varint(request_id);
    std::vector<std::uint8_t> message = {
        0x07,
        static_cast<std::uint8_t>((payload.size() >> 8) & 0xff),
        static_cast<std::uint8_t>(payload.size() & 0xff),
    };
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

std::vector<std::uint8_t> encode_publish_ok(std::uint64_t request_id) {
    std::vector<std::uint8_t> payload = encode_varint(request_id);
    payload.push_back(1);     // Forward.
    payload.push_back(0x80);  // Subscriber priority.
    payload.push_back(1);     // Group order.
    const std::vector<std::uint8_t> filter_type = encode_varint(0);
    const std::vector<std::uint8_t> parameter_count = encode_varint(0);
    payload.insert(payload.end(), filter_type.begin(), filter_type.end());
    payload.insert(payload.end(), parameter_count.begin(), parameter_count.end());

    std::vector<std::uint8_t> message = encode_varint(0x1e);
    const std::vector<std::uint8_t> payload_length = encode_varint(payload.size());
    message.insert(message.end(), payload_length.begin(), payload_length.end());
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

int smoke_server_callback(picoquic_cnx_t* cnx,
                          uint64_t stream_id,
                          uint8_t* bytes,
                          size_t length,
                          picoquic_call_back_event_t event,
                          void* callback_ctx,
                          void* stream_ctx) {
    static_cast<void>(cnx);
    static_cast<void>(stream_id);
    static_cast<void>(stream_ctx);

    auto* server = static_cast<SmokeServer*>(callback_ctx);
    if (server == nullptr) {
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }

    switch (event) {
        case picoquic_callback_stream_data:
        case picoquic_callback_stream_fin: {
            trace("server stream data event bytes=" + std::to_string(length));
            std::lock_guard<std::mutex> lock(server->mutex);
            server->bytes_received += length;
            if (stream_id != 0 && (stream_id & 0x2) == 0) {
                server->request_stream_bytes_received += length;
            }
            if ((stream_id & 0x3) == 0x2) {
                if (bytes != nullptr && length != 0) {
                    server->media_stream_bytes.insert(server->media_stream_bytes.end(), bytes, bytes + length);
                }
                if (event == picoquic_callback_stream_fin) {
                    server->media_stream_fin_received = true;
                    server->media_stream_fins.insert(stream_id);
                }
            }
            if (stream_id == 0) {
                server->control_bytes.insert(server->control_bytes.end(), bytes, bytes + length);
                std::size_t message_size = 0;
                while (next_control_message(server->control_bytes, DraftVersion::kDraft14, message_size)) {
                    const std::span<const std::uint8_t> message(server->control_bytes.data(), message_size);
                    std::size_t offset = 0;
                    std::uint64_t message_type = 0;
                    if (!decode_varint(message, offset, message_type)) {
                        return PICOQUIC_ERROR_UNEXPECTED_ERROR;
                    }

                    std::vector<std::uint8_t> response;
                    if (message_type == 0x20 && !server->setup_response_sent) {
                        response = encode_server_setup_message({
                            .draft = DraftVersion::kDraft14,
                            .max_request_id = 8,
                        });
                    } else if (message_type == 0x06 && !server->namespace_response_sent) {
                        response = encode_publish_namespace_ok(0);
                    } else if (message_type == 0x1d && server->publish_response_count < 2) {
                        const std::uint64_t request_id = 2 + (2 * server->publish_response_count);
                        response = encode_publish_ok(request_id);
                    }

                    if (!response.empty() &&
                        picoquic_add_to_stream(cnx, stream_id, response.data(), response.size(), 0) == 0) {
                        if (message_type == 0x20) {
                            server->setup_response_sent = true;
                        } else if (message_type == 0x06) {
                            server->namespace_response_sent = true;
                        } else if (message_type == 0x1d) {
                            ++server->publish_response_count;
                            server->publish_responses_sent = server->publish_response_count == 2;
                        }
                    }

                    server->control_bytes.erase(server->control_bytes.begin(),
                                                server->control_bytes.begin() +
                                                    static_cast<std::ptrdiff_t>(message_size));
                }
            }
            server->condition.notify_all();
            return 0;
        }
        case picoquic_callback_close:
        case picoquic_callback_application_close:
        case picoquic_callback_stateless_reset:
            trace("server connection close event");
            return 0;
        case picoquic_callback_stream_reset: {
            std::lock_guard<std::mutex> lock(server->mutex);
            server->media_stream_reset_errors.insert_or_assign(
                stream_id, picoquic_get_remote_stream_error(cnx, stream_id));
            server->condition.notify_all();
            return 0;
        }
        default:
            static_cast<void>(bytes);
            return 0;
    }
}

int smoke_server_loop_callback(picoquic_quic_t* quic,
                               picoquic_packet_loop_cb_enum cb_mode,
                               void* callback_ctx,
                               void* callback_arg) {
    static_cast<void>(quic);

    auto* server = static_cast<SmokeServer*>(callback_ctx);
    if (server == nullptr) {
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }

    switch (cb_mode) {
        case picoquic_packet_loop_ready: {
            trace("server packet loop ready");
            auto* options = static_cast<picoquic_packet_loop_options_t*>(callback_arg);
            if (options != nullptr) {
                options->do_time_check = 1;
            }
            std::lock_guard<std::mutex> lock(server->mutex);
            server->loop_ready = true;
            server->condition.notify_all();
            return 0;
        }
        case picoquic_packet_loop_after_receive:
            if (callback_arg != nullptr) {
                trace("server packet loop after receive count=" +
                      std::to_string(*static_cast<size_t*>(callback_arg)));
            } else {
                trace("server packet loop after receive");
            }
            {
                // Current picoquic overwrites the time_check return code on
                // the poll-timeout path, so also honour stop requests from the
                // traffic callbacks; stop_server() nudges the socket to
                // guarantee one fires.
                std::lock_guard<std::mutex> lock(server->mutex);
                if (server->stop_requested) {
                    return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
                }
            }
            return 0;
        case picoquic_packet_loop_after_send:
            if (callback_arg != nullptr) {
                trace("server packet loop after send count=" +
                      std::to_string(*static_cast<size_t*>(callback_arg)));
            } else {
                trace("server packet loop after send");
            }
            {
                std::lock_guard<std::mutex> lock(server->mutex);
                if (server->stop_requested) {
                    return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
                }
            }
            return 0;
        case picoquic_packet_loop_port_update:
            if (callback_arg != nullptr) {
                auto* addr = static_cast<sockaddr*>(callback_arg);
                std::lock_guard<std::mutex> lock(server->mutex);
                if (addr->sa_family == AF_INET) {
                    server->port = reinterpret_cast<sockaddr_in*>(addr)->sin_port;
                } else if (addr->sa_family == AF_INET6) {
                    server->port = reinterpret_cast<sockaddr_in6*>(addr)->sin6_port;
                }
                trace("server port update port=" + std::to_string(server->port));
                server->condition.notify_all();
            }
            return 0;
        case picoquic_packet_loop_time_check: {
            trace("server packet loop time check");
            auto* time_check = static_cast<packet_loop_time_check_arg_t*>(callback_arg);
            std::lock_guard<std::mutex> lock(server->mutex);
            if (time_check != nullptr && time_check->delta_t > 10000) {
                time_check->delta_t = 10000;
            }
            return server->stop_requested ? PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP : 0;
        }
        default:
            return 0;
    }
}

bool start_server(SmokeServer& server, bool hold_unidirectional_data = true) {
    const std::string cert_path = std::string(kPicoquicSourceDir) + "/certs/cert.pem";
    const std::string key_path = std::string(kPicoquicSourceDir) + "/certs/key.pem";

    server.quic = picoquic_create(8, cert_path.c_str(), key_path.c_str(), nullptr, "moq-00",
                                  smoke_server_callback, &server, nullptr, nullptr, nullptr,
                                  picoquic_current_time(), nullptr, nullptr, nullptr, 0);
    if (server.quic == nullptr) {
        return false;
    }

    picoquic_set_cookie_mode(server.quic, 2);
    picoquic_tp_t tp = *picoquic_get_default_tp(server.quic);
    // Hold client-initiated unidirectional data at the sender so the
    // transport's literal 10-byte admission budget can be exercised without
    // racing the packet loop. Bidirectional control/request traffic remains
    // flow-controlled independently and must still be delivered.
    if (hold_unidirectional_data) {
        tp.initial_max_stream_data_uni = 0;
    }
    picoquic_set_default_tp(server.quic, &tp);
    server.thread = std::thread([&server] {
        trace("server packet loop thread start");
        // Mirror the client-side socket loop configuration and avoid UDP GSO in
        // the loopback smoke test while the handshake path is being stabilized.
        const int ret =
            picoquic_packet_loop(server.quic, server.port, AF_INET, 0, 0, 1, smoke_server_loop_callback, &server);
        trace("server packet loop thread exit rc=" + std::to_string(ret));
        std::lock_guard<std::mutex> lock(server.mutex);
        server.loop_return_code = ret;
        server.loop_exited = true;
        server.condition.notify_all();
    });

    std::unique_lock<std::mutex> lock(server.mutex);
    const bool started = server.condition.wait_for(lock, std::chrono::seconds(5), [&] {
        return (server.loop_ready && server.port != 0) || server.loop_exited;
    });
    if (!started) {
        std::cerr << "server loop did not report ready within timeout\n";
    } else if (server.loop_exited) {
        std::cerr << "server loop exited early, return_code=" << server.loop_return_code << '\n';
    }
    return started && server.loop_ready && server.port != 0 && !server.loop_exited;
}

void stop_server(SmokeServer& server) {
    {
        std::lock_guard<std::mutex> lock(server.mutex);
        server.stop_requested = true;
    }

    // Wake the packet loop out of poll() so the stop request is observed; see
    // the comment in smoke_server_loop_callback.
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

    std::cerr << "smoke test start" << std::endl;
    std::cerr << "trace " << (trace_enabled() ? "enabled" : "disabled") << std::endl;

    SmokeServer server;
    ok &= expect(start_server(server), "expected picoquic smoke server to start");
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

    PicoquicClient transport(10);
    MoqtSession session(transport, "media", true);

    auto status = session.connect(endpoint, tls);
    if (!status.ok) {
        std::cerr << "connect error: " << status.message << '\n';
    }
    ok &= expect(status.ok,
                 status.ok ? "expected picoquic client handshake to succeed"
                           : "expected picoquic client handshake to succeed: " + status.message);

    const std::vector<std::uint8_t> source_bytes = {'I', 'N', 'I', 'T', 'M', 'S', 'G'};
    const PublishPlan materialized = materialize_publish_plan(make_span_backed_plan(), source_bytes);

    status = session.publish(materialized);
    if (!status.ok) {
        std::cerr << "publish error: " << status.message << '\n';
    }
    ok &= expect(status.ok, status.ok ? "expected publish to succeed after setup negotiation"
                                      : "expected publish to succeed after setup negotiation: " + status.message);

    {
        std::unique_lock<std::mutex> lock(server.mutex);
        ok &= expect(server.condition.wait_for(lock, std::chrono::seconds(5), [&] {
                         return server.setup_response_sent &&
                                server.namespace_response_sent &&
                                server.publish_responses_sent;
                     }),
                     "expected server to acknowledge setup, namespace, and track publishing");
    }

    std::uint64_t media_stream_id = 0;
    status = transport.open_stream(openmoq::publisher::transport::StreamDirection::kUnidirectional,
                                   media_stream_id);
    ok &= expect(status.ok, "expected media stream open to succeed");
    const std::vector<std::uint8_t> first_media(8, 0xA1);
    const std::vector<std::uint8_t> second_media(3, 0xB2);
    const std::vector<std::uint8_t> oversized_media(11, 0xD4);
    const auto admission_started = std::chrono::steady_clock::now();
    const auto oversized_result =
        transport.try_write_object(media_stream_id, oversized_media, false, ObjectWriteOptions{});
    const auto first_result =
        transport.try_write_object(media_stream_id, first_media, false, ObjectWriteOptions{});
    const auto second_result =
        transport.try_write_object(media_stream_id, second_media, true, ObjectWriteOptions{});
    const auto admission_elapsed = std::chrono::steady_clock::now() - admission_started;
    ok &= expect(oversized_result.disposition == ObjectWriteDisposition::kFailed,
                 "expected an individually oversized media object to report a resource-limit failure");
    ok &= expect(!oversized_result.message.empty() &&
                     oversized_result.message.find("budget") != std::string::npos,
                 "expected oversized media failure to explain the admission budget");
    ok &= expect(first_result.disposition == ObjectWriteDisposition::kAccepted,
                 "expected first media write within 10-byte budget to be accepted");
    ok &= expect(second_result.disposition == ObjectWriteDisposition::kWouldBlock,
                 "expected combined media writes over 10-byte budget to would-block");
    ok &= expect(admission_elapsed < std::chrono::seconds(1),
                 "expected full media admission to return synchronously without the 30-second wait");

    std::uint64_t request_stream_id = 0;
    status = transport.open_stream(openmoq::publisher::transport::StreamDirection::kBidirectional,
                                   request_stream_id);
    ok &= expect(status.ok, "expected request stream open while media is full to succeed");
    const std::vector<std::uint8_t> request_bytes = {0x5A};
    status = transport.write_stream(request_stream_id, request_bytes, true);
    ok &= expect(status.ok, "expected reliable request write while media is full to succeed");
    {
        std::unique_lock<std::mutex> lock(server.mutex);
        ok &= expect(server.condition.wait_for(lock, std::chrono::seconds(5), [&] {
                         return server.request_stream_bytes_received >= request_bytes.size();
                     }),
                     "expected reliable request bytes to arrive while media is full");
    }

    status = transport.reset_stream(media_stream_id, 0);
    ok &= expect(status.ok, "expected media reset to succeed");
    std::uint64_t timeout_stream_id = 0;
    status = transport.open_stream(openmoq::publisher::transport::StreamDirection::kUnidirectional,
                                   timeout_stream_id);
    ok &= expect(status.ok, "expected timer-wins raw-QUIC stream open to succeed");
    ObjectWriteOptions timer_wins_options;
    timer_wins_options.subgroup_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    const std::vector<std::uint8_t> timed_media(10, 0xE4);
    const auto timed_result = transport.try_write_object(
        timeout_stream_id, timed_media, true, timer_wins_options);
    ok &= expect(timed_result.disposition == ObjectWriteDisposition::kAccepted,
                 "expected raw-QUIC timer stream to fill the bounded media queue");
    {
        std::unique_lock<std::mutex> lock(server.mutex);
        ok &= expect(server.condition.wait_for(lock, std::chrono::seconds(3), [&] {
                         return server.media_stream_reset_errors.contains(timeout_stream_id);
                     }),
                     "expected the raw-QUIC subgroup timer to reset the flow-controlled stream");
        const auto reset = server.media_stream_reset_errors.find(timeout_stream_id);
        if (reset != server.media_stream_reset_errors.end()) {
            ok &= expect(reset->second == 0x02,
                         "expected raw-QUIC timer-wins reset error 0x02");
        }
    }
    ok &= expect(transport.media_stream_expired(timeout_stream_id),
                 "expected raw-QUIC timer expiry to remain queryable by stream ID");
    std::uint64_t replacement_stream_id = 0;
    status = transport.open_stream(openmoq::publisher::transport::StreamDirection::kUnidirectional,
                                   replacement_stream_id);
    ok &= expect(status.ok, "expected replacement media stream open to succeed");
    const std::vector<std::uint8_t> exact_budget(10, 0xC3);
    const auto after_reset =
        transport.try_write_object(replacement_stream_id, exact_budget, true, ObjectWriteOptions{});
    ok &= expect(after_reset.disposition == ObjectWriteDisposition::kAccepted,
                 "expected timeout reset to release the complete media admission budget");

    transport.note_delivery_timeout(std::chrono::milliseconds(1));

    status = session.close(0);
    ok &= expect(status.ok, "expected picoquic session close to succeed");

    status = transport.configure(endpoint, tls);
    ok &= expect(status.ok, "expected reconfigure after close to succeed");
    status = transport.connect();
    ok &= expect(status.ok, "expected reconnect after close to succeed");
    std::uint64_t post_close_stream_id = 0;
    status = transport.open_stream(openmoq::publisher::transport::StreamDirection::kUnidirectional,
                                   post_close_stream_id);
    ok &= expect(status.ok, "expected post-close media stream open to succeed");
    const auto after_close =
        transport.try_write_object(post_close_stream_id, exact_budget, true, ObjectWriteOptions{});
    ok &= expect(after_close.disposition == ObjectWriteDisposition::kAccepted,
                 "expected close to clear queued media before the next connection");
    transport.note_delivery_timeout(std::chrono::milliseconds(1));
    status = transport.close(0);
    ok &= expect(status.ok, "expected reconnected picoquic transport close to succeed");

    stop_server(server);

    SmokeServer delivery_server;
    ok &= expect(start_server(delivery_server, false),
                 "expected pull-delivery picoquic smoke server to start");
    if (!ok) {
        stop_server(delivery_server);
        return 1;
    }
    EndpointConfig delivery_endpoint = endpoint;
    delivery_endpoint.port = delivery_server.port;
    PicoquicClient delivery_client;
    ok &= expect(delivery_client.configure(delivery_endpoint, tls).ok,
                 "expected pull-delivery client configure to succeed");
    status = delivery_client.connect();
    ok &= expect(status.ok, "expected pull-delivery client connect to succeed");
    std::uint64_t delivery_stream_id = 0;
    status = delivery_client.open_stream(
        openmoq::publisher::transport::StreamDirection::kUnidirectional,
        delivery_stream_id);
    ok &= expect(status.ok, "expected pull-delivery media stream open to succeed");
    std::vector<std::uint8_t> delivery_bytes(5000);
    for (std::size_t index = 0; index < delivery_bytes.size(); ++index) {
        delivery_bytes[index] = static_cast<std::uint8_t>(index % 251);
    }
    const auto delivery_first = delivery_client.try_write_object(
        delivery_stream_id,
        std::span<const std::uint8_t>(delivery_bytes.data(), 3000),
        false,
        ObjectWriteOptions{});
    ObjectWriteOptions ack_wins_options;
    ack_wins_options.subgroup_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    const auto delivery_second = delivery_client.try_write_object(
        delivery_stream_id,
        std::span<const std::uint8_t>(delivery_bytes.data() + 3000, 2000),
        true,
        ack_wins_options);
    ok &= expect(delivery_first.disposition == ObjectWriteDisposition::kAccepted &&
                     delivery_second.disposition == ObjectWriteDisposition::kAccepted,
                 "expected multi-write pull delivery to accept both media objects");
    {
        std::unique_lock<std::mutex> lock(delivery_server.mutex);
        ok &= expect(delivery_server.condition.wait_for(lock, std::chrono::seconds(5), [&] {
                         return delivery_server.media_stream_fin_received;
                     }),
                     "expected pull delivery to send the media stream FIN");
        ok &= expect(delivery_server.media_stream_bytes == delivery_bytes,
                     "expected partial provide callbacks to deliver every media byte in order");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    ok &= expect(!delivery_client.media_stream_expired(delivery_stream_id),
                 "expected raw-QUIC stream release after ACK to cancel the subgroup timer");
    {
        std::lock_guard<std::mutex> lock(delivery_server.mutex);
        ok &= expect(!delivery_server.media_stream_reset_errors.contains(delivery_stream_id),
                     "expected no raw-QUIC timeout reset after the acknowledged deadline-bearing FIN");
    }
    std::uint64_t empty_fin_stream_id = 0;
    status = delivery_client.open_stream(
        openmoq::publisher::transport::StreamDirection::kUnidirectional,
        empty_fin_stream_id);
    ok &= expect(status.ok, "expected empty-FIN media stream open to succeed");
    const auto empty_fin = delivery_client.try_write_object(
        empty_fin_stream_id, {}, true, ObjectWriteOptions{});
    ok &= expect(empty_fin.disposition == ObjectWriteDisposition::kAccepted,
                 "expected zero-byte FIN media write to be admitted");
    std::uint64_t expired_stream_id = 0;
    status = delivery_client.open_stream(
        openmoq::publisher::transport::StreamDirection::kUnidirectional,
        expired_stream_id);
    ok &= expect(status.ok, "expected expired-object media stream open to succeed");
    ObjectWriteOptions expired_options;
    expired_options.object_deadline = std::chrono::steady_clock::now() -
                                      std::chrono::milliseconds(1);
    const std::vector<std::uint8_t> expired_bytes(5, 0xE5);
    const auto expired_result = delivery_client.try_write_object(
        expired_stream_id, expired_bytes, false, expired_options);
    ok &= expect(expired_result.disposition == ObjectWriteDisposition::kAccepted,
                 "expected already-expired media to enter admission before callback pruning");
    delivery_client.note_delivery_timeout(std::chrono::seconds(5));
    const auto close_started = std::chrono::steady_clock::now();
    status = delivery_client.close(0);
    ok &= expect(status.ok, "expected pull-delivery client close to drain an empty FIN");
    ok &= expect(std::chrono::steady_clock::now() - close_started < std::chrono::seconds(2),
                 "expected callback-pruned media not to strand close until its timeout");
    {
        std::lock_guard<std::mutex> lock(delivery_server.mutex);
        ok &= expect(delivery_server.media_stream_fins.contains(empty_fin_stream_id),
                     "expected close drain to deliver the zero-byte stream FIN");
    }
    stop_server(delivery_server);
    return ok ? 0 : 1;
}
