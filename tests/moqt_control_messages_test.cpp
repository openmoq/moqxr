#include "openmoq/publisher/transport/moqt_control_messages.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openmoq::publisher::DraftVersion;
using openmoq::publisher::transport::MaxRequestIdMessage;
using openmoq::publisher::transport::NamespaceMessage;
using openmoq::publisher::transport::PublishError;
using openmoq::publisher::transport::PublishNamespaceOk;
using openmoq::publisher::transport::PublishOk;
using openmoq::publisher::transport::RequestError;
using openmoq::publisher::transport::ServerSetupMessage;
using openmoq::publisher::transport::SetupMessage;
using openmoq::publisher::transport::SubscribeMessage;
using openmoq::publisher::transport::SubscribeNamespaceMessage;
using openmoq::publisher::transport::SubscribeTracksMessage;
using openmoq::publisher::transport::SubscribeUpdateMessage;
using openmoq::publisher::transport::TrackMessage;
using openmoq::publisher::transport::TransportKind;
using openmoq::publisher::transport::decode_max_request_id_message;
using openmoq::publisher::transport::decode_publish_error;
using openmoq::publisher::transport::decode_publish_ok;
using openmoq::publisher::transport::decode_request_error;
using openmoq::publisher::transport::decode_request_ok;
using openmoq::publisher::transport::decode_server_setup_message;
using openmoq::publisher::transport::decode_setup_response_message;
using openmoq::publisher::transport::decode_subscribe_message;
using openmoq::publisher::transport::decode_subscribe_namespace_message;
using openmoq::publisher::transport::decode_subscribe_tracks_message;
using openmoq::publisher::transport::decode_subscribe_update_message;
using openmoq::publisher::transport::decode_varint;
using openmoq::publisher::transport::encode_namespace_message;
using openmoq::publisher::transport::encode_publish_done_message;
using openmoq::publisher::transport::encode_publish_namespace_done_message;
using openmoq::publisher::transport::encode_request_error_message;
using openmoq::publisher::transport::encode_request_ok_message;
using openmoq::publisher::transport::encode_server_setup_message;
using openmoq::publisher::transport::encode_setup_message;
using openmoq::publisher::transport::encode_subgroup_header;
using openmoq::publisher::transport::encode_subgroup_object;
using openmoq::publisher::transport::encode_subscribe_error_message;
using openmoq::publisher::transport::encode_subscribe_namespace_ok_message;
using openmoq::publisher::transport::encode_subscribe_ok_message;
using openmoq::publisher::transport::encode_track_message;
using openmoq::publisher::transport::next_control_message;

constexpr std::uint64_t kDraft14Version = 0xff00000eULL;
constexpr std::uint64_t kDraft16Version = 0xff000010ULL;
constexpr std::uint64_t kDraft17Version = 0xff000011ULL;
constexpr std::uint64_t kDraft18Version = 0xff000012ULL;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

void append_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    if (value <= 63) {
        out.push_back(static_cast<std::uint8_t>(value));
    } else if (value <= 16383) {
        out.push_back(static_cast<std::uint8_t>(0x40 | ((value >> 8) & 0x3f)));
        out.push_back(static_cast<std::uint8_t>(value & 0xff));
    } else if (value <= 1073741823ULL) {
        out.push_back(static_cast<std::uint8_t>(0x80 | ((value >> 24) & 0x3f)));
        out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(value & 0xff));
    } else {
        out.push_back(static_cast<std::uint8_t>(0xc0 | ((value >> 56) & 0x3f)));
        out.push_back(static_cast<std::uint8_t>((value >> 48) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 40) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 32) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(value & 0xff));
    }
}

void append_vi64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    if (value <= 127) {
        out.push_back(static_cast<std::uint8_t>(value));
    } else if (value <= 16383) {
        out.push_back(static_cast<std::uint8_t>(0x80 | ((value >> 8) & 0x3f)));
        out.push_back(static_cast<std::uint8_t>(value & 0xff));
    } else if (value <= 2097151ULL) {
        out.push_back(static_cast<std::uint8_t>(0xc0 | ((value >> 16) & 0x1f)));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(value & 0xff));
    } else {
        out.push_back(static_cast<std::uint8_t>(0xe0 | ((value >> 24) & 0x0f)));
        out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(value & 0xff));
    }
}

bool uses_vi64(DraftVersion draft) {
    return draft == DraftVersion::kDraft17 || draft == DraftVersion::kDraft18;
}

void append_moqint(std::vector<std::uint8_t>& out, DraftVersion draft, std::uint64_t value) {
    uses_vi64(draft) ? append_vi64(out, value) : append_varint(out, value);
}

void append_string(std::vector<std::uint8_t>& out, DraftVersion draft, std::string_view value) {
    append_moqint(out, draft, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void append_track_namespace(std::vector<std::uint8_t>& out, DraftVersion draft, const std::vector<std::string_view>& tuple) {
    append_moqint(out, draft, tuple.size());
    for (std::string_view part : tuple) {
        append_string(out, draft, part);
    }
}

void append_uint16_length_message(std::vector<std::uint8_t>& out,
                                  std::uint64_t type,
                                  const std::vector<std::uint8_t>& payload) {
    append_varint(out, type);
    out.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(payload.size() & 0xff));
    out.insert(out.end(), payload.begin(), payload.end());
}

void append_varint_length_message(std::vector<std::uint8_t>& out,
                                  std::uint64_t type,
                                  const std::vector<std::uint8_t>& payload) {
    append_varint(out, type);
    append_varint(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

bool read_varint(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint64_t& value) {
    return decode_varint(bytes, offset, value);
}

bool read_vi64(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint64_t& value) {
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
    } else if ((first & 0xf8) == 0xf0) {
        length = 5;
        prefix_mask = 0x07;
    } else if ((first & 0xfc) == 0xf8) {
        length = 6;
        prefix_mask = 0x03;
    } else if ((first & 0xfe) == 0xfc) {
        length = 7;
        prefix_mask = 0x01;
    } else if (first == 0xfe) {
        length = 8;
        prefix_mask = 0;
    } else {
        length = 9;
        prefix_mask = 0;
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

bool read_moqint(std::span<const std::uint8_t> bytes,
                 std::size_t& offset,
                 DraftVersion draft,
                 std::uint64_t& value) {
    return uses_vi64(draft) ? read_vi64(bytes, offset, value) : read_varint(bytes, offset, value);
}

bool read_string(std::span<const std::uint8_t> bytes,
                 std::size_t& offset,
                 std::size_t end,
                 DraftVersion draft,
                 std::string& value) {
    std::uint64_t length = 0;
    if (!read_moqint(bytes, offset, draft, length) || offset + length > end) {
        return false;
    }
    value.assign(reinterpret_cast<const char*>(bytes.data() + offset), static_cast<std::size_t>(length));
    offset += static_cast<std::size_t>(length);
    return true;
}

bool read_track_namespace(std::span<const std::uint8_t> bytes,
                          std::size_t& offset,
                          std::size_t end,
                          DraftVersion draft,
                          std::vector<std::string>& tuple) {
    std::uint64_t count = 0;
    if (!read_moqint(bytes, offset, draft, count)) {
        return false;
    }
    tuple.clear();
    for (std::uint64_t i = 0; i < count; ++i) {
        std::string part;
        if (!read_string(bytes, offset, end, draft, part)) {
            return false;
        }
        tuple.push_back(part);
    }
    return true;
}

struct Uint16Frame {
    std::uint64_t type = 0;
    std::size_t payload_offset = 0;
    std::size_t payload_end = 0;
};

bool read_uint16_frame(std::span<const std::uint8_t> bytes, Uint16Frame& frame) {
    auto read_with = [&](auto reader) {
        std::size_t offset = 0;
        if (!reader(bytes, offset, frame.type) || offset + 2 > bytes.size()) {
            return false;
        }
        const std::size_t payload_length =
            (static_cast<std::size_t>(bytes[offset]) << 8) | static_cast<std::size_t>(bytes[offset + 1]);
        offset += 2;
        if (offset + payload_length != bytes.size()) {
            return false;
        }
        frame.payload_offset = offset;
        frame.payload_end = offset + payload_length;
        return true;
    };
    return read_with(read_varint) || read_with(read_vi64);
}

bool expect_uint16_frame(std::span<const std::uint8_t> bytes,
                         std::uint64_t expected_type,
                         Uint16Frame& frame,
                         const std::string& label) {
    return expect(read_uint16_frame(bytes, frame), label + " frame parses") &&
           expect(frame.type == expected_type, label + " type");
}

bool expect_varint_frame(std::span<const std::uint8_t> bytes,
                         std::uint64_t expected_type,
                         Uint16Frame& frame,
                         const std::string& label) {
    std::size_t offset = 0;
    std::uint64_t payload_length = 0;
    return expect(read_varint(bytes, offset, frame.type), label + " type parses") &&
           expect(frame.type == expected_type, label + " type") &&
           expect(read_varint(bytes, offset, payload_length), label + " length parses") &&
           expect(offset + payload_length == bytes.size(), label + " length fits") &&
           [&]() {
               frame.payload_offset = offset;
               frame.payload_end = offset + static_cast<std::size_t>(payload_length);
               return true;
           }();
}

std::uint64_t draft_version_number(DraftVersion draft) {
    switch (draft) {
        case DraftVersion::kDraft14:
            return kDraft14Version;
        case DraftVersion::kDraft16:
            return kDraft16Version;
        case DraftVersion::kDraft17:
            return kDraft17Version;
        case DraftVersion::kDraft18:
            return kDraft18Version;
    }
    return 0;
}

std::string draft_label(DraftVersion draft) {
    switch (draft) {
        case DraftVersion::kDraft14:
            return "draft-14";
        case DraftVersion::kDraft16:
            return "draft-16";
        case DraftVersion::kDraft17:
            return "draft-17";
        case DraftVersion::kDraft18:
            return "draft-18";
    }
    return "unknown";
}

std::vector<std::uint8_t> build_subscribe_message(DraftVersion draft) {
    std::vector<std::uint8_t> payload;
    append_moqint(payload, draft, 77);
    append_track_namespace(payload, draft, {"live", "alpha"});
    append_string(payload, draft, "catalog");

    if (draft == DraftVersion::kDraft14) {
        payload.push_back(128);  // subscriber priority
        payload.push_back(1);    // ascending group order
        payload.push_back(1);    // forward
        append_varint(payload, 3);
        append_varint(payload, 9);
        append_varint(payload, 4);
        append_varint(payload, 0);
    } else {
        append_moqint(payload, draft, 4);   // parameter count
        append_moqint(payload, draft, 16);  // FORWARD, delta from 0x00
        append_moqint(payload, draft, 1);
        append_moqint(payload, draft, 16);  // SUBSCRIBER_PRIORITY, delta from 0x10 to 0x20
        append_moqint(payload, draft, 127);
        append_moqint(payload, draft, 1);   // SUBSCRIPTION_FILTER, delta from 0x20 to 0x21
        std::vector<std::uint8_t> filter;
        append_moqint(filter, draft, 3);
        append_moqint(filter, draft, 9);
        append_moqint(filter, draft, 4);
        append_moqint(payload, draft, filter.size());
        payload.insert(payload.end(), filter.begin(), filter.end());
        append_moqint(payload, draft, 1);   // GROUP_ORDER, delta from 0x21 to 0x22
        append_moqint(payload, draft, 1);
    }

    std::vector<std::uint8_t> bytes;
    append_uint16_length_message(bytes, 0x03, payload);
    return bytes;
}

std::vector<std::uint8_t> build_subscribe_namespace_message(DraftVersion draft) {
    std::vector<std::uint8_t> payload;
    append_moqint(payload, draft, 91);
    append_track_namespace(payload, draft, {"live", "alpha"});
    if (draft == DraftVersion::kDraft16) {
        append_moqint(payload, draft, 1);  // subscribe options
    }
    append_moqint(payload, draft, 0);  // parameters

    std::vector<std::uint8_t> bytes;
    append_moqint(bytes, draft, draft == DraftVersion::kDraft18 ? 0x50 : 0x11);
    if (draft == DraftVersion::kDraft16 || draft == DraftVersion::kDraft18) {
        bytes.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>(payload.size() & 0xff));
    } else {
        append_varint(bytes, payload.size());
    }
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::vector<std::uint8_t> build_subscribe_tracks_message() {
    constexpr DraftVersion draft = DraftVersion::kDraft18;
    std::vector<std::uint8_t> payload;
    append_moqint(payload, draft, 93);
    append_track_namespace(payload, draft, {"live", "alpha"});
    append_moqint(payload, draft, 1);     // one parameter
    append_moqint(payload, draft, 0x10);  // FORWARD
    append_moqint(payload, draft, 0);

    std::vector<std::uint8_t> bytes;
    append_moqint(bytes, draft, 0x51);
    bytes.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>(payload.size() & 0xff));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::vector<std::uint8_t> build_subscribe_update_message() {
    std::vector<std::uint8_t> payload;
    append_varint(payload, 101);
    append_varint(payload, 77);
    append_varint(payload, 12);
    append_varint(payload, 5);
    append_varint(payload, 20);
    payload.push_back(200);
    payload.push_back(1);
    append_varint(payload, 0);

    std::vector<std::uint8_t> bytes;
    append_uint16_length_message(bytes, 0x02, payload);
    return bytes;
}

std::vector<std::uint8_t> build_publish_ok_message(DraftVersion draft) {
    std::vector<std::uint8_t> payload;
    if (draft != DraftVersion::kDraft18) {
        append_moqint(payload, draft, 55);
    }
    if (draft == DraftVersion::kDraft14) {
        payload.push_back(1);    // forward
        payload.push_back(128);  // subscriber priority
        payload.push_back(1);    // group order
        append_varint(payload, 3);
        append_varint(payload, 12);
        append_varint(payload, 5);
        append_varint(payload, 0);
    } else {
        append_moqint(payload, draft, 4);   // parameter count
        append_moqint(payload, draft, 16);  // FORWARD
        append_moqint(payload, draft, 1);
        append_moqint(payload, draft, 16);  // SUBSCRIBER_PRIORITY
        append_moqint(payload, draft, 127);
        append_moqint(payload, draft, 1);   // SUBSCRIPTION_FILTER
        std::vector<std::uint8_t> filter;
        append_moqint(filter, draft, 3);
        append_moqint(filter, draft, 12);
        append_moqint(filter, draft, 5);
        append_moqint(payload, draft, filter.size());
        payload.insert(payload.end(), filter.begin(), filter.end());
        append_moqint(payload, draft, 1);   // GROUP_ORDER
        append_moqint(payload, draft, 1);
    }

    std::vector<std::uint8_t> bytes;
    if (draft == DraftVersion::kDraft14) {
        append_varint_length_message(bytes, 0x1e, payload);
    } else {
        append_moqint(bytes, draft, draft == DraftVersion::kDraft18 ? 0x07 : 0x1e);
        bytes.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>(payload.size() & 0xff));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }
    return bytes;
}

std::vector<std::uint8_t> build_publish_error_message(DraftVersion draft) {
    std::vector<std::uint8_t> payload;
    append_varint(payload, 55);
    append_varint(payload, 2);
    append_string(payload, draft, "nope");

    std::vector<std::uint8_t> bytes;
    if (draft == DraftVersion::kDraft14) {
        append_varint_length_message(bytes, 0x1f, payload);
    } else {
        append_uint16_length_message(bytes, 0x1f, payload);
    }
    return bytes;
}

bool test_setup_serdes_for_all_drafts() {
    bool ok = true;
    for (DraftVersion draft : {DraftVersion::kDraft14, DraftVersion::kDraft16, DraftVersion::kDraft17, DraftVersion::kDraft18}) {
        const std::string label = draft_label(draft) + " setup";
        const SetupMessage setup{
            .draft = draft,
            .transport = TransportKind::kRawQuic,
            .authority = "relay.example.com:4433",
            .path = "/moq/live",
            .max_request_id = 32,
        };
        const std::vector<std::uint8_t> bytes = encode_setup_message(setup);
        Uint16Frame frame;
        ok &= expect_uint16_frame(bytes, uses_vi64(draft) ? 0x2f00 : 0x20, frame, label);

        std::size_t offset = frame.payload_offset;
        if (uses_vi64(draft)) {
            bool saw_path = false;
            bool saw_authority = false;
            std::uint64_t previous_option_type = 0;
            std::uint64_t option_index = 0;
            while (offset < frame.payload_end) {
                std::uint64_t option_delta = 0;
                std::uint64_t option_length = 0;
                ok &= expect(read_moqint(bytes, offset, draft, option_delta), label + " option delta type");
                if (option_index == 0) {
                    ok &= expect(option_delta == 0x01, label + " first setup option is PATH delta");
                } else if (option_index == 1) {
                    ok &= expect(option_delta == 0x04, label + " second setup option is AUTHORITY delta");
                }
                const std::uint64_t option_type = previous_option_type + option_delta;
                ok &= expect(read_moqint(bytes, offset, draft, option_length), label + " option length");
                ok &= expect(offset + option_length <= frame.payload_end, label + " option length fits");
                saw_path = saw_path || option_type == 0x01;
                saw_authority = saw_authority || option_type == 0x05;
                offset += static_cast<std::size_t>(option_length);
                previous_option_type = option_type;
                ++option_index;
            }
            ok &= expect(saw_path, label + " path option placement");
            ok &= expect(saw_authority, label + " authority option placement");
        } else {
            if (draft == DraftVersion::kDraft14) {
                std::uint64_t version_count = 0;
                std::uint64_t version = 0;
                ok &= expect(read_varint(bytes, offset, version_count) && version_count == 1, label + " version count");
                ok &= expect(read_varint(bytes, offset, version) && version == draft_version_number(draft), label + " version");
            }
            std::uint64_t parameter_count = 0;
            ok &= expect(read_varint(bytes, offset, parameter_count) && parameter_count == 3, label + " parameter count");
            ok &= expect(offset == frame.payload_end || parameter_count == 3, label + " payload remains valid");
        }

        const std::vector<std::uint8_t> server_bytes =
            uses_vi64(draft)
                ? encode_setup_message({.draft = draft, .transport = TransportKind::kWebTransport})
                : encode_server_setup_message({.draft = draft, .max_request_id = 32});
        ServerSetupMessage server;
        ok &= expect(decode_setup_response_message(server_bytes, draft, server), label + " setup response decode");
        ok &= expect(server.draft == draft, label + " server setup draft");
        if (!uses_vi64(draft)) {
            ok &= expect(server.max_request_id == 32, label + " server setup max request id");
        }
    }
    ServerSetupMessage legacy_server_setup;
    ok &= expect(!decode_setup_response_message(encode_server_setup_message({.draft = DraftVersion::kDraft16}),
                                                DraftVersion::kDraft18,
                                                legacy_server_setup),
                 "draft-18 setup response rejects legacy SERVER_SETUP");

    const std::vector<std::uint8_t> relay_setup_with_unknown_even_option{
        0xaf, 0x00, 0x00, 0x02, 0x02, 0x64,
    };
    ServerSetupMessage relay_server_setup;
    ok &= expect(decode_setup_response_message(relay_setup_with_unknown_even_option,
                                               DraftVersion::kDraft18,
                                               relay_server_setup),
                 "draft-18 setup response ignores unknown even option");
    return ok;
}

bool test_publisher_control_message_encoders_for_all_drafts() {
    bool ok = true;
    for (DraftVersion draft : {DraftVersion::kDraft14, DraftVersion::kDraft16, DraftVersion::kDraft17, DraftVersion::kDraft18}) {
        const std::string label = draft_label(draft);

        const NamespaceMessage namespace_message{.draft = draft, .track_namespace = "live/alpha", .request_id = 42};
        Uint16Frame frame;
        std::size_t offset = 0;
        std::vector<std::string> tuple;
        ok &= expect_uint16_frame(encode_namespace_message(namespace_message), 0x06, frame, label + " publish namespace");
        offset = frame.payload_offset;
        std::uint64_t request_id = 0;
        std::uint64_t parameter_count = 99;
        ok &= expect(read_moqint(encode_namespace_message(namespace_message), offset, draft, request_id) &&
                         request_id == 42,
                     label + " publish namespace request id");
        const auto namespace_bytes = encode_namespace_message(namespace_message);
        offset = frame.payload_offset;
        ok &= expect(read_moqint(namespace_bytes, offset, draft, request_id) && request_id == 42,
                     label + " publish namespace request id stable");
        if (draft == DraftVersion::kDraft17) {
            std::uint64_t required_request_id_delta = 99;
            ok &= expect(read_moqint(namespace_bytes, offset, draft, required_request_id_delta) &&
                             required_request_id_delta == 0,
                         label + " publish namespace required request id delta");
        }
        ok &= expect(read_track_namespace(namespace_bytes, offset, frame.payload_end, draft, tuple) &&
                         tuple == std::vector<std::string>({"live", "alpha"}),
                     label + " publish namespace tuple placement");
        ok &= expect(read_moqint(namespace_bytes, offset, draft, parameter_count) && parameter_count == 0 &&
                         offset == frame.payload_end,
                     label + " publish namespace params");

        const TrackMessage track{
            .draft = draft,
            .track_name = "catalog",
            .track_namespace = "live/alpha",
            .request_id = 44,
            .track_alias = 7,
            .largest_group_id = 3,
            .largest_object_id = 5,
            .content_exists = true,
        };
        const auto track_bytes = encode_track_message(track);
        if (draft == DraftVersion::kDraft14) {
            ok &= expect_varint_frame(track_bytes, 0x1d, frame, label + " publish track");
        } else {
            ok &= expect_uint16_frame(track_bytes, 0x1d, frame, label + " publish track");
        }
        offset = frame.payload_offset;
        std::string track_name;
        std::uint64_t alias = 0;
        ok &= expect(read_moqint(track_bytes, offset, draft, request_id) && request_id == 44, label + " track request id");
        if (draft == DraftVersion::kDraft17) {
            std::uint64_t required_request_id_delta = 99;
            ok &= expect(read_moqint(track_bytes, offset, draft, required_request_id_delta) &&
                             required_request_id_delta == 0,
                         label + " track required request id delta");
        }
        ok &= expect(read_track_namespace(track_bytes, offset, frame.payload_end, draft, tuple) &&
                         tuple == std::vector<std::string>({"live", "alpha"}),
                     label + " track namespace tuple placement");
        ok &= expect(read_string(track_bytes, offset, frame.payload_end, draft, track_name) && track_name == "catalog",
                     label + " track name placement");
        ok &= expect(read_moqint(track_bytes, offset, draft, alias) && alias == 7, label + " track alias placement");
        if (draft == DraftVersion::kDraft14) {
            ok &= expect(offset + 3 <= frame.payload_end, label + " draft-14 track fixed fields");
            ok &= expect(track_bytes[offset++] == 1, label + " draft-14 group order");
            ok &= expect(track_bytes[offset++] == 1, label + " draft-14 content exists");
            std::uint64_t largest_group = 0;
            std::uint64_t largest_object = 0;
            ok &= expect(read_varint(track_bytes, offset, largest_group) && largest_group == 3,
                         label + " draft-14 largest group");
            ok &= expect(read_varint(track_bytes, offset, largest_object) && largest_object == 5,
                         label + " draft-14 largest object");
            ok &= expect(track_bytes[offset++] == 1, label + " draft-14 forward preference");
        }
        ok &= expect(read_moqint(track_bytes, offset, draft, parameter_count) && parameter_count == 0 &&
                         offset == frame.payload_end,
                     label + " track params at end");
        PublishNamespaceOk ok_message;
        if (draft == DraftVersion::kDraft14) {
            std::vector<std::uint8_t> payload;
            append_varint(payload, 44);
            std::vector<std::uint8_t> namespace_ok_bytes;
            append_uint16_length_message(namespace_ok_bytes, 0x07, payload);
            ok &= expect(decode_request_ok(namespace_ok_bytes, draft, ok_message), label + " publish namespace ok decode");
            ok &= expect(ok_message.request_id == 44, label + " publish namespace ok request id");
        } else {
            ok &= expect(decode_request_ok(encode_request_ok_message(draft, 44), draft, ok_message), label + " request ok decode");
            ok &= expect(ok_message.request_id == (uses_vi64(draft) ? 0 : 44), label + " request ok request id");
        }
        RequestError request_error;
        if (draft == DraftVersion::kDraft14) {
            std::vector<std::uint8_t> payload;
            append_varint(payload, 44);
            append_varint(payload, 9);
            append_string(payload, draft, "denied");
            std::vector<std::uint8_t> namespace_error_bytes;
            append_uint16_length_message(namespace_error_bytes, 0x08, payload);
            ok &= expect(decode_request_error(namespace_error_bytes, draft, request_error), label + " publish namespace error decode");
        } else {
            ok &= expect(decode_request_error(encode_request_error_message(draft, 44, 9, 1, "denied"), draft, request_error),
                         label + " request error decode");
        }
        ok &= expect(request_error.request_id == (uses_vi64(draft) ? 0 : 44), label + " request error request id");
        ok &= expect(request_error.error_code == 9 && request_error.reason == "denied", label + " request error fields");

        ok &= expect_uint16_frame(encode_publish_done_message(draft, 44, 2), 0x0b, frame, label + " publish done");
        if (uses_vi64(draft)) {
            const auto namespace_done = encode_publish_namespace_done_message(namespace_message);
            ok &= expect_uint16_frame(namespace_done, 0x0e, frame, label + " namespace done");
            offset = frame.payload_offset;
            ok &= expect(read_track_namespace(namespace_done, offset, frame.payload_end, draft, tuple) &&
                             tuple == std::vector<std::string>({"live", "alpha"}),
                         label + " namespace done suffix placement");
            ok &= expect(offset == frame.payload_end, label + " namespace done boundary");
        } else {
            ok &= expect_uint16_frame(encode_publish_namespace_done_message(namespace_message), 0x09, frame,
                                      label + " publish namespace done");
        }
        const auto subscribe_ok = encode_subscribe_ok_message(draft, 44, 7, 3, 5, true);
        ok &= expect_uint16_frame(subscribe_ok, 0x04, frame, label + " subscribe ok");
        if (draft != DraftVersion::kDraft14) {
            offset = frame.payload_offset;
            if (!uses_vi64(draft)) {
                ok &= expect(read_moqint(subscribe_ok, offset, draft, request_id) && request_id == 44,
                             label + " subscribe ok request id");
            }
            ok &= expect(read_moqint(subscribe_ok, offset, draft, alias) && alias == 7,
                         label + " subscribe ok track alias");
            ok &= expect(read_moqint(subscribe_ok, offset, draft, parameter_count) && parameter_count == 1,
                         label + " subscribe ok largest-object parameter count");
            std::uint64_t largest_object_delta = 0;
            std::uint64_t largest_object_length = 0;
            std::uint64_t largest_group = 0;
            std::uint64_t largest_object = 0;
            ok &= expect(read_moqint(subscribe_ok, offset, draft, largest_object_delta) && largest_object_delta == 0x09,
                         label + " subscribe ok largest-object parameter delta");
            ok &= expect(read_moqint(subscribe_ok, offset, draft, largest_object_length),
                         label + " subscribe ok largest-object length");
            const std::size_t largest_object_end = offset + static_cast<std::size_t>(largest_object_length);
            ok &= expect(read_moqint(subscribe_ok, offset, draft, largest_group) && largest_group == 3,
                         label + " subscribe ok largest group");
            ok &= expect(read_moqint(subscribe_ok, offset, draft, largest_object) && largest_object == 5,
                         label + " subscribe ok largest object");
            ok &= expect(offset == largest_object_end && offset == frame.payload_end,
                         label + " subscribe ok largest-object payload boundary");
        }
        ok &= expect_uint16_frame(encode_subscribe_error_message(44, 2, "missing"), 0x05,
                                  frame, label + " subscribe error");
    }
    return ok;
}

bool test_peer_control_message_decoders_for_all_drafts() {
    bool ok = true;
    for (DraftVersion draft : {DraftVersion::kDraft14, DraftVersion::kDraft16, DraftVersion::kDraft18}) {
        const std::string label = draft_label(draft);

        SubscribeNamespaceMessage subscribe_namespace;
        ok &= expect(decode_subscribe_namespace_message(build_subscribe_namespace_message(draft), draft, subscribe_namespace),
                     label + " subscribe namespace decode");
        ok &= expect(subscribe_namespace.request_id == 91, label + " subscribe namespace request id");
        ok &= expect(subscribe_namespace.track_namespace_prefix == std::vector<std::string>({"live", "alpha"}),
                     label + " subscribe namespace tuple");
        if (draft == DraftVersion::kDraft18) {
            std::vector<std::uint8_t> legacy_subscribe_namespace = build_subscribe_namespace_message(draft);
            legacy_subscribe_namespace[0] = 0x11;
            ok &= expect(!decode_subscribe_namespace_message(legacy_subscribe_namespace, draft, subscribe_namespace),
                         label + " rejects legacy subscribe namespace type");
        }

        SubscribeMessage subscribe;
        ok &= expect(decode_subscribe_message(build_subscribe_message(draft), draft, subscribe),
                     label + " subscribe decode");
        ok &= expect(subscribe.request_id == 77, label + " subscribe request id");
        ok &= expect(subscribe.track_namespace == std::vector<std::string>({"live", "alpha"}),
                     label + " subscribe namespace tuple");
        ok &= expect(subscribe.track_name == "catalog", label + " subscribe track name");
        ok &= expect(subscribe.forward == 1 && subscribe.filter_type == 3 && subscribe.start_group_id == 9 &&
                         subscribe.start_object_id == 4,
                     label + " subscribe filter fields");

        SubscribeTracksMessage subscribe_tracks;
        if (draft == DraftVersion::kDraft18) {
            ok &= expect(decode_subscribe_tracks_message(build_subscribe_tracks_message(), draft, subscribe_tracks),
                         label + " subscribe tracks decode");
            ok &= expect(subscribe_tracks.request_id == 93, label + " subscribe tracks request id");
            ok &= expect(subscribe_tracks.track_namespace_prefix == std::vector<std::string>({"live", "alpha"}),
                         label + " subscribe tracks namespace tuple");
            ok &= expect(subscribe_tracks.forward == 0, label + " subscribe tracks forward parameter");
        } else {
            ok &= expect(!decode_subscribe_tracks_message(build_subscribe_tracks_message(), draft, subscribe_tracks),
                         label + " subscribe tracks rejected outside draft-18");
        }

        SubscribeUpdateMessage update;
        ok &= expect(decode_subscribe_update_message(build_subscribe_update_message(), update),
                     label + " subscribe update decode");
        ok &= expect(update.request_id == 101 && update.subscription_request_id == 77 && update.start_group_id == 12 &&
                         update.start_object_id == 5 && update.end_group_plus_one == 20,
                     label + " subscribe update fields");

        PublishOk publish_ok;
        ok &= expect(decode_publish_ok(build_publish_ok_message(draft), draft, publish_ok), label + " publish ok decode");
        ok &= expect(publish_ok.request_id == (draft == DraftVersion::kDraft18 ? 0 : 55) &&
                         publish_ok.forward == 1 && publish_ok.filter_type == 3,
                     label + " publish ok fields");

        PublishError publish_error;
        ok &= expect(decode_publish_error(build_publish_error_message(draft), draft, publish_error), label + " publish error decode");
        ok &= expect(publish_error.request_id == 55 && publish_error.error_code == 2 && publish_error.reason == "nope",
                     label + " publish error fields");

        MaxRequestIdMessage max_request_id;
        std::vector<std::uint8_t> max_payload;
        append_varint(max_payload, 900);
        std::vector<std::uint8_t> max_bytes;
        append_uint16_length_message(max_bytes, 0x15, max_payload);
        ok &= expect(decode_max_request_id_message(max_bytes, max_request_id), label + " max request id decode");
        ok &= expect(max_request_id.max_request_id == 900, label + " max request id field");
    }
    return ok;
}

bool test_subgroup_header_and_object_serdes_for_all_drafts() {
    bool ok = true;
    for (DraftVersion draft : {DraftVersion::kDraft14, DraftVersion::kDraft16, DraftVersion::kDraft18}) {
        const std::string label = draft_label(draft);
        const auto header = encode_subgroup_header(draft, 7, 3, 0, true);
        std::size_t offset = 0;
        std::uint64_t type = 0;
        std::uint64_t alias = 0;
        std::uint64_t group = 0;
        ok &= expect(read_varint(header, offset, type), label + " subgroup header type");
        ok &= expect(type == (draft == DraftVersion::kDraft14 ? 0x18 : 0x38), label + " subgroup header type value");
        ok &= expect(read_varint(header, offset, alias) && alias == 7, label + " subgroup header alias placement");
        ok &= expect(read_varint(header, offset, group) && group == 3, label + " subgroup header group placement");
        ok &= expect(offset == header.size(), label + " subgroup header contains no object fields");

        const std::vector<std::uint8_t> payload = {0xaa, 0xbb, 0xcc};
        const auto first = encode_subgroup_object(draft, std::nullopt, 5, payload);
        offset = 0;
        std::uint64_t object_delta = 0;
        std::uint64_t payload_length = 0;
        ok &= expect(read_varint(first, offset, object_delta) && object_delta == 5,
                     label + " first object absolute id placement");
        ok &= expect(read_varint(first, offset, payload_length) && payload_length == payload.size(),
                     label + " object payload length placement");
        ok &= expect(offset + payload_length == first.size(), label + " payload object has no status before payload");
        ok &= expect(std::vector<std::uint8_t>(first.begin() + static_cast<std::ptrdiff_t>(offset), first.end()) == payload,
                     label + " payload bytes placement");

        const auto second = encode_subgroup_object(draft, 5, 7, payload);
        offset = 0;
        ok &= expect(read_varint(second, offset, object_delta) && object_delta == 1,
                     label + " subsequent object id delta placement");

        const auto empty = encode_subgroup_object(draft, std::nullopt, 0, {});
        offset = 0;
        std::uint64_t object_status = 99;
        ok &= expect(read_varint(empty, offset, object_delta) && object_delta == 0, label + " empty object id");
        ok &= expect(read_varint(empty, offset, payload_length) && payload_length == 0, label + " empty payload length");
        if (draft == DraftVersion::kDraft18) {
            ok &= expect(read_varint(empty, offset, object_status) && object_status == 0,
                         label + " empty object status after zero length");
        }
        ok &= expect(offset == empty.size(), label + " empty object status/payload boundary");
    }
    return ok;
}

bool test_control_message_framing_and_parameter_regressions() {
    bool ok = true;

    std::vector<std::uint8_t> fetch_error_payload;
    append_varint(fetch_error_payload, 22);
    append_varint(fetch_error_payload, 3);
    append_string(fetch_error_payload, DraftVersion::kDraft16, "fetch failed");
    std::vector<std::uint8_t> fetch_error;
    append_uint16_length_message(fetch_error, 0x19, fetch_error_payload);
    std::size_t message_size = 0;
    ok &= expect(next_control_message(fetch_error, DraftVersion::kDraft14, message_size),
                 "FETCH_ERROR frames as uint16-length control message");
    ok &= expect(message_size == fetch_error.size(), "FETCH_ERROR message size");

    std::vector<std::uint8_t> duplicate_parameter_payload;
    append_varint(duplicate_parameter_payload, 77);
    append_track_namespace(duplicate_parameter_payload, DraftVersion::kDraft16, {"live", "alpha"});
    append_string(duplicate_parameter_payload, DraftVersion::kDraft16, "catalog");
    append_varint(duplicate_parameter_payload, 2);
    append_varint(duplicate_parameter_payload, 0x10);
    append_varint(duplicate_parameter_payload, 1);
    append_varint(duplicate_parameter_payload, 0);
    append_varint(duplicate_parameter_payload, 0);
    std::vector<std::uint8_t> duplicate_parameter_subscribe;
    append_uint16_length_message(duplicate_parameter_subscribe, 0x03, duplicate_parameter_payload);
    SubscribeMessage subscribe;
    ok &= expect(!decode_subscribe_message(duplicate_parameter_subscribe, DraftVersion::kDraft16, subscribe),
                 "draft-16 rejects duplicate delta-encoded parameter type");

    std::vector<std::uint8_t> draft14_bad_group_order_subscribe =
        build_subscribe_message(DraftVersion::kDraft14);
    for (std::size_t index = 0; index + 3 < draft14_bad_group_order_subscribe.size(); ++index) {
        if (draft14_bad_group_order_subscribe[index] == 128 &&
            draft14_bad_group_order_subscribe[index + 1] == 1 &&
            draft14_bad_group_order_subscribe[index + 2] == 1 &&
            draft14_bad_group_order_subscribe[index + 3] == 3) {
            draft14_bad_group_order_subscribe[index + 1] = 0;
            break;
        }
    }
    ok &= expect(!decode_subscribe_message(
                     draft14_bad_group_order_subscribe, DraftVersion::kDraft14, subscribe),
                 "draft-14 SUBSCRIBE rejects group order 0");

    std::vector<std::uint8_t> draft14_bad_group_order_publish_ok =
        build_publish_ok_message(DraftVersion::kDraft14);
    if (draft14_bad_group_order_publish_ok.size() > 5) {
        draft14_bad_group_order_publish_ok[5] = 0;
    }
    PublishOk publish_ok;
    ok &= expect(!decode_publish_ok(draft14_bad_group_order_publish_ok, DraftVersion::kDraft14, publish_ok),
                 "draft-14 PUBLISH_OK rejects group order 0");

    std::vector<std::uint8_t> request_ok_payload;
    append_varint(request_ok_payload, 44);
    append_varint(request_ok_payload, 1);
    append_varint(request_ok_payload, 0x10);
    append_varint(request_ok_payload, 1);
    std::vector<std::uint8_t> request_ok;
    append_uint16_length_message(request_ok, 0x07, request_ok_payload);
    PublishNamespaceOk namespace_ok;
    ok &= expect(decode_request_ok(request_ok, DraftVersion::kDraft16, namespace_ok),
                 "draft-16 REQUEST_OK decodes delta-encoded parameters");
    ok &= expect(namespace_ok.request_id == 44, "draft-16 REQUEST_OK request id with parameters");

    std::vector<std::uint8_t> draft18_request_ok_payload;
    append_moqint(draft18_request_ok_payload, DraftVersion::kDraft18, 2);
    append_moqint(draft18_request_ok_payload, DraftVersion::kDraft18, 0x21);
    append_moqint(draft18_request_ok_payload, DraftVersion::kDraft18, 0);
    append_moqint(draft18_request_ok_payload, DraftVersion::kDraft18, 1);
    append_moqint(draft18_request_ok_payload, DraftVersion::kDraft18, 7);
    std::vector<std::uint8_t> draft18_request_ok;
    append_moqint(draft18_request_ok, DraftVersion::kDraft18, 0x07);
    draft18_request_ok.push_back(static_cast<std::uint8_t>((draft18_request_ok_payload.size() >> 8) & 0xff));
    draft18_request_ok.push_back(static_cast<std::uint8_t>(draft18_request_ok_payload.size() & 0xff));
    draft18_request_ok.insert(
        draft18_request_ok.end(), draft18_request_ok_payload.begin(), draft18_request_ok_payload.end());
    ok &= expect(decode_request_ok(draft18_request_ok, DraftVersion::kDraft18, namespace_ok),
                 "draft-18 REQUEST_OK decodes multi-parameter delta KVPs");

    std::vector<std::uint8_t> draft18_subscribe_namespace_payload;
    append_moqint(draft18_subscribe_namespace_payload, DraftVersion::kDraft18, 2);
    append_track_namespace(draft18_subscribe_namespace_payload, DraftVersion::kDraft18, {"live"});
    append_moqint(draft18_subscribe_namespace_payload, DraftVersion::kDraft18, 2);
    append_moqint(draft18_subscribe_namespace_payload, DraftVersion::kDraft18, 0x21);
    append_moqint(draft18_subscribe_namespace_payload, DraftVersion::kDraft18, 0);
    append_moqint(draft18_subscribe_namespace_payload, DraftVersion::kDraft18, 1);
    append_moqint(draft18_subscribe_namespace_payload, DraftVersion::kDraft18, 7);
    std::vector<std::uint8_t> draft18_subscribe_namespace;
    append_moqint(draft18_subscribe_namespace, DraftVersion::kDraft18, 0x50);
    draft18_subscribe_namespace.push_back(
        static_cast<std::uint8_t>((draft18_subscribe_namespace_payload.size() >> 8) & 0xff));
    draft18_subscribe_namespace.push_back(static_cast<std::uint8_t>(draft18_subscribe_namespace_payload.size() & 0xff));
    draft18_subscribe_namespace.insert(draft18_subscribe_namespace.end(),
                                       draft18_subscribe_namespace_payload.begin(),
                                       draft18_subscribe_namespace_payload.end());
    SubscribeNamespaceMessage subscribe_namespace;
    ok &= expect(decode_subscribe_namespace_message(
                     draft18_subscribe_namespace, DraftVersion::kDraft18, subscribe_namespace),
                 "draft-18 SUBSCRIBE_NAMESPACE decodes multi-parameter delta KVPs");

    const std::vector<std::uint8_t> draft18_namespace_ok =
        encode_subscribe_namespace_ok_message(DraftVersion::kDraft18, 44);
    Uint16Frame frame;
    ok &= expect_uint16_frame(draft18_namespace_ok, 0x07, frame,
                              "draft-18 subscribe namespace ok delegates to REQUEST_OK");

    TrackMessage large_draft14_track{
        .draft = DraftVersion::kDraft14,
        .track_name = std::string(300, 'v'),
        .track_namespace = "live/alpha",
        .request_id = 66,
        .track_alias = 9,
        .largest_group_id = 3,
        .largest_object_id = 5,
        .content_exists = true,
    };
    const auto large_publish = encode_track_message(large_draft14_track);
    ok &= expect_varint_frame(large_publish, 0x1d, frame,
                              "draft-14 PUBLISH uses varint length for large payload");
    message_size = 0;
    ok &= expect(next_control_message(large_publish, DraftVersion::kDraft14, message_size),
                 "draft-14 large PUBLISH frames");
    ok &= expect(message_size == large_publish.size(), "draft-14 large PUBLISH message size");

    std::vector<std::uint8_t> large_publish_ok_payload = build_publish_ok_message(DraftVersion::kDraft14);
    large_publish_ok_payload.insert(large_publish_ok_payload.end(), 300, 0);
    // Rebuild the frame around the oversized payload from the helper's parsed payload.
    std::size_t payload_offset = 0;
    std::uint64_t ignored_type = 0;
    std::uint64_t ignored_length = 0;
    ok &= expect(read_varint(large_publish_ok_payload, payload_offset, ignored_type), "draft-14 helper type");
    ok &= expect(read_varint(large_publish_ok_payload, payload_offset, ignored_length), "draft-14 helper length");
    std::vector<std::uint8_t> oversized_publish_ok_payload(
        large_publish_ok_payload.begin() + static_cast<std::ptrdiff_t>(payload_offset),
        large_publish_ok_payload.end());
    oversized_publish_ok_payload.insert(oversized_publish_ok_payload.end(), 260, 0);
    std::vector<std::uint8_t> oversized_publish_ok;
    append_varint_length_message(oversized_publish_ok, 0x1e, oversized_publish_ok_payload);
    ok &= expect(next_control_message(oversized_publish_ok, DraftVersion::kDraft14, message_size),
                 "draft-14 oversized PUBLISH_OK frames with varint length");
    ok &= expect(message_size == oversized_publish_ok.size(), "draft-14 oversized PUBLISH_OK message size");

    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= test_setup_serdes_for_all_drafts();
    ok &= test_publisher_control_message_encoders_for_all_drafts();
    ok &= test_peer_control_message_decoders_for_all_drafts();
    ok &= test_subgroup_header_and_object_serdes_for_all_drafts();
    ok &= test_control_message_framing_and_parameter_regressions();
    return ok ? 0 : 1;
}
