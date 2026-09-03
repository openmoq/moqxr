#include "openmoq/publisher/transport/moqt_session.h"
#include "openmoq/publisher/transport/moqt_control_messages.h"
#include "openmoq/publisher/cmaf_segmenter.h"
#include "openmoq/publisher/live_srt_ingest.h"
#include "openmoq/publisher/mp4_box.h"
#include "openmoq/publisher/publisher_api.h"
#include "../live_media_queue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>
#include <vector>

#if !defined(_WIN32)
#include <poll.h>
#include <unistd.h>
#endif

namespace openmoq::publisher::transport {

namespace priority_scheduler_internal {

thread_local bool count_priority_comparisons = false;
thread_local std::uint64_t priority_comparison_count = 0;
thread_local bool track_generation_availability_storage = false;
thread_local std::size_t peak_generation_availability_entries = 0;

struct CandidatePriority {
    std::uint8_t subscriber_priority = 128;
    std::uint8_t publisher_priority = 128;
    std::uint64_t request_fairness_round = 0;
    std::uint8_t group_order = 1;
    std::uint64_t group_id = 0;
    std::uint64_t subgroup_id = 0;
    std::uint64_t object_id = 0;
    std::uint64_t request_id = 0;
    std::size_t plan_index = 0;
};

bool precedes(const CandidatePriority& first, const CandidatePriority& second) {
    if (count_priority_comparisons) {
        ++priority_comparison_count;
    }
    if (first.subscriber_priority != second.subscriber_priority) {
        return first.subscriber_priority < second.subscriber_priority;
    }
    if (first.publisher_priority != second.publisher_priority) {
        return first.publisher_priority < second.publisher_priority;
    }

    // Request fairness precedes the request-local object ordering. Entries in
    // one request's frontier set use round zero; the one entry exported to the
    // session heap receives the request's current scheduling round. Keeping a
    // single unconditional tuple order makes this a strict weak ordering even
    // while stale heap entries await generation-based removal.
    if (std::tie(first.request_fairness_round, first.request_id) !=
        std::tie(second.request_fairness_round, second.request_id)) {
        return std::tie(first.request_fairness_round, first.request_id) <
               std::tie(second.request_fairness_round, second.request_id);
    }

    const std::uint8_t first_group_order = first.group_order == 2 ? 2 : 1;
    const std::uint8_t second_group_order = second.group_order == 2 ? 2 : 1;
    if (first_group_order != second_group_order) {
        return first_group_order < second_group_order;
    }
    if (first.group_id != second.group_id) {
        return first_group_order == 2 ? first.group_id > second.group_id
                                      : first.group_id < second.group_id;
    }
    if (first.subgroup_id != second.subgroup_id) {
        return first.subgroup_id < second.subgroup_id;
    }
    if (first.object_id != second.object_id) {
        return first.object_id < second.object_id;
    }
    return std::tie(first.request_id, first.plan_index) <
           std::tie(second.request_id, second.plan_index);
}

struct CandidatePriorityLess {
    bool operator()(const CandidatePriority& first,
                    const CandidatePriority& second) const {
        return precedes(first, second);
    }
};

void begin_priority_comparison_count_for_testing() {
    priority_comparison_count = 0;
    count_priority_comparisons = true;
}

std::uint64_t end_priority_comparison_count_for_testing() {
    count_priority_comparisons = false;
    return priority_comparison_count;
}

void begin_generation_availability_storage_tracking_for_testing() {
    peak_generation_availability_entries = 0;
    track_generation_availability_storage = true;
}

std::size_t end_generation_availability_storage_tracking_for_testing() {
    track_generation_availability_storage = false;
    return peak_generation_availability_entries;
}

void note_generation_availability_storage(std::size_t entries) {
    if (track_generation_availability_storage) {
        peak_generation_availability_entries =
            (std::max)(peak_generation_availability_entries, entries);
    }
}

std::mutex& publisher_priority_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::uint64_t, std::uint8_t>& publisher_priorities_for_testing() {
    static std::map<std::uint64_t, std::uint8_t> priorities;
    return priorities;
}

std::uint8_t publisher_priority_for_request(std::uint64_t request_id) {
    std::lock_guard lock(publisher_priority_mutex());
    const auto& priorities = publisher_priorities_for_testing();
    const auto it = priorities.find(request_id);
    return it == priorities.end() ? 128 : it->second;
}

void set_publisher_priority_for_testing(std::uint64_t request_id,
                                        std::uint8_t priority) {
    std::lock_guard lock(publisher_priority_mutex());
    publisher_priorities_for_testing().insert_or_assign(request_id, priority);
}

void clear_publisher_priorities_for_testing() {
    std::lock_guard lock(publisher_priority_mutex());
    publisher_priorities_for_testing().clear();
}

std::uint8_t object_transport_priority(std::uint8_t subscriber_priority,
                                       std::uint8_t publisher_priority) {
    constexpr std::uint32_t kObjectPriorityClassCount = 254;
    constexpr std::uint32_t kObjectPriorityRange = kObjectPriorityClassCount - 1;
    constexpr std::uint32_t kTupleRange = 0xffff;
    const std::uint32_t tuple_rank =
        (static_cast<std::uint32_t>(subscriber_priority) << 8U) |
        static_cast<std::uint32_t>(publisher_priority);
    return static_cast<std::uint8_t>(
        2U + ((tuple_rank * kObjectPriorityRange) / kTupleRange));
}

std::uint8_t object_transport_priority_for_testing(
    std::uint8_t subscriber_priority,
    std::uint8_t publisher_priority) {
    return object_transport_priority(subscriber_priority, publisher_priority);
}

}  // namespace priority_scheduler_internal

namespace {

// Hands the negotiated delivery timeouts to the transport so it can bound the
// close() drain with the larger timeout (draft -16 9.15 / -18 10.11).
void note_delivery_timeouts(openmoq::publisher::transport::PublisherTransport& transport,
                            const DeliveryTimeouts& delivery_timeouts) {
    const std::uint64_t close_drain_timeout_ms =
        std::max(delivery_timeouts.object_ms, delivery_timeouts.subgroup_ms);
    if (close_drain_timeout_ms != 0) {
        transport.note_delivery_timeout(std::chrono::milliseconds(close_drain_timeout_ms));
    }
}


constexpr std::uint64_t kProtocolViolationErrorCode = 0x3;
constexpr std::uint64_t kDeliveryTimeoutErrorCode = 0x02;
constexpr std::uint64_t kInvalidRequestIdErrorCode = 0x04;
constexpr std::uint64_t kKeyValueFormattingErrorCode = 0x06;
constexpr std::uint64_t kTooManyRequestsErrorCode = 0x07;

std::uint64_t advertised_max_request_id(TransportKind transport) {
    return transport == TransportKind::kWebTransport ? 128 : 100;
}

TransportStatus close_session(PublisherTransport& transport,
                              std::uint64_t error_code,
                              std::string_view message) {
    const TransportStatus close_status = transport.close(error_code);
    static_cast<void>(close_status);
    return TransportStatus::failure(message);
}

class PeerRequestIdValidator {
public:
    PeerRequestIdValidator(openmoq::publisher::DraftVersion draft,
                           std::uint64_t advertised_max_request_id)
        : draft_(draft),
          advertised_max_request_id_(advertised_max_request_id) {}

    TransportStatus validate(PublisherTransport& transport,
                             std::uint64_t request_id) {
        if (draft_ != openmoq::publisher::DraftVersion::kDraft16 &&
            draft_ != openmoq::publisher::DraftVersion::kDraft18) {
            return TransportStatus::success();
        }

        if (draft_ == openmoq::publisher::DraftVersion::kDraft16 &&
            request_id >= advertised_max_request_id_) {
            return close_session(transport,
                                 kTooManyRequestsErrorCode,
                                 "peer request id exceeds advertised maximum");
        }
        if ((request_id & 0x1ULL) == 0) {
            return close_session(transport,
                                 kInvalidRequestIdErrorCode,
                                 "peer request id has invalid parity");
        }

        if (draft_ == openmoq::publisher::DraftVersion::kDraft16) {
            if (request_id != next_draft16_request_id_) {
                return close_session(transport,
                                     kInvalidRequestIdErrorCode,
                                     "peer request id is not next in sequence");
            }
            next_draft16_request_id_ += 2;
            return TransportStatus::success();
        }

        if (!draft18_request_ids_.insert(request_id).second) {
            return close_session(transport,
                                 kInvalidRequestIdErrorCode,
                                 "peer request id is duplicated");
        }
        return TransportStatus::success();
    }

private:
    openmoq::publisher::DraftVersion draft_;
    std::uint64_t advertised_max_request_id_ = 0;
    std::uint64_t next_draft16_request_id_ = 1;
    std::set<std::uint64_t> draft18_request_ids_;
};

const NowFunction& steady_now_function() {
    static const NowFunction now = std::chrono::steady_clock::now;
    return now;
}

std::chrono::steady_clock::time_point read_now(const NowFunction& now_function) {
    return now_function ? now_function() : std::chrono::steady_clock::now();
}

std::optional<std::chrono::steady_clock::time_point> deadline_after(
    std::chrono::steady_clock::time_point start,
    std::uint64_t timeout_ms) {
    if (timeout_ms == 0) {
        return std::nullopt;
    }

    using Clock = std::chrono::steady_clock;
    using Milliseconds = std::chrono::milliseconds;
    using Rep = Milliseconds::rep;
    const auto nonnegative_start = (std::max)(start, Clock::time_point{});
    const auto remaining_ms = std::chrono::duration_cast<Milliseconds>(
        Clock::time_point::max() - nonnegative_start);
    if (timeout_ms > static_cast<std::uint64_t>(remaining_ms.count()) ||
        timeout_ms > static_cast<std::uint64_t>((std::numeric_limits<Rep>::max)())) {
        return std::chrono::steady_clock::time_point::max();
    }
    const Milliseconds timeout(static_cast<Rep>(timeout_ms));
    return start + timeout;
}

DeliveryTimeouts timeouts_for_draft(openmoq::publisher::DraftVersion draft,
                                    DeliveryTimeouts timeouts) {
    if (draft != openmoq::publisher::DraftVersion::kDraft16 &&
        draft != openmoq::publisher::DraftVersion::kDraft18) {
        return {};
    }
    if (draft != openmoq::publisher::DraftVersion::kDraft18) {
        timeouts.subgroup_ms = 0;
    }
    return timeouts;
}

std::uint64_t smaller_nonzero_timeout(std::uint64_t first,
                                      std::uint64_t second) {
    if (first == 0) {
        return second;
    }
    if (second == 0) {
        return first;
    }
    return (std::min)(first, second);
}

DeliveryTimeouts merge_delivery_timeouts(DeliveryTimeouts first,
                                         const DeliveryTimeouts& second) {
    first.object_ms = smaller_nonzero_timeout(first.object_ms, second.object_ms);
    first.subgroup_ms = smaller_nonzero_timeout(first.subgroup_ms, second.subgroup_ms);
    return first;
}

DeliveryTimeouts delivery_timeouts_for_track(
    const std::map<std::uint64_t, SubscribeMessage>& subscriptions,
    std::string_view track_name,
    DeliveryTimeouts publisher_timeouts = {}) {
    DeliveryTimeouts effective = publisher_timeouts;
    for (const auto& [request_id, subscribe] : subscriptions) {
        static_cast<void>(request_id);
        if (subscribe.track_name == track_name) {
            effective = merge_delivery_timeouts(effective, subscribe.delivery_timeouts);
        }
    }
    return effective;
}

std::uint8_t subscriber_priority_for_track(
    const std::map<std::uint64_t, SubscribeMessage>& subscriptions,
    std::string_view track_name,
    std::uint8_t fallback = 128) {
    std::optional<std::uint8_t> priority;
    for (const auto& [request_id, subscribe] : subscriptions) {
        static_cast<void>(request_id);
        if (subscribe.track_name != track_name) {
            continue;
        }
        priority = priority.has_value()
                       ? (std::min)(*priority, subscribe.subscriber_priority)
                       : subscribe.subscriber_priority;
    }
    return priority.value_or(fallback);
}

bool deadline_reached(
    std::chrono::steady_clock::time_point now,
    const std::optional<std::chrono::steady_clock::time_point>& object_deadline,
    const std::optional<std::chrono::steady_clock::time_point>& subgroup_deadline) {
    return (object_deadline.has_value() && now >= *object_deadline) ||
           (subgroup_deadline.has_value() && now >= *subgroup_deadline);
}

TransportStatus protocol_violation(PublisherTransport& transport, std::string_view message) {
    const TransportStatus close_status = transport.close(kProtocolViolationErrorCode);
    static_cast<void>(close_status);
    return TransportStatus::failure(message);
}

TransportStatus request_update_decode_failure(
    PublisherTransport& transport,
    RequestUpdateDecodeError error,
    std::string_view message) {
    if (error == RequestUpdateDecodeError::kKeyValueFormatting) {
        return close_session(transport, kKeyValueFormattingErrorCode, message);
    }
    return protocol_violation(transport, message);
}

bool trace_enabled() {
    static const bool enabled = std::getenv("OPENMOQ_PICOQUIC_TRACE") != nullptr;
    return enabled;
}

const char* trace_csv_path() {
    static const char* path = std::getenv("OPENMOQ_PICOQUIC_TRACE_CSV");
    return path;
}

bool trace_csv_enabled() {
    const char* path = trace_csv_path();
    return trace_enabled() && path != nullptr && path[0] != '\0';
}

std::chrono::steady_clock::time_point trace_epoch() {
    static const auto epoch = std::chrono::steady_clock::now();
    return epoch;
}

long long trace_elapsed_ms(std::chrono::steady_clock::time_point time_point) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time_point - trace_epoch()).count();
}

std::uint64_t next_send_seq() {
    static std::uint64_t counter = 0;
    return ++counter;
}

struct TraceCsvSink {
    std::mutex mutex;
    std::ofstream stream;
    bool header_written = false;
};

TraceCsvSink& trace_csv_sink() {
    static TraceCsvSink sink;
    return sink;
}

void trace_csv_write_row(std::initializer_list<std::string_view> fields) {
    if (!trace_csv_enabled()) {
        return;
    }

    TraceCsvSink& sink = trace_csv_sink();
    std::lock_guard<std::mutex> lock(sink.mutex);
    if (!sink.stream.is_open()) {
        sink.stream.open(trace_csv_path(), std::ios::out | std::ios::app);
        if (!sink.stream.is_open()) {
            return;
        }
    }
    if (!sink.header_written) {
        sink.stream
            << "event,send_seq,now_ms,target_ms,delta_ms,track,group_id,subgroup_id,object_id,media_time_us,"
               "stream_id,opened,payload_bytes,wire_bytes,fin\n";
        sink.header_written = true;
    }

    bool first = true;
    for (const auto field : fields) {
        if (!first) {
            sink.stream << ',';
        }
        first = false;
        sink.stream << field;
    }
    sink.stream << '\n';
    sink.stream.flush();
}

bool is_idle_subscribe_exit(std::string_view message) {
    return message == "timed out waiting for stream data" ||
           message == "no queued read for stream" ||
           message == "webtransport connection closed";
}

bool uses_peer_max_request_id(openmoq::publisher::DraftVersion draft) {
    return draft != openmoq::publisher::DraftVersion::kDraft17 &&
           draft != openmoq::publisher::DraftVersion::kDraft18;
}

bool uses_request_streams(openmoq::publisher::DraftVersion draft) {
    return draft == openmoq::publisher::DraftVersion::kDraft17 ||
           draft == openmoq::publisher::DraftVersion::kDraft18;
}

bool uses_priority_scheduler(openmoq::publisher::DraftVersion draft) {
    return draft == openmoq::publisher::DraftVersion::kDraft16 ||
           draft == openmoq::publisher::DraftVersion::kDraft18;
}

bool subscription_forwards_objects(
    openmoq::publisher::DraftVersion draft,
    std::uint8_t forward) {
    return !uses_priority_scheduler(draft) || forward != 0;
}

TransportStatus assign_request_stream_priority(
    PublisherTransport& transport,
    openmoq::publisher::DraftVersion draft,
    std::uint64_t stream_id) {
    if (draft != openmoq::publisher::DraftVersion::kDraft18) {
        return TransportStatus::success();
    }
    return transport.set_reliable_stream_priority(stream_id, 1);
}

bool decode_vi64(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint64_t& value) {
    if (offset >= bytes.size()) {
        return false;
    }
    const std::uint8_t first = bytes[offset];
    std::size_t length = 0;
    std::uint8_t prefix_mask = 0;
    if ((first & 0x80) == 0) {
        length = 1;
        prefix_mask = 0x7f;
    } else if ((first & 0xc0) == 0x80) {
        length = 2;
        prefix_mask = 0x3f;
    } else if ((first & 0xe0) == 0xc0) {
        length = 3;
        prefix_mask = 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
        length = 4;
        prefix_mask = 0x0f;
    } else {
        return false;
    }
    if (offset + length > bytes.size()) {
        return false;
    }
    value = first & prefix_mask;
    for (std::size_t i = 1; i < length; ++i) {
        value = (value << 8) | bytes[offset + i];
    }
    offset += length;
    return true;
}

bool decode_moqint(std::span<const std::uint8_t> bytes,
                   std::size_t& offset,
                   openmoq::publisher::DraftVersion draft,
                   std::uint64_t& value) {
    return uses_request_streams(draft) ? decode_vi64(bytes, offset, value) : decode_varint(bytes, offset, value);
}

std::string hex_dump(std::span<const std::uint8_t> bytes);

TransportStatus try_read_wt_session_stream(PublisherTransport& transport,
                                           bool& saw_bytes,
                                           bool& fin) {
    std::vector<std::uint8_t> session_chunk;
    bool session_fin = false;
    const TransportStatus session_status =
        transport.read_stream(0, session_chunk, session_fin, std::chrono::milliseconds(0));
    if (!session_status.ok) {
        return session_status;
    }
    saw_bytes = !session_chunk.empty();
    if (trace_enabled()) {
        std::cerr << "[moqt-session] setup fallback read stream=0 bytes=" << session_chunk.size()
                  << " fin=" << (session_fin ? 1 : 0)
                  << " bytes=[" << hex_dump(session_chunk) << "]" << std::endl;
    }
    fin = session_fin;
    return TransportStatus::success();
}

const char* control_message_type_name(std::uint64_t message_type,
                                      openmoq::publisher::DraftVersion draft) {
    switch (message_type) {
        case 0x02:
            return "SUBSCRIBE_UPDATE";
        case 0x03:
            return "SUBSCRIBE";
        case 0x04:
            return "SUBSCRIBE_OK";
        case 0x05:
            return draft == openmoq::publisher::DraftVersion::kDraft16 ? "REQUEST_ERROR" : "SUBSCRIBE_ERROR";
        case 0x06:
            return "PUBLISH_NAMESPACE";
        case 0x07:
            return "PUBLISH_NAMESPACE_OK";
        case 0x08:
            return "PUBLISH_NAMESPACE_ERROR";
        case 0x09:
            return "PUBLISH_NAMESPACE_DONE";
        case 0x0b:
            return "PUBLISH_DONE";
        case 0x11:
            return uses_request_streams(draft) ? "UNKNOWN" : "SUBSCRIBE_NAMESPACE";
        case 0x12:
            return "SUBSCRIBE_NAMESPACE_OK";
        case 0x15:
            return "MAX_REQUEST_ID";
        case 0x1d:
            return "PUBLISH";
        case 0x1e:
            return "PUBLISH_OK";
        case 0x1f:
            return "PUBLISH_ERROR";
        case 0x51:
            return draft == openmoq::publisher::DraftVersion::kDraft18 ? "SUBSCRIBE_TRACKS"
                                                                       : "STREAM_HEADER_GROUP";
        case 0x20:
            return "CLIENT_SETUP";
        case 0x21:
            return "SERVER_SETUP";
        case 0x0a:
            return "UNSUBSCRIBE";
        case 0x10:
            return "GOAWAY";
        case 0x13:
            return "SUBSCRIBE_NAMESPACE_ERROR";
        case 0x14:
            return "SUBSCRIBE_DONE";
        case 0x16:
            return "FETCH";
        case 0x17:
            return "FETCH_OK";
        case 0x18:
            return "FETCH_CANCEL";
        case 0x1a:
            return "REQUESTS_BLOCKED";
        case 0x1b:
            return "UNSUBSCRIBE_NAMESPACE";
        case 0x2f00:
            return "SETUP";
        case 0x50:
            return draft == openmoq::publisher::DraftVersion::kDraft18 ? "SUBSCRIBE_NAMESPACE" : "UNKNOWN";
        default:
            return "UNKNOWN";
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

template <typename MapLike>
std::string format_request_id_keys(const MapLike& values) {
    std::ostringstream out;
    out << '[';
    bool first = true;
    for (const auto& [request_id, ignored] : values) {
        static_cast<void>(ignored);
        if (!first) {
            out << ',';
        }
        out << request_id;
        first = false;
    }
    out << ']';
    return out.str();
}

std::string format_request_id_keys(const std::set<std::uint64_t>& values) {
    std::ostringstream out;
    out << '[';
    bool first = true;
    for (const auto request_id : values) {
        if (!first) {
            out << ',';
        }
        out << request_id;
        first = false;
    }
    out << ']';
    return out.str();
}

void trace_control_message(std::span<const std::uint8_t> message_bytes, openmoq::publisher::DraftVersion draft) {
    if (!trace_enabled()) {
        return;
    }

    std::size_t offset = 0;
    std::uint64_t message_type = 0;
    if (!decode_moqint(message_bytes, offset, draft, message_type)) {
        std::cerr << "[moqt-session] control message parse error now_ms="
                  << trace_elapsed_ms(std::chrono::steady_clock::now())
                  << " bytes=[" << hex_dump(message_bytes) << "]" << std::endl;
        return;
    }

    std::cerr << "[moqt-session] control message now_ms="
              << trace_elapsed_ms(std::chrono::steady_clock::now())
              << " type=0x" << std::hex << message_type << std::dec << " "
              << control_message_type_name(message_type, draft);

    if (message_type == 0x07) {
        PublishNamespaceOk message;
        if (decode_request_ok(message_bytes, draft, message)) {
            std::cerr << " request_id=" << message.request_id;
        }
    } else if (message_type == 0x08) {
        PublishNamespaceError message;
        if (decode_publish_namespace_error(message_bytes, message)) {
            std::cerr << " request_id=" << message.request_id << " error_code=" << message.error_code
                      << " reason=" << message.reason;
        }
    } else if (message_type == 0x05 && draft == openmoq::publisher::DraftVersion::kDraft16) {
        RequestError message;
        if (decode_request_error(message_bytes, draft, message)) {
            std::cerr << " request_id=" << message.request_id << " error_code=" << message.error_code
                      << " reason=" << message.reason;
        }
    } else if (message_type == 0x02) {
        RequestUpdateMessage message;
        if (decode_request_update_message(message_bytes, draft, message)) {
            std::cerr << " request_id=" << message.request_id
                      << " existing_request_id=";
            if (message.existing_request_id.has_value()) {
                std::cerr << *message.existing_request_id;
            } else {
                std::cerr << "stream";
            }
            if (message.forward.has_value()) {
                std::cerr << " forward=" << static_cast<unsigned int>(*message.forward);
            }
        }
    } else if ((draft == openmoq::publisher::DraftVersion::kDraft18 && message_type == 0x50) ||
               (draft != openmoq::publisher::DraftVersion::kDraft18 && message_type == 0x11)) {
        SubscribeNamespaceMessage message;
        if (decode_subscribe_namespace_message(message_bytes, draft, message)) {
            std::cerr << " request_id=" << message.request_id;
            if (!message.track_namespace_prefix.empty()) {
                std::cerr << " prefix=" << message.track_namespace_prefix.front();
            } else {
                std::cerr << " prefix=<empty>";
            }
        }
    } else if (message_type == 0x03) {
        SubscribeMessage message;
        if (decode_subscribe_message(message_bytes, draft, message)) {
            std::cerr << " request_id=" << message.request_id << " track=" << message.track_name
                      << " forward=" << static_cast<unsigned int>(message.forward)
                      << " filter_type=" << message.filter_type;
        }
    } else if (message_type == 0x51 && draft == openmoq::publisher::DraftVersion::kDraft18) {
        SubscribeTracksMessage message;
        if (decode_subscribe_tracks_message(message_bytes, draft, message)) {
            std::cerr << " request_id=" << message.request_id
                      << " forward=" << static_cast<unsigned int>(message.forward);
            if (!message.track_namespace_prefix.empty()) {
                std::cerr << " prefix=" << message.track_namespace_prefix.front();
            } else {
                std::cerr << " prefix=<empty>";
            }
        }
    } else if (message_type == 0x1e) {
        PublishOk message;
        if (decode_publish_ok(message_bytes, draft, message)) {
            std::cerr << " request_id=" << message.request_id
                      << " forward=" << static_cast<unsigned int>(message.forward)
                      << " subscriber_priority=" << static_cast<unsigned int>(message.subscriber_priority)
                      << " group_order=" << static_cast<unsigned int>(message.group_order)
                      << " filter_type=" << message.filter_type;
        }
    } else if (message_type == 0x1f) {
        PublishError message;
        if (decode_publish_error(message_bytes, draft, message)) {
            std::cerr << " request_id=" << message.request_id << " error_code=" << message.error_code
                      << " reason=" << message.reason;
        }
    }

    std::cerr << " bytes=[" << hex_dump(message_bytes) << "]" << std::endl;
}

std::span<const std::uint8_t> object_payload(const openmoq::publisher::CmsfObject& object) {
    if (!object.owned_payload.empty()) {
        return object.owned_payload;
    }
    return {};
}

std::size_t object_payload_size(const openmoq::publisher::CmsfObject& object) {
    if (!object.owned_payload.empty()) {
        return object.owned_payload.size();
    }
    return object.payload.size;
}

void trace_csv_write_media_timing(std::string_view event,
                                  std::uint64_t send_seq,
                                  long long now_ms,
                                  long long target_ms,
                                  long long delta_ms,
                                  const openmoq::publisher::CmsfObject& object) {
    if (!trace_csv_enabled() || object.kind != openmoq::publisher::CmsfObjectKind::kMedia) {
        return;
    }

    trace_csv_write_row({
        std::string(event),
        std::to_string(send_seq),
        std::to_string(now_ms),
        std::to_string(target_ms),
        std::to_string(delta_ms),
        object.track_name,
        std::to_string(object.group_id),
        std::to_string(object.subgroup_id),
        std::to_string(object.object_id),
        std::to_string(object.media_time_us),
        "",
        "",
        std::to_string(object_payload_size(object)),
        "",
        "",
    });
}

void trace_csv_write_enqueue(std::uint64_t send_seq,
                             long long now_ms,
                             const openmoq::publisher::CmsfObject& object,
                             std::uint64_t stream_id,
                             bool opened_stream,
                             std::size_t payload_bytes,
                             std::size_t wire_bytes,
                             bool fin) {
    if (!trace_csv_enabled() || object.kind != openmoq::publisher::CmsfObjectKind::kMedia) {
        return;
    }

    trace_csv_write_row({
        "enqueue",
        std::to_string(send_seq),
        std::to_string(now_ms),
        "",
        "",
        object.track_name,
        std::to_string(object.group_id),
        std::to_string(object.subgroup_id),
        std::to_string(object.object_id),
        std::to_string(object.media_time_us),
        std::to_string(stream_id),
        opened_stream ? "1" : "0",
        std::to_string(payload_bytes),
        std::to_string(wire_bytes),
        fin ? "1" : "0",
    });
}

void trace_csv_write_served(std::string_view event,
                            std::uint64_t send_seq,
                            long long now_ms,
                            const openmoq::publisher::CmsfObject& object) {
    if (!trace_csv_enabled() || object.kind != openmoq::publisher::CmsfObjectKind::kMedia) {
        return;
    }

    trace_csv_write_row({
        std::string(event),
        std::to_string(send_seq),
        std::to_string(now_ms),
        "",
        "",
        object.track_name,
        std::to_string(object.group_id),
        std::to_string(object.subgroup_id),
        std::to_string(object.object_id),
        std::to_string(object.media_time_us),
        "",
        "",
        std::to_string(object_payload_size(object)),
        "",
        "",
    });
}

void pace_until(std::chrono::steady_clock::time_point start_time,
                std::uint64_t first_media_time_us,
                const openmoq::publisher::CmsfObject& object,
                bool paced) {
    if (!paced || object.kind != openmoq::publisher::CmsfObjectKind::kMedia) {
        return;
    }
    if (object.media_time_us < first_media_time_us) {
        return;
    }
    std::this_thread::sleep_until(start_time + std::chrono::microseconds(object.media_time_us - first_media_time_us));
}

void trace_pacing_decision(std::string_view phase,
                           std::uint64_t send_seq,
                           std::chrono::steady_clock::time_point pacing_start,
                           std::uint64_t first_media_time_us,
                           const openmoq::publisher::CmsfObject& object,
                           bool paced) {
    if (!trace_enabled() || object.kind != openmoq::publisher::CmsfObjectKind::kMedia) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const std::uint64_t target_offset_us =
        object.media_time_us < first_media_time_us ? 0 : (object.media_time_us - first_media_time_us);
    const auto target_time = pacing_start + std::chrono::microseconds(target_offset_us);
    const auto now_ms = trace_elapsed_ms(now);
    const auto target_ms = trace_elapsed_ms(target_time);
    const auto delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - target_time).count();

    std::cerr << "[moqt-session] pacing " << phase
              << " send_seq=" << send_seq
              << " now_ms=" << now_ms
              << " target_ms=" << target_ms
              << " delta_ms=" << delta_ms
              << " paced=" << (paced ? 1 : 0)
              << " track=" << object.track_name
              << " group=" << object.group_id
              << " object=" << object.object_id
              << " media_time_us=" << object.media_time_us
              << std::endl;
    trace_csv_write_media_timing(phase == "before" ? "pacing_before" : "pacing_after",
                                 send_seq,
                                 now_ms,
                                 target_ms,
                                 delta_ms,
                                 object);
}

struct TrackLoopInfo {
    std::size_t group_span = 0;
    std::size_t first_loop_object_index = 0;
    bool has_loopable_objects = false;
};

struct LoopState {
    bool enabled = false;
    std::uint64_t cycle_duration_us = 0;
    std::map<std::string, TrackLoopInfo> tracks;
};

struct PublishedTrack {
    std::string name;
    std::uint64_t alias = 0;
    std::size_t largest_group_id = 0;
    std::size_t largest_object_id = 0;
    bool content_exists = false;
};

class SubgroupSenderState;

struct SubgroupFrontier {
    std::vector<std::size_t> object_indices;
    std::size_t next = 0;
};

struct ActiveSubscription {
    SubscribeMessage subscribe;
    PublishedTrack track;
    std::shared_ptr<SubgroupSenderState> sender;
    std::uint64_t request_stream_id = 0;
    std::size_t loop_cycle = 0;
    std::size_t next_object_index = 0;
    std::set<std::size_t> admitted_object_indices;
    std::map<std::size_t, std::uint64_t> send_sequence_by_object_index;
    std::map<std::pair<std::uint64_t, std::uint64_t>, SubgroupFrontier>
        subgroup_frontiers;
    std::set<priority_scheduler_internal::CandidatePriority,
             priority_scheduler_internal::CandidatePriorityLess>
        scheduler_frontiers;
    std::map<std::size_t, ObjectWriteOptions> availability_by_object_index;
    std::vector<std::uint8_t> pending_request_bytes;
    std::uint8_t publisher_priority = 128;
    std::uint64_t scheduling_round = 0;
    std::uint64_t scheduler_generation = 0;
    bool forward_paused_by_update = false;
    bool generation_lead_parked = false;
    bool completed = false;
};

struct DormantPublishedTrack {
    SubscribeMessage subscribe;
    PublishedTrack track;
};

std::string format_alias_mapping(const std::map<std::uint64_t, std::uint64_t>* request_id_by_track_alias) {
    if (request_id_by_track_alias == nullptr || request_id_by_track_alias->empty()) {
        return "[]";
    }

    std::ostringstream out;
    out << '[';
    bool first = true;
    for (const auto& [alias, request_id] : *request_id_by_track_alias) {
        if (!first) {
            out << ',';
        }
        out << alias << "->" << request_id;
        first = false;
    }
    out << ']';
    return out.str();
}

void trace_subscribe_update_state(std::string_view reason,
                                  const SubscribeUpdateMessage& subscribe_update,
                                  std::uint64_t original_request_id,
                                  std::uint64_t remapped_request_id,
                                  const std::map<std::uint64_t, SubscribeMessage>& pending_subscriptions,
                                  const std::map<std::uint64_t, ActiveSubscription>& active_subscriptions,
                                  const std::map<std::uint64_t, DormantPublishedTrack>* dormant_published_tracks,
                                  const std::set<std::uint64_t>& completed_request_ids,
                                  const std::map<std::uint64_t, std::uint64_t>* request_id_by_track_alias) {
    if (!trace_enabled()) {
        return;
    }

    std::cerr << "[moqt-session] SUBSCRIBE_UPDATE reject reason=" << reason
              << " request_id=" << subscribe_update.request_id
              << " subscription_request_id=" << original_request_id
              << " remapped_request_id=" << remapped_request_id
              << " start_group=" << subscribe_update.start_group_id
              << " start_object=" << subscribe_update.start_object_id
              << " end_group_plus_one=" << subscribe_update.end_group_plus_one
              << " forward=" << static_cast<unsigned int>(subscribe_update.forward)
              << " subscriber_priority=" << static_cast<unsigned int>(subscribe_update.subscriber_priority)
              << " pending=" << format_request_id_keys(pending_subscriptions)
              << " active=" << format_request_id_keys(active_subscriptions)
              << " dormant="
              << (dormant_published_tracks != nullptr ? format_request_id_keys(*dormant_published_tracks) : "[]")
              << " completed=" << format_request_id_keys(completed_request_ids)
              << " alias_map=" << format_alias_mapping(request_id_by_track_alias)
              << std::endl;
}

LoopState build_loop_state(const openmoq::publisher::PublishPlan& plan, bool loop_enabled) {
    LoopState state;
    state.enabled = loop_enabled;
    if (!loop_enabled) {
        return state;
    }

    for (std::size_t index = 0; index < plan.objects.size(); ++index) {
        const auto& object = plan.objects[index];
        auto& track = state.tracks[object.track_name];
        track.group_span = std::max(track.group_span, object.group_id + 1);
        if (object.kind == openmoq::publisher::CmsfObjectKind::kInitialization) {
            continue;
        }
        if (!track.has_loopable_objects) {
            track.first_loop_object_index = index;
            track.has_loopable_objects = true;
        }
        if (object.kind == openmoq::publisher::CmsfObjectKind::kMedia) {
            state.cycle_duration_us = std::max(state.cycle_duration_us, object.media_time_us + object.media_duration_us);
        }
    }

    return state;
}

const TrackLoopInfo* find_track_loop_info(const LoopState& loop_state, std::string_view track_name) {
    const auto it = loop_state.tracks.find(std::string(track_name));
    return it == loop_state.tracks.end() ? nullptr : &it->second;
}

bool track_can_loop(const LoopState& loop_state, std::string_view track_name) {
    const TrackLoopInfo* info = find_track_loop_info(loop_state, track_name);
    return info != nullptr && info->has_loopable_objects;
}

openmoq::publisher::CmsfObject make_looped_object(const openmoq::publisher::CmsfObject& object,
                                                  const LoopState& loop_state,
                                                  std::size_t loop_cycle) {
    if (!loop_state.enabled || loop_cycle == 0) {
        return object;
    }

    const TrackLoopInfo* info = find_track_loop_info(loop_state, object.track_name);
    if (info == nullptr || !info->has_loopable_objects || object.kind == openmoq::publisher::CmsfObjectKind::kInitialization) {
        return object;
    }

    openmoq::publisher::CmsfObject adjusted = object;
    adjusted.group_id += info->group_span * loop_cycle;
    adjusted.media_time_us += loop_state.cycle_duration_us * loop_cycle;
    return adjusted;
}

std::vector<PublishedTrack> build_published_tracks(const openmoq::publisher::PublishPlan& plan) {
    std::map<std::string, PublishedTrack> tracks;
    std::uint64_t next_alias = 0;
    for (const auto& object : plan.objects) {
        auto [it, inserted] = tracks.emplace(object.track_name,
                                             PublishedTrack{
                                                 .name = object.track_name,
                                                 .alias = next_alias,
                                             });
        if (inserted) {
            ++next_alias;
        }
        it->second.content_exists = true;
        if (object.group_id > it->second.largest_group_id ||
            (object.group_id == it->second.largest_group_id && object.object_id > it->second.largest_object_id)) {
            it->second.largest_group_id = object.group_id;
            it->second.largest_object_id = object.object_id;
        }
    }

    std::vector<PublishedTrack> ordered_tracks;
    ordered_tracks.reserve(tracks.size());
    for (auto& [name, track] : tracks) {
        static_cast<void>(name);
        ordered_tracks.push_back(track);
    }
    return ordered_tracks;
}

TransportStatus collect_control_acknowledgements(PublisherTransport& transport,
                                                 std::uint64_t control_stream_id,
                                                 openmoq::publisher::DraftVersion draft,
                                                 std::size_t expected_namespace_responses,
                                                 std::size_t expected_publish_responses,
                                                 std::vector<std::uint8_t>& pending_control_bytes,
                                                 std::map<std::uint64_t, PublishOk>* publish_ok_by_request_id = nullptr) {
    std::vector<std::uint8_t> buffer = std::move(pending_control_bytes);
    std::vector<std::uint8_t> deferred_messages;
    pending_control_bytes.clear();
    std::size_t namespace_responses = 0;
    std::size_t publish_responses = 0;
    std::set<std::uint64_t> seen_publish_response_ids;
    bool fin = false;

    while (namespace_responses < expected_namespace_responses || publish_responses < expected_publish_responses) {
        std::size_t consumed = 0;
        std::size_t message_size = 0;
        while (next_control_message(std::span<const std::uint8_t>(buffer).subspan(consumed), draft, message_size)) {
            const std::vector<std::uint8_t> message_bytes(buffer.begin() + static_cast<std::ptrdiff_t>(consumed),
                                                          buffer.begin() + static_cast<std::ptrdiff_t>(consumed + message_size));
            std::size_t offset = 0;
            std::uint64_t message_type = 0;
            if (!decode_moqint(message_bytes, offset, draft, message_type)) {
                return protocol_violation(transport, "failed to parse control response type");
            }
            trace_control_message(message_bytes, draft);

            bool handled = false;
            if (message_type == 0x07 && namespace_responses < expected_namespace_responses) {
                PublishNamespaceOk message;
                if (!decode_request_ok(message_bytes, draft, message)) {
                    return protocol_violation(transport,
                                              draft == openmoq::publisher::DraftVersion::kDraft16
                                                  ? "received invalid REQUEST_OK"
                                                  : "received invalid PUBLISH_NAMESPACE_OK");
                }
                ++namespace_responses;
                handled = true;
            } else if (namespace_responses < expected_namespace_responses &&
                       draft == openmoq::publisher::DraftVersion::kDraft14 &&
                       message_type == 0x08) {
                RequestError message;
                if (!decode_request_error(message_bytes, draft, message)) {
                    return protocol_violation(transport, "received invalid PUBLISH_NAMESPACE_ERROR");
                }
                return TransportStatus::failure("peer rejected namespace publish: " + message.reason);
            } else if (draft == openmoq::publisher::DraftVersion::kDraft16 &&
                       message_type == 0x05) {
                RequestError message;
                if (!decode_request_error(message_bytes, draft, message)) {
                    return protocol_violation(transport, "received invalid REQUEST_ERROR");
                }
                if (message.request_id == 0 && namespace_responses < expected_namespace_responses) {
                    return TransportStatus::failure("peer rejected namespace publish: " + message.reason);
                }
                if (message.request_id != 0 && publish_responses < expected_publish_responses) {
                    if (message.request_id % 2 != 0) {
                        return protocol_violation(transport, "received invalid publish request_id in REQUEST_ERROR");
                    }
                    if (seen_publish_response_ids.contains(message.request_id)) {
                        return TransportStatus::failure("received duplicate publish response request_id");
                    }
                    seen_publish_response_ids.insert(message.request_id);
                    return TransportStatus::failure("peer rejected track publish: " + message.reason);
                }
            } else if (message_type == 0x1e && publish_responses < expected_publish_responses) {
                PublishOk message;
                if (!decode_publish_ok(message_bytes, draft, message)) {
                    return protocol_violation(transport, "received invalid PUBLISH_OK");
                }
                note_delivery_timeouts(transport, message.delivery_timeouts);
                if (message.request_id == 0 ||
                    (draft != openmoq::publisher::DraftVersion::kDraft14 && message.request_id % 2 != 0)) {
                    return protocol_violation(transport, "received invalid request_id in PUBLISH_OK");
                }
                if (seen_publish_response_ids.contains(message.request_id)) {
                    return TransportStatus::failure("received duplicate publish response request_id");
                }
                seen_publish_response_ids.insert(message.request_id);
                if (publish_ok_by_request_id != nullptr) {
                    publish_ok_by_request_id->insert_or_assign(message.request_id, message);
                }
                ++publish_responses;
                handled = true;
            } else if (message_type == 0x1f && publish_responses < expected_publish_responses) {
                PublishError message;
                if (!decode_publish_error(message_bytes, draft, message)) {
                    return protocol_violation(transport, "received invalid PUBLISH_ERROR");
                }
                if (message.request_id == 0 ||
                    (draft != openmoq::publisher::DraftVersion::kDraft14 && message.request_id % 2 != 0)) {
                    return protocol_violation(transport, "received invalid request_id in PUBLISH_ERROR");
                }
                if (seen_publish_response_ids.contains(message.request_id)) {
                    return TransportStatus::failure("received duplicate publish response request_id");
                }
                seen_publish_response_ids.insert(message.request_id);
                return TransportStatus::failure("peer rejected track publish: " + message.reason);
            }
            if (!handled) {
                deferred_messages.insert(deferred_messages.end(), message_bytes.begin(), message_bytes.end());
            }
            consumed += message_size;
        }

        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(consumed));

        if (namespace_responses >= expected_namespace_responses && publish_responses >= expected_publish_responses) {
            break;
        }
        if (fin) {
            break;
        }

        std::vector<std::uint8_t> chunk;
        const TransportStatus status = transport.read_stream(control_stream_id, chunk, fin, std::chrono::seconds(2));
        if (!status.ok) {
            // Preserve whatever bytes we already parsed-but-deferred (or received
            // but hadn't parsed yet) before bailing out. A caller that treats this
            // failure as non-fatal -- e.g. publish_live() tolerating a missing
            // namespace acknowledgement -- still needs those bytes available to
            // its own control-message loop, so a late-arriving ack is not
            // silently dropped here.
            deferred_messages.insert(deferred_messages.end(), buffer.begin(), buffer.end());
            pending_control_bytes = std::move(deferred_messages);
            return status;
        }
        if (trace_enabled()) {
            std::cerr << "[moqt-session] control chunk fin=" << (fin ? 1 : 0) << " bytes=[" << hex_dump(chunk)
                      << "]" << std::endl;
        }
        buffer.insert(buffer.end(), chunk.begin(), chunk.end());
    }

    if (namespace_responses < expected_namespace_responses || publish_responses < expected_publish_responses) {
        if (trace_enabled() && (!buffer.empty() || !deferred_messages.empty())) {
            if (!deferred_messages.empty()) {
                std::cerr << "[moqt-session] deferred control bytes=[" << hex_dump(deferred_messages) << "]" << std::endl;
            }
            if (!buffer.empty()) {
                std::cerr << "[moqt-session] unparsed control bytes=[" << hex_dump(buffer) << "]" << std::endl;
            }
        }
        return TransportStatus::failure("timed out waiting for publish acknowledgements");
    }

    deferred_messages.insert(deferred_messages.end(), buffer.begin(), buffer.end());
    pending_control_bytes = std::move(deferred_messages);
    return TransportStatus::success();
}

TransportStatus send_request_stream_and_wait(PublisherTransport& transport,
                                             openmoq::publisher::DraftVersion draft,
                                             std::span<const std::uint8_t> request_bytes,
                                             bool expect_publish_ack,
                                             PublishOk* publish_ok = nullptr,
                                             std::uint64_t* out_stream_id = nullptr,
                                             std::vector<std::uint8_t>* trailing_bytes = nullptr) {
    std::size_t request_offset = 0;
    std::uint64_t request_type = 0;
    if (!decode_moqint(request_bytes, request_offset, draft, request_type)) {
        return protocol_violation(transport, "failed to parse request type for request stream");
    }
    std::uint64_t request_stream_id = 0;
    TransportStatus status = transport.open_stream(StreamDirection::kBidirectional, request_stream_id);
    if (!status.ok) {
        return status;
    }
    status = assign_request_stream_priority(transport, draft, request_stream_id);
    if (!status.ok) {
        return status;
    }
    status = transport.write_stream(request_stream_id, request_bytes, false);
    if (!status.ok) {
        return status;
    }

    auto set_out_stream = [&]() {
        if (out_stream_id != nullptr) {
            *out_stream_id = request_stream_id;
        }
    };

    std::vector<std::uint8_t> buffered;
    bool fin = false;
    while (true) {
        std::vector<std::uint8_t> chunk;
        status = transport.read_stream(request_stream_id, chunk, fin, std::chrono::seconds(2));
        if (!status.ok) {
            return status;
        }
        buffered.insert(buffered.end(), chunk.begin(), chunk.end());

        std::size_t consumed = 0;
        while (consumed < buffered.size()) {
            std::size_t message_size = 0;
            if (!next_control_message(std::span<const std::uint8_t>(buffered).subspan(consumed), draft, message_size)) {
                break;
            }
            const std::vector<std::uint8_t> message_bytes(
                buffered.begin() + static_cast<std::ptrdiff_t>(consumed),
                buffered.begin() + static_cast<std::ptrdiff_t>(consumed + message_size));
            consumed += message_size;

            std::size_t response_offset = 0;
            std::uint64_t response_type = 0;
            if (!decode_moqint(message_bytes, response_offset, draft, response_type)) {
                return protocol_violation(transport, "failed to parse response type on request stream");
            }

            const bool is_request_error = response_type == 0x05;
            const bool is_request_ok = response_type == 0x07;
            const bool is_publish_ok = response_type == 0x1e;
            const bool is_goaway = response_type == 0x10;

            if (is_goaway) {
                if (draft != openmoq::publisher::DraftVersion::kDraft18) {
                    return protocol_violation(transport, "request stream received GOAWAY");
                }
                const TransportStatus reset_status = transport.reset_stream(request_stream_id, 0x0);
                if (!reset_status.ok) {
                    return reset_status;
                }
                return TransportStatus::failure("request stream received GOAWAY migration");
            }

            bool response_type_allowed = false;
            if (request_type == 0x06) {  // PUBLISH_NAMESPACE
                response_type_allowed = is_request_error || is_request_ok;
            } else if (request_type == 0x1d) {  // PUBLISH
                response_type_allowed = is_request_error || is_request_ok || is_publish_ok;
            } else {
                response_type_allowed = is_request_error || is_request_ok;
            }
            if (!response_type_allowed) {
                return protocol_violation(transport, "unexpected response type for request stream");
            }

            RequestError request_error;
            if (decode_request_error(message_bytes, draft, request_error)) {
                return TransportStatus::failure("request failed: " + request_error.reason);
            }

            if (expect_publish_ack) {
                PublishOk decoded_publish_ok;
                if (decode_publish_ok(message_bytes, draft, decoded_publish_ok)) {
                    note_delivery_timeouts(transport, decoded_publish_ok.delivery_timeouts);
                    if (publish_ok != nullptr) {
                        *publish_ok = decoded_publish_ok;
                    }
                    if (trailing_bytes != nullptr) {
                        trailing_bytes->assign(
                            buffered.begin() + static_cast<std::ptrdiff_t>(consumed), buffered.end());
                    }
                    set_out_stream();
                    return TransportStatus::success();
                }
                if (draft == openmoq::publisher::DraftVersion::kDraft18) {
                    return protocol_violation(transport, "invalid draft-18 PUBLISH_OK");
                }
            }

            PublishNamespaceOk request_ok;
            if (decode_request_ok(message_bytes, draft, request_ok)) {
                if (expect_publish_ack && publish_ok != nullptr) {
                    publish_ok->forward = 1;
                    publish_ok->subscriber_priority = 128;
                    publish_ok->group_order = 0;
                    publish_ok->filter_type = 0;
                }
                if (expect_publish_ack) {
                    if (trailing_bytes != nullptr) {
                        trailing_bytes->assign(
                            buffered.begin() + static_cast<std::ptrdiff_t>(consumed), buffered.end());
                    }
                    set_out_stream();
                    return TransportStatus::success();
                }
                if (consumed > 0) {
                    buffered.erase(buffered.begin(), buffered.begin() + static_cast<std::ptrdiff_t>(consumed));
                    consumed = 0;
                }
                // Request streams are single request/single terminal response.
                // Reject additional response messages on the same stream.
                while (true) {
                    std::vector<std::uint8_t> extra_chunk;
                    bool extra_fin = false;
                    const TransportStatus extra_status = transport.read_stream(
                        request_stream_id, extra_chunk, extra_fin, std::chrono::milliseconds(0));
                    if (!extra_status.ok) {
                        if (extra_status.message == "timed out waiting for stream data" ||
                            extra_status.message == "no queued read for stream") {
                            set_out_stream();
                            return TransportStatus::success();
                        }
                        return extra_status;
                    }
                    buffered.insert(buffered.end(), extra_chunk.begin(), extra_chunk.end());
                    std::size_t extra_consumed = 0;
                    std::size_t extra_message_size = 0;
                    if (next_control_message(
                            std::span<const std::uint8_t>(buffered).subspan(extra_consumed), draft, extra_message_size)) {
                        return protocol_violation(transport, "multiple responses on request stream");
                    }
                    if (extra_fin && !buffered.empty()) {
                        return protocol_violation(transport, "trailing bytes after request response");
                    }
                    if (extra_fin) {
                        set_out_stream();
                        return TransportStatus::success();
                    }
                }
            }

            // Draft-18 request streams are strictly request/response. Any
            // non-response control message on the same request stream is a
            // protocol mismatch for the current request.
            return protocol_violation(transport, "unexpected response message on request stream");
        }

        if (consumed > 0) {
            buffered.erase(buffered.begin(), buffered.begin() + static_cast<std::ptrdiff_t>(consumed));
        }

        if (fin) {
            return TransportStatus::failure("request stream closed before acknowledgement");
        }
    }
}

bool namespace_matches(std::span<const std::string> track_namespace, std::string_view expected) {
    std::size_t component_count = 0;
    std::size_t start = 0;
    for (; start <= expected.size(); ++component_count) {
        const std::size_t slash = expected.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? expected.size() : slash;
        if (component_count >= track_namespace.size() || track_namespace[component_count] != expected.substr(start, end - start)) {
            return false;
        }
        if (slash == std::string_view::npos) {
            return component_count + 1 == track_namespace.size();
        }
        start = slash + 1;
    }
    return false;
}

bool namespace_prefix_matches(std::span<const std::string> track_namespace_prefix, std::string_view expected) {
    if (track_namespace_prefix.empty()) {
        return true;
    }

    std::size_t component_index = 0;
    std::size_t start = 0;
    while (start <= expected.size()) {
        const std::size_t slash = expected.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? expected.size() : slash;
        if (component_index >= track_namespace_prefix.size() ||
            track_namespace_prefix[component_index] != expected.substr(start, end - start)) {
            return false;
        }
        ++component_index;
        if (slash == std::string_view::npos) {
            return component_index == track_namespace_prefix.size();
        }
        start = slash + 1;
    }
    return false;
}

std::vector<std::string> split_track_namespace_components(std::string_view ns) {
    std::vector<std::string> components;
    std::size_t start = 0;
    while (start <= ns.size()) {
        const std::size_t slash = ns.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? ns.size() : slash;
        components.emplace_back(ns.substr(start, end - start));
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return components;
}

bool object_matches_filter(const openmoq::publisher::CmsfObject& object, const SubscribeMessage& subscribe) {
    switch (subscribe.filter_type) {
        case 0x01:
        case 0x02:
            return true;
        case 0x03:
            return object.group_id > subscribe.start_group_id ||
                   (object.group_id == subscribe.start_group_id && object.object_id >= subscribe.start_object_id);
        case 0x04:
            return (object.group_id > subscribe.start_group_id ||
                    (object.group_id == subscribe.start_group_id && object.object_id >= subscribe.start_object_id)) &&
                   object.group_id <= subscribe.end_group_id;
        default:
            return true;
    }
}

using LargestObjectByTrack =
    std::map<std::string, std::pair<std::size_t, std::size_t>>;

void note_largest_live_object(LargestObjectByTrack& largest_by_track,
                              std::string_view track_name,
                              std::size_t group_id,
                              std::size_t object_id) {
    auto [it, inserted] = largest_by_track.try_emplace(
        std::string(track_name), group_id, object_id);
    if (!inserted) {
        it->second = std::max(it->second,
                              std::pair{group_id, object_id});
    }
}

std::vector<std::uint8_t> encode_live_request_ok_message(
    DraftVersion draft,
    std::uint64_t request_id,
    const std::string& track_name,
    const LargestObjectByTrack& largest_by_track) {
    const auto largest_it = largest_by_track.find(track_name);
    if (largest_it == largest_by_track.end()) {
        return encode_request_ok_message(draft, request_id);
    }
    return encode_request_ok_message(draft,
                                     request_id,
                                     largest_it->second.first,
                                     largest_it->second.second);
}

bool live_object_matches_request_union(
    const openmoq::publisher::CmsfObject& object,
    openmoq::publisher::DraftVersion draft,
    const std::map<std::uint64_t, SubscribeMessage>& active_subscriptions,
    const SubscribeMessage* published_request = nullptr) {
    if (!uses_priority_scheduler(draft)) {
        return true;
    }

    bool has_matching_request = false;
    for (const auto& [request_id, subscribe] : active_subscriptions) {
        static_cast<void>(request_id);
        if (subscribe.track_name != object.track_name) {
            continue;
        }
        has_matching_request = true;
        if (subscribe.forward != 0 &&
            object_matches_filter(object, subscribe)) {
            return true;
        }
    }
    if (published_request != nullptr &&
        published_request->track_name == object.track_name) {
        has_matching_request = true;
        if (published_request->forward != 0 &&
            object_matches_filter(object, *published_request)) {
            return true;
        }
    }
    return !has_matching_request;
}

bool apply_request_update(SubscribeMessage& subscribe,
                          const RequestUpdateMessage& update) {
    if (update.object_delivery_timeout_ms.has_value()) {
        subscribe.delivery_timeouts.object_ms = *update.object_delivery_timeout_ms;
    }
    if (update.subgroup_delivery_timeout_ms.has_value()) {
        subscribe.delivery_timeouts.subgroup_ms = *update.subgroup_delivery_timeout_ms;
    }
    if (update.subscriber_priority.has_value()) {
        subscribe.subscriber_priority = *update.subscriber_priority;
    }
    if (update.forward.has_value()) {
        subscribe.forward = *update.forward;
    }
    if (update.subscription_filter.has_value()) {
        const SubscriptionFilter& filter = *update.subscription_filter;
        subscribe.filter_type = filter.filter_type;
        subscribe.start_group_id = filter.start_group_id;
        subscribe.start_object_id = filter.start_object_id;
        subscribe.end_group_id = filter.end_group_id;
    }
    return true;
}

bool request_update_renews_peer_stopped_subgroups(
    openmoq::publisher::DraftVersion draft,
    const SubscribeMessage& subscribe,
    const RequestUpdateMessage& update) {
    return draft == openmoq::publisher::DraftVersion::kDraft18 &&
           subscribe.forward == 0 &&
           update.forward == std::optional<std::uint8_t>{1};
}

bool request_update_extends_end(const SubscribeMessage& subscribe,
                                const RequestUpdateMessage& update) {
    if (!update.subscription_filter.has_value() ||
        subscribe.filter_type != 0x04) {
        return false;
    }
    const SubscriptionFilter& updated = *update.subscription_filter;
    return updated.filter_type != 0x04 ||
           updated.end_group_id > subscribe.end_group_id;
}

bool apply_subscribe_update(SubscribeMessage& subscribe, const SubscribeUpdateMessage& update) {
    const bool start_increases = update.start_group_id > subscribe.start_group_id ||
                                 (update.start_group_id == subscribe.start_group_id &&
                                  update.start_object_id >= subscribe.start_object_id);
    if (!start_increases) {
        return false;
    }

    const std::size_t updated_end_group_id =
        update.end_group_plus_one == 0 ? 0 : (update.end_group_plus_one - 1);

    if (subscribe.filter_type == 0x04) {
        if (update.end_group_plus_one == 0 || updated_end_group_id > subscribe.end_group_id) {
            return false;
        }
    }

    subscribe.start_group_id = update.start_group_id;
    subscribe.start_object_id = update.start_object_id;
    subscribe.subscriber_priority = update.subscriber_priority;
    subscribe.forward = update.forward;

    if (update.end_group_plus_one != 0) {
        subscribe.filter_type = 0x04;
        subscribe.end_group_id = updated_end_group_id;
    } else if (subscribe.filter_type != 0x04) {
        subscribe.end_group_id = 0;
    }

    return true;
}

bool decode_legacy_subscribe_update_message(std::span<const std::uint8_t> bytes,
                                            const std::map<std::string, PublishedTrack>& tracks_by_name,
                                            const std::map<std::uint64_t, SubscribeMessage>& pending_subscriptions,
                                            const std::map<std::uint64_t, std::uint64_t>* request_id_by_track_alias,
                                            SubscribeUpdateMessage& message) {
    std::size_t offset = 0;
    std::uint64_t message_type = 0;
    if (!decode_varint(bytes, offset, message_type) || message_type != 0x02 || offset + 2 > bytes.size()) {
        return false;
    }

    const std::size_t payload_length =
        (static_cast<std::size_t>(bytes[offset]) << 8) | static_cast<std::size_t>(bytes[offset + 1]);
    offset += 2;
    if (offset + payload_length != bytes.size() || payload_length != 7) {
        return false;
    }

    std::size_t payload_offset = offset;
    std::uint64_t track_alias = 0;
    std::uint64_t start_group_id = 0;
    std::uint64_t start_object_id = 0;
    if (!decode_varint(bytes, payload_offset, track_alias) ||
        !decode_varint(bytes, payload_offset, start_group_id) ||
        !decode_varint(bytes, payload_offset, start_object_id) ||
        payload_offset + 4 != bytes.size()) {
        return false;
    }

    const std::uint8_t subscriber_priority = bytes[payload_offset++];
    const std::uint8_t group_order = bytes[payload_offset++];
    const std::uint8_t parameter_or_filter = bytes[payload_offset++];
    const std::uint8_t forward = bytes[payload_offset++];
    if (group_order > 2 || forward > 1) {
        return false;
    }
    static_cast<void>(parameter_or_filter);

    for (const auto& [request_id, subscribe] : pending_subscriptions) {
        const auto track_it = tracks_by_name.find(subscribe.track_name);
        if (track_it == tracks_by_name.end() || track_it->second.alias != track_alias) {
            continue;
        }

        message.request_id = 0;
        message.subscription_request_id = request_id;
        message.start_group_id = static_cast<std::size_t>(start_group_id);
        message.start_object_id = static_cast<std::size_t>(start_object_id);
        message.end_group_plus_one = subscribe.filter_type == 0x04 ? (subscribe.end_group_id + 1) : 0;
        message.subscriber_priority = subscriber_priority;
        message.forward = forward;
        return true;
    }

    if (request_id_by_track_alias != nullptr) {
        const auto request_id_it = request_id_by_track_alias->find(track_alias);
        if (request_id_it != request_id_by_track_alias->end()) {
            message.request_id = 0;
            message.subscription_request_id = request_id_it->second;
            message.start_group_id = static_cast<std::size_t>(start_group_id);
            message.start_object_id = static_cast<std::size_t>(start_object_id);
            message.end_group_plus_one = 0;
            message.subscriber_priority = subscriber_priority;
            message.forward = forward;
            return true;
        }
    }

    return false;
}

bool decode_unsubscribe_message(std::span<const std::uint8_t> bytes,
                                openmoq::publisher::DraftVersion draft,
                                std::uint64_t& request_id) {
    std::size_t offset = 0;
    std::uint64_t message_type = 0;
    if (!decode_moqint(bytes, offset, draft, message_type) || message_type != 0x0a || offset + 2 > bytes.size()) {
        return false;
    }
    const std::size_t payload_length =
        (static_cast<std::size_t>(bytes[offset]) << 8) | static_cast<std::size_t>(bytes[offset + 1]);
    offset += 2;
    const std::size_t payload_end = offset + payload_length;
    return payload_end == bytes.size() &&
           decode_moqint(bytes, offset, draft, request_id) &&
           offset == payload_end;
}

bool find_next_matching_object_index(const openmoq::publisher::PublishPlan& plan,
                                     const SubscribeMessage& subscribe,
                                     std::size_t start_index,
                                     std::size_t& object_index) {
    for (std::size_t index = start_index; index < plan.objects.size(); ++index) {
        const auto& object = plan.objects[index];
        if (object.track_name == subscribe.track_name && object_matches_filter(object, subscribe)) {
            object_index = index;
            return true;
        }
    }
    return false;
}

bool is_final_object_in_subgroup(const openmoq::publisher::PublishPlan& plan,
                                 std::size_t object_index);

// Batch-plan objects belong to the publisher generation, not to any one
// subscription.  Record availability once per loop generation so a late
// subscriber or a filter rebuild cannot restart a delivery timer.  In paced
// mode the application availability point is the object's media pacing point;
// an unpaced plan is wholly available when the generation is exposed.
constexpr std::size_t kMaximumGenerationLeadCycles = 64;

class GenerationAvailability {
public:
    GenerationAvailability(const openmoq::publisher::PublishPlan& plan,
                           const LoopState& loop_state,
                           bool paced,
                           const NowFunction& now_function,
                           bool enabled)
        : loop_state_(loop_state),
          paced_(paced),
          now_function_(now_function),
          enabled_(enabled) {
        if (!enabled_) {
            return;
        }
        first_generation_at_ = read_now(now_function);
        priority_scheduler_internal::note_generation_availability_storage(1);
        for (const auto& object : plan.objects) {
            if (object.kind !=
                openmoq::publisher::CmsfObjectKind::kMedia) {
                continue;
            }
            first_media_time_us_ =
                first_media_time_us_.has_value()
                    ? (std::min)(*first_media_time_us_, object.media_time_us)
                    : object.media_time_us;
        }

        object_offsets_.resize(plan.objects.size());
        using SubgroupKey =
            std::tuple<std::string, std::size_t, std::uint64_t>;
        std::map<SubgroupKey,
                 std::pair<std::uint64_t, std::size_t>>
            final_object_indices;
        for (std::size_t index = 0; index < plan.objects.size(); ++index) {
            const auto& object = plan.objects[index];
            if (paced_ && first_media_time_us_.has_value() &&
                object.kind ==
                    openmoq::publisher::CmsfObjectKind::kMedia &&
                object.media_time_us >= *first_media_time_us_) {
                object_offsets_[index] = std::chrono::microseconds(
                    object.media_time_us - *first_media_time_us_);
            }
            const SubgroupKey subgroup{
                object.track_name, object.group_id, object.subgroup_id};
            const auto final_it = final_object_indices.find(subgroup);
            if (final_it == final_object_indices.end() ||
                object.object_id >= final_it->second.first) {
                final_object_indices.insert_or_assign(
                    subgroup,
                    std::pair<std::uint64_t, std::size_t>{
                        object.object_id, index});
            }
        }

        subgroup_completion_offsets_.resize(plan.objects.size());
        for (std::size_t index = 0; index < plan.objects.size(); ++index) {
            const auto& object = plan.objects[index];
            const auto final_it = final_object_indices.find(
                {object.track_name, object.group_id, object.subgroup_id});
            if (final_it != final_object_indices.end()) {
                subgroup_completion_offsets_[index] =
                    object_offsets_[final_it->second.second];
            }
        }
    }

    std::chrono::steady_clock::time_point object_available_at(
        std::size_t loop_cycle,
        std::size_t plan_index) {
        return generation_origin(loop_cycle) + object_offsets_.at(plan_index);
    }

    std::chrono::steady_clock::time_point subgroup_completed_at(
        std::size_t loop_cycle,
        std::size_t plan_index) {
        return generation_origin(loop_cycle) +
               subgroup_completion_offsets_.at(plan_index);
    }

    std::size_t initial_loop_cycle() const {
        return enabled_ ? oldest_retained_cycle_ : 0;
    }

    void retain_generations(
        const std::set<std::size_t>& needed_cycles) {
        if (!enabled_ || paced_ || needed_cycles.empty()) {
            return;
        }
        oldest_retained_cycle_ =
            (std::max)(oldest_retained_cycle_, *needed_cycles.begin());
        const std::size_t newest_retained_cycle = *needed_cycles.rbegin();
        for (auto origin_it = generation_origins_.begin();
             origin_it != generation_origins_.end();) {
            if (origin_it->first >= oldest_retained_cycle_ &&
                origin_it->first <= newest_retained_cycle) {
                ++origin_it;
                continue;
            }
            origin_it = generation_origins_.erase(origin_it);
        }
        priority_scheduler_internal::note_generation_availability_storage(
            generation_origins_.size() + 1);
    }

private:
    std::chrono::steady_clock::time_point generation_origin(
        std::size_t loop_cycle) {
        if (loop_cycle == 0) {
            return first_generation_at_;
        }
        if (paced_ && loop_state_.cycle_duration_us != 0) {
            return first_generation_at_ +
                std::chrono::microseconds(
                    loop_cycle * loop_state_.cycle_duration_us);
        }

        // Exact origins are retained while a request can still start at that
        // cycle. New and reactivated requests begin at
        // oldest_retained_cycle_, so an evicted cycle is never recreated at
        // the current wall clock or silently mapped to cycle 0's timestamp.
        const auto existing_it = generation_origins_.find(loop_cycle);
        if (existing_it != generation_origins_.end()) {
            return existing_it->second;
        }
        const auto origin_it = generation_origins_.emplace(
            loop_cycle, read_now(now_function_)).first;
        priority_scheduler_internal::note_generation_availability_storage(
            generation_origins_.size() + 1);
        return origin_it->second;
    }

    const LoopState& loop_state_;
    bool paced_ = false;
    const NowFunction& now_function_;
    bool enabled_ = false;
    std::chrono::steady_clock::time_point first_generation_at_{};
    std::optional<std::uint64_t> first_media_time_us_;
    std::vector<std::chrono::microseconds> object_offsets_;
    std::vector<std::chrono::microseconds> subgroup_completion_offsets_;
    std::map<std::size_t, std::chrono::steady_clock::time_point>
        generation_origins_;
    std::size_t oldest_retained_cycle_ = 0;
};

void rebuild_priority_frontiers(const openmoq::publisher::PublishPlan& plan,
                                ActiveSubscription& active,
                                openmoq::publisher::DraftVersion draft,
                                GenerationAvailability& availability) {
    ++active.scheduler_generation;
    active.subgroup_frontiers.clear();
    active.scheduler_frontiers.clear();
    for (std::size_t index = 0; index < plan.objects.size(); ++index) {
        const auto& object = plan.objects[index];
        if (active.admitted_object_indices.contains(index) ||
            object.track_name != active.subscribe.track_name ||
            !object_matches_filter(object, active.subscribe)) {
            continue;
        }
        active.subgroup_frontiers[{static_cast<std::uint64_t>(object.group_id),
                                   object.subgroup_id}]
            .object_indices.push_back(index);
    }

    for (auto& [key, frontier] : active.subgroup_frontiers) {
        static_cast<void>(key);
        std::sort(frontier.object_indices.begin(),
                  frontier.object_indices.end(),
                  [&](std::size_t first, std::size_t second) {
                      return std::tie(plan.objects[first].object_id, first) <
                             std::tie(plan.objects[second].object_id, second);
                  });
        if (!frontier.object_indices.empty()) {
            const std::size_t plan_index = frontier.object_indices.front();
            const auto& object = plan.objects[plan_index];
            active.scheduler_frontiers.insert({
                .subscriber_priority = active.subscribe.subscriber_priority,
                .publisher_priority = active.publisher_priority,
                .request_fairness_round = 0,
                .group_order = active.subscribe.group_order,
                .group_id = static_cast<std::uint64_t>(object.group_id),
                .subgroup_id = object.subgroup_id,
                .object_id = static_cast<std::uint64_t>(object.object_id),
                .request_id = active.subscribe.request_id,
                .plan_index = plan_index,
            });
        }
    }

    for (auto availability_it = active.availability_by_object_index.begin();
         availability_it != active.availability_by_object_index.end();) {
        const std::size_t index = availability_it->first;
        const auto& object = plan.objects[index];
        if (active.admitted_object_indices.contains(index) ||
            object.track_name != active.subscribe.track_name ||
            !object_matches_filter(object, active.subscribe)) {
            availability_it =
                active.availability_by_object_index.erase(availability_it);
        } else {
            ++availability_it;
        }
    }

    const DeliveryTimeouts timeouts =
        timeouts_for_draft(draft, active.subscribe.delivery_timeouts);
    for (const auto& [key, frontier] : active.subgroup_frontiers) {
        static_cast<void>(key);
        for (const std::size_t plan_index : frontier.object_indices) {
            active.availability_by_object_index.try_emplace(
                plan_index,
                ObjectWriteOptions{
                    .transport_priority = 255,
                    .object_deadline = deadline_after(
                        availability.object_available_at(
                            active.loop_cycle, plan_index),
                        timeouts.object_ms),
                    .subgroup_deadline = deadline_after(
                        availability.subgroup_completed_at(
                            active.loop_cycle, plan_index),
                        timeouts.subgroup_ms),
                });
        }
    }
    active.completed = active.scheduler_frontiers.empty();
}

bool advance_subscription_to_next_loop_object(const openmoq::publisher::PublishPlan& plan,
                                              const LoopState& loop_state,
                                              ActiveSubscription& active,
                                              openmoq::publisher::DraftVersion draft,
                                              GenerationAvailability& availability,
                                              std::optional<std::size_t> maximum_loop_cycle = std::nullopt,
                                              bool* generation_lead_parked = nullptr) {
    if (generation_lead_parked != nullptr) {
        *generation_lead_parked = false;
    }
    if (!loop_state.enabled || !track_can_loop(loop_state, active.track.name)) {
        return false;
    }

    const TrackLoopInfo* info = find_track_loop_info(loop_state, active.track.name);
    if (info == nullptr || !info->has_loopable_objects) {
        return false;
    }

    std::size_t next_object_index = 0;
    if (!find_next_matching_object_index(plan, active.subscribe, info->first_loop_object_index, next_object_index)) {
        return false;
    }

    if (maximum_loop_cycle.has_value() &&
        active.loop_cycle >= *maximum_loop_cycle) {
        if (generation_lead_parked != nullptr) {
            *generation_lead_parked = true;
        }
        active.completed = false;
        return true;
    }

    ++active.loop_cycle;
    active.next_object_index = next_object_index;
    active.admitted_object_indices.clear();
    active.send_sequence_by_object_index.clear();
    active.availability_by_object_index.clear();
    if (uses_priority_scheduler(draft)) {
        rebuild_priority_frontiers(plan, active, draft, availability);
    }
    active.completed = false;
    return true;
}

bool is_final_object_in_subgroup(const openmoq::publisher::PublishPlan& plan, std::size_t object_index) {
    const auto& object = plan.objects.at(object_index);
    for (std::size_t index = 0; index < plan.objects.size(); ++index) {
        if (index == object_index) {
            continue;
        }
        const auto& candidate = plan.objects[index];
        if (candidate.track_name == object.track_name && candidate.group_id == object.group_id &&
            candidate.subgroup_id == object.subgroup_id && candidate.object_id > object.object_id) {
            return false;
        }
    }
    return true;
}

bool subgroup_contains_group_largest(const openmoq::publisher::PublishPlan& plan, std::size_t object_index) {
    // The subgroup "contains the group's largest object" iff no later plan
    // object in the same track+group has a larger object_id outside this
    // subgroup. For the current baseline (every object is in subgroup 0) this
    // is equivalent to is_final_object_in_group; the helper is defined
    // explicitly so future multi-subgroup packagers don't mis-set the
    // END_OF_GROUP bit.
    const auto& object = plan.objects.at(object_index);
    for (std::size_t index = 0; index < plan.objects.size(); ++index) {
        const auto& candidate = plan.objects[index];
        if (candidate.track_name != object.track_name || candidate.group_id != object.group_id) {
            continue;
        }
        if (candidate.object_id > object.object_id && candidate.subgroup_id != object.subgroup_id) {
            return false;
        }
    }
    return true;
}

// Tracks per-subgroup open QUIC streams so that the same subgroup's objects
// are appended onto a single data stream rather than splitting across streams
// (draft-16 §2.2 / §10.4.2). Callers instantiate one of these per "sending
// context" -- per-subscription for serve_subscriptions, per-track for the
// publish paths -- and delegate the stream lifecycle to it.
class SubgroupSenderState {
public:
    enum class ServeDisposition { kAccepted, kWouldBlock, kSkipped };

    struct ServeResult {
        TransportStatus status;
        ServeDisposition disposition = ServeDisposition::kSkipped;
    };

    struct SchedulingPriority {
        std::uint8_t subscriber;
        std::uint8_t publisher;
    };

    TransportStatus serve(PublisherTransport& transport,
                          openmoq::publisher::DraftVersion draft,
                          std::uint64_t track_alias,
                          std::uint64_t send_seq,
                          const openmoq::publisher::CmsfObject& object,
                          bool subgroup_contains_group_largest,
                          bool is_final_in_subgroup,
                          std::span<const std::uint8_t> payload,
                          DeliveryTimeouts delivery_timeouts = {},
                          const NowFunction& now_function = steady_now_function(),
                          bool* published = nullptr,
                          SchedulingPriority priority = {128, 128}) {
        if (published != nullptr) {
            *published = false;
        }
        while (true) {
            const ServeResult result = try_serve(transport,
                                                 draft,
                                                 track_alias,
                                                 send_seq,
                                                 object,
                                                 subgroup_contains_group_largest,
                                                 is_final_in_subgroup,
                                                 payload,
                                                 delivery_timeouts,
                                                 now_function,
                                                 priority);
            if (!result.status.ok) {
                return result.status;
            }
            if (result.disposition != ServeDisposition::kWouldBlock) {
                if (published != nullptr) {
                    *published = result.disposition == ServeDisposition::kAccepted;
                }
                return result.status;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    ServeResult try_serve(PublisherTransport& transport,
                          openmoq::publisher::DraftVersion draft,
                          std::uint64_t track_alias,
                          std::uint64_t send_seq,
                          const openmoq::publisher::CmsfObject& object,
                          bool subgroup_contains_group_largest,
                          bool is_final_in_subgroup,
                          std::span<const std::uint8_t> payload,
                          DeliveryTimeouts delivery_timeouts,
                          const NowFunction& now_function,
                          SchedulingPriority priority,
                          std::optional<ObjectWriteOptions> prepared_options =
                              std::nullopt) {
        const Key key{static_cast<std::uint64_t>(object.group_id), object.subgroup_id};
        observe_transport_stream_state(transport);
        if (closed_subgroups_.contains(key)) {
            return {TransportStatus::success(), ServeDisposition::kSkipped};
        }

        delivery_timeouts = timeouts_for_draft(draft, delivery_timeouts);
        const std::uint8_t transport_priority =
            uses_priority_scheduler(draft)
                ? priority_scheduler_internal::object_transport_priority(
                      priority.subscriber, priority.publisher)
                : 255;
        auto pending_it = pending_objects_.find(key);
        if (pending_it == pending_objects_.end()) {
            const auto first_available_at = read_now(now_function);
            const auto object_deadline =
                prepared_options.has_value()
                    ? prepared_options->object_deadline
                    : deadline_after(first_available_at,
                                     delivery_timeouts.object_ms);
            const auto subgroup_deadline =
                prepared_options.has_value()
                    ? prepared_options->subgroup_deadline
                    : (is_final_in_subgroup
                           ? deadline_after(first_available_at,
                                            delivery_timeouts.subgroup_ms)
                           : std::nullopt);
            if (deadline_reached(read_now(now_function), object_deadline, subgroup_deadline)) {
                const auto expired_stream_it = streams_.find(key);
                if (expired_stream_it != streams_.end()) {
                    const std::uint64_t expired_stream_id =
                        expired_stream_it->second.stream_id;
                    close_subgroup(key);
                    const TransportStatus reset_status = transport.reset_stream(
                        expired_stream_id, kDeliveryTimeoutErrorCode);
                    return {reset_status, ServeDisposition::kSkipped};
                }
                closed_subgroups_.insert(key);
                return {TransportStatus::success(), ServeDisposition::kSkipped};
            }

            auto stream_it = streams_.find(key);
            std::optional<std::uint64_t> previous_object_id;
            std::uint64_t stream_id = 0;
            std::vector<std::uint8_t> wire_bytes;
            const bool opened_stream = stream_it == streams_.end();
            if (opened_stream) {
                TransportStatus status =
                    transport.open_stream(StreamDirection::kUnidirectional, stream_id);
                if (!status.ok) {
                    return {status, ServeDisposition::kSkipped};
                }
                wire_bytes = encode_subgroup_header(
                    draft, track_alias, static_cast<std::uint64_t>(object.group_id),
                    object.subgroup_id, subgroup_contains_group_largest);
                ++stream_count_;
                stream_it = streams_.emplace(
                    key, OpenStream{stream_id, std::nullopt, transport_priority}).first;
                stream_keys_.insert_or_assign(stream_id, key);
            } else {
                stream_id = stream_it->second.stream_id;
                previous_object_id = stream_it->second.last_object_id;
                stream_it->second.transport_priority = transport_priority;
            }

            std::vector<std::uint8_t> object_bytes =
                encode_subgroup_object(draft, previous_object_id, object.object_id, payload);
            wire_bytes.insert(wire_bytes.end(), object_bytes.begin(), object_bytes.end());
            if (!payload.empty() &&
                (object_bytes.size() < payload.size() ||
                 !std::equal(payload.begin(), payload.end(),
                             object_bytes.end() - static_cast<std::ptrdiff_t>(payload.size())))) {
                return {TransportStatus::failure("encoded subgroup object payload mismatch"),
                        ServeDisposition::kSkipped};
            }

            pending_it = pending_objects_.emplace(
                key,
                PendingObject{
                    .object_id = object.object_id,
                    .wire_bytes = std::move(wire_bytes),
                    .fin = is_final_in_subgroup,
                    .options = ObjectWriteOptions{
                        .transport_priority = stream_it->second.transport_priority,
                        .object_deadline = object_deadline,
                        .subgroup_deadline = subgroup_deadline,
                    },
                }).first;

            if (trace_enabled()) {
                const auto now_ms = trace_elapsed_ms(std::chrono::steady_clock::now());
                std::cerr << "[moqt-session] enqueue object stream=" << stream_id
                          << " send_seq=" << send_seq
                          << " now_ms=" << now_ms
                          << " opened=" << (opened_stream ? 1 : 0)
                          << " track=" << object.track_name
                          << " group=" << object.group_id
                          << " subgroup=" << object.subgroup_id
                          << " object=" << object.object_id
                          << " payload_bytes=" << payload.size()
                          << " wire_bytes=" << pending_it->second.wire_bytes.size()
                          << " fin=" << (is_final_in_subgroup ? 1 : 0)
                          << std::endl;
                trace_csv_write_enqueue(send_seq,
                                        now_ms,
                                        object,
                                        stream_id,
                                        opened_stream,
                                        payload.size(),
                                        pending_it->second.wire_bytes.size(),
                                        is_final_in_subgroup);
            }
        } else if (pending_it->second.object_id != object.object_id) {
            return {TransportStatus::failure(
                        "attempted a later object while its subgroup predecessor is blocked"),
                    ServeDisposition::kSkipped};
        }

        auto stream_it = streams_.find(key);
        if (stream_it == streams_.end()) {
            pending_objects_.erase(key);
            return {TransportStatus::success(), ServeDisposition::kSkipped};
        }
        stream_it->second.transport_priority = transport_priority;
        pending_it->second.options.transport_priority = transport_priority;
        const std::uint64_t stream_id = stream_it->second.stream_id;
        const ObjectWriteOptions options = pending_it->second.options;
        if (deadline_reached(read_now(now_function),
                             options.object_deadline,
                             options.subgroup_deadline)) {
            close_subgroup(key);
            const TransportStatus reset_status =
                transport.reset_stream(stream_id, kDeliveryTimeoutErrorCode);
            return {reset_status, ServeDisposition::kSkipped};
        }

        const ObjectWriteResult result = transport.try_write_object(
            stream_id,
            pending_it->second.wire_bytes,
            pending_it->second.fin,
            options);
        if (result.disposition == ObjectWriteDisposition::kWouldBlock) {
            return {TransportStatus::success(), ServeDisposition::kWouldBlock};
        }
        if (result.disposition == ObjectWriteDisposition::kFailed) {
            observe_transport_stream_state(transport);
            if (closed_subgroups_.contains(key)) {
                return {TransportStatus::success(), ServeDisposition::kSkipped};
            }
            return {TransportStatus::failure(
                        result.message.empty() ? "transport object admission failed"
                                               : result.message),
                    ServeDisposition::kSkipped};
        }

        const std::uint64_t object_id = pending_it->second.object_id;
        const bool fin = pending_it->second.fin;
        pending_objects_.erase(pending_it);
        if (fin) {
            close_subgroup(key);
        } else {
            stream_it = streams_.find(key);
            if (stream_it != streams_.end()) {
                stream_it->second.last_object_id = object_id;
            }
        }
        return {TransportStatus::success(), ServeDisposition::kAccepted};
    }

    std::uint64_t stream_count() const { return stream_count_; }

    // REQUEST_UPDATE can remove an object while it is still owned entirely by
    // this sender after a would-block result. No transport bytes were accepted,
    // so discard only that pending record and retain any earlier accepted
    // objects on the subgroup stream.
    void discard_unadmitted(std::uint64_t group_id,
                            std::uint64_t subgroup_id,
                            std::uint64_t object_id) {
        const Key key{group_id, subgroup_id};
        const auto pending_it = pending_objects_.find(key);
        if (pending_it == pending_objects_.end() ||
            pending_it->second.object_id != object_id) {
            return;
        }
        pending_objects_.erase(pending_it);

        const auto stream_it = streams_.find(key);
        if (stream_it == streams_.end() || stream_it->second.last_object_id.has_value()) {
            return;
        }
        stream_keys_.erase(stream_it->second.stream_id);
        streams_.erase(stream_it);
        if (stream_count_ != 0) {
            --stream_count_;
        }
    }

    void refresh_unadmitted_deadlines(
        std::uint64_t group_id,
        std::uint64_t subgroup_id,
        std::uint64_t object_id,
        const std::optional<std::chrono::steady_clock::time_point>&
            object_deadline,
        const std::optional<std::chrono::steady_clock::time_point>&
            subgroup_deadline) {
        const Key key{group_id, subgroup_id};
        const auto pending_it = pending_objects_.find(key);
        if (pending_it == pending_objects_.end() ||
            pending_it->second.object_id != object_id) {
            return;
        }
        pending_it->second.options.object_deadline = object_deadline;
        pending_it->second.options.subgroup_deadline = subgroup_deadline;
    }

    void renew_peer_stopped_subgroups(PublisherTransport& transport) {
        observe_transport_stream_state(transport);
        for (const Key& key : peer_stopped_subgroups_) {
            closed_subgroups_.erase(key);
        }
        peer_stopped_subgroups_.clear();
    }

    TransportStatus cancel_all_open_subgroups(PublisherTransport& transport,
                                              std::uint64_t error_code) {
        observe_transport_stream_state(transport);
        const std::vector<std::pair<Key, OpenStream>> open_streams(
            streams_.begin(), streams_.end());
        TransportStatus first_failure = TransportStatus::success();
        for (const auto& [key, stream] : open_streams) {
            close_subgroup(key);
            const TransportStatus reset_status =
                transport.reset_stream(stream.stream_id, error_code);
            if (!reset_status.ok && first_failure.ok) {
                first_failure = reset_status;
            }
        }
        return first_failure;
    }

    // FIN all currently open streams (used when transitioning to a new group).
    TransportStatus finish_group(PublisherTransport& transport) {
        return finish_group(transport,
                            openmoq::publisher::DraftVersion::kDraft14,
                            {},
                            steady_now_function());
    }

    TransportStatus finish_group(PublisherTransport& transport,
                                 openmoq::publisher::DraftVersion draft,
                                 DeliveryTimeouts delivery_timeouts,
                                 const NowFunction& now_function,
                                 const std::function<TransportStatus()>&
                                     resource_status = {}) {
        delivery_timeouts = timeouts_for_draft(draft, delivery_timeouts);
        const auto subgroup_deadline =
            deadline_after(read_now(now_function), delivery_timeouts.subgroup_ms);
        observe_transport_stream_state(transport);
        const std::vector<std::pair<Key, OpenStream>> open_streams(streams_.begin(), streams_.end());
        for (const auto& [key, stream] : open_streams) {
            if (closed_subgroups_.contains(key)) {
                continue;
            }
            pending_objects_.erase(key);

            while (true) {
                if (resource_status) {
                    const TransportStatus status = resource_status();
                    if (!status.ok) {
                        return status;
                    }
                }
                if (deadline_reached(read_now(now_function), std::nullopt, subgroup_deadline)) {
                    const std::uint64_t stream_id = stream.stream_id;
                    close_subgroup(key);
                    const TransportStatus status =
                        transport.reset_stream(stream_id, kDeliveryTimeoutErrorCode);
                    if (!status.ok) {
                        return status;
                    }
                    break;
                }
                ObjectWriteResult result = transport.try_write_object(
                    stream.stream_id,
                    {},
                    true,
                    ObjectWriteOptions{
                        .transport_priority = stream.transport_priority,
                        .object_deadline = std::nullopt,
                        .subgroup_deadline = subgroup_deadline,
                    });
                if (result.disposition == ObjectWriteDisposition::kFailed) {
                    observe_transport_stream_state(transport);
                    if (closed_subgroups_.contains(key)) {
                        break;
                    }
                    return TransportStatus::failure(
                        result.message.empty() ? "transport subgroup FIN admission failed"
                                               : result.message);
                }
                if (result.disposition == ObjectWriteDisposition::kAccepted) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            close_subgroup(key);
        }
        streams_.clear();
        stream_keys_.clear();
        pending_objects_.clear();
        return TransportStatus::success();
    }

private:
    struct Key {
        std::uint64_t group_id;
        std::uint64_t subgroup_id;
        friend bool operator<(const Key& a, const Key& b) {
            return std::tie(a.group_id, a.subgroup_id) < std::tie(b.group_id, b.subgroup_id);
        }
    };
    struct OpenStream {
        std::uint64_t stream_id;
        std::optional<std::uint64_t> last_object_id;
        std::uint8_t transport_priority;
    };
    struct PendingObject {
        std::uint64_t object_id;
        std::vector<std::uint8_t> wire_bytes;
        bool fin;
        ObjectWriteOptions options;
    };

    void close_subgroup(const Key& key) {
        const auto stream_it = streams_.find(key);
        if (stream_it != streams_.end()) {
            stream_keys_.erase(stream_it->second.stream_id);
            streams_.erase(stream_it);
        }
        pending_objects_.erase(key);
        closed_subgroups_.insert(key);
    }

    void observe_transport_stream_state(PublisherTransport& transport) {
        for (auto it = stream_keys_.begin(); it != stream_keys_.end();) {
            const bool expired = transport.media_stream_expired(it->first);
            const bool peer_stopped =
                !expired && transport.media_stream_peer_stopped(it->first);
            if (!expired && !peer_stopped) {
                ++it;
                continue;
            }
            const std::uint64_t stream_id = it->first;
            const Key key = it->second;
            const auto stream_it = streams_.find(key);
            if (stream_it != streams_.end() && stream_it->second.stream_id == stream_id) {
                streams_.erase(stream_it);
            }
            pending_objects_.erase(key);
            closed_subgroups_.insert(key);
            if (peer_stopped) {
                peer_stopped_subgroups_.insert(key);
            }
            it = stream_keys_.erase(it);
        }
    }

    std::map<Key, OpenStream> streams_;
    std::map<Key, PendingObject> pending_objects_;
    std::map<std::uint64_t, Key> stream_keys_;
    std::set<Key> closed_subgroups_;
    std::set<Key> peer_stopped_subgroups_;
    std::uint64_t stream_count_ = 0;
};

std::uint64_t live_srt_resource_reset_code(
    openmoq::publisher::DraftVersion draft) {
    return uses_request_streams(draft) ? 0x05 : 0x01;
}

struct LiveSrtSchedulingOptions {
    DeliveryTimeouts delivery_timeouts;
    SubgroupSenderState::SchedulingPriority priority;
};

openmoq::publisher::LiveMediaAdmission admit_live_srt_fragment(
    LiveSrtQueueAdapter& queue,
    openmoq::publisher::MediaFragment fragment,
    const NowFunction& now_function) {
    const auto available_at = read_now(now_function);
    const auto available_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            available_at.time_since_epoch())
            .count();
    fragment.creation_time_us =
        available_us <= 0
            ? 0
            : static_cast<std::uint64_t>(available_us);
    return queue.admit(std::move(fragment));
}

std::chrono::steady_clock::time_point live_srt_fragment_available_at(
    const openmoq::publisher::MediaFragment& fragment) {
    return std::chrono::steady_clock::time_point{
        std::chrono::microseconds(fragment.creation_time_us)};
}

TransportStatus append_control_stream_bytes_nonblocking(
    PublisherTransport& transport,
    std::uint64_t control_stream_id,
    std::vector<std::uint8_t>& pending_bytes,
    bool* stream_fin = nullptr) {
    std::vector<std::uint8_t> chunk;
    bool fin = false;
    const TransportStatus status = transport.read_stream(
        control_stream_id, chunk, fin, std::chrono::milliseconds(0));
    if (!status.ok) {
        if (status.message == "timed out waiting for stream data") {
            return TransportStatus::success();
        }
        return status;
    }
    pending_bytes.insert(
        pending_bytes.end(), chunk.begin(), chunk.end());
    if (stream_fin != nullptr) {
        *stream_fin = fin;
    }
    return TransportStatus::success();
}

bool is_live_srt_resource_limit(const TransportStatus& status) {
    return !status.ok &&
           status.message == kLiveSrtResourceLimitDiagnostic;
}

TransportStatus terminate_live_srt_resource_limit(
    PublisherTransport& transport,
    openmoq::publisher::DraftVersion draft,
    std::uint64_t control_stream_id,
    const std::map<std::uint64_t, SubscribeMessage>& active_subscriptions,
    const std::map<std::uint64_t, std::uint64_t>& subscription_stream_ids,
    std::map<std::string, SubgroupSenderState>& sender_by_track,
    TransportStatus resource_status) {
    const std::uint64_t reset_code = live_srt_resource_reset_code(draft);
    bool all_subgroups_reset = true;
    for (auto& [track_name, sender] : sender_by_track) {
        static_cast<void>(track_name);
        const TransportStatus reset_status =
            sender.cancel_all_open_subgroups(transport, reset_code);
        if (!reset_status.ok) {
            all_subgroups_reset = false;
            std::cerr << "[moqt-session] warning: failed to reset live SRT "
                         "subgroup after resource overflow: "
                      << reset_status.message << '\n';
        }
    }

    // PUBLISH_DONE promises that all streams belonging to this request have
    // been closed. If any required reset failed, keep the stable resource
    // diagnostic but do not make that false wire-level promise. Every sender
    // above is still attempted so one failed reset cannot strand later ones.
    if (!all_subgroups_reset) {
        return resource_status;
    }

    const std::uint64_t too_far_behind =
        draft == openmoq::publisher::DraftVersion::kDraft18 ? 0x05 : 0x06;
    for (const auto& [request_id, subscribe] : active_subscriptions) {
        if (subscribe.track_name == "catalog") {
            continue;
        }
        const auto sender_it = sender_by_track.find(subscribe.track_name);
        if (sender_it == sender_by_track.end()) {
            continue;
        }
        std::uint64_t response_stream_id = control_stream_id;
        if (uses_request_streams(draft)) {
            const auto stream_it = subscription_stream_ids.find(request_id);
            if (stream_it == subscription_stream_ids.end()) {
                std::cerr << "[moqt-session] warning: missing retained "
                             "SUBSCRIBE stream for resource PUBLISH_DONE\n";
                continue;
            }
            response_stream_id = stream_it->second;
        }
        const TransportStatus done_status = transport.write_stream(
            response_stream_id,
            encode_publish_done_message(
                draft,
                request_id,
                sender_it->second.stream_count(),
                too_far_behind,
                "subscriber exceeded publisher resource limit"),
            uses_request_streams(draft));
        if (!done_status.ok) {
            std::cerr << "[moqt-session] warning: failed to send live SRT "
                         "resource PUBLISH_DONE: "
                      << done_status.message << '\n';
        }
    }
    return resource_status;
}

TransportStatus serve_live_srt_object_until_admitted(
    PublisherTransport& transport,
    LiveSrtQueueAdapter& queue,
    SubgroupSenderState& sender,
    openmoq::publisher::DraftVersion draft,
    std::uint64_t track_alias,
    std::uint64_t send_seq,
    const openmoq::publisher::CmsfObject& object,
    std::span<const std::uint8_t> payload,
    std::chrono::steady_clock::time_point object_available_at,
    const NowFunction& now_function,
    const std::function<LiveSrtSchedulingOptions()>& scheduling_options,
    const std::function<TransportStatus()>& poll_control,
    const std::function<bool()>& remains_eligible,
    bool* published) {
    if (published != nullptr) {
        *published = false;
    }
    const auto resource_status = [&]() {
        return queue.publishing_status(TransportStatus::success());
    };
    while (true) {
        const LiveSrtSchedulingOptions current = scheduling_options();
        const DeliveryTimeouts effective_timeouts =
            timeouts_for_draft(draft, current.delivery_timeouts);
        const ObjectWriteOptions prepared_options{
            .transport_priority = 255,
            .object_deadline = deadline_after(
                object_available_at, effective_timeouts.object_ms),
            .subgroup_deadline = std::nullopt,
        };
        sender.refresh_unadmitted_deadlines(
            object.group_id,
            object.subgroup_id,
            object.object_id,
            prepared_options.object_deadline,
            prepared_options.subgroup_deadline);
        const SubgroupSenderState::ServeResult result = sender.try_serve(
            transport,
            draft,
            track_alias,
            send_seq,
            object,
            true,
            false,
            payload,
            current.delivery_timeouts,
            now_function,
            current.priority,
            prepared_options);
        if (!result.status.ok) {
            return result.status;
        }
        if (result.disposition !=
            SubgroupSenderState::ServeDisposition::kWouldBlock) {
            if (published != nullptr) {
                *published = result.disposition ==
                             SubgroupSenderState::ServeDisposition::kAccepted;
            }
            const TransportStatus status = resource_status();
            if (!status.ok) {
                return status;
            }
            return result.status;
        }

        TransportStatus status = resource_status();
        if (!status.ok) {
            return status;
        }
        status = poll_control();
        if (!status.ok) {
            return status;
        }
        status = resource_status();
        if (!status.ok) {
            return status;
        }
        if (!remains_eligible()) {
            sender.discard_unadmitted(
                object.group_id, object.subgroup_id, object.object_id);
            return TransportStatus::success();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

class LiveSrtPublishFlow {
public:
    using ProcessControlMessages =
        std::function<std::pair<TransportStatus, std::size_t>()>;

    LiveSrtPublishFlow(
        PublisherTransport& transport,
        openmoq::publisher::DraftVersion draft,
        std::uint64_t control_stream_id,
        LiveSrtQueueAdapter& queue,
        std::vector<std::uint8_t>& pending_control_bytes,
        bool& control_fin,
        ProcessControlMessages& process_control_messages,
        std::map<std::uint64_t, SubscribeMessage>& active_subscriptions,
        std::map<std::uint64_t, std::uint64_t>& subscription_stream_ids,
        std::map<std::string, SubgroupSenderState>& sender_by_track,
        const NowFunction& now_function)
        : transport_(transport),
          draft_(draft),
          control_stream_id_(control_stream_id),
          queue_(queue),
          pending_control_bytes_(pending_control_bytes),
          control_fin_(control_fin),
          process_control_messages_(process_control_messages),
          active_subscriptions_(active_subscriptions),
          subscription_stream_ids_(subscription_stream_ids),
          sender_by_track_(sender_by_track),
          now_function_(now_function) {}

    TransportStatus publishing_status(TransportStatus status) const {
        return queue_.publishing_status(std::move(status));
    }

    TransportStatus poll_control() {
        if (!process_control_messages_) {
            return TransportStatus::failure(
                "live SRT control poll unavailable");
        }
        if (draft_ == openmoq::publisher::DraftVersion::kDraft16) {
            const TransportStatus read_status =
                append_control_stream_bytes_nonblocking(
                    transport_,
                    control_stream_id_,
                    pending_control_bytes_,
                    &control_fin_);
            if (!read_status.ok) {
                return read_status;
            }
        }
        auto [control_status, new_subscriptions] =
            process_control_messages_();
        static_cast<void>(new_subscriptions);
        return control_status;
    }

    TransportStatus serve_object(
        SubgroupSenderState& sender,
        std::uint64_t track_alias,
        std::uint64_t send_seq,
        const openmoq::publisher::CmsfObject& object,
        std::span<const std::uint8_t> payload,
        std::chrono::steady_clock::time_point object_available_at,
        const std::function<LiveSrtSchedulingOptions()>&
            scheduling_options,
        const std::function<bool()>& remains_eligible,
        bool* published) {
        return serve_live_srt_object_until_admitted(
            transport_,
            queue_,
            sender,
            draft_,
            track_alias,
            send_seq,
            object,
            payload,
            object_available_at,
            now_function_,
            scheduling_options,
            [this]() { return poll_control(); },
            remains_eligible,
            published);
    }

    TransportStatus finish_group(
        SubgroupSenderState& sender,
        DeliveryTimeouts delivery_timeouts,
        const std::function<void()>& before_resource_check = {}) {
        return sender.finish_group(
            transport_,
            draft_,
            delivery_timeouts,
            now_function_,
            [&]() {
                if (before_resource_check) {
                    before_resource_check();
                }
                return publishing_status(TransportStatus::success());
            });
    }

    TransportStatus terminate_resource(TransportStatus status) {
        return terminate_live_srt_resource_limit(
            transport_,
            draft_,
            control_stream_id_,
            active_subscriptions_,
            subscription_stream_ids_,
            sender_by_track_,
            std::move(status));
    }

private:
    PublisherTransport& transport_;
    openmoq::publisher::DraftVersion draft_;
    std::uint64_t control_stream_id_;
    LiveSrtQueueAdapter& queue_;
    std::vector<std::uint8_t>& pending_control_bytes_;
    bool& control_fin_;
    ProcessControlMessages& process_control_messages_;
    std::map<std::uint64_t, SubscribeMessage>& active_subscriptions_;
    std::map<std::uint64_t, std::uint64_t>& subscription_stream_ids_;
    std::map<std::string, SubgroupSenderState>& sender_by_track_;
    const NowFunction& now_function_;
};

TransportStatus finalize_subscription(PublisherTransport& transport,
                                      openmoq::publisher::DraftVersion draft,
                                      std::uint64_t response_stream_id,
                                      std::uint64_t request_id,
                                      std::uint64_t stream_count,
                                      std::set<std::uint64_t>& completed_request_ids,
                                      std::uint64_t status_code = 0x2,
                                      std::string_view reason = {}) {
    const TransportStatus write_status = transport.write_stream(
        response_stream_id,
        encode_publish_done_message(
            draft, request_id, stream_count, status_code, reason),
        false);
    if (!write_status.ok) {
        return write_status;
    }
    completed_request_ids.insert(request_id);
    return TransportStatus::success();
}

TransportStatus write_publish_done_for_request(PublisherTransport& transport,
                                               openmoq::publisher::DraftVersion draft,
                                               std::uint64_t control_stream_id,
                                               const std::map<std::uint64_t, std::uint64_t>& publish_stream_ids,
                                               std::uint64_t request_id,
                                               std::uint64_t stream_count,
                                               std::uint64_t status_code = 0x2,
                                               std::string_view reason = {}) {
    std::uint64_t response_stream_id = control_stream_id;
    if (uses_request_streams(draft)) {
        const auto stream_it = publish_stream_ids.find(request_id);
        if (stream_it == publish_stream_ids.end()) {
            return TransportStatus::failure("missing draft-18 publish request stream");
        }
        response_stream_id = stream_it->second;
    }
    return transport.write_stream(response_stream_id,
                                  encode_publish_done_message(draft, request_id, stream_count, status_code, reason),
                                  uses_request_streams(draft));
}

TransportStatus write_namespace_done_for_request(PublisherTransport& transport,
                                                 openmoq::publisher::DraftVersion draft,
                                                 std::uint64_t control_stream_id,
                                                 std::uint64_t namespace_stream_id,
                                                 NamespaceMessage namespace_message) {
    const std::uint64_t response_stream_id =
        uses_request_streams(draft) ? namespace_stream_id : control_stream_id;
    if (uses_request_streams(draft)) {
        return transport.reset_stream(response_stream_id, 0x0);
    }
    return transport.write_stream(response_stream_id, encode_publish_namespace_done_message(namespace_message), false);
}

TransportStatus publish_selected_tracks(PublisherTransport& transport,
                                        std::uint64_t control_stream_id,
                                        const openmoq::publisher::PublishPlan& plan,
                                        const LoopState& loop_state,
                                        std::span<const PublishedTrack> tracks,
                                        std::uint64_t peer_max_request_id,
                                        std::string_view track_namespace,
                                        const std::optional<std::vector<std::uint8_t>>& authorization_token,
                                        bool paced,
                                        std::vector<std::uint8_t>& pending_control_bytes,
                                        std::map<std::uint64_t, std::uint64_t>& publish_stream_ids,
                                        std::map<std::uint64_t, DormantPublishedTrack>* dormant_published_tracks,
                                        std::map<std::uint64_t, std::uint64_t>* request_id_by_track_alias,
                                        const NowFunction& now_function,
                                        std::uint64_t first_request_id = 2);

using PublishedObjectSink = std::function<void(const std::string&, std::uint64_t, std::size_t)>;

TransportStatus read_request_stream_message(PublisherTransport& transport,
                                            std::uint64_t request_stream_id,
                                            openmoq::publisher::DraftVersion draft,
                                            std::chrono::milliseconds timeout,
                                            std::vector<std::uint8_t>& message_bytes,
                                            std::vector<std::uint8_t>* trailing_bytes = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    message_bytes.clear();

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = now < deadline
                                   ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                                   : std::chrono::milliseconds(0);
        std::vector<std::uint8_t> chunk;
        bool fin = false;
        const TransportStatus read_status =
            transport.read_stream(request_stream_id, chunk, fin, remaining);
        if (!read_status.ok) {
            return read_status;
        }
        message_bytes.insert(message_bytes.end(), chunk.begin(), chunk.end());

        std::size_t message_size = 0;
        if (next_control_message(message_bytes, draft, message_size)) {
            if (trailing_bytes != nullptr) {
                trailing_bytes->assign(
                    message_bytes.begin() +
                        static_cast<std::ptrdiff_t>(message_size),
                    message_bytes.end());
            }
            message_bytes.resize(message_size);
            return TransportStatus::success();
        }
        if (fin) {
            return protocol_violation(transport, "request stream closed before a complete message");
        }
    }
}

template <typename ApplyUpdate>
TransportStatus poll_retained_subscribe_request_updates(
    PublisherTransport& transport,
    openmoq::publisher::DraftVersion draft,
    const std::map<std::uint64_t, SubscribeMessage>& active_subscriptions,
    const std::map<std::uint64_t, std::uint64_t>& request_stream_ids,
    std::map<std::uint64_t, std::vector<std::uint8_t>>& pending_bytes,
    PeerRequestIdValidator& peer_request_ids,
    ApplyUpdate&& apply_update) {
    if (draft != openmoq::publisher::DraftVersion::kDraft18) {
        return TransportStatus::success();
    }
    for (const auto& [existing_request_id, subscribe] :
         active_subscriptions) {
        static_cast<void>(subscribe);
        const auto stream_it = request_stream_ids.find(existing_request_id);
        if (stream_it == request_stream_ids.end()) {
            return TransportStatus::failure(
                "missing draft-18 subscriber request stream");
        }
        auto& pending = pending_bytes[existing_request_id];
        bool stream_fin = false;
        while (true) {
            std::size_t message_size = 0;
            if (next_control_message(pending, draft, message_size)) {
                const std::vector<std::uint8_t> message_bytes(
                    pending.begin(),
                    pending.begin() +
                        static_cast<std::ptrdiff_t>(message_size));
                RequestUpdateMessage update;
                RequestUpdateDecodeError decode_error =
                    RequestUpdateDecodeError::kNone;
                if (!decode_request_update_message(
                        message_bytes, draft, update, &decode_error)) {
                    return request_update_decode_failure(
                        transport,
                        decode_error,
                        "invalid REQUEST_UPDATE on retained SUBSCRIBE stream");
                }
                if (update.existing_request_id.has_value()) {
                    return protocol_violation(
                        transport,
                        "draft-18 REQUEST_UPDATE carried an existing request id");
                }
                const TransportStatus request_id_status =
                    peer_request_ids.validate(transport, update.request_id);
                if (!request_id_status.ok) {
                    return request_id_status;
                }
                const TransportStatus update_status = apply_update(
                    existing_request_id, stream_it->second, update);
                if (!update_status.ok) {
                    return update_status;
                }
                pending.erase(
                    pending.begin(),
                    pending.begin() +
                        static_cast<std::ptrdiff_t>(message_size));
                continue;
            }

            if (stream_fin) {
                if (pending.empty()) {
                    break;
                }
                return protocol_violation(
                    transport,
                    "truncated REQUEST_UPDATE on retained SUBSCRIBE stream");
            }

            std::vector<std::uint8_t> chunk;
            bool fin = false;
            const TransportStatus read_status = transport.read_stream(
                stream_it->second,
                chunk,
                fin,
                std::chrono::milliseconds(0));
            if (!read_status.ok) {
                if (read_status.message ==
                        "timed out waiting for stream data" ||
                    read_status.message == "no queued read for stream") {
                    break;
                }
                return read_status;
            }
            pending.insert(pending.end(), chunk.begin(), chunk.end());
            stream_fin = fin;
            if (chunk.empty() && !stream_fin) {
                break;
            }
        }
    }
    return TransportStatus::success();
}

template <typename ApplyUpdate, typename OnFin>
TransportStatus poll_retained_publish_request_updates(
    PublisherTransport& transport,
    openmoq::publisher::DraftVersion draft,
    const std::map<std::string, std::uint64_t>& request_ids_by_track,
    const std::map<std::uint64_t, std::uint64_t>& request_stream_ids,
    std::map<std::uint64_t, std::vector<std::uint8_t>>& pending_bytes,
    std::set<std::uint64_t>& terminated_request_ids,
    PeerRequestIdValidator& peer_request_ids,
    ApplyUpdate&& apply_update,
    OnFin&& on_fin) {
    if (draft != openmoq::publisher::DraftVersion::kDraft18) {
        return TransportStatus::success();
    }
    for (const auto& [track_name, request_id] : request_ids_by_track) {
        if (terminated_request_ids.contains(request_id)) {
            continue;
        }
        const auto stream_it = request_stream_ids.find(request_id);
        if (stream_it == request_stream_ids.end()) {
            return TransportStatus::failure(
                "missing draft-18 publish request stream");
        }
        auto& pending = pending_bytes[request_id];
        bool stream_fin = false;
        while (true) {
            std::size_t message_size = 0;
            if (next_control_message(pending, draft, message_size)) {
                const std::vector<std::uint8_t> message_bytes(
                    pending.begin(),
                    pending.begin() +
                        static_cast<std::ptrdiff_t>(message_size));
                RequestUpdateMessage update;
                RequestUpdateDecodeError decode_error =
                    RequestUpdateDecodeError::kNone;
                if (!decode_request_update_message(
                        message_bytes, draft, update, &decode_error)) {
                    return request_update_decode_failure(
                        transport,
                        decode_error,
                        "invalid REQUEST_UPDATE on PUBLISH stream");
                }
                if (update.existing_request_id.has_value()) {
                    return protocol_violation(
                        transport,
                        "draft-18 REQUEST_UPDATE carried an existing request id");
                }
                const TransportStatus request_id_status =
                    peer_request_ids.validate(transport, update.request_id);
                if (!request_id_status.ok) {
                    return request_id_status;
                }
                const TransportStatus update_status = apply_update(
                    track_name, request_id, stream_it->second, update);
                if (!update_status.ok) {
                    return update_status;
                }
                pending.erase(
                    pending.begin(),
                    pending.begin() +
                        static_cast<std::ptrdiff_t>(message_size));
                continue;
            }
            if (stream_fin) {
                if (!pending.empty()) {
                    return protocol_violation(
                        transport,
                        "truncated REQUEST_UPDATE on PUBLISH stream");
                }
                terminated_request_ids.insert(request_id);
                on_fin(track_name, request_id);
                break;
            }

            std::vector<std::uint8_t> chunk;
            bool fin = false;
            const TransportStatus read_status = transport.read_stream(
                stream_it->second,
                chunk,
                fin,
                std::chrono::milliseconds(0));
            if (!read_status.ok) {
                if (read_status.message ==
                        "timed out waiting for stream data" ||
                    read_status.message == "no queued read for stream") {
                    break;
                }
                return read_status;
            }
            pending.insert(pending.end(), chunk.begin(), chunk.end());
            stream_fin = fin;
            if (chunk.empty() && !stream_fin) {
                break;
            }
        }
    }
    return TransportStatus::success();
}

template <typename ApplyUpdate>
TransportStatus process_draft16_control_request_update(
    PublisherTransport& transport,
    std::span<const std::uint8_t> message_bytes,
    std::uint64_t control_stream_id,
    PeerRequestIdValidator& peer_request_ids,
    ApplyUpdate&& apply_update) {
    RequestUpdateMessage update;
    RequestUpdateDecodeError decode_error =
        RequestUpdateDecodeError::kNone;
    if (!decode_request_update_message(
            message_bytes,
            openmoq::publisher::DraftVersion::kDraft16,
            update,
            &decode_error)) {
        return request_update_decode_failure(
            transport, decode_error, "received invalid REQUEST_UPDATE");
    }
    if (!update.existing_request_id.has_value()) {
        return protocol_violation(
            transport,
            "draft-16 REQUEST_UPDATE omitted existing request id");
    }
    const TransportStatus request_id_status =
        peer_request_ids.validate(transport, update.request_id);
    if (!request_id_status.ok) {
        return request_id_status;
    }
    return apply_update(
        *update.existing_request_id, control_stream_id, update);
}

TransportStatus serve_subscriptions(PublisherTransport& transport,
                                    PublishedObjectSink published_sink,
                                    std::uint64_t control_stream_id,
                                    std::uint64_t namespace_stream_id,
                                    std::uint64_t peer_control_stream_id,
                                    const openmoq::publisher::PublishPlan& plan,
                                    const LoopState& loop_state,
                                    const std::map<std::string, PublishedTrack>& tracks_by_name,
                                    openmoq::publisher::DraftVersion draft,
                                    std::string_view track_namespace,
                                    bool paced,
                                    std::chrono::milliseconds subscriber_timeout,
                                    std::vector<std::uint8_t>& pending_control_bytes,
                                    bool send_namespace_done = true,
                                    std::map<std::uint64_t, DormantPublishedTrack>* dormant_published_tracks = nullptr,
                                    const std::map<std::uint64_t, std::uint64_t>* request_id_by_track_alias = nullptr,
                                    std::uint64_t peer_max_request_id = 0,
                                    std::uint64_t local_max_request_id = 100,
                                    std::uint64_t subscribe_tracks_next_request_id = 2,
                                    const std::optional<std::vector<std::uint8_t>>& authorization_token = std::nullopt,
                                    const NowFunction& now_function = steady_now_function()) {
    std::vector<std::uint8_t> buffer = std::move(pending_control_bytes);
    pending_control_bytes.clear();
    std::set<std::uint64_t> completed_request_ids;
    std::map<std::uint64_t, SubscribeMessage> pending_subscriptions;
    std::deque<std::uint64_t> pending_subscription_order;
    std::map<std::uint64_t, ActiveSubscription> active_subscriptions;
    struct EligibleCandidate {
        std::uint64_t request_id = 0;
        std::uint64_t scheduler_generation = 0;
        std::size_t loop_cycle = 0;
        std::size_t plan_index = 0;
        priority_scheduler_internal::CandidatePriority priority;
    };
    struct LaterCandidate {
        bool operator()(const EligibleCandidate& first,
                        const EligibleCandidate& second) const {
            return priority_scheduler_internal::precedes(
                second.priority, first.priority);
        }
    };
    std::priority_queue<EligibleCandidate,
                        std::vector<EligibleCandidate>,
                        LaterCandidate>
        eligible_candidates;
    std::optional<std::chrono::steady_clock::time_point>
        priority_stall_started_at;
    const auto enqueue_active_candidate =
        [&](std::uint64_t request_id, const ActiveSubscription& active) {
            if (active.completed || active.forward_paused_by_update ||
                active.generation_lead_parked ||
                active.scheduler_frontiers.empty()) {
                return;
            }
            auto priority = *active.scheduler_frontiers.begin();
            priority.request_fairness_round = active.scheduling_round;
            priority.request_id = request_id;
            eligible_candidates.push({
                .request_id = request_id,
                .scheduler_generation = active.scheduler_generation,
                .loop_cycle = active.loop_cycle,
                .plan_index = priority.plan_index,
                .priority = priority,
            });
        };
    PeerRequestIdValidator peer_request_ids(draft, local_max_request_id);
    bool fin = false;
    bool served_any_subscription = false;
    std::optional<std::chrono::steady_clock::time_point> catalog_last_served_at;
    std::uint64_t first_media_time_us = 0;
    bool first_media_time_set = false;
    const auto pacing_start = std::chrono::steady_clock::now();
    GenerationAvailability generation_availability(
        plan,
        loop_state,
        paced,
        now_function,
        uses_priority_scheduler(draft));
    const auto minimum_active_loop_cycle = [&]() {
        std::optional<std::size_t> minimum;
        for (const auto& [request_id, active] : active_subscriptions) {
            static_cast<void>(request_id);
            if (active.completed) {
                continue;
            }
            minimum = minimum.has_value()
                          ? (std::min)(*minimum, active.loop_cycle)
                          : active.loop_cycle;
        }
        return minimum;
    };
    const auto advance_active_subscription =
        [&](ActiveSubscription& active) {
            std::optional<std::size_t> maximum_loop_cycle;
            if (uses_priority_scheduler(draft)) {
                const auto minimum = minimum_active_loop_cycle();
                if (minimum.has_value()) {
                    maximum_loop_cycle =
                        *minimum <=
                                (std::numeric_limits<std::size_t>::max)() -
                                    kMaximumGenerationLeadCycles
                            ? *minimum + kMaximumGenerationLeadCycles
                            : (std::numeric_limits<std::size_t>::max)();
                }
            }
            return advance_subscription_to_next_loop_object(
                plan, loop_state, active, draft, generation_availability,
                maximum_loop_cycle, &active.generation_lead_parked);
        };
    const auto await_subscribe_deadline = pacing_start + subscriber_timeout;
    NamespaceMessage namespace_message{
        .draft = draft,
        .track_namespace = std::string(track_namespace),
        .request_id = 0,
        .authorization_token = std::nullopt,
    };
    const std::uint64_t control_read_stream_id =
        uses_request_streams(draft) ? peer_control_stream_id : control_stream_id;

    const auto apply_update_to_active = [&](ActiveSubscription& active,
                                            const RequestUpdateMessage& update) {
        const bool deadlines_changed =
            update.object_delivery_timeout_ms.has_value() ||
            update.subgroup_delivery_timeout_ms.has_value();
        const bool renew_peer_stopped_subgroups =
            request_update_renews_peer_stopped_subgroups(
                draft, active.subscribe, update);
        if (update.forward.has_value()) {
            active.forward_paused_by_update = *update.forward == 0;
        }
        apply_request_update(active.subscribe, update);
        if (renew_peer_stopped_subgroups) {
            active.sender->renew_peer_stopped_subgroups(transport);
        }
        note_delivery_timeouts(transport, active.subscribe.delivery_timeouts);

        if (deadlines_changed) {
            const DeliveryTimeouts timeouts = timeouts_for_draft(
                draft, active.subscribe.delivery_timeouts);
            for (auto& [plan_index, options] :
                 active.availability_by_object_index) {
                options.object_deadline = deadline_after(
                    generation_availability.object_available_at(
                        active.loop_cycle, plan_index),
                    timeouts.object_ms);
                options.subgroup_deadline = deadline_after(
                    generation_availability.subgroup_completed_at(
                        active.loop_cycle, plan_index),
                    timeouts.subgroup_ms);
                const auto object = make_looped_object(
                    plan.objects[plan_index], loop_state, active.loop_cycle);
                active.sender->refresh_unadmitted_deadlines(
                    object.group_id,
                    object.subgroup_id,
                    object.object_id,
                    options.object_deadline,
                    options.subgroup_deadline);
            }
        }

        for (auto sequence_it = active.send_sequence_by_object_index.begin();
             sequence_it != active.send_sequence_by_object_index.end();) {
            const std::size_t plan_index = sequence_it->first;
            const auto& source_object = plan.objects[plan_index];
            if (object_matches_filter(source_object, active.subscribe)) {
                ++sequence_it;
                continue;
            }
            const auto object = make_looped_object(
                source_object, loop_state, active.loop_cycle);
            active.sender->discard_unadmitted(
                object.group_id, object.subgroup_id, object.object_id);
            sequence_it = active.send_sequence_by_object_index.erase(sequence_it);
        }

        if (uses_priority_scheduler(draft)) {
            active.generation_lead_parked = false;
            rebuild_priority_frontiers(
                plan, active, draft, generation_availability);
            if (active.subgroup_frontiers.empty()) {
                active.completed = !advance_active_subscription(active);
            }
            return;
        }

        std::size_t next_object_index = 0;
        if (find_next_matching_object_index(
                plan, active.subscribe, 0, next_object_index)) {
            active.next_object_index = next_object_index;
            active.completed = false;
        } else {
            active.next_object_index = plan.objects.size();
            active.completed =
                !advance_subscription_to_next_loop_object(
                    plan, loop_state, active, draft,
                    generation_availability);
        }
    };

    const auto resume_generation_leaders = [&]() {
        for (auto& [request_id, active] : active_subscriptions) {
            if (!active.generation_lead_parked) {
                continue;
            }
            active.completed = !advance_active_subscription(active);
            enqueue_active_candidate(request_id, active);
        }
    };
    const auto erase_active_subscription =
        [&](std::uint64_t request_id) {
            const auto active_it = active_subscriptions.find(request_id);
            if (active_it == active_subscriptions.end()) {
                return;
            }
            active_subscriptions.erase(active_it);
            resume_generation_leaders();
        };

    const auto process_subscription_request_update =
        [&](const RequestUpdateMessage& update,
            std::uint64_t existing_request_id,
            std::uint64_t response_stream_id) -> TransportStatus {
        if (update.new_group_request.has_value()) {
            return protocol_violation(
                transport,
                "REQUEST_UPDATE NEW_GROUP requires negotiated DYNAMIC_GROUPS");
        }

        std::optional<std::pair<std::size_t, std::size_t>>
            largest_object_for_response;
        const auto note_end_extension =
            [&](const SubscribeMessage& subscribe,
                const PublishedTrack& track) {
                if (track.content_exists &&
                    request_update_extends_end(subscribe, update)) {
                    largest_object_for_response =
                        {track.largest_group_id, track.largest_object_id};
                }
            };
        const auto pending_before =
            pending_subscriptions.find(existing_request_id);
        if (pending_before != pending_subscriptions.end()) {
            const auto track_it =
                tracks_by_name.find(pending_before->second.track_name);
            if (track_it != tracks_by_name.end()) {
                note_end_extension(pending_before->second, track_it->second);
            }
        } else {
            const auto active_before =
                active_subscriptions.find(existing_request_id);
            if (active_before != active_subscriptions.end()) {
                note_end_extension(active_before->second.subscribe,
                                   active_before->second.track);
            } else if (dormant_published_tracks != nullptr) {
                const auto dormant_before =
                    dormant_published_tracks->find(existing_request_id);
                if (dormant_before != dormant_published_tracks->end()) {
                    note_end_extension(dormant_before->second.subscribe,
                                       dormant_before->second.track);
                }
            }
        }

        auto pending_it = pending_subscriptions.find(existing_request_id);
        if (pending_it != pending_subscriptions.end()) {
            apply_request_update(pending_it->second, update);
            note_delivery_timeouts(transport, pending_it->second.delivery_timeouts);
        } else {
            auto active_it = active_subscriptions.find(existing_request_id);
            if (active_it != active_subscriptions.end()) {
                apply_update_to_active(active_it->second, update);
                enqueue_active_candidate(existing_request_id,
                                         active_it->second);
            } else if (dormant_published_tracks != nullptr) {
                auto dormant_it =
                    dormant_published_tracks->find(existing_request_id);
                if (dormant_it == dormant_published_tracks->end()) {
                    return protocol_violation(
                        transport, "REQUEST_UPDATE specified an invalid existing request id");
                }

                ActiveSubscription active{
                    .subscribe = dormant_it->second.subscribe,
                    .track = dormant_it->second.track,
                    .sender = std::make_shared<SubgroupSenderState>(),
                    .request_stream_id = response_stream_id,
                    .loop_cycle = generation_availability.initial_loop_cycle(),
                    .next_object_index = 0,
                    .admitted_object_indices = {},
                    .send_sequence_by_object_index = {},
                    .subgroup_frontiers = {},
                    .scheduler_frontiers = {},
                    .availability_by_object_index = {},
                    .pending_request_bytes = {},
                    .publisher_priority =
                        priority_scheduler_internal::publisher_priority_for_request(
                            existing_request_id),
                    .scheduling_round = 0,
                    .forward_paused_by_update =
                        uses_priority_scheduler(draft) &&
                        dormant_it->second.subscribe.forward == 0,
                };
                apply_update_to_active(active, update);
                if (!active.completed) {
                    active_subscriptions.insert_or_assign(
                        existing_request_id, std::move(active));
                    enqueue_active_candidate(
                        existing_request_id,
                        active_subscriptions.at(existing_request_id));
                }
                dormant_published_tracks->erase(dormant_it);
            } else {
                return protocol_violation(
                    transport, "REQUEST_UPDATE specified an invalid existing request id");
            }
        }

        const std::vector<std::uint8_t> response =
            largest_object_for_response.has_value()
                ? encode_request_ok_message(
                      draft,
                      update.request_id,
                      largest_object_for_response->first,
                      largest_object_for_response->second)
                : encode_request_ok_message(draft, update.request_id);
        return transport.write_stream(response_stream_id, response, false);
    };

    while (true) {
        if (uses_request_streams(draft)) {
            while (true) {
                std::uint64_t request_stream_id = 0;
                const TransportStatus accept_status =
                    transport.accept_stream(StreamDirection::kBidirectional,
                                            request_stream_id,
                                            std::chrono::milliseconds(0));
                if (!accept_status.ok) {
                    if (accept_status.message == "timed out waiting for stream data") {
                        break;
                    }
                    return accept_status;
                }
                const TransportStatus priority_status =
                    assign_request_stream_priority(transport, draft, request_stream_id);
                if (!priority_status.ok) {
                    return priority_status;
                }

                std::vector<std::uint8_t> message_bytes;
                std::vector<std::uint8_t> trailing_request_bytes;
                const TransportStatus read_status =
                    read_request_stream_message(
                        transport,
                        request_stream_id,
                        draft,
                        subscriber_timeout,
                        message_bytes,
                        &trailing_request_bytes);
                if (!read_status.ok) {
                    return read_status;
                }
                trace_control_message(message_bytes, draft);

                std::size_t request_offset = 0;
                std::uint64_t request_type = 0;
                if (!decode_moqint(message_bytes, request_offset, draft, request_type)) {
                    return protocol_violation(transport, "failed to parse request stream type");
                }

                SubscribeTracksMessage subscribe_tracks;
                if (request_type == 0x03) {
                    SubscribeMessage subscribe;
                    if (!decode_subscribe_message(message_bytes, draft, subscribe)) {
                        const TransportStatus write_status =
                            transport.write_stream(request_stream_id,
                                                   encode_request_error_message(
                                                       draft, 0, 0x1, 0, "invalid SUBSCRIBE"),
                                                   true);
                        return write_status.ok ? protocol_violation(transport, "received invalid SUBSCRIBE")
                                               : write_status;
                    }
                    const TransportStatus request_id_status =
                        peer_request_ids.validate(transport, subscribe.request_id);
                    if (!request_id_status.ok) {
                        return request_id_status;
                    }
                    note_delivery_timeouts(transport, subscribe.delivery_timeouts);
                    if (!namespace_matches(subscribe.track_namespace, track_namespace)) {
                        const TransportStatus write_status =
                            transport.write_stream(request_stream_id,
                                                   encode_request_error_message(
                                                       draft, subscribe.request_id, 0x2, 0, "track does not exist"),
                                                   true);
                        return write_status.ok ? TransportStatus::failure("peer requested unsupported track namespace")
                                               : write_status;
                    }
                    const auto track_it = tracks_by_name.find(subscribe.track_name);
                    if (track_it == tracks_by_name.end()) {
                        const TransportStatus write_status =
                            transport.write_stream(request_stream_id,
                                                   encode_request_error_message(
                                                       draft, subscribe.request_id, 0x2, 0, "track does not exist"),
                                                   true);
                        if (!write_status.ok) {
                            return write_status;
                        }
                        continue;
                    }
                    const TransportStatus ok_status =
                        transport.write_stream(request_stream_id,
                                               encode_subscribe_ok_message(draft,
                                                                           subscribe.request_id,
                                                                           track_it->second.alias,
                                                                           0,
                                                                           0,
                                                                           false),
                                               false);
                    if (!ok_status.ok) {
                        return ok_status;
                    }
                    ActiveSubscription active{
                        .subscribe = subscribe,
                        .track = track_it->second,
                        .sender = std::make_shared<SubgroupSenderState>(),
                        .request_stream_id = request_stream_id,
                        .loop_cycle = generation_availability.initial_loop_cycle(),
                        .next_object_index = 0,
                        .admitted_object_indices = {},
                        .send_sequence_by_object_index = {},
                        .subgroup_frontiers = {},
                        .scheduler_frontiers = {},
                        .availability_by_object_index = {},
                        .pending_request_bytes =
                            std::move(trailing_request_bytes),
                        .publisher_priority =
                            priority_scheduler_internal::publisher_priority_for_request(
                                subscribe.request_id),
                        .scheduling_round = 0,
                        .forward_paused_by_update =
                            uses_priority_scheduler(draft) &&
                            subscribe.forward == 0,
                        .completed = false,
                    };
                    std::size_t next_object_index = 0;
                    if (find_next_matching_object_index(plan, active.subscribe, 0, next_object_index)) {
                        active.next_object_index = next_object_index;
                        if (uses_priority_scheduler(draft)) {
                            rebuild_priority_frontiers(
                                plan, active, draft,
                                generation_availability);
                        }
                        active_subscriptions.insert_or_assign(subscribe.request_id, std::move(active));
                        enqueue_active_candidate(
                            subscribe.request_id,
                            active_subscriptions.at(subscribe.request_id));
                    } else {
                        const TransportStatus finalize_status =
                            finalize_subscription(transport, draft, request_stream_id, subscribe.request_id, 0, completed_request_ids);
                        if (!finalize_status.ok) {
                            return finalize_status;
                        }
                        served_any_subscription = true;
                    }
                    continue;
                }

                if (request_type == 0x50) {
                    SubscribeNamespaceMessage subscribe_namespace;
                    if (!decode_subscribe_namespace_message(message_bytes, draft, subscribe_namespace)) {
                        const TransportStatus write_status =
                            transport.write_stream(request_stream_id,
                                                   encode_request_error_message(
                                                       draft, 0, 0x1, 0, "invalid SUBSCRIBE_NAMESPACE"),
                                                   true);
                        return write_status.ok ? protocol_violation(transport, "received invalid SUBSCRIBE_NAMESPACE")
                                               : write_status;
                    }
                    const TransportStatus request_id_status =
                        peer_request_ids.validate(
                            transport, subscribe_namespace.request_id);
                    if (!request_id_status.ok) {
                        return request_id_status;
                    }
                    if (!namespace_prefix_matches(subscribe_namespace.track_namespace_prefix, track_namespace)) {
                        const TransportStatus write_status =
                            transport.write_stream(request_stream_id,
                                                   encode_request_error_message(
                                                       draft, subscribe_namespace.request_id, 0x2, 0, "unsupported namespace prefix"),
                                                   true);
                        return write_status.ok ? TransportStatus::failure("peer requested unsupported namespace prefix")
                                               : write_status;
                    }
                    const TransportStatus ok_status =
                        transport.write_stream(request_stream_id,
                                               encode_request_ok_message(draft, subscribe_namespace.request_id),
                                               false);
                    if (!ok_status.ok) {
                        return ok_status;
                    }
                    continue;
                }

                if (request_type != 0x51 || !decode_subscribe_tracks_message(message_bytes, draft, subscribe_tracks)) {
                    const TransportStatus write_status =
                        transport.write_stream(request_stream_id,
                                               encode_request_error_message(
                                                   draft, 0, 0x1, 0, "unsupported request stream"),
                                               true);
                    return write_status.ok ? protocol_violation(transport, "received unsupported request stream")
                                           : write_status;
                }
                const TransportStatus request_id_status =
                    peer_request_ids.validate(transport,
                                              subscribe_tracks.request_id);
                if (!request_id_status.ok) {
                    return request_id_status;
                }
                if (!namespace_prefix_matches(subscribe_tracks.track_namespace_prefix, track_namespace)) {
                    const TransportStatus write_status =
                        transport.write_stream(request_stream_id,
                                               encode_request_error_message(
                                                   draft, 0, 0x2, 0, "unsupported namespace prefix"),
                                               true);
                    return write_status.ok ? TransportStatus::failure("peer requested unsupported namespace prefix")
                                           : write_status;
                }

                const TransportStatus ok_status =
                    transport.write_stream(request_stream_id,
                                           encode_request_ok_message(draft, subscribe_tracks.request_id),
                                           false);
                if (!ok_status.ok) {
                    return ok_status;
                }

                if (subscribe_tracks.forward == 0) {
                    continue;
                }

                std::vector<PublishedTrack> matching_tracks;
                matching_tracks.reserve(tracks_by_name.size());
                for (const auto& [ignored, track] : tracks_by_name) {
                    static_cast<void>(ignored);
                    matching_tracks.push_back(track);
                }

                std::map<std::uint64_t, std::uint64_t> publish_stream_ids;
                const TransportStatus publish_status =
                    publish_selected_tracks(transport,
                                            control_stream_id,
                                            plan,
                                            loop_state,
                                            matching_tracks,
                                            peer_max_request_id,
                                            track_namespace,
                                            authorization_token,
                                            paced,
                                            pending_control_bytes,
                                            publish_stream_ids,
                                            dormant_published_tracks,
                                            nullptr,
                                            now_function,
                                            subscribe_tracks_next_request_id);
                if (!publish_status.ok) {
                    return publish_status;
                }
                subscribe_tracks_next_request_id += matching_tracks.size() * 2;
                served_any_subscription = true;
            }
        }

        if (draft == DraftVersion::kDraft18) {
            for (auto& [existing_request_id, active] : active_subscriptions) {
                while (true) {
                    std::size_t update_size = 0;
                    if (next_control_message(
                            active.pending_request_bytes, draft, update_size)) {
                        const std::vector<std::uint8_t> update_bytes(
                            active.pending_request_bytes.begin(),
                            active.pending_request_bytes.begin() +
                                static_cast<std::ptrdiff_t>(update_size));
                        RequestUpdateMessage update;
                        RequestUpdateDecodeError decode_error =
                            RequestUpdateDecodeError::kNone;
                        if (!decode_request_update_message(
                                update_bytes, draft, update, &decode_error)) {
                            return request_update_decode_failure(
                                transport,
                                decode_error,
                                "received invalid REQUEST_UPDATE on retained SUBSCRIBE stream");
                        }
                        if (update.existing_request_id.has_value()) {
                            return protocol_violation(
                                transport,
                                "draft-18 REQUEST_UPDATE carried an existing request id");
                        }
                        const TransportStatus request_id_status =
                            peer_request_ids.validate(transport,
                                                      update.request_id);
                        if (!request_id_status.ok) {
                            return request_id_status;
                        }
                        const TransportStatus update_status =
                            process_subscription_request_update(
                                update,
                                existing_request_id,
                                active.request_stream_id);
                        if (!update_status.ok) {
                            return update_status;
                        }
                        active.pending_request_bytes.erase(
                            active.pending_request_bytes.begin(),
                            active.pending_request_bytes.begin() +
                                static_cast<std::ptrdiff_t>(update_size));
                        if (completed_request_ids.contains(existing_request_id)) {
                            break;
                        }
                        continue;
                    }

                    std::vector<std::uint8_t> chunk;
                    bool request_fin = false;
                    const TransportStatus read_status = transport.read_stream(
                        active.request_stream_id,
                        chunk,
                        request_fin,
                        std::chrono::milliseconds(0));
                    if (!read_status.ok) {
                        if (read_status.message ==
                                "timed out waiting for stream data" ||
                            read_status.message == "no queued read for stream") {
                            break;
                        }
                        return read_status;
                    }
                    active.pending_request_bytes.insert(
                        active.pending_request_bytes.end(),
                        chunk.begin(),
                        chunk.end());
                    if (request_fin &&
                        !next_control_message(
                            active.pending_request_bytes, draft, update_size)) {
                        return protocol_violation(
                            transport,
                            "retained SUBSCRIBE stream closed with a truncated REQUEST_UPDATE");
                    }
                }
            }
            for (const std::uint64_t completed_request_id :
                 completed_request_ids) {
                erase_active_subscription(completed_request_id);
            }
        }

        std::size_t message_size = 0;
        while (next_control_message(buffer, draft, message_size)) {
            const std::vector<std::uint8_t> message_bytes(buffer.begin(), buffer.begin() + message_size);
            std::size_t offset = 0;
            std::uint64_t message_type = 0;
            if (!decode_moqint(message_bytes, offset, draft, message_type)) {
                return protocol_violation(transport, "failed to parse control request type");
            }
            trace_control_message(message_bytes, draft);

            // Discard parsed control messages we don't act on (for example,
            // acknowledged responses, FETCH, GOAWAY, or UNSUBSCRIBE) so they
            // do not block the control-stream buffer. This only applies to
            // messages that next_control_message() can frame successfully;
            // log them for visibility only when tracing is explicitly enabled.
            if (uses_request_streams(draft) &&
                ((draft == DraftVersion::kDraft18 && message_type == 0x02) ||
                 message_type == 0x03 || message_type == 0x06 || message_type == 0x50 ||
                 message_type == 0x16 || message_type == 0x1d || message_type == 0x51)) {
                return protocol_violation(transport, "draft-18 request message received on control stream");
            }
            if (message_type == 0x0a) {
                if (uses_request_streams(draft)) {
                    return protocol_violation(transport, "UNSUBSCRIBE received on draft-18 control stream");
                }
                std::uint64_t unsubscribe_request_id = 0;
                if (!decode_unsubscribe_message(message_bytes, draft, unsubscribe_request_id)) {
                    return protocol_violation(transport, "received invalid UNSUBSCRIBE");
                }
                pending_subscriptions.erase(unsubscribe_request_id);
                auto active_it = active_subscriptions.find(unsubscribe_request_id);
                if (active_it != active_subscriptions.end()) {
                    const TransportStatus finish_status = active_it->second.sender->finish_group(transport);
                    if (!finish_status.ok) {
                        return finish_status;
                    }
                    const TransportStatus finalize_status =
                        finalize_subscription(transport,
                                              draft,
                                              control_stream_id,
                                              unsubscribe_request_id,
                                              active_it->second.sender->stream_count(),
                                              completed_request_ids);
                    if (!finalize_status.ok) {
                        return finalize_status;
                    }
                    erase_active_subscription(unsubscribe_request_id);
                    served_any_subscription = true;
                }
                buffer.erase(buffer.begin(), buffer.begin() + message_size);
                continue;
            }
            const bool is_handled_type =
                message_type == 0x02 ||  // SUBSCRIBE_UPDATE
                (!uses_request_streams(draft) &&
                 (message_type == 0x11 ||  // SUBSCRIBE_NAMESPACE
                  message_type == 0x03));  // SUBSCRIBE
            if (!is_handled_type) {
                if (trace_enabled()) {
                    std::cerr << "[moqt-session] skipping unhandled control message type=0x"
                              << std::hex << message_type << std::dec
                              << " (" << control_message_type_name(message_type, draft) << ")"
                              << " size=" << message_size << '\n';
                }
                buffer.erase(buffer.begin(), buffer.begin() + message_size);
                continue;
            }

            if (message_type == 0x02) {
                if (draft == DraftVersion::kDraft16) {
                    RequestUpdateMessage request_update;
                    RequestUpdateDecodeError decode_error =
                        RequestUpdateDecodeError::kNone;
                    if (!decode_request_update_message(
                            message_bytes, draft, request_update,
                            &decode_error)) {
                        return request_update_decode_failure(
                            transport,
                            decode_error,
                            "received invalid REQUEST_UPDATE");
                    }
                    if (!request_update.existing_request_id.has_value()) {
                        return protocol_violation(
                            transport, "received invalid REQUEST_UPDATE");
                    }
                    const TransportStatus request_id_status =
                        peer_request_ids.validate(
                            transport, request_update.request_id);
                    if (!request_id_status.ok) {
                        return request_id_status;
                    }
                    const TransportStatus update_status =
                        process_subscription_request_update(
                            request_update,
                            *request_update.existing_request_id,
                            control_stream_id);
                    if (!update_status.ok) {
                        return update_status;
                    }
                    if (completed_request_ids.contains(
                            *request_update.existing_request_id)) {
                        erase_active_subscription(
                            *request_update.existing_request_id);
                    }
                    buffer.erase(buffer.begin(), buffer.begin() + message_size);
                    continue;
                }

                SubscribeUpdateMessage subscribe_update;
                if (!decode_subscribe_update_message(message_bytes, subscribe_update) &&
                    !decode_legacy_subscribe_update_message(message_bytes,
                                                            tracks_by_name,
                                                            pending_subscriptions,
                                                            request_id_by_track_alias,
                                                            subscribe_update)) {
                    return protocol_violation(transport, "received invalid SUBSCRIBE_UPDATE");
                }
                const auto original_request_id = subscribe_update.subscription_request_id;
                auto remapped_request_id = subscribe_update.subscription_request_id;
                if (request_id_by_track_alias != nullptr && !pending_subscriptions.contains(remapped_request_id) &&
                    !active_subscriptions.contains(remapped_request_id) &&
                    (dormant_published_tracks == nullptr || !dormant_published_tracks->contains(remapped_request_id))) {
                    const auto alias_it = request_id_by_track_alias->find(remapped_request_id);
                    if (alias_it != request_id_by_track_alias->end()) {
                        remapped_request_id = alias_it->second;
                        subscribe_update.subscription_request_id = remapped_request_id;
                    }
                }
                if (completed_request_ids.contains(remapped_request_id)) {
                    trace_subscribe_update_state("request already completed",
                                                 subscribe_update,
                                                 original_request_id,
                                                 remapped_request_id,
                                                 pending_subscriptions,
                                                 active_subscriptions,
                                                 dormant_published_tracks,
                                                 completed_request_ids,
                                                 request_id_by_track_alias);
                    return TransportStatus::failure("received invalid SUBSCRIBE_UPDATE state transition");
                }

                auto pending_it = pending_subscriptions.find(remapped_request_id);
                if (pending_it != pending_subscriptions.end()) {
                    if (!apply_subscribe_update(pending_it->second, subscribe_update)) {
                        trace_subscribe_update_state("pending update not monotonic",
                                                     subscribe_update,
                                                     original_request_id,
                                                     remapped_request_id,
                                                     pending_subscriptions,
                                                     active_subscriptions,
                                                     dormant_published_tracks,
                                                     completed_request_ids,
                                                     request_id_by_track_alias);
                        return TransportStatus::failure("received invalid SUBSCRIBE_UPDATE state transition");
                    }
                    buffer.erase(buffer.begin(), buffer.begin() + message_size);
                    continue;
                }

                auto active_it = active_subscriptions.find(remapped_request_id);
                if (active_it != active_subscriptions.end()) {
                    if (!apply_subscribe_update(active_it->second.subscribe, subscribe_update)) {
                        trace_subscribe_update_state("active update not monotonic",
                                                     subscribe_update,
                                                     original_request_id,
                                                     remapped_request_id,
                                                     pending_subscriptions,
                                                     active_subscriptions,
                                                     dormant_published_tracks,
                                                     completed_request_ids,
                                                     request_id_by_track_alias);
                        return TransportStatus::failure("received invalid SUBSCRIBE_UPDATE state transition");
                    }

                    std::size_t next_object_index = 0;
                    if (find_next_matching_object_index(plan,
                                                        active_it->second.subscribe,
                                                        active_it->second.next_object_index,
                                                        next_object_index)) {
                        active_it->second.next_object_index = next_object_index;
                        active_it->second.completed = false;
                    } else {
                        active_it->second.next_object_index = plan.objects.size();
                        active_it->second.completed =
                            !advance_subscription_to_next_loop_object(
                                plan,
                                loop_state,
                                active_it->second,
                                draft,
                                generation_availability);
                    }
                    buffer.erase(buffer.begin(), buffer.begin() + message_size);
                    continue;
                }

                if (dormant_published_tracks != nullptr) {
                    auto dormant_it = dormant_published_tracks->find(remapped_request_id);
                    if (dormant_it != dormant_published_tracks->end()) {
                        ActiveSubscription active{
                            .subscribe = dormant_it->second.subscribe,
                            .track = dormant_it->second.track,
                            .sender = std::make_shared<SubgroupSenderState>(),
                            .request_stream_id = control_stream_id,
                            .loop_cycle = generation_availability.initial_loop_cycle(),
                            .next_object_index = 0,
                            .admitted_object_indices = {},
                            .send_sequence_by_object_index = {},
                            .subgroup_frontiers = {},
                            .scheduler_frontiers = {},
                            .availability_by_object_index = {},
                            .pending_request_bytes = {},
                            .publisher_priority =
                                priority_scheduler_internal::publisher_priority_for_request(
                                    remapped_request_id),
                            .scheduling_round = 0,
                            .forward_paused_by_update =
                                uses_priority_scheduler(draft) &&
                                dormant_it->second.subscribe.forward == 0,
                            .completed = false,
                        };
                        if (!apply_subscribe_update(active.subscribe, subscribe_update)) {
                            trace_subscribe_update_state("dormant update not monotonic",
                                                         subscribe_update,
                                                         original_request_id,
                                                         remapped_request_id,
                                                         pending_subscriptions,
                                                         active_subscriptions,
                                                         dormant_published_tracks,
                                                         completed_request_ids,
                                                         request_id_by_track_alias);
                            return TransportStatus::failure("received invalid SUBSCRIBE_UPDATE state transition");
                        }

                        std::size_t next_object_index = 0;
                        if (find_next_matching_object_index(plan, active.subscribe, 0, next_object_index)) {
                            active.next_object_index = next_object_index;
                            active_subscriptions.insert_or_assign(remapped_request_id, std::move(active));
                        } else {
                            if (advance_subscription_to_next_loop_object(
                                    plan,
                                    loop_state,
                                    active,
                                    draft,
                                    generation_availability)) {
                                active_subscriptions.insert_or_assign(remapped_request_id, std::move(active));
                            } else {
                                const TransportStatus finalize_status = finalize_subscription(
                                    transport, draft, control_stream_id, remapped_request_id, 0, completed_request_ids);
                                if (!finalize_status.ok) {
                                    return finalize_status;
                                }
                                served_any_subscription = true;
                            }
                        }
                        dormant_published_tracks->erase(dormant_it);
                        buffer.erase(buffer.begin(), buffer.begin() + message_size);
                        continue;
                    }
                }

                trace_subscribe_update_state("unknown subscription_request_id",
                                             subscribe_update,
                                             original_request_id,
                                             remapped_request_id,
                                             pending_subscriptions,
                                             active_subscriptions,
                                             dormant_published_tracks,
                                             completed_request_ids,
                                             request_id_by_track_alias);
                return TransportStatus::failure("received invalid SUBSCRIBE_UPDATE state transition");
            }

            if (message_type == 0x11) {
                SubscribeNamespaceMessage subscribe_namespace;
                if (!decode_subscribe_namespace_message(message_bytes, draft, subscribe_namespace)) {
                    return protocol_violation(transport, "received invalid SUBSCRIBE_NAMESPACE");
                }
                const TransportStatus request_id_status =
                    peer_request_ids.validate(
                        transport, subscribe_namespace.request_id);
                if (!request_id_status.ok) {
                    return request_id_status;
                }
                if (!namespace_prefix_matches(subscribe_namespace.track_namespace_prefix, track_namespace)) {
                    return TransportStatus::failure("peer requested unsupported namespace prefix");
                }
                const TransportStatus write_status =
                    transport.write_stream(control_stream_id,
                                           encode_subscribe_namespace_ok_message(draft, subscribe_namespace.request_id),
                                           false);
                if (!write_status.ok) {
                    return write_status;
                }
                buffer.erase(buffer.begin(), buffer.begin() + message_size);
                continue;
            }

            if (message_type == 0x03) {
                SubscribeMessage subscribe;
                if (!decode_subscribe_message(message_bytes, draft, subscribe)) {
                    return protocol_violation(transport, "received invalid SUBSCRIBE");
                }
                const TransportStatus request_id_status =
                    peer_request_ids.validate(transport, subscribe.request_id);
                if (!request_id_status.ok) {
                    return request_id_status;
                }
                note_delivery_timeouts(transport, subscribe.delivery_timeouts);
                if (!namespace_matches(subscribe.track_namespace, track_namespace)) {
                    return TransportStatus::failure("peer requested unsupported track namespace");
                }
                pending_subscriptions.insert_or_assign(subscribe.request_id, subscribe);
                pending_subscription_order.push_back(subscribe.request_id);
                buffer.erase(buffer.begin(), buffer.begin() + message_size);
                continue;
            }
        }

        if (!fin && !pending_subscription_order.empty()) {
            std::vector<std::uint8_t> chunk;
            bool immediate_fin = false;
            const TransportStatus read_status =
                transport.read_stream(control_read_stream_id, chunk, immediate_fin, std::chrono::milliseconds(0));
            if (read_status.ok) {
                priority_stall_started_at.reset();
                if (trace_enabled()) {
                    std::cerr << "[moqt-session] control chunk now_ms="
                              << trace_elapsed_ms(std::chrono::steady_clock::now())
                              << " fin=" << (immediate_fin ? 1 : 0) << " bytes=["
                              << hex_dump(chunk) << "]" << std::endl;
                }
                buffer.insert(buffer.end(), chunk.begin(), chunk.end());
                fin = immediate_fin;
                continue;
            }
            if (read_status.message != "timed out waiting for stream data") {
                return read_status;
            }
        }

        while (!pending_subscription_order.empty()) {
            const std::uint64_t request_id = pending_subscription_order.front();
            pending_subscription_order.pop_front();
            auto pending_it = pending_subscriptions.find(request_id);
            if (pending_it == pending_subscriptions.end()) {
                continue;
            }

            const SubscribeMessage subscribe = pending_it->second;
            pending_subscriptions.erase(pending_it);

            const auto track_it = tracks_by_name.find(subscribe.track_name);
            if (track_it == tracks_by_name.end()) {
                const TransportStatus write_status =
                    transport.write_stream(control_stream_id,
                                           encode_request_error_message(
                                               draft, subscribe.request_id, 0x2, 0, "track does not exist"),
                                           false);
                if (!write_status.ok) {
                    return write_status;
                }
                continue;
            }

            const TransportStatus write_status =
                transport.write_stream(control_stream_id,
                                       encode_subscribe_ok_message(draft,
                                                                   subscribe.request_id,
                                                                   track_it->second.alias,
                                                                   0,
                                                                   0,
                                                                   false),
                                       false);
            if (!write_status.ok) {
                return write_status;
            }
            std::cerr << "[moqt-session] accepted subscribe track=" << subscribe.track_name
                      << " request_id=" << subscribe.request_id << '\n';

            ActiveSubscription active{
                .subscribe = subscribe,
                .track = track_it->second,
                .sender = std::make_shared<SubgroupSenderState>(),
                .loop_cycle = generation_availability.initial_loop_cycle(),
                .next_object_index = 0,
                .admitted_object_indices = {},
                .send_sequence_by_object_index = {},
                .subgroup_frontiers = {},
                .scheduler_frontiers = {},
                .availability_by_object_index = {},
                .pending_request_bytes = {},
                .publisher_priority =
                    priority_scheduler_internal::publisher_priority_for_request(request_id),
                .scheduling_round = 0,
                .forward_paused_by_update =
                    uses_priority_scheduler(draft) &&
                    subscribe.forward == 0,
                .completed = false,
            };

            std::size_t next_object_index = 0;
            if (find_next_matching_object_index(plan, active.subscribe, 0, next_object_index)) {
                active.next_object_index = next_object_index;
                if (uses_priority_scheduler(draft)) {
                    rebuild_priority_frontiers(
                        plan, active, draft,
                        generation_availability);
                }
                active_subscriptions.insert_or_assign(request_id, std::move(active));
                enqueue_active_candidate(request_id,
                                         active_subscriptions.at(request_id));
                continue;
            }

            const TransportStatus finalize_status =
                finalize_subscription(transport, draft, control_stream_id, request_id, 0, completed_request_ids);
            if (!finalize_status.ok) {
                return finalize_status;
            }
            served_any_subscription = true;
        }

        if (!active_subscriptions.empty()) {
            std::vector<std::uint64_t> completed_request_ids_to_finalize;
            for (const auto& [request_id, active] : active_subscriptions) {
                if (active.completed) {
                    completed_request_ids_to_finalize.push_back(request_id);
                }
            }
            for (const auto request_id : completed_request_ids_to_finalize) {
                const auto active_it = active_subscriptions.find(request_id);
                if (active_it == active_subscriptions.end()) {
                    continue;
                }
                const TransportStatus finalize_status =
                    finalize_subscription(transport,
                                          draft,
                                          uses_request_streams(draft)
                                              ? active_it->second.request_stream_id
                                              : control_stream_id,
                                          request_id,
                                          active_it->second.sender->stream_count(),
                                          completed_request_ids);
                if (!finalize_status.ok) {
                    return finalize_status;
                }
                erase_active_subscription(request_id);
            }
            if (!completed_request_ids_to_finalize.empty()) {
                served_any_subscription = true;
                continue;
            }

            std::vector<std::uint8_t> chunk;
            bool immediate_fin = false;
            const TransportStatus read_status =
                transport.read_stream(control_read_stream_id, chunk, immediate_fin, std::chrono::milliseconds(0));
            if (read_status.ok) {
                if (trace_enabled()) {
                    std::cerr << "[moqt-session] control chunk now_ms="
                              << trace_elapsed_ms(std::chrono::steady_clock::now())
                              << " fin=" << (immediate_fin ? 1 : 0) << " bytes=["
                              << hex_dump(chunk) << "]" << std::endl;
                }
                buffer.insert(buffer.end(), chunk.begin(), chunk.end());
                fin = immediate_fin;
                continue;
            }
            if (read_status.message != "timed out waiting for stream data") {
                return read_status;
            }

            if (uses_priority_scheduler(draft)) {
                std::set<std::size_t> needed_generation_cycles;
                for (const auto& [request_id, active] :
                     active_subscriptions) {
                    static_cast<void>(request_id);
                    needed_generation_cycles.insert(active.loop_cycle);
                }
                generation_availability.retain_generations(
                    needed_generation_cycles);
                bool made_progress = false;
                bool attempted_eligible_candidate = false;
                std::vector<EligibleCandidate> blocked_candidates;
                while (!eligible_candidates.empty()) {
                    const EligibleCandidate selected = eligible_candidates.top();
                    eligible_candidates.pop();
                    auto active_it = active_subscriptions.find(selected.request_id);
                    if (active_it == active_subscriptions.end()) {
                        continue;
                    }
                    ActiveSubscription& active = active_it->second;
                    if (active.completed || active.forward_paused_by_update ||
                        selected.scheduler_generation !=
                            active.scheduler_generation ||
                        selected.loop_cycle != active.loop_cycle ||
                        selected.priority.request_fairness_round !=
                            active.scheduling_round ||
                        active.scheduler_frontiers.empty()) {
                        continue;
                    }
                    auto selected_frontier_key = selected.priority;
                    selected_frontier_key.request_fairness_round = 0;
                    const auto scheduler_frontier_it =
                        active.scheduler_frontiers.find(
                            selected_frontier_key);
                    if (scheduler_frontier_it ==
                        active.scheduler_frontiers.end()) {
                        continue;
                    }
                    attempted_eligible_candidate = true;
                    const auto& source_object = plan.objects[selected.plan_index];
                    const openmoq::publisher::CmsfObject object =
                        make_looped_object(source_object,
                                           loop_state,
                                           selected.loop_cycle);
                    const auto payload = object_payload(source_object);
                    if (payload.empty()) {
                        return TransportStatus::failure(
                            "transport publish requires materialized object payloads");
                    }

                    if (object.kind == openmoq::publisher::CmsfObjectKind::kMedia &&
                        !first_media_time_set) {
                        first_media_time_us = object.media_time_us;
                        first_media_time_set = true;
                    }
                    auto send_sequence_it =
                        active.send_sequence_by_object_index.find(selected.plan_index);
                    if (send_sequence_it ==
                        active.send_sequence_by_object_index.end()) {
                        const std::uint64_t send_sequence =
                            object.kind == openmoq::publisher::CmsfObjectKind::kMedia
                                ? next_send_seq()
                                : 0;
                        send_sequence_it =
                            active.send_sequence_by_object_index
                                .emplace(selected.plan_index, send_sequence)
                                .first;
                    }
                    const std::uint64_t send_seq = send_sequence_it->second;
                    trace_pacing_decision(
                        "before", send_seq, pacing_start, first_media_time_us, object, paced);
                    pace_until(pacing_start, first_media_time_us, object, paced);
                    trace_pacing_decision(
                        "after", send_seq, pacing_start, first_media_time_us, object, paced);

                    const SubgroupSenderState::ServeResult serve_result =
                        active.sender->try_serve(
                            transport,
                            draft,
                            active.track.alias,
                            send_seq,
                            object,
                            subgroup_contains_group_largest(plan,
                                                            selected.plan_index),
                            is_final_object_in_subgroup(plan,
                                                        selected.plan_index),
                            payload,
                            active.subscribe.delivery_timeouts,
                            now_function,
                            {
                                .subscriber = active.subscribe.subscriber_priority,
                                .publisher = active.publisher_priority,
                            },
                            active.availability_by_object_index.at(
                                selected.plan_index));
                    if (!serve_result.status.ok) {
                        return serve_result.status;
                    }
                    if (serve_result.disposition ==
                        SubgroupSenderState::ServeDisposition::kWouldBlock) {
                        blocked_candidates.push_back(selected);
                        const auto next_frontier_it =
                            std::next(scheduler_frontier_it);
                        if (next_frontier_it !=
                            active.scheduler_frontiers.end()) {
                            auto next_priority = *next_frontier_it;
                            next_priority.request_fairness_round =
                                active.scheduling_round;
                            eligible_candidates.push({
                                .request_id = selected.request_id,
                                .scheduler_generation =
                                    active.scheduler_generation,
                                .loop_cycle = active.loop_cycle,
                                .plan_index = next_priority.plan_index,
                                .priority = next_priority,
                            });
                        }
                        continue;
                    }

                    active.admitted_object_indices.insert(selected.plan_index);
                    active.send_sequence_by_object_index.erase(selected.plan_index);
                    active.availability_by_object_index.erase(selected.plan_index);
                    active.scheduler_frontiers.erase(
                        scheduler_frontier_it);
                    const auto frontier_it = active.subgroup_frontiers.find(
                        {static_cast<std::uint64_t>(source_object.group_id),
                         source_object.subgroup_id});
                    if (frontier_it == active.subgroup_frontiers.end() ||
                        frontier_it->second.next >=
                            frontier_it->second.object_indices.size() ||
                        frontier_it->second.object_indices[frontier_it->second.next] !=
                            selected.plan_index) {
                        return TransportStatus::failure(
                            "priority scheduler lost its subgroup frontier");
                    }
                    ++frontier_it->second.next;
                    ++active.scheduling_round;
                    if (frontier_it->second.next <
                        frontier_it->second.object_indices.size()) {
                        const std::size_t successor_index =
                            frontier_it->second
                                .object_indices[frontier_it->second.next];
                        const auto& successor = plan.objects[successor_index];
                        active.scheduler_frontiers.insert({
                            .subscriber_priority =
                                active.subscribe.subscriber_priority,
                            .publisher_priority = active.publisher_priority,
                            .request_fairness_round = 0,
                            .group_order = active.subscribe.group_order,
                            .group_id = static_cast<std::uint64_t>(
                                successor.group_id),
                            .subgroup_id = successor.subgroup_id,
                            .object_id = static_cast<std::uint64_t>(
                                successor.object_id),
                            .request_id = selected.request_id,
                            .plan_index = successor_index,
                        });
                    }
                    const bool object_published =
                        serve_result.disposition ==
                        SubgroupSenderState::ServeDisposition::kAccepted;
                    if (object_published && published_sink) {
                        published_sink(object.track_name,
                                       object.group_id,
                                       object_payload_size(source_object));
                    }
                    if (object_published) {
                        std::cerr << "[moqt-session] served object send_seq=" << send_seq
                                  << " now_ms="
                                  << trace_elapsed_ms(std::chrono::steady_clock::now())
                                  << " track=" << object.track_name
                                  << " group=" << object.group_id
                                  << " object=" << object.object_id
                                  << " bytes=" << object_payload_size(source_object)
                                  << '\n';
                        if (object.track_name == "catalog") {
                            catalog_last_served_at = std::chrono::steady_clock::now();
                        }
                        trace_csv_write_served(
                            "served",
                            send_seq,
                            trace_elapsed_ms(std::chrono::steady_clock::now()),
                            object);
                    }

                    if (active.scheduler_frontiers.empty()) {
                        active.next_object_index = plan.objects.size();
                        active.completed = !advance_active_subscription(active);
                    }
                    resume_generation_leaders();
                    enqueue_active_candidate(selected.request_id, active);
                    for (const auto& blocked : blocked_candidates) {
                        eligible_candidates.push(blocked);
                    }
                    priority_stall_started_at.reset();
                    made_progress = true;
                    break;
                }

                if (!made_progress) {
                    for (const auto& blocked : blocked_candidates) {
                        eligible_candidates.push(blocked);
                    }
                    if (attempted_eligible_candidate &&
                        !blocked_candidates.empty()) {
                        const auto now = read_now(now_function);
                        if (!priority_stall_started_at.has_value()) {
                            priority_stall_started_at = now;
                        } else if (now - *priority_stall_started_at >=
                                   subscriber_timeout) {
                            const EligibleCandidate& victim =
                                blocked_candidates.back();
                            auto victim_it =
                                active_subscriptions.find(victim.request_id);
                            if (victim_it != active_subscriptions.end()) {
                                const std::uint64_t reset_code =
                                    draft == openmoq::publisher::DraftVersion::kDraft16
                                        ? 0x01
                                        : 0x05;
                                const TransportStatus reset_status =
                                    victim_it->second.sender
                                        ->cancel_all_open_subgroups(
                                            transport, reset_code);
                                if (!reset_status.ok) {
                                    return reset_status;
                                }
                                const std::uint64_t too_far_behind =
                                    draft == openmoq::publisher::DraftVersion::kDraft16
                                        ? 0x06
                                        : 0x05;
                                const TransportStatus finalize_status =
                                    finalize_subscription(
                                        transport,
                                        draft,
                                        uses_request_streams(draft)
                                            ? victim_it->second.request_stream_id
                                            : control_stream_id,
                                        victim.request_id,
                                        victim_it->second.sender->stream_count(),
                                        completed_request_ids,
                                        too_far_behind,
                                        "subscriber exceeded publisher resource limit");
                                if (!finalize_status.ok) {
                                    return finalize_status;
                                }
                                erase_active_subscription(victim.request_id);
                                served_any_subscription = true;
                            }
                            priority_stall_started_at.reset();
                            continue;
                        }
                    } else {
                        priority_stall_started_at.reset();
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                continue;
            }

            // Pick the (loop_cycle, media_time_us) of the earliest pending
            // object across all active subscriptions. Scheduling by media time
            // — rather than plan-array index — keeps tracks properly
            // interleaved when the plan lays tracks out track-by-track (e.g.
            // a single giant video group followed by a single giant audio
            // group). Ordering by plan index in that layout would send every
            // video object before any audio object, starving the audio track.
            std::size_t next_plan_index = plan.objects.size();
            std::size_t next_loop_cycle = 0;
            std::uint64_t next_media_time_us = 0;
            bool have_candidate = false;
            for (const auto& [request_id, active] : active_subscriptions) {
                static_cast<void>(request_id);
                if (active.completed) {
                    continue;
                }
                if (active.next_object_index >= plan.objects.size()) {
                    continue;
                }
                const std::uint64_t candidate_time_us = plan.objects[active.next_object_index].media_time_us;
                const bool is_earlier = !have_candidate ||
                                        active.loop_cycle < next_loop_cycle ||
                                        (active.loop_cycle == next_loop_cycle &&
                                         (candidate_time_us < next_media_time_us ||
                                          (candidate_time_us == next_media_time_us &&
                                           active.next_object_index < next_plan_index)));
                if (is_earlier) {
                    next_plan_index = active.next_object_index;
                    next_loop_cycle = active.loop_cycle;
                    next_media_time_us = candidate_time_us;
                    have_candidate = true;
                }
            }

            if (next_plan_index < plan.objects.size()) {
                const auto& source_object = plan.objects[next_plan_index];
                const openmoq::publisher::CmsfObject object =
                    make_looped_object(source_object, loop_state, next_loop_cycle);
                const auto payload = object_payload(source_object);
                if (payload.empty()) {
                    return TransportStatus::failure("transport publish requires materialized object payloads");
                }

                if (object.kind == openmoq::publisher::CmsfObjectKind::kMedia && !first_media_time_set) {
                    first_media_time_us = object.media_time_us;
                    first_media_time_set = true;
                }
                const std::uint64_t send_seq = object.kind == openmoq::publisher::CmsfObjectKind::kMedia ? next_send_seq() : 0;
                trace_pacing_decision("before", send_seq, pacing_start, first_media_time_us, object, paced);
                pace_until(pacing_start, first_media_time_us, object, paced);
                trace_pacing_decision("after", send_seq, pacing_start, first_media_time_us, object, paced);

                for (auto& [request_id, active] : active_subscriptions) {
                    if (active.loop_cycle != next_loop_cycle || active.next_object_index != next_plan_index) {
                        continue;
                    }

                    bool object_published = false;
                    const TransportStatus write_status = active.sender->serve(
                        transport,
                        draft,
                        active.track.alias,
                        send_seq,
                        object,
                        subgroup_contains_group_largest(plan, next_plan_index),
                        is_final_object_in_subgroup(plan, next_plan_index),
                        payload,
                        active.subscribe.delivery_timeouts,
                        now_function,
                        &object_published);
                    if (!write_status.ok) {
                        return write_status;
                    }
                    // Record stats for the batch/file path too. Without this,
                    // Publisher::stats() returns zeros for everything except
                    // publish_live, even though objects are flowing.
                    if (object_published && published_sink) {
                        published_sink(object.track_name,
                                       object.group_id,
                                       object_payload_size(source_object));
                    }
                    if (object_published) {
                        std::cerr << "[moqt-session] served object send_seq=" << send_seq
                                  << " now_ms=" << trace_elapsed_ms(std::chrono::steady_clock::now())
                                  << " track=" << object.track_name
                                  << " group=" << object.group_id << " object=" << object.object_id
                                  << " bytes=" << object_payload_size(source_object) << '\n';
                        if (object.track_name == "catalog") {
                            catalog_last_served_at = std::chrono::steady_clock::now();
                        }
                        trace_csv_write_served("served",
                                               send_seq,
                                               trace_elapsed_ms(std::chrono::steady_clock::now()),
                                               object);
                    }

                    std::size_t upcoming_object_index = 0;
                    if (find_next_matching_object_index(plan,
                                                        active.subscribe,
                                                        next_plan_index + 1,
                                                        upcoming_object_index)) {
                        active.next_object_index = upcoming_object_index;
                    } else {
                        static_cast<void>(request_id);
                        active.next_object_index = plan.objects.size();
                        active.completed =
                            !advance_subscription_to_next_loop_object(
                                plan,
                                loop_state,
                                active,
                                draft,
                                generation_availability);
                    }
                }

                served_any_subscription = true;
                continue;
            }

        }

        if (fin) {
            break;
        }

        std::vector<std::uint8_t> chunk;
        const auto read_timeout = uses_request_streams(draft)
                                      ? std::chrono::milliseconds(25)
                                      : subscriber_timeout;
        const TransportStatus read_status =
            transport.read_stream(control_read_stream_id, chunk, fin, read_timeout);
        if (!read_status.ok) {
            if (read_status.message == "timed out waiting for stream data" ||
                read_status.message == "no queued read for stream") {
                if (uses_request_streams(draft) &&
                    std::chrono::steady_clock::now() < await_subscribe_deadline) {
                    continue;
                }
                break;
            }
            if (!served_any_subscription && is_idle_subscribe_exit(read_status.message)) {
                break;
            }
            return read_status;
        }

        if (trace_enabled()) {
            std::cerr << "[moqt-session] control chunk now_ms="
                      << trace_elapsed_ms(std::chrono::steady_clock::now())
                      << " fin=" << (fin ? 1 : 0) << " bytes=[" << hex_dump(chunk)
                      << "]" << std::endl;
        }
        buffer.insert(buffer.end(), chunk.begin(), chunk.end());
    }

    if (!served_any_subscription) {
        std::cerr << "[moqt-session] no downstream SUBSCRIBE before timeout; "
                     "idle await-subscribe publish closing"
                  << " namespace=" << track_namespace
                  << " draft=" << openmoq::publisher::to_string(draft)
                  << " timeout_s=" << subscriber_timeout.count()
                  << " note=expecting relay/internal subscriber subscription"
                  << '\n';
        pending_control_bytes = std::move(buffer);
        if (!send_namespace_done) {
            return TransportStatus::success();
        }
        return write_namespace_done_for_request(transport, draft, control_stream_id, namespace_stream_id, namespace_message);
    }

    if (send_namespace_done && catalog_last_served_at.has_value()) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *catalog_last_served_at);
        const auto desired_grace = std::min(subscriber_timeout, std::chrono::milliseconds(250));
        if (elapsed < desired_grace) {
            const auto remaining = desired_grace - elapsed;
            std::cerr << "[moqt-session] grace wait after catalog publish: " << remaining.count() << " ms\n";
            std::this_thread::sleep_for(remaining);
        }
    }

    pending_control_bytes = std::move(buffer);
    if (!send_namespace_done) {
        return TransportStatus::success();
    }
    return write_namespace_done_for_request(transport, draft, control_stream_id, namespace_stream_id, namespace_message);
}

TransportStatus forward_published_tracks(PublisherTransport& transport,
                                         PublishedObjectSink published_sink,
                                         std::uint64_t control_stream_id,
                                         std::uint64_t namespace_stream_id,
                                         const openmoq::publisher::PublishPlan& plan,
                                         const LoopState& loop_state,
                                         std::span<const PublishedTrack> tracks,
                                         std::uint64_t peer_max_request_id,
                                         std::uint64_t local_max_request_id,
                                         std::string_view track_namespace,
                                         const std::optional<std::vector<std::uint8_t>>& authorization_token,
                                         bool paced,
                                         std::chrono::milliseconds subscriber_timeout,
                                         std::vector<std::uint8_t>& pending_control_bytes,
                                         std::map<std::uint64_t, std::uint64_t>& publish_stream_ids,
                                         const NowFunction& now_function) {
    std::map<std::string, std::uint64_t> request_id_by_track;
    std::map<std::string, PublishedTrack> tracks_by_name;
    std::map<std::uint64_t, PublishOk> publish_ok_by_request_id;
    std::uint64_t next_request_id = 2;

    for (const auto& track : tracks) {
        if (uses_peer_max_request_id(plan.draft.version) && next_request_id > peer_max_request_id) {
            return TransportStatus::failure("peer max_request_id is too small for publish requests");
        }

        request_id_by_track.emplace(track.name, next_request_id);
        tracks_by_name.emplace(track.name, track);

        const TrackMessage track_message{
            .draft = plan.draft.version,
            .track_name = track.name,
            .track_namespace = std::string(track_namespace),
            .request_id = next_request_id,
            .track_alias = track.alias,
            .largest_group_id = track.largest_group_id,
            .largest_object_id = track.largest_object_id,
            .content_exists = track.content_exists,
            .authorization_token = authorization_token,
        };
        TransportStatus status = TransportStatus::success();
        if (uses_request_streams(plan.draft.version)) {
            PublishOk publish_ok;
            std::uint64_t track_stream_id = 0;
            status = send_request_stream_and_wait(
                transport, plan.draft.version, encode_track_message(track_message), true, &publish_ok,
                &track_stream_id);
            if (status.ok) {
                publish_ok.request_id = next_request_id;
                publish_ok_by_request_id.insert_or_assign(next_request_id, publish_ok);
                publish_stream_ids.insert_or_assign(next_request_id, track_stream_id);
            }
        } else {
            status = transport.write_stream(control_stream_id, encode_track_message(track_message), false);
        }
        if (!status.ok) {
            return status;
        }
        std::cerr << "[moqt-session] publish track namespace=" << track_namespace << " track=" << track.name
                  << " request_id=" << next_request_id << " alias=" << track.alias << '\n';
        next_request_id += 2;
    }

    TransportStatus status = TransportStatus::success();
    if (!uses_request_streams(plan.draft.version)) {
        status = collect_control_acknowledgements(transport,
                                                  control_stream_id,
                                                  plan.draft.version,
                                                  0,
                                                  tracks.size(),
                                                  pending_control_bytes,
                                                  &publish_ok_by_request_id);
        if (!status.ok) {
            return status;
        }
    }

    std::map<std::string, SubgroupSenderState> sender_by_track;
    std::map<std::string, PublishedTrack> downgraded_tracks_by_name;
    const auto pacing_start = std::chrono::steady_clock::now();
    std::uint64_t first_media_time_us = 0;
    bool first_media_time_set = false;
    std::size_t loop_cycle = 0;
    std::size_t cycle_start_index = 0;

    while (true) {
        for (std::size_t object_index = cycle_start_index; object_index < plan.objects.size(); ++object_index) {
            const auto& source_object = plan.objects[object_index];
            const auto track_it = tracks_by_name.find(source_object.track_name);
            if (track_it == tracks_by_name.end()) {
                continue;
            }
            const auto publish_ok_it = publish_ok_by_request_id.find(request_id_by_track.at(source_object.track_name));
            if (publish_ok_it == publish_ok_by_request_id.end()) {
                return TransportStatus::failure("missing PUBLISH_OK for published track");
            }
            if (publish_ok_it->second.forward == 0) {
                std::cerr << "[moqt-session] publish accepted without forwarding track=" << source_object.track_name
                          << " request_id=" << request_id_by_track.at(source_object.track_name) << '\n';
                downgraded_tracks_by_name.emplace(track_it->first, track_it->second);
                continue;
            }

            const openmoq::publisher::CmsfObject object =
                make_looped_object(source_object, loop_state, loop_cycle);
            const auto payload = object_payload(source_object);
            if (payload.empty()) {
                return TransportStatus::failure("transport publish requires materialized object payloads");
            }
            if (object.kind == openmoq::publisher::CmsfObjectKind::kMedia && !first_media_time_set) {
                first_media_time_us = object.media_time_us;
                first_media_time_set = true;
            }
            const std::uint64_t send_seq = object.kind == openmoq::publisher::CmsfObjectKind::kMedia ? next_send_seq() : 0;
            trace_pacing_decision("before", send_seq, pacing_start, first_media_time_us, object, paced);
            pace_until(pacing_start, first_media_time_us, object, paced);
            trace_pacing_decision("after", send_seq, pacing_start, first_media_time_us, object, paced);

            bool object_published = false;
            status = sender_by_track[source_object.track_name].serve(
                transport,
                plan.draft.version,
                track_it->second.alias,
                send_seq,
                object,
                subgroup_contains_group_largest(plan, object_index),
                is_final_object_in_subgroup(plan, object_index),
                payload,
                publish_ok_it->second.delivery_timeouts,
                now_function,
                &object_published,
                {.subscriber = publish_ok_it->second.subscriber_priority,
                 .publisher = 128});
            if (!status.ok) {
                return status;
            }
            if (!object_published) {
                continue;
            }
            std::cerr << "[moqt-session] sent object send_seq=" << send_seq
                      << " now_ms=" << trace_elapsed_ms(std::chrono::steady_clock::now())
                      << " track=" << object.track_name << " group=" << object.group_id
                      << " object=" << object.object_id << " bytes=" << object_payload_size(source_object)
                      << " kind="
                      << (object.kind == openmoq::publisher::CmsfObjectKind::kInitialization ? "catalog" : "media")
                      << '\n';
            trace_csv_write_served("sent", send_seq, trace_elapsed_ms(std::chrono::steady_clock::now()), object);
        }

        if (!loop_state.enabled) {
            break;
        }

        cycle_start_index = plan.objects.size();
        for (const auto& [track_name, info] : loop_state.tracks) {
            static_cast<void>(track_name);
            if (info.has_loopable_objects) {
                cycle_start_index = std::min(cycle_start_index, info.first_loop_object_index);
            }
        }
        if (cycle_start_index >= plan.objects.size()) {
            break;
        }
        ++loop_cycle;
    }

    for (const auto& track : tracks) {
        const auto publish_ok_it = publish_ok_by_request_id.find(request_id_by_track.at(track.name));
        if (publish_ok_it == publish_ok_by_request_id.end()) {
            return TransportStatus::failure("missing PUBLISH_OK for published track");
        }
        if (publish_ok_it->second.forward == 0) {
            downgraded_tracks_by_name.emplace(track.name, track);
            continue;
        }
        if (loop_state.enabled && track_can_loop(loop_state, track.name)) {
            continue;
        }
        status = write_publish_done_for_request(transport,
                                                plan.draft.version,
                                                control_stream_id,
                                                publish_stream_ids,
                                                request_id_by_track.at(track.name),
                                                sender_by_track[track.name].stream_count());
        if (!status.ok) {
            return status;
        }
    }

    if (!downgraded_tracks_by_name.empty()) {
        std::cerr << "[moqt-session] waiting for downstream SUBSCRIBE on "
                  << downgraded_tracks_by_name.size() << " track(s) after forward=0 reply" << '\n';
        status = serve_subscriptions(transport,
                                     published_sink,
                                     control_stream_id,
                                     namespace_stream_id,
                                     control_stream_id,
                                     plan,
                                     loop_state,
                                     downgraded_tracks_by_name,
                                     plan.draft.version,
                                     track_namespace,
                                     paced,
                                     subscriber_timeout,
                                     pending_control_bytes,
                                     false,
                                     nullptr,
                                     nullptr,
                                     0,
                                     local_max_request_id,
                                     2,
                                     authorization_token,
                                     now_function);
        if (!status.ok) {
            return status;
        }
    }

    if (loop_state.enabled) {
        return TransportStatus::success();
    }

    return write_namespace_done_for_request(transport,
                                            plan.draft.version,
                                            control_stream_id,
                                            namespace_stream_id,
                                            {
                                                .draft = plan.draft.version,
                                                .track_namespace = std::string(track_namespace),
                                                .request_id = 0,
                                                .authorization_token = std::nullopt,
                                            });
}

TransportStatus publish_selected_tracks(PublisherTransport& transport,
                                        std::uint64_t control_stream_id,
                                        const openmoq::publisher::PublishPlan& plan,
                                        const LoopState& loop_state,
                                        std::span<const PublishedTrack> tracks,
                                        std::uint64_t peer_max_request_id,
                                        std::string_view track_namespace,
                                        const std::optional<std::vector<std::uint8_t>>& authorization_token,
                                        bool paced,
                                        std::vector<std::uint8_t>& pending_control_bytes,
                                        std::map<std::uint64_t, std::uint64_t>& publish_stream_ids,
                                        std::map<std::uint64_t, DormantPublishedTrack>* dormant_published_tracks,
                                        std::map<std::uint64_t, std::uint64_t>* request_id_by_track_alias,
                                        const NowFunction& now_function,
                                        std::uint64_t first_request_id) {
    if (tracks.empty()) {
        return TransportStatus::success();
    }

    std::map<std::string, std::uint64_t> request_id_by_track;
    std::map<std::string, PublishedTrack> tracks_by_name;
    std::map<std::uint64_t, PublishOk> publish_ok_by_request_id;
    std::uint64_t next_request_id = first_request_id;

    for (const auto& track : tracks) {
        if (uses_peer_max_request_id(plan.draft.version) && next_request_id > peer_max_request_id) {
            return TransportStatus::failure("peer max_request_id is too small for publish requests");
        }

        request_id_by_track.emplace(track.name, next_request_id);
        tracks_by_name.emplace(track.name, track);
        const TrackMessage track_message{
            .draft = plan.draft.version,
            .track_name = track.name,
            .track_namespace = std::string(track_namespace),
            .request_id = next_request_id,
            .track_alias = track.alias,
            .largest_group_id = track.largest_group_id,
            .largest_object_id = track.largest_object_id,
            .content_exists = track.content_exists,
            .authorization_token = authorization_token,
        };
        TransportStatus status = TransportStatus::success();
        if (uses_request_streams(plan.draft.version)) {
            PublishOk publish_ok;
            std::uint64_t track_stream_id = 0;
            status = send_request_stream_and_wait(
                transport, plan.draft.version, encode_track_message(track_message), true, &publish_ok,
                &track_stream_id);
            if (status.ok) {
                publish_ok.request_id = next_request_id;
                publish_ok_by_request_id.insert_or_assign(next_request_id, publish_ok);
                publish_stream_ids.insert_or_assign(next_request_id, track_stream_id);
            }
        } else {
            status = transport.write_stream(control_stream_id, encode_track_message(track_message), false);
        }
        if (!status.ok) {
            return status;
        }
        std::cerr << "[moqt-session] publish selected track namespace=" << track_namespace
                  << " track=" << track.name << " request_id=" << next_request_id
                  << " alias=" << track.alias << '\n';
        next_request_id += 2;
    }

    TransportStatus status = TransportStatus::success();
    if (!uses_request_streams(plan.draft.version)) {
        status = collect_control_acknowledgements(transport,
                                                  control_stream_id,
                                                  plan.draft.version,
                                                  0,
                                                  tracks.size(),
                                                  pending_control_bytes,
                                                  &publish_ok_by_request_id);
        if (!status.ok) {
            return status;
        }
    }

    const auto pacing_start = std::chrono::steady_clock::now();
    std::uint64_t first_media_time_us = 0;
    bool first_media_time_set = false;
    std::map<std::string, SubgroupSenderState> sender_by_track;

    std::size_t loop_cycle = 0;
    std::size_t cycle_start_index = 0;
    while (true) {
        for (std::size_t object_index = cycle_start_index; object_index < plan.objects.size(); ++object_index) {
            const auto& source_object = plan.objects[object_index];
            const auto track_it = tracks_by_name.find(source_object.track_name);
            if (track_it == tracks_by_name.end()) {
                continue;
            }

            const auto publish_ok_it = publish_ok_by_request_id.find(request_id_by_track.at(source_object.track_name));
            if (publish_ok_it == publish_ok_by_request_id.end()) {
                return TransportStatus::failure("missing PUBLISH_OK for published track");
            }
            if (publish_ok_it->second.forward == 0) {
                if (dormant_published_tracks != nullptr) {
                    dormant_published_tracks->insert_or_assign(
                        request_id_by_track.at(source_object.track_name),
                        DormantPublishedTrack{
                            .subscribe =
                                SubscribeMessage{
                                    .request_id = request_id_by_track.at(source_object.track_name),
                                    .track_namespace = split_track_namespace_components(track_namespace),
                                    .track_name = source_object.track_name,
                                    .subscriber_priority = publish_ok_it->second.subscriber_priority,
                                    .group_order = publish_ok_it->second.group_order,
                                    .forward = publish_ok_it->second.forward,
                                    .filter_type = publish_ok_it->second.filter_type == 0 ? 0x03 : publish_ok_it->second.filter_type,
                                    .start_group_id = 0,
                                    .start_object_id = 0,
                                    .end_group_id = 0,
                                    .delivery_timeouts = publish_ok_it->second.delivery_timeouts,
                                },
                            .track = track_it->second,
                        });
                }
                if (request_id_by_track_alias != nullptr) {
                    request_id_by_track_alias->insert_or_assign(track_it->second.alias,
                                                                request_id_by_track.at(source_object.track_name));
                }
                continue;
            }

            const openmoq::publisher::CmsfObject object =
                make_looped_object(source_object, loop_state, loop_cycle);
            const auto payload = object_payload(source_object);
            if (payload.empty()) {
                return TransportStatus::failure("transport publish requires materialized object payloads");
            }
            if (object.kind == openmoq::publisher::CmsfObjectKind::kMedia && !first_media_time_set) {
                first_media_time_us = object.media_time_us;
                first_media_time_set = true;
            }
            const std::uint64_t send_seq = object.kind == openmoq::publisher::CmsfObjectKind::kMedia ? next_send_seq() : 0;
            trace_pacing_decision("before", send_seq, pacing_start, first_media_time_us, object, paced);
            pace_until(pacing_start, first_media_time_us, object, paced);
            trace_pacing_decision("after", send_seq, pacing_start, first_media_time_us, object, paced);

            bool object_published = false;
            status = sender_by_track[source_object.track_name].serve(
                transport,
                plan.draft.version,
                track_it->second.alias,
                send_seq,
                object,
                subgroup_contains_group_largest(plan, object_index),
                is_final_object_in_subgroup(plan, object_index),
                payload,
                publish_ok_it->second.delivery_timeouts,
                now_function,
                &object_published,
                {.subscriber = publish_ok_it->second.subscriber_priority,
                 .publisher = 128});
            if (!status.ok) {
                return status;
            }
            if (!object_published) {
                continue;
            }
            std::cerr << "[moqt-session] sent selected object send_seq=" << send_seq
                      << " now_ms=" << trace_elapsed_ms(std::chrono::steady_clock::now())
                      << " track=" << object.track_name
                      << " group=" << object.group_id
                      << " object=" << object.object_id
                      << " bytes=" << object_payload_size(source_object)
                      << " kind="
                      << (object.kind == openmoq::publisher::CmsfObjectKind::kInitialization ? "catalog" : "media")
                      << '\n';
            trace_csv_write_served("sent_selected",
                                   send_seq,
                                   trace_elapsed_ms(std::chrono::steady_clock::now()),
                                   object);
        }

        if (!loop_state.enabled) {
            break;
        }

        cycle_start_index = plan.objects.size();
        for (const auto& track : tracks) {
            const TrackLoopInfo* info = find_track_loop_info(loop_state, track.name);
            if (info != nullptr && info->has_loopable_objects) {
                cycle_start_index = std::min(cycle_start_index, info->first_loop_object_index);
            }
        }
        if (cycle_start_index >= plan.objects.size()) {
            break;
        }
        ++loop_cycle;
    }

    for (const auto& track : tracks) {
        const auto publish_ok_it = publish_ok_by_request_id.find(request_id_by_track.at(track.name));
        if (publish_ok_it == publish_ok_by_request_id.end()) {
            return TransportStatus::failure("missing PUBLISH_OK for published track");
        }
        if (publish_ok_it->second.forward == 0) {
            std::cerr << "[moqt-session] selected publish accepted without forwarding track=" << track.name
                      << " request_id=" << request_id_by_track.at(track.name) << '\n';
            continue;
        }
        if (loop_state.enabled && track_can_loop(loop_state, track.name)) {
            continue;
        }
        status = write_publish_done_for_request(transport,
                                                plan.draft.version,
                                                control_stream_id,
                                                publish_stream_ids,
                                                request_id_by_track.at(track.name),
                                                sender_by_track[track.name].stream_count());
        if (!status.ok) {
            return status;
        }
    }

    return TransportStatus::success();
}

// RAII guard ensuring the catalog subscription's deferred PUBLISH_DONE (see
// each publish_live() overload's catalog_publish_done_deferred /
// catalog_publish_done_sender local variables) is sent on every exit path
// from publish_live() -- not just the graceful end-of-loop cleanup, but also
// the roughly dozen early `return status;` sites triggered by
// drain_queue()/process_control_messages()/maybe_republish_catalog()/
// read_stream() failures partway through the loop. Before this guard
// existed, those early returns skipped the catalog cleanup entirely,
// silently dropping the completion signal -- exactly the "worse than the
// one-shot default" outcome the deferral was meant to avoid, not cause.
// Hand-editing each early-return site was rejected deliberately: that is
// how one gets missed, and a future `return` added to the loop would
// silently reintroduce the bug. A scope guard, constructed once before the
// loop, fires unconditionally when the enclosing function returns by any
// path -- normal, error, or exception unwinding -- so there is nothing left
// to miss.
class DeferredCatalogPublishDoneGuard {
public:
    DeferredCatalogPublishDoneGuard(bool& deferred, std::function<TransportStatus()>& sender)
        : deferred_(deferred), sender_(sender) {}

    DeferredCatalogPublishDoneGuard(const DeferredCatalogPublishDoneGuard&) = delete;
    DeferredCatalogPublishDoneGuard& operator=(const DeferredCatalogPublishDoneGuard&) = delete;

    // Must not throw: this can run during stack unwinding, and a throwing
    // destructor there would call std::terminate. Any failure sending the
    // PUBLISH_DONE is logged, not propagated -- there is no TransportStatus
    // for a destructor to return to begin with.
    ~DeferredCatalogPublishDoneGuard() {
        if (!deferred_) {
            return;
        }
        try {
            if (sender_) {
                const TransportStatus status = sender_();
                if (!status.ok) {
                    std::cerr << "[moqt-session] warning: failed to send deferred catalog "
                                 "PUBLISH_DONE while exiting: "
                              << status.message << '\n';
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[moqt-session] warning: exception sending deferred catalog PUBLISH_DONE "
                         "while exiting: "
                      << e.what() << '\n';
        } catch (...) {
            std::cerr << "[moqt-session] warning: unknown exception sending deferred catalog "
                         "PUBLISH_DONE while exiting\n";
        }
        deferred_ = false;
    }

private:
    bool& deferred_;
    std::function<TransportStatus()>& sender_;
};

}  // namespace

namespace live_srt_internal {

TransportStatus exercise_live_srt_publish_flow_for_testing(
    PublisherTransport& transport,
    openmoq::publisher::DraftVersion draft,
    std::size_t* control_poll_count,
    bool trigger_resource_limit,
    std::uint64_t initial_object_timeout_ms,
    std::uint8_t initial_subscriber_priority,
    const NowFunction& now_function,
    const std::function<void()>& after_queue_admission) {
    std::atomic<bool> stop_requested = false;
    LiveSrtQueueAdapter queue(stop_requested);
    if (trigger_resource_limit) {
        openmoq::publisher::MediaFragment retained_fragment;
        retained_fragment.track_name = "video";
        retained_fragment.duration_us = 2'000'000;
        retained_fragment.is_video_keyframe = true;
        retained_fragment.payload.owned_bytes = {'V'};
        if (admit_live_srt_fragment(
                queue, std::move(retained_fragment), now_function) !=
            openmoq::publisher::LiveMediaAdmission::kAccepted) {
            return TransportStatus::failure(
                "failed to prepare live SRT stalled-admission test");
        }
    }

    std::map<std::string, SubgroupSenderState> sender_by_track;
    SubgroupSenderState& sender = sender_by_track["video"];
    std::map<std::uint64_t, SubscribeMessage> active_subscriptions;
    active_subscriptions.emplace(
        1,
        SubscribeMessage{
            .request_id = 1,
            .track_namespace = {},
            .track_name = "video",
            .subscriber_priority = initial_subscriber_priority,
            .group_order = 1,
            .forward = 1,
            .filter_type = 0x01,
            .delivery_timeouts =
                {.object_ms = initial_object_timeout_ms,
                 .subgroup_ms = 0},
        });
    std::map<std::uint64_t, std::uint64_t>
        active_subscription_stream_ids{{1, 5}};
    std::vector<std::uint8_t> pending_control_bytes;
    bool control_fin = false;
    LiveSrtPublishFlow::ProcessControlMessages process_control_messages;

    if (trigger_resource_limit) {
        for (std::uint64_t subgroup_id = 0; subgroup_id < 2;
             ++subgroup_id) {
            const std::uint8_t marker =
                subgroup_id == 0 ? 'A' : 'B';
            const openmoq::publisher::CmsfObject prefix_object{
                .kind = openmoq::publisher::CmsfObjectKind::kMedia,
                .track_name = "video",
                .group_id = 1,
                .subgroup_id = subgroup_id,
                .object_id = 0,
                .payload = {},
                .owned_payload = {marker},
            };
            const auto prefix_result = sender.try_serve(
                transport,
                draft,
                1,
                subgroup_id + 1,
                prefix_object,
                true,
                false,
                prefix_object.owned_payload,
                {},
                now_function,
                {.subscriber = initial_subscriber_priority,
                 .publisher = 128});
            if (!prefix_result.status.ok ||
                prefix_result.disposition !=
                    SubgroupSenderState::ServeDisposition::kAccepted) {
                return prefix_result.status.ok
                           ? TransportStatus::failure(
                                 "failed to prepare open SRT subgroup")
                           : prefix_result.status;
            }
        }

        process_control_messages = []() {
            return std::pair{TransportStatus::success(), std::size_t{0}};
        };
        LiveSrtPublishFlow flow(
            transport,
            draft,
            0,
            queue,
            pending_control_bytes,
            control_fin,
            process_control_messages,
            active_subscriptions,
            active_subscription_stream_ids,
            sender_by_track,
            now_function);
        std::size_t finalization_polls = 0;
        const TransportStatus finalization_status = flow.finish_group(
            sender,
            {},
            [&]() {
                ++finalization_polls;
                if (finalization_polls == 1) {
                    return;
                }
                if (control_poll_count != nullptr) {
                    ++*control_poll_count;
                }
                openmoq::publisher::MediaFragment overflow;
                overflow.track_name = "audio";
                overflow.start_time_us = 2'000'000;
                overflow.duration_us = 1;
                overflow.payload.owned_bytes = {'R'};
                static_cast<void>(admit_live_srt_fragment(
                    queue, std::move(overflow), now_function));
            });
        return flow.terminate_resource(finalization_status);
    }

    openmoq::publisher::MediaFragment queued_object;
    queued_object.track_name = "video";
    queued_object.group_id = 1;
    queued_object.object_id = 0;
    queued_object.duration_us = 1;
    queued_object.is_video_keyframe = true;
    queued_object.payload.owned_bytes = {'X'};
    if (admit_live_srt_fragment(
            queue, std::move(queued_object), now_function) !=
        openmoq::publisher::LiveMediaAdmission::kAccepted) {
        return TransportStatus::failure(
            "failed to queue live SRT admission test object");
    }
    if (after_queue_admission) {
        after_queue_admission();
    }
    std::optional<openmoq::publisher::MediaFragment> popped_object =
        queue.try_pop();
    if (!popped_object.has_value()) {
        return TransportStatus::failure(
            "failed to pop live SRT admission test object");
    }
    const auto object_available_at =
        live_srt_fragment_available_at(*popped_object);
    const openmoq::publisher::CmsfObject object{
        .kind = openmoq::publisher::CmsfObjectKind::kMedia,
        .track_name = popped_object->track_name,
        .group_id = popped_object->group_id,
        .subgroup_id = 0,
        .object_id = popped_object->object_id,
        .payload = {},
        .owned_payload = std::move(popped_object->payload.owned_bytes),
    };
    PeerRequestIdValidator peer_request_ids(draft, 100);
    const TransportStatus initial_request_id_status =
        peer_request_ids.validate(transport, 1);
    if (!initial_request_id_status.ok) {
        return initial_request_id_status;
    }
    process_control_messages = [&]()
        -> std::pair<TransportStatus, std::size_t> {
            if (control_poll_count != nullptr) {
                ++*control_poll_count;
            }
            std::size_t message_size = 0;
            while (next_control_message(
                pending_control_bytes, draft, message_size)) {
                const std::vector<std::uint8_t> message(
                    pending_control_bytes.begin(),
                    pending_control_bytes.begin() +
                        static_cast<std::ptrdiff_t>(message_size));
                const TransportStatus update_status =
                    process_draft16_control_request_update(
                        transport,
                        message,
                        0,
                        peer_request_ids,
                        [&](std::uint64_t existing_request_id,
                            std::uint64_t response_stream_id,
                            const RequestUpdateMessage& update) {
                            const auto active_it =
                                active_subscriptions.find(
                                    existing_request_id);
                            if (active_it ==
                                active_subscriptions.end()) {
                                return protocol_violation(
                                    transport,
                                    "REQUEST_UPDATE specified an invalid existing subscription");
                            }
                            apply_request_update(
                                active_it->second, update);
                            return transport.write_stream(
                                response_stream_id,
                                encode_request_ok_message(
                                    draft, update.request_id),
                                false);
                        });
                if (!update_status.ok) {
                    return {update_status, 0};
                }
                pending_control_bytes.erase(
                    pending_control_bytes.begin(),
                    pending_control_bytes.begin() +
                        static_cast<std::ptrdiff_t>(message_size));
            }
            return {TransportStatus::success(), 0};
        };
    LiveSrtPublishFlow flow(
        transport,
        draft,
        0,
        queue,
        pending_control_bytes,
        control_fin,
        process_control_messages,
        active_subscriptions,
        active_subscription_stream_ids,
        sender_by_track,
        now_function);
    bool published = false;
    const TransportStatus status = flow.serve_object(
        sender,
        1,
        1,
        object,
        object.owned_payload,
        object_available_at,
        [&]() {
            return LiveSrtSchedulingOptions{
                .delivery_timeouts = delivery_timeouts_for_track(
                    active_subscriptions, "video"),
                .priority =
                    {.subscriber = subscriber_priority_for_track(
                         active_subscriptions, "video"),
                     .publisher = 128},
            };
        },
        [&]() {
            return live_object_matches_request_union(
                object, draft, active_subscriptions);
        },
        &published);
    if (!is_live_srt_resource_limit(status)) {
        return status;
    }
    return flow.terminate_resource(status);
}

}  // namespace live_srt_internal

MoqtSession::MoqtSession(PublisherTransport& transport,
                         std::string track_namespace,
                         bool auto_forward,
                         bool publish_catalog,
                         bool paced,
                         std::chrono::seconds subscriber_timeout,
                         openmoq::publisher::cat4moq::AuthorizationConfig authorization,
                         NowFunction now_function)
    : MoqtSession(transport,
                  std::move(track_namespace),
                  auto_forward,
                  publish_catalog,
                  paced,
                  false,
                  subscriber_timeout,
                  std::move(authorization),
                  std::move(now_function)) {}

MoqtSession::MoqtSession(PublisherTransport& transport,
                         std::string track_namespace,
                         bool auto_forward,
                         bool publish_catalog,
                         bool paced,
                         bool loop,
                         std::chrono::seconds subscriber_timeout,
                         openmoq::publisher::cat4moq::AuthorizationConfig authorization,
                         NowFunction now_function)
    : transport_(transport),
      track_namespace_(std::move(track_namespace)),
      auto_forward_(auto_forward),
      publish_catalog_(publish_catalog),
      paced_(paced),
      loop_(loop),
      subscriber_timeout_(subscriber_timeout),
      authorization_(std::move(authorization)),
      now_function_(now_function ? std::move(now_function) : steady_now_function()) {}

void MoqtSession::reset_publish_stats() {
    publish_stats_ = PublishStats{};
    last_group_by_track_.clear();
}

void MoqtSession::record_published_object(const std::string& track_name,
                                          std::uint64_t group_id,
                                          std::size_t payload_bytes) {
    publish_stats_.bytes_published += static_cast<std::uint64_t>(payload_bytes);
    publish_stats_.objects_published += 1;
    auto [it, inserted] = last_group_by_track_.emplace(track_name, group_id);
    if (inserted || it->second != group_id) {
        publish_stats_.groups_published += 1;
        it->second = group_id;
    }
}

MoqtSession::PublishStats MoqtSession::publish_stats() const {
    return publish_stats_;
}

void MoqtSession::remember_catalog_delivery_timeouts(DeliveryTimeouts delivery_timeouts) {
    std::lock_guard<std::mutex> lock(catalog_delivery_timeouts_mutex_);
    catalog_delivery_timeouts_ = delivery_timeouts;
}

DeliveryTimeouts MoqtSession::catalog_delivery_timeouts_snapshot() const {
    std::lock_guard<std::mutex> lock(catalog_delivery_timeouts_mutex_);
    return catalog_delivery_timeouts_;
}

TransportStatus MoqtSession::send_catalog_objects(
    openmoq::publisher::DraftVersion draft_version,
    std::uint64_t track_alias,
    const std::vector<openmoq::publisher::CatalogObject>& objects,
    DeliveryTimeouts delivery_timeouts,
    std::size_t* streams_opened) {
    if (streams_opened != nullptr) {
        *streams_opened = 0;
    }
    for (const auto& object : objects) {
        // Precondition this function relies on, not an invariant that always
        // holds: a fresh SubgroupSenderState per call only opens a stream
        // correctly for object_id 0, the start of a subgroup.
        // CatalogPublisher::publish() returns deltas at object_id >= 1 in the
        // CURRENT group (MSF section 5.3) -- nothing in this codebase drives
        // that path onto the wire today (see docs/status.md), but the type
        // does not forbid it, and send_catalog() (this function's only
        // caller alongside force_independent()/end_broadcast(), which always
        // emit object_id 0) calls publish() directly. A subgroup maps to
        // exactly one MOQT stream, so a delta object would need to continue
        // an already-open stream, not start a fresh one via a new
        // SubgroupSenderState -- that would open a second stream for the
        // same (group, subgroup) key. Fail loudly rather than silently do
        // that if a future producer ever starts emitting deltas.
        if (object.object_id != 0) {
            return TransportStatus::failure(
                "send_catalog_objects only supports independent catalogs (object_id 0); a "
                "delta object would need to continue an already-open stream, not start a new "
                "one");
        }
        const openmoq::publisher::CmsfObject catalog_object{
            .kind = openmoq::publisher::CmsfObjectKind::kInitialization,
            .track_name = "catalog",
            .group_id = object.group_id,
            .subgroup_id = object.subgroup_id,
            .object_id = object.object_id,
            .media_time_us = 0,
            .media_duration_us = 0,
            .payload = {},
            .owned_payload = std::vector<std::uint8_t>(object.payload.begin(), object.payload.end()),
        };
        SubgroupSenderState catalog_sender;
        bool object_published = false;
        const TransportStatus status = catalog_sender.serve(
            transport_, draft_version, track_alias, 0,
            catalog_object, true, true,
            std::span<const std::uint8_t>(catalog_object.owned_payload),
            delivery_timeouts,
            now_function_,
            &object_published);
        if (streams_opened != nullptr) {
            *streams_opened += catalog_sender.stream_count();
        }
        if (!status.ok) {
            return status;
        }
        if (object_published) {
            record_published_object("catalog", object.group_id, object.payload.size());
        }
    }
    return TransportStatus::success();
}

std::optional<std::vector<std::uint8_t>> MoqtSession::setup_authorization_token() const {
    if (!authorization_.setup_token.has_value()) {
        return std::nullopt;
    }
    return authorization_.setup_token->bytes;
}

std::optional<std::vector<std::uint8_t>> MoqtSession::action_authorization_token() const {
    if (!authorization_.action_token.has_value()) {
        return std::nullopt;
    }
    return authorization_.action_token->bytes;
}

TransportStatus MoqtSession::connect(const EndpointConfig& endpoint, const TlsConfig& tls) {
    endpoint_ = endpoint;
    setup_complete_ = false;
    peer_max_request_id_ = 0;
    pending_control_bytes_.clear();
    reset_publish_stats();
    TransportStatus status = transport_.configure(endpoint, tls);
    if (!status.ok) {
        return status;
    }

    status = transport_.connect();
    if (!status.ok) {
        return status;
    }

    if (trace_enabled()) {
        std::cerr << "[moqt-session] transport connected id=" << transport_.connection_id()
                  << " transport="
                  << (endpoint.transport == openmoq::publisher::transport::TransportKind::kWebTransport ? "webtransport"
                                                                                                         : "raw")
                  << std::endl;
    }

    return TransportStatus::success();
}

TransportStatus MoqtSession::publish(const openmoq::publisher::PublishPlan& plan) {
    if (transport_.state() != ConnectionState::kConnected) {
        return TransportStatus::failure("transport is not connected");
    }

    TransportStatus status = ensure_setup(plan.draft.version);
    if (!status.ok) {
        if (trace_enabled()) {
            std::cerr << "[moqt-session] setup failed error=" << status.message << std::endl;
        }
        return status;
    }
    std::cout << "connection_id=" << transport_.connection_id() << '\n' << std::flush;
    const auto action_token = action_authorization_token();

    const std::vector<PublishedTrack> tracks = build_published_tracks(plan);
    const LoopState loop_state = build_loop_state(plan, loop_);

    NamespaceMessage namespace_message{
        .draft = plan.draft.version,
        .track_namespace = track_namespace_,
        .request_id = 0,
        .authorization_token = action_token,
    };
    if (uses_request_streams(plan.draft.version)) {
        status = send_request_stream_and_wait(
            transport_, plan.draft.version, encode_namespace_message(namespace_message), false, nullptr,
            &namespace_stream_id_);
        if (status.ok) {
            namespace_stream_open_ = true;
        }
    } else {
        status = write_frame(control_stream_id_, encode_namespace_message(namespace_message), false);
        if (status.ok) {
            status = collect_control_acknowledgements(
                transport_, control_stream_id_, plan.draft.version, 1, 0, pending_control_bytes_);
        }
    }
    if (!status.ok) {
        return status;
    }
    std::cerr << "[moqt-session] namespace published namespace=" << track_namespace_ << " tracks=" << tracks.size()
              << " mode=" << (auto_forward_ ? "forward" : "await-subscribe") << '\n';

    std::map<std::string, PublishedTrack> tracks_by_name;
    for (auto track : tracks) {
        tracks_by_name.emplace(track.name, track);
    }

    auto stats_sink = [this](const std::string& track, std::uint64_t group, std::size_t bytes) {
        this->record_published_object(track, group, bytes);
    };

    if (auto_forward_) {
        status = forward_published_tracks(
            transport_,
            stats_sink,
            control_stream_id_,
            namespace_stream_id_,
            plan,
            loop_state,
            tracks,
            peer_max_request_id_,
            advertised_max_request_id(endpoint_->transport),
            track_namespace_,
            action_token,
            paced_,
            subscriber_timeout_,
            pending_control_bytes_,
            publish_stream_id_by_request_id_,
            now_function_);
        if (status.ok && uses_request_streams(plan.draft.version) && !loop_state.enabled) {
            namespace_stream_open_ = false;
        }
        return status;
    }

    if (publish_catalog_) {
        std::map<std::uint64_t, DormantPublishedTrack> dormant_published_tracks;
        std::map<std::uint64_t, std::uint64_t> request_id_by_track_alias;
        std::vector<PublishedTrack> selected_tracks;
        for (const auto& track : tracks) {
            if (track.name == "catalog") {
                selected_tracks.push_back(track);
            }
        }
        if (!selected_tracks.empty()) {
            status = publish_selected_tracks(transport_,
                                             control_stream_id_,
                                             plan,
                                             loop_state,
                                             selected_tracks,
                                             peer_max_request_id_,
                                             track_namespace_,
                                             action_token,
                                             paced_,
                                             pending_control_bytes_,
                                             publish_stream_id_by_request_id_,
                                             &dormant_published_tracks,
                                             &request_id_by_track_alias,
                                             now_function_);
            if (!status.ok) {
                return status;
            }

            return serve_subscriptions(transport_,
                                       stats_sink,
                                       control_stream_id_,
                                       namespace_stream_id_,
                                       peer_control_stream_id_,
                                       plan,
                                       loop_state,
                                       tracks_by_name,
                                       plan.draft.version,
                                       track_namespace_,
                                       paced_,
                                       subscriber_timeout_,
                                       pending_control_bytes_,
                                       true,
                                       &dormant_published_tracks,
                                       &request_id_by_track_alias,
                                       peer_max_request_id_,
                                       advertised_max_request_id(endpoint_->transport),
                                       2 + (selected_tracks.size() * 2),
                                       action_token,
                                       now_function_);
        }
    }

    std::cerr << "[moqt-session] awaiting SUBSCRIBE for tracks:";
    for (const auto& track : tracks) {
        std::cerr << ' ' << track.name;
    }
    std::cerr << '\n';

    return serve_subscriptions(transport_,
                               stats_sink,
                               control_stream_id_,
                               namespace_stream_id_,
                               peer_control_stream_id_,
                               plan,
                               loop_state,
                               tracks_by_name,
                               plan.draft.version,
                               track_namespace_,
                               paced_,
                               subscriber_timeout_,
                               pending_control_bytes_,
                               true,
                               nullptr,
                               nullptr,
                               peer_max_request_id_,
                               advertised_max_request_id(endpoint_->transport),
                               2,
                               action_token,
                               now_function_);
}

TransportStatus MoqtSession::publish_live(const LiveIngestOptions& ingest,
                                          std::istream* stdin_input,
                                          openmoq::publisher::DraftVersion draft_version,
                                          bool split_cmaf_chunks,
                                          bool stream_per_object) {
    if (ingest.use_stdin && !ingest.srt_callers.empty()) {
        return TransportStatus::failure("mixed stdin+SRT ingest is not supported; use either stdin or srt");
    }
    if (ingest.use_stdin && stdin_input == nullptr) {
        return TransportStatus::failure("live ingest requested stdin but no stdin stream was provided");
    }
    if (!ingest.use_stdin && ingest.srt_callers.empty()) {
        return TransportStatus::failure("live ingest requires at least one active source");
    }
    if (ingest.use_stdin) {
        return publish_live(*stdin_input, draft_version, split_cmaf_chunks, stream_per_object);
    }

    // SRT-only path below.
    if (transport_.state() != ConnectionState::kConnected) {
        return TransportStatus::failure("transport is not connected");
    }

    TransportStatus status = ensure_setup(draft_version);
    if (!status.ok) {
        return status;
    }
    std::cout << "connection_id=" << transport_.connection_id() << '\n' << std::flush;
    const auto action_token = action_authorization_token();
    PeerRequestIdValidator peer_request_ids(
        draft_version, advertised_max_request_id(endpoint_->transport));

    // MoqtSession is not otherwise reused across broadcasts in this project
    // (publisher_api.cpp constructs a fresh session per publish), but nothing
    // enforces that. Reset catalog_publisher_ and its alias bookkeeping
    // unconditionally so a second publish_live() call on the same instance
    // starts a clean broadcast rather than finding `last_` already set from a
    // prior one -- which would make catalogs_equal() silently send no
    // catalog at all to this broadcast's subscribers.
    catalog_publisher_.reset();
    catalog_track_alias_known_ = false;
    remember_catalog_delivery_timeouts({});
    last_catalog_published_at_ = std::chrono::steady_clock::time_point{};

    std::atomic<bool> stop_requested = false;
    auto queue = std::make_shared<LiveSrtQueueAdapter>(stop_requested);

    std::vector<openmoq::publisher::LiveSrtCallerRuntimeConfig> srt_callers;
    srt_callers.reserve(ingest.srt_callers.size());
    for (const auto& caller : ingest.srt_callers) {
        openmoq::publisher::LiveSrtCallerRuntimeConfig config;
        config.id = caller.id;
        config.endpoint = caller.endpoint;
        config.fragment_on_keyframe = caller.fragment_on_keyframe;
        config.empty_moov = caller.empty_moov;
        config.default_base_moof = caller.default_base_moof;
        config.separate_moof_per_track = caller.separate_moof_per_track;
        config.target_fragment_duration_ms = caller.target_fragment_duration_ms;
        config.latency_ms = caller.latency_ms;
        config.auto_detect_program = caller.auto_detect_program;
        config.program_number = caller.program_number.value_or(0);
        config.has_program_number = caller.program_number.has_value();
        config.video_pid = caller.video_pid.value_or(0);
        config.has_video_pid = caller.video_pid.has_value();
        config.audio_pid = caller.audio_pid.value_or(0);
        config.has_audio_pid = caller.audio_pid.has_value();
        srt_callers.push_back(std::move(config));
    }

    const NowFunction live_srt_now_function = now_function_;
    openmoq::publisher::LiveSrtIngestManager srt_manager(
        std::move(srt_callers),
        [queue, live_srt_now_function](
            openmoq::publisher::MediaFragment&& fragment) {
            static_cast<void>(admit_live_srt_fragment(
                *queue, std::move(fragment), live_srt_now_function));
        },
        stop_requested);

    status = srt_manager.start();
    if (!status.ok) {
        return status;
    }
    const auto& srt_bootstrap = srt_manager.bootstrap();
    const auto& tracks = srt_bootstrap.tracks;

    if (tracks.empty()) {
        stop_requested = true;
        srt_manager.join();
        return TransportStatus::failure("no live tracks available from SRT source");
    }

    const std::vector<std::uint8_t> synthetic_init =
        openmoq::publisher::LiveSrtIngestManager::build_synthetic_init_segment(tracks);
    openmoq::publisher::LiveCatalog live_catalog =
        openmoq::publisher::build_live_catalog(tracks, synthetic_init, true);

    std::thread srt_join_thread([&srt_manager, queue]() {
        srt_manager.join();
        queue->mark_eof();
    });

    NamespaceMessage namespace_message{
        .draft = draft_version,
        .track_namespace = track_namespace_,
        .request_id = 0,
        .authorization_token = action_token,
    };
    if (draft_version == openmoq::publisher::DraftVersion::kDraft18) {
        status = send_request_stream_and_wait(
            transport_, draft_version, encode_namespace_message(namespace_message), false, nullptr,
            &namespace_stream_id_);
    } else {
        status = write_frame(control_stream_id_, encode_namespace_message(namespace_message), false);
        if (status.ok) {
            status = collect_control_acknowledgements(
                transport_, control_stream_id_, draft_version, 1, 0, pending_control_bytes_);
        }
    }
    if (!status.ok) {
        stop_requested = true;
        if (srt_join_thread.joinable()) srt_join_thread.join();
        return status;
    }

    std::map<std::string, std::uint64_t> alias_by_track;
    std::uint64_t next_alias = 0;
    alias_by_track.emplace("catalog", next_alias++);
    for (const auto& track : tracks) {
        alias_by_track.emplace(track.track_name, next_alias++);
    }
    catalog_track_alias_ = alias_by_track.at("catalog");
    catalog_track_alias_known_ = true;

    // draft-ietf-moq-transport-19 section 10.11: "A sender MUST NOT send
    // PUBLISH_DONE until it has closed all streams it will ever open ... for
    // a subscription." end_broadcast() can open a further independent-catalog
    // stream on this same alias at any point before the session ends --
    // regardless of catalog_republish_interval_ -- so PUBLISH_DONE for the
    // catalog subscription can never be sent at SUBSCRIBE time: it is always
    // deferred to this loop's own exit below (search
    // catalog_publish_done_deferred), by which point catalog_publisher_ can
    // no longer open a new stream (either genuinely finished, or ended via a
    // concurrent end_broadcast()). catalog_stream_count accumulates the
    // total number of catalog streams opened, for that deferred
    // PUBLISH_DONE's stream_count field. catalog_publish_done_sender holds
    // how to actually send it (set alongside catalog_publish_done_deferred
    // below, once the catalog subscription's request id and response stream
    // are known); catalog_publish_done_guard is what actually fires it on
    // every exit from this function, not just the graceful cleanup further
    // down -- see the guard class's own comment for why a scope guard is used
    // instead of hand-editing every early return.
    bool catalog_publish_done_deferred = false;
    std::uint64_t catalog_stream_count = 0;
    DeliveryTimeouts catalog_delivery_timeouts;
    std::function<TransportStatus()> catalog_publish_done_sender;
    DeferredCatalogPublishDoneGuard catalog_publish_done_guard(catalog_publish_done_deferred,
                                                               catalog_publish_done_sender);

    // Replaces the old one-shot `catalog_sent` latch: catalog_publisher_ owns
    // the group/object sequencing (MSF section 5) and, on the first call,
    // returns exactly the one independent-catalog object the old code sent by
    // hand; on any later call with an unchanged catalog it returns nothing,
    // which is what keeps repeat SUBSCRIBEs across the branches below a
    // no-op without needing their own latch.
    auto send_catalog = [&](std::uint64_t track_alias) -> TransportStatus {
        std::vector<openmoq::publisher::CatalogObject> objects;
        try {
            objects = catalog_publisher_.publish(live_catalog.msf_catalog);
        } catch (const std::runtime_error&) {
            // Publisher::end_broadcast() is documented to call
            // MoqtSession::end_broadcast() without holding a lock across it,
            // specifically so it can run concurrently with an in-progress
            // publish_live() -- so catalog_publisher_ may have already ended
            // on another thread. Treat that as "nothing to send" rather than
            // let the exception escape this TransportStatus-returning path.
            return TransportStatus::success();
        }
        if (objects.empty()) {
            return TransportStatus::success();
        }
        std::size_t streams_opened = 0;
        const TransportStatus status = send_catalog_objects(
            draft_version,
            track_alias,
            objects,
            catalog_delivery_timeouts,
            &streams_opened);
        if (status.ok) {
            last_catalog_published_at_ = std::chrono::steady_clock::now();
            catalog_stream_count += streams_opened;
        }
        return status;
    };

    // MSF section 5.3 cache-expiry republication: when configured, re-emit
    // an independent catalog on its own interval regardless of whether the
    // content changed. A no-op until the first send_catalog() call sets
    // last_catalog_published_at_.
    auto maybe_republish_catalog = [&]() -> TransportStatus {
        if (catalog_republish_interval_.count() <= 0) {
            return TransportStatus::success();
        }
        if (last_catalog_published_at_.time_since_epoch().count() == 0) {
            return TransportStatus::success();
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_catalog_published_at_ < catalog_republish_interval_) {
            return TransportStatus::success();
        }
        std::vector<openmoq::publisher::CatalogObject> objects;
        try {
            objects = catalog_publisher_.force_independent();
        } catch (const std::runtime_error&) {
            // See send_catalog's identical guard: a concurrent
            // end_broadcast() may have already ended catalog_publisher_.
            return TransportStatus::success();
        }
        if (objects.empty()) {
            return TransportStatus::success();
        }
        std::size_t streams_opened = 0;
        const TransportStatus status = send_catalog_objects(
            draft_version,
            catalog_track_alias_,
            objects,
            catalog_delivery_timeouts,
            &streams_opened);
        if (status.ok) {
            last_catalog_published_at_ = now;
            catalog_stream_count += streams_opened;
        }
        return status;
    };

    std::map<std::string, SubgroupSenderState> sender_by_track;
    std::map<std::string, std::uint64_t> last_group_id_by_track;
    std::map<std::uint64_t, SubscribeMessage> active_subscriptions;
    std::map<std::uint64_t, std::uint64_t> active_subscription_stream_ids;
    std::map<std::uint64_t, std::vector<std::uint8_t>>
        pending_subscription_request_bytes;
    std::set<std::string> subscribed_tracks;
    LargestObjectByTrack largest_object_by_track;
    std::function<std::pair<TransportStatus, std::size_t>()>
        process_control_messages;
    bool control_fin = false;
    LiveSrtPublishFlow live_srt_flow(
        transport_,
        draft_version,
        control_stream_id_,
        *queue,
        pending_control_bytes_,
        control_fin,
        process_control_messages,
        active_subscriptions,
        active_subscription_stream_ids,
        sender_by_track,
        now_function_);

    auto drain_queue = [&]() -> TransportStatus {
        while (true) {
            const TransportStatus queue_status =
                live_srt_flow.publishing_status(
                    TransportStatus::success());
            if (!queue_status.ok) {
                return queue_status;
            }
            std::optional<openmoq::publisher::MediaFragment> queued_fragment =
                queue->try_pop();
            if (!queued_fragment.has_value()) break;
            openmoq::publisher::MediaFragment fragment =
                std::move(*queued_fragment);
            const auto object_available_at =
                live_srt_fragment_available_at(fragment);

            note_largest_live_object(largest_object_by_track,
                                     fragment.track_name,
                                     fragment.group_id,
                                     fragment.object_id);

            if (!auto_forward_ && !subscribed_tracks.count(fragment.track_name)) {
                continue;
            }

            const auto alias_it = alias_by_track.find(fragment.track_name);
            if (alias_it == alias_by_track.end()) {
                continue;
            }

            const openmoq::publisher::CmsfObject object{
                .kind = openmoq::publisher::CmsfObjectKind::kMedia,
                .track_name = fragment.track_name,
                .group_id = fragment.group_id,
                .subgroup_id = 0,
                .object_id = fragment.object_id,
                .media_time_us = fragment.start_time_us,
                .media_duration_us = fragment.duration_us,
                .payload = {},
                .owned_payload = fragment.payload.owned_bytes,
            };
            if (!live_object_matches_request_union(
                    object,
                    draft_version,
                    active_subscriptions)) {
                continue;
            }

            auto& sender = sender_by_track[fragment.track_name];
            const DeliveryTimeouts delivery_timeouts =
                delivery_timeouts_for_track(active_subscriptions, fragment.track_name);
            const auto group_it = last_group_id_by_track.find(fragment.track_name);
            if (group_it != last_group_id_by_track.end() && group_it->second != static_cast<std::uint64_t>(fragment.group_id)) {
                TransportStatus finish_status =
                    live_srt_flow.finish_group(
                        sender, delivery_timeouts);
                if (!finish_status.ok) {
                    return finish_status;
                }
            }

            const std::uint64_t send_seq = next_send_seq();
            const std::span<const std::uint8_t> payload(fragment.payload.owned_bytes);
            bool object_published = false;
            const TransportStatus write_status =
                live_srt_flow.serve_object(
                    sender,
                    alias_it->second,
                    send_seq,
                    object,
                    payload,
                    object_available_at,
                    [&]() {
                        return LiveSrtSchedulingOptions{
                            .delivery_timeouts = delivery_timeouts_for_track(
                                active_subscriptions,
                                fragment.track_name),
                            .priority =
                                {.subscriber = subscriber_priority_for_track(
                                     active_subscriptions,
                                     fragment.track_name),
                                 .publisher = 128},
                        };
                    },
                    [&]() {
                        return (auto_forward_ ||
                                subscribed_tracks.count(
                                    fragment.track_name) != 0) &&
                               live_object_matches_request_union(
                                   object,
                                   draft_version,
                                   active_subscriptions);
                    },
                    &object_published);
            if (!write_status.ok) {
                return write_status;
            }
            if (!object_published) {
                continue;
            }
            last_group_id_by_track[fragment.track_name] = static_cast<std::uint64_t>(fragment.group_id);
            record_published_object(fragment.track_name,
                                    static_cast<std::uint64_t>(fragment.group_id),
                                    fragment.payload.owned_bytes.size());
            if (publish_stats_.objects_published % 100 == 1) {
                std::cerr << "[moqt-session] published track=" << fragment.track_name
                          << " group=" << fragment.group_id << " obj=" << fragment.object_id
                          << " bytes=" << fragment.payload.owned_bytes.size()
                          << " total_objects=" << publish_stats_.objects_published
                          << " total_groups=" << publish_stats_.groups_published << std::endl;
            }
        }
        return TransportStatus::success();
    };

    const auto apply_subscriber_request_update =
        [&](std::uint64_t existing_request_id,
            std::uint64_t response_stream_id,
            const RequestUpdateMessage& update) -> TransportStatus {
            const auto active_it =
                active_subscriptions.find(existing_request_id);
            if (active_it == active_subscriptions.end()) {
                return protocol_violation(
                    transport_,
                    "REQUEST_UPDATE specified an invalid existing subscription");
            }
            if (update.new_group_request.has_value()) {
                return protocol_violation(
                    transport_,
                    "REQUEST_UPDATE NEW_GROUP requires negotiated DYNAMIC_GROUPS");
            }
            const std::string track_name = active_it->second.track_name;
            const bool renew_peer_stopped_subgroups =
                request_update_renews_peer_stopped_subgroups(
                    draft_version, active_it->second, update);
            apply_request_update(active_it->second, update);
            if (renew_peer_stopped_subgroups) {
                sender_by_track[track_name].renew_peer_stopped_subgroups(
                    transport_);
            }
            note_delivery_timeouts(
                transport_, active_it->second.delivery_timeouts);
            const bool forward = std::any_of(
                active_subscriptions.begin(),
                active_subscriptions.end(),
                [&](const auto& entry) {
                    return entry.second.track_name == track_name &&
                           entry.second.forward != 0;
                });
            if (track_name == "catalog") {
                catalog_delivery_timeouts = delivery_timeouts_for_track(
                    active_subscriptions, "catalog");
                remember_catalog_delivery_timeouts(
                    catalog_delivery_timeouts);
                if (forward) {
                    const auto alias_it = alias_by_track.find(track_name);
                    if (alias_it != alias_by_track.end()) {
                        const TransportStatus catalog_status =
                            send_catalog(alias_it->second);
                        if (!catalog_status.ok) {
                            return catalog_status;
                        }
                    }
                }
            } else if (forward) {
                subscribed_tracks.insert(track_name);
            } else {
                subscribed_tracks.erase(track_name);
            }
            const std::vector<std::uint8_t> response =
                encode_live_request_ok_message(draft_version,
                                               update.request_id,
                                               track_name,
                                               largest_object_by_track);
            return transport_.write_stream(
                response_stream_id, response, false);
        };

    process_control_messages = [&]() -> std::pair<TransportStatus, std::size_t> {
        std::size_t new_subs = 0;
        TransportStatus update_status =
            poll_retained_subscribe_request_updates(
                transport_,
                draft_version,
                active_subscriptions,
                active_subscription_stream_ids,
                pending_subscription_request_bytes,
                peer_request_ids,
                apply_subscriber_request_update);
        if (!update_status.ok) {
            return {update_status, 0};
        }

        if (uses_request_streams(draft_version)) {
            while (true) {
                std::uint64_t request_stream_id = 0;
                const TransportStatus accept_status =
                    transport_.accept_stream(
                        StreamDirection::kBidirectional,
                        request_stream_id,
                        std::chrono::milliseconds(0));
                if (!accept_status.ok) {
                    if (accept_status.message ==
                        "timed out waiting for stream data") {
                        break;
                    }
                    return {accept_status, 0};
                }
                const TransportStatus priority_status =
                    assign_request_stream_priority(
                        transport_, draft_version, request_stream_id);
                if (!priority_status.ok) {
                    return {priority_status, 0};
                }

                std::vector<std::uint8_t> request_bytes;
                std::vector<std::uint8_t> trailing_request_bytes;
                const TransportStatus read_status =
                    read_request_stream_message(
                        transport_,
                        request_stream_id,
                        draft_version,
                        subscriber_timeout_,
                        request_bytes,
                        &trailing_request_bytes);
                if (!read_status.ok) {
                    return {read_status, 0};
                }
                std::size_t request_offset = 0;
                std::uint64_t request_type = 0;
                if (!decode_moqint(
                        request_bytes,
                        request_offset,
                        draft_version,
                        request_type)) {
                    return {protocol_violation(
                                transport_,
                                "failed to parse request stream type"),
                            0};
                }
                if (request_type == 0x50) {
                    SubscribeNamespaceMessage request;
                    if (!decode_subscribe_namespace_message(
                            request_bytes, draft_version, request)) {
                        return {protocol_violation(
                                    transport_,
                                    "received invalid SUBSCRIBE_NAMESPACE"),
                                0};
                    }
                    const TransportStatus request_id_status =
                        peer_request_ids.validate(
                            transport_, request.request_id);
                    if (!request_id_status.ok) {
                        return {request_id_status, 0};
                    }
                    const TransportStatus response_status =
                        transport_.write_stream(
                            request_stream_id,
                            encode_request_ok_message(
                                draft_version, request.request_id),
                            false);
                    if (!response_status.ok) {
                        return {response_status, 0};
                    }
                    continue;
                }
                SubscribeMessage subscribe;
                if (request_type != 0x03 ||
                    !decode_subscribe_message(
                        request_bytes, draft_version, subscribe)) {
                    return {protocol_violation(
                                transport_,
                                "received unsupported request stream"),
                            0};
                }
                const TransportStatus request_id_status =
                    peer_request_ids.validate(
                        transport_, subscribe.request_id);
                if (!request_id_status.ok) {
                    return {request_id_status, 0};
                }
                const auto track_it =
                    alias_by_track.find(subscribe.track_name);
                if (track_it == alias_by_track.end()) {
                    const TransportStatus response_status =
                        transport_.write_stream(
                            request_stream_id,
                            encode_request_error_message(
                                draft_version,
                                subscribe.request_id,
                                0x2,
                                0,
                                "track does not exist"),
                            true);
                    if (!response_status.ok) {
                        return {response_status, 0};
                    }
                    continue;
                }
                const TransportStatus response_status =
                    transport_.write_stream(
                        request_stream_id,
                        encode_subscribe_ok_message(
                            draft_version,
                            subscribe.request_id,
                            track_it->second,
                            0,
                            0,
                            false),
                        false);
                if (!response_status.ok) {
                    return {response_status, 0};
                }
                active_subscriptions.insert_or_assign(
                    subscribe.request_id, subscribe);
                active_subscription_stream_ids.insert_or_assign(
                    subscribe.request_id, request_stream_id);
                pending_subscription_request_bytes.insert_or_assign(
                    subscribe.request_id,
                    std::move(trailing_request_bytes));
                note_delivery_timeouts(
                    transport_, subscribe.delivery_timeouts);
                ++new_subs;
                if (subscribe.track_name == "catalog") {
                    catalog_delivery_timeouts = delivery_timeouts_for_track(
                        active_subscriptions, "catalog");
                    remember_catalog_delivery_timeouts(
                        catalog_delivery_timeouts);
                    if (subscription_forwards_objects(
                            draft_version, subscribe.forward)) {
                        const TransportStatus catalog_status =
                            send_catalog(track_it->second);
                        if (!catalog_status.ok) {
                            return {catalog_status, 0};
                        }
                    }
                    catalog_publish_done_deferred = true;
                    catalog_publish_done_sender =
                        [&, request_id = subscribe.request_id,
                         response_stream_id = request_stream_id]() {
                            return transport_.write_stream(
                                response_stream_id,
                                encode_publish_done_message(
                                    draft_version,
                                    request_id,
                                    catalog_stream_count),
                                false);
                        };
                } else if (subscription_forwards_objects(
                               draft_version, subscribe.forward)) {
                    subscribed_tracks.insert(subscribe.track_name);
                }
                const TransportStatus retained_update_status =
                    poll_retained_subscribe_request_updates(
                        transport_,
                        draft_version,
                        active_subscriptions,
                        active_subscription_stream_ids,
                        pending_subscription_request_bytes,
                        peer_request_ids,
                        apply_subscriber_request_update);
                if (!retained_update_status.ok) {
                    return {retained_update_status, 0};
                }
            }
        }

        std::size_t message_size = 0;
        while (next_control_message(pending_control_bytes_, draft_version, message_size)) {
            const std::vector<std::uint8_t> message_bytes(
                pending_control_bytes_.begin(),
                pending_control_bytes_.begin() + static_cast<std::ptrdiff_t>(message_size));
            std::size_t offset = 0;
            std::uint64_t message_type = 0;
            if (!decode_varint(message_bytes, offset, message_type)) {
                return {TransportStatus::failure("failed to parse control request type"), 0};
            }

            if ((draft_version == DraftVersion::kDraft18 &&
                 message_type == 0x02) ||
                (uses_request_streams(draft_version) &&
                 (message_type == 0x03 || message_type == 0x06 ||
                  message_type == 0x50 || message_type == 0x16 ||
                  message_type == 0x1d || message_type == 0x51))) {
                return {protocol_violation(
                            transport_,
                            "draft-18 request message received on control stream"),
                        0};
            }

            if (message_type == 0x02 &&
                draft_version == DraftVersion::kDraft16) {
                const TransportStatus request_update_status =
                    process_draft16_control_request_update(
                        transport_,
                        message_bytes,
                        control_stream_id_,
                        peer_request_ids,
                        apply_subscriber_request_update);
                if (!request_update_status.ok) {
                    return {request_update_status, 0};
                }
            } else if (message_type == 0x03) {
                SubscribeMessage subscribe;
                if (!decode_subscribe_message(message_bytes, draft_version, subscribe)) {
                    return {TransportStatus::failure("received invalid SUBSCRIBE"), 0};
                }
                const TransportStatus request_id_status =
                    peer_request_ids.validate(transport_, subscribe.request_id);
                if (!request_id_status.ok) {
                    return {request_id_status, 0};
                }
                note_delivery_timeouts(transport_, subscribe.delivery_timeouts);

                const auto track_it = alias_by_track.find(subscribe.track_name);
                if (track_it == alias_by_track.end()) {
                    auto ws = transport_.write_stream(control_stream_id_,
                        encode_subscribe_error_message(subscribe.request_id, 0x2, "track does not exist"), false);
                    if (!ws.ok) {
                        return {ws, 0};
                    }
                } else {
                    auto ws = transport_.write_stream(control_stream_id_,
                        encode_subscribe_ok_message(draft_version, subscribe.request_id,
                                                    track_it->second, 0, 0, false), false);
                    if (!ws.ok) {
                        return {ws, 0};
                    }
                    active_subscriptions.emplace(subscribe.request_id, subscribe);
                    active_subscription_stream_ids.insert_or_assign(
                        subscribe.request_id, control_stream_id_);
                    ++new_subs;

                    if (subscribe.track_name == "catalog") {
                        catalog_delivery_timeouts = delivery_timeouts_for_track(
                            active_subscriptions, "catalog");
                        remember_catalog_delivery_timeouts(catalog_delivery_timeouts);
                        if (subscription_forwards_objects(
                                draft_version, subscribe.forward)) {
                            ws = send_catalog(track_it->second);
                            if (!ws.ok) {
                                return {ws, 0};
                            }
                        }
                        // section 10.11 MUST NOT: end_broadcast() can open a
                        // further catalog stream on this alias at any point
                        // before the session ends, regardless of
                        // catalog_republish_interval_, so PUBLISH_DONE for the
                        // catalog subscription is always deferred to this
                        // loop's own exit (or, on an early error return, to
                        // catalog_publish_done_guard) -- never sent
                        // immediately at SUBSCRIBE time.
                        catalog_publish_done_deferred = true;
                        catalog_publish_done_sender = [&, request_id = subscribe.request_id]() -> TransportStatus {
                            return transport_.write_stream(
                                control_stream_id_,
                                encode_publish_done_message(draft_version, request_id, catalog_stream_count),
                                false);
                        };
                    } else if (subscription_forwards_objects(
                                   draft_version, subscribe.forward)) {
                        subscribed_tracks.insert(subscribe.track_name);
                    }
                }
            } else if (message_type == 0x11) {
                SubscribeNamespaceMessage subscribe_namespace;
                if (decode_subscribe_namespace_message(message_bytes, draft_version, subscribe_namespace)) {
                    const TransportStatus request_id_status =
                        peer_request_ids.validate(
                            transport_, subscribe_namespace.request_id);
                    if (!request_id_status.ok) {
                        return {request_id_status, 0};
                    }
                    auto ws = transport_.write_stream(control_stream_id_,
                        encode_subscribe_namespace_ok_message(draft_version, subscribe_namespace.request_id), false);
                    if (!ws.ok) {
                        return {ws, 0};
                    }
                }
            }

            pending_control_bytes_.erase(
                pending_control_bytes_.begin(),
                pending_control_bytes_.begin() + static_cast<std::ptrdiff_t>(message_size));
        }
        return {TransportStatus::success(), new_subs};
    };

    while (true) {
        const bool is_eof = queue->done();

        status = drain_queue();
        if (!status.ok) {
            break;
        }

        auto [ctrl_status, _new_subs] = process_control_messages();
        if (!ctrl_status.ok) {
            status = ctrl_status;
            break;
        }

        status = maybe_republish_catalog();
        if (!status.ok) {
            break;
        }

        std::vector<std::uint8_t> chunk;
        bool immediate_fin = false;
        const TransportStatus read_status =
            transport_.read_stream(control_stream_id_, chunk, immediate_fin, std::chrono::milliseconds(0));
        if (read_status.ok) {
            pending_control_bytes_.insert(pending_control_bytes_.end(), chunk.begin(), chunk.end());
            control_fin = immediate_fin;
        }

        if (control_fin || is_eof) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    stop_requested = true;
    if (srt_join_thread.joinable()) {
        srt_join_thread.join();
    }
    status = live_srt_flow.publishing_status(std::move(status));

    if (is_live_srt_resource_limit(status)) {
        return live_srt_flow.terminate_resource(std::move(status));
    }

    for (auto& [track_name, sender] : sender_by_track) {
        const DeliveryTimeouts delivery_timeouts =
            delivery_timeouts_for_track(active_subscriptions, track_name);
        TransportStatus finish_status =
            live_srt_flow.finish_group(sender, delivery_timeouts);
        if (!finish_status.ok && status.ok) {
            status = finish_status;
        }
    }

    if (is_live_srt_resource_limit(status)) {
        return live_srt_flow.terminate_resource(std::move(status));
    }

    if (!status.ok) {
        return status;
    }

    for (const auto& [request_id, subscribe] : active_subscriptions) {
        if (subscribe.track_name == "catalog") {
            // By now catalog_publisher_ can no longer open a new stream for
            // this subscription -- either the loop above is genuinely done
            // (no more catalog sends will happen), or a concurrent
            // end_broadcast() already ended catalog_publisher_ and every
            // subsequent maybe_republish_catalog()/send_catalog() call has
            // been a no-op. Section 10.11's MUST NOT is satisfied either way.
            if (catalog_publish_done_deferred) {
                status = transport_.write_stream(control_stream_id_,
                    encode_publish_done_message(draft_version, request_id, catalog_stream_count), false);
                if (!status.ok) {
                    return status;
                }
                catalog_publish_done_deferred = false;
            }
            continue;
        }
        status = transport_.write_stream(control_stream_id_,
            encode_publish_done_message(draft_version, request_id, sender_by_track[subscribe.track_name].stream_count()), false);
        if (!status.ok) {
            return status;
        }
    }

    return transport_.write_stream(control_stream_id_,
        encode_publish_namespace_done_message(namespace_message), false);
}

namespace {

// Reads live input for publish_live, appending to the streaming reader and
// returning the byte count (0 on end of stream or stop request). For real
// stdin on POSIX, reads the fd directly behind a bounded poll: istream::read
// would park until a full chunk arrives, which made shutdown joins hang on a
// feeder that is alive but idle, and mixing istream reads with raw fd reads
// would lose bytes buffered inside cin/stdio — so every stdin byte in this
// flow must come through here. A null stop means wait indefinitely for data,
// matching the blocking semantics of the pre-thread discovery phase.
std::size_t read_live_input(std::istream& input,
                            openmoq::publisher::StreamingMp4Reader& reader,
                            const std::atomic<bool>* stop) {
#if !defined(_WIN32)
    if (&input == &std::cin) {
        constexpr int kStdinPollTimeoutMsec = 100;
        std::array<std::uint8_t, 16384> buffer;
        while (stop == nullptr || !stop->load(std::memory_order_acquire)) {
            pollfd poll_fd{};
            poll_fd.fd = STDIN_FILENO;
            poll_fd.events = POLLIN;
            const int ready = ::poll(&poll_fd, 1, kStdinPollTimeoutMsec);
            if (ready <= 0) {
                continue;
            }
            const ssize_t count = ::read(STDIN_FILENO, buffer.data(), buffer.size());
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                return 0;
            }
            if (count == 0) {
                return 0;
            }
            reader.append(buffer.data(), static_cast<std::size_t>(count));
            return static_cast<std::size_t>(count);
        }
        return 0;
    }
#endif
    return reader.read_from(input);
}

}  // namespace

TransportStatus MoqtSession::publish_live(std::istream& input,
                                           openmoq::publisher::DraftVersion draft_version,
                                           bool /*split_cmaf_chunks*/,
                                           bool stream_per_object) {
    if (transport_.state() != ConnectionState::kConnected) {
        return TransportStatus::failure("transport is not connected");
    }

    TransportStatus status = ensure_setup(draft_version);
    if (!status.ok) {
        return status;
    }
    std::cout << "connection_id=" << transport_.connection_id() << '\n' << std::flush;
    const auto action_token = action_authorization_token();
    PeerRequestIdValidator peer_request_ids(
        draft_version, advertised_max_request_id(endpoint_->transport));

    // MoqtSession is not otherwise reused across broadcasts in this project
    // (publisher_api.cpp constructs a fresh session per publish), but nothing
    // enforces that. Reset catalog_publisher_ and its alias bookkeeping
    // unconditionally so a second publish_live() call on the same instance
    // starts a clean broadcast rather than finding `last_` already set from a
    // prior one -- which would make catalogs_equal() silently send no
    // catalog at all to this broadcast's subscribers.
    catalog_publisher_.reset();
    catalog_track_alias_known_ = false;
    remember_catalog_delivery_timeouts({});
    last_catalog_published_at_ = std::chrono::steady_clock::time_point{};

    // Phase 1: Read stdin until we have ftyp + moov (track discovery).
    openmoq::publisher::StreamingMp4Reader reader;
    std::vector<std::uint8_t> ftyp_bytes;
    std::vector<std::uint8_t> moov_bytes;
    std::vector<openmoq::publisher::TrackDescription> tracks;
    std::vector<openmoq::publisher::Mp4Box> init_top_level_boxes;

    std::cerr << "[moqt-session] live: waiting for ftyp+moov from stdin...\n";

    while (ftyp_bytes.empty() || moov_bytes.empty()) {
        const std::size_t bytes_read = read_live_input(input, reader, nullptr);
        if (bytes_read == 0 && ftyp_bytes.empty()) {
            return TransportStatus::failure("stdin EOF before ftyp box");
        }
        if (bytes_read == 0 && moov_bytes.empty()) {
            return TransportStatus::failure("stdin EOF before moov box");
        }

        while (auto box = reader.next_box()) {
            if (box->type == "ftyp") {
                ftyp_bytes = std::move(box->bytes);
            } else if (box->type == "moov") {
                moov_bytes = std::move(box->bytes);
                break;  // Stop consuming boxes; remaining data is for Phase 4.
            }
            // Skip other pre-moov boxes (e.g. free, styp)
        }
    }

    // Build initialization segment (ftyp + moov)
    std::vector<std::uint8_t> init_segment;
    init_segment.reserve(ftyp_bytes.size() + moov_bytes.size());
    init_segment.insert(init_segment.end(), ftyp_bytes.begin(), ftyp_bytes.end());
    init_segment.insert(init_segment.end(), moov_bytes.begin(), moov_bytes.end());

    init_top_level_boxes = openmoq::publisher::parse_mp4_boxes(init_segment);
    tracks = openmoq::publisher::extract_tracks(init_top_level_boxes, init_segment);
    if (tracks.empty()) {
        return TransportStatus::failure("no tracks found in moov box");
    }

    std::cerr << "[moqt-session] live: discovered " << tracks.size() << " track(s): "
              << openmoq::publisher::summarize_tracks(tracks) << '\n';

    // Build catalog and init data. A protected track with no pssh anywhere in
    // the init segment (CMSF 4.1.2: an absent contentProtectionRefIDs means
    // "not protected") makes build_live_catalog throw std::runtime_error
    // rather than silently omitting the protection entry. Catch it here and
    // return a failure status instead of letting it unwind past this
    // function: the caller (Publisher::publish_live in publisher_api.cpp)
    // only runs its normal teardown -- close(0) + clear_active_session(),
    // which populates stats_.last_error and closes the MOQT session cleanly
    // -- when publish_live() returns rather than throws.
    openmoq::publisher::LiveCatalog live_catalog;
    try {
        live_catalog = openmoq::publisher::build_live_catalog(tracks, init_segment, true);
    } catch (const std::runtime_error& error) {
        return TransportStatus::failure(std::string("live catalog build failed: ") + error.what());
    }

    // Phase 2: Publish namespace + PUBLISH all tracks
    NamespaceMessage namespace_message{
        .draft = draft_version,
        .track_namespace = track_namespace_,
        .request_id = 0,
        .authorization_token = action_token,
    };
    if (uses_request_streams(draft_version)) {
        status = send_request_stream_and_wait(
            transport_, draft_version, encode_namespace_message(namespace_message), false, nullptr,
            &namespace_stream_id_);
        if (status.ok) {
            namespace_stream_open_ = true;
        }
        if (!status.ok) {
            return status;
        }
    } else {
        status = write_frame(control_stream_id_, encode_namespace_message(namespace_message), false);
        if (!status.ok) {
            // The write itself failed -- the transport is broken. Always fatal.
            return status;
        }
        status = collect_control_acknowledgements(
            transport_, control_stream_id_, draft_version, 1, 0, pending_control_bytes_);
        if (!status.ok) {
            if (status.message != "timed out waiting for stream data") {
                // A real transport failure (connection closed/reset, malformed
                // response, peer rejection, ...) -- still fatal.
                return status;
            }
            // Only the per-read wait (collect_control_acknowledgements's 2s
            // read_stream timeout) elapsed with no PUBLISH_NAMESPACE_OK/
            // REQUEST_OK; the connection itself is healthy. Per the live-publish
            // contract, a missing/late namespace acknowledgement must not tear
            // the session down: moqxr stays ready, publishes its tracks, and
            // waits for subscriptions until the RTMP source itself ends. Any
            // ack that does arrive later is still sitting in
            // pending_control_bytes_ (collect_control_acknowledgements
            // preserves it even on this early return) and will be parsed
            // harmlessly -- and silently, since it isn't one of the message
            // types process_control_messages() acts on -- once the
            // await-subscriptions loop below starts draining control bytes.
            std::cerr << "[moqt-session] live: no namespace acknowledgement within timeout; "
                         "continuing without waiting further (relay may ack late or not at all)\n";
        }
    }

    // Build track alias map (self-assigned, matching legacy serve_subscriptions behavior)
    std::map<std::string, std::uint64_t> alias_by_track;
    std::uint64_t next_alias = 0;
    alias_by_track.emplace("catalog", next_alias++);
    for (const auto& track : tracks) {
        alias_by_track.emplace(track.track_name, next_alias++);
    }
    catalog_track_alias_ = alias_by_track.at("catalog");
    catalog_track_alias_known_ = true;

    // Map PUBLISH request_ids to track names so we can handle PUBLISH_OK from
    // relays that accept tracks via PUBLISH_OK without forwarding SUBSCRIBE.
    std::map<std::uint64_t, std::string> publish_request_id_to_track;

    // Preannounce media tracks so relay implementations that need explicit
    // PUBLISH before subscribing can discover them. Do not wait for PUBLISH_OK
    // here: some relays first send SUBSCRIBE or stay idle until a subscriber
    // appears, and live await-subscribe mode must keep draining control bytes.
    //
    // In forward mode this is not optional: draft-ietf-moq-transport-16
    // section 9.13 defines "start transmitting objects immediately, possibly
    // before PUBLISH_OK" as the FORWARD=1 behaviour of a PUBLISH. The PUBLISH
    // is what tells the relay which track alias the data streams carry;
    // pushing objects without it leaves the relay holding data for an alias it
    // cannot resolve until its own SUBSCRIBE round-trips, and the first group
    // of every track is at the relay's mercy. Draft-17/18 send the PUBLISH on
    // request streams instead, see the forward-mode loop below.
    if (!uses_request_streams(draft_version)) {
        std::uint64_t pub_req_id = 2;
        for (const auto& track : tracks) {
            const TrackMessage track_msg{
                .draft = draft_version,
                .track_name = track.track_name,
                .track_namespace = track_namespace_,
                .request_id = pub_req_id,
                .track_alias = alias_by_track.at(track.track_name),
                .largest_group_id = 0,
                .largest_object_id = 0,
                .content_exists = true,
                .authorization_token = action_token,
            };
            status = transport_.write_stream(control_stream_id_, encode_track_message(track_msg), false);
            if (!status.ok) {
                return status;
            }
            std::cerr << "[moqt-session] live: PUBLISH track=" << track.track_name
                      << " request_id=" << pub_req_id << '\n';
            publish_request_id_to_track.emplace(pub_req_id, track.track_name);
            pub_req_id += 2;
        }
    }

    std::cerr << "[moqt-session] live: awaiting subscriptions, mode="
              << (auto_forward_ ? "forward" : "await-subscribe") << '\n';
    const std::uint64_t live_control_read_stream_id =
        uses_request_streams(draft_version) ? peer_control_stream_id_ : control_stream_id_;

    // Phase 3: Catalog will be sent when a SUBSCRIBE arrives for it.
    // The relay forwards SUBSCRIBE to us; we respond with SUBSCRIBE_OK,
    // send data, then PUBLISH_DONE -- matching the legacy serve_subscriptions flow.
    //
    // Replaces the old one-shot `catalog_sent` latch: catalog_publisher_ owns
    // the group/object sequencing (MSF section 5) and, on the first call,
    // returns exactly the one independent-catalog object the old code sent by
    // hand; on any later call with an unchanged catalog it returns nothing,
    // which is what keeps repeat SUBSCRIBEs across the branches below a
    // no-op without needing their own latch.
    //
    // draft-ietf-moq-transport-19 section 10.11: "A sender MUST NOT send
    // PUBLISH_DONE until it has closed all streams it will ever open ... for
    // a subscription." end_broadcast() can open a further independent-catalog
    // stream on this same alias at any point before the session ends --
    // regardless of catalog_republish_interval_ -- so PUBLISH_DONE for the
    // catalog subscription can never be sent at SUBSCRIBE time: it is always
    // deferred to this loop's own exit below (search
    // catalog_publish_done_deferred), by which point catalog_publisher_ can
    // no longer open a new stream (either genuinely finished, or ended via a
    // concurrent end_broadcast()). catalog_stream_count accumulates the
    // total number of catalog streams opened, for that deferred
    // PUBLISH_DONE's stream_count field. catalog_publish_done_sender holds
    // how to actually send it (set alongside catalog_publish_done_deferred at
    // whichever of the three SUBSCRIBE-handling branches below defers it);
    // catalog_publish_done_guard is what actually fires it on every exit from
    // this function, not just the graceful cleanup further down -- see the
    // guard class's own comment for why a scope guard is used instead of
    // hand-editing every early return.
    bool catalog_publish_done_deferred = false;
    std::uint64_t catalog_stream_count = 0;
    DeliveryTimeouts catalog_delivery_timeouts;
    std::function<TransportStatus()> catalog_publish_done_sender;
    DeferredCatalogPublishDoneGuard catalog_publish_done_guard(catalog_publish_done_deferred,
                                                               catalog_publish_done_sender);

    auto send_catalog = [&](std::uint64_t track_alias) -> TransportStatus {
        std::vector<openmoq::publisher::CatalogObject> objects;
        try {
            objects = catalog_publisher_.publish(live_catalog.msf_catalog);
        } catch (const std::runtime_error&) {
            // Publisher::end_broadcast() is documented to call
            // MoqtSession::end_broadcast() without holding a lock across it,
            // specifically so it can run concurrently with an in-progress
            // publish_live() -- so catalog_publisher_ may have already ended
            // on another thread. Treat that as "nothing to send" rather than
            // let the exception escape this TransportStatus-returning path.
            return TransportStatus::success();
        }
        if (objects.empty()) {
            return TransportStatus::success();
        }
        std::size_t streams_opened = 0;
        const TransportStatus status = send_catalog_objects(
            draft_version,
            track_alias,
            objects,
            catalog_delivery_timeouts,
            &streams_opened);
        if (status.ok) {
            last_catalog_published_at_ = std::chrono::steady_clock::now();
            catalog_stream_count += streams_opened;
            std::cerr << "[moqt-session] live: catalog published (" << objects.back().payload.size() << " bytes)\n";
        }
        return status;
    };

    // MSF section 5.3 cache-expiry republication: when configured, re-emit
    // an independent catalog on its own interval regardless of whether the
    // content changed. A no-op until the first send_catalog() call sets
    // last_catalog_published_at_.
    auto maybe_republish_catalog = [&]() -> TransportStatus {
        if (catalog_republish_interval_.count() <= 0) {
            return TransportStatus::success();
        }
        if (last_catalog_published_at_.time_since_epoch().count() == 0) {
            return TransportStatus::success();
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_catalog_published_at_ < catalog_republish_interval_) {
            return TransportStatus::success();
        }
        std::vector<openmoq::publisher::CatalogObject> objects;
        try {
            objects = catalog_publisher_.force_independent();
        } catch (const std::runtime_error&) {
            // See send_catalog's identical guard: a concurrent
            // end_broadcast() may have already ended catalog_publisher_.
            return TransportStatus::success();
        }
        if (objects.empty()) {
            return TransportStatus::success();
        }
        std::size_t streams_opened = 0;
        const TransportStatus status = send_catalog_objects(
            draft_version,
            catalog_track_alias_,
            objects,
            catalog_delivery_timeouts,
            &streams_opened);
        if (status.ok) {
            last_catalog_published_at_ = now;
            catalog_stream_count += streams_opened;
        }
        return status;
    };

    // Phase 4: Stream media from stdin.
    // Use a reader thread so we can also handle control messages.
    struct LiveMediaQueue {
        std::mutex mutex;
        std::deque<openmoq::publisher::MediaFragment> fragments;
        bool eof = false;
    };
    auto queue = std::make_shared<LiveMediaQueue>();

    std::atomic<bool> stdin_stop{false};
    std::thread stdin_thread([&reader, &input, &tracks, queue, &stdin_stop]() {
        std::vector<std::uint8_t> pending_moof;
        std::size_t shared_group_id = 0;
        std::map<std::string, std::size_t> object_id_in_group;  // per track, resets on new group
        bool first_keyframe_seen = false;

        while (true) {
            if (stdin_stop.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(queue->mutex);
                queue->eof = true;
                break;
            }
            const std::size_t bytes_read = read_live_input(input, reader, &stdin_stop);

            while (auto box = reader.next_box()) {
                if (box->type == "moof") {
                    pending_moof = std::move(box->bytes);
                } else if (box->type == "mdat") {
                    if (pending_moof.empty()) {
                        std::cerr << "[moqt-session] live: mdat without preceding moof, skipping\n";
                        continue;
                    }
                    try {
                        // Build fragment (group_id=0 placeholder, we'll assign below)
                        auto fragment = openmoq::publisher::build_live_fragment(
                            pending_moof, box->bytes, tracks, 0);

                        // Keyframe-based grouping:
                        // When a video keyframe arrives, start a new group for ALL tracks.
                        if (fragment.is_video_keyframe) {
                            if (first_keyframe_seen) {
                                ++shared_group_id;
                            }
                            first_keyframe_seen = true;
                            // Reset object counters for all tracks on new group
                            object_id_in_group.clear();
                        }

                        if (!first_keyframe_seen) {
                            // Drop fragments before first keyframe (can't decode without IDR)
                            pending_moof.clear();
                            continue;
                        }

                        // Assign shared group_id and per-track object_id
                        fragment.group_id = shared_group_id;
                        fragment.object_id = object_id_in_group[fragment.track_name]++;

                        {
                            std::lock_guard<std::mutex> lock(queue->mutex);
                            queue->fragments.push_back(std::move(fragment));
                            // Trim queue: keep only fragments from the latest 2 groups.
                            // This prevents unbounded backlog when ffmpeg encodes
                            // faster than realtime, while keeping enough data for
                            // A/V sync (audio from the previous group).
                            if (!queue->fragments.empty()) {
                                const std::size_t latest = queue->fragments.back().group_id;
                                const std::size_t min_keep = latest > 1 ? latest - 1 : 0;
                                while (!queue->fragments.empty() &&
                                       queue->fragments.front().group_id < min_keep) {
                                    queue->fragments.pop_front();
                                }
                            }
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "[moqt-session] live: fragment parse error: " << e.what() << '\n';
                    }
                    pending_moof.clear();
                }
                // Skip other box types (styp, free, etc.)
            }

            if (bytes_read == 0) {
                std::lock_guard<std::mutex> lock(queue->mutex);
                queue->eof = true;
                break;
            }
        }
    });

    const auto join_stdin_thread = [&stdin_thread, &stdin_stop]() {
        stdin_stop.store(true, std::memory_order_release);
        if (stdin_thread.joinable()) {
            stdin_thread.join();
        }
    };

    // Main loop: drain queue and publish fragments
    std::map<std::string, SubgroupSenderState> sender_by_track;
    std::map<std::string, std::uint64_t> last_group_id_by_track;
    std::map<std::uint64_t, SubscribeMessage> active_subscriptions;
    std::map<std::uint64_t, std::uint64_t> active_subscription_stream_ids;
    std::map<std::uint64_t, std::vector<std::uint8_t>>
        pending_subscription_request_bytes;
    std::set<std::string> subscribed_tracks;
    std::map<std::string, std::uint64_t> subscribe_tracks_publish_request_ids;
    std::map<std::string, DeliveryTimeouts> published_track_delivery_timeouts;
    std::map<std::string, SubscribeMessage> published_track_settings;
    std::map<std::uint64_t, std::vector<std::uint8_t>>
        pending_publish_request_bytes;
    std::set<std::uint64_t> terminated_publish_request_ids;
    LargestObjectByTrack largest_object_by_track;
    std::uint64_t next_subscribe_tracks_publish_request_id = 2;

    auto drain_queue = [&]() -> TransportStatus {
        // Send all available fragments. The queue is bounded by stdin_thread's
        // trim logic (keeps at most the latest 2 groups), so we never burst
        // more than ~4 fragments (2 groups × 2 tracks) at a time.
        // No pacing needed: trimming prevents backlog accumulation and
        // ffmpeg's realtime encoding rate naturally limits throughput.
        while (true) {
            openmoq::publisher::MediaFragment fragment;
            {
                std::lock_guard<std::mutex> lock(queue->mutex);
                if (queue->fragments.empty()) break;
                fragment = std::move(queue->fragments.front());
                queue->fragments.pop_front();
            }

            note_largest_live_object(largest_object_by_track,
                                     fragment.track_name,
                                     fragment.group_id,
                                     fragment.object_id);

            // In auto-forward mode, publish media immediately even before a downstream
            // SUBSCRIBE arrives. Keep subscription gating for await-subscribe mode.
            if (!auto_forward_ && !subscribed_tracks.count(fragment.track_name)) {
                continue;
            }

            const auto alias_it = alias_by_track.find(fragment.track_name);
            if (alias_it == alias_by_track.end()) {
                continue;
            }

            const openmoq::publisher::CmsfObject object{
                .kind = openmoq::publisher::CmsfObjectKind::kMedia,
                .track_name = fragment.track_name,
                .group_id = fragment.group_id,
                .subgroup_id = 0,
                .object_id = fragment.object_id,
                .media_time_us = fragment.start_time_us,
                .media_duration_us = fragment.duration_us,
                .payload = {},
                .owned_payload = fragment.payload.owned_bytes,
            };
            const auto published_settings_it =
                published_track_settings.find(fragment.track_name);
            if (!live_object_matches_request_union(
                    object,
                    draft_version,
                    active_subscriptions,
                    published_settings_it != published_track_settings.end()
                        ? &published_settings_it->second
                        : nullptr)) {
                continue;
            }

            auto& sender = sender_by_track[fragment.track_name];
            const auto publisher_timeout_it =
                published_track_delivery_timeouts.find(fragment.track_name);
            const DeliveryTimeouts delivery_timeouts = delivery_timeouts_for_track(
                active_subscriptions,
                fragment.track_name,
                publisher_timeout_it != published_track_delivery_timeouts.end()
                    ? publisher_timeout_it->second
                    : DeliveryTimeouts{});
            const auto group_it = last_group_id_by_track.find(fragment.track_name);
            if (group_it != last_group_id_by_track.end() && group_it->second != static_cast<std::uint64_t>(fragment.group_id)) {
                TransportStatus finish_status = sender.finish_group(
                    transport_, draft_version, delivery_timeouts, now_function_);
                if (!finish_status.ok) {
                    return finish_status;
                }
            }

            const std::uint64_t send_seq = next_send_seq();
            const std::span<const std::uint8_t> payload(fragment.payload.owned_bytes);
            bool object_published = false;
            TransportStatus write_status = sender.serve(
                transport_, draft_version, alias_it->second, send_seq,
                object, true, stream_per_object, payload, delivery_timeouts,
                now_function_, &object_published,
                {.subscriber = subscriber_priority_for_track(
                     active_subscriptions,
                     fragment.track_name,
                     published_settings_it != published_track_settings.end()
                         ? published_settings_it->second.subscriber_priority
                         : 128),
                 .publisher = 128});
            if (!write_status.ok) {
                return write_status;
            }
            if (!object_published) {
                continue;
            }
            last_group_id_by_track[fragment.track_name] = static_cast<std::uint64_t>(fragment.group_id);
            record_published_object(fragment.track_name,
                                    static_cast<std::uint64_t>(fragment.group_id),
                                    fragment.payload.owned_bytes.size());
            std::cerr << "[moqt-session] live: sent track=" << fragment.track_name
                      << " group=" << fragment.group_id
                      << " obj=" << fragment.object_id
                      << " time_us=" << fragment.start_time_us
                      << " bytes=" << fragment.payload.owned_bytes.size()
                      << " sap=" << static_cast<unsigned int>(fragment.sap_type) << '\n';
        }
        return TransportStatus::success();
    };

    // PUBLISH each named track on its own request stream (draft-17/18), waiting
    // for the reply. Idempotent per track: a name already published is skipped,
    // so the forward-mode preannounce (media tracks only) and a later
    // SUBSCRIBE_TRACKS (catalog plus media) compose without duplicate PUBLISHes.
    auto publish_live_tracks = [&](const std::vector<std::string>& live_track_names) -> TransportStatus {
        if (!uses_request_streams(draft_version)) {
            return TransportStatus::success();
        }

        for (const auto& track_name : live_track_names) {
            if (subscribe_tracks_publish_request_ids.contains(track_name)) {
                continue;
            }
            const auto alias_it = alias_by_track.find(track_name);
            if (alias_it == alias_by_track.end()) {
                continue;
            }
            TrackMessage track_message{
                .draft = draft_version,
                .track_name = track_name,
                .track_namespace = track_namespace_,
                .request_id = next_subscribe_tracks_publish_request_id,
                .track_alias = alias_it->second,
                .largest_group_id = 0,
                .largest_object_id = 0,
                .content_exists = true,
                .authorization_token = action_token,
            };
            PublishOk publish_ok;
            std::uint64_t track_stream_id = 0;
            const std::uint64_t publish_request_id =
                next_subscribe_tracks_publish_request_id;
            TransportStatus publish_status =
                send_request_stream_and_wait(transport_,
                                             draft_version,
                                             encode_track_message(track_message),
                                             true,
                                             &publish_ok,
                                             &track_stream_id,
                                             &pending_publish_request_bytes[
                                                 publish_request_id]);
            if (!publish_status.ok) {
                return publish_status;
            }
            published_track_delivery_timeouts.insert_or_assign(
                track_name, publish_ok.delivery_timeouts);
            SubscribeMessage settings;
            settings.request_id = publish_request_id;
            settings.track_name = track_name;
            settings.subscriber_priority = publish_ok.subscriber_priority;
            settings.group_order = publish_ok.group_order;
            settings.forward = publish_ok.forward;
            settings.filter_type = publish_ok.filter_type;
            settings.delivery_timeouts = publish_ok.delivery_timeouts;
            published_track_settings.insert_or_assign(
                track_name, settings);
            if (track_name == "catalog") {
                catalog_delivery_timeouts = merge_delivery_timeouts(
                    catalog_delivery_timeouts, publish_ok.delivery_timeouts);
                remember_catalog_delivery_timeouts(catalog_delivery_timeouts);
            }
            subscribe_tracks_publish_request_ids.insert_or_assign(
                track_name, publish_request_id);
            publish_stream_id_by_request_id_.insert_or_assign(
                publish_request_id, track_stream_id);
            std::cerr << "[moqt-session] live: PUBLISH track=" << track_name
                      << " request_id=" << publish_request_id
                      << " (request stream)\n";
            next_subscribe_tracks_publish_request_id += 2;
        }
        return TransportStatus::success();
    };

    auto publish_live_tracks_for_subscribe_tracks = [&]() -> TransportStatus {
        std::vector<std::string> live_track_names = {"catalog"};
        for (const auto& track : tracks) {
            live_track_names.push_back(track.track_name);
        }
        return publish_live_tracks(live_track_names);
    };

    const auto apply_subscriber_request_update =
        [&](std::uint64_t existing_request_id,
            std::uint64_t response_stream_id,
            const RequestUpdateMessage& update) -> TransportStatus {
            const auto active_it =
                active_subscriptions.find(existing_request_id);
            if (active_it == active_subscriptions.end()) {
                return protocol_violation(
                    transport_,
                    "REQUEST_UPDATE specified an invalid existing subscription");
            }
            if (update.new_group_request.has_value()) {
                return protocol_violation(
                    transport_,
                    "REQUEST_UPDATE NEW_GROUP requires negotiated DYNAMIC_GROUPS");
            }
            const std::string track_name = active_it->second.track_name;
            const bool renew_peer_stopped_subgroups =
                request_update_renews_peer_stopped_subgroups(
                    draft_version, active_it->second, update);
            apply_request_update(active_it->second, update);
            if (renew_peer_stopped_subgroups) {
                sender_by_track[track_name].renew_peer_stopped_subgroups(
                    transport_);
            }
            note_delivery_timeouts(
                transport_, active_it->second.delivery_timeouts);

            const bool forward = std::any_of(
                active_subscriptions.begin(),
                active_subscriptions.end(),
                [&](const auto& entry) {
                    return entry.second.track_name == track_name &&
                           entry.second.forward != 0;
                });
            if (track_name == "catalog") {
                catalog_delivery_timeouts = delivery_timeouts_for_track(
                    active_subscriptions,
                    "catalog",
                    catalog_delivery_timeouts);
                remember_catalog_delivery_timeouts(
                    catalog_delivery_timeouts);
                if (forward) {
                    const auto alias_it = alias_by_track.find(track_name);
                    if (alias_it != alias_by_track.end()) {
                        const TransportStatus catalog_status =
                            send_catalog(alias_it->second);
                        if (!catalog_status.ok) {
                            return catalog_status;
                        }
                    }
                }
            } else if (forward) {
                subscribed_tracks.insert(track_name);
            } else {
                subscribed_tracks.erase(track_name);
            }
            const std::vector<std::uint8_t> response =
                encode_live_request_ok_message(draft_version,
                                               update.request_id,
                                               track_name,
                                               largest_object_by_track);
            return transport_.write_stream(
                response_stream_id, response, false);
        };

    const auto process_publish_request_updates = [&]() -> TransportStatus {
        return poll_retained_publish_request_updates(
            transport_,
            draft_version,
            subscribe_tracks_publish_request_ids,
            publish_stream_id_by_request_id_,
            pending_publish_request_bytes,
            terminated_publish_request_ids,
            peer_request_ids,
            [&](const std::string& track_name,
                std::uint64_t request_id,
                std::uint64_t response_stream_id,
                const RequestUpdateMessage& update) -> TransportStatus {
                static_cast<void>(request_id);
                if (update.new_group_request.has_value()) {
                    return protocol_violation(
                        transport_,
                        "REQUEST_UPDATE NEW_GROUP requires negotiated DYNAMIC_GROUPS");
                }
                const auto settings_it =
                    published_track_settings.find(track_name);
                if (settings_it == published_track_settings.end()) {
                    return TransportStatus::failure(
                        "missing settings for draft-18 PUBLISH request");
                }
                const bool renew_peer_stopped_subgroups =
                    request_update_renews_peer_stopped_subgroups(
                        draft_version, settings_it->second, update);
                apply_request_update(settings_it->second, update);
                if (renew_peer_stopped_subgroups) {
                    sender_by_track[track_name]
                        .renew_peer_stopped_subgroups(transport_);
                }
                published_track_delivery_timeouts.insert_or_assign(
                    track_name, settings_it->second.delivery_timeouts);
                note_delivery_timeouts(
                    transport_, settings_it->second.delivery_timeouts);
                if (settings_it->second.forward != 0) {
                    subscribed_tracks.insert(track_name);
                } else {
                    subscribed_tracks.erase(track_name);
                }
                const std::vector<std::uint8_t> response =
                    encode_live_request_ok_message(draft_version,
                                                   update.request_id,
                                                   track_name,
                                                   largest_object_by_track);
                return transport_.write_stream(
                    response_stream_id, response, false);
            },
            [&](const std::string& track_name, std::uint64_t request_id) {
                static_cast<void>(track_name);
                static_cast<void>(request_id);
            });
    };

    // Helper: process pending SUBSCRIBE/SUBSCRIBE_NAMESPACE messages
    // from the control stream. Returns the number of new subscriptions accepted.
    auto process_control_messages = [&]() -> std::pair<TransportStatus, std::size_t> {
        std::size_t new_subs = 0;
        TransportStatus update_status = process_publish_request_updates();
        if (!update_status.ok) {
            return {update_status, 0};
        }
        update_status =
            poll_retained_subscribe_request_updates(
                transport_,
                draft_version,
                active_subscriptions,
                active_subscription_stream_ids,
                pending_subscription_request_bytes,
                peer_request_ids,
                apply_subscriber_request_update);
        if (!update_status.ok) {
            return {update_status, 0};
        }
        if (uses_request_streams(draft_version)) {
            while (true) {
                std::uint64_t request_stream_id = 0;
                TransportStatus accept_status =
                    transport_.accept_stream(StreamDirection::kBidirectional,
                                             request_stream_id,
                                             std::chrono::milliseconds(0));
                if (!accept_status.ok) {
                    if (accept_status.message == "timed out waiting for stream data") {
                        break;
                    }
                    return {accept_status, 0};
                }
                const TransportStatus priority_status =
                    assign_request_stream_priority(transport_, draft_version, request_stream_id);
                if (!priority_status.ok) {
                    return {priority_status, 0};
                }

                std::vector<std::uint8_t> request_bytes;
                std::vector<std::uint8_t> trailing_request_bytes;
                TransportStatus read_status =
                    read_request_stream_message(
                        transport_, request_stream_id, draft_version,
                        subscriber_timeout_, request_bytes,
                        &trailing_request_bytes);
                if (!read_status.ok) {
                    return {read_status, 0};
                }
                trace_control_message(request_bytes, draft_version);

                std::size_t request_offset = 0;
                std::uint64_t request_type = 0;
                if (!decode_moqint(request_bytes, request_offset, draft_version, request_type)) {
                    return {protocol_violation(transport_, "failed to parse request stream type"), 0};
                }

                if (request_type == 0x03) {
                    SubscribeMessage subscribe;
                    if (!decode_subscribe_message(request_bytes, draft_version, subscribe)) {
                        TransportStatus write_status =
                            transport_.write_stream(request_stream_id,
                                                    encode_request_error_message(
                                                        draft_version, 0, 0x1, 0, "invalid SUBSCRIBE"),
                                                    true);
                        return {write_status.ok ? protocol_violation(transport_, "received invalid SUBSCRIBE")
                                                : write_status,
                                0};
                    }
                    const TransportStatus request_id_status =
                        peer_request_ids.validate(transport_, subscribe.request_id);
                    if (!request_id_status.ok) {
                        return {request_id_status, 0};
                    }
                    note_delivery_timeouts(transport_, subscribe.delivery_timeouts);

                    const auto track_it = alias_by_track.find(subscribe.track_name);
                    if (track_it == alias_by_track.end()) {
                        TransportStatus write_status =
                            transport_.write_stream(request_stream_id,
                                                    encode_request_error_message(
                                                        draft_version, subscribe.request_id, 0x2, 0, "track does not exist"),
                                                    true);
                        if (!write_status.ok) {
                            return {write_status, 0};
                        }
                        continue;
                    }

                    TransportStatus write_status =
                        transport_.write_stream(request_stream_id,
                                                encode_subscribe_ok_message(draft_version,
                                                                            subscribe.request_id,
                                                                            track_it->second,
                                                                            0,
                                                                            0,
                                                                            false),
                                                false);
                    if (!write_status.ok) {
                        return {write_status, 0};
                    }
                    active_subscriptions.emplace(subscribe.request_id, subscribe);
                    active_subscription_stream_ids.insert_or_assign(subscribe.request_id, request_stream_id);
                    pending_subscription_request_bytes.insert_or_assign(
                        subscribe.request_id,
                        std::move(trailing_request_bytes));
                    ++new_subs;
                    std::cerr << "[moqt-session] live: accepted subscribe track=" << subscribe.track_name
                              << " request_id=" << subscribe.request_id << '\n';

                    if (subscribe.track_name == "catalog") {
                        catalog_delivery_timeouts = delivery_timeouts_for_track(
                            active_subscriptions,
                            "catalog",
                            catalog_delivery_timeouts);
                        remember_catalog_delivery_timeouts(catalog_delivery_timeouts);
                        if (subscription_forwards_objects(
                                draft_version, subscribe.forward)) {
                            write_status = send_catalog(track_it->second);
                            if (!write_status.ok) {
                                return {write_status, 0};
                            }
                        }
                        // section 10.11 MUST NOT: end_broadcast() can open a
                        // further catalog stream on this alias at any point
                        // before the session ends, regardless of
                        // catalog_republish_interval_, so PUBLISH_DONE for the
                        // catalog subscription is always deferred to this
                        // loop's own exit (or, on an early error return, to
                        // catalog_publish_done_guard) -- never sent
                        // immediately at SUBSCRIBE time.
                        catalog_publish_done_deferred = true;
                        catalog_publish_done_sender = [&, request_id = subscribe.request_id,
                                                       response_stream_id = request_stream_id]() -> TransportStatus {
                            return transport_.write_stream(
                                response_stream_id,
                                encode_publish_done_message(draft_version, request_id, catalog_stream_count),
                                false);
                        };
                    } else if (subscription_forwards_objects(
                                   draft_version, subscribe.forward)) {
                        subscribed_tracks.insert(subscribe.track_name);
                    }
                    const TransportStatus retained_update_status =
                        poll_retained_subscribe_request_updates(
                            transport_,
                            draft_version,
                            active_subscriptions,
                            active_subscription_stream_ids,
                            pending_subscription_request_bytes,
                            peer_request_ids,
                            apply_subscriber_request_update);
                    if (!retained_update_status.ok) {
                        return {retained_update_status, 0};
                    }
                    continue;
                }

                if (request_type == 0x50) {
                    SubscribeNamespaceMessage subscribe_namespace;
                    if (!decode_subscribe_namespace_message(request_bytes, draft_version, subscribe_namespace)) {
                        TransportStatus write_status =
                            transport_.write_stream(request_stream_id,
                                                    encode_request_error_message(
                                                        draft_version, 0, 0x1, 0, "invalid SUBSCRIBE_NAMESPACE"),
                                                    true);
                        return {write_status.ok ? protocol_violation(transport_, "received invalid SUBSCRIBE_NAMESPACE")
                                                : write_status,
                                0};
                    }
                    const TransportStatus request_id_status =
                        peer_request_ids.validate(
                            transport_, subscribe_namespace.request_id);
                    if (!request_id_status.ok) {
                        return {request_id_status, 0};
                    }
                    TransportStatus write_status =
                        transport_.write_stream(request_stream_id,
                                                encode_request_ok_message(draft_version, subscribe_namespace.request_id),
                                                false);
                    if (!write_status.ok) {
                        return {write_status, 0};
                    }
                    continue;
                }

                SubscribeTracksMessage subscribe_tracks;
                if (request_type != 0x51 ||
                    !decode_subscribe_tracks_message(request_bytes, draft_version, subscribe_tracks)) {
                    TransportStatus write_status =
                        transport_.write_stream(request_stream_id,
                                                encode_request_error_message(
                                                    draft_version, 0, 0x1, 0, "unsupported request stream"),
                                                true);
                    return {write_status.ok ? protocol_violation(transport_, "received unsupported request stream")
                                            : write_status,
                            0};
                }
                const TransportStatus request_id_status =
                    peer_request_ids.validate(transport_,
                                              subscribe_tracks.request_id);
                if (!request_id_status.ok) {
                    return {request_id_status, 0};
                }
                if (!namespace_prefix_matches(subscribe_tracks.track_namespace_prefix, track_namespace_)) {
                    TransportStatus write_status =
                        transport_.write_stream(request_stream_id,
                                                encode_request_error_message(
                                                    draft_version, 0, 0x2, 0, "unsupported namespace prefix"),
                                                true);
                    return {write_status.ok ? TransportStatus::failure("peer requested unsupported namespace prefix")
                                            : write_status,
                            0};
                }

                TransportStatus write_status =
                    transport_.write_stream(request_stream_id,
                                            encode_request_ok_message(draft_version, subscribe_tracks.request_id),
                                            false);
                if (!write_status.ok) {
                    return {write_status, 0};
                }
                if (subscribe_tracks.forward == 0) {
                    continue;
                }

                TransportStatus publish_status = publish_live_tracks_for_subscribe_tracks();
                if (!publish_status.ok) {
                    return {publish_status, 0};
                }
                publish_status = process_publish_request_updates();
                if (!publish_status.ok) {
                    return {publish_status, 0};
                }
                const auto catalog_request_id = subscribe_tracks_publish_request_ids.find("catalog");
                const auto catalog_alias = alias_by_track.find("catalog");
                if (catalog_request_id != subscribe_tracks_publish_request_ids.end() &&
                    catalog_alias != alias_by_track.end()) {
                    TransportStatus catalog_status = send_catalog(catalog_alias->second);
                    if (!catalog_status.ok) {
                        return {catalog_status, 0};
                    }
                    // section 10.11 MUST NOT: end_broadcast() can open a
                    // further catalog stream on this alias at any point
                    // before the session ends, regardless of
                    // catalog_republish_interval_, so PUBLISH_DONE is always
                    // deferred to this loop's own exit, via the
                    // subscribe_tracks_publish_request_ids cleanup below (or,
                    // on an early error return, to
                    // catalog_publish_done_guard) -- never sent immediately.
                    catalog_publish_done_deferred = true;
                    catalog_publish_done_sender = [&, request_id = catalog_request_id->second]() -> TransportStatus {
                        return write_publish_done_for_request(transport_, draft_version, control_stream_id_,
                                                              publish_stream_id_by_request_id_, request_id,
                                                              catalog_stream_count);
                    };
                }
                for (const auto& track : tracks) {
                    subscribed_tracks.insert(track.track_name);
                    ++new_subs;
                }
            }
        }

        std::size_t message_size = 0;
        while (next_control_message(pending_control_bytes_, draft_version, message_size)) {
            const std::vector<std::uint8_t> message_bytes(
                pending_control_bytes_.begin(),
                pending_control_bytes_.begin() + static_cast<std::ptrdiff_t>(message_size));
            std::size_t offset = 0;
            std::uint64_t message_type = 0;
            if (!decode_moqint(message_bytes, offset, draft_version, message_type)) {
                return {protocol_violation(transport_, "failed to parse control request type"), 0};
            }
            trace_control_message(message_bytes, draft_version);

            if ((draft_version == DraftVersion::kDraft18 &&
                 message_type == 0x02) ||
                (uses_request_streams(draft_version) &&
                 (message_type == 0x03 || message_type == 0x06 ||
                  message_type == 0x50 || message_type == 0x16 ||
                  message_type == 0x1d || message_type == 0x51))) {
                return {protocol_violation(transport_, "draft-18 request message received on control stream"), 0};
            }

            if (message_type == 0x02 &&
                draft_version == DraftVersion::kDraft16) {
                const TransportStatus request_update_status =
                    process_draft16_control_request_update(
                        transport_,
                        message_bytes,
                        control_stream_id_,
                        peer_request_ids,
                        apply_subscriber_request_update);
                if (!request_update_status.ok) {
                    return {request_update_status, 0};
                }
            } else if (message_type == 0x03) {  // SUBSCRIBE
                SubscribeMessage subscribe;
                if (!decode_subscribe_message(message_bytes, draft_version, subscribe)) {
                    return {protocol_violation(transport_, "received invalid SUBSCRIBE"), 0};
                }
                const TransportStatus request_id_status =
                    peer_request_ids.validate(transport_, subscribe.request_id);
                if (!request_id_status.ok) {
                    return {request_id_status, 0};
                }
                note_delivery_timeouts(transport_, subscribe.delivery_timeouts);

                const auto track_it = alias_by_track.find(subscribe.track_name);
                if (track_it == alias_by_track.end()) {
                    auto ws = transport_.write_stream(control_stream_id_,
                        encode_subscribe_error_message(subscribe.request_id, 0x2, "track does not exist"), false);
                    if (!ws.ok) {
                        return {ws, 0};
                    }
                } else {
                    auto ws = transport_.write_stream(control_stream_id_,
                        encode_subscribe_ok_message(draft_version, subscribe.request_id,
                                                    track_it->second, 0, 0, false), false);
                    if (!ws.ok) {
                        return {ws, 0};
                    }
                    active_subscriptions.emplace(subscribe.request_id, subscribe);
                    active_subscription_stream_ids.insert_or_assign(subscribe.request_id, control_stream_id_);
                    ++new_subs;
                    std::cerr << "[moqt-session] live: accepted subscribe track=" << subscribe.track_name
                              << " request_id=" << subscribe.request_id << '\n';

                    // draft-ietf-moq-transport-19 section 10.11 forbids
                    // sending PUBLISH_DONE for a subscription until every
                    // stream it will ever open has closed. end_broadcast()
                    // can open a further catalog stream on this alias at any
                    // point before the session ends, regardless of
                    // catalog_republish_interval_, so PUBLISH_DONE for the
                    // catalog subscription is always deferred to this loop's
                    // own exit instead -- see catalog_publish_done_deferred.
                    if (subscribe.track_name == "catalog") {
                        catalog_delivery_timeouts = delivery_timeouts_for_track(
                            active_subscriptions,
                            "catalog",
                            catalog_delivery_timeouts);
                        remember_catalog_delivery_timeouts(catalog_delivery_timeouts);
                        if (subscription_forwards_objects(
                                draft_version, subscribe.forward)) {
                            ws = send_catalog(track_it->second);
                            if (!ws.ok) {
                                return {ws, 0};
                            }
                        }
                        const auto stream_it = active_subscription_stream_ids.find(subscribe.request_id);
                        const std::uint64_t response_stream_id =
                            uses_request_streams(draft_version) &&
                                    stream_it != active_subscription_stream_ids.end()
                                ? stream_it->second
                                : control_stream_id_;
                        catalog_publish_done_deferred = true;
                        catalog_publish_done_sender = [&, request_id = subscribe.request_id,
                                                       response_stream_id]() -> TransportStatus {
                            return transport_.write_stream(
                                response_stream_id,
                                encode_publish_done_message(draft_version, request_id, catalog_stream_count),
                                false);
                        };
                    } else if (subscription_forwards_objects(
                                   draft_version, subscribe.forward)) {
                        // Only add media tracks to subscribed_tracks (not catalog).
                        // This gates drain_queue until media is actually subscribed.
                        subscribed_tracks.insert(subscribe.track_name);
                    }
                }
            } else if (message_type == 0x11) {  // SUBSCRIBE_NAMESPACE
                SubscribeNamespaceMessage subscribe_namespace;
                if (decode_subscribe_namespace_message(message_bytes, draft_version, subscribe_namespace)) {
                    const TransportStatus request_id_status =
                        peer_request_ids.validate(
                            transport_, subscribe_namespace.request_id);
                    if (!request_id_status.ok) {
                        return {request_id_status, 0};
                    }
                    auto ws = transport_.write_stream(control_stream_id_,
                        encode_subscribe_namespace_ok_message(draft_version, subscribe_namespace.request_id), false);
                    if (!ws.ok) {
                        return {ws, 0};
                    }
                }
            } else if (message_type == 0x1e) {  // PUBLISH_OK
                // Relay accepted a PUBLISHed track. Some relays (e.g. moqx) do not
                // forward SUBSCRIBE after PUBLISH_OK; they expect the publisher to
                // start sending data once the track is accepted. Mark the track as
                // subscribed so drain_queue will forward fragments.
                PublishOk publish_ok_msg;
                if (decode_publish_ok(message_bytes, draft_version, publish_ok_msg)) {
                    note_delivery_timeouts(transport_, publish_ok_msg.delivery_timeouts);
                    const auto it = publish_request_id_to_track.find(publish_ok_msg.request_id);
                    if (it != publish_request_id_to_track.end()) {
                        published_track_delivery_timeouts.insert_or_assign(
                            it->second, publish_ok_msg.delivery_timeouts);
                        subscribed_tracks.insert(it->second);
                        ++new_subs;
                        std::cerr << "[moqt-session] live: PUBLISH_OK track=" << it->second
                                  << " request_id=" << publish_ok_msg.request_id << '\n';
                    }
                }
            }
            // Skip other message types

            pending_control_bytes_.erase(
                pending_control_bytes_.begin(),
                pending_control_bytes_.begin() + static_cast<std::ptrdiff_t>(message_size));
        }
        return {TransportStatus::success(), new_subs};
    };

    if (auto_forward_) {
        // Forward mode: publish media as fragments arrive, independent of downstream subscriptions.
        //
        // Draft-17/18: PUBLISH each media track on its own request stream before
        // the first object goes out -- same reasoning as the control-stream
        // preannounce above, through the shared request-stream PUBLISH path so
        // request-id / stream bookkeeping (and PUBLISH_DONE at exit) is common
        // with SUBSCRIBE_TRACKS. The catalog stays SUBSCRIBE-driven, as on the
        // control-stream drafts: it is a one-shot object, and a subscriber that
        // arrives after a proactive push would find nothing new to receive. A
        // relay that does not answer within the request-stream wait is logged
        // and tolerated: forward mode has always pushed regardless, and the
        // relay-side recovery (placeholder track / parked stream) still applies.
        if (uses_request_streams(draft_version)) {
            std::vector<std::string> media_track_names;
            for (const auto& track : tracks) {
                media_track_names.push_back(track.track_name);
            }
            const TransportStatus preannounce_status = publish_live_tracks(media_track_names);
            if (!preannounce_status.ok) {
                std::cerr << "[moqt-session] live: forward-mode PUBLISH preannounce failed ("
                          << preannounce_status.message << "); continuing without it\n";
            } else {
                const TransportStatus update_status =
                    process_publish_request_updates();
                if (!update_status.ok) {
                    join_stdin_thread();
                    return update_status;
                }
            }
        }
        std::optional<std::chrono::steady_clock::time_point> eof_deadline;
        while (true) {
            // Drain media continuously in auto-forward mode.
            {
                status = drain_queue();
                if (!status.ok) {
                    join_stdin_thread();
                    return status;
                }
            }

            bool is_eof;
            {
                std::lock_guard<std::mutex> lock(queue->mutex);
                is_eof = queue->eof && queue->fragments.empty();
            }

            auto [pre_status, pre_subs] = process_control_messages();
            if (!pre_status.ok) {
                join_stdin_thread();
                return pre_status;
            }

            // Read and process control messages (SUBSCRIBE, SUBSCRIBE_NAMESPACE)
            std::vector<std::uint8_t> chunk;
            bool fin = false;
            const TransportStatus read_status =
                transport_.read_stream(live_control_read_stream_id, chunk, fin, std::chrono::milliseconds(0));
            if (read_status.ok && !chunk.empty()) {
                pending_control_bytes_.insert(pending_control_bytes_.end(), chunk.begin(), chunk.end());
            }
            auto [ctrl_status, new_subs] = process_control_messages();
            if (!ctrl_status.ok) {
                join_stdin_thread();
                return ctrl_status;
            }

            status = maybe_republish_catalog();
            if (!status.ok) {
                join_stdin_thread();
                return status;
            }

            // Keep polling control briefly after EOF so downstream can still request
            // one-shot tracks (catalog) before we tear down the session.
            if (is_eof) {
                if (!eof_deadline.has_value()) {
                    eof_deadline = std::chrono::steady_clock::now() + subscriber_timeout_;
                } else if (std::chrono::steady_clock::now() >= *eof_deadline) {
                    break;
                }
            } else {
                eof_deadline.reset();
            }

            // Brief sleep to avoid busy-waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } else {
        // Await-subscribe mode: wait for subscriptions, then stream
        bool fin = false;
        const auto await_subscribe_deadline = std::chrono::steady_clock::now() + subscriber_timeout_;

        while (true) {
            bool is_eof;
            {
                std::lock_guard<std::mutex> lock(queue->mutex);
                is_eof = queue->eof && queue->fragments.empty();
            }
            if (is_eof && (!active_subscriptions.empty() || !subscribed_tracks.empty())) {
                // Drain remaining
                status = drain_queue();
                if (!status.ok) {
                    join_stdin_thread();
                    return status;
                }
                break;
            }

            // Flush any control bytes already buffered by collect_control_acknowledgements
            // (e.g. a relay SUBSCRIBE that arrived in the same chunk as ANNOUNCE_OK) before
            // we block on read_stream — otherwise they sit forever while the read hangs.
            if (!pending_control_bytes_.empty()) {
                auto [pre_status, pre_subs] = process_control_messages();
                if (!pre_status.ok) {
                    join_stdin_thread();
                    return pre_status;
                }
            }

            auto [request_status, request_subs] = process_control_messages();
            if (!request_status.ok) {
                join_stdin_thread();
                return request_status;
            }

            // Read control messages
            std::vector<std::uint8_t> chunk;
            bool immediate_fin = false;
            const bool waiting_for_first_subscription =
                active_subscriptions.empty() && subscribed_tracks.empty();
            const auto read_timeout =
                waiting_for_first_subscription && !uses_request_streams(draft_version)
                    ? subscriber_timeout_
                    : (waiting_for_first_subscription ? std::chrono::milliseconds(25)
                                                      : std::chrono::milliseconds(0));
            const TransportStatus read_status =
                transport_.read_stream(live_control_read_stream_id, chunk, immediate_fin, read_timeout);

            if (read_status.ok) {
                pending_control_bytes_.insert(pending_control_bytes_.end(), chunk.begin(), chunk.end());
                fin = immediate_fin;
            } else if (read_status.message == "timed out waiting for stream data" ||
                       read_status.message == "no queued read for stream") {
                if (waiting_for_first_subscription &&
                    (!uses_request_streams(draft_version) ||
                     std::chrono::steady_clock::now() >= await_subscribe_deadline)) {
                    std::cerr << "[moqt-session] live: no downstream SUBSCRIBE before timeout; "
                                 "idle await-subscribe publish exiting"
                              << " namespace=" << track_namespace_
                              << " draft=" << openmoq::publisher::to_string(draft_version)
                              << " timeout_s=" << subscriber_timeout_.count()
                              << " note=expecting relay/internal subscriber subscription"
                              << '\n';
                    break;
                }
            } else {
                join_stdin_thread();
                return read_status;
            }

            // Process control messages
            auto [ctrl_status, new_subs] = process_control_messages();
            if (!ctrl_status.ok) {
                join_stdin_thread();
                return ctrl_status;
            }

            status = maybe_republish_catalog();
            if (!status.ok) {
                join_stdin_thread();
                return status;
            }

            if (fin) {
                break;
            }

            // Drain any available media from the queue
            if (!subscribed_tracks.empty()) {
                status = drain_queue();
                if (!status.ok) {
                    join_stdin_thread();
                    return status;
                }
            }

            if (active_subscriptions.empty() && subscribed_tracks.empty()) {
                continue;
            }

            // Small sleep to prevent busy-waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    join_stdin_thread();

    for (auto& [track_name, sender] : sender_by_track) {
        const auto publisher_timeout_it = published_track_delivery_timeouts.find(track_name);
        const DeliveryTimeouts delivery_timeouts = delivery_timeouts_for_track(
            active_subscriptions,
            track_name,
            publisher_timeout_it != published_track_delivery_timeouts.end()
                ? publisher_timeout_it->second
                : DeliveryTimeouts{});
        status = sender.finish_group(
            transport_, draft_version, delivery_timeouts, now_function_);
        if (!status.ok) {
            return status;
        }
    }

    // Send PUBLISH_DONE for each subscribed media track. The catalog
    // subscription's PUBLISH_DONE is always deferred here (see
    // catalog_publish_done_deferred's own comment above for why: section
    // 10.11's MUST NOT -- end_broadcast() can open a further catalog stream
    // at any time before this loop exits). By now catalog_publisher_ can no
    // longer open a new stream for it, so it is safe to close.
    for (const auto& [request_id, subscribe] : active_subscriptions) {
        if (subscribe.track_name == "catalog") {
            if (catalog_publish_done_deferred) {
                const auto stream_it = active_subscription_stream_ids.find(request_id);
                const std::uint64_t response_stream_id =
                    uses_request_streams(draft_version) && stream_it != active_subscription_stream_ids.end()
                        ? stream_it->second
                        : control_stream_id_;
                status = transport_.write_stream(response_stream_id,
                                                 encode_publish_done_message(
                                                     draft_version, request_id, catalog_stream_count),
                                                 false);
                if (!status.ok) {
                    return status;
                }
                catalog_publish_done_deferred = false;
            }
            continue;
        }
        const auto stream_it = active_subscription_stream_ids.find(request_id);
        const std::uint64_t response_stream_id =
            uses_request_streams(draft_version) && stream_it != active_subscription_stream_ids.end()
                ? stream_it->second
                : control_stream_id_;
        status = transport_.write_stream(response_stream_id,
                                         encode_publish_done_message(
                                             draft_version, request_id, sender_by_track[subscribe.track_name].stream_count()),
                                         false);
        if (!status.ok) {
            return status;
        }
    }
    for (const auto& [track_name, request_id] : subscribe_tracks_publish_request_ids) {
        if (track_name == "catalog") {
            if (catalog_publish_done_deferred) {
                status = write_publish_done_for_request(transport_,
                                                        draft_version,
                                                        control_stream_id_,
                                                        publish_stream_id_by_request_id_,
                                                        request_id,
                                                        catalog_stream_count);
                if (!status.ok) {
                    return status;
                }
                catalog_publish_done_deferred = false;
            }
            continue;
        }
        if (!subscribed_tracks.contains(track_name)) {
            continue;
        }
        status = write_publish_done_for_request(transport_,
                                                draft_version,
                                                control_stream_id_,
                                                publish_stream_id_by_request_id_,
                                                request_id,
                                                sender_by_track[track_name].stream_count());
        if (!status.ok) {
            return status;
        }
    }

    std::cerr << "[moqt-session] live: stdin EOF, publishing complete\n";

    return write_namespace_done_for_request(transport_,
                                            draft_version,
                                            control_stream_id_,
                                            namespace_stream_id_,
                                            namespace_message);
}

TransportStatus MoqtSession::publish_live_objects(const openmoq::publisher::LiveObjectSource& source,
                                                  openmoq::publisher::DraftVersion draft_version) {
    if (transport_.state() != ConnectionState::kConnected) {
        return TransportStatus::failure("transport is not connected");
    }
    if (source.tracks.empty()) {
        return TransportStatus::failure("live object source has no tracks");
    }
    if (!source.next_object) {
        return TransportStatus::failure("live object source has no object reader");
    }

    TransportStatus status = ensure_setup(draft_version);
    if (!status.ok) {
        return status;
    }
    std::cout << "connection_id=" << transport_.connection_id() << '\n' << std::flush;
    const auto action_token = action_authorization_token();
    PeerRequestIdValidator peer_request_ids(
        draft_version, advertised_max_request_id(endpoint_->transport));

    NamespaceMessage namespace_message{
        .draft = draft_version,
        .track_namespace = track_namespace_,
        .request_id = 0,
        .authorization_token = action_token,
    };
    if (uses_request_streams(draft_version)) {
        status = send_request_stream_and_wait(
            transport_, draft_version, encode_namespace_message(namespace_message), false, nullptr,
            &namespace_stream_id_);
        if (status.ok) {
            namespace_stream_open_ = true;
        }
    } else {
        status = write_frame(control_stream_id_, encode_namespace_message(namespace_message), false);
        if (status.ok) {
            status = collect_control_acknowledgements(
                transport_, control_stream_id_, draft_version, 1, 0, pending_control_bytes_);
        }
    }
    if (!status.ok) {
        return status;
    }

    std::map<std::string, std::uint64_t> alias_by_track;
    std::uint64_t next_alias = 0;
    for (const auto& track : source.tracks) {
        if (track.track_name.empty()) {
            return TransportStatus::failure("live object source includes an empty track name");
        }
        if (!alias_by_track.emplace(track.track_name, next_alias).second) {
            return TransportStatus::failure("live object source includes duplicate track name: " + track.track_name);
        }
        ++next_alias;
    }

    // Opt-in preannounce: send a PUBLISH per track before any subscriber exists.
    //
    // Relays split into two camps. Some accept tracks this way, answer PUBLISH_OK
    // and never forward a SUBSCRIBE upstream, so a publisher that waits for one
    // would stall without this. Others resolve the track namespace only once a
    // subscriber appears; an early PUBLISH there is at best ignored and at worst
    // disturbs the namespace registration, after which SUBSCRIBEs are rejected.
    //
    // Off by default, because this path never dispatches PUBLISH_OK: the Forward
    // State in the reply cannot be read, so the request would be made and its
    // answer discarded. Callers whose relay needs it set
    // PublisherConfig::preannounce_tracks (CLI: --preannounce-tracks).
    if (!uses_request_streams(draft_version) && preannounce_tracks_) {
        std::uint64_t request_id = 2;
        for (const auto& [track_name, alias] : alias_by_track) {
            TrackMessage track_message{
                .draft = draft_version,
                .track_name = track_name,
                .track_namespace = track_namespace_,
                .request_id = request_id,
                .track_alias = alias,
                .largest_group_id = 0,
                .largest_object_id = 0,
                .content_exists = true,
                .authorization_token = action_token,
            };
            status = transport_.write_stream(control_stream_id_, encode_track_message(track_message), false);
            if (!status.ok) {
                return status;
            }
            request_id += 2;
        }
    }

    const std::uint64_t control_read_stream_id =
        uses_request_streams(draft_version) ? peer_control_stream_id_ : control_stream_id_;
    std::map<std::string, SubgroupSenderState> sender_by_track;
    std::map<std::uint64_t, SubscribeMessage> active_subscriptions;
    std::map<std::uint64_t, std::uint64_t> active_subscription_stream_ids;
    std::map<std::uint64_t, std::vector<std::uint8_t>>
        pending_subscription_request_bytes;
    std::set<std::string> subscribed_tracks;
    std::map<std::string, std::uint64_t> published_track_request_ids;
    std::map<std::string, DeliveryTimeouts> published_track_delivery_timeouts;
    std::map<std::string, SubscribeMessage> published_track_settings;
    std::map<std::uint64_t, std::vector<std::uint8_t>> pending_publish_request_bytes;
    std::set<std::uint64_t> terminated_publish_request_ids;
    LargestObjectByTrack largest_object_by_track;
    std::uint64_t next_publish_request_id = 2;

    auto publish_tracks = [&]() -> TransportStatus {
        if (!uses_request_streams(draft_version) || !published_track_request_ids.empty()) {
            return TransportStatus::success();
        }
        for (const auto& [track_name, alias] : alias_by_track) {
            TrackMessage track_message{
                .draft = draft_version,
                .track_name = track_name,
                .track_namespace = track_namespace_,
                .request_id = next_publish_request_id,
                .track_alias = alias,
                .largest_group_id = 0,
                .largest_object_id = 0,
                .content_exists = true,
                .authorization_token = action_token,
            };
            PublishOk publish_ok;
            std::uint64_t stream_id = 0;
            TransportStatus publish_status =
                send_request_stream_and_wait(transport_,
                                             draft_version,
                                             encode_track_message(track_message),
                                             true,
                                             &publish_ok,
                                             &stream_id,
                                             &pending_publish_request_bytes[next_publish_request_id]);
            if (!publish_status.ok) {
                return publish_status;
            }
            published_track_delivery_timeouts.insert_or_assign(
                track_name, publish_ok.delivery_timeouts);
            SubscribeMessage settings;
            settings.request_id = next_publish_request_id;
            settings.track_name = track_name;
            settings.subscriber_priority = publish_ok.subscriber_priority;
            settings.group_order = publish_ok.group_order;
            settings.forward = publish_ok.forward;
            settings.filter_type = publish_ok.filter_type;
            settings.delivery_timeouts = publish_ok.delivery_timeouts;
            published_track_settings.insert_or_assign(track_name, settings);
            published_track_request_ids.insert_or_assign(track_name, next_publish_request_id);
            publish_stream_id_by_request_id_.insert_or_assign(next_publish_request_id, stream_id);
            if (publish_ok.forward != 0) {
                subscribed_tracks.insert(track_name);
            }
            next_publish_request_id += 2;
        }
        return TransportStatus::success();
    };

    const auto effective_delivery_timeouts = [&](std::string_view track_name) {
        const auto publisher_timeout_it =
            published_track_delivery_timeouts.find(std::string(track_name));
        return delivery_timeouts_for_track(
            active_subscriptions,
            track_name,
            publisher_timeout_it != published_track_delivery_timeouts.end()
                ? publisher_timeout_it->second
                : DeliveryTimeouts{});
    };

    if (auto_forward_ && uses_request_streams(draft_version)) {
        status = publish_tracks();
        if (!status.ok) {
            return status;
        }
    }

    auto accept_subscribe = [&](const SubscribeMessage& subscribe,
                                std::uint64_t response_stream_id) -> TransportStatus {
        const TransportStatus request_id_status =
            peer_request_ids.validate(transport_, subscribe.request_id);
        if (!request_id_status.ok) {
            return request_id_status;
        }
        note_delivery_timeouts(transport_, subscribe.delivery_timeouts);
        const auto track_it = alias_by_track.find(subscribe.track_name);
        if (track_it == alias_by_track.end()) {
            if (uses_request_streams(draft_version)) {
                return transport_.write_stream(response_stream_id,
                                               encode_request_error_message(
                                                   draft_version, subscribe.request_id, 0x2, 0, "track does not exist"),
                                               true);
            }
            return transport_.write_stream(response_stream_id,
                                           encode_subscribe_error_message(
                                               subscribe.request_id, 0x2, "track does not exist"),
                                           false);
        }

        TransportStatus write_status =
            transport_.write_stream(response_stream_id,
                                    encode_subscribe_ok_message(draft_version,
                                                                subscribe.request_id,
                                                                track_it->second,
                                                                0,
                                                                0,
                                                                false),
                                    false);
        if (!write_status.ok) {
            return write_status;
        }
        active_subscriptions.emplace(subscribe.request_id, subscribe);
        active_subscription_stream_ids.insert_or_assign(subscribe.request_id, response_stream_id);
        if (subscription_forwards_objects(
                draft_version, subscribe.forward)) {
            subscribed_tracks.insert(subscribe.track_name);
        }
        return TransportStatus::success();
    };

    const auto refresh_subscriber_forward_permission =
        [&](std::string_view track_name) {
            const bool subscriber_forward = std::any_of(
                active_subscriptions.begin(),
                active_subscriptions.end(),
                [&](const auto& entry) {
                    return entry.second.track_name == track_name &&
                           entry.second.forward != 0;
                });
            const auto published_it =
                published_track_settings.find(std::string(track_name));
            const bool publisher_forward =
                published_it != published_track_settings.end() &&
                published_it->second.forward != 0;
            if (subscriber_forward || publisher_forward) {
                subscribed_tracks.insert(std::string(track_name));
            } else {
                subscribed_tracks.erase(std::string(track_name));
            }
        };

    const auto apply_subscriber_request_update =
        [&](std::uint64_t existing_request_id,
            std::uint64_t response_stream_id,
            const RequestUpdateMessage& update) -> TransportStatus {
            const auto active_it =
                active_subscriptions.find(existing_request_id);
            if (active_it == active_subscriptions.end()) {
                return protocol_violation(
                    transport_,
                    "REQUEST_UPDATE specified an invalid existing subscription");
            }
            if (update.new_group_request.has_value()) {
                return protocol_violation(
                    transport_,
                    "REQUEST_UPDATE NEW_GROUP requires negotiated DYNAMIC_GROUPS");
            }

            const std::string track_name = active_it->second.track_name;
            const bool renew_peer_stopped_subgroups =
                request_update_renews_peer_stopped_subgroups(
                    draft_version, active_it->second, update);
            apply_request_update(active_it->second, update);
            if (renew_peer_stopped_subgroups) {
                sender_by_track[track_name].renew_peer_stopped_subgroups(
                    transport_);
            }
            note_delivery_timeouts(
                transport_, active_it->second.delivery_timeouts);
            refresh_subscriber_forward_permission(track_name);
            const std::vector<std::uint8_t> response =
                encode_live_request_ok_message(draft_version,
                                               update.request_id,
                                               track_name,
                                               largest_object_by_track);
            return transport_.write_stream(
                response_stream_id, response, false);
        };

    const auto process_subscriber_request_updates = [&]() -> TransportStatus {
        return poll_retained_subscribe_request_updates(
            transport_,
            draft_version,
            active_subscriptions,
            active_subscription_stream_ids,
            pending_subscription_request_bytes,
            peer_request_ids,
            apply_subscriber_request_update);
    };

    auto process_publish_request_updates = [&]() -> TransportStatus {
        return poll_retained_publish_request_updates(
            transport_,
            draft_version,
            published_track_request_ids,
            publish_stream_id_by_request_id_,
            pending_publish_request_bytes,
            terminated_publish_request_ids,
            peer_request_ids,
            [&](const std::string& track_name,
                std::uint64_t request_id,
                std::uint64_t response_stream_id,
                const RequestUpdateMessage& update) -> TransportStatus {
                static_cast<void>(request_id);
                if (update.new_group_request.has_value()) {
                    return protocol_violation(
                        transport_,
                        "REQUEST_UPDATE NEW_GROUP requires negotiated DYNAMIC_GROUPS");
                }
                const auto settings_it =
                    published_track_settings.find(track_name);
                if (settings_it == published_track_settings.end()) {
                    return TransportStatus::failure(
                        "missing settings for draft-18 PUBLISH request");
                }
                const bool renew_peer_stopped_subgroups =
                    request_update_renews_peer_stopped_subgroups(
                        draft_version, settings_it->second, update);
                apply_request_update(settings_it->second, update);
                if (renew_peer_stopped_subgroups) {
                    sender_by_track[track_name]
                        .renew_peer_stopped_subgroups(transport_);
                }
                published_track_delivery_timeouts.insert_or_assign(
                    track_name, settings_it->second.delivery_timeouts);
                note_delivery_timeouts(
                    transport_, settings_it->second.delivery_timeouts);
                refresh_subscriber_forward_permission(track_name);
                const std::vector<std::uint8_t> response =
                    encode_live_request_ok_message(draft_version,
                                                   update.request_id,
                                                   track_name,
                                                   largest_object_by_track);
                return transport_.write_stream(
                    response_stream_id, response, false);
            },
            [&](const std::string& track_name, std::uint64_t request_id) {
                static_cast<void>(track_name);
                static_cast<void>(request_id);
            });
    };

    auto process_control_messages = [&]() -> TransportStatus {
        TransportStatus update_status = process_publish_request_updates();
        if (!update_status.ok) {
            return update_status;
        }
        update_status = process_subscriber_request_updates();
        if (!update_status.ok) {
            return update_status;
        }
        if (uses_request_streams(draft_version)) {
            while (true) {
                std::uint64_t request_stream_id = 0;
                TransportStatus accept_status =
                    transport_.accept_stream(StreamDirection::kBidirectional,
                                             request_stream_id,
                                             std::chrono::milliseconds(0));
                if (!accept_status.ok) {
                    if (accept_status.message == "timed out waiting for stream data") {
                        break;
                    }
                    return accept_status;
                }
                const TransportStatus priority_status =
                    assign_request_stream_priority(transport_, draft_version, request_stream_id);
                if (!priority_status.ok) {
                    return priority_status;
                }

                std::vector<std::uint8_t> request_bytes;
                std::vector<std::uint8_t> trailing_request_bytes;
                TransportStatus read_status =
                    read_request_stream_message(
                        transport_, request_stream_id, draft_version,
                        subscriber_timeout_, request_bytes,
                        &trailing_request_bytes);
                if (!read_status.ok) {
                    return read_status;
                }
                trace_control_message(request_bytes, draft_version);

                std::size_t request_offset = 0;
                std::uint64_t request_type = 0;
                if (!decode_moqint(request_bytes, request_offset, draft_version, request_type)) {
                    return protocol_violation(transport_, "failed to parse request stream type");
                }
                if (request_type == 0x03) {
                    SubscribeMessage subscribe;
                    if (!decode_subscribe_message(request_bytes, draft_version, subscribe)) {
                        TransportStatus write_status =
                            transport_.write_stream(request_stream_id,
                                                    encode_request_error_message(
                                                        draft_version, 0, 0x1, 0, "invalid SUBSCRIBE"),
                                                    true);
                        return write_status.ok ? protocol_violation(transport_, "received invalid SUBSCRIBE")
                                               : write_status;
                    }
                    if (!namespace_matches(subscribe.track_namespace, track_namespace_)) {
                        TransportStatus write_status =
                            transport_.write_stream(request_stream_id,
                                                    encode_request_error_message(
                                                        draft_version, subscribe.request_id, 0x2, 0, "track does not exist"),
                                                    true);
                        return write_status.ok ? TransportStatus::failure("peer requested unsupported track namespace")
                                               : write_status;
                    }
                    TransportStatus subscribe_status = accept_subscribe(subscribe, request_stream_id);
                    if (!subscribe_status.ok) {
                        return subscribe_status;
                    }
                    pending_subscription_request_bytes.insert_or_assign(
                        subscribe.request_id,
                        std::move(trailing_request_bytes));
                    const TransportStatus retained_update_status =
                        process_subscriber_request_updates();
                    if (!retained_update_status.ok) {
                        return retained_update_status;
                    }
                    continue;
                }
                if (request_type == 0x50) {
                    SubscribeNamespaceMessage subscribe_namespace;
                    if (!decode_subscribe_namespace_message(request_bytes, draft_version, subscribe_namespace)) {
                        TransportStatus write_status =
                            transport_.write_stream(request_stream_id,
                                                    encode_request_error_message(
                                                        draft_version, 0, 0x1, 0, "invalid SUBSCRIBE_NAMESPACE"),
                                                    true);
                        return write_status.ok ? protocol_violation(transport_, "received invalid SUBSCRIBE_NAMESPACE")
                                               : write_status;
                    }
                    const TransportStatus request_id_status =
                        peer_request_ids.validate(
                            transport_, subscribe_namespace.request_id);
                    if (!request_id_status.ok) {
                        return request_id_status;
                    }
                    if (!namespace_prefix_matches(subscribe_namespace.track_namespace_prefix, track_namespace_)) {
                        TransportStatus write_status =
                            transport_.write_stream(request_stream_id,
                                                    encode_request_error_message(
                                                        draft_version, subscribe_namespace.request_id, 0x2, 0,
                                                        "unsupported namespace prefix"),
                                                    true);
                        return write_status.ok ? TransportStatus::failure("peer requested unsupported namespace prefix")
                                               : write_status;
                    }
                    TransportStatus write_status =
                        transport_.write_stream(request_stream_id,
                                                encode_request_ok_message(draft_version, subscribe_namespace.request_id),
                                                false);
                    if (!write_status.ok) {
                        return write_status;
                    }
                    continue;
                }

                SubscribeTracksMessage subscribe_tracks;
                if (request_type != 0x51 ||
                    !decode_subscribe_tracks_message(request_bytes, draft_version, subscribe_tracks)) {
                    TransportStatus write_status =
                        transport_.write_stream(request_stream_id,
                                                encode_request_error_message(
                                                    draft_version, 0, 0x1, 0, "unsupported request stream"),
                                                true);
                    return write_status.ok ? protocol_violation(transport_, "received unsupported request stream")
                                           : write_status;
                }
                const TransportStatus request_id_status =
                    peer_request_ids.validate(transport_,
                                              subscribe_tracks.request_id);
                if (!request_id_status.ok) {
                    return request_id_status;
                }
                if (!namespace_prefix_matches(subscribe_tracks.track_namespace_prefix, track_namespace_)) {
                    TransportStatus write_status =
                        transport_.write_stream(request_stream_id,
                                                encode_request_error_message(
                                                    draft_version, 0, 0x2, 0, "unsupported namespace prefix"),
                                                true);
                    return write_status.ok ? TransportStatus::failure("peer requested unsupported namespace prefix")
                                           : write_status;
                }
                TransportStatus write_status =
                    transport_.write_stream(request_stream_id,
                                            encode_request_ok_message(draft_version, subscribe_tracks.request_id),
                                            false);
                if (!write_status.ok) {
                    return write_status;
                }
                if (subscribe_tracks.forward == 0) {
                    continue;
                }
                TransportStatus publish_status = publish_tracks();
                if (!publish_status.ok) {
                    return publish_status;
                }
            }
        }

        std::size_t message_size = 0;
        while (next_control_message(pending_control_bytes_, draft_version, message_size)) {
            const std::vector<std::uint8_t> message_bytes(
                pending_control_bytes_.begin(),
                pending_control_bytes_.begin() + static_cast<std::ptrdiff_t>(message_size));
            std::size_t offset = 0;
            std::uint64_t message_type = 0;
            if (!decode_moqint(message_bytes, offset, draft_version, message_type)) {
                return protocol_violation(transport_, "failed to parse control request type");
            }
            trace_control_message(message_bytes, draft_version);

            if ((draft_version == DraftVersion::kDraft18 &&
                 message_type == 0x02) ||
                (uses_request_streams(draft_version) &&
                 (message_type == 0x03 || message_type == 0x06 ||
                  message_type == 0x50 || message_type == 0x16 ||
                  message_type == 0x1d || message_type == 0x51))) {
                return protocol_violation(transport_, "draft-18 request message received on control stream");
            }
            if (message_type == 0x02 &&
                draft_version == DraftVersion::kDraft16) {
                const TransportStatus request_update_status =
                    process_draft16_control_request_update(
                        transport_,
                        message_bytes,
                        control_stream_id_,
                        peer_request_ids,
                        apply_subscriber_request_update);
                if (!request_update_status.ok) {
                    return request_update_status;
                }
            } else if (message_type == 0x03) {
                SubscribeMessage subscribe;
                if (!decode_subscribe_message(message_bytes, draft_version, subscribe)) {
                    return protocol_violation(transport_, "received invalid SUBSCRIBE");
                }
                if (!namespace_matches(subscribe.track_namespace, track_namespace_)) {
                    return TransportStatus::failure("peer requested unsupported track namespace");
                }
                TransportStatus subscribe_status = accept_subscribe(subscribe, control_stream_id_);
                if (!subscribe_status.ok) {
                    return subscribe_status;
                }
            } else if (message_type == 0x11) {
                SubscribeNamespaceMessage subscribe_namespace;
                if (decode_subscribe_namespace_message(message_bytes, draft_version, subscribe_namespace)) {
                    const TransportStatus request_id_status =
                        peer_request_ids.validate(
                            transport_, subscribe_namespace.request_id);
                    if (!request_id_status.ok) {
                        return request_id_status;
                    }
                    if (!namespace_prefix_matches(subscribe_namespace.track_namespace_prefix, track_namespace_)) {
                        return TransportStatus::failure("peer requested unsupported namespace prefix");
                    }
                    TransportStatus write_status =
                        transport_.write_stream(control_stream_id_,
                                                encode_subscribe_namespace_ok_message(
                                                    draft_version, subscribe_namespace.request_id),
                                                false);
                    if (!write_status.ok) {
                        return write_status;
                    }
                }
            }

            pending_control_bytes_.erase(
                pending_control_bytes_.begin(),
                pending_control_bytes_.begin() + static_cast<std::ptrdiff_t>(message_size));
        }
        return TransportStatus::success();
    };

    bool source_eof = false;
    bool control_fin = false;
    bool awaiting_source_catalog =
        source.catalog_mode == openmoq::publisher::LiveCatalogMode::kSourceObject;
    std::optional<std::chrono::steady_clock::time_point> object_pacing_start;
    std::optional<std::uint64_t> object_first_media_time_us;
    std::map<std::string, std::uint64_t> last_group_id_by_track;
    bool live_object_catalog_sent = !alias_by_track.contains("catalog");
    std::optional<openmoq::publisher::LiveObject> retained_source_catalog;
    std::size_t served_catalog_subscription_count = 0;
    const bool has_media_tracks = std::any_of(alias_by_track.begin(), alias_by_track.end(),
                                              [](const auto& entry) { return entry.first != "catalog"; });
    const auto has_media_subscription = [&]() {
        return std::any_of(subscribed_tracks.begin(), subscribed_tracks.end(),
                           [](const std::string& track_name) { return track_name != "catalog"; });
    };
    const auto catalog_subscription_count = [&]() {
        return static_cast<std::size_t>(std::count_if(
            active_subscriptions.begin(),
            active_subscriptions.end(),
            [](const auto& entry) { return entry.second.track_name == "catalog"; }));
    };
    const auto send_retained_catalog = [&]() -> TransportStatus {
        if (!retained_source_catalog.has_value()) {
            return TransportStatus::success();
        }
        const std::size_t subscription_count = catalog_subscription_count();
        const bool should_send =
            (auto_forward_ &&
             (!uses_request_streams(draft_version) || subscribed_tracks.contains("catalog")) &&
             !live_object_catalog_sent) ||
            (!auto_forward_ &&
             subscription_count > served_catalog_subscription_count);
        if (!should_send) {
            return TransportStatus::success();
        }

        const auto alias_it = alias_by_track.find("catalog");
        const auto& catalog = *retained_source_catalog;
        const openmoq::publisher::CmsfObject object{
            .kind = openmoq::publisher::CmsfObjectKind::kInitialization,
            .track_name = catalog.track_name,
            .group_id = catalog.group_id,
            .subgroup_id = catalog.subgroup_id,
            .object_id = catalog.object_id,
            .media_time_us = catalog.media_time_us,
            .media_duration_us = catalog.media_duration_us,
            .payload = {},
            .owned_payload = catalog.payload,
        };
        const std::uint64_t send_seq = next_send_seq();
        const DeliveryTimeouts delivery_timeouts =
            effective_delivery_timeouts("catalog");
        bool object_published = false;
        TransportStatus send_status = sender_by_track["catalog"].serve(
            transport_,
            draft_version,
            alias_it->second,
            send_seq,
            object,
            catalog.subgroup_contains_group_largest,
            catalog.final_in_subgroup,
            std::span<const std::uint8_t>(catalog.payload),
            delivery_timeouts,
            now_function_,
            &object_published,
            {.subscriber = subscriber_priority_for_track(
                 active_subscriptions, "catalog"),
             .publisher = 128});
        if (!send_status.ok) {
            return send_status;
        }
        if (object_published) {
            record_published_object(
                "catalog", static_cast<std::uint64_t>(catalog.group_id), catalog.payload.size());
        }
        live_object_catalog_sent = true;
        served_catalog_subscription_count = subscription_count;
        return TransportStatus::success();
    };
    const auto await_subscribe_deadline = std::chrono::steady_clock::now() + subscriber_timeout_;
    while (!source_eof) {
        if (stop_requested_.load(std::memory_order_acquire)) {
            break;
        }
        status = process_control_messages();
        if (!status.ok) {
            return status;
        }

        std::vector<std::uint8_t> chunk;
        bool read_fin = false;
        const bool requires_forward_permission =
            !auto_forward_ || uses_request_streams(draft_version);
        const bool waiting_for_first_subscription =
            requires_forward_permission && active_subscriptions.empty() && subscribed_tracks.empty();
        const bool waiting_for_media_subscription =
            requires_forward_permission && live_object_catalog_sent && has_media_tracks && !has_media_subscription();
        const bool waiting_for_required_subscription =
            waiting_for_first_subscription || waiting_for_media_subscription;
        const auto read_timeout = waiting_for_required_subscription && !uses_request_streams(draft_version)
                                      ? subscriber_timeout_
                                      : std::chrono::milliseconds(0);
        const TransportStatus read_status =
            transport_.read_stream(control_read_stream_id, chunk, read_fin, read_timeout);
        if (read_status.ok) {
            pending_control_bytes_.insert(pending_control_bytes_.end(), chunk.begin(), chunk.end());
            control_fin = read_fin;
        } else if (read_status.message == "timed out waiting for stream data" ||
                   read_status.message == "no queued read for stream") {
            if (waiting_for_required_subscription &&
                std::chrono::steady_clock::now() >= await_subscribe_deadline) {
                break;
            }
        } else {
            return read_status;
        }

        if (stop_requested_.load(std::memory_order_acquire)) {
            break;
        }

        status = process_control_messages();
        if (!status.ok) {
            return status;
        }
        status = send_retained_catalog();
        if (!status.ok) {
            if (stop_requested_.load(std::memory_order_acquire)) {
                break;
            }
            return status;
        }
        if (control_fin) {
            break;
        }

        if (requires_forward_permission && subscribed_tracks.empty()) {
            continue;
        }
        if (requires_forward_permission && live_object_catalog_sent && has_media_tracks && !has_media_subscription()) {
            continue;
        }

        std::optional<openmoq::publisher::LiveObject> next;
        try {
            next = source.next_object();
        } catch (const std::runtime_error& error) {
            // A catalog-build refusal (e.g. a protected track with no pssh
            // anywhere in the init segment, per CMSF 4.1.2) throws from
            // inside next_object() rather than returning nullopt. Return a
            // failure status instead of letting it unwind past this
            // function: the caller (Publisher::publish_live_objects in
            // publisher_api.cpp) only runs its normal teardown -- close(0) +
            // clear_active_session(), which populates stats_.last_error and
            // closes the MOQT session cleanly -- when publish_live_objects()
            // returns rather than throws.
            return TransportStatus::failure(
                std::string("live object source failed: ") + error.what());
        }
        if (!next.has_value()) {
            // A live source with a liveness predicate distinguishes a transient
            // media gap (keep polling, keep servicing control) from real EOF.
            if (source.is_finished && !source.is_finished()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (awaiting_source_catalog) {
                return TransportStatus::failure(
                    "source-catalog mode ended before producing its catalog object");
            }
            source_eof = true;
            break;
        }
        if (awaiting_source_catalog) {
            if (next->track_name != "catalog") {
                return TransportStatus::failure(
                    "source-catalog mode requires the first object to be the catalog");
            }
            if (next->payload.empty()) {
                return TransportStatus::failure(
                    "source-catalog mode requires a non-empty catalog object");
            }
            awaiting_source_catalog = false;
        }
        const auto alias_it = alias_by_track.find(next->track_name);
        if (alias_it == alias_by_track.end()) {
            return TransportStatus::failure("live object references unknown track: " + next->track_name);
        }
        note_largest_live_object(largest_object_by_track,
                                 next->track_name,
                                 next->group_id,
                                 next->object_id);
        if (next->track_name == "catalog") {
            retained_source_catalog = std::move(*next);
            status = send_retained_catalog();
            if (!status.ok) {
                if (stop_requested_.load(std::memory_order_acquire)) {
                    break;
                }
                return status;
            }
            if (stop_requested_.load(std::memory_order_acquire)) {
                break;
            }
            continue;
        }
        if (auto_forward_ && !live_object_catalog_sent) {
            continue;
        }
        if (auto_forward_ && uses_request_streams(draft_version) &&
            !subscribed_tracks.contains(next->track_name)) {
            continue;
        }
        if (!auto_forward_ && !subscribed_tracks.contains(next->track_name)) {
            continue;
        }

        const openmoq::publisher::CmsfObject object{
            .kind = openmoq::publisher::CmsfObjectKind::kMedia,
            .track_name = next->track_name,
            .group_id = next->group_id,
            .subgroup_id = next->subgroup_id,
            .object_id = next->object_id,
            .media_time_us = next->media_time_us,
            .media_duration_us = next->media_duration_us,
            .payload = {},
            .owned_payload = next->payload,
        };
        const auto published_settings_it =
            published_track_settings.find(next->track_name);
        if (!live_object_matches_request_union(
                object,
                draft_version,
                active_subscriptions,
                published_settings_it != published_track_settings.end()
                    ? &published_settings_it->second
                    : nullptr)) {
            continue;
        }
        const std::uint64_t send_seq = next_send_seq();
        if (paced_) {
            if (!object_pacing_start.has_value()) {
                object_pacing_start = std::chrono::steady_clock::now();
                object_first_media_time_us = object.media_time_us;
            }
            pace_until(*object_pacing_start, *object_first_media_time_us, object, true);
        }
        auto& sender = sender_by_track[next->track_name];
        const DeliveryTimeouts delivery_timeouts =
            effective_delivery_timeouts(next->track_name);
        const auto group_it = last_group_id_by_track.find(next->track_name);
        if (group_it != last_group_id_by_track.end() &&
            group_it->second != static_cast<std::uint64_t>(next->group_id)) {
            status = sender.finish_group(
                transport_, draft_version, delivery_timeouts, now_function_);
            if (!status.ok) {
                return status;
            }
        }
        bool object_published = false;
        status = sender.serve(
            transport_,
            draft_version,
            alias_it->second,
            send_seq,
            object,
            next->subgroup_contains_group_largest,
            next->final_in_subgroup,
            std::span<const std::uint8_t>(next->payload),
            delivery_timeouts,
            now_function_,
            &object_published,
            {.subscriber = subscriber_priority_for_track(
                 active_subscriptions,
                 next->track_name,
                 published_settings_it != published_track_settings.end()
                     ? published_settings_it->second.subscriber_priority
                     : 128),
             .publisher = 128});
        if (!status.ok) {
            // A send failure that races with a concurrent close() is a benign
            // stop, not a transport error: break to the flush.
            if (stop_requested_.load(std::memory_order_acquire)) {
                break;
            }
            return status;
        }
        if (!object_published) {
            continue;
        }
        record_published_object(next->track_name,
                                static_cast<std::uint64_t>(next->group_id),
                                next->payload.size());
        last_group_id_by_track[next->track_name] = static_cast<std::uint64_t>(next->group_id);
        if (stop_requested_.load(std::memory_order_acquire)) {
            break;
        }
    }

    // Bound the post-loop graceful flush. When a stop is in progress, a flush
    // write can fail (transport torn down by a concurrent close()) or the total
    // flush can run long; in either case return success rather than surfacing a
    // stop-driven flush failure. The deadline is secondary insurance for a
    // multi-write flush that is merely slow.
    const auto flush_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    const auto flush_budget_exhausted = [&]() {
        return stop_requested_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() >= flush_deadline;
    };
    const auto stopping = [&]() {
        return stop_requested_.load(std::memory_order_acquire);
    };

    for (auto& [track_name, sender] : sender_by_track) {
        if (flush_budget_exhausted()) {
            return TransportStatus::success();
        }
        status = sender.finish_group(
            transport_,
            draft_version,
            effective_delivery_timeouts(track_name),
            now_function_);
        if (!status.ok) {
            return stopping() ? TransportStatus::success() : status;
        }
    }

    for (const auto& [request_id, subscribe] : active_subscriptions) {
        if (flush_budget_exhausted()) {
            return TransportStatus::success();
        }
        const auto stream_it = active_subscription_stream_ids.find(request_id);
        const std::uint64_t response_stream_id =
            uses_request_streams(draft_version) && stream_it != active_subscription_stream_ids.end()
                ? stream_it->second
                : control_stream_id_;
        status = transport_.write_stream(response_stream_id,
                                         encode_publish_done_message(
                                             draft_version, request_id, sender_by_track[subscribe.track_name].stream_count()),
                                         false);
        if (!status.ok) {
            return stopping() ? TransportStatus::success() : status;
        }
    }
    for (const auto& [track_name, request_id] : published_track_request_ids) {
        if (terminated_publish_request_ids.contains(request_id)) {
            continue;
        }
        if (flush_budget_exhausted()) {
            return TransportStatus::success();
        }
        status = write_publish_done_for_request(transport_,
                                                draft_version,
                                                control_stream_id_,
                                                publish_stream_id_by_request_id_,
                                                request_id,
                                                sender_by_track[track_name].stream_count());
        if (!status.ok) {
            return stopping() ? TransportStatus::success() : status;
        }
        publish_stream_id_by_request_id_.erase(request_id);
    }

    if (flush_budget_exhausted()) {
        return TransportStatus::success();
    }
    const auto namespace_done_status = write_namespace_done_for_request(transport_,
                                            draft_version,
                                            control_stream_id_,
                                            namespace_stream_id_,
                                            namespace_message);
    if (!namespace_done_status.ok && stopping()) {
        return TransportStatus::success();
    }
    return namespace_done_status;
}

TransportStatus MoqtSession::close(std::uint64_t application_error_code) {
    stop_requested_.store(true, std::memory_order_release);
    if (namespace_stream_open_) {
        transport_.reset_stream(namespace_stream_id_, 0x0);
        namespace_stream_id_ = 0;
        namespace_stream_open_ = false;
    }
    for (const auto& entry : publish_stream_id_by_request_id_) {
        transport_.reset_stream(entry.second, 0x0);
    }
    publish_stream_id_by_request_id_.clear();
    control_stream_open_ = false;
    control_stream_id_ = 0;
    peer_control_stream_open_ = false;
    peer_control_stream_id_ = 0;
    return transport_.close(application_error_code);
}

TransportStatus MoqtSession::end_broadcast(openmoq::publisher::EndBroadcastMode mode,
                                           openmoq::publisher::DraftVersion draft_version) {
    TransportStatus status = TransportStatus::success();

    // Publish the final independent catalog (MSF 11.3) via the persistent
    // catalog_publisher_ member: isComplete with empty tracks for kTerminate,
    // or isLive false for kConvertToVod. Per-track media durations are not
    // tracked as persistent per-track state on MoqtSession, so this always
    // passes an empty duration map -- every track is still correctly marked
    // not live, it simply omits trackDuration rather than reporting a
    // fabricated figure.
    //
    // Guarded by catalog_track_alias_known_: only publish_live()'s two
    // overloads ever call catalog_publisher_.publish(), so only they can
    // know catalog_track_alias_ actually names the catalog track (see the
    // member's own comment). A session driven through publish() or
    // publish_live_objects() skips this write entirely rather than risk
    // injecting a catalog payload onto an arbitrary track's alias.
    // Additionally guarded by ended() rather than a try/catch: a second
    // end_broadcast() call should still send media PUBLISH_DONE below
    // instead of bailing out entirely.
    if (catalog_track_alias_known_ && !catalog_publisher_.ended()) {
        const auto catalog_objects = catalog_publisher_.end_broadcast(mode, {});
        const TransportStatus catalog_status =
            send_catalog_objects(draft_version,
                                 catalog_track_alias_,
                                 catalog_objects,
                                 catalog_delivery_timeouts_snapshot());
        if (!catalog_status.ok) {
            status = catalog_status;
        }
    } else if (!catalog_track_alias_known_) {
        std::cerr << "[moqt-session] end_broadcast: skipping final catalog write -- this session "
                     "never populated catalog_publisher_ (not driven through publish_live())\n";
    } else {
        std::cerr << "[moqt-session] end_broadcast: skipping final catalog write -- "
                     "catalog_publisher_ already ended (end_broadcast called before, or a live "
                     "catalog subscription already closed via publish_live()'s own end)\n";
    }

    // publish_stream_id_by_request_id_ is the only per-request state that
    // survives outside the blocking publish_*() loop, so it is also the only
    // set of requests we can honestly report as "ended" here. Snapshot the
    // request IDs before writing so mutating the map mid-loop (if a future
    // change adds that) can't invalidate the iteration.
    std::vector<std::uint64_t> request_ids;
    request_ids.reserve(publish_stream_id_by_request_id_.size());
    for (const auto& entry : publish_stream_id_by_request_id_) {
        request_ids.push_back(entry.first);
    }

    for (const std::uint64_t request_id : request_ids) {
        // stream_count is real per-track state owned by SubgroupSenderState
        // inside the blocking publish loop, not persisted on MoqtSession.
        // Report 0 (not tracked) rather than fabricate a number.
        const TransportStatus done_status = write_publish_done_for_request(
            transport_, draft_version, control_stream_id_, publish_stream_id_by_request_id_, request_id,
            /*stream_count=*/0);
        if (!done_status.ok) {
            status = done_status;
            continue;
        }
        publish_stream_id_by_request_id_.erase(request_id);
    }
    return status;
}

TransportStatus MoqtSession::ensure_control_stream(openmoq::publisher::DraftVersion draft) {
    if (control_stream_open_) {
        return TransportStatus::success();
    }

    const StreamDirection direction = uses_request_streams(draft)
                                          ? StreamDirection::kUnidirectional
                                          : StreamDirection::kBidirectional;
    TransportStatus status = transport_.open_stream(direction, control_stream_id_);
    if (!status.ok) {
        return status;
    }
    if (uses_priority_scheduler(draft)) {
        status = transport_.set_reliable_stream_priority(control_stream_id_, 0);
        if (!status.ok) {
            return status;
        }
    }

    control_stream_open_ = true;
    return TransportStatus::success();
}

TransportStatus MoqtSession::ensure_setup(openmoq::publisher::DraftVersion draft) {
    if (setup_complete_) {
        return TransportStatus::success();
    }

    if (!endpoint_.has_value()) {
        return TransportStatus::failure("session endpoint is not configured");
    }

    TransportStatus status = ensure_control_stream(draft);
    if (!status.ok) {
        return status;
    }

    std::string authority = endpoint_->host + ":" + std::to_string(endpoint_->port);
    const std::uint64_t max_request_id =
        endpoint_->transport == openmoq::publisher::transport::TransportKind::kWebTransport ? 128 : 100;
    const std::vector<std::uint8_t> setup_bytes = encode_setup_message({
        .draft = draft,
        .transport = endpoint_->transport,
        .authority = authority,
        .path = endpoint_->path,
        .max_request_id = max_request_id,
        .authorization_token = setup_authorization_token(),
    });
    status = write_frame(control_stream_id_, setup_bytes, false);
    if (!status.ok) {
        return status;
    }
    if (trace_enabled()) {
        std::cerr << "[moqt-session] sent "
                  << (uses_request_streams(draft) ? "SETUP" : "CLIENT_SETUP")
                  << " stream=" << control_stream_id_
                  << " draft=" << openmoq::publisher::to_string(draft)
                  << " transport="
                  << (endpoint_->transport == openmoq::publisher::transport::TransportKind::kWebTransport ? "webtransport"
                                                                                                            : "raw")
                  << " bytes=[" << hex_dump(setup_bytes) << "]"
                  << std::endl;
    }

    std::vector<std::uint8_t> response = std::move(pending_control_bytes_);
    pending_control_bytes_.clear();
    std::vector<std::uint8_t> chunk;
    bool fin = false;
    bool saw_setup_response = false;
    std::size_t consumed = 0;

    while (true) {
        std::size_t message_size = 0;
        if (!next_control_message(std::span<const std::uint8_t>(response).subspan(consumed), draft, message_size)) {
            if (fin) {
                break;
            }
            if (endpoint_->transport == openmoq::publisher::transport::TransportKind::kWebTransport) {
                bool saw_session_bytes = false;
                bool session_fin = false;
                const TransportStatus session_status = try_read_wt_session_stream(transport_, saw_session_bytes, session_fin);
                if (session_status.ok) {
                    if (session_fin) {
                        return TransportStatus::failure("webtransport session control stream closed during setup");
                    }
                    if (saw_session_bytes) {
                        continue;
                    }
                    continue;
                }
            }
            if (uses_request_streams(draft) && !peer_control_stream_open_) {
                status = transport_.accept_stream(StreamDirection::kUnidirectional,
                                                  peer_control_stream_id_,
                                                  std::chrono::seconds(5));
                if (!status.ok) {
                    return status;
                }
                peer_control_stream_open_ = true;
            }
            const std::uint64_t read_stream_id =
                uses_request_streams(draft) ? peer_control_stream_id_ : control_stream_id_;
            chunk.clear();
            status = transport_.read_stream(read_stream_id, chunk, fin, std::chrono::seconds(5));
            if (!status.ok) {
                if (endpoint_->transport == openmoq::publisher::transport::TransportKind::kWebTransport) {
                    bool saw_session_bytes = false;
                    bool session_fin = false;
                    const TransportStatus session_status = try_read_wt_session_stream(transport_, saw_session_bytes, session_fin);
                    if (session_status.ok) {
                        if (session_fin) {
                            return TransportStatus::failure("webtransport session control stream closed during setup");
                        }
                        if (saw_session_bytes) {
                            continue;
                        }
                        continue;
                    }
                }
                if (trace_enabled()) {
                    std::cerr << "[moqt-session] setup read failed stream=" << control_stream_id_
                              << " error=" << status.message << std::endl;
                }
                return status;
            }
            if (trace_enabled()) {
                std::cerr << "[moqt-session] setup read now_ms="
                          << trace_elapsed_ms(std::chrono::steady_clock::now())
                          << " stream=" << control_stream_id_
                          << " bytes=" << chunk.size()
                          << " fin=" << (fin ? 1 : 0)
                          << " buffered=" << response.size() + chunk.size() << std::endl;
            }
            response.insert(response.end(), chunk.begin(), chunk.end());
            continue;
        }

        const std::vector<std::uint8_t> message_bytes(response.begin() + static_cast<std::ptrdiff_t>(consumed),
                                                      response.begin() + static_cast<std::ptrdiff_t>(consumed + message_size));
        std::size_t offset = 0;
        std::uint64_t message_type = 0;
        if (!decode_moqint(message_bytes, offset, draft, message_type)) {
            return protocol_violation(transport_, "failed to parse setup response type");
        }

        if (!saw_setup_response) {
            ServerSetupMessage setup_response;
            if (!decode_setup_response_message(message_bytes, draft, setup_response)) {
                if (trace_enabled()) {
                    std::cerr << "[moqt-session] invalid "
                              << (uses_request_streams(draft) ? "SETUP" : "SERVER_SETUP")
                              << " bytes=[" << hex_dump(message_bytes) << "]"
                              << std::endl;
                }
                return protocol_violation(
                    transport_,
                    uses_request_streams(draft) ? "received invalid SETUP message"
                                                                         : "received invalid SERVER_SETUP message");
            }
            saw_setup_response = true;
            // Older drafts can carry max_request_id in SERVER_SETUP. Draft-18
            // SETUP has no corresponding peer request credit parameter here.
            if (setup_response.max_request_id != 0) {
                peer_max_request_id_ = setup_response.max_request_id;
            }
        } else if (draft == openmoq::publisher::DraftVersion::kDraft16 && message_type == 0x15) {
            MaxRequestIdMessage max_request_id;
            if (!decode_max_request_id_message(message_bytes, max_request_id)) {
                return protocol_violation(transport_, "received invalid MAX_REQUEST_ID message");
            }
            peer_max_request_id_ = max_request_id.max_request_id;
        } else {
            break;
        }

        consumed += message_size;
        if (saw_setup_response &&
            (draft == openmoq::publisher::DraftVersion::kDraft14 || uses_request_streams(draft) ||
             !auto_forward_ || peer_max_request_id_ != 0)) {
            break;
        }
    }

    if (!saw_setup_response) {
        return TransportStatus::failure(uses_request_streams(draft)
                                            ? "received incomplete SETUP message"
                                            : "received incomplete SERVER_SETUP message");
    }

    pending_control_bytes_.assign(response.begin() + static_cast<std::ptrdiff_t>(consumed), response.end());
    setup_complete_ = true;
    return TransportStatus::success();
}

TransportStatus MoqtSession::write_frame(std::uint64_t stream_id,
                                         std::span<const std::uint8_t> frame,
                                         bool fin) {
    return transport_.write_stream(stream_id, frame, fin);
}

}  // namespace openmoq::publisher::transport
