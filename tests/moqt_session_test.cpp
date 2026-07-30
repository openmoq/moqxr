#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/cat4moq.h"
#include "openmoq/publisher/moq_draft.h"
#include "openmoq/publisher/transport/moqt_control_messages.h"
#include "openmoq/publisher/transport/moqt_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using openmoq::publisher::ByteSpan;
using openmoq::publisher::CmsfObject;
using openmoq::publisher::CmsfObjectKind;
using openmoq::publisher::DraftVersion;
using openmoq::publisher::LiveCatalogMode;
using openmoq::publisher::LiveObject;
using openmoq::publisher::LiveObjectSource;
using openmoq::publisher::LiveTrack;
using openmoq::publisher::PublishPlan;
using openmoq::publisher::TrackDescription;
using openmoq::publisher::materialize_publish_plan;
using openmoq::publisher::transport::ConnectionState;
using openmoq::publisher::transport::SubscribeMessage;
using openmoq::publisher::transport::SubscribeNamespaceMessage;
using openmoq::publisher::transport::EndpointConfig;
using openmoq::publisher::transport::MoqtSession;
using openmoq::publisher::transport::PublisherTransport;
using openmoq::publisher::transport::StreamDirection;
using openmoq::publisher::transport::TlsConfig;
using openmoq::publisher::transport::TransportStatus;
using openmoq::publisher::transport::decode_varint;
using openmoq::publisher::transport::encode_setup_message;
using openmoq::publisher::transport::encode_server_setup_message;
using openmoq::publisher::transport::encode_varint;

std::vector<std::uint8_t> encode_vi64(std::uint64_t value) {
    if (value <= 127) {
        return {static_cast<std::uint8_t>(value)};
    }
    if (value <= 16383) {
        return {
            static_cast<std::uint8_t>(0x80 | ((value >> 8) & 0x3f)),
            static_cast<std::uint8_t>(value & 0xff),
        };
    }
    if (value <= 2097151ULL) {
        return {
            static_cast<std::uint8_t>(0xc0 | ((value >> 16) & 0x1f)),
            static_cast<std::uint8_t>((value >> 8) & 0xff),
            static_cast<std::uint8_t>(value & 0xff),
        };
    }
    return {
        static_cast<std::uint8_t>(0xe0 | ((value >> 24) & 0x0f)),
        static_cast<std::uint8_t>((value >> 16) & 0xff),
        static_cast<std::uint8_t>((value >> 8) & 0xff),
        static_cast<std::uint8_t>(value & 0xff),
    };
}

std::vector<std::uint8_t> encode_moqint(DraftVersion draft, std::uint64_t value) {
    return (draft == DraftVersion::kDraft17 || draft == DraftVersion::kDraft18)
               ? encode_vi64(value)
               : encode_varint(value);
}

void append_bytes(std::vector<std::uint8_t>& out, std::vector<std::uint8_t> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
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

struct MockTransport final : PublisherTransport {
    struct WriteEvent {
        std::uint64_t stream_id = 0;
        std::vector<std::uint8_t> bytes;
        bool fin = false;
    };
    struct OpenEvent {
        StreamDirection direction = StreamDirection::kBidirectional;
        std::uint64_t stream_id = 0;
    };

    TransportStatus configure(const EndpointConfig& endpoint, const TlsConfig& tls) override {
        endpoint_ = endpoint;
        tls_ = tls;
        return TransportStatus::success();
    }

    TransportStatus connect() override {
        state_ = ConnectionState::kConnected;
        return TransportStatus::success();
    }

    ConnectionState state() const override {
        return state_;
    }

    TransportStatus open_stream(StreamDirection direction, std::uint64_t& stream_id) override {
        if (state_ != ConnectionState::kConnected) {
            return TransportStatus::failure("not connected");
        }

        if (direction == StreamDirection::kBidirectional) {
            stream_id = next_bidi_;
            next_bidi_ += 4;
        } else {
            stream_id = next_uni_;
            next_uni_ += 4;
        }
        opens.push_back({direction, stream_id});
        return TransportStatus::success();
    }

    TransportStatus accept_stream(StreamDirection direction,
                                  std::uint64_t& stream_id,
                                  std::chrono::milliseconds timeout) override {
        read_timeouts.push_back(timeout);
        const auto matches_direction = [direction](std::uint64_t id) {
            if (direction == StreamDirection::kBidirectional) {
                return (id & 0x3ULL) == 0x1ULL;
            }
            return (id & 0x3ULL) == 0x3ULL;
        };
        for (const auto& [candidate_stream_id, chunks] : reads) {
            static_cast<void>(chunks);
            if (matches_direction(candidate_stream_id) && !accepted_streams.contains(candidate_stream_id)) {
                accepted_streams.insert(candidate_stream_id);
                stream_id = candidate_stream_id;
                return TransportStatus::success();
            }
        }
        if (on_accept_timeout) {
            on_accept_timeout(*this, direction);
        }
        return TransportStatus::failure("timed out waiting for stream data");
    }

    TransportStatus write_stream(std::uint64_t stream_id,
                                 std::span<const std::uint8_t> bytes,
                                 bool fin) override {
        // A blocked write is released either explicitly or by transport close,
        // mirroring how a real picoquic write unblocks when the connection is
        // torn down.
        while (block_writes.load() && !release_writes.load() &&
               state_ != ConnectionState::kClosed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (state_ == ConnectionState::kClosed) {
            return TransportStatus::failure("transport closed");
        }
        writes.push_back({
            .stream_id = stream_id,
            .bytes = std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
            .fin = fin,
        });
        return TransportStatus::success();
    }

    TransportStatus read_stream(std::uint64_t stream_id,
                                std::vector<std::uint8_t>& bytes,
                                bool& fin,
                                std::chrono::milliseconds timeout) override {
        read_timeouts.push_back(timeout);
        ++read_count;
        if (on_read) {
            on_read(*this, stream_id);
        }
        const auto it = reads.find(stream_id);
        if (it == reads.end()) {
            if (!missing_read_error.empty()) {
                return TransportStatus::failure(missing_read_error);
            }
            return timeout == std::chrono::milliseconds(0) ? TransportStatus::failure("timed out waiting for stream data")
                                                           : TransportStatus::failure("no queued read for stream");
        }

        if (it->second.empty()) {
            reads.erase(it);
            return TransportStatus::failure("queued stream had no chunks");
        }

        bytes = it->second.front();
        it->second.erase(it->second.begin());
        fin = it->second.empty();
        if (fin) {
            reads.erase(it);
        }
        return TransportStatus::success();
    }

    TransportStatus close(std::uint64_t application_error_code) override {
        last_close_code = application_error_code;
        state_ = ConnectionState::kClosed;
        return TransportStatus::success();
    }

    TransportStatus reset_stream(std::uint64_t stream_id, std::uint64_t error_code) override {
        reset_calls.emplace_back(stream_id, error_code);
        return TransportStatus::success();
    }

    std::string connection_id() const override {
        return "mock-connection-id";
    }

    EndpointConfig endpoint_;
    TlsConfig tls_;
    ConnectionState state_ = ConnectionState::kIdle;
    std::uint64_t next_bidi_ = 0;
    std::uint64_t next_uni_ = 2;
    std::uint64_t last_close_code = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> reset_calls;
    std::size_t read_count = 0;
    std::string missing_read_error;
    std::vector<OpenEvent> opens;
    std::vector<WriteEvent> writes;
    std::vector<std::chrono::milliseconds> read_timeouts;
    std::map<std::uint64_t, std::vector<std::vector<std::uint8_t>>> reads;
    std::set<std::uint64_t> accepted_streams;
    std::function<void(MockTransport&, StreamDirection)> on_accept_timeout;
    std::function<void(MockTransport&, std::uint64_t)> on_read;
    std::atomic<bool> block_writes{false};
    std::atomic<bool> release_writes{false};
};

std::vector<std::size_t> object_write_indices(const MockTransport& transport) {
    std::vector<std::size_t> indices;
    for (std::size_t index = 0; index < transport.writes.size(); ++index) {
        if (transport.writes[index].stream_id != 0) {
            indices.push_back(index);
        }
    }
    return indices;
}

std::size_t control_message_count(const MockTransport& transport, std::uint8_t type) {
    std::size_t count = 0;
    for (const auto& write : transport.writes) {
        if (write.stream_id == 0) {
            std::size_t cursor = 0;
            std::uint64_t decoded_type = 0;
            if (decode_varint(write.bytes, cursor, decoded_type) && decoded_type == type) {
                ++count;
            }
        }
    }
    return count;
}

void append_be16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void append_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

std::vector<std::uint8_t> make_box(std::string_view type, std::vector<std::uint8_t> payload) {
    std::vector<std::uint8_t> out;
    out.reserve(8 + payload.size());
    append_be32(out, static_cast<std::uint32_t>(8 + payload.size()));
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> make_full_box(std::string_view type, std::vector<std::uint8_t> payload) {
    std::vector<std::uint8_t> full_payload(4, 0);
    full_payload.insert(full_payload.end(), payload.begin(), payload.end());
    return make_box(type, std::move(full_payload));
}

std::vector<std::uint8_t> concat(std::initializer_list<std::vector<std::uint8_t>> boxes) {
    std::vector<std::uint8_t> out;
    for (const auto& box : boxes) {
        out.insert(out.end(), box.begin(), box.end());
    }
    return out;
}

std::vector<std::uint8_t> make_live_init_mp4() {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = make_full_box("tkhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto mdhd = make_full_box("mdhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x5d, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0});
    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    auto visual_header = std::vector<std::uint8_t>(78, 0);
    visual_header[24] = 0x01;
    visual_header[25] = 0x40;
    visual_header[26] = 0x00;
    visual_header[27] = 0xf0;
    const auto sample_entry = make_box("avc1", concat({visual_header, make_box("avcC", {1, 100, 0, 12, 0xff})}));
    const auto stsd = make_full_box("stsd", concat({std::vector<std::uint8_t>{0, 0, 0, 1}, sample_entry}));
    const auto stbl = make_box("stbl", stsd);
    const auto minf = make_box("minf", stbl);
    const auto mdia = make_box("mdia", concat({mdhd, hdlr, minf}));
    const auto trak = make_box("trak", concat({tkhd, mdia}));
    const auto moov = make_box("moov", trak);
    return concat({ftyp, moov});
}

std::vector<std::uint8_t> encode_publish_namespace_ok_message(DraftVersion draft, std::uint64_t request_id) {
    std::vector<std::uint8_t> payload;
    if (draft != DraftVersion::kDraft18) {
        payload = encode_moqint(draft, request_id);
    }
    if (draft == DraftVersion::kDraft16) {
        const std::vector<std::uint8_t> parameter_count = encode_varint(0);
        payload.insert(payload.end(), parameter_count.begin(), parameter_count.end());
    } else if (draft == DraftVersion::kDraft18) {
        const std::vector<std::uint8_t> parameter_count = encode_moqint(draft, 0);
        payload.insert(payload.end(), parameter_count.begin(), parameter_count.end());
    }
    std::vector<std::uint8_t> message = encode_moqint(draft, 0x07);
    append_be16(message, static_cast<std::uint16_t>(payload.size()));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

std::vector<std::uint8_t> encode_subscribe_namespace_message(DraftVersion draft,
                                                             std::uint64_t request_id,
                                                             std::string_view track_namespace) {
    std::vector<std::uint8_t> payload = encode_moqint(draft, request_id);
    const std::vector<std::uint8_t> tuple_len = encode_moqint(draft, 1);
    const std::vector<std::uint8_t> component_len = encode_moqint(draft, track_namespace.size());
    payload.insert(payload.end(), tuple_len.begin(), tuple_len.end());
    payload.insert(payload.end(), component_len.begin(), component_len.end());
    payload.insert(payload.end(), track_namespace.begin(), track_namespace.end());
    if (draft == DraftVersion::kDraft16) {
        const std::vector<std::uint8_t> subscribe_options = encode_varint(0x02);
        payload.insert(payload.end(), subscribe_options.begin(), subscribe_options.end());
    }
    const std::vector<std::uint8_t> parameter_count = encode_moqint(draft, 0);
    payload.insert(payload.end(), parameter_count.begin(), parameter_count.end());

    std::vector<std::uint8_t> message = encode_moqint(draft, draft == DraftVersion::kDraft18 ? 0x50 : 0x11);
    if (draft == DraftVersion::kDraft16 || draft == DraftVersion::kDraft18) {
        append_be16(message, static_cast<std::uint16_t>(payload.size()));
    } else {
        const std::vector<std::uint8_t> length = encode_varint(payload.size());
        message.insert(message.end(), length.begin(), length.end());
    }
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

std::vector<std::uint8_t> encode_subscribe_message(std::uint64_t request_id,
                                                   std::string_view track_namespace,
                                                   std::string_view track_name,
                                                   std::uint8_t forward,
                                                   DraftVersion draft = DraftVersion::kDraft14) {
    std::vector<std::uint8_t> payload = encode_moqint(draft, request_id);
    const std::vector<std::uint8_t> tuple_len = encode_moqint(draft, 1);
    const std::vector<std::uint8_t> component_len = encode_moqint(draft, track_namespace.size());
    payload.insert(payload.end(), tuple_len.begin(), tuple_len.end());
    payload.insert(payload.end(), component_len.begin(), component_len.end());
    payload.insert(payload.end(), track_namespace.begin(), track_namespace.end());
    const std::vector<std::uint8_t> track_name_length = encode_moqint(draft, track_name.size());
    payload.insert(payload.end(), track_name_length.begin(), track_name_length.end());
    payload.insert(payload.end(), track_name.begin(), track_name.end());

    if (draft == DraftVersion::kDraft14) {
        payload.push_back(0x80);  // subscriber_priority
        payload.push_back(0x01);  // group_order
        payload.push_back(forward);  // forward
        const std::vector<std::uint8_t> filter_type = encode_varint(0x03);  // AbsoluteStart
        const std::vector<std::uint8_t> start_group = encode_varint(0);
        const std::vector<std::uint8_t> start_object = encode_varint(0);
        const std::vector<std::uint8_t> parameter_count = encode_varint(0);
        payload.insert(payload.end(), filter_type.begin(), filter_type.end());
        payload.insert(payload.end(), start_group.begin(), start_group.end());
        payload.insert(payload.end(), start_object.begin(), start_object.end());
        payload.insert(payload.end(), parameter_count.begin(), parameter_count.end());
    } else {
        // Draft-16: parameters as delta-encoded KVPs.
        // FORWARD(0x10), SUBSCRIBER_PRIORITY(0x20), SUBSCRIPTION_FILTER(0x21)
        const std::vector<std::uint8_t> parameter_count = encode_moqint(draft, 3);
        payload.insert(payload.end(), parameter_count.begin(), parameter_count.end());
        // FORWARD (0x10, even) delta=0x10, value=forward
        const std::vector<std::uint8_t> forward_delta = encode_moqint(draft, 0x10);
        const std::vector<std::uint8_t> forward_value = encode_moqint(draft, forward);
        payload.insert(payload.end(), forward_delta.begin(), forward_delta.end());
        payload.insert(payload.end(), forward_value.begin(), forward_value.end());
        // SUBSCRIBER_PRIORITY (0x20, even) delta=0x10
        const std::vector<std::uint8_t> priority_delta = encode_moqint(draft, 0x20 - 0x10);
        const std::vector<std::uint8_t> priority_value = encode_moqint(draft, 0x80);
        payload.insert(payload.end(), priority_delta.begin(), priority_delta.end());
        payload.insert(payload.end(), priority_value.begin(), priority_value.end());
        // SUBSCRIPTION_FILTER (0x21, odd) delta=0x01
        // Value: FilterType(0x03=AbsoluteStart) + StartGroup(0) + StartObject(0)
        const std::vector<std::uint8_t> filter_delta = encode_moqint(draft, 0x21 - 0x20);
        std::vector<std::uint8_t> filter_value;
        const std::vector<std::uint8_t> ft = encode_moqint(draft, 0x03);
        const std::vector<std::uint8_t> sg = encode_moqint(draft, 0);
        const std::vector<std::uint8_t> so = encode_moqint(draft, 0);
        filter_value.insert(filter_value.end(), ft.begin(), ft.end());
        filter_value.insert(filter_value.end(), sg.begin(), sg.end());
        filter_value.insert(filter_value.end(), so.begin(), so.end());
        const std::vector<std::uint8_t> filter_len = encode_moqint(draft, filter_value.size());
        payload.insert(payload.end(), filter_delta.begin(), filter_delta.end());
        payload.insert(payload.end(), filter_len.begin(), filter_len.end());
        payload.insert(payload.end(), filter_value.begin(), filter_value.end());
    }

    std::vector<std::uint8_t> message = encode_moqint(draft, 0x03);
    append_be16(message, static_cast<std::uint16_t>(payload.size()));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

std::vector<std::uint8_t> encode_subscribe_tracks_message(std::uint64_t request_id,
                                                          std::string_view track_namespace,
                                                          std::uint8_t forward = 1) {
    constexpr DraftVersion draft = DraftVersion::kDraft18;
    std::vector<std::uint8_t> payload = encode_moqint(draft, request_id);
    const std::vector<std::uint8_t> tuple_len = encode_moqint(draft, 1);
    const std::vector<std::uint8_t> component_len = encode_moqint(draft, track_namespace.size());
    payload.insert(payload.end(), tuple_len.begin(), tuple_len.end());
    payload.insert(payload.end(), component_len.begin(), component_len.end());
    payload.insert(payload.end(), track_namespace.begin(), track_namespace.end());
    if (forward == 0) {
        const std::vector<std::uint8_t> parameter_count = encode_moqint(draft, 1);
        const std::vector<std::uint8_t> forward_delta = encode_moqint(draft, 0x10);
        const std::vector<std::uint8_t> forward_value = encode_moqint(draft, 0);
        payload.insert(payload.end(), parameter_count.begin(), parameter_count.end());
        payload.insert(payload.end(), forward_delta.begin(), forward_delta.end());
        payload.insert(payload.end(), forward_value.begin(), forward_value.end());
    } else {
        const std::vector<std::uint8_t> parameter_count = encode_moqint(draft, 0);
        payload.insert(payload.end(), parameter_count.begin(), parameter_count.end());
    }

    std::vector<std::uint8_t> message = encode_moqint(draft, 0x51);
    append_be16(message, static_cast<std::uint16_t>(payload.size()));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

std::vector<std::uint8_t> encode_subscribe_update_message(std::uint64_t request_id,
                                                          std::uint64_t subscription_request_id = 6) {
    std::vector<std::uint8_t> payload = encode_varint(request_id);
    const std::vector<std::uint8_t> subscription_request_id_bytes = encode_varint(subscription_request_id);
    const std::vector<std::uint8_t> start_group = encode_varint(0);
    const std::vector<std::uint8_t> start_object = encode_varint(0);
    const std::vector<std::uint8_t> end_group_plus_one = encode_varint(1);
    const std::vector<std::uint8_t> parameter_count = encode_varint(0);
    payload.insert(payload.end(), subscription_request_id_bytes.begin(), subscription_request_id_bytes.end());
    payload.insert(payload.end(), start_group.begin(), start_group.end());
    payload.insert(payload.end(), start_object.begin(), start_object.end());
    payload.insert(payload.end(), end_group_plus_one.begin(), end_group_plus_one.end());
    payload.push_back(0x80);
    payload.push_back(0x01);
    payload.insert(payload.end(), parameter_count.begin(), parameter_count.end());
    std::vector<std::uint8_t> message = encode_varint(0x02);
    append_be16(message, static_cast<std::uint16_t>(payload.size()));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

std::vector<std::uint8_t> encode_unsubscribe_message(DraftVersion draft, std::uint64_t request_id) {
    std::vector<std::uint8_t> payload = encode_moqint(draft, request_id);
    std::vector<std::uint8_t> message = encode_moqint(draft, 0x0a);
    append_be16(message, static_cast<std::uint16_t>(payload.size()));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

std::vector<std::uint8_t> encode_legacy_subscribe_update_message(std::uint64_t track_alias) {
    std::vector<std::uint8_t> payload = encode_varint(track_alias);
    const std::vector<std::uint8_t> start_group = encode_varint(0);
    const std::vector<std::uint8_t> start_object = encode_varint(0);
    payload.insert(payload.end(), start_group.begin(), start_group.end());
    payload.insert(payload.end(), start_object.begin(), start_object.end());
    payload.push_back(0x80);
    payload.push_back(0x01);
    payload.push_back(0x10);
    payload.push_back(0x01);

    std::vector<std::uint8_t> message = encode_varint(0x02);
    append_be16(message, static_cast<std::uint16_t>(payload.size()));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

std::vector<std::uint8_t> encode_publish_ok_message(DraftVersion draft,
                                                    std::uint64_t request_id,
                                                    std::uint8_t forward = 1) {
    std::vector<std::uint8_t> payload = encode_varint(request_id);
    if (draft == DraftVersion::kDraft14) {
        payload.push_back(forward);
        payload.push_back(0x80);
        payload.push_back(0x01);
        const std::vector<std::uint8_t> filter_type = encode_varint(0);
        const std::vector<std::uint8_t> parameter_count = encode_varint(0);
        payload.insert(payload.end(), filter_type.begin(), filter_type.end());
        payload.insert(payload.end(), parameter_count.begin(), parameter_count.end());
    } else {
        const std::vector<std::uint8_t> parameter_count = encode_varint(forward == 1 ? 0 : 1);
        payload.insert(payload.end(), parameter_count.begin(), parameter_count.end());
        if (forward != 1) {
            const std::vector<std::uint8_t> forward_type_delta = encode_varint(0x10);
            const std::vector<std::uint8_t> forward_value = encode_varint(forward);
            payload.insert(payload.end(), forward_type_delta.begin(), forward_type_delta.end());
            payload.insert(payload.end(), forward_value.begin(), forward_value.end());
        }
    }

    std::vector<std::uint8_t> message = encode_varint(0x1e);
    if (draft == DraftVersion::kDraft14) {
        const std::vector<std::uint8_t> length = encode_varint(payload.size());
        message.insert(message.end(), length.begin(), length.end());
    } else {
        append_be16(message, static_cast<std::uint16_t>(payload.size()));
    }
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

void queue_subscribe_requests(MockTransport& transport,
                              DraftVersion draft,
                              std::string_view track_namespace,
                              std::initializer_list<std::pair<std::uint64_t, std::string>> requests,
                              bool include_subscribe_namespace = false,
                              std::uint8_t forward = 0) {
    transport.reads[0].push_back(encode_publish_namespace_ok_message(draft, 0));
    if (include_subscribe_namespace) {
        transport.reads[0].push_back(encode_subscribe_namespace_message(draft, 1, track_namespace));
    }
    for (const auto& [request_id, track_name] : requests) {
        transport.reads[0].push_back(encode_subscribe_message(request_id, track_namespace, track_name, forward, draft));
    }
}

void queue_publish_ok_responses(MockTransport& transport,
                                DraftVersion draft,
                                std::initializer_list<std::uint64_t> request_ids,
                                std::uint8_t forward = 1) {
    transport.reads[0].push_back(encode_publish_namespace_ok_message(draft, 0));
    for (const auto request_id : request_ids) {
        transport.reads[0].push_back(encode_publish_ok_message(draft, request_id, forward));
    }
}

bool bytes_equal(const std::vector<std::uint8_t>& bytes, std::initializer_list<std::uint8_t> expected) {
    return std::vector<std::uint8_t>(expected) == bytes;
}

bool contains_subsequence(const std::vector<std::uint8_t>& bytes,
                          const std::vector<std::uint8_t>& expected) {
    for (std::size_t index = 0; index + expected.size() <= bytes.size(); ++index) {
        if (std::equal(expected.begin(), expected.end(), bytes.begin() + static_cast<std::ptrdiff_t>(index))) {
            return true;
        }
    }
    return false;
}

std::string hex_dump(const std::vector<std::uint8_t>& bytes) {
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

std::optional<std::uint64_t> message_type(const std::vector<std::uint8_t>& bytes) {
    std::size_t offset = 0;
    std::uint64_t type = 0;
    if (!decode_varint(bytes, offset, type)) {
        offset = 0;
        if (!decode_vi64(bytes, offset, type)) {
            return std::nullopt;
        }
    }
    if ((type == 0 || type > 0xff) && !bytes.empty() && (bytes[0] & 0x40) != 0) {
        offset = 0;
        decode_vi64(bytes, offset, type);
    }
    return type;
}

std::vector<std::uint8_t> encode_draft18_setup_response() {
    return encode_setup_message({
        .draft = DraftVersion::kDraft18,
        .transport = openmoq::publisher::transport::TransportKind::kWebTransport,
    });
}

bool decode_setup_fields(const std::vector<std::uint8_t>& bytes,
                         DraftVersion expected_draft,
                         openmoq::publisher::transport::TransportKind expected_transport,
                         std::string& authority,
                         std::string& path,
                         std::uint64_t& max_request_id) {
    authority.clear();
    path.clear();
    max_request_id = 0;

    std::size_t offset = 0;
    std::uint64_t message_type = 0;
    if (!decode_varint(bytes, offset, message_type) || message_type != 0x20) {
        return false;
    }
    if (offset + 2 > bytes.size()) {
        return false;
    }

    const std::size_t payload_length =
        (static_cast<std::size_t>(bytes[offset]) << 8) | static_cast<std::size_t>(bytes[offset + 1]);
    offset += 2;
    if (offset + payload_length != bytes.size()) {
        return false;
    }

    if (expected_draft == DraftVersion::kDraft14) {
        std::uint64_t version_count = 0;
        std::uint64_t version = 0;
        if (!decode_varint(bytes, offset, version_count) || version_count != 1 ||
            !decode_varint(bytes, offset, version) || version != 0xff00000eULL) {
            return false;
        }
    }

    std::uint64_t parameter_count = 0;
    const std::uint64_t expected_parameter_count =
        expected_transport == openmoq::publisher::transport::TransportKind::kRawQuic ? 3 : 1;
    if (!decode_varint(bytes, offset, parameter_count) || parameter_count != expected_parameter_count) {
        return false;
    }

    std::uint64_t previous_parameter_type = 0;
    for (std::uint64_t index = 0; index < parameter_count; ++index) {
        std::uint64_t encoded_type = 0;
        if (!decode_varint(bytes, offset, encoded_type)) {
            return false;
        }
        std::uint64_t parameter_type = encoded_type;
        if (expected_draft == DraftVersion::kDraft16) {
            parameter_type = previous_parameter_type + encoded_type;
        }
        previous_parameter_type = parameter_type;

        if ((parameter_type & 0x1ULL) == 0) {
            if (parameter_type != 0x02) {
                return false;
            }
            std::size_t parameter_offset = offset;
            if (!decode_varint(bytes, parameter_offset, max_request_id)) {
                return false;
            }
            offset = parameter_offset;
            continue;
        }

        std::uint64_t parameter_length = 0;
        if (!decode_varint(bytes, offset, parameter_length) || offset + parameter_length > bytes.size()) {
            return false;
        }

        const auto parameter_bytes = std::span<const std::uint8_t>(bytes).subspan(offset, parameter_length);
        if (parameter_type == 0x05ULL) {
            authority.assign(parameter_bytes.begin(), parameter_bytes.end());
        } else if (parameter_type == 0x01) {
            path.assign(parameter_bytes.begin(), parameter_bytes.end());
        }
        offset += parameter_length;
    }

    return offset == bytes.size();
}

bool decode_object_stream_fields(const std::vector<std::uint8_t>& bytes,
                                 std::uint64_t& stream_type,
                                 std::uint64_t& track_alias,
                                 std::uint64_t& group_id,
                                 std::uint64_t& object_id_delta,
                                 std::uint64_t& payload_length,
                                 std::vector<std::uint8_t>& payload) {
    // Matches the encoder in src/transport/moqt_control_messages.cpp: the
    // subgroup header uses SUBGROUP_ID_MODE=0 (implicit value 0) and
    // DEFAULT_PRIORITY=1 (priority byte omitted), so the on-wire field order
    // is type | track_alias | group_id | object_id_delta | payload_length |
    // payload.
    payload.clear();
    std::size_t offset = 0;
    if (!decode_varint(bytes, offset, stream_type) ||
        !decode_varint(bytes, offset, track_alias) ||
        !decode_varint(bytes, offset, group_id) ||
        !decode_varint(bytes, offset, object_id_delta) ||
        !decode_varint(bytes, offset, payload_length) ||
        offset + payload_length != bytes.size()) {
        return false;
    }

    payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());
    return true;
}

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

// Collect every value that follows a given "key":" prefix, e.g. every
// "initRef":"..." or "id":"..." value in a serialized MSF catalog.
std::vector<std::string> extract_all_values(std::string_view catalog, std::string_view key_prefix) {
    std::vector<std::string> values;
    std::size_t pos = 0;
    while (true) {
        const std::size_t found = catalog.find(key_prefix, pos);
        if (found == std::string_view::npos) {
            break;
        }
        const std::size_t value_start = found + key_prefix.size();
        const std::size_t value_end = catalog.find('"', value_start);
        if (value_end == std::string_view::npos) {
            break;
        }
        values.emplace_back(catalog.substr(value_start, value_end - value_start));
        pos = value_end + 1;
    }
    return values;
}

// True when every "initRef":"..." value in the catalog also appears as an
// "id":"..." value (i.e. an initDataList entry).
bool all_init_refs_resolve(std::string_view catalog) {
    const std::vector<std::string> init_refs = extract_all_values(catalog, "\"initRef\":\"");
    const std::vector<std::string> init_ids = extract_all_values(catalog, "\"id\":\"");
    for (const auto& ref : init_refs) {
        bool found = false;
        for (const auto& id : init_ids) {
            if (id == ref) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

PublishPlan make_span_backed_plan() {
    return {
        .draft = openmoq::publisher::draft_profile(DraftVersion::kDraft14),
        .tracks = {TrackDescription{.track_id = 0, .handler_type = "meta", .codec = "catalog", .sample_entry_type = "catalog", .track_name = "catalog"},
                   TrackDescription{.track_id = 1, .handler_type = "vide", .codec = "avc1.64000C", .sample_entry_type = "avc1", .track_name = "vide_1"}},
        .objects = {
            CmsfObject{
                .kind = CmsfObjectKind::kInitialization,
                .track_name = "catalog",
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

PublishPlan make_span_backed_plan(DraftVersion draft) {
    PublishPlan plan = make_span_backed_plan();
    plan.draft = openmoq::publisher::draft_profile(draft);
    return plan;
}

PublishPlan make_multitrack_plan(DraftVersion draft) {
    return {
        .draft = openmoq::publisher::draft_profile(draft),
        .tracks = {
            TrackDescription{.track_id = 0, .handler_type = "meta", .codec = "catalog", .sample_entry_type = "catalog", .track_name = "catalog"},
            TrackDescription{.track_id = 1, .handler_type = "vide", .codec = "avc1.64000C", .sample_entry_type = "avc1", .track_name = "vide_1"},
            TrackDescription{.track_id = 2, .handler_type = "soun", .codec = "mp4a.40.2", .sample_entry_type = "mp4a", .track_name = "soun_2"},
        },
        .objects = {
            CmsfObject{
                .kind = CmsfObjectKind::kInitialization,
                .track_name = "catalog",
                .group_id = 0,
                .object_id = 0,
                .owned_payload = {'I', 'N', 'I', 'T'},
            },
            CmsfObject{
                .kind = CmsfObjectKind::kMedia,
                .track_name = "vide_1",
                .group_id = 1,
                .object_id = 0,
                .media_time_us = 0,
                .owned_payload = {'V', '0'},
            },
            CmsfObject{
                .kind = CmsfObjectKind::kMedia,
                .track_name = "soun_2",
                .group_id = 1,
                .object_id = 0,
                .media_time_us = 0,
                .owned_payload = {'A', '0'},
            },
            CmsfObject{
                .kind = CmsfObjectKind::kMedia,
                .track_name = "vide_1",
                .group_id = 2,
                .object_id = 0,
                .media_time_us = 1000,
                .owned_payload = {'V', '1'},
            },
            CmsfObject{
                .kind = CmsfObjectKind::kMedia,
                .track_name = "soun_2",
                .group_id = 2,
                .object_id = 0,
                .media_time_us = 1000,
                .owned_payload = {'A', '1'},
            },
        },
    };
}

PublishPlan make_multi_object_subgroup_plan() {
    // A single video group (group_id = 0, subgroup_id = 0) carrying three
    // sequential objects. Used to verify that multiple objects in the same
    // subgroup are appended to a single QUIC stream per spec §2.2.
    return {
        .draft = openmoq::publisher::draft_profile(DraftVersion::kDraft14),
        .tracks = {
            TrackDescription{.track_id = 0, .handler_type = "meta", .codec = "catalog", .sample_entry_type = "catalog", .track_name = "catalog"},
            TrackDescription{.track_id = 1, .handler_type = "vide", .codec = "avc1.64000C", .sample_entry_type = "avc1", .track_name = "vide_1"},
        },
        .objects = {
            CmsfObject{
                .kind = CmsfObjectKind::kInitialization,
                .track_name = "catalog",
                .group_id = 0,
                .object_id = 0,
                .owned_payload = {'I', 'N', 'I', 'T'},
            },
            CmsfObject{
                .kind = CmsfObjectKind::kMedia,
                .track_name = "vide_1",
                .group_id = 0,
                .object_id = 0,
                .media_time_us = 0,
                .owned_payload = {'V', '0'},
            },
            CmsfObject{
                .kind = CmsfObjectKind::kMedia,
                .track_name = "vide_1",
                .group_id = 0,
                .object_id = 1,
                .media_time_us = 33333,
                .owned_payload = {'V', '1'},
            },
            CmsfObject{
                .kind = CmsfObjectKind::kMedia,
                .track_name = "vide_1",
                .group_id = 0,
                .object_id = 2,
                .media_time_us = 66666,
                .owned_payload = {'V', '2'},
            },
        },
    };
}

}  // namespace

int main() {
    bool ok = true;
    constexpr std::string_view kTestTrackNamespace = "interop";
    constexpr std::uint64_t kExpectedClientMaxRequestId = 100;
    const EndpointConfig endpoint{
        .host = "example.com",
        .port = 4433,
        .alpn = "moq-00",
    };
    const TlsConfig tls{
        .certificate_path = {},
        .private_key_path = {},
        .ca_path = {},
        .insecure_skip_verify = true,
    };
    const std::vector<std::uint8_t> source_bytes = {'I', 'N', 'I', 'T', 'M', 'S', 'G'};
    auto status = TransportStatus::success();
    std::string authority;
    std::string path;
    std::uint64_t max_request_id = 1;

    {
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        queue_subscribe_requests(transport, DraftVersion::kDraft14, kTestTrackNamespace, {{1, "catalog"}, {3, "vide_1"}});
        MoqtSession session(transport, std::string(kTestTrackNamespace), false);

        auto status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected session connect to succeed");

        const PublishPlan materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes);

        status = session.publish(materialized);
        ok &= expect(status.ok, "expected publish to succeed with relay subscribe flow");
        ok &= expect(transport.writes.size() == 9,
                     "expected setup, namespace, two subscribe_ok, two object streams, two publish_done, namespace_done");
        ok &= expect(!transport.writes[0].bytes.empty() && transport.writes[0].bytes.front() == 0x20,
                     "expected binary CLIENT_SETUP message type");
        authority.clear();
        path.clear();
        max_request_id = 1;
        ok &= expect(
            decode_setup_fields(transport.writes[0].bytes,
                                DraftVersion::kDraft14,
                                openmoq::publisher::transport::TransportKind::kRawQuic,
                                authority,
                                path,
                                max_request_id),
                     "expected draft-14 CLIENT_SETUP to decode");
        ok &= expect(authority == "example.com:4433", "expected draft-14 CLIENT_SETUP authority");
        ok &= expect(path == "/", "expected draft-14 CLIENT_SETUP path");
        ok &= expect(max_request_id == kExpectedClientMaxRequestId, "expected draft-14 CLIENT_SETUP max_request_id");
        ok &= expect(message_type(transport.writes[1].bytes) == 0x06, "expected PUBLISH_NAMESPACE");
        ok &= expect(transport.writes[1].bytes == std::vector<std::uint8_t>({0x06, 0x00, 0x0b, 0x00, 0x01, 0x07, 0x69, 0x6e,
                                                                             0x74, 0x65, 0x72, 0x6f, 0x70, 0x00}),
                     "expected namespace write to use the configured track namespace");
        ok &= expect(message_type(transport.writes[2].bytes) == 0x04, "expected first SUBSCRIBE_OK");
        ok &= expect(message_type(transport.writes[3].bytes) == 0x04, "expected second SUBSCRIBE_OK");
        const auto object_indices = object_write_indices(transport);
        ok &= expect(object_indices.size() == 2, "expected two object stream writes");
        ok &= expect(!transport.writes[object_indices[0]].bytes.empty() &&
                         transport.writes[object_indices[0]].bytes.front() == 0x18,
                     "expected first draft-14 object stream to use draft-14 subgroup header with end-of-group");
        std::uint64_t stream_type = 0;
        std::uint64_t track_alias = 0;
        std::uint64_t group_id = 0;
        std::uint64_t object_id_delta = 0;
        std::uint64_t payload_length = 0;
        std::vector<std::uint8_t> payload;
        ok &= expect(decode_object_stream_fields(transport.writes[object_indices[0]].bytes,
                                                 stream_type,
                                                 track_alias,
                                                 group_id,
                                                 object_id_delta,
                                                 payload_length,
                                                 payload),
                     "expected first object stream to decode");
        ok &= expect(stream_type == 0x18, "expected draft-14 subgroup stream type with end-of-group");
        ok &= expect(group_id == 0, "expected catalog group id");
        ok &= expect(object_id_delta == 0, "expected catalog object id delta before payload length");
        ok &= expect(payload_length == 4, "expected catalog payload length to be encoded after object id delta");
        ok &= expect(payload == std::vector<std::uint8_t>({'I', 'N', 'I', 'T'}),
                     "expected catalog payload bytes after subgroup object fields");
        ok &= expect(transport.writes[object_indices[0]].fin, "expected first object stream write to set FIN");
        ok &= expect(!transport.writes[object_indices[1]].bytes.empty() &&
                         transport.writes[object_indices[1]].bytes.front() == 0x18,
                     "expected second draft-14 object stream to use draft-14 subgroup header with end-of-group");
        payload.clear();
        ok &= expect(decode_object_stream_fields(transport.writes[object_indices[1]].bytes,
                                                 stream_type,
                                                 track_alias,
                                                 group_id,
                                                 object_id_delta,
                                                 payload_length,
                                                 payload),
                     "expected second object stream to decode");
        ok &= expect(group_id == 1, "expected media group id");
        ok &= expect(object_id_delta == 0, "expected media object id delta before payload length");
        ok &= expect(payload_length == 3, "expected media payload length to be encoded after object id delta");
        ok &= expect(payload == std::vector<std::uint8_t>({'M', 'S', 'G'}),
                     "expected media payload bytes after subgroup object fields");
        ok &= expect(transport.writes[object_indices[1]].fin, "expected second object stream write to set FIN");
        ok &= expect(control_message_count(transport, 0x0b) == 2, "expected two PUBLISH_DONE messages");
        ok &= expect(message_type(transport.writes[8].bytes) == 0x09, "expected PUBLISH_NAMESPACE_DONE");
        ok &= expect(transport.writes[8].bytes == std::vector<std::uint8_t>({0x09, 0x00, 0x09, 0x01, 0x07, 0x69, 0x6e,
                                                                             0x74, 0x65, 0x72, 0x6f, 0x70}),
                     "expected draft-14 PUBLISH_NAMESPACE_DONE to contain the configured track namespace");
        ok &= expect(std::find(transport.read_timeouts.begin(), transport.read_timeouts.end(), std::chrono::seconds(30)) !=
                         transport.read_timeouts.end(),
                     "expected default subscriber wait timeout to be 30 seconds");
    }

    {
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        transport.reads[0].push_back(encode_subscribe_message(1, kTestTrackNamespace, "vide_1", 1));

        bool injected_unsubscribe = false;
        transport.on_read = [&](MockTransport& current, std::uint64_t stream_id) {
            if (stream_id != 0 || injected_unsubscribe) {
                return;
            }
            std::size_t object_payload_writes = 0;
            for (const auto& write : current.writes) {
                if (write.stream_id != 0 && !write.bytes.empty()) {
                    ++object_payload_writes;
                }
            }
            if (object_payload_writes == 1) {
                current.reads[0].push_back(encode_unsubscribe_message(DraftVersion::kDraft14, 1));
                injected_unsubscribe = true;
            }
        };

        MoqtSession session(transport, std::string(kTestTrackNamespace), false);
        auto status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected unsubscribe session connect to succeed");

        status = session.publish(make_multi_object_subgroup_plan());
        ok &= expect(status.ok, "expected UNSUBSCRIBE flow to finish cleanly");
        ok &= expect(injected_unsubscribe, "expected test to inject UNSUBSCRIBE after first object");
        std::size_t object_payload_writes = 0;
        bool saw_stream_fin = false;
        for (const auto& write : transport.writes) {
            if (write.stream_id == 0) {
                continue;
            }
            if (write.bytes.empty() && write.fin) {
                saw_stream_fin = true;
            } else if (!write.bytes.empty()) {
                ++object_payload_writes;
            }
        }
        ok &= expect(object_payload_writes == 1, "expected UNSUBSCRIBE to stop further object writes");
        ok &= expect(saw_stream_fin, "expected UNSUBSCRIBE to FIN the open data stream");
        ok &= expect(control_message_count(transport, 0x0b) == 1,
                     "expected UNSUBSCRIBE to emit PUBLISH_DONE for active subscription");
    }

    {
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        transport.reads[0].push_back(encode_subscribe_message(1, kTestTrackNamespace, "catalog", 0));
        transport.reads[0].push_back(encode_subscribe_message(3, kTestTrackNamespace, "vide_1", 0));

        bool saw_media_before_media_subscribe = false;
        transport.on_read = [&](const MockTransport& current, std::uint64_t stream_id) {
            if (stream_id != 0 || current.read_count != 4) {
                return;
            }

            for (const auto& write : current.writes) {
                if (write.stream_id != 6) {
                    continue;
                }
                std::uint64_t stream_type = 0;
                std::uint64_t track_alias = 0;
                std::uint64_t group_id = 0;
                std::uint64_t object_id_delta = 0;
                std::uint64_t payload_length = 0;
                std::vector<std::uint8_t> payload;
                if (decode_object_stream_fields(write.bytes,
                                                stream_type,
                                                track_alias,
                                                group_id,
                                                object_id_delta,
                                                payload_length,
                                                payload) &&
                    track_alias == 1) {
                    saw_media_before_media_subscribe = true;
                }
            }
        };

        MoqtSession session(transport, std::string(kTestTrackNamespace), false);

        auto status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected delayed-subscriber session connect to succeed");

        const PublishPlan materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes);
        status = session.publish(materialized);
        ok &= expect(status.ok, "expected publish to succeed with delayed media subscriber");
        ok &= expect(!saw_media_before_media_subscribe,
                     "expected forward=0 to avoid sending media before the media subscriber arrives");
    }

    {
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 12,
        }));
        transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        transport.reads[0].push_back(encode_subscribe_message(1, kTestTrackNamespace, "vide_1", 0));
        transport.reads[0].push_back(encode_subscribe_message(3, kTestTrackNamespace, "soun_2", 0));
        MoqtSession session(transport, std::string(kTestTrackNamespace), false);

        auto status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected multitrack subscribe session connect to succeed");

        status = session.publish(make_multitrack_plan(DraftVersion::kDraft14));
        ok &= expect(status.ok, "expected publish to interleave multitrack subscribed objects");

        std::vector<std::vector<std::uint8_t>> served_payloads;
        for (const auto& write : transport.writes) {
            if (write.stream_id == 0) {
                continue;
            }
            std::uint64_t stream_type = 0;
            std::uint64_t track_alias = 0;
            std::uint64_t group_id = 0;
            std::uint64_t object_id_delta = 0;
            std::uint64_t payload_length = 0;
            std::vector<std::uint8_t> payload;
            if (!decode_object_stream_fields(write.bytes,
                                             stream_type,
                                             track_alias,
                                             group_id,
                                             object_id_delta,
                                             payload_length,
                                             payload)) {
                continue;
            }
            served_payloads.push_back(payload);
        }
        ok &= expect(served_payloads == std::vector<std::vector<std::uint8_t>>({{'V', '0'}, {'A', '0'}, {'V', '1'}, {'A', '1'}}),
                     "expected subscribed multitrack payloads to alternate by media order");
    }

    {
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 12,
        }));
        transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        transport.reads[0].push_back(encode_subscribe_message(1, kTestTrackNamespace, "vide_1", 0));

        bool queued_delayed_audio_subscribe = false;
        transport.on_read = [&](const MockTransport& current, std::uint64_t stream_id) {
            if (queued_delayed_audio_subscribe || stream_id != 0 || current.read_count != 4) {
                return;
            }
            transport.reads[0].push_back(encode_subscribe_message(3, kTestTrackNamespace, "soun_2", 0));
            queued_delayed_audio_subscribe = true;
        };

        MoqtSession session(transport, std::string(kTestTrackNamespace), false);

        auto status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected delayed multitrack subscribe session connect to succeed");

        status = session.publish(make_multitrack_plan(DraftVersion::kDraft14));
        ok &= expect(status.ok, "expected delayed second multitrack subscribe to succeed");

        std::vector<std::vector<std::uint8_t>> served_payloads;
        for (const auto& write : transport.writes) {
            if (write.stream_id == 0) {
                continue;
            }
            std::uint64_t stream_type = 0;
            std::uint64_t track_alias = 0;
            std::uint64_t group_id = 0;
            std::uint64_t object_id_delta = 0;
            std::uint64_t payload_length = 0;
            std::vector<std::uint8_t> payload;
            if (!decode_object_stream_fields(write.bytes,
                                             stream_type,
                                             track_alias,
                                             group_id,
                                             object_id_delta,
                                             payload_length,
                                             payload)) {
                continue;
            }
            served_payloads.push_back(payload);
        }
        ok &= expect(served_payloads == std::vector<std::vector<std::uint8_t>>({{'V', '0'}, {'A', '0'}, {'V', '1'}, {'A', '1'}}),
                     "expected delayed second multitrack subscribe to join future interleaved media order");
    }

    {
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        queue_publish_ok_responses(transport, DraftVersion::kDraft14, {2, 4});
        MoqtSession session(transport, std::string(kTestTrackNamespace), true);

        auto status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected auto-forward session connect to succeed");

        const PublishPlan materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes);
        status = session.publish(materialized);
        ok &= expect(status.ok, "expected publish to succeed with auto-forward flow");
        ok &= expect(transport.writes.size() == 9,
                     "expected setup, namespace, two publish requests, two object streams, two publish_done, namespace_done");
        if (transport.writes.size() >= 9) {
            ok &= expect(message_type(transport.writes[1].bytes) == 0x06,
                         "expected PUBLISH_NAMESPACE before auto-forward track publish");
            ok &= expect(message_type(transport.writes[2].bytes) == 0x1d, "expected first PUBLISH");
            ok &= expect(message_type(transport.writes[3].bytes) == 0x1d, "expected second PUBLISH");
            ok &= expect(transport.writes[4].stream_id == 2, "expected first auto-forward object stream on stream 2");
            ok &= expect(transport.writes[5].stream_id == 6, "expected second auto-forward object stream on stream 6");
            ok &= expect(message_type(transport.writes[6].bytes) == 0x0b, "expected first auto-forward PUBLISH_DONE");
            ok &= expect(message_type(transport.writes[7].bytes) == 0x0b, "expected second auto-forward PUBLISH_DONE");
            ok &= expect(message_type(transport.writes[8].bytes) == 0x09,
                         "expected auto-forward PUBLISH_NAMESPACE_DONE");
        }
    }

    {
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        transport.reads[0].push_back(encode_publish_ok_message(DraftVersion::kDraft14, 2, 1));
        transport.reads[0].push_back(encode_subscribe_message(3, kTestTrackNamespace, "vide_1", 0));
        MoqtSession session(transport, std::string(kTestTrackNamespace), false, true);

        auto status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected publish-catalog session connect to succeed");

        const PublishPlan materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes);
        status = session.publish(materialized);
        ok &= expect(status.ok, "expected publish to succeed with proactive catalog publish");
        ok &= expect(transport.writes.size() == 9,
                     "expected setup, namespace, catalog publish, catalog object, catalog publish_done, media subscribe_ok, media object, media publish_done, namespace_done");
        if (transport.writes.size() >= 9) {
            ok &= expect(message_type(transport.writes[2].bytes) == 0x1d,
                         "expected catalog PUBLISH before subscriber-driven media flow");
            ok &= expect(transport.writes[3].stream_id == 2,
                         "expected proactive catalog object stream on stream 2");
            ok &= expect(message_type(transport.writes[4].bytes) == 0x0b,
                         "expected proactive catalog PUBLISH_DONE");
            ok &= expect(message_type(transport.writes[5].bytes) == 0x04,
                         "expected subscriber-driven media SUBSCRIBE_OK after proactive catalog publish");
            ok &= expect(transport.writes[6].stream_id == 6,
                         "expected subscriber-driven media object stream on stream 6");
            ok &= expect(message_type(transport.writes[7].bytes) == 0x0b,
                         "expected subscriber-driven media PUBLISH_DONE");
            ok &= expect(message_type(transport.writes[8].bytes) == 0x09,
                         "expected namespace done after proactive catalog publish and media subscribe");
        }
    }

    {
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        transport.reads[0].push_back(encode_publish_ok_message(DraftVersion::kDraft14, 2, 1));
        transport.reads[0].push_back(encode_subscribe_message(3, kTestTrackNamespace, "vide_1", 0));
        MoqtSession session(transport, std::string(kTestTrackNamespace), false, true);

        auto status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected catalog-content session connect to succeed");

        PublishPlan catalog_content_plan = make_span_backed_plan(DraftVersion::kDraft14);
        const std::string catalog_text =
            "{\"version\":\"1\",\"generatedAt\":1751000000000,\"tracks\":[{\"name\":\"vide_1\",\"packaging\":\"cmaf\","
            "\"role\":\"video\",\"isLive\":true,\"initRef\":\"vide_1-init\",\"bitrate\":2000000}],"
            "\"initDataList\":[{\"id\":\"vide_1-init\",\"type\":\"inline\",\"data\":\"AAAA\"}]}";
        catalog_content_plan.objects[0].payload = ByteSpan{.offset = 0, .size = catalog_text.size()};
        catalog_content_plan.objects[1].payload = ByteSpan{.offset = catalog_text.size(), .size = 3};
        std::vector<std::uint8_t> catalog_source_bytes(catalog_text.begin(), catalog_text.end());
        catalog_source_bytes.insert(catalog_source_bytes.end(), {'V', 'I', 'D'});
        const PublishPlan materialized = materialize_publish_plan(catalog_content_plan, catalog_source_bytes);

        status = session.publish(materialized);
        ok &= expect(status.ok, "expected publish to succeed with catalog content validation");
        ok &= expect(transport.writes.size() >= 4,
                     "expected catalog-content flow to emit catalog object stream");
        if (transport.writes.size() >= 4) {
            std::uint64_t stream_type = 0;
            std::uint64_t track_alias = 0;
            std::uint64_t group_id = 0;
            std::uint64_t object_id_delta = 0;
            std::uint64_t payload_length = 0;
            std::vector<std::uint8_t> payload;
            ok &= expect(
                decode_object_stream_fields(transport.writes[3].bytes,
                                            stream_type,
                                            track_alias,
                                            group_id,
                                            object_id_delta,
                                            payload_length,
                                            payload),
                "expected catalog-content object stream fields to decode");
            const std::string served_catalog(payload.begin(), payload.end());
            ok &= expect(served_catalog.find("\"version\":\"1\"") != std::string::npos,
                         "expected served catalog payload to include MSF v1 string version");
            ok &= expect(served_catalog.find("\"name\":\"vide_1\"") != std::string::npos,
                         "expected served catalog payload to include vide_1 track");
            ok &= expect(served_catalog.find("\"isLive\":true") != std::string::npos,
                         "expected served catalog payload to mark the track isLive");
            const std::string generated_at_prefix = "\"generatedAt\":";
            const std::size_t generated_at_pos = served_catalog.find(generated_at_prefix);
            ok &= expect(generated_at_pos != std::string::npos,
                         "expected served live catalog payload to include generatedAt");
            if (generated_at_pos != std::string::npos) {
                const std::size_t value_start = generated_at_pos + generated_at_prefix.size();
                const std::size_t value_end = served_catalog.find_first_not_of("0123456789", value_start);
                const std::string generated_at_value =
                    served_catalog.substr(value_start, value_end - value_start);
                ok &= expect(generated_at_value.size() == 13,
                             "expected generatedAt to be a plausible epoch-milliseconds value");
                ok &= expect(!generated_at_value.empty() && std::stoull(generated_at_value) != 0,
                             "expected generatedAt to not be zero");
            }
            ok &= expect(served_catalog.find("\"trackDuration\"") == std::string::npos,
                         "expected no trackDuration in a live catalog");
            ok &= expect(all_init_refs_resolve(served_catalog),
                         "expected every initRef in the served live catalog to resolve to an initDataList id");
            ok &= expect(served_catalog.find("\"data\":\"AAAA\"") != std::string::npos,
                         "expected served catalog payload to include initDataList data");
        }
    }

    {
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        transport.reads[0].push_back(encode_publish_ok_message(DraftVersion::kDraft14, 2, 0));
        std::vector<std::uint8_t> control = encode_subscribe_update_message(1, 0);
        const auto media_subscribe = encode_subscribe_message(3, kTestTrackNamespace, "vide_1", 0);
        control.insert(control.end(), media_subscribe.begin(), media_subscribe.end());
        transport.reads[0].push_back(control);
        MoqtSession session(transport, std::string(kTestTrackNamespace), false, true);

        auto status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected publish-catalog alias-update session connect to succeed");

        const PublishPlan materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes);
        status = session.publish(materialized);
        ok &= expect(status.ok, "expected publish to succeed with alias-based catalog SUBSCRIBE_UPDATE activation");
        ok &= expect(transport.writes.size() == 9,
                     "expected setup, namespace, catalog publish, catalog object, catalog publish_done, media subscribe_ok, media object, media publish_done, namespace_done");
        if (transport.writes.size() >= 9) {
            ok &= expect(message_type(transport.writes[2].bytes) == 0x1d,
                         "expected catalog PUBLISH before alias-based update activation");
            ok &= expect(message_type(transport.writes[3].bytes) == 0x04,
                         "expected media SUBSCRIBE_OK while catalog activation is pending");
            ok &= expect(object_write_indices(transport).size() == 2,
                         "expected catalog and media object stream writes after alias-based activation");
            ok &= expect(control_message_count(transport, 0x0b) == 2,
                         "expected catalog and media PUBLISH_DONE after alias-based activation");
            ok &= expect(message_type(transport.writes[8].bytes) == 0x09,
                         "expected namespace done after alias-based catalog activation");
        }
    }

    {
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        std::vector<std::uint8_t> interleaved_control = encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0);
        const auto catalog_publish_ok = encode_publish_ok_message(DraftVersion::kDraft14, 2, 0);
        interleaved_control.insert(interleaved_control.end(), catalog_publish_ok.begin(), catalog_publish_ok.end());
        const auto catalog_subscribe = encode_subscribe_message(5, kTestTrackNamespace, "catalog", 0);
        interleaved_control.insert(interleaved_control.end(), catalog_subscribe.begin(), catalog_subscribe.end());
        const auto catalog_subscribe_update = encode_legacy_subscribe_update_message(0);
        interleaved_control.insert(interleaved_control.end(),
                                   catalog_subscribe_update.begin(),
                                   catalog_subscribe_update.end());
        const auto media_publish_ok = encode_publish_ok_message(DraftVersion::kDraft14, 4, 1);
        interleaved_control.insert(interleaved_control.end(), media_publish_ok.begin(), media_publish_ok.end());
        transport.reads[0].push_back(interleaved_control);
        MoqtSession session(transport, std::string(kTestTrackNamespace), true);

        auto status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected legacy update session connect to succeed");

        const PublishPlan materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes);
        status = session.publish(materialized);
        ok &= expect(status.ok, "expected publish to succeed with legacy alias-based SUBSCRIBE_UPDATE");
    }

    {
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        std::vector<std::uint8_t> interleaved_control = encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0);
        const auto catalog_publish_ok = encode_publish_ok_message(DraftVersion::kDraft14, 2, 0);
        interleaved_control.insert(interleaved_control.end(), catalog_publish_ok.begin(), catalog_publish_ok.end());
        const auto catalog_subscribe = encode_subscribe_message(5, kTestTrackNamespace, "catalog", 0);
        interleaved_control.insert(interleaved_control.end(), catalog_subscribe.begin(), catalog_subscribe.end());
        const auto catalog_subscribe_update = encode_subscribe_update_message(7, 5);
        interleaved_control.insert(interleaved_control.end(),
                                   catalog_subscribe_update.begin(),
                                   catalog_subscribe_update.end());
        const auto media_publish_ok = encode_publish_ok_message(DraftVersion::kDraft14, 4, 1);
        interleaved_control.insert(interleaved_control.end(), media_publish_ok.begin(), media_publish_ok.end());
        transport.reads[0].push_back(interleaved_control);
        MoqtSession session(transport, std::string(kTestTrackNamespace), true);

        auto status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected interleaved control session connect to succeed");

        const PublishPlan materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes);
        status = session.publish(materialized);
        ok &= expect(status.ok, "expected publish to succeed with interleaved SUBSCRIBE and SUBSCRIBE_UPDATE");
        ok &= expect(transport.writes.size() == 10,
                     "expected setup, namespace, two publish requests, media object, media publish_done, subscribe_ok, catalog object, catalog publish_done, namespace_done");
        if (transport.writes.size() >= 10) {
            ok &= expect(message_type(transport.writes[2].bytes) == 0x1d,
                         "expected first interleaved-flow PUBLISH");
            ok &= expect(message_type(transport.writes[3].bytes) == 0x1d,
                         "expected second interleaved-flow PUBLISH");
            ok &= expect(transport.writes[4].stream_id == 2,
                         "expected forwarded media object stream before deferred subscribe handling");
            ok &= expect(message_type(transport.writes[5].bytes) == 0x0b,
                         "expected forwarded media PUBLISH_DONE before deferred subscribe handling");
            ok &= expect(message_type(transport.writes[6].bytes) == 0x04,
                         "expected deferred catalog SUBSCRIBE_OK after publish acknowledgements");
            ok &= expect(transport.writes[7].stream_id == 6,
                         "expected deferred catalog object stream after SUBSCRIBE_OK");
            ok &= expect(message_type(transport.writes[8].bytes) == 0x0b,
                         "expected deferred catalog PUBLISH_DONE");
            ok &= expect(message_type(transport.writes[9].bytes) == 0x09,
                         "expected namespace done after deferred subscribe handling");
        }
    }

    {
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        queue_publish_ok_responses(transport, DraftVersion::kDraft14, {2, 4}, 0);
        transport.reads[0].push_back(encode_subscribe_message(5, kTestTrackNamespace, "catalog", 0));
        transport.reads[0].push_back(encode_subscribe_message(7, kTestTrackNamespace, "vide_1", 0));
        MoqtSession session(transport, std::string(kTestTrackNamespace), true);

        auto status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected downgraded auto-forward session connect to succeed");

        const PublishPlan materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes);
        status = session.publish(materialized);
        ok &= expect(status.ok, "expected publish to succeed after PUBLISH_OK forward downgrade");
        ok &= expect(transport.writes.size() == 11,
                     "expected setup, namespace, two publish requests, two subscribe_ok, two object streams, two publish_done, namespace_done");
        if (transport.writes.size() >= 11) {
            ok &= expect(message_type(transport.writes[2].bytes) == 0x1d, "expected first downgraded PUBLISH");
            ok &= expect(message_type(transport.writes[3].bytes) == 0x1d, "expected second downgraded PUBLISH");
            ok &= expect(message_type(transport.writes[4].bytes) == 0x04,
                         "expected downgraded catalog SUBSCRIBE_OK");
            ok &= expect(message_type(transport.writes[5].bytes) == 0x04,
                         "expected downgraded media SUBSCRIBE_OK");
            ok &= expect(object_write_indices(transport).size() == 2,
                         "expected downgraded catalog and media object stream writes");
            ok &= expect(control_message_count(transport, 0x0b) == 2,
                         "expected downgraded catalog and media PUBLISH_DONE");
            ok &= expect(message_type(transport.writes[10].bytes) == 0x09,
                         "expected downgraded auto-forward PUBLISH_NAMESPACE_DONE");
        }
    }

    MockTransport failing_transport;
    MoqtSession failing_session(failing_transport, std::string(kTestTrackNamespace));
    status = failing_session.connect(endpoint, tls);
    ok &= expect(status.ok, "expected second session connect to succeed");

    status = failing_session.publish(make_span_backed_plan(DraftVersion::kDraft14));
    ok &= expect(!status.ok, "expected span-backed publish to fail before full transport integration");

    MockTransport draft16_transport;
    draft16_transport.reads[0].push_back(encode_server_setup_message({
        .draft = DraftVersion::kDraft16,
        .max_request_id = 8,
    }));
    queue_subscribe_requests(draft16_transport, DraftVersion::kDraft16, kTestTrackNamespace, {{1, "catalog"}, {3, "vide_1"}});
    MoqtSession draft16_session(draft16_transport, std::string(kTestTrackNamespace));
    status = draft16_session.connect(endpoint, tls);
    ok &= expect(status.ok, "expected draft-16 session connect to succeed");

    const PublishPlan draft16_materialized =
        materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft16), source_bytes);
    status = draft16_session.publish(draft16_materialized);
    ok &= expect(status.ok, "expected draft-16 publish to succeed");
    ok &= expect(draft16_transport.writes.size() == 9, "expected draft-16 relay subscribe control/object sequence");
    ok &= expect(!draft16_transport.writes[0].bytes.empty() && draft16_transport.writes[0].bytes.front() == 0x20,
                 "expected draft-16 binary CLIENT_SETUP message type");
    authority.clear();
    path.clear();
    max_request_id = 1;
    ok &= expect(
        decode_setup_fields(draft16_transport.writes[0].bytes,
                            DraftVersion::kDraft16,
                            openmoq::publisher::transport::TransportKind::kRawQuic,
                            authority,
                            path,
                            max_request_id),
        "expected draft-16 CLIENT_SETUP to decode");
    ok &= expect(authority == "example.com:4433", "expected draft-16 CLIENT_SETUP authority");
    ok &= expect(path == "/", "expected draft-16 CLIENT_SETUP path");
    ok &= expect(max_request_id == kExpectedClientMaxRequestId, "expected draft-16 CLIENT_SETUP max_request_id");

    {
        const std::vector<std::uint8_t> raw_cwt{0xa1, 0x18, 0x64, 0x81, 0x83};
        const auto auth_token = openmoq::publisher::cat4moq::wrap_cat_token(raw_cwt);
        openmoq::publisher::cat4moq::AuthorizationConfig auth_config;
        auth_config.setup_token = auth_token;
        auth_config.action_token = auth_token;

        MockTransport auth_transport;
        auth_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        queue_subscribe_requests(auth_transport, DraftVersion::kDraft16, kTestTrackNamespace, {{1, "catalog"}, {3, "vide_1"}});
        MoqtSession auth_session(auth_transport,
                                 std::string(kTestTrackNamespace),
                                 false,
                                 false,
                                 false,
                                 false,
                                 std::chrono::seconds(30),
                                 auth_config);
        status = auth_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected auth session connect to succeed");
        status = auth_session.publish(draft16_materialized);
        ok &= expect(status.ok, "expected auth session publish to succeed");
        ok &= expect(auth_transport.writes.size() >= 2,
                     "expected auth session to write setup and namespace messages");
        if (auth_transport.writes.size() >= 2) {
            ok &= expect(contains_subsequence(auth_transport.writes[0].bytes, auth_token.bytes),
                         "expected setup message to include configured CAT token");
            ok &= expect(contains_subsequence(auth_transport.writes[1].bytes, auth_token.bytes),
                         "expected namespace publish to include configured CAT token");
        }

        MockTransport auth_forward_transport;
        auth_forward_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        queue_publish_ok_responses(auth_forward_transport, DraftVersion::kDraft16, {2, 4});
        MoqtSession auth_forward_session(auth_forward_transport,
                                         std::string(kTestTrackNamespace),
                                         true,
                                         false,
                                         false,
                                         false,
                                         std::chrono::seconds(30),
                                         auth_config);
        status = auth_forward_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected auth forward session connect to succeed");
        status = auth_forward_session.publish(draft16_materialized);
        ok &= expect(status.ok, "expected auth forward session publish to succeed");
        ok &= expect(auth_forward_transport.writes.size() >= 3,
                     "expected auth forward session to write setup, namespace, and publish messages");
        if (auth_forward_transport.writes.size() >= 3) {
            ok &= expect(contains_subsequence(auth_forward_transport.writes[2].bytes, auth_token.bytes),
                         "expected publish track request to include configured CAT token");
        }
    }

    const auto draft14_wt_setup = encode_setup_message({
        .draft = DraftVersion::kDraft14,
        .transport = openmoq::publisher::transport::TransportKind::kWebTransport,
        .authority = "example.com:4433",
        .path = "/moq",
        .max_request_id = kExpectedClientMaxRequestId,
    });
    authority.clear();
    path.clear();
    max_request_id = 1;
    ok &= expect(decode_setup_fields(draft14_wt_setup,
                                     DraftVersion::kDraft14,
                                     openmoq::publisher::transport::TransportKind::kWebTransport,
                                     authority,
                                     path,
                                     max_request_id),
                 "expected draft-14 WebTransport CLIENT_SETUP to decode");
    ok &= expect(authority.empty(), "expected draft-14 WebTransport CLIENT_SETUP to omit authority");
    ok &= expect(path.empty(), "expected draft-14 WebTransport CLIENT_SETUP to omit path");
    ok &= expect(max_request_id == kExpectedClientMaxRequestId,
                 "expected draft-14 WebTransport CLIENT_SETUP max_request_id");

    const auto draft16_wt_setup = encode_setup_message({
        .draft = DraftVersion::kDraft16,
        .transport = openmoq::publisher::transport::TransportKind::kWebTransport,
        .authority = "example.com:4433",
        .path = "/moq",
        .max_request_id = kExpectedClientMaxRequestId,
    });
    authority.clear();
    path.clear();
    max_request_id = 1;
    ok &= expect(decode_setup_fields(draft16_wt_setup,
                                     DraftVersion::kDraft16,
                                     openmoq::publisher::transport::TransportKind::kWebTransport,
                                     authority,
                                     path,
                                     max_request_id),
                 "expected draft-16 WebTransport CLIENT_SETUP to decode");
    ok &= expect(authority.empty(), "expected draft-16 WebTransport CLIENT_SETUP to omit authority");
    ok &= expect(path.empty(), "expected draft-16 WebTransport CLIENT_SETUP to omit path");
    ok &= expect(max_request_id == kExpectedClientMaxRequestId,
                 "expected draft-16 WebTransport CLIENT_SETUP max_request_id");
    ok &= expect(message_type(draft16_transport.writes[1].bytes) == 0x06, "expected draft-16 PUBLISH_NAMESPACE");
    ok &= expect(draft16_transport.writes[1].bytes == std::vector<std::uint8_t>({0x06, 0x00, 0x0b, 0x00, 0x01, 0x07,
                                                                                  0x69, 0x6e, 0x74, 0x65, 0x72, 0x6f,
                                                                                  0x70, 0x00}),
                 "expected draft-16 namespace write to use the configured track namespace");
    ok &= expect(message_type(draft16_transport.writes[2].bytes) == 0x04, "expected first draft-16 SUBSCRIBE_OK");
    ok &= expect(message_type(draft16_transport.writes[3].bytes) == 0x04, "expected second draft-16 SUBSCRIBE_OK");
    ok &= expect(object_write_indices(draft16_transport).size() == 2, "expected two draft-16 object stream writes");
    ok &= expect(control_message_count(draft16_transport, 0x0b) == 2,
                 "expected two draft-16 PUBLISH_DONE messages");
    ok &= expect(message_type(draft16_transport.writes[8].bytes) == 0x09,
                 "expected draft-16 PUBLISH_NAMESPACE_DONE");
    ok &= expect(draft16_transport.writes[8].bytes == std::vector<std::uint8_t>({0x09, 0x00, 0x01, 0x00}),
                 "expected draft-16 PUBLISH_NAMESPACE_DONE to contain only the request ID");

    {
        MockTransport draft16_publish_reject_transport;
        draft16_publish_reject_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        draft16_publish_reject_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));
        draft16_publish_reject_transport.reads[0].push_back(
            openmoq::publisher::transport::encode_request_error_message(
                DraftVersion::kDraft16, 2, 0x2, 0, "publish rejected"));
        MoqtSession draft16_publish_reject_session(
            draft16_publish_reject_transport, std::string(kTestTrackNamespace), true);
        status = draft16_publish_reject_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-16 publish-reject session connect to succeed");
        status = draft16_publish_reject_session.publish(draft16_materialized);
        ok &= expect(!status.ok, "expected draft-16 publish to fail on REQUEST_ERROR publish rejection");
        ok &= expect(status.message != "timed out waiting for publish acknowledgements",
                     "expected draft-16 publish REQUEST_ERROR to fail directly, not via ack timeout");
    }

    {
        MockTransport draft14_duplicate_publish_ok_id_transport;
        draft14_duplicate_publish_ok_id_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        draft14_duplicate_publish_ok_id_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        draft14_duplicate_publish_ok_id_transport.reads[0].push_back(
            encode_publish_ok_message(DraftVersion::kDraft14, 2, 1));
        draft14_duplicate_publish_ok_id_transport.reads[0].push_back(
            encode_publish_ok_message(DraftVersion::kDraft14, 2, 1));
        MoqtSession draft14_duplicate_publish_ok_id_session(
            draft14_duplicate_publish_ok_id_transport, std::string(kTestTrackNamespace), true);
        status = draft14_duplicate_publish_ok_id_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-14 duplicate-publish-ok-id session connect to succeed");
        status = draft14_duplicate_publish_ok_id_session.publish(
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes));
        ok &= expect(!status.ok, "expected draft-14 publish to fail on duplicate PUBLISH_OK request_id");
        ok &= expect(status.message == "received duplicate publish response request_id",
                     "expected strict duplicate publish response failure for draft-14");
    }

    {
        MockTransport draft14_duplicate_publish_error_id_transport;
        draft14_duplicate_publish_error_id_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        draft14_duplicate_publish_error_id_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        // First response succeeds for request_id=2.
        draft14_duplicate_publish_error_id_transport.reads[0].push_back(
            encode_publish_ok_message(DraftVersion::kDraft14, 2, 1));
        // Then a duplicate response for same request_id=2 appears as PUBLISH_ERROR.
        std::vector<std::uint8_t> duplicate_publish_error_payload = encode_varint(2);
        const auto error_code = encode_varint(0x2);
        const auto reason = encode_varint(5);
        duplicate_publish_error_payload.insert(duplicate_publish_error_payload.end(), error_code.begin(), error_code.end());
        duplicate_publish_error_payload.insert(duplicate_publish_error_payload.end(), reason.begin(), reason.end());
        duplicate_publish_error_payload.insert(duplicate_publish_error_payload.end(), {'d', 'u', 'p', 'e', '2'});
        std::vector<std::uint8_t> duplicate_publish_error = encode_varint(0x1f);
        const std::vector<std::uint8_t> duplicate_publish_error_length =
            encode_varint(duplicate_publish_error_payload.size());
        duplicate_publish_error.insert(
            duplicate_publish_error.end(),
            duplicate_publish_error_length.begin(),
            duplicate_publish_error_length.end());
        duplicate_publish_error.insert(
            duplicate_publish_error.end(), duplicate_publish_error_payload.begin(), duplicate_publish_error_payload.end());
        draft14_duplicate_publish_error_id_transport.reads[0].push_back(duplicate_publish_error);

        MoqtSession draft14_duplicate_publish_error_id_session(
            draft14_duplicate_publish_error_id_transport, std::string(kTestTrackNamespace), true);
        status = draft14_duplicate_publish_error_id_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-14 duplicate-publish-error-id session connect to succeed");
        status = draft14_duplicate_publish_error_id_session.publish(
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes));
        ok &= expect(!status.ok, "expected draft-14 publish to fail on duplicate publish response ID");
        ok &= expect(status.message == "received duplicate publish response request_id",
                     "expected duplicate publish response rejection for draft-14 PUBLISH_ERROR");
    }

    {
        MockTransport draft16_invalid_publish_ok_id_transport;
        draft16_invalid_publish_ok_id_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        draft16_invalid_publish_ok_id_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));
        // request_id=1 is odd/invalid for client-initiated publish requests.
        draft16_invalid_publish_ok_id_transport.reads[0].push_back(
            encode_publish_ok_message(DraftVersion::kDraft16, 1, 1));
        MoqtSession draft16_invalid_publish_ok_id_session(
            draft16_invalid_publish_ok_id_transport, std::string(kTestTrackNamespace), true);
        status = draft16_invalid_publish_ok_id_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-16 invalid-publish-ok-id session connect to succeed");
        status = draft16_invalid_publish_ok_id_session.publish(draft16_materialized);
        ok &= expect(!status.ok, "expected draft-16 publish to fail on invalid PUBLISH_OK request_id");
        ok &= expect(status.message == "received invalid request_id in PUBLISH_OK",
                     "expected strict invalid publish request_id failure");
        ok &= expect(draft16_invalid_publish_ok_id_transport.last_close_code == 0x3,
                     "expected invalid PUBLISH_OK request_id to close with PROTOCOL_VIOLATION");
    }

    {
        MockTransport draft16_unknown_publish_ok_param_transport;
        draft16_unknown_publish_ok_param_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        draft16_unknown_publish_ok_param_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));
        std::vector<std::uint8_t> payload = encode_varint(2);
        const std::vector<std::uint8_t> parameter_count = encode_varint(1);
        const std::vector<std::uint8_t> unknown_parameter_delta = encode_varint(0x11);
        const std::vector<std::uint8_t> unknown_parameter_length = encode_varint(0);
        payload.insert(payload.end(), parameter_count.begin(), parameter_count.end());
        payload.insert(payload.end(), unknown_parameter_delta.begin(), unknown_parameter_delta.end());
        payload.insert(payload.end(), unknown_parameter_length.begin(), unknown_parameter_length.end());
        std::vector<std::uint8_t> invalid_publish_ok = encode_varint(0x1e);
        append_be16(invalid_publish_ok, static_cast<std::uint16_t>(payload.size()));
        invalid_publish_ok.insert(invalid_publish_ok.end(), payload.begin(), payload.end());
        draft16_unknown_publish_ok_param_transport.reads[0].push_back(invalid_publish_ok);

        MoqtSession draft16_unknown_publish_ok_param_session(
            draft16_unknown_publish_ok_param_transport, std::string(kTestTrackNamespace), true);
        status = draft16_unknown_publish_ok_param_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-16 unknown-param session connect to succeed");
        status = draft16_unknown_publish_ok_param_session.publish(draft16_materialized);
        ok &= expect(!status.ok, "expected draft-16 publish to fail on unknown PUBLISH_OK parameter");
        ok &= expect(status.message == "received invalid PUBLISH_OK",
                     "expected unknown PUBLISH_OK parameter to fail decode");
        ok &= expect(draft16_unknown_publish_ok_param_transport.last_close_code == 0x3,
                     "expected unknown PUBLISH_OK parameter to close with PROTOCOL_VIOLATION");
    }

    {
        MockTransport draft16_duplicate_publish_ok_id_transport;
        draft16_duplicate_publish_ok_id_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        draft16_duplicate_publish_ok_id_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));
        draft16_duplicate_publish_ok_id_transport.reads[0].push_back(
            encode_publish_ok_message(DraftVersion::kDraft16, 2, 1));
        draft16_duplicate_publish_ok_id_transport.reads[0].push_back(
            encode_publish_ok_message(DraftVersion::kDraft16, 2, 1));
        MoqtSession draft16_duplicate_publish_ok_id_session(
            draft16_duplicate_publish_ok_id_transport, std::string(kTestTrackNamespace), true);
        status = draft16_duplicate_publish_ok_id_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-16 duplicate-publish-ok-id session connect to succeed");
        status = draft16_duplicate_publish_ok_id_session.publish(draft16_materialized);
        ok &= expect(!status.ok, "expected draft-16 publish to fail on duplicate PUBLISH_OK request_id");
        ok &= expect(status.message == "received duplicate publish response request_id",
                     "expected strict duplicate publish response failure");
    }

    {
        const auto subscribe_namespace = encode_subscribe_namespace_message(DraftVersion::kDraft16, 7, kTestTrackNamespace);
        std::size_t message_size = 0;
        ok &= expect(openmoq::publisher::transport::next_control_message(
                         subscribe_namespace, DraftVersion::kDraft16, message_size),
                     "expected draft-16 SUBSCRIBE_NAMESPACE to frame with uint16 length");
        ok &= expect(message_size == subscribe_namespace.size(),
                     "expected draft-16 SUBSCRIBE_NAMESPACE frame size to match bytes");

        SubscribeNamespaceMessage decoded;
        ok &= expect(openmoq::publisher::transport::decode_subscribe_namespace_message(
                         subscribe_namespace, DraftVersion::kDraft16, decoded),
                     "expected draft-16 SUBSCRIBE_NAMESPACE with Subscribe Options to decode");
        ok &= expect(decoded.request_id == 7, "expected draft-16 SUBSCRIBE_NAMESPACE request_id");
        ok &= expect(decoded.track_namespace_prefix.size() == 1 &&
                         decoded.track_namespace_prefix.front() == kTestTrackNamespace,
                     "expected draft-16 SUBSCRIBE_NAMESPACE prefix");
    }

    {
        const auto subscribe_namespace = encode_subscribe_namespace_message(DraftVersion::kDraft18, 7, kTestTrackNamespace);
        std::size_t message_size = 0;
        ok &= expect(message_type(subscribe_namespace) == 0x50,
                     "expected draft-18 SUBSCRIBE_NAMESPACE type 0x50");
        ok &= expect(openmoq::publisher::transport::next_control_message(
                         subscribe_namespace, DraftVersion::kDraft18, message_size),
                     "expected draft-18 SUBSCRIBE_NAMESPACE to frame with uint16 length");
        ok &= expect(message_size == subscribe_namespace.size(),
                     "expected draft-18 SUBSCRIBE_NAMESPACE frame size to match bytes");

        SubscribeNamespaceMessage decoded;
        ok &= expect(openmoq::publisher::transport::decode_subscribe_namespace_message(
                         subscribe_namespace, DraftVersion::kDraft18, decoded),
                     "expected draft-18 SUBSCRIBE_NAMESPACE to decode");
        ok &= expect(decoded.request_id == 7, "expected draft-18 SUBSCRIBE_NAMESPACE request_id");
        ok &= expect(decoded.track_namespace_prefix.size() == 1 &&
                         decoded.track_namespace_prefix.front() == kTestTrackNamespace,
                     "expected draft-18 SUBSCRIBE_NAMESPACE prefix");
    }

    {
        openmoq::publisher::transport::PublishOk empty_publish_ok;
        ok &= expect(openmoq::publisher::transport::decode_publish_ok(
                         encode_publish_ok_message(DraftVersion::kDraft16, 9), DraftVersion::kDraft16, empty_publish_ok),
                     "expected empty draft-16 PUBLISH_OK parameters to decode");
        ok &= expect(empty_publish_ok.request_id == 9, "expected draft-16 PUBLISH_OK request_id");
        ok &= expect(empty_publish_ok.forward == 1, "expected draft-16 PUBLISH_OK default FORWARD=1");
        ok &= expect(empty_publish_ok.subscriber_priority == 128,
                     "expected draft-16 PUBLISH_OK default SUBSCRIBER_PRIORITY=128");

        openmoq::publisher::transport::PublishOk forward_zero_publish_ok;
        ok &= expect(openmoq::publisher::transport::decode_publish_ok(
                         encode_publish_ok_message(DraftVersion::kDraft16, 11, 0),
                         DraftVersion::kDraft16,
                         forward_zero_publish_ok),
                     "expected draft-16 PUBLISH_OK FORWARD parameter to decode");
        ok &= expect(forward_zero_publish_ok.forward == 0, "expected draft-16 PUBLISH_OK FORWARD=0");

        const std::vector<std::uint8_t> invalid_forward_publish_ok = {
            0x1e, 0x00, 0x04, 0x0b, 0x01, 0x10, 0x02,
        };
        openmoq::publisher::transport::PublishOk invalid_forward;
        ok &= expect(!openmoq::publisher::transport::decode_publish_ok(
                         invalid_forward_publish_ok, DraftVersion::kDraft16, invalid_forward),
                     "expected invalid draft-16 PUBLISH_OK FORWARD value to be rejected");
    }

    {
        const auto request_error =
            openmoq::publisher::transport::encode_request_error_message(
                DraftVersion::kDraft16, 7, 2, 25, "track does not exist");
        openmoq::publisher::transport::RequestError decoded{};
        ok &= expect(openmoq::publisher::transport::decode_request_error(
                         request_error, DraftVersion::kDraft16, decoded),
                     "expected draft-16 REQUEST_ERROR with retry_interval to decode");
        ok &= expect(decoded.request_id == 7, "expected draft-16 REQUEST_ERROR request_id");
        ok &= expect(decoded.error_code == 2, "expected draft-16 REQUEST_ERROR error_code");
        ok &= expect(decoded.retry_interval == 25, "expected draft-16 REQUEST_ERROR retry_interval");
        ok &= expect(decoded.reason == "track does not exist", "expected draft-16 REQUEST_ERROR reason");
    }

    const std::vector<std::uint8_t> split_server_setup = encode_server_setup_message({
        .draft = DraftVersion::kDraft14,
        .max_request_id = 23,
    });
    MockTransport segmented_transport;
    segmented_transport.reads[0].push_back(
        std::vector<std::uint8_t>(split_server_setup.begin(), split_server_setup.begin() + 3));
    segmented_transport.reads[0].push_back(
        std::vector<std::uint8_t>(split_server_setup.begin() + 3, split_server_setup.end()));
    queue_subscribe_requests(segmented_transport, DraftVersion::kDraft14, kTestTrackNamespace, {{1, "catalog"}, {3, "vide_1"}});
    MoqtSession segmented_session(segmented_transport, std::string(kTestTrackNamespace));
    status = segmented_session.connect(endpoint, tls);
    ok &= expect(status.ok, "expected segmented setup connect to succeed");
    status = segmented_session.publish(materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes));
    ok &= expect(status.ok, "expected segmented SERVER_SETUP path to publish successfully");

    MockTransport extra_server_setup_param_transport;
    extra_server_setup_param_transport.reads[0].push_back(
        std::vector<std::uint8_t>({0x21, 0x00, 0x0f, 0xc0, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x0e, 0x02,
                                   0x02, 0x40, 0x64, 0x04, 0x44, 0x00}));
    queue_subscribe_requests(extra_server_setup_param_transport,
                             DraftVersion::kDraft14,
                             kTestTrackNamespace,
                             {{1, "catalog"}, {3, "vide_1"}});
    MoqtSession extra_server_setup_param_session(extra_server_setup_param_transport, std::string(kTestTrackNamespace));
    status = extra_server_setup_param_session.connect(endpoint, tls);
    ok &= expect(status.ok, "expected SERVER_SETUP with extra even-numbered parameter to connect successfully");
    status = extra_server_setup_param_session.publish(
        materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes));
    ok &= expect(status.ok, "expected publish to succeed after SERVER_SETUP with extra even-numbered parameter");

    MockTransport explicit_draft16_server_setup_transport;
    explicit_draft16_server_setup_transport.reads[0].push_back(
        std::vector<std::uint8_t>({0x21, 0x00, 0x0c, 0xc0, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x10, 0x01,
                                   0x02, 0x40, 0x64}));
    explicit_draft16_server_setup_transport.reads[0].push_back(
        encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));
    MoqtSession explicit_draft16_server_setup_session(explicit_draft16_server_setup_transport,
                                                      std::string(kTestTrackNamespace),
                                                      false,
                                                      false,
                                                      false,
                                                      std::chrono::seconds(5));
    status = explicit_draft16_server_setup_session.connect(endpoint, tls);
    ok &= expect(status.ok, "expected explicit-version draft-16 SERVER_SETUP to connect successfully");
    status = explicit_draft16_server_setup_session.publish(make_multitrack_plan(DraftVersion::kDraft16));
    ok &= expect(status.ok, "expected publish to succeed after explicit-version draft-16 SERVER_SETUP");

    ok &= expect(bytes_equal(encode_varint(0), {0x00}), "expected single-byte varint encoding");
    ok &= expect(bytes_equal(encode_varint(63), {0x3f}), "expected max one-byte varint encoding");
    ok &= expect(bytes_equal(encode_varint(64), {0x40, 0x40}), "expected two-byte varint encoding");
    ok &= expect(bytes_equal(encode_varint(16383), {0x7f, 0xff}), "expected max two-byte varint encoding");
    ok &= expect(bytes_equal(encode_varint(16384), {0x80, 0x00, 0x40, 0x00}), "expected four-byte varint encoding");
    ok &= expect(bytes_equal(encode_varint(1073741824ULL), {0xc0, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00}),
                 "expected eight-byte varint encoding");
    ok &= expect(encode_varint(4611686018427387904ULL).empty(), "expected oversized varint encoding to fail");

    const auto draft14_setup = encode_setup_message({
        .draft = DraftVersion::kDraft14,
        .authority = "example.com:4433",
        .path = "/moq",
        .max_request_id = 0,
    });
    const auto draft16_setup = encode_setup_message({
        .draft = DraftVersion::kDraft16,
        .authority = "example.com:4433",
        .path = "/moq",
        .max_request_id = 0,
    });
    if (!bytes_equal(draft14_setup,
                     {0x20, 0x00, 0x24, 0x01, 0xc0, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x0e, 0x03, 0x05, 0x10,
                      0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d, 0x3a, 0x34, 0x34, 0x33,
                      0x33, 0x01, 0x04, 0x2f, 0x6d, 0x6f, 0x71, 0x02, 0x00})) {
        std::cerr << "draft14 actual: " << hex_dump(draft14_setup) << '\n';
        ok = false;
    }
    if (!bytes_equal(draft16_setup,
                     {0x20, 0x00, 0x1b, 0x03, 0x01, 0x04, 0x2f, 0x6d, 0x6f, 0x71, 0x01, 0x00, 0x03, 0x10, 0x65,
                      0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d, 0x3a, 0x34, 0x34, 0x33, 0x33})) {
        std::cerr << "draft16 actual: " << hex_dump(draft16_setup) << '\n';
        ok = false;
    }

    {
        MockTransport draft18_transport;
        draft18_transport.reads[3].push_back(encode_draft18_setup_response());
        draft18_transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        draft18_transport.reads[4].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft18, 2));
        draft18_transport.reads[8].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft18, 4));

        MoqtSession draft18_session(draft18_transport, std::string(kTestTrackNamespace), true);
        status = draft18_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 session connect to succeed");

        const PublishPlan draft18_materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft18), source_bytes);
        status = draft18_session.publish(draft18_materialized);
        ok &= expect(status.ok, "expected draft-18 publish to succeed");
        ok &= expect(draft18_transport.writes.size() >= 4, "expected draft-18 request-stream writes");
        if (draft18_transport.writes.size() >= 4) {
            ok &= expect(draft18_transport.writes[0].stream_id == 2,
                         "expected draft-18 setup on client unidirectional control stream");
            ok &= expect(draft18_transport.writes[1].stream_id == 0 &&
                             message_type(draft18_transport.writes[1].bytes) == 0x06,
                         "expected draft-18 PUBLISH_NAMESPACE on dedicated request stream");
            ok &= expect(draft18_transport.writes[2].stream_id == 4 &&
                             message_type(draft18_transport.writes[2].bytes) == 0x1d,
                         "expected first draft-18 PUBLISH on dedicated request stream");
            ok &= expect(draft18_transport.writes[3].stream_id == 8 &&
                             message_type(draft18_transport.writes[3].bytes) == 0x1d,
                         "expected second draft-18 PUBLISH on dedicated request stream");
        }
        ok &= expect(draft18_transport.opens.size() >= 4 &&
                         draft18_transport.opens[0].direction == StreamDirection::kUnidirectional &&
                         draft18_transport.opens[1].direction == StreamDirection::kBidirectional,
                     "expected draft-18 to use uni setup then bidi request streams");
    }

    {
        MockTransport draft18_wt_transport;
        draft18_wt_transport.reads[3].push_back(encode_draft18_setup_response());
        EndpointConfig wt_endpoint = endpoint;
        wt_endpoint.transport = openmoq::publisher::transport::TransportKind::kWebTransport;
        wt_endpoint.path = "/moq";

        MoqtSession draft18_wt_session(draft18_wt_transport, std::string(kTestTrackNamespace), true);
        status = draft18_wt_session.connect(wt_endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 WebTransport session connect to succeed");

        const PublishPlan draft18_materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft18), source_bytes);
        status = draft18_wt_session.publish(draft18_materialized);
        ok &= expect(!status.ok, "expected draft-18 WebTransport publish to stop after setup without request fixtures");
        ok &= expect(!draft18_wt_transport.writes.empty(), "expected draft-18 WebTransport SETUP write");
        if (!draft18_wt_transport.writes.empty()) {
            const auto& setup = draft18_wt_transport.writes.front().bytes;
            std::size_t offset = 0;
            std::uint64_t type = 0;
            ok &= expect(decode_vi64(setup, offset, type) && type == 0x2f00,
                         "expected draft-18 WebTransport setup type SETUP");
            ok &= expect(offset + 2 == setup.size() && setup[offset] == 0 && setup[offset + 1] == 0,
                         "expected draft-18 WebTransport SETUP to omit AUTHORITY and PATH options");
        }
    }

    {
        MockTransport draft18_legacy_setup_transport;
        draft18_legacy_setup_transport.reads[3].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 0,
        }));

        MoqtSession draft18_legacy_setup_session(
            draft18_legacy_setup_transport, std::string(kTestTrackNamespace), true);
        status = draft18_legacy_setup_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 legacy-setup session connect to defer setup");

        const PublishPlan draft18_materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft18), source_bytes);
        status = draft18_legacy_setup_session.publish(draft18_materialized);
        ok &= expect(!status.ok, "expected draft-18 to reject legacy SERVER_SETUP");
        ok &= expect(status.message == "received invalid SETUP message",
                     "expected draft-18 legacy SERVER_SETUP rejection to name SETUP");
        ok &= expect(draft18_legacy_setup_transport.last_close_code == 0x3,
                     "expected invalid draft-18 SETUP to close with PROTOCOL_VIOLATION");
    }

    {
        MockTransport draft18_subscribe_tracks_transport;
        draft18_subscribe_tracks_transport.reads[3].push_back(encode_draft18_setup_response());
        draft18_subscribe_tracks_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        draft18_subscribe_tracks_transport.reads[1].push_back(
            encode_subscribe_tracks_message(91, kTestTrackNamespace));
        draft18_subscribe_tracks_transport.reads[4].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 2));
        draft18_subscribe_tracks_transport.reads[8].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 4));

        MoqtSession draft18_subscribe_tracks_session(
            draft18_subscribe_tracks_transport,
            std::string(kTestTrackNamespace),
            false,
            false,
            false,
            std::chrono::seconds(1));
        status = draft18_subscribe_tracks_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 SUBSCRIBE_TRACKS session connect to succeed");

        const PublishPlan draft18_materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft18), source_bytes);
        status = draft18_subscribe_tracks_session.publish(draft18_materialized);
        ok &= expect(status.ok, "expected draft-18 SUBSCRIBE_TRACKS publish to succeed");

        bool saw_subscribe_tracks_ok = false;
        bool saw_first_publish = false;
        bool saw_second_publish = false;
        for (const auto& write : draft18_subscribe_tracks_transport.writes) {
            if (write.stream_id == 1 && message_type(write.bytes) == 0x07) {
                saw_subscribe_tracks_ok = true;
            }
            if (write.stream_id == 4 && message_type(write.bytes) == 0x1d) {
                saw_first_publish = true;
            }
            if (write.stream_id == 8 && message_type(write.bytes) == 0x1d) {
                saw_second_publish = true;
            }
        }
        ok &= expect(saw_subscribe_tracks_ok, "expected SUBSCRIBE_TRACKS response stream REQUEST_OK");
        ok &= expect(saw_first_publish && saw_second_publish,
                     "expected SUBSCRIBE_TRACKS to publish matching draft-18 tracks on request streams");
    }

    {
        MockTransport draft18_subscribe_transport;
        draft18_subscribe_transport.reads[3].push_back(encode_draft18_setup_response());
        draft18_subscribe_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        const std::vector<std::uint8_t> subscribe_message =
            encode_subscribe_message(91, kTestTrackNamespace, "vide_1", 0, DraftVersion::kDraft18);
        const auto subscribe_split = subscribe_message.begin() + 4;
        const std::vector<std::uint8_t> subscribe_prefix(subscribe_message.begin(), subscribe_split);
        const std::vector<std::uint8_t> subscribe_suffix(subscribe_split, subscribe_message.end());

        MoqtSession draft18_subscribe_session(
            draft18_subscribe_transport,
            std::string(kTestTrackNamespace),
            false,
            false,
            false,
            std::chrono::seconds(1));
        status = draft18_subscribe_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 SUBSCRIBE session connect to succeed");

        bool injected_subscribe = false;
        draft18_subscribe_transport.on_accept_timeout =
            [&](MockTransport& transport, StreamDirection direction) {
                if (!injected_subscribe && direction == StreamDirection::kBidirectional) {
                    transport.reads[1].push_back(subscribe_prefix);
                    transport.reads[1].push_back(subscribe_suffix);
                    injected_subscribe = true;
                }
            };

        const PublishPlan draft18_materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft18), source_bytes);
        status = draft18_subscribe_session.publish(draft18_materialized);
        ok &= expect(status.ok, "expected draft-18 SUBSCRIBE request stream publish to succeed");
        ok &= expect(injected_subscribe, "expected draft-18 SUBSCRIBE to arrive during the control-stream wait");

        bool saw_subscribe_ok_on_request_stream = false;
        bool saw_object_on_unidirectional_stream = false;
        bool saw_publish_done_on_request_stream = false;
        for (const auto& write : draft18_subscribe_transport.writes) {
            if (write.stream_id == 1 && message_type(write.bytes) == 0x04) {
                saw_subscribe_ok_on_request_stream = true;
            }
            if ((write.stream_id & 0x2ULL) != 0 && message_type(write.bytes) != 0x2f00) {
                saw_object_on_unidirectional_stream = true;
            }
            if (write.stream_id == 1 && message_type(write.bytes) == 0x0b) {
                saw_publish_done_on_request_stream = true;
            }
        }
        ok &= expect(saw_subscribe_ok_on_request_stream,
                     "expected draft-18 SUBSCRIBE_OK on the inbound request stream");
        ok &= expect(saw_object_on_unidirectional_stream,
                     "expected draft-18 SUBSCRIBE objects on unidirectional streams");
        ok &= expect(saw_publish_done_on_request_stream,
                     "expected draft-18 PUBLISH_DONE on the inbound request stream");
    }

    {
        MockTransport draft18_subscribe_namespace_transport;
        draft18_subscribe_namespace_transport.reads[3].push_back(encode_draft18_setup_response());
        draft18_subscribe_namespace_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        draft18_subscribe_namespace_transport.reads[1].push_back(
            encode_subscribe_namespace_message(DraftVersion::kDraft18, 91, kTestTrackNamespace));

        MoqtSession draft18_subscribe_namespace_session(
            draft18_subscribe_namespace_transport,
            std::string(kTestTrackNamespace),
            false,
            false,
            false,
            std::chrono::seconds(1));
        status = draft18_subscribe_namespace_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 SUBSCRIBE_NAMESPACE session connect to succeed");

        const PublishPlan draft18_materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft18), source_bytes);
        status = draft18_subscribe_namespace_session.publish(draft18_materialized);
        ok &= expect(status.ok, "expected draft-18 SUBSCRIBE_NAMESPACE request stream publish to succeed");

        bool saw_namespace_request_ok = false;
        for (const auto& write : draft18_subscribe_namespace_transport.writes) {
            if (write.stream_id == 1 && message_type(write.bytes) == 0x07) {
                saw_namespace_request_ok = true;
            }
        }
        ok &= expect(saw_namespace_request_ok,
                     "expected draft-18 SUBSCRIBE_NAMESPACE REQUEST_OK on the inbound request stream");
    }

    {
        MockTransport draft18_legacy_subscribe_namespace_transport;
        draft18_legacy_subscribe_namespace_transport.reads[3].push_back(encode_draft18_setup_response());
        draft18_legacy_subscribe_namespace_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        std::vector<std::uint8_t> legacy_subscribe_namespace =
            encode_subscribe_namespace_message(DraftVersion::kDraft18, 91, kTestTrackNamespace);
        legacy_subscribe_namespace[0] = 0x11;
        draft18_legacy_subscribe_namespace_transport.reads[1].push_back(legacy_subscribe_namespace);

        MoqtSession draft18_legacy_subscribe_namespace_session(
            draft18_legacy_subscribe_namespace_transport,
            std::string(kTestTrackNamespace),
            false,
            false,
            false,
            std::chrono::seconds(1));
        status = draft18_legacy_subscribe_namespace_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 legacy SUBSCRIBE_NAMESPACE session connect to succeed");

        const PublishPlan draft18_materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft18), source_bytes);
        status = draft18_legacy_subscribe_namespace_session.publish(draft18_materialized);
        ok &= expect(!status.ok, "expected draft-18 legacy SUBSCRIBE_NAMESPACE type to fail");
        ok &= expect(status.message == "received unsupported request stream",
                     "expected draft-18 legacy SUBSCRIBE_NAMESPACE to be treated as unsupported request type");
    }

    {
        MockTransport draft18_control_subscribe_transport;
        draft18_control_subscribe_transport.reads[3].push_back(encode_draft18_setup_response());
        draft18_control_subscribe_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        draft18_control_subscribe_transport.reads[3].push_back(
            encode_subscribe_message(91, kTestTrackNamespace, "vide_1", 0, DraftVersion::kDraft18));

        MoqtSession draft18_control_subscribe_session(
            draft18_control_subscribe_transport,
            std::string(kTestTrackNamespace),
            false,
            false,
            false,
            std::chrono::seconds(1));
        status = draft18_control_subscribe_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 misplaced-control SUBSCRIBE session connect to succeed");

        const PublishPlan draft18_materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft18), source_bytes);
        status = draft18_control_subscribe_session.publish(draft18_materialized);
        ok &= expect(!status.ok, "expected draft-18 SUBSCRIBE on control stream to fail");
        ok &= expect(status.message == "draft-18 request message received on control stream",
                     "expected explicit draft-18 control-stream request rejection");
    }

    {
        MockTransport draft18_bad_response_transport;
        draft18_bad_response_transport.reads[3].push_back(encode_draft18_setup_response());
        // Inject SUBSCRIBE (0x03) on namespace request stream where REQUEST_OK/REQUEST_ERROR is expected.
        draft18_bad_response_transport.reads[0].push_back(
            encode_subscribe_message(1, kTestTrackNamespace, "catalog", 0, DraftVersion::kDraft18));
        MoqtSession draft18_bad_response_session(draft18_bad_response_transport, std::string(kTestTrackNamespace), true);
        status = draft18_bad_response_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 bad-response session connect to succeed");

        const PublishPlan draft18_materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft18), source_bytes);
        status = draft18_bad_response_session.publish(draft18_materialized);
        ok &= expect(!status.ok, "expected draft-18 publish to fail on unexpected request-stream response");
        ok &= expect(status.message == "unexpected response type for request stream",
                     "expected strict request-stream response validation message");
    }

    {
        MockTransport draft18_wrong_type_transport;
        draft18_wrong_type_transport.reads[3].push_back(encode_draft18_setup_response());
        // Namespace request stream receives PUBLISH_OK, which is a valid MOQT
        // message type but invalid response type for a namespace request.
        draft18_wrong_type_transport.reads[0].push_back(
            encode_publish_ok_message(DraftVersion::kDraft18, 9, 1));

        MoqtSession draft18_wrong_type_session(draft18_wrong_type_transport, std::string(kTestTrackNamespace), true);
        status = draft18_wrong_type_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 wrong-type session connect to succeed");

        const PublishPlan draft18_materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft18), source_bytes);
        status = draft18_wrong_type_session.publish(draft18_materialized);
        ok &= expect(!status.ok, "expected draft-18 publish to fail on wrong response type");
        ok &= expect(status.message == "unexpected response type for request stream",
                     "expected strict request/response type correlation failure message");
    }

    {
        MockTransport draft18_goaway_transport;
        draft18_goaway_transport.reads[3].push_back(encode_draft18_setup_response());
        // GOAWAY message on namespace request stream:
        // type=0x10, length=0.
        draft18_goaway_transport.reads[0].push_back(std::vector<std::uint8_t>{0x10, 0x00, 0x00});

        MoqtSession draft18_goaway_session(draft18_goaway_transport, std::string(kTestTrackNamespace), true);
        status = draft18_goaway_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 GOAWAY session connect to succeed");

        const PublishPlan draft18_materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft18), source_bytes);
        status = draft18_goaway_session.publish(draft18_materialized);
        ok &= expect(!status.ok, "expected draft-18 publish to surface request-stream GOAWAY migration");
        ok &= expect(status.message == "request stream received GOAWAY migration",
                     "expected explicit GOAWAY request-stream migration message");
        ok &= expect(draft18_goaway_transport.state() == ConnectionState::kConnected,
                     "expected request-stream GOAWAY not to close draft-18 session");
        ok &= expect(!draft18_goaway_transport.reset_calls.empty() &&
                         draft18_goaway_transport.reset_calls.back().first == 0,
                     "expected request-stream GOAWAY to reset the old request stream");
    }

    {
        MockTransport draft18_duplicate_response_transport;
        draft18_duplicate_response_transport.reads[3].push_back(encode_draft18_setup_response());
        draft18_duplicate_response_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        draft18_duplicate_response_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));

        MoqtSession draft18_duplicate_response_session(
            draft18_duplicate_response_transport, std::string(kTestTrackNamespace), true);
        status = draft18_duplicate_response_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 duplicate-response session connect to succeed");

        const PublishPlan draft18_materialized =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft18), source_bytes);
        status = draft18_duplicate_response_session.publish(draft18_materialized);
        ok &= expect(!status.ok, "expected draft-18 publish to fail on duplicate response");
        ok &= expect(status.message == "multiple responses on request stream",
                     "expected duplicate request-stream response failure message");
    }

    {
        // After a successful draft-18 publish, close() resets all request stream IDs.
        // MockTransport draft-18 stream IDs: 2=local setup/control, 0=PUBLISH_NAMESPACE,
        // 4=first PUBLISH, 8=second PUBLISH.
        MockTransport d18_retain_transport;
        d18_retain_transport.reads[3].push_back(encode_draft18_setup_response());
        d18_retain_transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        d18_retain_transport.reads[4].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft18, 2));
        d18_retain_transport.reads[8].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft18, 4));

        MoqtSession d18_retain_session(d18_retain_transport, std::string(kTestTrackNamespace), true);
        TransportStatus st = d18_retain_session.connect(endpoint, tls);
        ok &= expect(st.ok, "draft-18 retain: connect should succeed");

        const PublishPlan d18_plan =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft18), source_bytes);
        st = d18_retain_session.publish(d18_plan);
        ok &= expect(st.ok, "draft-18 retain: publish should succeed");

        st = d18_retain_session.close();
        ok &= expect(st.ok, "draft-18 retain: close should succeed");

        const auto& resets = d18_retain_transport.reset_calls;
        ok &= expect(resets.size() == 3,
                     "expected 3 reset_stream calls on close (1 namespace + 2 track)");

        std::set<std::uint64_t> reset_ids;
        for (const auto& [sid, ec] : resets) {
            reset_ids.insert(sid);
            ok &= expect(ec == 0, "expected error_code 0 for all resets");
        }
        ok &= expect(reset_ids.count(0) == 1, "expected namespace stream 0 to be reset");
        ok &= expect(reset_ids.count(4) == 1, "expected track stream 4 to be reset");
        ok &= expect(reset_ids.count(8) == 1, "expected track stream 8 to be reset");
    }

    {
        // Calling close() twice must not re-reset already-cleared stream IDs.
        MockTransport d18_dc_transport;
        d18_dc_transport.reads[3].push_back(encode_draft18_setup_response());
        d18_dc_transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        d18_dc_transport.reads[4].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft18, 2));
        d18_dc_transport.reads[8].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft18, 4));

        MoqtSession d18_dc_session(d18_dc_transport, std::string(kTestTrackNamespace), true);
        d18_dc_session.connect(endpoint, tls);
        const PublishPlan d18_plan2 =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft18), source_bytes);
        d18_dc_session.publish(d18_plan2);

        d18_dc_session.close();
        const std::size_t after_first = d18_dc_transport.reset_calls.size();
        d18_dc_session.close();
        ok &= expect(d18_dc_transport.reset_calls.size() == after_first,
                     "second close() must not re-reset already-cleared stream IDs");
    }

    MockTransport close_transport;
    MoqtSession close_session(close_transport, std::string(kTestTrackNamespace));
    status = close_session.connect(endpoint, tls);
    ok &= expect(status.ok, "expected close session connect to succeed");
    status = close_session.close(7);
    ok &= expect(status.ok, "expected close to succeed");
    ok &= expect(close_transport.last_close_code == 7, "expected close code to propagate");

    {
        MockTransport timeout_transport;
        timeout_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        queue_subscribe_requests(timeout_transport,
                                 DraftVersion::kDraft14,
                                 kTestTrackNamespace,
                                 {{1, "catalog"}, {3, "vide_1"}});
        MoqtSession timeout_session(
            timeout_transport, std::string(kTestTrackNamespace), false, false, false, std::chrono::seconds(11));
        status = timeout_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected custom-timeout session connect to succeed");
        status = timeout_session.publish(
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes));
        ok &= expect(status.ok, "expected publish to succeed with custom subscriber timeout");
        ok &= expect(std::find(timeout_transport.read_timeouts.begin(),
                               timeout_transport.read_timeouts.end(),
                               std::chrono::seconds(11)) != timeout_transport.read_timeouts.end(),
                     "expected custom subscriber timeout to reach transport reads");
    }

    {
        MockTransport idle_publish_transport;
        idle_publish_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        idle_publish_transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));
        MoqtSession idle_publish_session(
            idle_publish_transport, std::string(kTestTrackNamespace), false, false, false, std::chrono::seconds(5));
        status = idle_publish_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected idle-publish session connect to succeed");
        status = idle_publish_session.publish(make_multitrack_plan(DraftVersion::kDraft16));
        ok &= expect(status.ok, "expected idle publish without subscribers to exit cleanly");
        ok &= expect(!idle_publish_transport.writes.empty() &&
                         message_type(idle_publish_transport.writes.back().bytes) == 0x09,
                     "expected idle publish flow to send PUBLISH_NAMESPACE_DONE");
        ok &= expect(std::find(idle_publish_transport.read_timeouts.begin(),
                               idle_publish_transport.read_timeouts.end(),
                               std::chrono::seconds(5)) != idle_publish_transport.read_timeouts.end(),
                     "expected idle publish to honor the configured subscriber timeout");
    }

    {
        MockTransport closed_idle_publish_transport;
        closed_idle_publish_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        closed_idle_publish_transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));
        closed_idle_publish_transport.missing_read_error = "webtransport connection closed";
        MoqtSession closed_idle_publish_session(
            closed_idle_publish_transport, std::string(kTestTrackNamespace), false, false, false, std::chrono::seconds(5));
        status = closed_idle_publish_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected closed-idle session connect to succeed");
        status = closed_idle_publish_session.publish(make_multitrack_plan(DraftVersion::kDraft16));
        ok &= expect(status.ok, "expected idle publish to exit cleanly when the control stream closes");
        ok &= expect(!closed_idle_publish_transport.writes.empty() &&
                         message_type(closed_idle_publish_transport.writes.back().bytes) == 0x09,
                     "expected closed-idle publish flow to send PUBLISH_NAMESPACE_DONE");
    }

    for (const DraftVersion draft : {DraftVersion::kDraft14, DraftVersion::kDraft16}) {
        MockTransport live_transport;
        live_transport.reads[0].push_back(encode_server_setup_message({
            .draft = draft,
            .max_request_id = 8,
        }));
        live_transport.reads[0].push_back(encode_publish_namespace_ok_message(draft, 0));

        MoqtSession live_session(
            live_transport, std::string(kTestTrackNamespace), false, false, false, std::chrono::seconds(1));
        status = live_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected live session connect to succeed");

        const auto live_bytes = make_live_init_mp4();
        std::string live_input_bytes(live_bytes.begin(), live_bytes.end());
        std::istringstream live_input(live_input_bytes);
        status = live_session.publish_live(live_input, draft, false);
        ok &= expect(status.ok, "expected live publish to avoid blocking on preannounce PUBLISH_OK");
        ok &= expect(control_message_count(live_transport, 0x1d) == 1,
                     "expected live publish to preannounce one media track");
        ok &= expect(!live_transport.writes.empty() &&
                         message_type(live_transport.writes.back().bytes) == 0x09,
                     "expected live publish to finish with PUBLISH_NAMESPACE_DONE");
    }

    {
        // MSF section 11.3: after a live catalog has actually been published
        // through publish_live(), end_broadcast() must write a genuine final
        // catalog object onto the wire (not just send PUBLISH_DONE for media).
        //
        // C1 (final review): a prior round deferred the catalog
        // subscription's PUBLISH_DONE only when catalog_republish_interval_
        // was set (> 0). end_broadcast() can open a further catalog stream
        // on the same alias regardless of that knob, so at the DEFAULT
        // config (as configured here) the deferral must be unconditional --
        // section 10.11 forbids PUBLISH_DONE before every stream a
        // subscription will ever open has closed. This is verified two ways
        // below: (a) via on_read, observing mid-flight that the catalog
        // object goes out before PUBLISH_DONE does -- proving PUBLISH_DONE
        // is deferred to publish_live()'s own exit rather than sent
        // synchronously while processing the catalog SUBSCRIBE, which is
        // exactly the gap end_broadcast() could otherwise land a stream
        // into; and (b) the original assertion, retargeted: PUBLISH_DONE
        // must still be sent by the time publish_live() returns, just not
        // "immediately" in the old, unsafe sense.
        MockTransport end_broadcast_transport;
        end_broadcast_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        end_broadcast_transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        end_broadcast_transport.reads[0].push_back(
            encode_subscribe_message(1, kTestTrackNamespace, "catalog", 0));

        bool catalog_object_seen = false;
        bool observed_catalog_object_before_publish_done = false;
        end_broadcast_transport.on_read = [&](MockTransport& transport, std::uint64_t) {
            bool has_catalog_object = false;
            bool has_publish_done = false;
            for (const auto& write : transport.writes) {
                if (write.stream_id != 0) {
                    has_catalog_object = true;
                } else if (message_type(write.bytes) == 0x0b) {
                    has_publish_done = true;
                }
            }
            if (has_catalog_object) {
                catalog_object_seen = true;
                if (!has_publish_done) {
                    observed_catalog_object_before_publish_done = true;
                }
            }
        };

        // auto_forward=true so the loop keeps polling control (and calling
        // read_stream, which fires on_read above) for subscriber_timeout_
        // after EOF instead of exiting on the very next tick -- that grace
        // window is what gives on_read a chance to observe the wire between
        // send_catalog() and the deferred PUBLISH_DONE.
        MoqtSession end_broadcast_session(
            end_broadcast_transport, std::string(kTestTrackNamespace), true, false, false, std::chrono::seconds(1));
        status = end_broadcast_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected end-broadcast live session connect to succeed");

        const auto live_bytes = make_live_init_mp4();
        std::string live_input_bytes(live_bytes.begin(), live_bytes.end());
        std::istringstream live_input(live_input_bytes);
        status = end_broadcast_session.publish_live(live_input, DraftVersion::kDraft14, false);
        ok &= expect(status.ok, "expected end-broadcast live publish to succeed");
        ok &= expect(catalog_object_seen, "expected the catalog object to actually be sent");
        ok &= expect(observed_catalog_object_before_publish_done,
                     "expected at least one moment where the catalog object had gone out but "
                     "PUBLISH_DONE had not -- proving PUBLISH_DONE is deferred to publish_live()'s "
                     "own exit rather than sent synchronously while processing the catalog "
                     "SUBSCRIBE, even at the default catalog_republish_interval_ (section 10.11 "
                     "MUST NOT: end_broadcast() must still be free to open a further catalog "
                     "stream without racing an already-sent PUBLISH_DONE)");
        ok &= expect(control_message_count(end_broadcast_transport, 0x0b) >= 1,
                     "expected the live catalog subscription to still eventually receive "
                     "PUBLISH_DONE by the time publish_live() returns");

        const std::size_t writes_before_end = end_broadcast_transport.writes.size();
        status = end_broadcast_session.end_broadcast(
            openmoq::publisher::EndBroadcastMode::kTerminate, DraftVersion::kDraft14);
        ok &= expect(status.ok, "expected end_broadcast to succeed after a live catalog publish");
        ok &= expect(end_broadcast_transport.writes.size() > writes_before_end,
                     "expected end_broadcast to write the final catalog onto the wire");

        bool found_final_catalog = false;
        for (std::size_t i = writes_before_end; i < end_broadcast_transport.writes.size(); ++i) {
            std::uint64_t stream_type = 0;
            std::uint64_t track_alias = 0;
            std::uint64_t group_id = 0;
            std::uint64_t object_id_delta = 0;
            std::uint64_t payload_length = 0;
            std::vector<std::uint8_t> payload;
            if (decode_object_stream_fields(end_broadcast_transport.writes[i].bytes,
                                            stream_type,
                                            track_alias,
                                            group_id,
                                            object_id_delta,
                                            payload_length,
                                            payload)) {
                const std::string text(payload.begin(), payload.end());
                if (text.find("\"isComplete\":true") != std::string::npos) {
                    found_final_catalog = true;
                    ok &= expect(track_alias == 0, "expected the final catalog on the catalog track alias");
                    ok &= expect(group_id == 1,
                                 "expected the final catalog in a new group after the initial one");
                    ok &= expect(text.find("\"tracks\":[]") != std::string::npos,
                                 "expected the final catalog to carry an empty tracks array");
                }
            }
        }
        ok &= expect(found_final_catalog,
                     "expected end_broadcast to publish an isComplete final catalog on the wire");
    }

    {
        // Fix round 1, Finding 1: draft-ietf-moq-transport-19 section 10.11
        // forbids sending PUBLISH_DONE for a subscription until every stream
        // it will ever open has closed. With catalog_republish_interval set,
        // more independent-catalog streams can open after the first one, so
        // PUBLISH_DONE for the catalog subscription must be deferred rather
        // than sent immediately (as it still correctly is when the interval
        // is zero, per the test above and the many other catalog-subscribe
        // tests in this file).
        //
        // Catalog object writes and control writes are disambiguated by
        // stream_id rather than by decoding message contents: a catalog
        // object write always lands on a freshly opened, non-control
        // stream_id (see SubgroupSenderState::serve), while PUBLISH_DONE is
        // always written to control_stream_id_ (0 here, since draft-14 does
        // not use request streams). Decoding PUBLISH_DONE's bytes with
        // decode_object_stream_fields can spuriously "succeed" (both are
        // varint sequences), so stream_id is the only reliable signal.
        //
        // auto-forward mode is used (rather than await-subscribe) so the
        // loop keeps polling for subscriber_timeout_ after EOF (see
        // eof_deadline in the auto-forward loop) instead of exiting on the
        // very next tick -- that grace window is what gives
        // maybe_republish_catalog() a chance to actually fire more than
        // once before the loop, and the deferred PUBLISH_DONE, exit.
        MockTransport republish_transport;
        republish_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        republish_transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        republish_transport.reads[0].push_back(
            encode_subscribe_message(1, kTestTrackNamespace, "catalog", 0));

        MoqtSession republish_session(
            republish_transport, std::string(kTestTrackNamespace), true, false, false, std::chrono::seconds(2));
        republish_session.set_catalog_republish_interval(std::chrono::seconds(1));
        status = republish_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected republish-interval session connect to succeed");

        const auto live_bytes = make_live_init_mp4();
        std::string live_input_bytes(live_bytes.begin(), live_bytes.end());
        std::istringstream live_input(live_input_bytes);
        status = republish_session.publish_live(live_input, DraftVersion::kDraft14, false);
        ok &= expect(status.ok, "expected republish-interval live publish to succeed");

        std::vector<std::size_t> catalog_object_indices;
        std::vector<std::size_t> catalog_publish_done_indices;
        for (std::size_t i = 0; i < republish_transport.writes.size(); ++i) {
            const auto& write = republish_transport.writes[i];
            if (write.stream_id != 0) {
                // Every non-control-stream write in this test is a catalog
                // object write: no media track was subscribed.
                catalog_object_indices.push_back(i);
                continue;
            }
            if (message_type(write.bytes) == 0x0b) {
                catalog_publish_done_indices.push_back(i);
            }
        }

        ok &= expect(!catalog_object_indices.empty(),
                     "expected at least the initial catalog object to have been written");
        ok &= expect(catalog_object_indices.size() >= 2,
                     "expected catalog_republish_interval to cause at least one republish before "
                     "the 2-second subscriber_timeout grace window closed");
        ok &= expect(catalog_publish_done_indices.size() == 1,
                     "expected exactly one PUBLISH_DONE for the catalog subscription");

        if (!catalog_object_indices.empty() && catalog_publish_done_indices.size() == 1) {
            ok &= expect(catalog_publish_done_indices.front() > catalog_object_indices.back(),
                         "expected PUBLISH_DONE to be deferred until after every catalog object "
                         "write, not sent immediately after the first one (section 10.11 MUST NOT)");
        }
    }

    {
        // Fix round 2, Finding 1 residual: the deferred catalog PUBLISH_DONE
        // must still be sent when publish_live() exits via an error path
        // (drain_queue()/process_control_messages()/maybe_republish_catalog()/
        // read_stream() failing partway through the loop), not just via the
        // graceful end-of-loop cleanup -- that is exactly what
        // DeferredCatalogPublishDoneGuard exists for.
        //
        // Getting a *deterministic* error exit out of MockTransport took two
        // discarded attempts, recorded here since each cost real debugging
        // time: (1) forcing the error via MockTransport::missing_read_error
        // alone races the await-subscribe loop's own graceful is_eof exit
        // check, which (with no media in this test's input) usually wins,
        // making the test flaky and, in several runs, unable to reach the
        // error path at all; (2) delaying queue->eof with a custom
        // sleep-before-EOF streambuf to try to win that race predictably
        // still did not reliably reach a second control read in practice.
        //
        // This version sidesteps the race entirely: a second, malformed
        // control message (type 0x03 SUBSCRIBE with a 1-byte payload, too
        // short for decode_subscribe_message to parse) is delivered in the
        // *same* chunk as the valid catalog SUBSCRIBE, so both are processed
        // within process_control_messages()'s first pass over that chunk,
        // before the loop ever re-checks is_eof. This deterministically
        // reaches `return {protocol_violation(transport_, "received invalid
        // SUBSCRIBE"), 0};` -- one of the early-return sites the reviewer
        // enumerated.
        //
        // What this test can and cannot observe: protocol_violation() calls
        // transport.close() before returning the failure, so by the time
        // DeferredCatalogPublishDoneGuard's destructor tries to write the
        // deferred PUBLISH_DONE, the mock transport is already closed and
        // the write correctly, silently fails (logged, not propagated, per
        // the guard's contract) -- there is no way to observe a
        // *successful* wire write here, because a real connection closed by
        // a protocol violation genuinely cannot carry any more data,
        // deferred or not. What this test verifies instead: (a) the catalog
        // object was actually sent before the error (proving send_catalog()
        // ran), (b) publish_live() truly exited via the error path and not
        // the graceful one, and (c) the guard's destructor actually *ran and
        // attempted* the deferred send -- observed via its warning
        // diagnostic on stderr, which only fires from inside the guard when
        // catalog_publish_done_deferred was still true at function exit.
        // I could not find, within reasonable effort, a MockTransport-
        // drivable error path that both reaches one of the early-return
        // sites and leaves the transport open enough to observe a
        // subsequent successful write; every reproducible decode-triggered
        // failure in this code goes through protocol_violation().
        MockTransport error_exit_transport;
        error_exit_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        error_exit_transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        std::vector<std::uint8_t> subscribe_then_malformed =
            encode_subscribe_message(1, kTestTrackNamespace, "catalog", 0);
        const std::vector<std::uint8_t> malformed_subscribe{0x03, 0x00, 0x01, 0xff};
        subscribe_then_malformed.insert(subscribe_then_malformed.end(), malformed_subscribe.begin(),
                                        malformed_subscribe.end());
        error_exit_transport.reads[0].push_back(subscribe_then_malformed);

        MoqtSession error_exit_session(
            error_exit_transport, std::string(kTestTrackNamespace), false, false, false, std::chrono::seconds(1));
        error_exit_session.set_catalog_republish_interval(std::chrono::seconds(1));
        status = error_exit_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected error-exit session connect to succeed");

        const auto live_bytes = make_live_init_mp4();
        std::string live_input_bytes(live_bytes.begin(), live_bytes.end());
        std::istringstream live_input(live_input_bytes);

        std::ostringstream captured_stderr;
        std::streambuf* real_cerr_buf = std::cerr.rdbuf(captured_stderr.rdbuf());
        status = error_exit_session.publish_live(live_input, DraftVersion::kDraft14, false);
        std::cerr.rdbuf(real_cerr_buf);

        ok &= expect(!status.ok && status.message == "received invalid SUBSCRIBE",
                     "expected the malformed SUBSCRIBE to surface as an error from publish_live, "
                     "proving this test actually exercises an error-exit path and not the graceful one");

        bool saw_catalog_object = false;
        for (const auto& write : error_exit_transport.writes) {
            if (write.stream_id != 0) {
                saw_catalog_object = true;
            }
        }
        ok &= expect(saw_catalog_object, "expected the catalog object to have been sent before the error");
        ok &= expect(captured_stderr.str().find("failed to send deferred catalog PUBLISH_DONE") != std::string::npos,
                     "expected DeferredCatalogPublishDoneGuard's destructor to run and attempt the "
                     "deferred send on this error-exit path, logging its failure since the transport "
                     "is already closed by protocol_violation() by that point");
    }

    {
        // Regression: a session driven entirely through the batch publish()
        // path never calls catalog_publisher_.publish(), so it never learns
        // which track alias is "catalog" (build_published_tracks() assigns
        // alias 0 to whichever track appears first in the plan, which need
        // not be catalog). end_broadcast() must not guess: it should send
        // only the media PUBLISH_DONE and never inject a catalog JSON
        // payload onto some other track's alias.
        MockTransport batch_transport;
        batch_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        queue_publish_ok_responses(batch_transport, DraftVersion::kDraft14, {2, 4});
        MoqtSession batch_session(batch_transport, std::string(kTestTrackNamespace), true);

        status = batch_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected batch-only session connect to succeed");
        status = batch_session.publish(
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft14), source_bytes));
        ok &= expect(status.ok, "expected batch-only publish to succeed");

        status = batch_session.end_broadcast(
            openmoq::publisher::EndBroadcastMode::kTerminate, DraftVersion::kDraft14);
        ok &= expect(status.ok, "expected end_broadcast to succeed on a batch-only session");

        bool leaked_catalog_write = false;
        for (const auto& write : batch_transport.writes) {
            std::uint64_t stream_type = 0;
            std::uint64_t track_alias = 0;
            std::uint64_t group_id = 0;
            std::uint64_t object_id_delta = 0;
            std::uint64_t payload_length = 0;
            std::vector<std::uint8_t> payload;
            if (decode_object_stream_fields(write.bytes, stream_type, track_alias, group_id, object_id_delta,
                                            payload_length, payload)) {
                const std::string text(payload.begin(), payload.end());
                if (text.find("\"isComplete\":true") != std::string::npos) {
                    leaked_catalog_write = true;
                }
            }
        }
        ok &= expect(!leaked_catalog_write,
                     "expected end_broadcast on a batch-only session to skip the unverified catalog alias");
    }

    {
        MockTransport object_live_transport;
        object_live_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        object_live_transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        object_live_transport.reads[0].push_back(
            encode_subscribe_message(1, kTestTrackNamespace, "events", 0));
        object_live_transport.reads[0].push_back({});

        std::vector<LiveObject> objects = {
            LiveObject{
                .track_name = "events",
                .group_id = 7,
                .subgroup_id = 0,
                .object_id = 3,
                .payload = {'O', 'K'},
            },
        };
        std::size_t object_index = 0;
        LiveObjectSource source{
            .tracks = {LiveTrack{.track_name = "events"}},
            .next_object = [&objects, &object_index]() -> std::optional<LiveObject> {
                if (object_index >= objects.size()) {
                    return std::nullopt;
                }
                return objects[object_index++];
            },
        };

        MoqtSession object_live_session(
            object_live_transport, std::string(kTestTrackNamespace), false, false, false, std::chrono::seconds(1));
        status = object_live_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected arbitrary live-object session connect to succeed");
        status = object_live_session.publish_live_objects(source, DraftVersion::kDraft14);
        ok &= expect(status.ok, "expected arbitrary live-object publish to succeed");
        ok &= expect(control_message_count(object_live_transport, 0x1d) == 1,
                     "expected arbitrary live-object publish to preannounce its track");
        ok &= expect(object_live_session.publish_stats().bytes_published == 2,
                     "expected arbitrary live-object bytes to be counted");
        ok &= expect(object_live_session.publish_stats().objects_published == 1,
                     "expected arbitrary live-object count to be updated");
        ok &= expect(!object_live_transport.writes.empty() &&
                         message_type(object_live_transport.writes.back().bytes) == 0x09,
                     "expected arbitrary live-object publish to finish with PUBLISH_NAMESPACE_DONE");
    }

    {
        MockTransport invalid_catalog_transport;
        invalid_catalog_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        invalid_catalog_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));

        LiveObjectSource source{
            .tracks = {
                LiveTrack{.track_name = "catalog"},
                LiveTrack{.track_name = "video"},
            },
            .next_object = []() -> std::optional<LiveObject> {
                return LiveObject{
                    .track_name = "video",
                    .group_id = 0,
                    .object_id = 0,
                    .payload = {'V'},
                };
            },
            .catalog_mode = LiveCatalogMode::kSourceObject,
        };

        MoqtSession invalid_catalog_session(
            invalid_catalog_transport,
            std::string(kTestTrackNamespace),
            true,
            false,
            false,
            std::chrono::seconds(1));
        status = invalid_catalog_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected source-catalog validation session connect to succeed");
        status = invalid_catalog_session.publish_live_objects(source, DraftVersion::kDraft16);
        ok &= expect(!status.ok, "expected source-catalog mode to reject a media object first");
        ok &= expect(status.message.find("first object") != std::string::npos,
                     "expected source-catalog ordering failure to identify the first object");
    }

    {
        MockTransport empty_catalog_transport;
        empty_catalog_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        empty_catalog_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));

        LiveObjectSource source{
            .tracks = {
                LiveTrack{.track_name = "catalog"},
                LiveTrack{.track_name = "video"},
            },
            .next_object = []() -> std::optional<LiveObject> {
                return LiveObject{
                    .track_name = "catalog",
                    .group_id = 0,
                    .object_id = 0,
                };
            },
            .catalog_mode = LiveCatalogMode::kSourceObject,
        };

        MoqtSession empty_catalog_session(
            empty_catalog_transport,
            std::string(kTestTrackNamespace),
            true,
            false,
            false,
            std::chrono::seconds(1));
        status = empty_catalog_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected empty-catalog validation session connect to succeed");
        status = empty_catalog_session.publish_live_objects(source, DraftVersion::kDraft16);
        ok &= expect(!status.ok, "expected source-catalog mode to reject an empty catalog");
        ok &= expect(status.message.find("non-empty") != std::string::npos,
                     "expected empty-catalog failure to identify the payload contract");
    }

    {
        MockTransport missing_catalog_transport;
        missing_catalog_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        missing_catalog_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));

        LiveObjectSource source{
            .tracks = {
                LiveTrack{.track_name = "catalog"},
                LiveTrack{.track_name = "video"},
            },
            .next_object = []() { return std::nullopt; },
            .catalog_mode = LiveCatalogMode::kSourceObject,
        };

        MoqtSession missing_catalog_session(
            missing_catalog_transport,
            std::string(kTestTrackNamespace),
            true,
            false,
            false,
            std::chrono::seconds(1));
        status = missing_catalog_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected missing-catalog validation session connect to succeed");
        status = missing_catalog_session.publish_live_objects(source, DraftVersion::kDraft16);
        ok &= expect(!status.ok && status.message.find("before producing") != std::string::npos,
                     "expected source EOF before its catalog to fail");
    }

    {
        MockTransport media_first_transport;
        media_first_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        media_first_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));
        media_first_transport.reads[0].push_back(
            encode_subscribe_message(
                1, kTestTrackNamespace, "video", 0, DraftVersion::kDraft16));
        media_first_transport.reads[0].push_back({});
        media_first_transport.reads[0].push_back(
            encode_subscribe_message(
                3, kTestTrackNamespace, "catalog", 0, DraftVersion::kDraft16));
        media_first_transport.reads[0].push_back({});
        std::size_t object_writes_before_catalog_subscribe = 0;
        media_first_transport.on_read = [&](MockTransport& transport,
                                            std::uint64_t stream_id) {
            if (stream_id == 0 && transport.read_count == 5) {
                object_writes_before_catalog_subscribe =
                    static_cast<std::size_t>(std::count_if(
                        transport.writes.begin(),
                        transport.writes.end(),
                        [](const auto& write) { return write.stream_id != 0; }));
            }
        };

        std::vector<LiveObject> objects = {
            LiveObject{.track_name = "catalog", .payload = {'C'}},
            LiveObject{.track_name = "video", .payload = {'V'}},
        };
        std::size_t object_index = 0;
        LiveObjectSource source{
            .tracks = {
                LiveTrack{.track_name = "catalog"},
                LiveTrack{.track_name = "video"},
            },
            .next_object = [&]() -> std::optional<LiveObject> {
                if (object_index >= objects.size()) {
                    return std::nullopt;
                }
                return objects[object_index++];
            },
            .catalog_mode = LiveCatalogMode::kSourceObject,
        };

        MoqtSession media_first_session(
            media_first_transport,
            std::string(kTestTrackNamespace),
            false,
            false,
            false,
            std::chrono::seconds(1));
        status = media_first_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected media-first subscription session connect to succeed");
        status = media_first_session.publish_live_objects(source, DraftVersion::kDraft16);
        ok &= expect(status.ok, "expected media-first subscription publish to succeed");
        ok &= expect(object_writes_before_catalog_subscribe == 1,
                     "expected only media to publish before the catalog subscription");
        ok &= expect(media_first_session.publish_stats().objects_published == 2,
                     "expected media object plus retained catalog replay for late catalog subscription");
    }

    {
        MockTransport grouped_live_transport;
        grouped_live_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        grouped_live_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));
        grouped_live_transport.reads[0].push_back(
            encode_subscribe_message(1, kTestTrackNamespace, "video", 0, DraftVersion::kDraft16));
        for (int i = 0; i < 4; ++i) {
            grouped_live_transport.reads[0].push_back({});
        }

        std::vector<LiveObject> objects = {
            LiveObject{.track_name = "video", .group_id = 0, .object_id = 0,
                       .payload = {'V', '0'}, .final_in_subgroup = false},
            LiveObject{.track_name = "video", .group_id = 0, .object_id = 1,
                       .payload = {'V', '1'}, .final_in_subgroup = false},
            LiveObject{.track_name = "video", .group_id = 1, .object_id = 0,
                       .payload = {'V', '2'}, .final_in_subgroup = false},
        };
        std::size_t object_index = 0;
        LiveObjectSource source{
            .tracks = {LiveTrack{.track_name = "video"}},
            .next_object = [&]() -> std::optional<LiveObject> {
                if (object_index >= objects.size()) {
                    return std::nullopt;
                }
                return objects[object_index++];
            },
        };

        MoqtSession grouped_live_session(
            grouped_live_transport,
            std::string(kTestTrackNamespace),
            false,
            false,
            false,
            std::chrono::seconds(1));
        status = grouped_live_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected grouped live-object session connect to succeed");
        status = grouped_live_session.publish_live_objects(source, DraftVersion::kDraft16);
        ok &= expect(status.ok, "expected grouped live-object publish to succeed");

        std::vector<MockTransport::WriteEvent> media_writes;
        for (const auto& write : grouped_live_transport.writes) {
            if (write.stream_id != 0) {
                media_writes.push_back(write);
            }
        }
        ok &= expect(media_writes.size() == 5,
                     "expected two group streams with explicit FIN writes");
        if (media_writes.size() == 5) {
            ok &= expect(media_writes[0].stream_id == media_writes[1].stream_id &&
                             !media_writes[0].fin && !media_writes[1].fin,
                         "expected same-group live objects on one open subgroup stream");
            ok &= expect(media_writes[2].stream_id == media_writes[0].stream_id &&
                             media_writes[2].bytes.empty() && media_writes[2].fin,
                         "expected previous subgroup stream to finish at the group boundary");
            ok &= expect(media_writes[3].stream_id != media_writes[0].stream_id &&
                             !media_writes[3].fin,
                         "expected next group to open a new subgroup stream");
            ok &= expect(media_writes[4].stream_id == media_writes[3].stream_id &&
                             media_writes[4].bytes.empty() && media_writes[4].fin,
                         "expected final live subgroup stream to finish at end of source");
        }
    }

    {
        MockTransport dash_live_transport;
        dash_live_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 16,
        }));
        dash_live_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));
        dash_live_transport.reads[0].push_back(
            encode_subscribe_message(
                1, kTestTrackNamespace, "catalog", 0, DraftVersion::kDraft16));
        dash_live_transport.reads[0].push_back({});
        dash_live_transport.reads[0].push_back(
            encode_subscribe_message(
                3, kTestTrackNamespace, "video0_vide_1", 0, DraftVersion::kDraft16));
        // Keep the mock control stream open while each queued source object is considered.
        for (int i = 0; i < 4; ++i) {
            dash_live_transport.reads[0].push_back({});
        }

        bool catalog_subscribe_read = false;
        bool media_subscribe_read = false;
        dash_live_transport.on_read = [&](MockTransport& transport, std::uint64_t stream_id) {
            if (stream_id == 0 && transport.read_count >= 3) {
                catalog_subscribe_read = true;
            }
            if (stream_id == 0 && transport.read_count >= 5) {
                media_subscribe_read = true;
            }
        };

        std::vector<LiveObject> objects = {
            LiveObject{.track_name = "catalog", .group_id = 0, .object_id = 0, .payload = {'C'}},
            LiveObject{.track_name = "video0_vide_1", .group_id = 0, .object_id = 0, .payload = {'V', '0'}},
            LiveObject{.track_name = "video1_vide_2", .group_id = 0, .object_id = 0, .payload = {'V', '1'}},
            LiveObject{.track_name = "video2_vide_3", .group_id = 0, .object_id = 0, .payload = {'V', '2'}},
        };
        std::size_t object_index = 0;
        bool consumed_before_subscribe = false;
        bool media_consumed_before_media_subscribe = false;
        LiveObjectSource source{
            .tracks = {
                LiveTrack{.track_name = "catalog"},
                LiveTrack{.track_name = "video0_vide_1"},
                LiveTrack{.track_name = "video1_vide_2"},
                LiveTrack{.track_name = "video2_vide_3"},
            },
            .next_object = [&]() -> std::optional<LiveObject> {
                consumed_before_subscribe = consumed_before_subscribe || !catalog_subscribe_read;
                media_consumed_before_media_subscribe =
                    media_consumed_before_media_subscribe || (object_index > 0 && !media_subscribe_read);
                if (object_index >= objects.size()) {
                    return std::nullopt;
                }
                return objects[object_index++];
            },
        };

        MoqtSession dash_live_session(
            dash_live_transport,
            std::string(kTestTrackNamespace),
            false,
            true,
            false,
            std::chrono::seconds(1));
        status = dash_live_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected FFmpeg-style DASH session connect to succeed");
        status = dash_live_session.publish_live_objects(source, DraftVersion::kDraft16);
        ok &= expect(status.ok, "expected FFmpeg-style DASH publish to enter await-subscribe mode");
        ok &= expect(catalog_subscribe_read && media_subscribe_read && !consumed_before_subscribe,
                     "expected FFmpeg-style DASH media consumption to wait for SUBSCRIBE");
        ok &= expect(!media_consumed_before_media_subscribe,
                     "expected catalog-first subscription to preserve queued media until media SUBSCRIBE");
        ok &= expect(control_message_count(dash_live_transport, 0x1d) == 4,
                     "expected catalog plus three FFmpeg-style DASH tracks to be published");
        ok &= expect(control_message_count(dash_live_transport, 0x04) == 2,
                     "expected FFmpeg-style DASH subscriber to receive SUBSCRIBE_OK");
        ok &= expect(dash_live_session.publish_stats().objects_published == 2,
                     "expected catalog and subscribed FFmpeg-style DASH representation only");
    }

    {
        MockTransport live_draft18_transport;
        live_draft18_transport.reads[3].push_back(encode_draft18_setup_response());
        live_draft18_transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));

        MoqtSession live_draft18_session(
            live_draft18_transport, std::string(kTestTrackNamespace), false, false, false, std::chrono::seconds(1));
        status = live_draft18_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 live session connect to succeed");

        const auto live_bytes = make_live_init_mp4();
        std::string live_input_bytes(live_bytes.begin(), live_bytes.end());
        std::istringstream live_input(live_input_bytes);
        status = live_draft18_session.publish_live(live_input, DraftVersion::kDraft18, false);
        ok &= expect(status.ok, "expected draft-18 live publish namespace request stream to succeed");
        ok &= expect(live_draft18_transport.writes.size() >= 2 &&
                         live_draft18_transport.writes[1].stream_id == 0 &&
                         message_type(live_draft18_transport.writes[1].bytes) == 0x06,
                     "expected draft-18 live PUBLISH_NAMESPACE on a request stream");
        ok &= expect(control_message_count(live_draft18_transport, 0x1d) == 0,
                     "expected draft-18 live publish to avoid legacy control-stream PUBLISH");
    }

    {
        MockTransport live_draft18_subscribe_tracks_transport;
        live_draft18_subscribe_tracks_transport.reads[3].push_back(encode_draft18_setup_response());
        live_draft18_subscribe_tracks_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        live_draft18_subscribe_tracks_transport.reads[1].push_back(
            encode_subscribe_tracks_message(91, kTestTrackNamespace));
        live_draft18_subscribe_tracks_transport.reads[4].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 2));
        live_draft18_subscribe_tracks_transport.reads[8].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 4));

        MoqtSession live_draft18_subscribe_tracks_session(
            live_draft18_subscribe_tracks_transport,
            std::string(kTestTrackNamespace),
            false,
            false,
            false,
            std::chrono::seconds(1));
        status = live_draft18_subscribe_tracks_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 live SUBSCRIBE_TRACKS session connect to succeed");

        const auto live_bytes = make_live_init_mp4();
        std::string live_input_bytes(live_bytes.begin(), live_bytes.end());
        std::istringstream live_input(live_input_bytes);
        status = live_draft18_subscribe_tracks_session.publish_live(live_input, DraftVersion::kDraft18, false);
        ok &= expect(status.ok, "expected draft-18 live SUBSCRIBE_TRACKS publish to succeed");

        bool saw_subscribe_tracks_ok = false;
        bool saw_catalog_publish = false;
        bool saw_media_publish = false;
        for (const auto& write : live_draft18_subscribe_tracks_transport.writes) {
            if (write.stream_id == 1 && message_type(write.bytes) == 0x07) {
                saw_subscribe_tracks_ok = true;
            }
            if (write.stream_id == 4 && message_type(write.bytes) == 0x1d) {
                saw_catalog_publish = true;
            }
            if (write.stream_id == 8 && message_type(write.bytes) == 0x1d) {
                saw_media_publish = true;
            }
        }
        ok &= expect(saw_subscribe_tracks_ok, "expected live SUBSCRIBE_TRACKS response stream REQUEST_OK");
        ok &= expect(saw_catalog_publish && saw_media_publish,
                     "expected live SUBSCRIBE_TRACKS to PUBLISH catalog and media tracks");
    }

    {
        // Regression: draft-16 SUBSCRIBE wire bytes captured from the Akamai
        // test relay. Namespace=["moqxr"], track="catalog", one parameter:
        // SUBSCRIPTION_FILTER (0x21) with filter type Largest Object (0x02).
        const std::vector<std::uint8_t> akamai_subscribe = {
            0x03, 0x00, 0x14, 0x01, 0x01, 0x05, 0x6d, 0x6f, 0x71, 0x78, 0x72,
            0x07, 0x63, 0x61, 0x74, 0x61, 0x6c, 0x6f, 0x67, 0x01, 0x21, 0x01, 0x02,
        };
        SubscribeMessage draft16_msg;
        ok &= expect(openmoq::publisher::transport::decode_subscribe_message(
                         akamai_subscribe, DraftVersion::kDraft16, draft16_msg),
                     "expected draft-16 SUBSCRIBE from Akamai relay to decode");
        ok &= expect(draft16_msg.request_id == 1, "expected request_id=1");
        ok &= expect(draft16_msg.track_namespace.size() == 1 &&
                         draft16_msg.track_namespace.front() == "moqxr",
                     "expected namespace [\"moqxr\"]");
        ok &= expect(draft16_msg.track_name == "catalog", "expected track_name=catalog");
        ok &= expect(draft16_msg.filter_type == 0x02, "expected filter_type=Largest Object (0x02)");
        ok &= expect(draft16_msg.subscriber_priority == 128, "expected default subscriber_priority=128");
        ok &= expect(draft16_msg.forward == 1, "expected default forward=1");

        // And confirm that parsing the same bytes as draft-14 still fails, so
        // nothing is silently cross-wired.
        SubscribeMessage draft14_msg;
        ok &= expect(!openmoq::publisher::transport::decode_subscribe_message(
                         akamai_subscribe, DraftVersion::kDraft14, draft14_msg),
                     "expected draft-14 decoder to reject draft-16 SUBSCRIBE bytes");

        // Spec §9.2: unknown Message Parameter MUST cause PROTOCOL_VIOLATION.
        // Parameter type 0x40 is not defined for SUBSCRIBE in draft-16 — the
        // decoder should reject it rather than silently skip. Built by
        // replacing the SUBSCRIPTION_FILTER parameter with an unknown even
        // parameter (0x40) with a varint value.
        const std::vector<std::uint8_t> unknown_param_subscribe = {
            0x03, 0x00, 0x14, 0x01, 0x01, 0x05, 0x6d, 0x6f, 0x71, 0x78, 0x72,
            0x07, 0x63, 0x61, 0x74, 0x61, 0x6c, 0x6f, 0x67, 0x01, 0x40, 0x40, 0x00,
        };
        SubscribeMessage unknown_msg;
        ok &= expect(!openmoq::publisher::transport::decode_subscribe_message(
                         unknown_param_subscribe, DraftVersion::kDraft16, unknown_msg),
                     "expected unknown draft-16 Message Parameter to be rejected");

        // Spec §9.2.2.4: GROUP_ORDER=0 is not a legal wire value.
        // Message: same namespace/track, 1 parameter: GROUP_ORDER (0x22) with
        // value 0. Length: req(1)+tuple(1)+"moqxr"(6)+"catalog"(8)+nparams(1)+
        // delta(1)+value(1) = 19 = 0x13.
        const std::vector<std::uint8_t> group_order_zero_subscribe = {
            0x03, 0x00, 0x13, 0x01, 0x01, 0x05, 0x6d, 0x6f, 0x71, 0x78, 0x72,
            0x07, 0x63, 0x61, 0x74, 0x61, 0x6c, 0x6f, 0x67, 0x01, 0x22, 0x00,
        };
        SubscribeMessage group_order_zero_msg;
        ok &= expect(!openmoq::publisher::transport::decode_subscribe_message(
                         group_order_zero_subscribe, DraftVersion::kDraft16, group_order_zero_msg),
                     "expected GROUP_ORDER=0 wire value to be rejected");

        // Spec §9.2.2.2: DELIVERY_TIMEOUT=0 is not a legal wire value.
        // Message: same namespace/track, 1 parameter: DELIVERY_TIMEOUT (0x02)
        // with value 0. Length: req(1)+tuple(1)+"moqxr"(6)+"catalog"(8)+
        // nparams(1)+delta(1)+value(1) = 19 = 0x13.
        const std::vector<std::uint8_t> delivery_timeout_zero_subscribe = {
            0x03, 0x00, 0x13, 0x01, 0x01, 0x05, 0x6d, 0x6f, 0x71, 0x78, 0x72,
            0x07, 0x63, 0x61, 0x74, 0x61, 0x6c, 0x6f, 0x67, 0x01, 0x02, 0x00,
        };
        SubscribeMessage delivery_timeout_zero_msg;
        ok &= expect(!openmoq::publisher::transport::decode_subscribe_message(
                         delivery_timeout_zero_subscribe, DraftVersion::kDraft16, delivery_timeout_zero_msg),
                     "expected DELIVERY_TIMEOUT=0 wire value to be rejected");
    }

    {
        // Spec §2.2: objects from the same subgroup MUST NOT be sent on
        // different streams. Serve a plan where vide_1 carries three objects
        // in group 0, subgroup 0 and verify the publisher opens a single
        // unidirectional stream and appends the objects onto it with
        // delta-encoded Object IDs.
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        transport.reads[0].push_back(encode_subscribe_message(1, kTestTrackNamespace, "vide_1", 0));
        MoqtSession session(transport, std::string(kTestTrackNamespace));
        status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected multi-object-subgroup session connect to succeed");
        status = session.publish(make_multi_object_subgroup_plan());
        ok &= expect(status.ok, "expected multi-object-subgroup publish to succeed");

        // Extract the unidirectional-stream writes (stream_id >= 2 for client
        // uni, and the writes are ordered in open-sequence). There should be
        // exactly three writes on the same stream_id.
        std::vector<std::size_t> vide_1_stream_writes;
        std::uint64_t first_uni_stream_id = std::numeric_limits<std::uint64_t>::max();
        for (std::size_t index = 0; index < transport.writes.size(); ++index) {
            const auto& write = transport.writes[index];
            if (write.stream_id == 0) {
                continue;  // control stream
            }
            if (first_uni_stream_id == std::numeric_limits<std::uint64_t>::max()) {
                first_uni_stream_id = write.stream_id;
            }
            if (write.stream_id == first_uni_stream_id) {
                vide_1_stream_writes.push_back(index);
            }
        }

        ok &= expect(vide_1_stream_writes.size() == 3,
                     "expected three writes on the single vide_1 subgroup stream");

        if (vide_1_stream_writes.size() == 3) {
            ok &= expect(!transport.writes[vide_1_stream_writes[0]].fin,
                         "expected first write to not FIN the subgroup stream");
            ok &= expect(!transport.writes[vide_1_stream_writes[1]].fin,
                         "expected second write to not FIN the subgroup stream");
            ok &= expect(transport.writes[vide_1_stream_writes[2]].fin,
                         "expected final write to FIN the subgroup stream");

            // First write: draft-14 subgroup header (type=0x18 end-of-group set, mode=0) +
            // first object {Object ID Delta=0, payload_len=2, payload="V0"}.
            const auto& w0 = transport.writes[vide_1_stream_writes[0]].bytes;
            ok &= expect(!w0.empty() && w0.front() == 0x18,
                         "expected first write to begin with draft-14 subgroup header | END_OF_GROUP");

            // Second and third writes: no header, just {Object ID Delta, payload_len, payload}.
            // Delta for sequential object IDs 0 -> 1 -> 2 is 0 each time.
            const auto& w1 = transport.writes[vide_1_stream_writes[1]].bytes;
            ok &= expect(w1 == std::vector<std::uint8_t>{0x00, 0x02, 'V', '1'},
                         "expected second write to be Object ID Delta=0, payload_len=2, 'V1'");
            const auto& w2 = transport.writes[vide_1_stream_writes[2]].bytes;
            ok &= expect(w2 == std::vector<std::uint8_t>{0x00, 0x02, 'V', '2'},
                         "expected third write to be Object ID Delta=0, payload_len=2, 'V2'");
        }

        // PUBLISH_DONE stream_count should be 1 (one subgroup stream opened),
        // not 3 (one per object). Per spec §9.14 the stream_count field counts
        // data streams, not objects.
        std::optional<std::vector<std::uint8_t>> publish_done_bytes;
        for (const auto& write : transport.writes) {
            if (!write.bytes.empty() && write.bytes.front() == 0x0b) {
                publish_done_bytes = write.bytes;
                break;
            }
        }
        ok &= expect(publish_done_bytes.has_value(), "expected PUBLISH_DONE to be sent");
        if (publish_done_bytes.has_value()) {
            // PUBLISH_DONE payload: type(0x0b) + len(uint16 BE) +
            // request_id(i) + status(i=0x02 Track_Ended) + stream_count(i) +
            // reason_len(i=0). For request_id=1 and stream_count=1 the full
            // bytes are [0x0b, 0x00, 0x04, 0x01, 0x02, 0x01, 0x00].
            const std::vector<std::uint8_t> expected = {0x0b, 0x00, 0x04, 0x01, 0x02, 0x01, 0x00};
            ok &= expect(*publish_done_bytes == expected,
                         "expected PUBLISH_DONE.stream_count = 1 for a single-subgroup delivery");
        }
    }

    {
        MockTransport transport;
        transport.state_ = ConnectionState::kConnected;
        const TransportStatus rst = transport.reset_stream(42, 0);
        ok &= expect(rst.ok, "mock reset_stream should succeed");
        ok &= expect(transport.reset_calls.size() == 1, "expected one reset_stream call");
        ok &= expect(transport.reset_calls[0].first == 42, "expected stream_id 42");
        ok &= expect(transport.reset_calls[0].second == 0, "expected error_code 0");
    }

    {
        // Stop-hang regression: close() from another thread must make a running
        // publish_live_objects loop return promptly. Against pre-fix code this
        // hangs forever; the future wait_for bounds the failure.
        // auto_forward=true drives objects without needing a subscriber. The
        // draft-14 setup writes PUBLISH_NAMESPACE then collects 1 namespace ack,
        // so queue server setup + namespace_ok. After that, with no further
        // queued reads, the mock returns a benign "timed out" for stream 0, so
        // the loop reaches next_object() every iteration and spins endlessly;
        // only a stop (close) can end it.
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        MoqtSession session(transport, std::string(kTestTrackNamespace),
                            /*auto_forward=*/true, /*publish_catalog=*/false,
                            /*paced=*/false, std::chrono::seconds(1));

        auto connect_status = session.connect(endpoint, tls);
        ok &= expect(connect_status.ok, "expected stop-cancel session connect to succeed");

        LiveObjectSource source;
        source.tracks = {LiveTrack{.track_name = "events"}};
        std::atomic<int> object_calls{0};
        source.next_object = [&object_calls]() -> std::optional<LiveObject> {
            // Endless objects so the loop never reaches natural EOF; only a
            // stop (close) can end it.
            const int n = object_calls.fetch_add(1);
            return LiveObject{.track_name = "events", .group_id = 1,
                              .object_id = static_cast<std::size_t>(n),
                              .payload = {'O', 'K'}};
        };

        std::promise<TransportStatus> result_promise;
        auto result_future = result_promise.get_future();
        std::thread worker([&]() {
            result_promise.set_value(session.publish_live_objects(source, DraftVersion::kDraft14));
        });
        // Let the loop spin a few iterations, then cancel from this thread.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        session.close(0);

        const auto wait_status = result_future.wait_for(std::chrono::seconds(3));
        ok &= expect(wait_status == std::future_status::ready,
                     "expected publish_live_objects to return within 3s after close()");
        if (wait_status == std::future_status::ready) {
            worker.join();
            ok &= expect(object_calls.load() > 0,
                         "expected the loop to actually serve objects before stop");
            ok &= expect(result_future.get().ok,
                         "expected stop-initiated publish_live_objects to return success");
        } else {
            worker.detach();  // broken build: leak rather than hang the whole suite
        }
    }

    {
        // Flush fallback: if post-stop flush writes block, publish_live_objects
        // must still return promptly. close() tears down the transport, which
        // makes the blocked write fail; the stop-swallow path returns success.
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        MoqtSession session(transport, std::string(kTestTrackNamespace),
                            /*auto_forward=*/true, /*publish_catalog=*/false,
                            /*paced=*/false, std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected flush-fallback session connect to succeed");

        LiveObjectSource source;
        source.tracks = {LiveTrack{.track_name = "events"}};
        std::atomic<int> object_calls{0};
        source.next_object = [&object_calls]() -> std::optional<LiveObject> {
            const int n = object_calls.fetch_add(1);
            return LiveObject{.track_name = "events", .group_id = 1,
                              .object_id = static_cast<std::size_t>(n),
                              .payload = {'O', 'K'}};
        };

        std::promise<TransportStatus> result_promise;
        auto result_future = result_promise.get_future();
        std::thread worker([&]() {
            result_promise.set_value(session.publish_live_objects(source, DraftVersion::kDraft14));
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        transport.block_writes.store(true);  // any flush write now blocks
        session.close(0);                     // tears down transport -> unblocks via failure

        const auto wait_status = result_future.wait_for(std::chrono::seconds(4));
        ok &= expect(wait_status == std::future_status::ready,
                     "expected publish_live_objects to return promptly despite blocked flush");
        transport.release_writes.store(true);  // belt-and-suspenders unblock
        if (wait_status == std::future_status::ready) {
            worker.join();
            ok &= expect(result_future.get().ok,
                         "expected stop with blocked flush to still return success");
        } else {
            worker.detach();
        }
    }

    {
        // Graceful flush: with a healthy transport, a stop still returns success
        // and the loop served at least one object.
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        MoqtSession session(transport, std::string(kTestTrackNamespace),
                            /*auto_forward=*/true, /*publish_catalog=*/false,
                            /*paced=*/false, std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected graceful-flush session connect to succeed");

        LiveObjectSource source;
        source.tracks = {LiveTrack{.track_name = "events"}};
        std::atomic<int> object_calls{0};
        source.next_object = [&object_calls]() -> std::optional<LiveObject> {
            const int n = object_calls.fetch_add(1);
            return LiveObject{.track_name = "events", .group_id = 1,
                              .object_id = static_cast<std::size_t>(n),
                              .payload = {'O', 'K'}};
        };

        std::promise<TransportStatus> result_promise;
        auto result_future = result_promise.get_future();
        std::thread worker([&]() {
            result_promise.set_value(session.publish_live_objects(source, DraftVersion::kDraft14));
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        session.close(0);
        const auto wait_status = result_future.wait_for(std::chrono::seconds(3));
        ok &= expect(wait_status == std::future_status::ready,
                     "expected graceful-flush publish to return after close()");
        if (wait_status == std::future_status::ready) {
            worker.join();
            ok &= expect(result_future.get().ok, "expected graceful-flush return success");
            ok &= expect(object_calls.load() > 0,
                         "expected graceful flush after serving objects");
        } else {
            worker.detach();
        }
    }

    {
        // No false positive: a transport error WITHOUT a stop request must still
        // return failure. next_object returns an object for an unknown track,
        // which the loop rejects with failure at runtime; close() is never called.
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        MoqtSession session(transport, std::string(kTestTrackNamespace),
                            /*auto_forward=*/true, /*publish_catalog=*/false,
                            /*paced=*/false, std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected no-false-positive session connect to succeed");

        LiveObjectSource source;
        source.tracks = {LiveTrack{.track_name = "events"}};
        source.next_object = []() -> std::optional<LiveObject> {
            return LiveObject{.track_name = "not_a_declared_track", .group_id = 1,
                              .object_id = 0, .payload = {'X'}};
        };

        const TransportStatus result = session.publish_live_objects(source, DraftVersion::kDraft14);
        ok &= expect(!result.ok,
                     "expected an unknown-track error without stop to return failure");
    }

    return ok ? 0 : 1;
}
