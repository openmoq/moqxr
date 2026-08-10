#include "openmoq/publisher/transport/picoquic_client.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef OPENMOQ_HAS_PICOQUIC
#include <picoquic.h>
#include <picoquic_packet_loop.h>
#include <picoquic_utils.h>
#include <picosocks.h>

#include "tls_verification.h"
#endif

namespace openmoq::publisher::transport {

struct PicoquicClient::Impl {
    struct PendingWrite {
        std::uint64_t stream_id = 0;
        std::vector<std::uint8_t> bytes;
        bool fin = false;
        std::chrono::steady_clock::time_point enqueued_at{};
        std::size_t defer_count = 0;
    };

    struct ReceivedStreamData {
        std::vector<std::uint8_t> bytes;
        bool fin = false;
    };

    struct AcceptedWrite {
        std::uint64_t stream_id = 0;
        std::size_t bytes = 0;
        bool fin = false;
        std::size_t defer_count = 0;
        std::chrono::steady_clock::time_point accepted_at{};
        long long queue_age_ms = 0;
    };

    std::mutex mutex;
    std::mutex join_mutex; // serialises concurrent close()/disconnect() join attempts
    std::condition_variable condition;
    std::deque<PendingWrite> pending_writes;
    std::deque<AcceptedWrite> accepted_writes_waiting_for_send;
    std::map<std::uint64_t, ReceivedStreamData> received_streams;
    std::set<std::uint64_t> accepted_streams;
    bool connected = false;
    bool failed = false;
    bool disconnected = false;
    bool close_requested = false;
    std::uint64_t close_error_code = 0;
    bool loop_ready = false;
    bool loop_exited = false;
    std::size_t total_bytes_sent = 0;
    std::size_t total_bytes_received = 0;
    std::size_t nonzero_send_events = 0;
    std::size_t nonzero_receive_events = 0;
    std::size_t consecutive_zero_send_events = 0;
    std::string last_error;
    std::thread packet_loop_thread;

#ifdef OPENMOQ_HAS_PICOQUIC
    picoquic_quic_t* quic = nullptr;
    picoquic_cnx_t* cnx = nullptr;
    sockaddr_storage server_address{};
    int packet_loop_return_code = 0;
#endif
};

#ifdef OPENMOQ_HAS_PICOQUIC
namespace {

bool trace_enabled() {
    static const bool enabled = std::getenv("OPENMOQ_PICOQUIC_TRACE") != nullptr;
    return enabled;
}

std::chrono::steady_clock::time_point trace_epoch() {
    static const auto epoch = std::chrono::steady_clock::now();
    return epoch;
}

long long trace_elapsed_ms(std::chrono::steady_clock::time_point time_point) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time_point - trace_epoch()).count();
}

void trace(const std::string& message) {
    if (trace_enabled()) {
        std::cerr << "[picoquic-client] " << message << std::endl;
    }
}

std::string hex_dump(std::span<const std::uint8_t> bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index != 0) {
            out << ' ';
        }
        out << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return out.str();
}

int apply_pending_operations(PicoquicClient::Impl& impl) {
    if (impl.cnx == nullptr) {
        return 0;
    }

    std::deque<PicoquicClient::Impl::PendingWrite> writes;
    std::size_t queued_count = 0;
    std::size_t queued_bytes = 0;
    bool close_requested = false;

    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        writes.swap(impl.pending_writes);
        queued_count = writes.size();
        for (const auto& write : writes) {
            queued_bytes += write.bytes.size();
        }
        close_requested = impl.close_requested;
    }

    if (trace_enabled() && (queued_count != 0 || close_requested)) {
        trace("flush start queued_count=" + std::to_string(queued_count) +
              " queued_bytes=" + std::to_string(queued_bytes) +
              " close_requested=" + std::to_string(close_requested ? 1 : 0) +
              " now_ms=" + std::to_string(trace_elapsed_ms(std::chrono::steady_clock::now())));
    }

    std::deque<PicoquicClient::Impl::PendingWrite> deferred_writes;
    std::deque<PicoquicClient::Impl::AcceptedWrite> accepted_writes;
    std::size_t accepted_count = 0;
    std::size_t accepted_bytes = 0;
    std::size_t deferred_count = 0;
    std::size_t deferred_bytes = 0;
    while (!writes.empty()) {
        auto& write = writes.front();
        const auto now = std::chrono::steady_clock::now();
        const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - write.enqueued_at).count();
        trace("stream " + std::to_string(write.stream_id) +
              " bytes=[" + hex_dump(write.bytes) + "]" +
              " queue_age_ms=" + std::to_string(age_ms) +
              " defer_count=" + std::to_string(write.defer_count) +
              " now_ms=" + std::to_string(trace_elapsed_ms(now)));
        const int ret = picoquic_add_to_stream(
            impl.cnx, write.stream_id, write.bytes.data(), write.bytes.size(), write.fin ? 1 : 0);
        if (ret != 0) {
            // picoquic rejected the write, most commonly because its per-stream
            // send buffer is full. This is transient back-pressure: defer the
            // remaining writes in order so they stay queued until picoquic has
            // drained enough to accept them. write_stream caps total queued
            // bytes on the producer side, so this cannot grow without bound.
            write.defer_count += 1;
            deferred_count += 1;
            deferred_bytes += write.bytes.size();
            trace("add_to_stream deferred for stream " + std::to_string(write.stream_id) +
                  " queue_age_ms=" + std::to_string(age_ms) +
                  " defer_count=" + std::to_string(write.defer_count));
            deferred_writes.push_back(std::move(write));
            writes.pop_front();
            for (auto& pending : writes) {
                deferred_count += 1;
                deferred_bytes += pending.bytes.size();
                deferred_writes.push_back(std::move(pending));
            }
            writes.clear();
            break;
        }
        accepted_count += 1;
        accepted_bytes += write.bytes.size();
        accepted_writes.push_back(PicoquicClient::Impl::AcceptedWrite{
            .stream_id = write.stream_id,
            .bytes = write.bytes.size(),
            .fin = write.fin,
            .defer_count = write.defer_count,
            .accepted_at = now,
            .queue_age_ms = age_ms,
        });
        if (write.defer_count != 0) {
            trace("add_to_stream accepted after defer stream=" + std::to_string(write.stream_id) +
                  " queue_age_ms=" + std::to_string(age_ms) +
                  " defer_count=" + std::to_string(write.defer_count));
        }
        writes.pop_front();
    }

    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        for (auto& accepted : accepted_writes) {
            impl.accepted_writes_waiting_for_send.push_back(std::move(accepted));
        }
        for (auto it = deferred_writes.rbegin(); it != deferred_writes.rend(); ++it) {
            impl.pending_writes.push_front(std::move(*it));
        }
        impl.condition.notify_all();
    }

    if (trace_enabled() && (queued_count != 0 || accepted_count != 0 || deferred_count != 0 || close_requested)) {
        trace("flush end accepted_count=" + std::to_string(accepted_count) +
              " accepted_bytes=" + std::to_string(accepted_bytes) +
              " deferred_count=" + std::to_string(deferred_count) +
              " deferred_bytes=" + std::to_string(deferred_bytes) +
              " pending_after=" + std::to_string(deferred_writes.size()) +
              " now_ms=" + std::to_string(trace_elapsed_ms(std::chrono::steady_clock::now())));
    }

    if (close_requested) {
        std::uint64_t close_error_code = 0;
        {
            std::lock_guard<std::mutex> lock(impl.mutex);
            close_error_code = impl.close_error_code;
        }
        trace("close requested");
        const int ret = picoquic_close(impl.cnx, close_error_code);
        if (ret != 0) {
            std::lock_guard<std::mutex> lock(impl.mutex);
            impl.failed = true;
            impl.last_error = "picoquic_close failed";
            impl.condition.notify_all();
            return ret;
        }
    }

    return 0;
}

int client_callback(picoquic_cnx_t* cnx,
                    uint64_t stream_id,
                    uint8_t* bytes,
                    size_t length,
                    picoquic_call_back_event_t event,
                    void* callback_ctx,
                    void* stream_ctx) {
    static_cast<void>(cnx);
    static_cast<void>(stream_ctx);

    auto* impl = static_cast<PicoquicClient::Impl*>(callback_ctx);
    if (impl == nullptr) {
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }

    switch (event) {
        case picoquic_callback_stream_data:
        case picoquic_callback_stream_fin: {
            std::lock_guard<std::mutex> lock(impl->mutex);
            auto& received = impl->received_streams[stream_id];
            if (bytes != nullptr && length != 0) {
                received.bytes.insert(received.bytes.end(), bytes, bytes + length);
            }
            if (event == picoquic_callback_stream_fin) {
                received.fin = true;
            }
            impl->condition.notify_all();
            return 0;
        }
        case picoquic_callback_almost_ready:
        case picoquic_callback_ready: {
            trace(event == picoquic_callback_ready ? "callback ready" : "callback almost_ready");
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->connected = true;
            impl->condition.notify_all();
            return 0;
        }
        case picoquic_callback_close:
        case picoquic_callback_application_close:
        case picoquic_callback_stateless_reset: {
            const std::uint64_t local_error = picoquic_get_local_error(cnx);
            const std::uint64_t remote_error = picoquic_get_remote_error(cnx);
            const std::uint64_t application_error = picoquic_get_application_error(cnx);
            std::uint64_t local_reason = 0;
            std::uint64_t remote_reason = 0;
            std::uint64_t local_application_reason = 0;
            std::uint64_t remote_application_reason = 0;
            picoquic_get_close_reasons(
                cnx, &local_reason, &remote_reason, &local_application_reason, &remote_application_reason);
            trace(std::string("callback connection closed event=") +
                  (event == picoquic_callback_application_close
                       ? "application_close"
                       : (event == picoquic_callback_stateless_reset ? "stateless_reset" : "close")) +
                  " local_error=" + std::to_string(local_error) +
                  " remote_error=" + std::to_string(remote_error) +
                  " application_error=" + std::to_string(application_error) +
                  " local_reason=" + std::to_string(local_reason) +
                  " remote_reason=" + std::to_string(remote_reason) +
                  " local_application_reason=" + std::to_string(local_application_reason) +
                  " remote_application_reason=" + std::to_string(remote_application_reason));
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->disconnected = true;
            if (!impl->connected) {
                impl->failed = true;
                impl->last_error = "connection closed before reaching ready state";
                if (local_error != 0) {
                    impl->last_error += "; local: " + tlsverify::describe_quic_error(local_error);
                    if (tlsverify::is_certificate_verification_error(local_error)) {
                        impl->last_error +=
                            " -- server certificate verification failed; check the server "
                            "certificate, ca_path (--ca), and that the SNI/host matches the "
                            "certificate, or set insecure_skip_verify (--insecure) to "
                            "intentionally skip verification";
                    }
                }
                if (remote_error != 0) {
                    impl->last_error += "; peer: " + tlsverify::describe_quic_error(remote_error);
                    if (remote_error >= 0x0100 && remote_error <= 0x01ff) {
                        impl->last_error += " -- the server rejected the TLS handshake";
                    }
                }
            } else if (impl->last_error.empty() &&
                       (remote_error != 0 || remote_reason != 0 || remote_application_reason != 0 ||
                        application_error != 0)) {
                impl->last_error = "peer closed connection with remote_error=" + std::to_string(remote_error) +
                                   ", remote_reason=" + std::to_string(remote_reason) +
                                   ", remote_application_reason=" + std::to_string(remote_application_reason) +
                                   ", application_error=" + std::to_string(application_error);
            }
            impl->condition.notify_all();
            return 0;
        }
        default:
            return 0;
    }
}

int loop_callback(picoquic_quic_t* quic,
                  picoquic_packet_loop_cb_enum cb_mode,
                  void* callback_ctx,
                  void* callback_arg) {
    static_cast<void>(quic);
    static_cast<void>(callback_arg);

    auto* impl = static_cast<PicoquicClient::Impl*>(callback_ctx);
    if (impl == nullptr) {
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }

    switch (cb_mode) {
        case picoquic_packet_loop_ready: {
            trace("packet loop ready");
            auto* options = static_cast<picoquic_packet_loop_options_t*>(callback_arg);
            if (options != nullptr) {
                options->do_time_check = 1;
            }
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->loop_ready = true;
            impl->condition.notify_all();
            return 0;
        }
        case picoquic_packet_loop_after_receive:
            if (callback_arg != nullptr) {
                const auto count = *static_cast<size_t*>(callback_arg);
                {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    impl->total_bytes_received += count;
                    if (count != 0) {
                        impl->nonzero_receive_events++;
                    }
                }
                trace("packet loop after receive count=" + std::to_string(count));
            } else {
                trace("packet loop after receive");
            }
            return 0;
        case picoquic_packet_loop_wake_up:
            trace("packet loop wake up");
            return apply_pending_operations(*impl);
        case picoquic_packet_loop_time_check: {
            trace("packet loop time check");
            auto* time_check = static_cast<packet_loop_time_check_arg_t*>(callback_arg);
            if (time_check != nullptr && time_check->delta_t > 10000) {
                time_check->delta_t = 10000;
            }
            return apply_pending_operations(*impl);
        }
        case picoquic_packet_loop_after_send: {
            if (callback_arg != nullptr) {
                const auto count = *static_cast<size_t*>(callback_arg);
                {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    impl->total_bytes_sent += count;
                    if (count != 0) {
                        impl->nonzero_send_events++;
                        impl->consecutive_zero_send_events = 0;
                    } else {
                        impl->consecutive_zero_send_events++;
                    }
                    if (count != 0 || impl->consecutive_zero_send_events == 1 ||
                        impl->consecutive_zero_send_events % 50 == 0) {
                        trace("packet loop after send count=" + std::to_string(count) +
                              " total_sent=" + std::to_string(impl->total_bytes_sent) +
                              " total_received=" + std::to_string(impl->total_bytes_received));
                    }
                    if (count != 0 && !impl->accepted_writes_waiting_for_send.empty()) {
                        const auto now = std::chrono::steady_clock::now();
                        while (!impl->accepted_writes_waiting_for_send.empty()) {
                            const auto& accepted = impl->accepted_writes_waiting_for_send.front();
                            const auto accepted_to_send_ms =
                                std::chrono::duration_cast<std::chrono::milliseconds>(now - accepted.accepted_at)
                                    .count();
                            trace("accepted_to_after_send stream=" + std::to_string(accepted.stream_id) +
                                  " accepted_to_send_ms=" + std::to_string(accepted_to_send_ms) +
                                  " queue_age_ms=" + std::to_string(accepted.queue_age_ms) +
                                  " bytes=" + std::to_string(accepted.bytes) +
                                  " fin=" + std::to_string(accepted.fin ? 1 : 0) +
                                  " defer_count=" + std::to_string(accepted.defer_count) +
                                  " now_ms=" + std::to_string(trace_elapsed_ms(now)));
                            impl->accepted_writes_waiting_for_send.pop_front();
                        }
                    }
                }
            } else {
                trace("packet loop after send");
            }
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (impl->disconnected) {
                return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
            }
            return 0;
        }
        case picoquic_packet_loop_port_update:
            return 0;
        default:
            return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }
}

}  // namespace
#endif

PicoquicClient::PicoquicClient() : impl_(std::make_unique<Impl>()) {}

PicoquicClient::~PicoquicClient() {
    const auto status = close(0);
    static_cast<void>(status);
}

TransportStatus PicoquicClient::configure(const EndpointConfig& endpoint, const TlsConfig& tls) {
    endpoint_ = endpoint;
    tls_ = tls;
    state_ = ConnectionState::kIdle;
    next_bidirectional_stream_id_ = 0;
    next_unidirectional_stream_id_ = 2;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->connected = false;
        impl_->failed = false;
        impl_->disconnected = false;
        impl_->close_requested = false;
        impl_->close_error_code = 0;
        impl_->loop_ready = false;
        impl_->loop_exited = false;
        impl_->last_error.clear();
        impl_->pending_writes.clear();
        impl_->accepted_writes_waiting_for_send.clear();
    }

    return TransportStatus::success();
}

TransportStatus PicoquicClient::connect() {
    if (!endpoint_.has_value()) {
        state_ = ConnectionState::kFailed;
        return TransportStatus::failure("picoquic transport is not configured");
    }

#ifndef OPENMOQ_HAS_PICOQUIC
    state_ = ConnectionState::kFailed;
    return TransportStatus::failure("picoquic support is not enabled in this build");
#else
    state_ = ConnectionState::kConnecting;

    int is_name = 0;
    if (picoquic_get_server_address(endpoint_->host.c_str(), endpoint_->port, &impl_->server_address, &is_name) != 0) {
        state_ = ConnectionState::kFailed;
        return TransportStatus::failure("failed to resolve picoquic server address");
    }

    const bool verify_peer = !(tls_.has_value() && tls_->insecure_skip_verify);
    std::string root_certificate_file;
    {
        const TlsConfig tls_config = tls_.has_value() ? *tls_ : TlsConfig{};
        TransportStatus resolved = tlsverify::resolve_root_certificate_file(tls_config, root_certificate_file);
        if (!resolved.ok) {
            state_ = ConnectionState::kFailed;
            return resolved;
        }
    }

    const char* sni = nullptr;
    if (!endpoint_->sni.empty()) {
        sni = endpoint_->sni.c_str();
    } else if (is_name || verify_peer) {
        // When verifying the server certificate, always provide a server name:
        // picotls matches DNS names via X509_check_host() and IP literals via
        // X509_VERIFY_PARAM_set1_ip_asc(), and omits the SNI extension for IP
        // literals on the wire, so this is safe for both cases. Without a
        // server name the identity check would be silently skipped.
        sni = endpoint_->host.c_str();
    }
    const char* alpn = endpoint_->alpn.c_str();
    const uint64_t current_time = picoquic_current_time();
    trace("connect start host=" + endpoint_->host + " port=" + std::to_string(endpoint_->port) +
          " sni=" + (sni != nullptr ? sni : "<none>") + " alpn=" + alpn +
          " verify=" + (verify_peer ? ("ca=" + root_certificate_file) : "off"));

    impl_->quic =
        picoquic_create(1, nullptr, nullptr, verify_peer ? root_certificate_file.c_str() : nullptr, alpn,
                        nullptr, nullptr, nullptr, nullptr, nullptr,
                        current_time, nullptr, nullptr, nullptr, 0);
    if (impl_->quic == nullptr) {
        state_ = ConnectionState::kFailed;
        return TransportStatus::failure("failed to create picoquic context");
    }

    if (!verify_peer) {
        picoquic_set_null_verifier(impl_->quic);
    }

    impl_->cnx = picoquic_create_cnx(impl_->quic, picoquic_null_connection_id, picoquic_null_connection_id,
                                     reinterpret_cast<struct sockaddr*>(&impl_->server_address), current_time, 0,
                                     sni, alpn, 1);
    if (impl_->cnx == nullptr) {
        picoquic_free(impl_->quic);
        impl_->quic = nullptr;
        state_ = ConnectionState::kFailed;
        return TransportStatus::failure("failed to create picoquic connection");
    }

    picoquic_set_callback(impl_->cnx, client_callback, impl_.get());

    // Keep the connection alive while the application has nothing to send.
    //
    // A publisher in await-subscribe mode parks in a blocking control-stream read
    // until a SUBSCRIBE arrives, transmitting nothing meanwhile. With no keepalive
    // the peer's idle timeout expires first and tears the connection down beneath
    // that read, which surfaces to the caller as "transport close requested" after
    // roughly 30 seconds. That makes a long-running publisher unusable whenever a
    // subscriber is absent or between sessions.
    //
    // The interval must be comfortably below the negotiated idle timeout. Five
    // seconds is far enough inside any commonly negotiated value to be safe, and
    // the traffic is one PING frame, so the cost is negligible.
    constexpr std::uint64_t kKeepAliveIntervalUs = 5'000'000;
    picoquic_enable_keep_alive(impl_->cnx, kKeepAliveIntervalUs);

    if (picoquic_start_client_cnx(impl_->cnx) != 0) {
        picoquic_free(impl_->quic);
        impl_->cnx = nullptr;
        impl_->quic = nullptr;
        state_ = ConnectionState::kFailed;
        return TransportStatus::failure("failed to start picoquic client connection");
    }

    impl_->packet_loop_thread = std::thread([impl = impl_.get()] {
        trace("client packet loop thread start");
        // Disable UDP GSO in the default socket loop. Some Linux driver paths can
        // report a successful send while dropping coalesced UDP packets, which
        // looks exactly like a handshake black hole in local loopback tests.
        impl->packet_loop_return_code =
            picoquic_packet_loop(impl->quic, 0, impl->server_address.ss_family, 0, 0, 1, loop_callback, impl);
        trace("client packet loop thread exit rc=" + std::to_string(impl->packet_loop_return_code));
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->loop_exited = true;
        if (!impl->connected && !impl->failed && !impl->disconnected) {
            impl->failed = true;
            impl->last_error = "picoquic packet loop exited before handshake completed";
        }
        impl->condition.notify_all();
    });

    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        const bool completed = impl_->condition.wait_for(lock, std::chrono::seconds(5), [&] {
            return (impl_->loop_ready && (impl_->connected || impl_->failed || impl_->disconnected)) ||
                   impl_->failed || impl_->disconnected || impl_->loop_exited;
        });

        if (!completed) {
            trace("handshake wait timed out");
            impl_->failed = true;
            impl_->last_error = "timed out waiting for picoquic handshake";
            if (impl_->nonzero_send_events != 0 && impl_->nonzero_receive_events == 0) {
                impl_->last_error += " (sent " + std::to_string(impl_->total_bytes_sent) +
                                     " bytes, received 0; relay did not answer)";
            } else if (impl_->nonzero_receive_events != 0) {
                impl_->last_error += " (sent " + std::to_string(impl_->total_bytes_sent) +
                                     " bytes, received " + std::to_string(impl_->total_bytes_received) +
                                     " bytes before timeout)";
            }
        }

        if (impl_->connected) {
            state_ = ConnectionState::kConnected;
            return TransportStatus::success();
        }

        state_ = ConnectionState::kFailed;
        return TransportStatus::failure(impl_->last_error.empty() ? "picoquic handshake failed" : impl_->last_error);
    }
#endif
}

ConnectionState PicoquicClient::state() const {
    return state_;
}

TransportStatus PicoquicClient::open_stream(StreamDirection direction, std::uint64_t& stream_id) {
    if (state_ != ConnectionState::kConnected) {
        return TransportStatus::failure("transport is not connected");
    }

    if (direction == StreamDirection::kBidirectional) {
        stream_id = next_bidirectional_stream_id_;
        next_bidirectional_stream_id_ += 4;
    } else {
        stream_id = next_unidirectional_stream_id_;
        next_unidirectional_stream_id_ += 4;
    }

    return TransportStatus::success();
}

TransportStatus PicoquicClient::write_stream(std::uint64_t stream_id,
                                             std::span<const std::uint8_t> bytes,
                                             bool fin) {
    if (state_ != ConnectionState::kConnected) {
        return TransportStatus::failure("transport is not connected");
    }

#ifndef OPENMOQ_HAS_PICOQUIC
    static_cast<void>(stream_id);
    static_cast<void>(bytes);
    static_cast<void>(fin);
    return TransportStatus::failure("picoquic support is not enabled in this build");
#else
    // Back-pressure: apply_pending_operations re-queues writes that picoquic
    // temporarily rejects, so pending_writes can accumulate during congestion.
    // Cap total queued bytes and block the producer until the queue drains.
    constexpr std::size_t kMaxPendingBytes = 4ULL * 1024 * 1024;
    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        const auto has_room = [&]() {
            if (impl_->failed) {
                return true;
            }
            std::size_t queued = 0;
            for (const auto& write : impl_->pending_writes) {
                queued += write.bytes.size();
                if (queued >= kMaxPendingBytes) {
                    return false;
                }
            }
            return true;
        };
        if (!has_room()) {
            impl_->condition.wait_for(lock, std::chrono::seconds(30), [&]() { return has_room(); });
        }
        if (impl_->failed) {
            return TransportStatus::failure(impl_->last_error.empty() ? "picoquic transport failed"
                                                                       : impl_->last_error);
        }
        trace("queue write stream=" + std::to_string(stream_id) + " bytes=" + std::to_string(bytes.size()) +
              " fin=" + std::to_string(fin ? 1 : 0));
        impl_->pending_writes.push_back({
            .stream_id = stream_id,
            .bytes = std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
            .fin = fin,
            .enqueued_at = std::chrono::steady_clock::now(),
            .defer_count = 0,
        });
    }

    return TransportStatus::success();
#endif
}

TransportStatus PicoquicClient::accept_stream(StreamDirection direction,
                                              std::uint64_t& stream_id,
                                              std::chrono::milliseconds timeout) {
    if (state_ != ConnectionState::kConnected) {
        return TransportStatus::failure("transport is not connected");
    }

#ifndef OPENMOQ_HAS_PICOQUIC
    static_cast<void>(direction);
    static_cast<void>(stream_id);
    static_cast<void>(timeout);
    return TransportStatus::failure("picoquic support is not enabled in this build");
#else
    const auto matches_direction = [direction](std::uint64_t id) {
        if (direction == StreamDirection::kBidirectional) {
            return (id & 0x3ULL) == 0x1ULL;
        }
        return (id & 0x3ULL) == 0x3ULL;
    };

    std::unique_lock<std::mutex> lock(impl_->mutex);
    const bool ready = impl_->condition.wait_for(lock, timeout, [&] {
        if (impl_->failed || impl_->disconnected || impl_->close_requested) {
            return true;
        }
        for (const auto& [candidate_stream_id, ignored] : impl_->received_streams) {
            static_cast<void>(ignored);
            if (matches_direction(candidate_stream_id) &&
                !impl_->accepted_streams.contains(candidate_stream_id)) {
                return true;
            }
        }
        return false;
    });

    if (!ready) {
        return TransportStatus::failure("timed out waiting for stream data");
    }
    if (impl_->failed) {
        return TransportStatus::failure(impl_->last_error.empty() ? "picoquic transport failed"
                                                                  : impl_->last_error);
    }
    if (impl_->close_requested || impl_->disconnected) {
        return TransportStatus::failure("transport close requested");
    }

    for (const auto& [candidate_stream_id, ignored] : impl_->received_streams) {
        static_cast<void>(ignored);
        if (matches_direction(candidate_stream_id) &&
            !impl_->accepted_streams.contains(candidate_stream_id)) {
            impl_->accepted_streams.insert(candidate_stream_id);
            stream_id = candidate_stream_id;
            return TransportStatus::success();
        }
    }
    return TransportStatus::failure("no queued read for stream");
#endif
}

TransportStatus PicoquicClient::read_stream(std::uint64_t stream_id,
                                            std::vector<std::uint8_t>& bytes,
                                            bool& fin,
                                            std::chrono::milliseconds timeout) {
    if (state_ != ConnectionState::kConnected) {
        return TransportStatus::failure("transport is not connected");
    }

#ifndef OPENMOQ_HAS_PICOQUIC
    static_cast<void>(stream_id);
    static_cast<void>(bytes);
    static_cast<void>(fin);
    static_cast<void>(timeout);
    return TransportStatus::failure("picoquic support is not enabled in this build");
#else
    std::unique_lock<std::mutex> lock(impl_->mutex);
    const bool ready = impl_->condition.wait_for(lock, timeout, [&] {
        const auto it = impl_->received_streams.find(stream_id);
        return it != impl_->received_streams.end() || impl_->failed || impl_->disconnected ||
               impl_->close_requested;
    });

    if (!ready) {
        return TransportStatus::failure("timed out waiting for stream data");
    }

    const auto it = impl_->received_streams.find(stream_id);
    if (it == impl_->received_streams.end()) {
        if (impl_->close_requested || impl_->disconnected) {
            return TransportStatus::failure("transport close requested");
        }
        return TransportStatus::failure(impl_->last_error.empty() ? "stream closed before data arrived"
                                                                  : impl_->last_error);
    }

    bytes = std::move(it->second.bytes);
    fin = it->second.fin;
    impl_->received_streams.erase(it);
    return TransportStatus::success();
#endif
}

TransportStatus PicoquicClient::reset_stream(std::uint64_t stream_id,
                                              std::uint64_t error_code) {
#ifndef OPENMOQ_HAS_PICOQUIC
    static_cast<void>(stream_id);
    static_cast<void>(error_code);
    return TransportStatus::failure("picoquic support is not enabled in this build");
#else
    if (impl_ == nullptr || impl_->cnx == nullptr) {
        return TransportStatus::failure("transport is not connected");
    }
    picoquic_reset_stream(impl_->cnx, stream_id, error_code);
    return TransportStatus::success();
#endif
}

std::string PicoquicClient::connection_id() const {
#ifndef OPENMOQ_HAS_PICOQUIC
    return {};
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_ == nullptr || impl_->cnx == nullptr) {
        return {};
    }

    char buffer[513] = {};
    const picoquic_connection_id_t cid = picoquic_get_logging_cnxid(impl_->cnx);
    if (picoquic_print_connection_id_hexa(buffer, sizeof(buffer), &cid) != 0) {
        return {};
    }
    return buffer;
#endif
}

TransportStatus PicoquicClient::close(std::uint64_t application_error_code) {
    static_cast<void>(application_error_code);

#ifdef OPENMOQ_HAS_PICOQUIC
    if (impl_) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->close_requested = true;
            impl_->close_error_code = application_error_code;
        }
        trace("joining client packet loop thread");
        {
            std::lock_guard<std::mutex> join_lock(impl_->join_mutex);
            if (impl_->packet_loop_thread.joinable()) {
                impl_->packet_loop_thread.join();
            }
        }
    }

    if (impl_ && impl_->quic != nullptr) {
        picoquic_free(impl_->quic);
        impl_->quic = nullptr;
        impl_->cnx = nullptr;
    }
#endif

    state_ = ConnectionState::kClosed;
    return TransportStatus::success();
}

}  // namespace openmoq::publisher::transport
