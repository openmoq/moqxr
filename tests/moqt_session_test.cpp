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
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace openmoq::publisher::transport::priority_scheduler_internal {

void set_publisher_priority_for_testing(std::uint64_t request_id,
                                        std::uint8_t priority);
void clear_publisher_priorities_for_testing();
std::uint8_t object_transport_priority_for_testing(
    std::uint8_t subscriber_priority,
    std::uint8_t publisher_priority);
void begin_priority_comparison_count_for_testing();
std::uint64_t end_priority_comparison_count_for_testing();

}  // namespace openmoq::publisher::transport::priority_scheduler_internal

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
using openmoq::publisher::transport::ObjectWriteDisposition;
using openmoq::publisher::transport::ObjectWriteOptions;
using openmoq::publisher::transport::ObjectWriteResult;
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
        std::size_t reset_count_before_write = 0;
    };
    struct OpenEvent {
        StreamDirection direction = StreamDirection::kBidirectional;
        std::uint64_t stream_id = 0;
    };
    struct ObjectWriteEvent {
        std::uint64_t stream_id = 0;
        std::vector<std::uint8_t> bytes;
        bool fin = false;
        ObjectWriteOptions options;
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
            .reset_count_before_write = reset_calls.size(),
        });
        return TransportStatus::success();
    }

    TransportStatus set_reliable_stream_priority(std::uint64_t stream_id,
                                                 std::uint8_t priority) override {
        reliable_stream_priorities.emplace_back(stream_id, priority);
        return TransportStatus::success();
    }

    ObjectWriteResult try_write_object(std::uint64_t stream_id,
                                       std::span<const std::uint8_t> bytes,
                                       bool fin,
                                       ObjectWriteOptions options) override {
        ObjectWriteEvent event{
            .stream_id = stream_id,
            .bytes = std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
            .fin = fin,
            .options = std::move(options),
        };
        object_write_attempts.push_back(event);
        ObjectWriteResult result{ObjectWriteDisposition::kAccepted, {}};
        if (on_try_write_object) {
            result = on_try_write_object(*this, event);
        }
        if (result.disposition != ObjectWriteDisposition::kAccepted) {
            return result;
        }
        const TransportStatus status = write_stream(stream_id, bytes, fin);
        return {
            .disposition = status.ok ? ObjectWriteDisposition::kAccepted
                                     : ObjectWriteDisposition::kFailed,
            .message = std::move(status.message),
        };
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
        fin = it->second.empty() && !keep_open_streams.contains(stream_id);
        if (it->second.empty()) {
            reads.erase(it);
        }
        return TransportStatus::success();
    }

    TransportStatus close(std::uint64_t application_error_code) override {
        last_close_code = application_error_code;
        state_ = ConnectionState::kClosed;
        return TransportStatus::success();
    }

    void note_delivery_timeout(std::chrono::milliseconds timeout) override {
        delivery_timeouts.push_back(timeout);
    }

    bool media_stream_expired(std::uint64_t stream_id) const override {
        return expired_media_streams.contains(stream_id);
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
    std::vector<ObjectWriteEvent> object_write_attempts;
    std::vector<std::pair<std::uint64_t, std::uint8_t>> reliable_stream_priorities;
    std::vector<std::chrono::milliseconds> read_timeouts;
    std::vector<std::chrono::milliseconds> delivery_timeouts;
    std::set<std::uint64_t> expired_media_streams;
    std::map<std::uint64_t, std::vector<std::vector<std::uint8_t>>> reads;
    std::set<std::uint64_t> keep_open_streams;
    std::set<std::uint64_t> accepted_streams;
    std::function<void(MockTransport&, StreamDirection)> on_accept_timeout;
    std::function<void(MockTransport&, std::uint64_t)> on_read;
    std::function<ObjectWriteResult(MockTransport&, const ObjectWriteEvent&)>
        on_try_write_object;
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
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x5d, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});  // timescale 24000 at bytes 8-11
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

// One CMAF media fragment (moof + 1-byte mdat) for track 1 of make_live_init_mp4(),
// so a live publish actually pushes an object. Box layout mirrors
// tests/live_dash_ingest_test.cpp's make_media_fragment().
std::vector<std::uint8_t> make_live_media_fragment(std::uint32_t decode_time, std::uint8_t payload_byte) {
    auto be32 = [](std::uint32_t value) {
        std::vector<std::uint8_t> out;
        append_be32(out, value);
        return out;
    };
    auto flagged_full_box = [&](std::string_view type, std::uint32_t version_flags, std::vector<std::uint8_t> payload) {
        return make_box(type, concat({be32(version_flags), std::move(payload)}));
    };
    const auto tfhd = flagged_full_box("tfhd", 0x000038, concat({be32(1), be32(1000), be32(1), be32(0x02000000)}));
    const auto tfdt = flagged_full_box("tfdt", 0, be32(decode_time));
    const auto trun = flagged_full_box("trun", 0x000201, concat({be32(1), be32(16), be32(1)}));
    const auto traf = make_box("traf", concat({tfhd, tfdt, trun}));
    const auto moof = make_box("moof", traf);
    const auto mdat = make_box("mdat", {payload_byte});
    return concat({moof, mdat});
}

// CENC fixture builders, ported from tests/cmaf_segmenter_test.cpp (each test
// binary owns its own fixtures in this codebase). Byte layouts must match what
// parse_track_protection and parse_pssh_boxes actually read -- see that file's
// make_frma/make_schm/make_tenc_box/make_sinf for the authoritative version.
std::vector<std::uint8_t> make_frma(const std::string& original_format) {
    return make_box("frma", std::vector<std::uint8_t>(original_format.begin(), original_format.end()));
}

std::vector<std::uint8_t> make_schm(const std::string& scheme_type) {
    std::vector<std::uint8_t> payload(scheme_type.begin(), scheme_type.end());
    append_be32(payload, 0x00010000);
    return make_full_box("schm", payload);
}

std::vector<std::uint8_t> make_tenc_box(std::uint8_t is_protected, std::uint8_t iv_size) {
    std::vector<std::uint8_t> payload{0, 0, is_protected, iv_size};
    const std::vector<std::uint8_t> kid{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    payload.insert(payload.end(), kid.begin(), kid.end());
    return make_full_box("tenc", payload);
}

std::vector<std::uint8_t> make_sinf(const std::string& original_format, const std::string& scheme_type) {
    const auto schi = make_box("schi", make_tenc_box(1, 8));
    return make_box("sinf", concat({make_frma(original_format), make_schm(scheme_type), schi}));
}

// A live init segment (ftyp + moov) describing a single CENC-protected video
// track (encv + sinf/schm/schi/tenc) with no pssh box anywhere -- the CMSF
// 4.1.2 refusal fixture for the stdin live path's build_live_catalog guard.
// Mirrors make_live_init_mp4() above but swaps the avc1 sample entry for an
// encrypted encv one and omits the pssh box that make_encrypted_fragmented_
// test_mp4(false) would otherwise place under moov (not needed here since
// this fixture carries no pssh at all).
std::vector<std::uint8_t> make_live_init_mp4_cenc_no_pssh() {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = make_full_box("tkhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto mdhd = make_full_box("mdhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x5d, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});  // timescale 24000 at bytes 8-11
    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    auto visual_header = std::vector<std::uint8_t>(78, 0);
    visual_header[24] = 0x01;
    visual_header[25] = 0x40;
    visual_header[26] = 0x00;
    visual_header[27] = 0xf0;
    const auto avcc = make_box("avcC", {1, 100, 0, 12, 0xff});
    const auto sinf = make_sinf("avc1", "cenc");
    const auto sample_entry = make_box("encv", concat({visual_header, avcc, sinf}));
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
                                                   DraftVersion draft = DraftVersion::kDraft14,
                                                   std::uint64_t delivery_timeout_ms = 0,
                                                   std::uint64_t subgroup_delivery_timeout_ms = 0,
                                                   std::uint8_t subscriber_priority = 128,
                                                   std::uint8_t group_order = 0,
                                                   std::size_t start_group_id = 0,
                                                   std::size_t start_object_id = 0,
                                                   std::uint64_t filter_type_value = 0x03,
                                                   std::size_t end_group_id = 0) {
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
        payload.push_back(subscriber_priority);
        payload.push_back(group_order == 0 ? 0x01 : group_order);
        payload.push_back(forward);  // forward
        const std::vector<std::uint8_t> filter_type =
            encode_varint(filter_type_value);
        const std::vector<std::uint8_t> start_group = encode_varint(start_group_id);
        const std::vector<std::uint8_t> start_object = encode_varint(start_object_id);
        const std::vector<std::uint8_t> parameter_count = encode_varint(0);
        payload.insert(payload.end(), filter_type.begin(), filter_type.end());
        payload.insert(payload.end(), start_group.begin(), start_group.end());
        payload.insert(payload.end(), start_object.begin(), start_object.end());
        if (filter_type_value == 0x04) {
            const std::vector<std::uint8_t> end_group =
                encode_varint(end_group_id);
            payload.insert(payload.end(), end_group.begin(), end_group.end());
        }
        payload.insert(payload.end(), parameter_count.begin(), parameter_count.end());
    } else {
        // Draft-16/18: parameters as delta-encoded KVPs. Draft 16 has the
        // single DELIVERY_TIMEOUT (0x02); draft 18 independently adds
        // SUBGROUP_DELIVERY_TIMEOUT (0x06).
        const std::uint64_t timeout_parameter_count =
            (delivery_timeout_ms != 0 ? 1 : 0) +
            (draft == DraftVersion::kDraft18 && subgroup_delivery_timeout_ms != 0 ? 1 : 0);
        const std::vector<std::uint8_t> parameter_count =
            encode_moqint(draft, 3 + timeout_parameter_count + (group_order != 0 ? 1 : 0));
        payload.insert(payload.end(), parameter_count.begin(), parameter_count.end());
        std::uint64_t previous_type = 0;
        if (delivery_timeout_ms != 0) {
            const std::vector<std::uint8_t> timeout_delta = encode_moqint(draft, 0x02);
            const std::vector<std::uint8_t> timeout_value = encode_moqint(draft, delivery_timeout_ms);
            payload.insert(payload.end(), timeout_delta.begin(), timeout_delta.end());
            payload.insert(payload.end(), timeout_value.begin(), timeout_value.end());
            previous_type = 0x02;
        }
        if (draft == DraftVersion::kDraft18 && subgroup_delivery_timeout_ms != 0) {
            const std::vector<std::uint8_t> timeout_delta =
                encode_moqint(draft, 0x06 - previous_type);
            const std::vector<std::uint8_t> timeout_value =
                encode_moqint(draft, subgroup_delivery_timeout_ms);
            payload.insert(payload.end(), timeout_delta.begin(), timeout_delta.end());
            payload.insert(payload.end(), timeout_value.begin(), timeout_value.end());
            previous_type = 0x06;
        }
        // FORWARD (0x10, even) delta from previous type, value=forward
        const std::vector<std::uint8_t> forward_delta = encode_moqint(draft, 0x10 - previous_type);
        const std::vector<std::uint8_t> forward_value = encode_moqint(draft, forward);
        payload.insert(payload.end(), forward_delta.begin(), forward_delta.end());
        payload.insert(payload.end(), forward_value.begin(), forward_value.end());
        // SUBSCRIBER_PRIORITY (0x20, even) delta=0x10. draft-17/18 encode the
        // value as a single uint8 byte; earlier drafts use a varint.
        const std::vector<std::uint8_t> priority_delta = encode_moqint(draft, 0x20 - 0x10);
        const bool uint8_params =
            draft == DraftVersion::kDraft17 || draft == DraftVersion::kDraft18;
        const std::vector<std::uint8_t> priority_value =
            uint8_params ? std::vector<std::uint8_t>{subscriber_priority}
                         : encode_moqint(draft, subscriber_priority);
        payload.insert(payload.end(), priority_delta.begin(), priority_delta.end());
        payload.insert(payload.end(), priority_value.begin(), priority_value.end());
        // SUBSCRIPTION_FILTER (0x21, odd) delta=0x01
        // Value: filter type followed by its applicable locations.
        const std::vector<std::uint8_t> filter_delta = encode_moqint(draft, 0x21 - 0x20);
        std::vector<std::uint8_t> filter_value;
        const std::vector<std::uint8_t> ft =
            encode_moqint(draft, filter_type_value);
        const std::vector<std::uint8_t> sg = encode_moqint(draft, start_group_id);
        const std::vector<std::uint8_t> so = encode_moqint(draft, start_object_id);
        filter_value.insert(filter_value.end(), ft.begin(), ft.end());
        filter_value.insert(filter_value.end(), sg.begin(), sg.end());
        filter_value.insert(filter_value.end(), so.begin(), so.end());
        if (filter_type_value == 0x04) {
            append_bytes(filter_value, encode_moqint(draft, end_group_id));
        }
        const std::vector<std::uint8_t> filter_len = encode_moqint(draft, filter_value.size());
        payload.insert(payload.end(), filter_delta.begin(), filter_delta.end());
        payload.insert(payload.end(), filter_len.begin(), filter_len.end());
        payload.insert(payload.end(), filter_value.begin(), filter_value.end());
        if (group_order != 0) {
            const std::vector<std::uint8_t> group_order_delta = encode_moqint(draft, 0x22 - 0x21);
            const std::vector<std::uint8_t> group_order_value =
                uint8_params ? std::vector<std::uint8_t>{group_order}
                             : encode_moqint(draft, group_order);
            payload.insert(payload.end(), group_order_delta.begin(), group_order_delta.end());
            payload.insert(payload.end(), group_order_value.begin(), group_order_value.end());
        }
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

std::vector<std::uint8_t> encode_archived_subscribe_update_message(
    std::uint64_t request_id,
    std::uint64_t subscription_request_id = 6,
    std::uint8_t subscriber_priority = 128,
    std::uint8_t forward = 1,
    std::size_t end_group_plus_one_value = 1) {
    std::vector<std::uint8_t> payload = encode_varint(request_id);
    const std::vector<std::uint8_t> subscription_request_id_bytes = encode_varint(subscription_request_id);
    const std::vector<std::uint8_t> start_group = encode_varint(0);
    const std::vector<std::uint8_t> start_object = encode_varint(0);
    const std::vector<std::uint8_t> end_group_plus_one =
        encode_varint(end_group_plus_one_value);
    const std::vector<std::uint8_t> parameter_count = encode_varint(0);
    payload.insert(payload.end(), subscription_request_id_bytes.begin(), subscription_request_id_bytes.end());
    payload.insert(payload.end(), start_group.begin(), start_group.end());
    payload.insert(payload.end(), start_object.begin(), start_object.end());
    payload.insert(payload.end(), end_group_plus_one.begin(), end_group_plus_one.end());
    payload.push_back(subscriber_priority);
    payload.push_back(forward);
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

std::vector<std::uint8_t> encode_request_update_message(DraftVersion draft,
                                                        std::uint64_t request_id,
                                                        std::uint8_t value,
                                                        std::uint64_t parameter_type = 0x10,
                                                        std::optional<std::uint64_t> existing_request_id = std::nullopt) {
    std::vector<std::uint8_t> payload = encode_moqint(draft, request_id);
    if (draft == DraftVersion::kDraft16) {
        append_bytes(payload, encode_moqint(draft, existing_request_id.value_or(0)));
    }
    append_bytes(payload, encode_moqint(draft, 1));
    append_bytes(payload, encode_moqint(draft, parameter_type));
    if (draft == DraftVersion::kDraft18 &&
        (parameter_type == 0x10 || parameter_type == 0x20 || parameter_type == 0x22)) {
        payload.push_back(value);
    } else {
        append_bytes(payload, encode_moqint(draft, value));
    }

    std::vector<std::uint8_t> message = encode_moqint(draft, 0x02);
    append_be16(message, static_cast<std::uint16_t>(payload.size()));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

std::vector<std::uint8_t> encode_request_update_filter_message(
    DraftVersion draft,
    std::uint64_t request_id,
    std::optional<std::uint64_t> existing_request_id,
    std::uint64_t filter_type,
    std::size_t start_group_id,
    std::size_t start_object_id,
    std::size_t end_group_id = 0) {
    std::vector<std::uint8_t> payload = encode_moqint(draft, request_id);
    if (draft == DraftVersion::kDraft16) {
        append_bytes(payload, encode_moqint(draft, existing_request_id.value_or(0)));
    }
    append_bytes(payload, encode_moqint(draft, 1));
    append_bytes(payload, encode_moqint(draft, 0x21));
    std::vector<std::uint8_t> filter = encode_moqint(draft, filter_type);
    if (filter_type == 0x03 || filter_type == 0x04) {
        append_bytes(filter, encode_moqint(draft, start_group_id));
        append_bytes(filter, encode_moqint(draft, start_object_id));
        if (filter_type == 0x04) {
            append_bytes(filter, encode_moqint(draft, end_group_id));
        }
    }
    append_bytes(payload, encode_moqint(draft, filter.size()));
    payload.insert(payload.end(), filter.begin(), filter.end());

    std::vector<std::uint8_t> message = encode_moqint(draft, 0x02);
    append_be16(message, static_cast<std::uint16_t>(payload.size()));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

std::vector<std::uint8_t> encode_request_update_authorization_message(
    DraftVersion draft,
    std::uint64_t request_id,
    std::optional<std::uint64_t> existing_request_id) {
    std::vector<std::uint8_t> payload = encode_moqint(draft, request_id);
    if (draft == DraftVersion::kDraft16) {
        append_bytes(payload,
                     encode_moqint(draft, existing_request_id.value_or(0)));
    }
    append_bytes(payload, encode_moqint(draft, 2));
    std::uint64_t previous_type = 0;
    for (const std::uint8_t value : {'a', 'b'}) {
        append_bytes(payload, encode_moqint(draft, 0x03 - previous_type));
        std::vector<std::uint8_t> token = encode_moqint(draft, 0x03);
        append_bytes(token, encode_moqint(draft, 0));
        token.push_back(value);
        append_bytes(payload, encode_moqint(draft, token.size()));
        payload.insert(payload.end(), token.begin(), token.end());
        previous_type = 0x03;
    }

    std::vector<std::uint8_t> message = encode_moqint(draft, 0x02);
    append_be16(message, static_cast<std::uint16_t>(payload.size()));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

std::vector<std::uint8_t> encode_malformed_request_update_parameter(
    DraftVersion draft,
    std::uint64_t request_id,
    std::optional<std::uint64_t> existing_request_id,
    std::uint64_t parameter_type) {
    std::vector<std::uint8_t> payload = encode_moqint(draft, request_id);
    if (draft == DraftVersion::kDraft16) {
        append_bytes(payload,
                     encode_moqint(draft, existing_request_id.value_or(0)));
    }
    append_bytes(payload, encode_moqint(draft, 1));
    append_bytes(payload, encode_moqint(draft, parameter_type));
    append_bytes(payload, encode_moqint(draft, 1));
    // REGISTER lacks its required alias/type; a range filter lacks all
    // required locations. Both are malformed known key/value encodings.
    append_bytes(payload,
                 encode_moqint(draft,
                               parameter_type == 0x03 ? 0x01 : 0x03));

    std::vector<std::uint8_t> message = encode_moqint(draft, 0x02);
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
    std::vector<std::uint8_t> payload;
    if (draft != DraftVersion::kDraft18) {
        payload = encode_varint(request_id);
    }
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

    std::vector<std::uint8_t> message = encode_moqint(draft, draft == DraftVersion::kDraft18 ? 0x07 : 0x1e);
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

// True for a write that opens an object data stream: a SUBGROUP_HEADER type
// (draft-14 0x10-0x1D, draft-16 0x30-0x3D, draft-18 FIRST_OBJECT 0x50-0x7D).
// Draft-17+ also puts the control stream on a unidirectional stream, so
// "any unidirectional write" is not a usable definition of data.
bool is_data_stream_write(const MockTransport::WriteEvent& write) {
    if ((write.stream_id & 0x3ULL) != 0x2ULL) {
        return false;
    }
    const auto type = message_type(write.bytes);
    if (!type) {
        return false;
    }
    return (*type >= 0x10 && *type <= 0x1d) || (*type >= 0x30 && *type <= 0x3d) ||
           (*type >= 0x50 && *type <= 0x7d);
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

PublishPlan make_delivery_deadline_plan(DraftVersion draft,
                                        bool include_later_object = true) {
    PublishPlan plan{
        .draft = openmoq::publisher::draft_profile(draft),
        .tracks = {
            TrackDescription{.track_id = 1,
                             .handler_type = "vide",
                             .codec = "avc1.64000C",
                             .sample_entry_type = "avc1",
                             .track_name = "events"},
        },
        .objects = {
            CmsfObject{
                .kind = CmsfObjectKind::kMedia,
                .track_name = "events",
                .group_id = 7,
                .subgroup_id = 3,
                .object_id = 0,
                .media_time_us = 0,
                .owned_payload = {'A'},
            },
        },
    };
    if (include_later_object) {
        plan.objects.push_back(CmsfObject{
            .kind = CmsfObjectKind::kMedia,
            .track_name = "events",
            .group_id = 7,
            .subgroup_id = 3,
            .object_id = 1,
            .media_time_us = 1000,
            .owned_payload = {'B'},
        });
    }
    return plan;
}

PublishPlan make_transport_expiry_plan(DraftVersion draft) {
    return {
        .draft = openmoq::publisher::draft_profile(draft),
        .tracks = {
            TrackDescription{.track_id = 1,
                             .handler_type = "vide",
                             .codec = "avc1.64000C",
                             .sample_entry_type = "avc1",
                             .track_name = "events"},
        },
        .objects = {
            CmsfObject{
                .kind = CmsfObjectKind::kMedia,
                .track_name = "events",
                .group_id = 7,
                .subgroup_id = 3,
                .object_id = 0,
                .owned_payload = {'A'},
            },
            CmsfObject{
                .kind = CmsfObjectKind::kMedia,
                .track_name = "events",
                .group_id = 7,
                .subgroup_id = 3,
                .object_id = 1,
                .owned_payload = {'B'},
            },
            CmsfObject{
                .kind = CmsfObjectKind::kMedia,
                .track_name = "events",
                .group_id = 7,
                .subgroup_id = 4,
                .object_id = 2,
                .owned_payload = {'C'},
            },
            CmsfObject{
                .kind = CmsfObjectKind::kMedia,
                .track_name = "events",
                .group_id = 7,
                .subgroup_id = 3,
                .object_id = 3,
                .owned_payload = {'D'},
            },
        },
    };
}

PublishPlan make_completed_subgroup_reopen_plan(DraftVersion draft) {
    return {
        .draft = openmoq::publisher::draft_profile(draft),
        .tracks = {
            TrackDescription{.track_id = 1,
                             .handler_type = "vide",
                             .codec = "avc1.64000C",
                             .sample_entry_type = "avc1",
                             .track_name = "events"},
        },
        .objects = {
            CmsfObject{
                .kind = CmsfObjectKind::kMedia,
                .track_name = "events",
                .group_id = 9,
                .subgroup_id = 1,
                .object_id = 0,
                .owned_payload = {'A'},
            },
            CmsfObject{
                .kind = CmsfObjectKind::kMedia,
                .track_name = "events",
                .group_id = 9,
                .subgroup_id = 2,
                .object_id = 1,
                .owned_payload = {'B'},
            },
            CmsfObject{
                .kind = CmsfObjectKind::kMedia,
                .track_name = "events",
                .group_id = 9,
                .subgroup_id = 1,
                .object_id = 0,
                .owned_payload = {'C'},
            },
        },
    };
}

struct SchedulingObjectSpec {
    std::string track_name;
    std::size_t group_id = 0;
    std::uint64_t subgroup_id = 0;
    std::size_t object_id = 0;
    std::uint64_t media_time_us = 0;
    std::uint8_t marker = 0;
};

PublishPlan make_scheduling_plan(std::vector<SchedulingObjectSpec> specs) {
    PublishPlan plan;
    plan.draft = openmoq::publisher::draft_profile(DraftVersion::kDraft16);
    std::set<std::string> track_names;
    for (const auto& spec : specs) {
        if (track_names.insert(spec.track_name).second) {
            plan.tracks.push_back(TrackDescription{
                .track_id = static_cast<std::uint32_t>(plan.tracks.size() + 1),
                .handler_type = "meta",
                .codec = "mett",
                .sample_entry_type = "mett",
                .track_name = spec.track_name,
            });
        }
        plan.objects.push_back(CmsfObject{
            .kind = CmsfObjectKind::kMedia,
            .track_name = spec.track_name,
            .group_id = spec.group_id,
            .subgroup_id = spec.subgroup_id,
            .object_id = spec.object_id,
            .media_time_us = spec.media_time_us,
            .owned_payload = {spec.marker},
        });
    }
    return plan;
}

struct ScheduledObjectIdentity {
    std::uint64_t request_id = 0;
    std::uint64_t group_id = 0;
    std::uint64_t subgroup_id = 0;
    std::uint64_t object_id = 0;
    friend bool operator==(const ScheduledObjectIdentity&,
                           const ScheduledObjectIdentity&) = default;
};

template <typename Event>
std::vector<ScheduledObjectIdentity> scheduled_identities(
    const std::vector<Event>& events,
    const std::map<std::uint8_t, ScheduledObjectIdentity>& identity_by_marker) {
    std::vector<ScheduledObjectIdentity> identities;
    for (const auto& event : events) {
        if (event.bytes.empty()) {
            continue;
        }
        const auto identity_it = identity_by_marker.find(event.bytes.back());
        if (identity_it != identity_by_marker.end()) {
            identities.push_back(identity_it->second);
        }
    }
    return identities;
}

void queue_draft16_scheduling_prefix(MockTransport& transport,
                                     std::uint64_t max_request_id = 32) {
    transport.reads[0].push_back(encode_server_setup_message({
        .draft = DraftVersion::kDraft16,
        .max_request_id = max_request_id,
    }));
    transport.reads[0].push_back(
        encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));
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

    ok &= expect(
        openmoq::publisher::transport::priority_scheduler_internal::
            object_transport_priority_for_testing(0, 0) == 2,
        "expected the highest object tuple to start after control and request classes");
    ok &= expect(
        openmoq::publisher::transport::priority_scheduler_internal::
            object_transport_priority_for_testing(255, 255) == 255,
        "expected the lowest object tuple to saturate without uint8 overflow");
    ok &= expect(
        openmoq::publisher::transport::priority_scheduler_internal::
            object_transport_priority_for_testing(42, 0) <
            openmoq::publisher::transport::priority_scheduler_internal::
                object_transport_priority_for_testing(42, 255),
        "expected transport mapping to retain publisher priority where one class permits it");
    ok &= expect(
        openmoq::publisher::transport::priority_scheduler_internal::
            object_transport_priority_for_testing(42, 255) <=
            openmoq::publisher::transport::priority_scheduler_internal::
                object_transport_priority_for_testing(43, 0),
        "expected transport mapping to preserve subscriber-priority precedence");

    {
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "publisher-low", 1,
            DraftVersion::kDraft16, 0, 0, 20, 1));
        transport.reads[0].push_back(encode_subscribe_message(
            3, kTestTrackNamespace, "publisher-high", 1,
            DraftVersion::kDraft16, 0, 0, 20, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "publisher-low", .group_id = 1,
             .subgroup_id = 0, .object_id = 0, .marker = 'A'},
            {.track_name = "publisher-high", .group_id = 9,
             .subgroup_id = 2, .object_id = 4, .marker = 'P'},
        });
        openmoq::publisher::transport::priority_scheduler_internal::
            clear_publisher_priorities_for_testing();
        openmoq::publisher::transport::priority_scheduler_internal::
            set_publisher_priority_for_testing(1, 201);
        openmoq::publisher::transport::priority_scheduler_internal::
            set_publisher_priority_for_testing(3, 7);

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected publisher-priority session connect to succeed");
        status = session.publish(plan);
        openmoq::publisher::transport::priority_scheduler_internal::
            clear_publisher_priorities_for_testing();
        ok &= expect(status.ok, "expected publisher-priority publish to succeed");
        ok &= expect(transport.reliable_stream_priorities ==
                         std::vector<std::pair<std::uint64_t, std::uint8_t>>{{0, 0}},
                     "expected draft-16 session to explicitly assign its control stream class 0");
        const std::map<std::uint8_t, ScheduledObjectIdentity> identities{
            {'A', {1, 1, 0, 0}},
            {'P', {3, 9, 2, 4}},
        };
        ok &= expect(
            scheduled_identities(transport.object_write_attempts, identities) ==
                std::vector<ScheduledObjectIdentity>({{3, 9, 2, 4},
                                                      {1, 1, 0, 0}}),
            "expected production scheduler request 3/group 9/subgroup 2/object 4 to win by publisher priority");
    }

    {
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "low", 1, DraftVersion::kDraft16,
            0, 0, 200, 1));
        transport.reads[0].push_back(encode_subscribe_message(
            3, kTestTrackNamespace, "high", 1, DraftVersion::kDraft16,
            0, 0, 7, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "low", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .media_time_us = 0, .marker = 'L'},
            {.track_name = "high", .group_id = 9, .subgroup_id = 0,
             .object_id = 0, .media_time_us = 9000, .marker = 'H'},
        });

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected subscriber-priority session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok, "expected subscriber-priority publish to succeed");
        const std::map<std::uint8_t, ScheduledObjectIdentity> identities{
            {'L', {1, 1, 0, 0}},
            {'H', {3, 9, 0, 0}},
        };
        ok &= expect(
            scheduled_identities(transport.writes, identities) ==
                std::vector<ScheduledObjectIdentity>({{3, 9, 0, 0},
                                                      {1, 1, 0, 0}}),
            "expected literal request 3/group 9 before request 1/group 1 by subscriber priority");
    }

    {
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "events", 1, DraftVersion::kDraft16,
            0, 0, 20, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 9, .subgroup_id = 0,
             .object_id = 0, .media_time_us = 0, .marker = 'N'},
            {.track_name = "events", .group_id = 2, .subgroup_id = 0,
             .object_id = 0, .media_time_us = 9000, .marker = 'A'},
        });

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected ascending-order session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok, "expected ascending-order publish to succeed");
        const std::map<std::uint8_t, ScheduledObjectIdentity> identities{
            {'N', {1, 9, 0, 0}},
            {'A', {1, 2, 0, 0}},
        };
        ok &= expect(
            scheduled_identities(transport.writes, identities) ==
                std::vector<ScheduledObjectIdentity>({{1, 2, 0, 0},
                                                      {1, 9, 0, 0}}),
            "expected ascending request 1 to write literal group 2 before group 9");
    }

    {
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "events", 1, DraftVersion::kDraft16,
            0, 0, 20, 2));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 2, .subgroup_id = 0,
             .object_id = 0, .media_time_us = 0, .marker = 'A'},
            {.track_name = "events", .group_id = 9, .subgroup_id = 0,
             .object_id = 0, .media_time_us = 9000, .marker = 'D'},
        });

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected descending-order session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok, "expected descending-order publish to succeed");
        const std::map<std::uint8_t, ScheduledObjectIdentity> identities{
            {'A', {1, 2, 0, 0}},
            {'D', {1, 9, 0, 0}},
        };
        ok &= expect(
            scheduled_identities(transport.writes, identities) ==
                std::vector<ScheduledObjectIdentity>({{1, 9, 0, 0},
                                                      {1, 2, 0, 0}}),
            "expected descending request 1 to write literal group 9 before group 2");
    }

    {
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "ascending", 1, DraftVersion::kDraft16,
            0, 0, 20, 1));
        transport.reads[0].push_back(encode_subscribe_message(
            3, kTestTrackNamespace, "descending", 1, DraftVersion::kDraft16,
            0, 0, 20, 2));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "ascending", .group_id = 9, .subgroup_id = 0,
             .object_id = 0, .marker = 'B'},
            {.track_name = "ascending", .group_id = 2, .subgroup_id = 0,
             .object_id = 0, .marker = 'A'},
            {.track_name = "descending", .group_id = 2, .subgroup_id = 0,
             .object_id = 0, .marker = 'C'},
            {.track_name = "descending", .group_id = 9, .subgroup_id = 0,
             .object_id = 0, .marker = 'D'},
        });

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected opposite-order fairness session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok, "expected opposite-order fairness publish to succeed");
        const std::map<std::uint8_t, ScheduledObjectIdentity> identities{
            {'A', {1, 2, 0, 0}},
            {'B', {1, 9, 0, 0}},
            {'C', {3, 2, 0, 0}},
            {'D', {3, 9, 0, 0}},
        };
        ok &= expect(
            scheduled_identities(transport.object_write_attempts, identities) ==
                std::vector<ScheduledObjectIdentity>({{1, 2, 0, 0},
                                                      {3, 9, 0, 0},
                                                      {1, 9, 0, 0},
                                                      {3, 2, 0, 0}}),
            "expected fair request tie without comparing opposite group directions");
    }

    {
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "events", 1, DraftVersion::kDraft16,
            0, 0, 20, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 4, .subgroup_id = 9,
             .object_id = 0, .media_time_us = 0, .marker = 'N'},
            {.track_name = "events", .group_id = 4, .subgroup_id = 2,
             .object_id = 0, .media_time_us = 9000, .marker = 'S'},
        });

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected subgroup-order session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok, "expected subgroup-order publish to succeed");
        const std::map<std::uint8_t, ScheduledObjectIdentity> identities{
            {'N', {1, 4, 9, 0}},
            {'S', {1, 4, 2, 0}},
        };
        ok &= expect(
            scheduled_identities(transport.writes, identities) ==
                std::vector<ScheduledObjectIdentity>({{1, 4, 2, 0},
                                                      {1, 4, 9, 0}}),
            "expected request 1/group 4/subgroup 2 before subgroup 9");
    }

    {
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "events", 1, DraftVersion::kDraft16,
            0, 0, 20, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 4, .subgroup_id = 2,
             .object_id = 9, .media_time_us = 0, .marker = 'N'},
            {.track_name = "events", .group_id = 4, .subgroup_id = 2,
             .object_id = 2, .media_time_us = 9000, .marker = 'O'},
        });

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected object-order session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok, "expected object-order publish to succeed");
        const std::map<std::uint8_t, ScheduledObjectIdentity> identities{
            {'N', {1, 4, 2, 9}},
            {'O', {1, 4, 2, 2}},
        };
        ok &= expect(
            scheduled_identities(transport.writes, identities) ==
                std::vector<ScheduledObjectIdentity>({{1, 4, 2, 2},
                                                      {1, 4, 2, 9}}),
            "expected request 1/group 4/subgroup 2/object 2 before object 9");
    }

    {
        using Clock = std::chrono::steady_clock;
        Clock::time_point now{};
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "high", 1, DraftVersion::kDraft16,
            1000, 0, 1, 1));
        transport.reads[0].push_back(encode_subscribe_message(
            3, kTestTrackNamespace, "low", 1, DraftVersion::kDraft16,
            1000, 0, 100, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "high", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .media_time_us = 0, .marker = 'H'},
            {.track_name = "low", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .media_time_us = 9000, .marker = 'L'},
        });
        bool blocked_high_once = false;
        transport.on_try_write_object =
            [&blocked_high_once, &now](MockTransport&,
                                       const MockTransport::ObjectWriteEvent& event) {
                now += std::chrono::milliseconds(10);
                if (!blocked_high_once && !event.bytes.empty() &&
                    event.bytes.back() == 'H') {
                    blocked_high_once = true;
                    return ObjectWriteResult{ObjectWriteDisposition::kWouldBlock,
                                             "high-priority test stream blocked"};
                }
                return ObjectWriteResult{ObjectWriteDisposition::kAccepted, {}};
            };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1),
                            {},
                            [&now]() { return now; });
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected blocked-priority session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok, "expected blocked-priority publish to succeed");
        const std::map<std::uint8_t, ScheduledObjectIdentity> identities{
            {'H', {1, 1, 0, 0}},
            {'L', {3, 1, 0, 0}},
        };
        ok &= expect(
            scheduled_identities(transport.object_write_attempts, identities) ==
                std::vector<ScheduledObjectIdentity>({{1, 1, 0, 0},
                                                      {3, 1, 0, 0},
                                                      {1, 1, 0, 0}}),
            "expected blocked request 1, eligible request 3, then retried request 1");
        if (transport.object_write_attempts.size() >= 3) {
            ok &= expect(
                transport.object_write_attempts[0].stream_id ==
                    transport.object_write_attempts[2].stream_id,
                "expected blocked retry to retain the original subgroup stream");
            ok &= expect(
                transport.object_write_attempts[0].options.object_deadline ==
                    transport.object_write_attempts[2].options.object_deadline,
                "expected blocked retry to retain first availability and absolute deadline");
        }
        ok &= expect(transport.reset_calls.empty(),
                     "expected would-block scheduling not to reset any subgroup");
    }

    {
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "events", 1, DraftVersion::kDraft16,
            1000, 0, 1, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'H'},
            {.track_name = "events", .group_id = 1, .subgroup_id = 1,
             .object_id = 0, .marker = 'L'},
        });
        bool blocked_high_once = false;
        transport.on_try_write_object =
            [&](MockTransport&,
                const MockTransport::ObjectWriteEvent& event) {
                if (!blocked_high_once && !event.bytes.empty() &&
                    event.bytes.back() == 'H') {
                    blocked_high_once = true;
                    return ObjectWriteResult{
                        ObjectWriteDisposition::kWouldBlock,
                        "same-request high-priority subgroup blocked"};
                }
                return ObjectWriteResult{
                    ObjectWriteDisposition::kAccepted, {}};
            };

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected same-request blocked scheduler session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok,
                     "expected same-request blocked scheduler publish to succeed");
        const std::map<std::uint8_t, ScheduledObjectIdentity> identities{
            {'H', {1, 1, 0, 0}},
            {'L', {1, 1, 1, 0}},
        };
        ok &= expect(
            scheduled_identities(transport.object_write_attempts,
                                 identities) ==
                std::vector<ScheduledObjectIdentity>({{1, 1, 0, 0},
                                                      {1, 1, 1, 0},
                                                      {1, 1, 0, 0}}),
            "expected a blocked subgroup frontier not to hide a lower-priority eligible subgroup in the same request");
    }

    {
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "blocked-high", 1, DraftVersion::kDraft16,
            0, 0, 1, 1));
        transport.reads[0].push_back(encode_subscribe_message(
            3, kTestTrackNamespace, "blocked-low", 1, DraftVersion::kDraft16,
            0, 0, 100, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "blocked-high", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'H'},
            {.track_name = "blocked-low", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'L'},
        });
        std::atomic<bool> release{false};
        std::atomic<std::size_t> attempts{0};
        bool saw_high_while_blocked = false;
        bool saw_low_while_blocked = false;
        transport.on_try_write_object =
            [&](MockTransport&, const MockTransport::ObjectWriteEvent& event) {
                attempts.fetch_add(1, std::memory_order_relaxed);
                if (release.load(std::memory_order_acquire)) {
                    return ObjectWriteResult{ObjectWriteDisposition::kAccepted, {}};
                }
                if (!event.bytes.empty() && event.bytes.back() == 'H') {
                    saw_high_while_blocked = true;
                }
                if (!event.bytes.empty() && event.bytes.back() == 'L') {
                    saw_low_while_blocked = true;
                }
                return ObjectWriteResult{ObjectWriteDisposition::kWouldBlock,
                                         "all-candidates-blocked test"};
            };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected all-blocked session connect to succeed");
        std::thread releaser([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            release.store(true, std::memory_order_release);
        });
        status = session.publish(plan);
        releaser.join();
        ok &= expect(status.ok, "expected all-blocked scheduler to make cooperative progress");
        ok &= expect(saw_high_while_blocked && saw_low_while_blocked,
                     "expected each eligible candidate to be attempted before the cooperative retry");
        ok &= expect(attempts.load(std::memory_order_relaxed) < 64,
                     "expected all-blocked retries to avoid a hot busy-spin");
        ok &= expect(transport.reset_calls.empty(),
                     "expected all-blocked retries not to reset either subgroup");
    }

    for (const DraftVersion draft :
         {DraftVersion::kDraft16, DraftVersion::kDraft18}) {
        using Clock = std::chrono::steady_clock;
        Clock::time_point now{};
        MockTransport transport;
        if (draft == DraftVersion::kDraft16) {
            queue_draft16_scheduling_prefix(transport);
            transport.reads[0].push_back(encode_subscribe_message(
                1, kTestTrackNamespace, "stalled-high", 1, draft,
                0, 0, 1, 1));
            transport.reads[0].push_back(encode_subscribe_message(
                3, kTestTrackNamespace, "stalled-low", 1, draft,
                0, 0, 100, 1));
        } else {
            transport.keep_open_streams.insert(1);
            transport.keep_open_streams.insert(5);
            transport.reads[3].push_back(encode_draft18_setup_response());
            transport.reads[0].push_back(
                encode_publish_namespace_ok_message(draft, 0));
            transport.reads[1].push_back(encode_subscribe_message(
                91, kTestTrackNamespace, "stalled-high", 1, draft,
                0, 0, 1, 1));
            transport.reads[5].push_back(encode_subscribe_message(
                93, kTestTrackNamespace, "stalled-low", 1, draft,
                0, 0, 100, 1));
        }
        PublishPlan plan = make_scheduling_plan({
            {.track_name = "stalled-high", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'H'},
            {.track_name = "stalled-low", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'L'},
            {.track_name = "stalled-low", .group_id = 1, .subgroup_id = 1,
             .object_id = 0, .marker = 'M'},
        });
        plan.draft = openmoq::publisher::draft_profile(draft);
        std::set<std::uint64_t> low_stream_ids;
        transport.on_try_write_object =
            [&](MockTransport& current,
                const MockTransport::ObjectWriteEvent& event) {
                now += std::chrono::milliseconds(600);
                if (!event.bytes.empty() &&
                    (event.bytes.back() == 'L' ||
                     event.bytes.back() == 'M')) {
                    low_stream_ids.insert(event.stream_id);
                }
                if (!current.reset_calls.empty() && !event.bytes.empty() &&
                    event.bytes.back() == 'H') {
                    return ObjectWriteResult{
                        ObjectWriteDisposition::kAccepted, {}};
                }
                return ObjectWriteResult{
                    ObjectWriteDisposition::kWouldBlock,
                    "permanently blocked until resource eviction"};
            };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1),
                            {},
                            [&now]() { return now; });
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected resource-stall session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok,
                     "expected persistent all-block resource stall to terminate the lowest-priority subscription");
        const std::uint64_t expected_reset_code =
            draft == DraftVersion::kDraft16 ? 0x01 : 0x05;
        ok &= expect(low_stream_ids.size() == 2 &&
                         std::all_of(
                             low_stream_ids.begin(),
                             low_stream_ids.end(),
                             [&](std::uint64_t stream_id) {
                                 return std::find(
                                            transport.reset_calls.begin(),
                                            transport.reset_calls.end(),
                                            std::pair{stream_id,
                                                      expected_reset_code}) !=
                                        transport.reset_calls.end();
                             }),
                     "expected every opened subgroup in the stalled request to receive the draft-specific resource reset");
        const std::uint64_t low_request_id =
            draft == DraftVersion::kDraft16 ? 3 : 93;
        const std::uint64_t response_stream_id =
            draft == DraftVersion::kDraft16 ? 0 : 5;
        const std::uint64_t too_far_behind =
            draft == DraftVersion::kDraft16 ? 0x06 : 0x05;
        const auto expected_done =
            openmoq::publisher::transport::encode_publish_done_message(
                draft,
                low_request_id,
                2,
                too_far_behind,
                "subscriber exceeded publisher resource limit");
        ok &= expect(std::count_if(
                         transport.writes.begin(),
                         transport.writes.end(),
                         [&](const MockTransport::WriteEvent& write) {
                             return write.stream_id == response_stream_id &&
                                    write.bytes == expected_done &&
                                    write.reset_count_before_write >= 2;
                         }) == 1,
                     "expected TOO_FAR_BEHIND PUBLISH_DONE only after every opened subgroup reset");
    }

    {
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "scale", 1, DraftVersion::kDraft16,
            0, 0, 10, 1));
        std::vector<SchedulingObjectSpec> objects;
        constexpr std::size_t kScaleObjectCount = 128;
        objects.reserve(kScaleObjectCount);
        for (std::size_t index = 0; index < kScaleObjectCount; ++index) {
            objects.push_back({.track_name = "scale",
                               .group_id = 1,
                               .subgroup_id = index,
                               .object_id = 0,
                               .media_time_us = index,
                               .marker = static_cast<std::uint8_t>(index)});
        }
        const PublishPlan plan = make_scheduling_plan(objects);
        openmoq::publisher::transport::priority_scheduler_internal::
            begin_priority_comparison_count_for_testing();
        MoqtSession session(transport, std::string(kTestTrackNamespace));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected retained-heap scale session connect to succeed");
        status = session.publish(plan);
        const std::uint64_t comparison_count =
            openmoq::publisher::transport::priority_scheduler_internal::
                end_priority_comparison_count_for_testing();
        ok &= expect(status.ok &&
                         transport.object_write_attempts.size() ==
                             kScaleObjectCount,
                     "expected retained scheduler heap to admit every scale object once");
        ok &= expect(comparison_count < 5000,
                     "expected retained scheduler heap to avoid rebuilding all eligible candidates after each admission");
    }

    {
        using Clock = std::chrono::steady_clock;
        Clock::time_point now{};
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "high", 1, DraftVersion::kDraft16,
            0, 0, 1, 1));
        transport.reads[0].push_back(encode_subscribe_message(
            3, kTestTrackNamespace, "deadline", 1, DraftVersion::kDraft16,
            5, 0, 100, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "high", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'H'},
            {.track_name = "deadline", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'D'},
        });
        transport.on_try_write_object =
            [&now](MockTransport&,
                   const MockTransport::ObjectWriteEvent& event) {
                if (!event.bytes.empty() && event.bytes.back() == 'H') {
                    now += std::chrono::milliseconds(10);
                }
                return ObjectWriteResult{ObjectWriteDisposition::kAccepted, {}};
            };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1),
                            {},
                            [&now]() { return now; });
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected priority-wait deadline session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok, "expected priority-wait deadline publish to succeed");
        const std::map<std::uint8_t, ScheduledObjectIdentity> identities{
            {'H', {1, 1, 0, 0}},
            {'D', {3, 1, 0, 0}},
        };
        ok &= expect(
            scheduled_identities(transport.object_write_attempts, identities) ==
                std::vector<ScheduledObjectIdentity>({{1, 1, 0, 0}}),
            "expected lower-priority object deadline to age from availability before scheduler selection");
    }

    {
        using Clock = std::chrono::steady_clock;
        Clock::time_point now{};
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "events", 1, DraftVersion::kDraft16,
            5, 0, 1, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'A'},
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 1, .marker = 'B'},
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 2, .marker = 'C'},
        });
        transport.on_try_write_object =
            [&now](MockTransport&,
                   const MockTransport::ObjectWriteEvent& event) {
                if (!event.bytes.empty() && event.bytes.back() == 'A') {
                    now += std::chrono::milliseconds(10);
                }
                return ObjectWriteResult{ObjectWriteDisposition::kAccepted, {}};
            };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1),
                            {},
                            [&now]() { return now; });
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected predecessor-wait object deadline session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok,
                     "expected predecessor-wait object deadline publish to succeed");
        ok &= expect(transport.object_write_attempts.size() == 1 &&
                         transport.object_write_attempts.front().bytes.back() == 'A',
                     "expected every provided object deadline to age while its subgroup predecessor runs");
        ok &= expect(std::any_of(transport.reset_calls.begin(),
                                 transport.reset_calls.end(),
                                 [](const auto& reset) {
                                     return reset.second == 0x02;
                                 }),
                     "expected predecessor-aged object expiry to reset its open subgroup");
    }

    {
        using Clock = std::chrono::steady_clock;
        Clock::time_point now{};
        MockTransport transport;
        transport.reads[3].push_back(encode_draft18_setup_response());
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        transport.reads[1].push_back(encode_subscribe_message(
            91, kTestTrackNamespace, "events", 1, DraftVersion::kDraft18,
            0, 5, 1, 1));
        PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'A'},
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 1, .marker = 'B'},
        });
        plan.draft = openmoq::publisher::draft_profile(DraftVersion::kDraft18);
        transport.on_try_write_object =
            [&now](MockTransport&,
                   const MockTransport::ObjectWriteEvent& event) {
                if (!event.bytes.empty() && event.bytes.back() == 'A') {
                    now += std::chrono::milliseconds(10);
                }
                return ObjectWriteResult{ObjectWriteDisposition::kAccepted, {}};
            };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1),
                            {},
                            [&now]() { return now; });
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected predecessor-wait subgroup deadline session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok,
                     "expected predecessor-wait subgroup deadline publish to succeed");
        ok &= expect(transport.object_write_attempts.size() == 1 &&
                         transport.object_write_attempts.front().bytes.back() == 'A',
                     "expected provided subgroup completion deadline to age while its predecessor runs");
        ok &= expect(std::any_of(transport.reset_calls.begin(),
                                 transport.reset_calls.end(),
                                 [](const auto& reset) {
                                     return reset.second == 0x02;
                                 }),
                     "expected predecessor-aged subgroup completion expiry to reset its stream");
    }

    {
        using Clock = std::chrono::steady_clock;
        Clock::time_point now{};
        MockTransport transport;
        transport.keep_open_streams.insert(0);
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "events", 1, DraftVersion::kDraft16,
            5, 0, 1, 1, 0, 0, 0x04, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'H'},
            {.track_name = "events", .group_id = 2, .subgroup_id = 0,
             .object_id = 0, .marker = 'G'},
        });
        bool update_queued = false;
        transport.on_try_write_object =
            [&](MockTransport& current,
                const MockTransport::ObjectWriteEvent& event) {
                if (!update_queued && !event.bytes.empty() &&
                    event.bytes.back() == 'H') {
                    now += std::chrono::milliseconds(10);
                    current.reads[0].push_back(
                        encode_request_update_filter_message(
                            DraftVersion::kDraft16, 3, 1, 0x03, 0, 0));
                    update_queued = true;
                    return ObjectWriteResult{
                        ObjectWriteDisposition::kWouldBlock,
                        "advance generation clock before filter rebuild"};
                }
                return ObjectWriteResult{
                    ObjectWriteDisposition::kAccepted, {}};
            };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1),
                            {},
                            [&now]() { return now; });
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected generation-owned filter session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok &&
                         scheduled_identities(
                             transport.object_write_attempts,
                             {{'H', {1, 1, 0, 0}},
                              {'G', {1, 2, 0, 0}}}) ==
                             std::vector<ScheduledObjectIdentity>{
                                 {1, 1, 0, 0}},
                     "expected filter rebuild to reuse generation-owned object availability instead of refreshing it");
    }

    {
        using Clock = std::chrono::steady_clock;
        Clock::time_point now{};
        MockTransport transport;
        transport.reads[3].push_back(encode_draft18_setup_response());
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        transport.reads[1].push_back(encode_subscribe_message(
            91, kTestTrackNamespace, "events", 1, DraftVersion::kDraft18,
            0, 5, 1, 1));
        PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'A'},
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 1, .marker = 'B'},
        });
        plan.draft = openmoq::publisher::draft_profile(DraftVersion::kDraft18);
        bool blocked_once = false;
        transport.on_try_write_object =
            [&](MockTransport&,
                const MockTransport::ObjectWriteEvent& event) {
                if (!blocked_once && !event.bytes.empty() &&
                    event.bytes.back() == 'A') {
                    blocked_once = true;
                    now += std::chrono::milliseconds(10);
                    return ObjectWriteResult{
                        ObjectWriteDisposition::kWouldBlock,
                        "block predecessor past subgroup completion deadline"};
                }
                return ObjectWriteResult{
                    ObjectWriteDisposition::kAccepted, {}};
            };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1),
                            {},
                            [&now]() { return now; });
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected blocked subgroup-completion session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok &&
                         transport.object_write_attempts.size() == 1 &&
                         transport.object_write_attempts.front().bytes.back() ==
                             'A',
                     "expected subgroup completion deadline to constrain a blocked predecessor write");
        ok &= expect(std::any_of(
                         transport.reset_calls.begin(),
                         transport.reset_calls.end(),
                         [](const auto& reset) {
                             return reset.second == 0x02;
                         }),
                     "expected blocked predecessor to reset at the generation-owned subgroup completion deadline");
    }

    {
        using Clock = std::chrono::steady_clock;
        Clock::time_point now{};
        MockTransport transport;
        transport.keep_open_streams.insert(0);
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "events", 1, DraftVersion::kDraft16,
            100, 0, 1, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'S'},
        });
        bool update_queued = false;
        transport.on_try_write_object =
            [&](MockTransport& current,
                const MockTransport::ObjectWriteEvent&) {
                if (!update_queued) {
                    current.reads[0].push_back(encode_request_update_message(
                        DraftVersion::kDraft16, 3, 5, 0x02, 1));
                    now += std::chrono::milliseconds(10);
                    update_queued = true;
                    return ObjectWriteResult{
                        ObjectWriteDisposition::kWouldBlock,
                        "shorten retained object timeout"};
                }
                return ObjectWriteResult{
                    ObjectWriteDisposition::kAccepted, {}};
            };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1),
                            {},
                            [&now]() { return now; });
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected shortened-timeout session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok &&
                         transport.object_write_attempts.size() == 1 &&
                         std::any_of(transport.reset_calls.begin(),
                                     transport.reset_calls.end(),
                                     [](const auto& reset) {
                                         return reset.second == 0x02;
                                     }),
                     "expected a shortened REQUEST_UPDATE timeout to expire the retained object from its original availability");
    }

    {
        using Clock = std::chrono::steady_clock;
        Clock::time_point now{};
        MockTransport transport;
        transport.keep_open_streams.insert(0);
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "events", 1, DraftVersion::kDraft16,
            5, 0, 1, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'L'},
        });
        bool update_queued = false;
        transport.on_try_write_object =
            [&](MockTransport& current,
                const MockTransport::ObjectWriteEvent&) {
                if (!update_queued) {
                    current.reads[0].push_back(encode_request_update_message(
                        DraftVersion::kDraft16, 3, 100, 0x02, 1));
                    now += std::chrono::milliseconds(10);
                    update_queued = true;
                    return ObjectWriteResult{
                        ObjectWriteDisposition::kWouldBlock,
                        "lengthen retained object timeout"};
                }
                return ObjectWriteResult{
                    ObjectWriteDisposition::kAccepted, {}};
            };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1),
                            {},
                            [&now]() { return now; });
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected lengthened-timeout session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok &&
                         transport.object_write_attempts.size() == 2 &&
                         transport.object_write_attempts[0].stream_id ==
                             transport.object_write_attempts[1].stream_id &&
                         transport.object_write_attempts[0].bytes ==
                             transport.object_write_attempts[1].bytes &&
                         transport.object_write_attempts[1]
                                 .options.object_deadline ==
                             Clock::time_point{} +
                                 std::chrono::milliseconds(100),
                     "expected a lengthened REQUEST_UPDATE timeout to refresh only options on the retained bytes and stream");
    }

    {
        using Clock = std::chrono::steady_clock;
        Clock::time_point now{};
        MockTransport transport;
        transport.keep_open_streams.insert(1);
        transport.reads[3].push_back(encode_draft18_setup_response());
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        transport.reads[1].push_back(encode_subscribe_message(
            91, kTestTrackNamespace, "events", 1, DraftVersion::kDraft18,
            0, 0, 1, 1));
        PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'Z'},
        });
        plan.draft =
            openmoq::publisher::draft_profile(DraftVersion::kDraft18);
        bool update_queued = false;
        transport.on_try_write_object =
            [&](MockTransport& current,
                const MockTransport::ObjectWriteEvent&) {
                if (!update_queued) {
                    current.reads[1].push_back(encode_request_update_message(
                        DraftVersion::kDraft18, 93, 5, 0x02));
                    now += std::chrono::milliseconds(10);
                    update_queued = true;
                    return ObjectWriteResult{
                        ObjectWriteDisposition::kWouldBlock,
                        "arm retained draft-18 object timeout"};
                }
                return ObjectWriteResult{
                    ObjectWriteDisposition::kAccepted, {}};
            };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1),
                            {},
                            [&now]() { return now; });
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected draft-18 zero-timeout update session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok &&
                         transport.object_write_attempts.size() == 1 &&
                         std::any_of(transport.reset_calls.begin(),
                                     transport.reset_calls.end(),
                                     [](const auto& reset) {
                                         return reset.second == 0x02;
                                     }),
                     "expected draft-18 zero-to-nonzero timeout update to age from immutable availability");
    }

    {
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "events", 1, DraftVersion::kDraft16,
            0, 0, 20, 1, 0, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 0, .subgroup_id = 0,
             .object_id = 0, .marker = 'X'},
            {.track_name = "events", .group_id = 0, .subgroup_id = 0,
             .object_id = 1, .marker = 'O'},
        });

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected filtered-predecessor session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok, "expected filtered-predecessor publish to succeed");
        const std::map<std::uint8_t, ScheduledObjectIdentity> identities{
            {'X', {1, 0, 0, 0}},
            {'O', {1, 0, 0, 1}},
        };
        ok &= expect(
            scheduled_identities(transport.object_write_attempts, identities) ==
                std::vector<ScheduledObjectIdentity>({{1, 0, 0, 1}}),
            "expected an object before the subscription start not to block its eligible successor");
    }

    {
        MockTransport transport;
        transport.keep_open_streams.insert(0);
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "high", 1, DraftVersion::kDraft16,
            1000, 0, 1, 1));
        transport.reads[0].push_back(encode_subscribe_message(
            3, kTestTrackNamespace, "middle", 1, DraftVersion::kDraft16,
            1000, 0, 50, 1));
        transport.reads[0].push_back(encode_subscribe_message(
            5, kTestTrackNamespace, "low", 1, DraftVersion::kDraft16,
            1000, 0, 100, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "high", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'H'},
            {.track_name = "middle", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'M'},
            {.track_name = "low", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'L'},
        });
        bool update_queued = false;
        transport.on_try_write_object =
            [&update_queued](MockTransport& current,
                             const MockTransport::ObjectWriteEvent& event) {
                if (!update_queued && !event.bytes.empty() &&
                    event.bytes.back() == 'H') {
                    current.reads[0].push_back(
                        encode_request_update_message(
                            DraftVersion::kDraft16, 7, 200, 0x20, 1));
                    update_queued = true;
                    return ObjectWriteResult{ObjectWriteDisposition::kWouldBlock,
                                             "update-priority test stream blocked"};
                }
                return ObjectWriteResult{ObjectWriteDisposition::kAccepted, {}};
            };

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected update-priority session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok, "expected update-priority publish to succeed");
        const std::map<std::uint8_t, ScheduledObjectIdentity> identities{
            {'H', {1, 1, 0, 0}},
            {'M', {3, 1, 0, 0}},
            {'L', {5, 1, 0, 0}},
        };
        ok &= expect(
            scheduled_identities(transport.object_write_attempts, identities) ==
                std::vector<ScheduledObjectIdentity>({{1, 1, 0, 0},
                                                      {3, 1, 0, 0},
                                                      {5, 1, 0, 0},
                                                      {1, 1, 0, 0}}),
            "expected draft-16 REQUEST_UPDATE priority to reorder the not-yet-admitted retry");
        if (transport.object_write_attempts.size() >= 4) {
            ok &= expect(
                transport.object_write_attempts[0].options.transport_priority <
                    transport.object_write_attempts[3].options.transport_priority,
                "expected REQUEST_UPDATE to reprioritize the session-owned object in the transport");
        }
        const auto request_update_ok =
            openmoq::publisher::transport::encode_request_ok_message(
                DraftVersion::kDraft16, 7);
        ok &= expect(std::count_if(
                         transport.writes.begin(),
                         transport.writes.end(),
                         [&](const MockTransport::WriteEvent& write) {
                             return write.stream_id == 0 && write.bytes == request_update_ok;
                         }) == 1,
                     "expected exactly one draft-16 REQUEST_OK carrying update request id 7");
    }

    {
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "events", 0, DraftVersion::kDraft16,
            1000, 0, 1, 1));
        transport.reads[0].push_back(encode_request_update_message(
            DraftVersion::kDraft16, 3, 9, 0x32, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'U'},
        });

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected draft-16 rejected-update session connect to succeed");
        status = session.publish(plan);
        ok &= expect(!status.ok && transport.last_close_code == 0x03,
                     "expected unnegotiated NEW_GROUP to close the session with PROTOCOL_VIOLATION");
        ok &= expect(std::none_of(
                         transport.writes.begin(),
                         transport.writes.end(),
                         [](const MockTransport::WriteEvent& write) {
                             const auto type = message_type(write.bytes);
                             return type == 0x05 || type == 0x0b;
                         }),
                     "expected unnegotiated NEW_GROUP not to emit REQUEST_ERROR or PUBLISH_DONE");
        ok &= expect(transport.object_write_attempts.empty(),
                     "expected rejected update not to admit subscription objects");
    }

    for (const DraftVersion draft :
         {DraftVersion::kDraft16, DraftVersion::kDraft18}) {
        MockTransport transport;
        const std::uint64_t subscription_request_id =
            draft == DraftVersion::kDraft16 ? 1 : 91;
        const std::uint64_t update_request_id =
            draft == DraftVersion::kDraft16 ? 3 : 93;
        const std::uint64_t response_stream_id =
            draft == DraftVersion::kDraft16 ? 0 : 1;
        if (draft == DraftVersion::kDraft16) {
            queue_draft16_scheduling_prefix(transport);
            transport.reads[0].push_back(encode_subscribe_message(
                subscription_request_id,
                kTestTrackNamespace,
                "events",
                1,
                draft,
                1000,
                0,
                1,
                1));
            transport.reads[0].push_back(
                encode_request_update_authorization_message(
                    draft, update_request_id, subscription_request_id));
        } else {
            transport.reads[3].push_back(encode_draft18_setup_response());
            transport.reads[0].push_back(
                encode_publish_namespace_ok_message(draft, 0));
            transport.reads[1].push_back(encode_subscribe_message(
                subscription_request_id,
                kTestTrackNamespace,
                "events",
                1,
                draft,
                1000,
                0,
                1,
                1));
            transport.reads[1].push_back(
                encode_request_update_authorization_message(
                    draft, update_request_id, std::nullopt));
        }
        PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'A'},
        });
        plan.draft = openmoq::publisher::draft_profile(draft);

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected repeated-authorization update session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok &&
                         transport.object_write_attempts.size() == 1,
                     "expected repeated valid AUTHORIZATION_TOKEN parameters to preserve the subscription");
        const auto expected_ok =
            openmoq::publisher::transport::encode_request_ok_message(
                draft, update_request_id);
        ok &= expect(std::count_if(
                         transport.writes.begin(),
                         transport.writes.end(),
                         [&](const MockTransport::WriteEvent& write) {
                             return write.stream_id == response_stream_id &&
                                    write.bytes == expected_ok;
                         }) == 1,
                     "expected repeated valid AUTHORIZATION_TOKEN parameters to receive REQUEST_OK");
    }

    for (const DraftVersion draft :
         {DraftVersion::kDraft16, DraftVersion::kDraft18}) {
        for (const std::uint64_t parameter_type : {0x03ULL, 0x21ULL}) {
            MockTransport transport;
            const std::uint64_t subscription_request_id =
                draft == DraftVersion::kDraft16 ? 1 : 91;
            const std::uint64_t update_request_id =
                draft == DraftVersion::kDraft16 ? 3 : 93;
            if (draft == DraftVersion::kDraft16) {
                queue_draft16_scheduling_prefix(transport);
                transport.reads[0].push_back(encode_subscribe_message(
                    subscription_request_id,
                    kTestTrackNamespace,
                    "events",
                    1,
                    draft,
                    1000,
                    0,
                    1,
                    1));
                transport.reads[0].push_back(
                    encode_malformed_request_update_parameter(
                        draft,
                        update_request_id,
                        subscription_request_id,
                        parameter_type));
            } else {
                transport.reads[3].push_back(encode_draft18_setup_response());
                transport.reads[0].push_back(
                    encode_publish_namespace_ok_message(draft, 0));
                transport.reads[1].push_back(encode_subscribe_message(
                    subscription_request_id,
                    kTestTrackNamespace,
                    "events",
                    1,
                    draft,
                    1000,
                    0,
                    1,
                    1));
                transport.reads[1].push_back(
                    encode_malformed_request_update_parameter(
                        draft,
                        update_request_id,
                        std::nullopt,
                        parameter_type));
            }
            PublishPlan plan = make_scheduling_plan({
                {.track_name = "events", .group_id = 1,
                 .subgroup_id = 0, .object_id = 0, .marker = 'E'},
            });
            plan.draft = openmoq::publisher::draft_profile(draft);

            MoqtSession session(transport,
                                std::string(kTestTrackNamespace),
                                false,
                                false,
                                false,
                                false,
                                std::chrono::seconds(1));
            ok &= expect(session.connect(endpoint, tls).ok,
                         "expected malformed update session connect to succeed");
            status = session.publish(plan);
            ok &= expect(!status.ok && transport.last_close_code == 0x06,
                         "expected malformed known REQUEST_UPDATE key/value encoding to close with KEY_VALUE_FORMATTING_ERROR");
        }
    }

    {
        MockTransport transport;
        transport.keep_open_streams.insert(0);
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "events", 1, DraftVersion::kDraft16,
            1000, 0, 1, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'X'},
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 1, .marker = 'Y'},
        });
        bool update_queued = false;
        transport.on_try_write_object =
            [&update_queued](MockTransport& current,
                             const MockTransport::ObjectWriteEvent& event) {
                if (!update_queued && !event.bytes.empty() &&
                    event.bytes.back() == 'X') {
                    current.reads[0].push_back(encode_request_update_filter_message(
                        DraftVersion::kDraft16, 3, 1, 0x03, 1, 1));
                    update_queued = true;
                    return ObjectWriteResult{ObjectWriteDisposition::kWouldBlock,
                                             "filter-update predecessor blocked"};
                }
                return ObjectWriteResult{ObjectWriteDisposition::kAccepted, {}};
            };

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected filter-update session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok, "expected narrowed filter update to succeed");
        const std::map<std::uint8_t, ScheduledObjectIdentity> identities{
            {'X', {1, 1, 0, 0}},
            {'Y', {1, 1, 0, 1}},
        };
        ok &= expect(
            scheduled_identities(transport.object_write_attempts, identities) ==
                std::vector<ScheduledObjectIdentity>({{1, 1, 0, 0},
                                                      {1, 1, 0, 1}}),
            "expected a newly excluded would-block object to be dropped before its valid successor");
        ok &= expect(transport.reset_calls.empty(),
                     "expected successful filter narrowing not to reset already-admitted subgroup bytes");
    }

    {
        MockTransport transport;
        transport.keep_open_streams.insert(1);
        transport.reads[3].push_back(encode_draft18_setup_response());
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        transport.reads[1].push_back(encode_subscribe_message(
            91, kTestTrackNamespace, "events", 1, DraftVersion::kDraft18,
            1000, 0, 1, 1));
        PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'R'},
        });
        plan.draft = openmoq::publisher::draft_profile(DraftVersion::kDraft18);

        bool update_queued = false;
        transport.on_try_write_object =
            [&update_queued](MockTransport& current,
                             const MockTransport::ObjectWriteEvent& event) {
                if (!update_queued && !event.bytes.empty() &&
                    event.bytes.back() == 'R') {
                    current.reads[1].push_back(encode_request_update_message(
                        DraftVersion::kDraft18, 93, 200, 0x20));
                    update_queued = true;
                    return ObjectWriteResult{ObjectWriteDisposition::kWouldBlock,
                                             "retained request stream update pending"};
                }
                return ObjectWriteResult{ObjectWriteDisposition::kAccepted, {}};
            };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected draft-18 retained-update session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok,
                     "expected draft-18 REQUEST_UPDATE on retained SUBSCRIBE stream to succeed");
        ok &= expect(
            transport.reliable_stream_priorities ==
                std::vector<std::pair<std::uint64_t, std::uint8_t>>{{2, 0}, {0, 1}, {1, 1}},
            "expected draft-18 session to assign control, local request, and peer request roles explicitly");
        const auto expected_ok =
            openmoq::publisher::transport::encode_request_ok_message(
                DraftVersion::kDraft18, 93);
        ok &= expect(std::count_if(
                         transport.writes.begin(),
                         transport.writes.end(),
                         [&](const MockTransport::WriteEvent& write) {
                             return write.stream_id == 1 && write.bytes == expected_ok;
                         }) == 1,
                     "expected exactly one draft-18 REQUEST_OK without a request id on retained stream 1");
        ok &= expect(transport.object_write_attempts.size() == 2 &&
                         transport.object_write_attempts[0].stream_id ==
                             transport.object_write_attempts[1].stream_id &&
                         transport.object_write_attempts[0].options.transport_priority <
                             transport.object_write_attempts[1].options.transport_priority,
                     "expected retained draft-18 update to reprioritize the same session-owned object");
    }

    {
        MockTransport transport;
        transport.keep_open_streams.insert(0);
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "events", 1, DraftVersion::kDraft16,
            1000, 0, 1, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'F'},
        });
        std::size_t attempts = 0;
        bool pause_queued = false;
        std::size_t reads_after_pause = 0;
        std::size_t attempts_when_resume_queued = 0;
        transport.on_try_write_object =
            [&](MockTransport& current,
                const MockTransport::ObjectWriteEvent&) {
                ++attempts;
                if (!pause_queued) {
                    current.reads[0].push_back(encode_request_update_message(
                        DraftVersion::kDraft16, 3, 0, 0x10, 1));
                    pause_queued = true;
                    return ObjectWriteResult{
                        ObjectWriteDisposition::kWouldBlock,
                        "pause retained candidate"};
                }
                return ObjectWriteResult{ObjectWriteDisposition::kAccepted, {}};
            };
        transport.on_read = [&](MockTransport& current, std::uint64_t stream_id) {
            if (!pause_queued || stream_id != 0 || ++reads_after_pause != 3) {
                return;
            }
            attempts_when_resume_queued = attempts;
            current.reads[0].push_back(encode_request_update_message(
                DraftVersion::kDraft16, 5, 1, 0x10, 1));
        };

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected draft-16 Forward park/resume session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok,
                     "expected draft-16 Forward park/resume update sequence to succeed");
        ok &= expect(attempts_when_resume_queued == 1 && attempts == 2,
                     "expected draft-16 Forward 1-to-0 to park and 0-to-1 to resume the retained candidate");
    }

    {
        MockTransport transport;
        transport.keep_open_streams.insert(1);
        transport.reads[3].push_back(encode_draft18_setup_response());
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        transport.reads[1].push_back(encode_subscribe_message(
            91, kTestTrackNamespace, "events", 1, DraftVersion::kDraft18,
            1000, 0, 1, 1));
        PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'F'},
        });
        plan.draft = openmoq::publisher::draft_profile(DraftVersion::kDraft18);
        std::size_t attempts = 0;
        bool pause_queued = false;
        std::size_t reads_after_pause = 0;
        std::size_t attempts_when_resume_queued = 0;
        transport.on_try_write_object =
            [&](MockTransport& current,
                const MockTransport::ObjectWriteEvent&) {
                ++attempts;
                if (!pause_queued) {
                    current.reads[1].push_back(encode_request_update_message(
                        DraftVersion::kDraft18, 93, 0, 0x10));
                    pause_queued = true;
                    return ObjectWriteResult{
                        ObjectWriteDisposition::kWouldBlock,
                        "pause retained candidate"};
                }
                return ObjectWriteResult{ObjectWriteDisposition::kAccepted, {}};
            };
        transport.on_read = [&](MockTransport& current, std::uint64_t stream_id) {
            if (!pause_queued || stream_id != 1 || ++reads_after_pause != 3) {
                return;
            }
            attempts_when_resume_queued = attempts;
            current.reads[1].push_back(encode_request_update_message(
                DraftVersion::kDraft18, 95, 1, 0x10));
        };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected draft-18 Forward park/resume session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok,
                     "expected draft-18 Forward park/resume update sequence to succeed");
        ok &= expect(attempts_when_resume_queued == 1 && attempts == 2,
                     "expected draft-18 Forward 1-to-0 to park and 0-to-1 to resume the retained candidate");
    }

    for (const DraftVersion draft :
         {DraftVersion::kDraft16, DraftVersion::kDraft18}) {
        MockTransport transport;
        const std::uint64_t request_stream_id =
            draft == DraftVersion::kDraft16 ? 0 : 1;
        if (draft == DraftVersion::kDraft16) {
            transport.keep_open_streams.insert(0);
            queue_draft16_scheduling_prefix(transport);
            transport.reads[0].push_back(encode_subscribe_message(
                1, kTestTrackNamespace, "initially-paused", 0, draft,
                1000, 0, 1, 1));
        } else {
            transport.keep_open_streams.insert(1);
            transport.reads[3].push_back(encode_draft18_setup_response());
            transport.reads[0].push_back(
                encode_publish_namespace_ok_message(draft, 0));
            transport.reads[1].push_back(encode_subscribe_message(
                91, kTestTrackNamespace, "initially-paused", 0, draft,
                1000, 0, 1, 1));
        }
        PublishPlan plan = make_scheduling_plan({
            {.track_name = "initially-paused", .group_id = 1,
             .subgroup_id = 0, .object_id = 0, .marker = 'I'},
        });
        plan.draft = openmoq::publisher::draft_profile(draft);
        std::size_t request_reads = 0;
        bool resume_queued = false;
        std::size_t attempts_before_resume = 0;
        transport.on_read = [&](MockTransport& current,
                                std::uint64_t stream_id) {
            if (stream_id != request_stream_id) {
                return;
            }
            ++request_reads;
            const std::size_t resume_read =
                draft == DraftVersion::kDraft16 ? 5 : 3;
            if (request_reads != resume_read) {
                return;
            }
            attempts_before_resume = current.object_write_attempts.size();
            resume_queued = true;
            current.reads[request_stream_id].push_back(
                encode_request_update_message(
                    draft,
                    draft == DraftVersion::kDraft16 ? 3 : 93,
                    1,
                    0x10,
                    draft == DraftVersion::kDraft16
                        ? std::optional<std::uint64_t>{1}
                        : std::nullopt));
        };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected initial Forward=0 session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok && resume_queued &&
                         attempts_before_resume == 0 &&
                         transport.object_write_attempts.size() == 1,
                     "expected initial Forward=0 to stay parked until REQUEST_UPDATE resumes it");
    }

    {
        MockTransport transport;
        transport.keep_open_streams.insert(0);
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "pending-paused", 1,
            DraftVersion::kDraft16, 1000, 0, 1, 1));
        transport.reads[0].push_back(encode_request_update_message(
            DraftVersion::kDraft16, 3, 0, 0x10, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "pending-paused", .group_id = 1,
             .subgroup_id = 0, .object_id = 0, .marker = 'P'},
        });
        std::size_t control_reads = 0;
        bool resume_queued = false;
        std::size_t attempts_before_resume = 0;
        transport.on_read = [&](MockTransport& current,
                                std::uint64_t stream_id) {
            if (stream_id != 0 || ++control_reads != 6) {
                return;
            }
            attempts_before_resume = current.object_write_attempts.size();
            resume_queued = true;
            current.reads[0].push_back(encode_request_update_message(
                DraftVersion::kDraft16, 5, 1, 0x10, 1));
        };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected pending Forward=0 session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok && resume_queued &&
                         attempts_before_resume == 0 &&
                         transport.object_write_attempts.size() == 1,
                     "expected draft-16 pending Forward=0 update to seed scheduler pause state");
    }

    {
        MockTransport transport;
        transport.keep_open_streams.insert(1);
        transport.reads[3].push_back(encode_draft18_setup_response());
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        std::vector<std::uint8_t> coalesced = encode_subscribe_message(
            91, kTestTrackNamespace, "events", 1, DraftVersion::kDraft18,
            1000, 0, 100, 1);
        append_bytes(coalesced,
                     encode_request_update_message(
                         DraftVersion::kDraft18, 93, 7, 0x20));
        transport.reads[1].push_back(std::move(coalesced));

        PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'C'},
        });
        plan.draft = openmoq::publisher::draft_profile(DraftVersion::kDraft18);

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected coalesced draft-18 request session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok,
                     "expected coalesced SUBSCRIBE and REQUEST_UPDATE frames to succeed");
        ok &= expect(transport.object_write_attempts.size() == 1 &&
                         transport.object_write_attempts.front()
                                 .options.transport_priority ==
                             openmoq::publisher::transport::
                                 priority_scheduler_internal::
                                     object_transport_priority_for_testing(7, 128),
                     "expected retained coalesced REQUEST_UPDATE to affect first object admission");
        const auto update_ok =
            openmoq::publisher::transport::encode_request_ok_message(
                DraftVersion::kDraft18, 93);
        ok &= expect(std::count_if(
                         transport.writes.begin(),
                         transport.writes.end(),
                         [&](const MockTransport::WriteEvent& write) {
                             return write.stream_id == 1 &&
                                    write.bytes == update_ok;
                         }) == 1,
                     "expected exactly one response to the coalesced REQUEST_UPDATE frame");
    }

    {
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            2, kTestTrackNamespace, "events", 0, DraftVersion::kDraft16));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'P'},
        });

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected draft-16 parity session connect to succeed");
        status = session.publish(plan);
        ok &= expect(!status.ok && transport.last_close_code == 0x04,
                     "expected draft-16 peer request parity failure to close with INVALID_REQUEST_ID");
    }

    {
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "events", 1, DraftVersion::kDraft16,
            0, 0, 20, 1, 0, 0, 0x04, 1));
        transport.reads[0].push_back(encode_request_update_filter_message(
            DraftVersion::kDraft16, 3, 1, 0x04, 0, 0, 4));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 3, .subgroup_id = 0,
             .object_id = 7, .marker = 'E'},
        });

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected draft-16 end-extension session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok,
                     "expected draft-16 subscription end extension to succeed");
        const auto expected_ok =
            openmoq::publisher::transport::encode_request_ok_message(
                DraftVersion::kDraft16, 3, 3, 7);
        ok &= expect(std::count_if(
                         transport.writes.begin(),
                         transport.writes.end(),
                         [&](const MockTransport::WriteEvent& write) {
                             return write.stream_id == 0 &&
                                    write.bytes == expected_ok;
                         }) == 1,
                     "expected draft-16 end-extension REQUEST_OK to include current LARGEST_OBJECT");
    }

    {
        MockTransport transport;
        transport.keep_open_streams.insert(1);
        transport.reads[3].push_back(encode_draft18_setup_response());
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        std::vector<std::uint8_t> coalesced = encode_subscribe_message(
            91, kTestTrackNamespace, "events", 1, DraftVersion::kDraft18,
            0, 0, 20, 1, 0, 0, 0x04, 1);
        append_bytes(coalesced, encode_request_update_filter_message(
                                    DraftVersion::kDraft18,
                                    93,
                                    std::nullopt,
                                    0x04,
                                    0,
                                    0,
                                    4));
        transport.reads[1].push_back(std::move(coalesced));
        PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'A'},
            {.track_name = "events", .group_id = 3, .subgroup_id = 0,
             .object_id = 7, .marker = 'E'},
        });
        plan.draft = openmoq::publisher::draft_profile(DraftVersion::kDraft18);

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected draft-18 end-extension session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok,
                     "expected draft-18 subscription end extension to succeed");
        const auto expected_ok =
            openmoq::publisher::transport::encode_request_ok_message(
                DraftVersion::kDraft18, 93, 3, 7);
        ok &= expect(std::count_if(
                         transport.writes.begin(),
                         transport.writes.end(),
                         [&](const MockTransport::WriteEvent& write) {
                             return write.stream_id == 1 &&
                                    write.bytes == expected_ok;
                         }) == 1,
                     "expected draft-18 end-extension REQUEST_OK to include current LARGEST_OBJECT without an id");
    }

    {
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            3, kTestTrackNamespace, "events", 0, DraftVersion::kDraft16));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'S'},
        });

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected draft-16 sequence session connect to succeed");
        status = session.publish(plan);
        ok &= expect(!status.ok && transport.last_close_code == 0x04,
                     "expected draft-16 skipped peer request id to close with INVALID_REQUEST_ID");
    }

    {
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            101, kTestTrackNamespace, "events", 0, DraftVersion::kDraft16));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'L'},
        });

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected draft-16 request-limit session connect to succeed");
        status = session.publish(plan);
        ok &= expect(!status.ok && transport.last_close_code == 0x07,
                     "expected draft-16 advertised request limit failure to close with TOO_MANY_REQUESTS");
    }

    {
        MockTransport transport;
        transport.reads[3].push_back(encode_draft18_setup_response());
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        transport.reads[1].push_back(encode_subscribe_message(
            92, kTestTrackNamespace, "events", 0, DraftVersion::kDraft18));
        PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'P'},
        });
        plan.draft = openmoq::publisher::draft_profile(DraftVersion::kDraft18);

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected draft-18 parity session connect to succeed");
        status = session.publish(plan);
        ok &= expect(!status.ok && transport.last_close_code == 0x04,
                     "expected draft-18 peer request parity failure to close with INVALID_REQUEST_ID");
    }

    {
        MockTransport transport;
        transport.reads[3].push_back(encode_draft18_setup_response());
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        transport.reads[1].push_back(encode_subscribe_message(
            91, kTestTrackNamespace, "events", 0, DraftVersion::kDraft18));
        transport.reads[5].push_back(encode_subscribe_message(
            91, kTestTrackNamespace, "events", 0, DraftVersion::kDraft18));
        PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .marker = 'D'},
        });
        plan.draft = openmoq::publisher::draft_profile(DraftVersion::kDraft18);

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected draft-18 duplicate-id session connect to succeed");
        status = session.publish(plan);
        ok &= expect(!status.ok && transport.last_close_code == 0x04,
                     "expected duplicate draft-18 peer request id to close with INVALID_REQUEST_ID");
    }

    {
        MockTransport transport;
        queue_draft16_scheduling_prefix(transport);
        transport.reads[0].push_back(encode_subscribe_message(
            1, kTestTrackNamespace, "events", 1, DraftVersion::kDraft16,
            0, 0, 0, 1));
        const PublishPlan plan = make_scheduling_plan({
            {.track_name = "events", .group_id = 5, .subgroup_id = 0,
             .object_id = 0, .marker = 'Z'},
        });

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected zero-timeout admission session connect to succeed");
        status = session.publish(plan);
        ok &= expect(status.ok, "expected zero-timeout object publish to succeed");
        ok &= expect(transport.object_write_attempts.size() == 1,
                     "expected a timeout-free object to use bounded try_write_object");
        if (!transport.object_write_attempts.empty()) {
            const auto& options = transport.object_write_attempts.front().options;
            ok &= expect(!options.object_deadline.has_value() &&
                             !options.subgroup_deadline.has_value(),
                         "expected zero timeout to retain absent object and subgroup deadlines");
            ok &= expect(options.transport_priority >= 2,
                         "expected object traffic never to outrank control or request classes");
        }
    }

    {
        MockTransport transport;
        transport.reads[3].push_back(encode_setup_message({
            .draft = DraftVersion::kDraft17,
        }));
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft17, 0));
        transport.reads[4].push_back(
            encode_publish_ok_message(DraftVersion::kDraft17, 2, 1));
        LiveObjectSource source{
            .tracks = {LiveTrack{.track_name = "events"}},
            .next_object = []() -> std::optional<LiveObject> {
                return std::nullopt;
            },
        };
        MoqtSession session(
            transport, std::string(kTestTrackNamespace), true, false, false,
            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected draft-17 priority-isolation session connect to succeed");
        status = session.publish_live_objects(source, DraftVersion::kDraft17);
        ok &= expect(status.ok,
                     "expected draft-17 priority-isolation publish to succeed");
        ok &= expect(transport.reliable_stream_priorities.empty(),
                     "expected draft-17 reliable writes to retain archived transport behavior");
    }

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
        ok &= expect(transport.reliable_stream_priorities.empty(),
                     "expected draft-14 reliable writes to retain archived transport behavior");
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
        std::vector<std::uint8_t> control =
            encode_archived_subscribe_update_message(1, 0);
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
        const auto catalog_subscribe_update =
            encode_archived_subscribe_update_message(7, 5);
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

    // The largest DELIVERY_TIMEOUT negotiated on any subscription bounds how
    // long the transport may hold close() while the last objects drain
    // (draft -16 9.15 / -18 10.11); the session hands it to the transport.
    {
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));
        transport.reads[0].push_back(
            encode_subscribe_message(1, kTestTrackNamespace, "catalog", 1, DraftVersion::kDraft16, 1500));
        transport.reads[0].push_back(
            encode_subscribe_message(3, kTestTrackNamespace, "vide_1", 1, DraftVersion::kDraft16, 4000));
        MoqtSession session(transport, std::string(kTestTrackNamespace));
        status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected delivery-timeout session connect to succeed");
        status = session.publish(materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft16), source_bytes));
        ok &= expect(status.ok, "expected delivery-timeout publish to succeed");
        ok &= expect(transport.delivery_timeouts ==
                         std::vector<std::chrono::milliseconds>({std::chrono::milliseconds(1500),
                                                                 std::chrono::milliseconds(4000)}),
                     "expected each negotiated DELIVERY_TIMEOUT to be handed to the transport");
    }

    {
        // Draft 16 section 9.2.2.2: a session-owned object that remains
        // blocked beyond DELIVERY_TIMEOUT resets its already-open subgroup
        // exactly once. The second object proves that the expired subgroup is
        // remembered rather than reopened on a fresh stream.
        using Clock = std::chrono::steady_clock;
        Clock::time_point now{};
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));
        transport.reads[0].push_back(
            encode_subscribe_message(1,
                                     kTestTrackNamespace,
                                     "events",
                                     1,
                                     DraftVersion::kDraft16,
                                     100));
        transport.on_try_write_object =
            [&now](MockTransport&, const MockTransport::ObjectWriteEvent&) {
                now += std::chrono::milliseconds(101);
                return ObjectWriteResult{ObjectWriteDisposition::kWouldBlock,
                                         "test transport stalled"};
            };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1),
                            {},
                            [&now]() { return now; });
        status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-16 deadline session connect to succeed");
        status = session.publish(make_delivery_deadline_plan(DraftVersion::kDraft16));
        ok &= expect(status.ok, "expected draft-16 object expiry to complete the subscription");
        ok &= expect(transport.object_write_attempts.size() == 1,
                     "expected the later object in an expired draft-16 subgroup not to be retried");
        ok &= expect(transport.reset_calls.size() == 1,
                     "expected exactly one draft-16 DELIVERY_TIMEOUT reset");
        if (!transport.object_write_attempts.empty() && !transport.reset_calls.empty()) {
            const auto stream_id = transport.object_write_attempts.front().stream_id;
            ok &= expect(transport.reset_calls.front() == std::pair<std::uint64_t, std::uint64_t>{stream_id, 0x02},
                         "expected draft-16 expiry to reset the exact subgroup stream with 0x02");
            ok &= expect(transport.object_write_attempts.front().options.object_deadline ==
                             Clock::time_point{} + std::chrono::milliseconds(100),
                         "expected draft-16 object deadline to retain the first-availability timestamp");
            ok &= expect(!transport.object_write_attempts.front().options.subgroup_deadline.has_value(),
                         "expected draft-16 not to arm a draft-18 subgroup deadline");
        }
        const auto unidirectional_opens = static_cast<std::size_t>(std::count_if(
            transport.opens.begin(), transport.opens.end(), [](const MockTransport::OpenEvent& event) {
                return event.direction == StreamDirection::kUnidirectional;
            }));
        ok &= expect(unidirectional_opens == 1,
                     "expected an expired draft-16 subgroup never to reopen");
    }

    {
        // Draft 18 section 8 keeps OBJECT_DELIVERY_TIMEOUT independent from
        // the longer subgroup value. The complete application-provided
        // subgroup owns a 500 ms completion deadline, while the first object's
        // stalled admission still expires at its earlier 100 ms deadline.
        using Clock = std::chrono::steady_clock;
        Clock::time_point now{};
        MockTransport transport;
        transport.reads[3].push_back(encode_draft18_setup_response());
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        transport.reads[1].push_back(
            encode_subscribe_message(1,
                                     kTestTrackNamespace,
                                     "events",
                                     1,
                                     DraftVersion::kDraft18,
                                     100,
                                     500));
        transport.on_try_write_object =
            [&now](MockTransport&, const MockTransport::ObjectWriteEvent&) {
                now += std::chrono::milliseconds(101);
                return ObjectWriteResult{ObjectWriteDisposition::kWouldBlock,
                                         "test transport stalled"};
            };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1),
                            {},
                            [&now]() { return now; });
        status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 object-deadline session connect to succeed");
        status = session.publish(make_delivery_deadline_plan(DraftVersion::kDraft18));
        ok &= expect(status.ok, "expected draft-18 object expiry to complete the subscription");
        ok &= expect(transport.object_write_attempts.size() == 1,
                     "expected draft-18 object expiry to suppress the later subgroup object");
        const auto delivery_timeout_resets = static_cast<std::size_t>(std::count_if(
            transport.reset_calls.begin(), transport.reset_calls.end(),
            [](const auto& reset) { return reset.second == 0x02; }));
        ok &= expect(delivery_timeout_resets == 1,
                     "expected exactly one draft-18 object-timeout reset");
        const auto delivery_reset = std::find_if(
            transport.reset_calls.begin(), transport.reset_calls.end(),
            [](const auto& reset) { return reset.second == 0x02; });
        if (!transport.object_write_attempts.empty() && delivery_reset != transport.reset_calls.end()) {
            const auto& attempt = transport.object_write_attempts.front();
            ok &= expect(attempt.options.object_deadline ==
                             Clock::time_point{} + std::chrono::milliseconds(100),
                         "expected draft-18 object timeout to use its independent 100 ms value");
            ok &= expect(attempt.options.subgroup_deadline ==
                             Clock::time_point{} +
                                 std::chrono::milliseconds(500),
                         "expected every remaining object to inherit the generation-owned subgroup completion deadline");
            ok &= expect(*delivery_reset ==
                             std::pair<std::uint64_t, std::uint64_t>{attempt.stream_id, 0x02},
                         "expected draft-18 object expiry to reset the exact subgroup stream with 0x02");
        }
    }

    {
        // Reverse the draft-18 timeout values. The complete batch subgroup is
        // application-available at the generation boundary, so its 100 ms
        // completion deadline constrains the first write as well as the final
        // object while it waits behind that predecessor.
        using Clock = std::chrono::steady_clock;
        Clock::time_point now{};
        MockTransport transport;
        transport.reads[3].push_back(encode_draft18_setup_response());
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        transport.reads[1].push_back(
            encode_subscribe_message(1,
                                     kTestTrackNamespace,
                                     "events",
                                     1,
                                     DraftVersion::kDraft18,
                                     500,
                                     100));
        transport.on_try_write_object =
            [&now](MockTransport&, const MockTransport::ObjectWriteEvent& event) {
                if (!event.fin) {
                    now += std::chrono::milliseconds(101);
                }
                return ObjectWriteResult{ObjectWriteDisposition::kAccepted, {}};
            };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1),
                            {},
                            [&now]() { return now; });
        status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 subgroup-deadline session connect to succeed");
        status = session.publish(make_delivery_deadline_plan(DraftVersion::kDraft18));
        ok &= expect(status.ok, "expected draft-18 final subgroup publication to succeed");
        ok &= expect(transport.object_write_attempts.size() == 1,
                     "expected final subgroup object to expire while waiting for its predecessor");
        ok &= expect(std::any_of(transport.reset_calls.begin(),
                                 transport.reset_calls.end(),
                                 [](const auto& reset) {
                                     return reset.second == 0x02;
                                 }),
                     "expected the aged subgroup-completion deadline to reset the open stream");
    }

    {
        // A timeout can win on the packet-loop thread after admission has
        // returned. The next application object must observe that stable
        // per-stream state before writing: the expired subgroup is retired
        // without failing the subscription, while an independent subgroup
        // still publishes and the later expired-key object stays suppressed.
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));
        transport.reads[0].push_back(
            encode_subscribe_message(1,
                                     kTestTrackNamespace,
                                     "events",
                                     1,
                                     DraftVersion::kDraft16,
                                     1000));
        transport.on_try_write_object =
            [](MockTransport& current, const MockTransport::ObjectWriteEvent& event) {
                if (current.object_write_attempts.size() == 1) {
                    current.expired_media_streams.insert(event.stream_id);
                }
                return ObjectWriteResult{ObjectWriteDisposition::kAccepted, {}};
            };

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected pre-admission-expiry session connect to succeed");
        status = session.publish(make_transport_expiry_plan(DraftVersion::kDraft16));
        ok &= expect(status.ok, "expected transport-owned expiry to remain nonfatal");
        ok &= expect(transport.object_write_attempts.size() == 2,
                     "expected both later objects for the expired subgroup to be suppressed while another subgroup publishes");
        if (transport.object_write_attempts.size() == 2) {
            ok &= expect(transport.object_write_attempts[0].stream_id !=
                             transport.object_write_attempts[1].stream_id,
                         "expected the post-expiry write to belong to the independent subgroup");
        }
    }

    {
        // Close the check-versus-write race: the stream is healthy at the
        // pre-admission query, then expires while try_write_object decides its
        // result. The post-failure query must distinguish timeout from a
        // terminal transport failure and let another subgroup continue.
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));
        transport.reads[0].push_back(
            encode_subscribe_message(1,
                                     kTestTrackNamespace,
                                     "events",
                                     1,
                                     DraftVersion::kDraft16,
                                     1000));
        transport.on_try_write_object =
            [](MockTransport& current, const MockTransport::ObjectWriteEvent& event) {
                if (current.object_write_attempts.size() == 2) {
                    current.expired_media_streams.insert(event.stream_id);
                    return ObjectWriteResult{ObjectWriteDisposition::kFailed,
                                             "simulated packet-loop expiry"};
                }
                return ObjectWriteResult{ObjectWriteDisposition::kAccepted, {}};
            };

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected check-write-race session connect to succeed");
        status = session.publish(make_transport_expiry_plan(DraftVersion::kDraft16));
        ok &= expect(status.ok,
                     "expected kFailed paired with stable stream expiry to retire only that subgroup");
        ok &= expect(transport.object_write_attempts.size() == 3,
                     "expected one raced failure, one continuing subgroup write, and no expired-key reopen");
        if (transport.object_write_attempts.size() == 3) {
            ok &= expect(transport.object_write_attempts[0].stream_id ==
                             transport.object_write_attempts[1].stream_id &&
                             transport.object_write_attempts[2].stream_id !=
                                 transport.object_write_attempts[0].stream_id,
                         "expected the race on the original stream followed by progress on an independent stream");
        }
    }

    {
        // Once a FIN is accepted the subgroup is permanently complete. A
        // duplicate late application publication for that exact key must not
        // open a second stream; an intervening subgroup remains unaffected.
        MockTransport transport;
        transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));
        transport.reads[0].push_back(
            encode_subscribe_message(1,
                                     kTestTrackNamespace,
                                     "events",
                                     1,
                                     DraftVersion::kDraft16,
                                     1000));

        MoqtSession session(transport, std::string(kTestTrackNamespace));
        status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected completed-subgroup session connect to succeed");
        status = session.publish(make_completed_subgroup_reopen_plan(DraftVersion::kDraft16));
        ok &= expect(status.ok, "expected a duplicate completed-key publication to be ignored nonfatally");
        ok &= expect(transport.object_write_attempts.size() == 2,
                     "expected a completed subgroup not to reopen while another subgroup publishes");
    }

    MockTransport draft16_transport;
    draft16_transport.reads[0].push_back(encode_server_setup_message({
        .draft = DraftVersion::kDraft16,
        .max_request_id = 8,
    }));
    queue_subscribe_requests(draft16_transport,
                             DraftVersion::kDraft16,
                             kTestTrackNamespace,
                             {{1, "catalog"}, {3, "vide_1"}},
                             false,
                             1);
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
        queue_subscribe_requests(auth_transport,
                                 DraftVersion::kDraft16,
                                 kTestTrackNamespace,
                                 {{1, "catalog"}, {3, "vide_1"}},
                                 false,
                                 1);
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
            encode_subscribe_message(91, kTestTrackNamespace, "vide_1", 1, DraftVersion::kDraft18);
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
        // Namespace request stream receives the removed standalone PUBLISH_OK
        // type instead of draft-18 REQUEST_OK.
        std::vector<std::uint8_t> removed_publish_ok_type =
            encode_publish_ok_message(DraftVersion::kDraft18, 9, 1);
        removed_publish_ok_type[0] = 0x1e;
        draft18_wrong_type_transport.reads[0].push_back(removed_publish_ok_type);

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
        // Regression for the stack owner's required behavior: once publish_live()
        // has sent PUBLISH_NAMESPACE, a relay that never acknowledges it must NOT
        // cause the session to tear down. moqxr should still publish its tracks
        // and enter the await-subscriptions loop, exiting only via the normal
        // subscriber-timeout idle path (or when the RTMP/stdin source ends), not
        // because the ack never arrived. Before the fix, collect_control_
        // acknowledgements()'s read_stream timeout ("timed out waiting for
        // stream data") was propagated straight out of publish_live() as fatal.
        MockTransport no_ack_transport;
        no_ack_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        // Deliberately no PUBLISH_NAMESPACE_OK/REQUEST_OK queued for stream 0:
        // once the setup message is consumed, every further read on the control
        // stream reports the same failure a real transport reports for a
        // genuine per-read timeout with a healthy connection.
        no_ack_transport.missing_read_error = "timed out waiting for stream data";

        MoqtSession no_ack_session(
            no_ack_transport, std::string(kTestTrackNamespace), false, false, false, std::chrono::seconds(1));
        status = no_ack_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected no-ack session connect to succeed");

        const auto no_ack_bytes = make_live_init_mp4();
        std::string no_ack_input_bytes(no_ack_bytes.begin(), no_ack_bytes.end());
        std::istringstream no_ack_input(no_ack_input_bytes);
        status = no_ack_session.publish_live(no_ack_input, DraftVersion::kDraft16, false);
        ok &= expect(status.ok,
                     "expected publish_live to tolerate a missing namespace acknowledgement "
                     "rather than tearing the session down");
        ok &= expect(control_message_count(no_ack_transport, 0x1d) == 1,
                     "expected publish_live to still preannounce media tracks after a missing ack");
        ok &= expect(!no_ack_transport.writes.empty() &&
                         message_type(no_ack_transport.writes.back().bytes) == 0x09,
                     "expected publish_live to still reach a clean PUBLISH_NAMESPACE_DONE exit "
                     "via the normal idle await-subscribe timeout, not an error return");
    }

    {
        // Companion regression: an acknowledgement that arrives late (after
        // collect_control_acknowledgements() has already given up and
        // publish_live() has moved on) must be consumed harmlessly by the main
        // control-message loop rather than being lost or mis-parsed as a
        // protocol violation. PUBLISH_NAMESPACE_OK/REQUEST_OK (type 0x07) is not
        // one of the message types process_control_messages() acts on, so it
        // should simply be drained off pending_control_bytes_ once it shows up.
        MockTransport late_ack_transport;
        late_ack_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        // No ack queued yet -- the first collect_control_acknowledgements() read
        // times out (same mechanism as the no-ack case above). on_read injects
        // the ack just before the *second* read of stream 0, modeling a relay
        // that answers slightly later than the initial short wait -- this is
        // read by the main await-subscribe loop, not by
        // collect_control_acknowledgements() (which has already returned).
        std::size_t stream0_reads = 0;
        late_ack_transport.on_read = [&stream0_reads](MockTransport& transport, std::uint64_t stream_id) {
            if (stream_id != 0) {
                return;
            }
            ++stream0_reads;
            if (stream0_reads == 2) {
                transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));
            }
        };

        MoqtSession late_ack_session(
            late_ack_transport, std::string(kTestTrackNamespace), false, false, false, std::chrono::seconds(1));
        status = late_ack_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected late-ack session connect to succeed");

        const auto late_ack_bytes = make_live_init_mp4();
        std::string late_ack_input_bytes(late_ack_bytes.begin(), late_ack_bytes.end());
        std::istringstream late_ack_input(late_ack_input_bytes);
        status = late_ack_session.publish_live(late_ack_input, DraftVersion::kDraft16, false);
        ok &= expect(status.ok,
                     "expected a late-arriving namespace acknowledgement to be consumed without "
                     "surfacing as a protocol violation");
        ok &= expect(stream0_reads >= 2,
                     "expected the main loop to perform a second read that picks up the late ack");
        ok &= expect(!late_ack_transport.writes.empty() &&
                         message_type(late_ack_transport.writes.back().bytes) == 0x09,
                     "expected the late-ack session to still reach a clean PUBLISH_NAMESPACE_DONE exit");
    }

    {
        // Phase 5 regression: CMSF 4.1.2 makes an absent contentProtectionRefIDs
        // mean "not protected", so a CENC-protected track (encv/sinf/schm/schi/
        // tenc) whose init segment carries no pssh anywhere makes
        // build_live_catalog() throw std::runtime_error rather than silently
        // publishing a catalog that misdescribes encrypted media as clear. The
        // stdin live path (publish_live) must catch that and return a failure
        // status instead of letting the exception unwind past this function --
        // in production, Publisher::publish_live's teardown (session->close(0)
        // + clear_active_session(), which populates stats_.last_error) only
        // runs when publish_live() returns rather than throws. This asserts
        // three things: (1) a failure status comes back rather than the
        // process aborting on an uncaught exception, (2) the message names the
        // missing pssh, and (3) the session is left in a state where the
        // caller's normal teardown (modeled here by close(0), matching
        // publisher_api.cpp's `active->session->close(0)`) still runs
        // cleanly and is observable on the transport -- so a future
        // regression that returns failure without leaving the session in a
        // closeable state still fails this test.
        MockTransport cenc_no_pssh_transport;
        cenc_no_pssh_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        cenc_no_pssh_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));

        MoqtSession cenc_no_pssh_session(
            cenc_no_pssh_transport, std::string(kTestTrackNamespace), false, false, false, std::chrono::seconds(1));
        status = cenc_no_pssh_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected CENC-no-pssh live session connect to succeed");

        const auto cenc_no_pssh_bytes = make_live_init_mp4_cenc_no_pssh();
        std::string cenc_no_pssh_input_bytes(cenc_no_pssh_bytes.begin(), cenc_no_pssh_bytes.end());
        std::istringstream cenc_no_pssh_input(cenc_no_pssh_input_bytes);
        status = cenc_no_pssh_session.publish_live(cenc_no_pssh_input, DraftVersion::kDraft14, false);
        ok &= expect(!status.ok,
                     "expected a CENC-protected track with no pssh to refuse the stdin live "
                     "publish rather than letting build_live_catalog's throw escape");
        ok &= expect(status.message.find("pssh") != std::string::npos,
                     "expected the failure status to name the missing pssh");
        ok &= expect(control_message_count(cenc_no_pssh_transport, 0x06) == 0,
                     "expected the refusal to bail out before announcing the namespace");

        const TransportStatus cenc_no_pssh_close_status = cenc_no_pssh_session.close(0);
        ok &= expect(cenc_no_pssh_close_status.ok,
                     "expected the caller's normal teardown (close(0)) to still succeed after "
                     "the guard returns a failure status");
        ok &= expect(cenc_no_pssh_transport.state() == ConnectionState::kClosed &&
                         cenc_no_pssh_transport.last_close_code == 0,
                     "expected teardown to actually reach the transport -- proving the session "
                     "was left in a closeable state rather than mid-unwind");
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
        // The catalog timeout pair is negotiated inside publish_live(), but
        // the final independent catalog is sent later by end_broadcast().
        // Keep both draft-18 values across that boundary: stalling the final
        // object beyond its shorter object deadline must reset the exact new
        // catalog stream with DELIVERY_TIMEOUT rather than taking the legacy
        // timeout-free write path.
        using Clock = std::chrono::steady_clock;
        Clock::time_point now{};
        bool final_catalog_phase = false;
        MockTransport transport;
        transport.reads[3].push_back(encode_draft18_setup_response());
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        transport.reads[1].push_back(
            encode_subscribe_message(1,
                                     kTestTrackNamespace,
                                     "catalog",
                                     0,
                                     DraftVersion::kDraft18,
                                     100,
                                     250));
        transport.on_try_write_object =
            [&now, &final_catalog_phase](MockTransport&,
                                         const MockTransport::ObjectWriteEvent&) {
                if (final_catalog_phase) {
                    now += std::chrono::milliseconds(101);
                    return ObjectWriteResult{ObjectWriteDisposition::kWouldBlock,
                                             "stall final catalog"};
                }
                return ObjectWriteResult{ObjectWriteDisposition::kAccepted, {}};
            };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            true,
                            false,
                            false,
                            false,
                            std::chrono::seconds(1),
                            {},
                            [&now]() { return now; });
        status = session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 final-catalog deadline session connect to succeed");

        const auto live_bytes = make_live_init_mp4();
        std::string live_input_bytes(live_bytes.begin(), live_bytes.end());
        std::istringstream live_input(live_input_bytes);
        status = session.publish_live(live_input, DraftVersion::kDraft18, false);
        ok &= expect(status.ok, "expected initial draft-18 live catalog publish to succeed");

        const std::size_t attempts_before_final = transport.object_write_attempts.size();
        const std::size_t resets_before_final = transport.reset_calls.size();
        final_catalog_phase = true;
        status = session.end_broadcast(
            openmoq::publisher::EndBroadcastMode::kTerminate, DraftVersion::kDraft18);
        ok &= expect(status.ok, "expected final catalog deadline expiry to remain nonfatal");
        ok &= expect(transport.object_write_attempts.size() == attempts_before_final + 1,
                     "expected end_broadcast to forward the retained timeout pair through object admission");
        ok &= expect(transport.reset_calls.size() == resets_before_final + 1,
                     "expected one final-catalog delivery-timeout reset");
        if (transport.object_write_attempts.size() == attempts_before_final + 1 &&
            transport.reset_calls.size() == resets_before_final + 1) {
            const auto& attempt = transport.object_write_attempts.back();
            ok &= expect(attempt.options.object_deadline ==
                             Clock::time_point{} + std::chrono::milliseconds(100),
                         "expected final catalog to retain the 100 ms object timeout");
            ok &= expect(attempt.options.subgroup_deadline ==
                             Clock::time_point{} + std::chrono::milliseconds(250),
                         "expected final catalog to retain the independent 250 ms subgroup timeout");
            ok &= expect(transport.reset_calls.back() ==
                             std::pair<std::uint64_t, std::uint64_t>{attempt.stream_id, 0x02},
                         "expected final catalog expiry to reset its exact stream with 0x02");
        }
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
        // Preannounce is opt-in; this case exists to exercise it.
        object_live_session.set_preannounce_tracks(true);
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
        // Default (preannounce off): no PUBLISH is emitted before a subscriber
        // exists. The same flow as above with the opt-in left at its default, so a
        // regression in the gate shows up as a count change rather than silently.
        MockTransport default_no_preannounce_transport;
        default_no_preannounce_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        default_no_preannounce_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        default_no_preannounce_transport.reads[0].push_back({});

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

        MoqtSession default_no_preannounce_session(default_no_preannounce_transport,
                                                   std::string(kTestTrackNamespace),
                                                   false,
                                                   false,
                                                   false,
                                                   std::chrono::seconds(1));
        status = default_no_preannounce_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected default live-object session connect to succeed");
        status = default_no_preannounce_session.publish_live_objects(source, DraftVersion::kDraft14);
        ok &= expect(control_message_count(default_no_preannounce_transport, 0x1d) == 0,
                     "expected no PUBLISH preannounce when preannounce_tracks is left at its default");
    }

    {
        // Regression: draft-18 assigns a track alias in PUBLISH, so an
        // auto-forward live-object publisher must establish every track on a
        // request stream before it emits subgroup objects using those aliases.
        MockTransport draft18_object_live_transport;
        draft18_object_live_transport.reads[3].push_back(encode_draft18_setup_response());
        draft18_object_live_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        draft18_object_live_transport.reads[4].push_back(
            encode_publish_ok_message(DraftVersion::kDraft18, 2, 1));
        draft18_object_live_transport.reads[8].push_back(
            encode_publish_ok_message(DraftVersion::kDraft18, 4, 1));

        std::vector<LiveObject> objects = {
            LiveObject{
                .track_name = "catalog",
                .group_id = 0,
                .subgroup_id = 0,
                .object_id = 0,
                .payload = {'{', '}'},
            },
            LiveObject{
                .track_name = "video0_vide_1",
                .group_id = 1,
                .subgroup_id = 0,
                .object_id = 0,
                .payload = {'M'},
            },
        };
        std::size_t object_index = 0;
        LiveObjectSource source{
            .tracks = {
                LiveTrack{.track_name = "catalog"},
                LiveTrack{.track_name = "video0_vide_1"},
            },
            .next_object = [&objects, &object_index]() -> std::optional<LiveObject> {
                if (object_index >= objects.size()) {
                    return std::nullopt;
                }
                return objects[object_index++];
            },
            .catalog_mode = LiveCatalogMode::kSourceObject,
        };

        MoqtSession draft18_object_live_session(
            draft18_object_live_transport,
            std::string(kTestTrackNamespace),
            true,
            true,
            false,
            std::chrono::seconds(1));
        status = draft18_object_live_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 auto-forward live-object session connect to succeed");
        status = draft18_object_live_session.publish_live_objects(source, DraftVersion::kDraft18);
        ok &= expect(status.ok, "expected draft-18 auto-forward live-object publish to succeed");
        ok &= expect(!draft18_object_live_transport.reads.contains(4) &&
                         !draft18_object_live_transport.reads.contains(8),
                     "expected draft-18 live-object publish to wait for both PUBLISH_OK responses");
        ok &= expect(draft18_object_live_session.publish_stats().objects_published == 2,
                     "expected draft-18 auto-forward to publish catalog and media objects");

        std::size_t catalog_publish_index = draft18_object_live_transport.writes.size();
        std::size_t media_publish_index = draft18_object_live_transport.writes.size();
        std::size_t first_object_index = draft18_object_live_transport.writes.size();
        std::size_t publish_done_count = 0;
        bool publish_done_with_fin = true;
        for (std::size_t index = 0; index < draft18_object_live_transport.writes.size(); ++index) {
            const auto& write = draft18_object_live_transport.writes[index];
            if (write.stream_id == 4 && message_type(write.bytes) == 0x1d) {
                catalog_publish_index = index;
            } else if (write.stream_id == 8 && message_type(write.bytes) == 0x1d) {
                media_publish_index = index;
            } else if (write.stream_id >= 6 && (write.stream_id & 0x3ULL) == 0x2ULL) {
                first_object_index = std::min(first_object_index, index);
            }
            if ((write.stream_id == 4 || write.stream_id == 8) && message_type(write.bytes) == 0x0b) {
                ++publish_done_count;
                publish_done_with_fin = publish_done_with_fin && write.fin;
            }
        }
        ok &= expect(first_object_index < draft18_object_live_transport.writes.size(),
                     "expected draft-18 auto-forward to emit an object stream");
        ok &= expect(catalog_publish_index < first_object_index,
                     "expected draft-18 catalog PUBLISH before the first live object");
        ok &= expect(media_publish_index < first_object_index,
                     "expected draft-18 media PUBLISH before the first live object");
        ok &= expect(publish_done_count == 2 && publish_done_with_fin,
                     "expected one final PUBLISH_DONE per draft-18 PUBLISH stream");
    }

    {
        // A successful PUBLISH with Forward=0 establishes the aliases but
        // does not authorize auto-forward object delivery.
        MockTransport draft18_no_forward_transport;
        draft18_no_forward_transport.reads[3].push_back(encode_draft18_setup_response());
        draft18_no_forward_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        draft18_no_forward_transport.reads[4].push_back(
            encode_publish_ok_message(DraftVersion::kDraft18, 2, 0));
        draft18_no_forward_transport.reads[8].push_back(
            encode_publish_ok_message(DraftVersion::kDraft18, 4, 0));

        std::vector<LiveObject> objects = {
            LiveObject{
                .track_name = "catalog",
                .group_id = 0,
                .subgroup_id = 0,
                .object_id = 0,
                .payload = {'{', '}'},
            },
            LiveObject{
                .track_name = "video0_vide_1",
                .group_id = 1,
                .subgroup_id = 0,
                .object_id = 0,
                .payload = {'M'},
            },
        };
        std::size_t object_index = 0;
        LiveObjectSource source{
            .tracks = {
                LiveTrack{.track_name = "catalog"},
                LiveTrack{.track_name = "video0_vide_1"},
            },
            .next_object = [&objects, &object_index]() -> std::optional<LiveObject> {
                if (object_index >= objects.size()) {
                    return std::nullopt;
                }
                return objects[object_index++];
            },
            .catalog_mode = LiveCatalogMode::kSourceObject,
        };

        MoqtSession draft18_no_forward_session(
            draft18_no_forward_transport,
            std::string(kTestTrackNamespace),
            true,
            true,
            false,
            std::chrono::seconds(1));
        status = draft18_no_forward_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 Forward=0 live-object session connect to succeed");
        status = draft18_no_forward_session.publish_live_objects(source, DraftVersion::kDraft18);
        ok &= expect(status.ok, "expected draft-18 Forward=0 live-object publish to succeed");

        const bool sent_object = std::any_of(
            draft18_no_forward_transport.writes.begin(),
            draft18_no_forward_transport.writes.end(),
            [](const MockTransport::WriteEvent& write) {
                return write.stream_id >= 6 && (write.stream_id & 0x3ULL) == 0x2ULL;
            });
        ok &= expect(!sent_object, "expected draft-18 Forward=0 to suppress auto-forward objects");
        ok &= expect(draft18_no_forward_session.publish_stats().objects_published == 0,
                     "expected draft-18 Forward=0 not to count suppressed objects");
        ok &= expect(object_index == 0,
                     "expected draft-18 Forward=0 to leave source objects queued for a later subscriber");
    }

    {
        MockTransport draft18_invalid_publish_ok_transport;
        draft18_invalid_publish_ok_transport.reads[3].push_back(encode_draft18_setup_response());
        draft18_invalid_publish_ok_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        draft18_invalid_publish_ok_transport.reads[4].push_back(
            encode_publish_ok_message(DraftVersion::kDraft18, 2, 2));
        LiveObjectSource source{
            .tracks = {LiveTrack{.track_name = "events"}},
            .next_object = []() -> std::optional<LiveObject> { return std::nullopt; },
        };

        MoqtSession draft18_invalid_publish_ok_session(
            draft18_invalid_publish_ok_transport, std::string(kTestTrackNamespace), true);
        status = draft18_invalid_publish_ok_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected invalid draft-18 PUBLISH_OK session connect to succeed");
        status = draft18_invalid_publish_ok_session.publish_live_objects(source, DraftVersion::kDraft18);
        ok &= expect(!status.ok && status.message == "invalid draft-18 PUBLISH_OK",
                     "expected invalid draft-18 PUBLISH_OK to be rejected without generic fallback");
        ok &= expect(draft18_invalid_publish_ok_transport.last_close_code == 0x3,
                     "expected invalid draft-18 PUBLISH_OK to close with PROTOCOL_VIOLATION");
    }

    {
        MockTransport transport;
        transport.reads[3].push_back(encode_draft18_setup_response());
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        transport.reads[3].push_back(encode_request_update_message(
            DraftVersion::kDraft18, 91, 0, 0x10));
        LiveObjectSource source{
            .tracks = {LiveTrack{.track_name = "events"}},
            .next_object = []() -> std::optional<LiveObject> {
                return std::nullopt;
            },
        };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            std::chrono::seconds(1));
        status = session.connect(endpoint, tls);
        ok &= expect(status.ok,
                     "expected misplaced live REQUEST_UPDATE session connect to succeed");
        status = session.publish_live_objects(source, DraftVersion::kDraft18);
        ok &= expect(!status.ok &&
                         status.message ==
                             "draft-18 request message received on control stream",
                     "expected live dispatcher to reject draft-18 REQUEST_UPDATE on the control stream");
        ok &= expect(transport.last_close_code == 0x3,
                     "expected misplaced live REQUEST_UPDATE to close with PROTOCOL_VIOLATION");
    }

    for (const DraftVersion draft :
         {DraftVersion::kDraft16, DraftVersion::kDraft18}) {
        MockTransport transport;
        const std::uint64_t response_stream_id =
            draft == DraftVersion::kDraft16 ? 0 : 1;
        if (draft == DraftVersion::kDraft16) {
            transport.keep_open_streams.insert(0);
            transport.reads[0].push_back(encode_server_setup_message({
                .draft = draft,
                .max_request_id = 16,
            }));
            transport.reads[0].push_back(
                encode_publish_namespace_ok_message(draft, 0));
            transport.reads[0].push_back(encode_subscribe_message(
                1, kTestTrackNamespace, "events", 0, draft,
                1000, 0, 100, 1));
            transport.reads[0].push_back(encode_request_update_message(
                draft, 3, 7, 0x20, 1));
            transport.reads[0].push_back(encode_request_update_message(
                draft, 5, 1, 0x10, 1));
        } else {
            transport.keep_open_streams.insert(1);
            transport.reads[3].push_back(encode_draft18_setup_response());
            transport.reads[0].push_back(
                encode_publish_namespace_ok_message(draft, 0));
            std::vector<std::uint8_t> coalesced =
                encode_subscribe_message(
                    91, kTestTrackNamespace, "events", 0, draft,
                    1000, 0, 100, 1);
            append_bytes(coalesced,
                         encode_request_update_message(
                             draft, 93, 7, 0x20));
            append_bytes(coalesced,
                         encode_request_update_message(
                             draft, 95, 1, 0x10));
            transport.reads[1].push_back(std::move(coalesced));
        }

        std::size_t object_index = 0;
        LiveObjectSource source{
            .tracks = {LiveTrack{.track_name = "events"}},
            .next_object = [&object_index]() -> std::optional<LiveObject> {
                if (object_index++ != 0) {
                    return std::nullopt;
                }
                return LiveObject{
                    .track_name = "events",
                    .group_id = 1,
                    .subgroup_id = 0,
                    .object_id = 0,
                    .payload = {'M'},
                };
            },
        };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            std::chrono::seconds(1));
        status = session.connect(endpoint, tls);
        ok &= expect(status.ok,
                     "expected live subscriber-update session connect to succeed");
        status = session.publish_live_objects(source, draft);
        ok &= expect(status.ok && object_index == 2 &&
                         session.publish_stats().objects_published == 1,
                     "expected live subscriber Forward update to release exactly one queued object");
        ok &= expect(!transport.object_write_attempts.empty() &&
                         transport.object_write_attempts.front()
                                 .options.transport_priority ==
                             openmoq::publisher::transport::
                                 priority_scheduler_internal::
                                     object_transport_priority_for_testing(
                                         7, 128),
                     "expected live subscriber priority update to reach object admission");
        if (draft == DraftVersion::kDraft16) {
            const auto priority_ok =
                openmoq::publisher::transport::encode_request_ok_message(
                    draft, 3);
            const auto forward_ok =
                openmoq::publisher::transport::encode_request_ok_message(
                    draft, 5);
            ok &= expect(std::any_of(
                             transport.writes.begin(),
                             transport.writes.end(),
                             [&](const MockTransport::WriteEvent& write) {
                                 return write.stream_id == response_stream_id &&
                                        write.bytes == priority_ok;
                             }) &&
                             std::any_of(
                                 transport.writes.begin(),
                                 transport.writes.end(),
                                 [&](const MockTransport::WriteEvent& write) {
                                     return write.stream_id == response_stream_id &&
                                            write.bytes == forward_ok;
                                 }),
                         "expected draft-16 live REQUEST_UPDATE acknowledgements before publication to use each fresh request id and the short wire shape");
        } else {
            const auto request_ok =
                openmoq::publisher::transport::encode_request_ok_message(
                    draft, 93);
            ok &= expect(std::count_if(
                             transport.writes.begin(),
                             transport.writes.end(),
                             [&](const MockTransport::WriteEvent& write) {
                                 return write.stream_id == response_stream_id &&
                                        write.bytes == request_ok;
                             }) == 2,
                         "expected two short ID-less REQUEST_OK frames before publication on the retained draft-18 subscriber stream");
        }
    }

    {
        MockTransport transport;
        transport.keep_open_streams.insert(0);
        queue_draft16_scheduling_prefix(transport);
        std::vector<std::uint8_t> subscriptions = encode_subscribe_message(
            1, kTestTrackNamespace, "events", 1, DraftVersion::kDraft16,
            0, 0, 100, 1, 1, 0, 0x04, 1);
        append_bytes(subscriptions, encode_subscribe_message(
            3, kTestTrackNamespace, "events", 1, DraftVersion::kDraft16,
            0, 0, 100, 1, 2, 0, 0x04, 2));
        transport.reads[0].push_back(std::move(subscriptions));

        std::vector<LiveObject> objects{
            {.track_name = "events", .group_id = 1, .subgroup_id = 0,
             .object_id = 0, .payload = {'A'},
             .subgroup_contains_group_largest = true,
             .final_in_subgroup = true},
            {.track_name = "events", .group_id = 2, .subgroup_id = 0,
             .object_id = 0, .payload = {'B'},
             .subgroup_contains_group_largest = true,
             .final_in_subgroup = true},
            {.track_name = "events", .group_id = 3, .subgroup_id = 0,
             .object_id = 0, .payload = {'C'},
             .subgroup_contains_group_largest = true,
             .final_in_subgroup = true},
        };
        std::size_t object_index = 0;
        LiveObjectSource source{
            .tracks = {LiveTrack{.track_name = "events"}},
            .next_object = [&]() -> std::optional<LiveObject> {
                if (object_index >= objects.size()) {
                    return std::nullopt;
                }
                return objects[object_index++];
            },
        };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected live filter-union session connect to succeed");
        status = session.publish_live_objects(source, DraftVersion::kDraft16);
        std::vector<std::uint8_t> admitted_markers;
        for (const auto& attempt : transport.object_write_attempts) {
            if (!attempt.bytes.empty()) {
                admitted_markers.push_back(attempt.bytes.back());
            }
        }
        ok &= expect(status.ok && admitted_markers ==
                         std::vector<std::uint8_t>{'A', 'B'},
                     "expected the union of active subscriber filters to admit groups 1 and 2 but reject group 3");
    }

    for (const DraftVersion draft :
         {DraftVersion::kDraft16, DraftVersion::kDraft18}) {
        MockTransport transport;
        const std::uint64_t response_stream_id =
            draft == DraftVersion::kDraft16 ? 0 : 1;
        if (draft == DraftVersion::kDraft16) {
            transport.keep_open_streams.insert(0);
            queue_draft16_scheduling_prefix(transport);
            transport.reads[0].push_back(encode_subscribe_message(
                1, kTestTrackNamespace, "events", 1, draft,
                0, 0, 100, 1, 0, 0, 0x04, 4));
        } else {
            transport.keep_open_streams.insert(1);
            transport.reads[3].push_back(encode_draft18_setup_response());
            transport.reads[0].push_back(
                encode_publish_namespace_ok_message(draft, 0));
            transport.reads[1].push_back(encode_subscribe_message(
                91, kTestTrackNamespace, "events", 1, draft,
                0, 0, 100, 1, 0, 0, 0x04, 4));
        }

        transport.on_try_write_object =
            [&](MockTransport& current,
                const MockTransport::ObjectWriteEvent&) {
                if (draft == DraftVersion::kDraft16) {
                    current.reads[0].push_back(
                        encode_request_update_message(
                            draft, 3, 7, 0x20, 1));
                } else {
                    current.reads[1].push_back(
                        encode_request_update_message(
                            draft, 93, 7, 0x20));
                }
                return ObjectWriteResult{
                    ObjectWriteDisposition::kAccepted, {}};
            };

        std::size_t object_index = 0;
        LiveObjectSource source{
            .tracks = {LiveTrack{.track_name = "events"}},
            .next_object = [&]() -> std::optional<LiveObject> {
                if (object_index++ != 0) {
                    return std::nullopt;
                }
                return LiveObject{
                    .track_name = "events",
                    .group_id = 3,
                    .subgroup_id = 0,
                    .object_id = 7,
                    .payload = {'X'},
                    .subgroup_contains_group_largest = true,
                    .final_in_subgroup = true,
                };
            },
        };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected live post-publication priority-update session connect to succeed");
        status = session.publish_live_objects(source, draft);
        const std::uint64_t update_request_id =
            draft == DraftVersion::kDraft16 ? 3 : 93;
        const auto expected_ok =
            openmoq::publisher::transport::encode_request_ok_message(
                draft, update_request_id, 3, 7);
        ok &= expect(status.ok &&
                         session.publish_stats().objects_published == 1 &&
                         std::count_if(
                             transport.writes.begin(),
                             transport.writes.end(),
                             [&](const MockTransport::WriteEvent& write) {
                                 return write.stream_id == response_stream_id &&
                                        write.bytes == expected_ok;
                             }) == 1,
                     "expected a post-publication priority REQUEST_OK to carry the current largest known object in the draft-specific shape");
    }

    for (const bool coalesced : {false, true}) {
        MockTransport transport;
        transport.reads[3].push_back(encode_draft18_setup_response());
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        std::vector<std::uint8_t> subscribe = encode_subscribe_message(
            91, kTestTrackNamespace, "events", 1, DraftVersion::kDraft18,
            0, 0, 100, 1);
        std::vector<std::uint8_t> update = encode_request_update_message(
            DraftVersion::kDraft18, 93, 7, 0x20);
        if (coalesced) {
            append_bytes(subscribe, std::move(update));
            transport.reads[1].push_back(std::move(subscribe));
        } else {
            transport.reads[1].push_back(std::move(subscribe));
            transport.reads[1].push_back(std::move(update));
        }
        LiveObjectSource source{
            .tracks = {LiveTrack{.track_name = "events"}},
            .next_object = []() -> std::optional<LiveObject> {
                return std::nullopt;
            },
        };

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            false,
                            false,
                            false,
                            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected EOF retained-update session connect to succeed");
        status = session.publish_live_objects(source, DraftVersion::kDraft18);
        const auto expected_ok =
            openmoq::publisher::transport::encode_request_ok_message(
                DraftVersion::kDraft18, 93);
        ok &= expect(status.ok && std::count_if(
                         transport.writes.begin(),
                         transport.writes.end(),
                         [&](const MockTransport::WriteEvent& write) {
                             return write.stream_id == 1 &&
                                    write.bytes == expected_ok;
                         }) == 1,
                     coalesced
                         ? "expected coalesced trailing update accepted at source EOF to be drained in the same pass"
                         : "expected complete update arriving with request-stream FIN to be processed before FIN");
    }

    {
        MockTransport draft18_forward_update_transport;
        draft18_forward_update_transport.reads[3].push_back(encode_draft18_setup_response());
        draft18_forward_update_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        draft18_forward_update_transport.reads[4].push_back(
            encode_publish_ok_message(DraftVersion::kDraft18, 2, 0));

        std::size_t publish_stream_read_count = 0;
        draft18_forward_update_transport.on_read = [&](MockTransport& transport, std::uint64_t stream_id) {
            if (stream_id != 4 || ++publish_stream_read_count != 2) {
                return;
            }
            transport.reads[4].push_back(
                encode_request_update_message(DraftVersion::kDraft18, 1, 1));
        };

        std::size_t object_index = 0;
        LiveObjectSource source{
            .tracks = {LiveTrack{.track_name = "events"}},
            .next_object = [&object_index]() -> std::optional<LiveObject> {
                if (object_index++ != 0) {
                    return std::nullopt;
                }
                return LiveObject{
                    .track_name = "events",
                    .group_id = 1,
                    .subgroup_id = 0,
                    .object_id = 0,
                    .payload = {'M'},
                };
            },
        };

        MoqtSession draft18_forward_update_session(
            draft18_forward_update_transport,
            std::string(kTestTrackNamespace),
            true,
            false,
            false,
            std::chrono::seconds(1));
        status = draft18_forward_update_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 forward-update session connect to succeed");
        status = draft18_forward_update_session.publish_live_objects(source, DraftVersion::kDraft18);
        ok &= expect(status.ok, "expected draft-18 Forward=1 REQUEST_UPDATE publish to succeed");
        ok &= expect(draft18_forward_update_session.publish_stats().objects_published == 1,
                     "expected Forward=1 REQUEST_UPDATE to release the queued object");

        const auto expected_update_ok =
            openmoq::publisher::transport::encode_request_ok_message(
                DraftVersion::kDraft18, 1);
        const bool sent_update_ok = std::count_if(
            draft18_forward_update_transport.writes.begin(),
            draft18_forward_update_transport.writes.end(),
            [&](const MockTransport::WriteEvent& write) {
                return write.stream_id == 4 && write.bytes == expected_update_ok;
            }) == 1;
        ok &= expect(sent_update_ok,
                     "expected one ID-less REQUEST_OK on the retained PUBLISH stream");
    }

    {
        MockTransport draft18_priority_update_transport;
        draft18_priority_update_transport.reads[3].push_back(encode_draft18_setup_response());
        draft18_priority_update_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        draft18_priority_update_transport.reads[4].push_back(
            encode_publish_ok_message(DraftVersion::kDraft18, 2, 1));

        std::size_t publish_stream_read_count = 0;
        draft18_priority_update_transport.on_read = [&](MockTransport& transport, std::uint64_t stream_id) {
            if (stream_id != 4 || ++publish_stream_read_count != 2) {
                return;
            }
            transport.reads[4].push_back(
                encode_request_update_message(DraftVersion::kDraft18, 1, 7, 0x20));
        };

        std::size_t object_index = 0;
        LiveObjectSource source{
            .tracks = {LiveTrack{.track_name = "events"}},
            .next_object = [&object_index]() -> std::optional<LiveObject> {
                if (object_index++ != 0) {
                    return std::nullopt;
                }
                return LiveObject{
                    .track_name = "events",
                    .group_id = 1,
                    .subgroup_id = 0,
                    .object_id = 0,
                    .payload = {'M'},
                };
            },
        };

        MoqtSession draft18_priority_update_session(
            draft18_priority_update_transport,
            std::string(kTestTrackNamespace),
            true,
            false,
            false,
            std::chrono::seconds(1));
        status = draft18_priority_update_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected draft-18 priority-update session connect to succeed");
        status = draft18_priority_update_session.publish_live_objects(source, DraftVersion::kDraft18);
        ok &= expect(status.ok, "expected draft-18 subscriber-priority update to succeed");
        ok &= expect(object_index == 2 &&
                         draft18_priority_update_session.publish_stats().objects_published == 1,
                     "expected the priority-updated PUBLISH subscription to send its object");
        ok &= expect(!draft18_priority_update_transport.object_write_attempts.empty() &&
                         draft18_priority_update_transport.object_write_attempts.front()
                                 .options.transport_priority ==
                             openmoq::publisher::transport::priority_scheduler_internal::
                                 object_transport_priority_for_testing(7, 128),
                     "expected PUBLISH REQUEST_UPDATE priority to reach object admission");
        ok &= expect(std::count_if(
                         draft18_priority_update_transport.writes.begin(),
                         draft18_priority_update_transport.writes.end(),
                         [](const MockTransport::WriteEvent& write) {
                             return write.stream_id == 4 && message_type(write.bytes) == 0x07;
                         }) == 1,
                     "expected successful PUBLISH update to receive exactly one REQUEST_OK");
    }

    {
        MockTransport draft18_invalid_scope_update_transport;
        draft18_invalid_scope_update_transport.reads[3].push_back(encode_draft18_setup_response());
        draft18_invalid_scope_update_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        draft18_invalid_scope_update_transport.reads[4].push_back(
            encode_publish_ok_message(DraftVersion::kDraft18, 2, 1));

        std::size_t publish_stream_read_count = 0;
        draft18_invalid_scope_update_transport.on_read = [&](MockTransport& transport, std::uint64_t stream_id) {
            if (stream_id != 4 || ++publish_stream_read_count != 2) {
                return;
            }
            transport.reads[4].push_back(
                encode_request_update_message(DraftVersion::kDraft18, 1, 1, 0x22));
        };
        LiveObjectSource source{
            .tracks = {LiveTrack{.track_name = "events"}},
            .next_object = []() -> std::optional<LiveObject> { return std::nullopt; },
        };

        MoqtSession draft18_invalid_scope_update_session(
            draft18_invalid_scope_update_transport, std::string(kTestTrackNamespace), true);
        status = draft18_invalid_scope_update_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected invalid-scope draft-18 update session connect to succeed");
        status = draft18_invalid_scope_update_session.publish_live_objects(source, DraftVersion::kDraft18);
        ok &= expect(!status.ok && status.message == "invalid REQUEST_UPDATE on PUBLISH stream",
                     "expected invalid-scope draft-18 update parameter to be a protocol violation");
        ok &= expect(draft18_invalid_scope_update_transport.last_close_code == 0x3,
                     "expected invalid-scope draft-18 update to close with PROTOCOL_VIOLATION");
    }

    {
        MockTransport transport;
        transport.reads[3].push_back(encode_draft18_setup_response());
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        transport.reads[4].push_back(
            encode_publish_ok_message(DraftVersion::kDraft18, 2, 1));
        std::size_t reads = 0;
        transport.on_read = [&](MockTransport& current, std::uint64_t stream_id) {
            if (stream_id == 4 && ++reads == 2) {
                current.reads[4].push_back(
                    encode_request_update_message(
                        DraftVersion::kDraft18, 1, 9, 0x32));
            }
        };
        LiveObjectSource source{
            .tracks = {LiveTrack{.track_name = "events"}},
            .next_object = []() -> std::optional<LiveObject> {
                return std::nullopt;
            },
        };
        MoqtSession session(
            transport, std::string(kTestTrackNamespace), true, false, false,
            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected unsuccessful-update session connect to succeed");
        status = session.publish_live_objects(source, DraftVersion::kDraft18);
        ok &= expect(!status.ok && transport.last_close_code == 0x03,
                     "expected unnegotiated PUBLISH NEW_GROUP to close the session with PROTOCOL_VIOLATION");
        ok &= expect(std::none_of(
                         transport.writes.begin(),
                         transport.writes.end(),
                         [](const MockTransport::WriteEvent& write) {
                             return write.stream_id == 4 &&
                                    (message_type(write.bytes) == 0x05 ||
                                     message_type(write.bytes) == 0x0b);
                         }),
                     "expected unnegotiated PUBLISH NEW_GROUP not to emit REQUEST_ERROR or PUBLISH_DONE");
    }

    {
        MockTransport transport;
        transport.reads[3].push_back(encode_draft18_setup_response());
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        transport.reads[4].push_back(
            encode_publish_ok_message(DraftVersion::kDraft18, 2, 1));
        std::size_t reads = 0;
        transport.on_read = [&](MockTransport& current, std::uint64_t stream_id) {
            if (stream_id == 4 && ++reads == 2) {
                current.reads[4].push_back(
                    encode_request_update_message(
                        DraftVersion::kDraft18, 2, 1));
            }
        };
        LiveObjectSource source{
            .tracks = {LiveTrack{.track_name = "events"}},
            .next_object = []() -> std::optional<LiveObject> {
                return std::nullopt;
            },
        };
        MoqtSession session(
            transport, std::string(kTestTrackNamespace), true, false, false,
            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected stale-update session connect to succeed");
        status = session.publish_live_objects(source, DraftVersion::kDraft18);
        ok &= expect(!status.ok &&
                         status.message ==
                             "peer request id has invalid parity",
                     "expected REQUEST_UPDATE to use the peer request-id space");
        ok &= expect(transport.last_close_code == 0x4,
                     "expected wrong-parity REQUEST_UPDATE id to close with INVALID_REQUEST_ID");
    }

    {
        // Phase 5 regression, DASH live path: a LiveObjectSource's next_object()
        // can throw the same CMSF 4.1.2 refusal (a CENC-protected track with no
        // pssh anywhere in the init segment) that build_live_catalog() throws
        // on the stdin path -- e.g. a DASH ingest session building its catalog
        // lazily inside next_object(). publish_live_objects() must catch that
        // std::runtime_error and return a failure status rather than letting it
        // unwind past this function, for the same teardown reason as the
        // stdin-path guard above: the caller's normal teardown only runs when
        // this returns rather than throws. Asserts the same three things: a
        // failure status (not a process abort on an uncaught exception), a
        // message naming the missing pssh, and that the caller's teardown
        // (close(0)) still runs cleanly and is observable on the transport
        // afterward.
        MockTransport cenc_no_pssh_objects_transport;
        cenc_no_pssh_objects_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft14,
            .max_request_id = 8,
        }));
        cenc_no_pssh_objects_transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft14, 0));
        cenc_no_pssh_objects_transport.reads[0].push_back(
            encode_subscribe_message(1, kTestTrackNamespace, "events", 0));
        cenc_no_pssh_objects_transport.reads[0].push_back({});

        LiveObjectSource cenc_no_pssh_source{
            .tracks = {LiveTrack{.track_name = "events"}},
            .next_object = []() -> std::optional<LiveObject> {
                throw std::runtime_error(
                    "track 'events' is protected (CENC) but no pssh system was found in the "
                    "initialization segment; refusing to publish a catalog with no "
                    "contentProtections entry for encrypted content");
            },
        };

        MoqtSession cenc_no_pssh_objects_session(cenc_no_pssh_objects_transport,
                                                 std::string(kTestTrackNamespace),
                                                 false,
                                                 false,
                                                 false,
                                                 std::chrono::seconds(1));
        status = cenc_no_pssh_objects_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected CENC-no-pssh DASH live-object session connect to succeed");

        status = cenc_no_pssh_objects_session.publish_live_objects(cenc_no_pssh_source, DraftVersion::kDraft14);
        ok &= expect(!status.ok,
                     "expected a next_object() CMSF 4.1.2 refusal to fail the DASH live-object "
                     "publish rather than letting the throw escape publish_live_objects");
        ok &= expect(status.message.find("pssh") != std::string::npos,
                     "expected the failure status to name the missing pssh");

        const TransportStatus cenc_no_pssh_objects_close_status = cenc_no_pssh_objects_session.close(0);
        ok &= expect(cenc_no_pssh_objects_close_status.ok,
                     "expected the caller's normal teardown (close(0)) to still succeed after "
                     "the guard returns a failure status");
        ok &= expect(cenc_no_pssh_objects_transport.state() == ConnectionState::kClosed &&
                         cenc_no_pssh_objects_transport.last_close_code == 0,
                     "expected teardown to actually reach the transport -- proving the session "
                     "was left in a closeable state rather than mid-unwind");
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
                1, kTestTrackNamespace, "video", 1, DraftVersion::kDraft16));
        media_first_transport.reads[0].push_back({});
        media_first_transport.reads[0].push_back(
            encode_subscribe_message(
                3, kTestTrackNamespace, "catalog", 1, DraftVersion::kDraft16));
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
            encode_subscribe_message(1, kTestTrackNamespace, "video", 1, DraftVersion::kDraft16));
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
                1, kTestTrackNamespace, "catalog", 1, DraftVersion::kDraft16));
        dash_live_transport.reads[0].push_back({});
        dash_live_transport.reads[0].push_back(
            encode_subscribe_message(
                3, kTestTrackNamespace, "video0_vide_1", 1, DraftVersion::kDraft16));
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
        // Preannounce is opt-in; this case asserts the PUBLISH count it produces.
        dash_live_session.set_preannounce_tracks(true);
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
        // Forward mode (--forward 1) on a control-stream draft: draft-16 section
        // 9.13 makes "objects immediately, possibly before PUBLISH_OK" the
        // FORWARD=1 semantics of a PUBLISH, so the media track must be
        // PUBLISHed before its first object is pushed -- otherwise the relay
        // receives data for an alias nothing has established.
        MockTransport forward_transport;
        forward_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft16,
            .max_request_id = 8,
        }));
        forward_transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft16, 0));

        MoqtSession forward_session(
            forward_transport, std::string(kTestTrackNamespace), true, false, false, std::chrono::seconds(1));
        status = forward_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected forward-mode draft-16 session connect to succeed");

        const auto forward_bytes = concat({make_live_init_mp4(), make_live_media_fragment(0, 0xAB)});
        std::string forward_input_bytes(forward_bytes.begin(), forward_bytes.end());
        std::istringstream forward_input(forward_input_bytes);
        status = forward_session.publish_live(forward_input, DraftVersion::kDraft16, true);
        ok &= expect(status.ok, "expected forward-mode draft-16 live publish to succeed");
        ok &= expect(control_message_count(forward_transport, 0x1d) == 1,
                     "expected forward mode to PUBLISH the media track on the control stream");
        ok &= expect(forward_session.publish_stats().objects_published >= 1,
                     "expected forward mode to push the media fragment");

        std::optional<std::size_t> first_publish;
        std::optional<std::size_t> first_data;
        for (std::size_t i = 0; i < forward_transport.writes.size(); ++i) {
            const auto& write = forward_transport.writes[i];
            if (!first_publish && write.stream_id == 0 && message_type(write.bytes) == 0x1d) {
                first_publish = i;
            }
            if (!first_data && is_data_stream_write(write)) {
                first_data = i;
            }
        }
        ok &= expect(first_publish && first_data && *first_publish < *first_data,
                     "expected forward-mode PUBLISH to be written before the first data stream");
    }

    {
        // Forward mode on a request-stream draft: each media track is PUBLISHed
        // on its own request stream before any object is pushed. The catalog is
        // not preannounced -- it stays SUBSCRIBE-driven, as on draft-14/16.
        MockTransport forward_draft18_transport;
        forward_draft18_transport.reads[3].push_back(encode_draft18_setup_response());
        forward_draft18_transport.reads[0].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        forward_draft18_transport.reads[4].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft18, 2));

        MoqtSession forward_draft18_session(
            forward_draft18_transport, std::string(kTestTrackNamespace), true, false, false, std::chrono::seconds(1));
        status = forward_draft18_session.connect(endpoint, tls);
        ok &= expect(status.ok, "expected forward-mode draft-18 session connect to succeed");

        const auto forward_bytes = concat({make_live_init_mp4(), make_live_media_fragment(0, 0xCD)});
        std::string forward_input_bytes(forward_bytes.begin(), forward_bytes.end());
        std::istringstream forward_input(forward_input_bytes);
        status = forward_draft18_session.publish_live(forward_input, DraftVersion::kDraft18, true);
        ok &= expect(status.ok, "expected forward-mode draft-18 live publish to succeed");
        ok &= expect(control_message_count(forward_draft18_transport, 0x1d) == 0,
                     "expected forward-mode draft-18 to keep PUBLISH off the control stream");

        std::optional<std::size_t> media_publish;
        std::optional<std::size_t> first_data;
        std::size_t request_stream_publishes = 0;
        for (std::size_t i = 0; i < forward_draft18_transport.writes.size(); ++i) {
            const auto& write = forward_draft18_transport.writes[i];
            if (write.stream_id != 0 && (write.stream_id & 0x3ULL) == 0 && message_type(write.bytes) == 0x1d) {
                ++request_stream_publishes;
                if (!media_publish && write.stream_id == 4) {
                    media_publish = i;
                }
            }
            if (!first_data && is_data_stream_write(write)) {
                first_data = i;
            }
        }
        ok &= expect(request_stream_publishes == 1 && media_publish,
                     "expected forward-mode draft-18 to PUBLISH only the media track, on the first request stream");
        ok &= expect(media_publish && first_data && *media_publish < *first_data,
                     "expected forward-mode draft-18 PUBLISH before the first data stream");
        ok &= expect(forward_draft18_session.publish_stats().objects_published >= 1,
                     "expected forward-mode draft-18 to push the media fragment");
    }

    {
        MockTransport transport;
        transport.reads[3].push_back(encode_draft18_setup_response());
        transport.reads[0].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        std::vector<std::uint8_t> publish_response =
            encode_publish_ok_message(DraftVersion::kDraft18, 2, 1);
        append_bytes(publish_response, encode_request_update_message(
            DraftVersion::kDraft18, 1, 7, 0x20));
        transport.reads[4].push_back(std::move(publish_response));

        MoqtSession session(transport,
                            std::string(kTestTrackNamespace),
                            true,
                            false,
                            false,
                            std::chrono::seconds(1));
        ok &= expect(session.connect(endpoint, tls).ok,
                     "expected stdin retained-PUBLISH-update session connect to succeed");
        const auto live_bytes = concat(
            {make_live_init_mp4(), make_live_media_fragment(0, 0xCE)});
        std::string input_bytes(live_bytes.begin(), live_bytes.end());
        std::istringstream input(input_bytes);
        status = session.publish_live(
            input, DraftVersion::kDraft18, true);
        const auto media_attempt = std::find_if(
            transport.object_write_attempts.begin(),
            transport.object_write_attempts.end(),
            [](const MockTransport::ObjectWriteEvent& attempt) {
                return !attempt.bytes.empty() && attempt.bytes.back() == 0xCE;
            });
        const auto expected_ok =
            openmoq::publisher::transport::encode_request_ok_message(
                DraftVersion::kDraft18, 1);
        ok &= expect(status.ok &&
                         media_attempt != transport.object_write_attempts.end() &&
                         media_attempt->options.transport_priority ==
                             openmoq::publisher::transport::
                                 priority_scheduler_internal::
                                     object_transport_priority_for_testing(
                                         7, 128) &&
                         std::count_if(
                             transport.writes.begin(),
                             transport.writes.end(),
                             [&](const MockTransport::WriteEvent& write) {
                                 return write.stream_id == 4 &&
                                        write.bytes == expected_ok;
                             }) == 1,
                     "expected stdin draft-18 to retain, apply, and acknowledge REQUEST_UPDATE bytes trailing PUBLISH_OK");
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
