#pragma once

// Send-side drain check shared by the raw QUIC and WebTransport clients.
//
// MoQT winds a subscription down with stream FIN -> PUBLISH_DONE -> "let
// delivery complete" (draft-ietf-moq-transport-16 section 9.15 / -18 section
// 10.11: stream state persists until the FIN is acknowledged). picoquic_close()
// does none of that: it moves the connection straight to disconnecting and the
// next packet out is CONNECTION_CLOSE, so anything still queued or in flight
// is dropped. Both clients therefore hold the close until this predicate says
// the send side is drained, or a bounded timeout expires.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

#include <picoquic.h>
#include <picoquic_internal.h>

namespace openmoq::publisher::transport {

// Fallback bound on how long close() waits for queued/in-flight stream data
// to be delivered and acknowledged when no delivery timeout was negotiated
// on the session.
inline constexpr std::chrono::milliseconds kDefaultCloseDrainTimeout{10000};

// Ceiling on the negotiated bound. delivery_timeout_ms arrives as an
// arbitrary varint chosen by the peer; without a clamp a relay advertising a
// huge value pins close() (and the thread joining the packet loop) for that
// long whenever it also stops acknowledging data.
inline constexpr std::chrono::milliseconds kMaxCloseDrainTimeout{30000};

struct CloseDrainState {
    // Bytes queued in picoquic stream send queues but not yet packetised.
    std::uint64_t unsent_bytes = 0;
    // Streams whose FIN (or reset) was requested but has not gone out yet.
    std::uint64_t pending_fin_streams = 0;
    // True when every packet carrying non-ACK frames has been acknowledged.
    bool backlog_empty = true;

    bool drained() const { return unsent_bytes == 0 && pending_fin_streams == 0 && backlog_empty; }

    std::string describe() const {
        return "unsent_bytes=" + std::to_string(unsent_bytes) +
               " pending_fin_streams=" + std::to_string(pending_fin_streams) +
               " backlog_empty=" + std::to_string(backlog_empty ? 1 : 0);
    }
};

// Must be called from the picoquic packet-loop thread (it walks connection
// state that picoquic only guards with its thread check).
inline CloseDrainState inspect_close_drain(picoquic_cnx_t* cnx) {
    CloseDrainState state;
    if (cnx == nullptr) {
        return state;
    }
    for (picoquic_stream_head_t* stream = picoquic_first_stream(cnx); stream != nullptr;
         stream = picoquic_next_stream(stream)) {
        // send_queue nodes carry (stream offset, length); picoquic drops a node
        // once it is fully packetised, so anything past sent_offset is unsent.
        std::uint64_t queued_end = stream->sent_offset;
        for (const picoquic_stream_queue_node_t* node = stream->send_queue; node != nullptr;
             node = node->next_stream_data) {
            const std::uint64_t node_end = node->offset + static_cast<std::uint64_t>(node->length);
            if (node_end > queued_end) {
                queued_end = node_end;
            }
        }
        state.unsent_bytes += queued_end - stream->sent_offset;
        if ((stream->fin_requested && !stream->fin_sent) || (stream->reset_requested && !stream->reset_sent)) {
            ++state.pending_fin_streams;
        }
    }
    state.backlog_empty = picoquic_is_cnx_backlog_empty(cnx) != 0;
    return state;
}

// Per-connection bookkeeping for a deferred close. Owned by the client Impl.
// `bound` and `deadline` are written under the Impl mutex (note_delivery_
// timeout() / close()); `timeout_logged` is only touched from the packet-loop
// thread.
struct CloseDrainTracker {
    // Largest delivery timeout negotiated on the session, or the fallback.
    std::chrono::milliseconds bound = kDefaultCloseDrainTimeout;
    bool bound_negotiated = false;
    std::chrono::steady_clock::time_point deadline{};
    bool timeout_logged = false;

    void reset() {
        bound = kDefaultCloseDrainTimeout;
        bound_negotiated = false;
        deadline = {};
        timeout_logged = false;
    }

    // draft-18 10.11: wait "the larger of" the negotiated delivery timeouts.
    void note_delivery_timeout(std::chrono::milliseconds timeout) {
        if (timeout <= std::chrono::milliseconds::zero()) {
            return;
        }
        // Parenthesised: picoquic_internal.h pulls in windows.h on MSVC, whose
        // max()/min() macros would otherwise mangle the calls.
        timeout = (std::min)(timeout, kMaxCloseDrainTimeout);
        bound = bound_negotiated ? (std::max)(bound, timeout) : timeout;
        bound_negotiated = true;
    }

    void arm() { deadline = std::chrono::steady_clock::now() + bound; }
};

// Decides whether the packet loop may issue the QUIC close now. Returns true
// when the close must keep waiting: the client still has writes it has not
// handed to picoquic (`queued_writes`), or picoquic has unsent/unacked stream
// data, and the deadline has not passed. On timeout it logs once (tagged with
// `log_prefix`) and returns false so the caller closes with whatever is left.
// Must run on the packet-loop thread; `deadline` and `bound` must be
// snapshots taken under the Impl mutex.
inline bool close_drain_should_wait(picoquic_cnx_t* cnx,
                                    std::size_t queued_writes,
                                    std::chrono::steady_clock::time_point deadline,
                                    std::chrono::milliseconds bound,
                                    bool& timeout_logged,
                                    const char* log_prefix,
                                    std::string* trace_summary) {
    if (cnx == nullptr || picoquic_get_cnx_state(cnx) >= picoquic_state_disconnecting) {
        return false;
    }
    const CloseDrainState state = inspect_close_drain(cnx);
    if (queued_writes == 0 && state.drained()) {
        if (trace_summary != nullptr) {
            *trace_summary = "close drain complete " + state.describe();
        }
        return false;
    }
    if (std::chrono::steady_clock::now() < deadline) {
        return true;
    }
    if (!timeout_logged) {
        timeout_logged = true;
        std::cerr << log_prefix << " warning: close drain timed out after " << bound.count()
                  << " ms; closing with undelivered data (queued_writes=" << queued_writes << ' ' << state.describe()
                  << ")\n";
    }
    return false;
}

}  // namespace openmoq::publisher::transport
