#include "openmoq/publisher/transport/moqt_control_messages.h"

#include <algorithm>

#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

namespace openmoq::publisher::transport {

namespace {

constexpr std::uint64_t kClientSetupType = 0x20;
constexpr std::uint64_t kServerSetupType = 0x21;
constexpr std::uint64_t kSetupType = 0x2f00;
constexpr std::uint64_t kSubscribeUpdateType = 0x02;
constexpr std::uint64_t kSubscribeType = 0x03;
constexpr std::uint64_t kSubscribeOkType = 0x04;
constexpr std::uint64_t kSubscribeErrorType = 0x05;
constexpr std::uint64_t kRequestErrorType = 0x05;
constexpr std::uint64_t kPublishNamespaceType = 0x06;
constexpr std::uint64_t kPublishNamespaceOkType = 0x07;
constexpr std::uint64_t kRequestOkType = 0x07;
constexpr std::uint64_t kPublishNamespaceErrorType = 0x08;
constexpr std::uint64_t kPublishNamespaceDoneType = 0x09;
constexpr std::uint64_t kNamespaceDoneType = 0x0e;
constexpr std::uint64_t kPublishDoneType = 0x0b;
constexpr std::uint64_t kSubscribeNamespaceType = 0x11;
constexpr std::uint64_t kSubscribeNamespaceOkType = 0x12;
constexpr std::uint64_t kMaxRequestIdType = 0x15;
constexpr std::uint64_t kPublishType = 0x1d;
constexpr std::uint64_t kPublishOkType = 0x1e;
constexpr std::uint64_t kPublishErrorType = 0x1f;
constexpr std::uint64_t kSubscribeNamespaceTypeDraft18 = 0x50;
constexpr std::uint64_t kSubscribeTracksType = 0x51;
// SUBGROUP_HEADER type byte layout (draft-16 §10.4.2):
//   bit 0: EXTENSIONS         (0 = no per-object extensions on this stream)
//   bits 1-2: SUBGROUP_ID_MODE (00 = Subgroup ID absent, value is 0)
//   bit 3: END_OF_GROUP       (set when this subgroup owns the group's largest)
//   bit 4: always 1 (identifies a subgroup header)
//   bit 5: DEFAULT_PRIORITY   (1 = priority byte omitted, use subscription default)
// Base byte = 0x10 | 0x20 = 0x30. End-of-group variant = 0x38.
constexpr std::uint64_t kSubgroupHeaderType = 0x30;
constexpr std::uint64_t kSubgroupHeaderEndOfGroupBit = 0x08;
constexpr std::uint64_t kObjectDatagramTypeDraft14 = 0x10;
constexpr std::uint64_t kSetupParamPath = 0x1;
constexpr std::uint64_t kSetupParamMaxRequestId = 0x2;
constexpr std::uint64_t kParamAuthorizationToken = 0x3;
constexpr std::uint64_t kSetupParamAuthority = 0x5;
constexpr std::uint64_t kDraft14Version = 0xff00000eULL;
constexpr std::uint64_t kDraft16Version = 0xff000010ULL;
constexpr std::uint64_t kDraft17Version = 0xff000011ULL;
constexpr std::uint64_t kDraft18Version = 0xff000012ULL;
constexpr std::uint64_t kMaxQuicVarintValue = 4611686018427387903ULL;
constexpr std::uint64_t kSubscribeErrorTrackDoesNotExist = 0x2;
constexpr std::uint8_t kGroupOrderAscending = 0x1;
constexpr std::uint8_t kForwardPreference = 0x1;
constexpr std::uint8_t kContentExistsTrue = 0x1;
constexpr std::uint8_t kPublisherPriority = 0x00;

bool decode_varint_impl(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint64_t& value);
bool decode_vi64_impl(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint64_t& value);

bool uses_moq_vi64(DraftVersion draft) {
    return draft == DraftVersion::kDraft17 || draft == DraftVersion::kDraft18;
}

bool decode_moqint_impl(std::span<const std::uint8_t> bytes,
                        std::size_t& offset,
                        DraftVersion draft,
                        std::uint64_t& value) {
    return uses_moq_vi64(draft) ? decode_vi64_impl(bytes, offset, value) : decode_varint_impl(bytes, offset, value);
}

std::vector<std::uint8_t> to_bytes(std::string_view value) {
    return std::vector<std::uint8_t>(value.begin(), value.end());
}

std::vector<std::string> split_track_namespace(std::string_view track_namespace) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= track_namespace.size()) {
        const std::size_t slash = track_namespace.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? track_namespace.size() : slash;
        if (end > start) {
            parts.emplace_back(track_namespace.substr(start, end - start));
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    if (parts.empty()) {
        parts.emplace_back(track_namespace);
    }
    return parts;
}

void append_uint16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

bool parse_uint16_length_message(std::span<const std::uint8_t> bytes,
                                 DraftVersion draft,
                                 std::uint64_t expected_type,
                                 std::size_t& payload_offset,
                                 std::size_t& payload_length) {
    std::size_t offset = 0;
    std::uint64_t type = 0;
    if (!decode_moqint_impl(bytes, offset, draft, type) || type != expected_type || offset + 2 > bytes.size()) {
        return false;
    }
    payload_length =
        (static_cast<std::size_t>(bytes[offset]) << 8) | static_cast<std::size_t>(bytes[offset + 1]);
    offset += 2;
    if (offset + payload_length != bytes.size()) {
        return false;
    }
    payload_offset = offset;
    return true;
}

bool parse_varint_length_message(std::span<const std::uint8_t> bytes,
                                 DraftVersion draft,
                                 std::uint64_t expected_type,
                                 std::size_t& payload_offset,
                                 std::size_t& payload_length) {
    std::size_t offset = 0;
    std::uint64_t type = 0;
    std::uint64_t length = 0;
    if (!decode_moqint_impl(bytes, offset, draft, type) || type != expected_type ||
        !decode_moqint_impl(bytes, offset, draft, length)) {
        return false;
    }
    if (offset + length != bytes.size()) {
        return false;
    }
    payload_offset = offset;
    payload_length = static_cast<std::size_t>(length);
    return true;
}

bool parse_publish_family_message(std::span<const std::uint8_t> bytes,
                                  DraftVersion draft,
                                  std::uint64_t expected_type,
                                  std::size_t& payload_offset,
                                  std::size_t& payload_length) {
    if (draft == DraftVersion::kDraft14) {
        return parse_varint_length_message(bytes, draft, expected_type, payload_offset, payload_length);
    }
    return parse_uint16_length_message(bytes, draft, expected_type, payload_offset, payload_length);
}

bool decode_reason_phrase(std::span<const std::uint8_t> bytes,
                          std::size_t& offset,
                          DraftVersion draft,
                          std::string& reason) {
    std::uint64_t length = 0;
    if (!decode_moqint_impl(bytes, offset, draft, length) || offset + length > bytes.size()) {
        return false;
    }
    reason.assign(reinterpret_cast<const char*>(bytes.data() + offset), static_cast<std::size_t>(length));
    offset += static_cast<std::size_t>(length);
    return true;
}

bool append_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    if (value > kMaxQuicVarintValue) {
        return false;
    }

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

    return true;
}

bool append_vi64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    if (value <= 127) {
        out.push_back(static_cast<std::uint8_t>(value));
    } else if (value <= 16383) {
        out.push_back(static_cast<std::uint8_t>(0x80 | ((value >> 8) & 0x3f)));
        out.push_back(static_cast<std::uint8_t>(value & 0xff));
    } else if (value <= 2097151ULL) {
        out.push_back(static_cast<std::uint8_t>(0xc0 | ((value >> 16) & 0x1f)));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(value & 0xff));
    } else if (value <= 268435455ULL) {
        out.push_back(static_cast<std::uint8_t>(0xe0 | ((value >> 24) & 0x0f)));
        out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(value & 0xff));
    } else if (value <= 34359738367ULL) {
        out.push_back(static_cast<std::uint8_t>(0xf0 | ((value >> 32) & 0x07)));
        out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(value & 0xff));
    } else if (value <= 4398046511103ULL) {
        out.push_back(static_cast<std::uint8_t>(0xf8 | ((value >> 40) & 0x03)));
        out.push_back(static_cast<std::uint8_t>((value >> 32) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(value & 0xff));
    } else if (value <= 562949953421311ULL) {
        out.push_back(static_cast<std::uint8_t>(0xfc | ((value >> 48) & 0x01)));
        out.push_back(static_cast<std::uint8_t>((value >> 40) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 32) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(value & 0xff));
    } else if (value <= 72057594037927935ULL) {
        out.push_back(0xfe);
        for (int shift = 48; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
        }
    } else {
        out.push_back(0xff);
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
        }
    }
    return true;
}

bool append_moqint(std::vector<std::uint8_t>& out, DraftVersion draft, std::uint64_t value) {
    return uses_moq_vi64(draft) ? append_vi64(out, value) : append_varint(out, value);
}

void append_string(std::vector<std::uint8_t>& out, DraftVersion draft, std::string_view value) {
    append_moqint(out, draft, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void append_track_namespace(std::vector<std::uint8_t>& out, DraftVersion draft, std::string_view track_namespace) {
    const auto parts = split_track_namespace(track_namespace);
    append_moqint(out, draft, parts.size());
    for (const auto& part : parts) {
        append_string(out, draft, part);
    }
}

void append_location(std::vector<std::uint8_t>& out, DraftVersion draft, std::size_t group_id, std::size_t object_id) {
    append_moqint(out, draft, group_id);
    append_moqint(out, draft, object_id);
}

bool decode_track_namespace(std::span<const std::uint8_t> bytes,
                            std::size_t& offset,
                            DraftVersion draft,
                            std::vector<std::string>& track_namespace) {
    std::uint64_t entry_count = 0;
    if (!decode_moqint_impl(bytes, offset, draft, entry_count)) {
        return false;
    }
    track_namespace.clear();
    track_namespace.reserve(static_cast<std::size_t>(entry_count));
    for (std::uint64_t index = 0; index < entry_count; ++index) {
        std::uint64_t length = 0;
        if (!decode_moqint_impl(bytes, offset, draft, length) || offset + length > bytes.size()) {
            return false;
        }
        track_namespace.emplace_back(reinterpret_cast<const char*>(bytes.data() + offset), static_cast<std::size_t>(length));
        offset += static_cast<std::size_t>(length);
    }
    return true;
}

bool decode_varint_impl(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint64_t& value) {
    if (offset >= bytes.size()) {
        return false;
    }

    const std::uint8_t first = bytes[offset];
    const std::size_t length = 1ULL << (first >> 6);
    if (offset + length > bytes.size()) {
        return false;
    }

    value = first & 0x3f;
    for (std::size_t index = 1; index < length; ++index) {
        value = (value << 8) | bytes[offset + index];
    }
    offset += length;
    return true;
}

bool decode_vi64_impl(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint64_t& value) {
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
        prefix_mask = 0x00;
    } else {
        length = 9;
        prefix_mask = 0x00;
    }
    if (offset + length > bytes.size()) {
        return false;
    }
    value = first & prefix_mask;
    for (std::size_t index = 1; index < length; ++index) {
        value = (value << 8) | bytes[offset + index];
    }
    offset += length;
    return true;
}

void append_parameter(std::vector<std::uint8_t>& out,
                      DraftVersion draft,
                      std::uint64_t type,
                      std::span<const std::uint8_t> value) {
    append_moqint(out, draft, type);
    if ((type & 0x1ULL) == 0) {
        out.insert(out.end(), value.begin(), value.end());
    } else {
        append_moqint(out, draft, value.size());
        out.insert(out.end(), value.begin(), value.end());
    }
}

void append_setup_option_delta(std::vector<std::uint8_t>& out,
                               DraftVersion draft,
                               std::uint64_t& previous_type,
                               std::uint64_t type,
                               std::span<const std::uint8_t> value) {
    append_moqint(out, draft, type - previous_type);
    if ((type & 0x1ULL) != 0) {
        append_moqint(out, draft, value.size());
    }
    out.insert(out.end(), value.begin(), value.end());
    previous_type = type;
}

void append_parameter_delta(std::vector<std::uint8_t>& out,
                            DraftVersion draft,
                            std::uint64_t& previous_type,
                            std::uint64_t type,
                            std::span<const std::uint8_t> value) {
    const std::uint64_t delta = type - previous_type;
    append_moqint(out, draft, delta);
    if ((type & 0x1ULL) == 0) {
        out.insert(out.end(), value.begin(), value.end());
    } else {
        append_moqint(out, draft, value.size());
        out.insert(out.end(), value.begin(), value.end());
    }
    previous_type = type;
}

bool decode_parameter_type(std::span<const std::uint8_t> bytes,
                           std::size_t& offset,
                           DraftVersion draft,
                           std::uint64_t& previous_type,
                           bool delta_encoded,
                           std::uint64_t& parameter_type) {
    std::uint64_t encoded_type = 0;
    if (!decode_moqint_impl(bytes, offset, draft, encoded_type)) {
        return false;
    }
    if (!delta_encoded) {
        parameter_type = encoded_type;
        previous_type = parameter_type;
        return true;
    }
    if (previous_type != 0 && encoded_type == 0) {
        return false;
    }
    if (encoded_type > std::numeric_limits<std::uint64_t>::max() - previous_type) {
        return false;
    }
    parameter_type = previous_type + encoded_type;
    previous_type = parameter_type;
    return true;
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

    return kDraft18Version;
}

}  // namespace

std::vector<std::uint8_t> encode_varint(std::uint64_t value) {
    std::vector<std::uint8_t> bytes;
    if (!append_varint(bytes, value)) {
        return {};
    }
    return bytes;
}

bool decode_varint(std::span<const std::uint8_t> bytes, std::size_t& offset, std::uint64_t& value) {
    return decode_varint_impl(bytes, offset, value);
}

bool next_control_message(std::span<const std::uint8_t> bytes, DraftVersion draft, std::size_t& message_size) {
    std::size_t offset = 0;
    std::uint64_t type = 0;
    if (!decode_moqint_impl(bytes, offset, draft, type)) {
        return false;
    }

    switch (type) {
        // uint16 payload length: type varint + uint16 length + payload
        case kClientSetupType:
        case kServerSetupType:
        case kSetupType:
        case kSubscribeUpdateType:
        case kSubscribeType:
        case kSubscribeOkType:
        case kSubscribeErrorType:
        case kPublishNamespaceType:
        case kPublishNamespaceOkType:
        case kPublishNamespaceErrorType:
        case kPublishNamespaceDoneType:
        case kNamespaceDoneType:
        case kPublishDoneType:
        case kMaxRequestIdType:
        case kSubscribeNamespaceTypeDraft18:
        case kSubscribeTracksType:
        case 0x0a:  // UNSUBSCRIBE
        case 0x10:  // GOAWAY
        case 0x14:  // SUBSCRIBE_DONE (draft-14)
        case 0x16:  // FETCH
        case 0x17:  // FETCH_CANCEL
        case 0x18:  // FETCH_OK
        case 0x19:  // FETCH_ERROR
        case 0x1a:  // REQUESTS_BLOCKED (draft-16)
        {
            if (offset + 2 > bytes.size()) {
                return false;
            }
            const std::size_t payload_length =
                (static_cast<std::size_t>(bytes[offset]) << 8) | static_cast<std::size_t>(bytes[offset + 1]);
            message_size = offset + 2 + payload_length;
            return bytes.size() >= message_size;
        }
        case kPublishType:
        case kPublishOkType:
        case kPublishErrorType:
        {
            if (draft == DraftVersion::kDraft14) {
                std::uint64_t payload_length = 0;
                if (!decode_moqint_impl(bytes, offset, draft, payload_length)) {
                    return false;
                }
                message_size = offset + static_cast<std::size_t>(payload_length);
                return bytes.size() >= message_size;
            }
            if (offset + 2 > bytes.size()) {
                return false;
            }
            const std::size_t payload_length =
                (static_cast<std::size_t>(bytes[offset]) << 8) | static_cast<std::size_t>(bytes[offset + 1]);
            message_size = offset + 2 + payload_length;
            return bytes.size() >= message_size;
        }
        // varint payload length: type varint + varint length + payload
        case kSubscribeNamespaceOkType:      // 0x12
        case 0x13:  // SUBSCRIBE_NAMESPACE_ERROR (draft-14)
        case 0x1b:  // UNSUBSCRIBE_NAMESPACE  (draft-14)
        {
            std::uint64_t payload_length = 0;
            if (!decode_moqint_impl(bytes, offset, draft, payload_length)) {
                return false;
            }
            message_size = offset + static_cast<std::size_t>(payload_length);
            return bytes.size() >= message_size;
        }
        case kSubscribeNamespaceType: {  // 0x11
            if (draft == DraftVersion::kDraft16) {
                if (offset + 2 > bytes.size()) {
                    return false;
                }
                const std::size_t payload_length =
                    (static_cast<std::size_t>(bytes[offset]) << 8) | static_cast<std::size_t>(bytes[offset + 1]);
                message_size = offset + 2 + payload_length;
                return bytes.size() >= message_size;
            }
            std::uint64_t payload_length = 0;
            if (!decode_moqint_impl(bytes, offset, draft, payload_length)) {
                return false;
            }
            message_size = offset + static_cast<std::size_t>(payload_length);
            return bytes.size() >= message_size;
        }
        default:
            return false;
    }
}

std::vector<std::uint8_t> encode_setup_message(const SetupMessage& message) {
    if (uses_moq_vi64(message.draft)) {
        std::vector<std::uint8_t> payload;
        if (message.transport == TransportKind::kRawQuic) {
            const std::vector<std::uint8_t> path = to_bytes(message.path);
            const std::vector<std::uint8_t> authority = to_bytes(message.authority);
            std::uint64_t previous_option_type = 0;
            append_setup_option_delta(payload, message.draft, previous_option_type, kSetupParamPath, path);
            if (message.authorization_token.has_value()) {
                append_setup_option_delta(payload, message.draft, previous_option_type, kParamAuthorizationToken, *message.authorization_token);
            }
            append_setup_option_delta(payload, message.draft, previous_option_type, kSetupParamAuthority, authority);
        } else if (message.authorization_token.has_value()) {
            std::uint64_t previous_option_type = 0;
            append_setup_option_delta(payload, message.draft, previous_option_type, kParamAuthorizationToken, *message.authorization_token);
        }

        std::vector<std::uint8_t> message_bytes;
        append_moqint(message_bytes, message.draft, kSetupType);
        append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
        message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
        return message_bytes;
    }

    std::vector<std::uint8_t> payload;

    if (message.draft == DraftVersion::kDraft14) {
        append_moqint(payload, message.draft, 1);
        append_moqint(payload, message.draft, draft_version_number(message.draft));
    }

    const bool include_native_quic_location = message.transport == TransportKind::kRawQuic;
    std::uint64_t parameter_count = include_native_quic_location ? 3 : 1;
    if (message.authorization_token.has_value()) {
        ++parameter_count;
    }
    append_moqint(payload, message.draft, parameter_count);
    std::uint64_t previous_parameter_type = 0;
    if (include_native_quic_location) {
        const std::vector<std::uint8_t> path = to_bytes(message.path);
        const std::vector<std::uint8_t> authority = to_bytes(message.authority);
        if (message.draft == DraftVersion::kDraft16) {
            append_parameter_delta(payload, message.draft, previous_parameter_type, kSetupParamPath, path);
        } else {
            append_parameter(payload, message.draft, kSetupParamAuthority, authority);
            append_parameter(payload, message.draft, kSetupParamPath, path);
        }
    }
    std::vector<std::uint8_t> max_request_id;
    append_moqint(max_request_id, message.draft, message.max_request_id);
    if (message.draft == DraftVersion::kDraft16) {
        append_parameter_delta(payload, message.draft, previous_parameter_type, kSetupParamMaxRequestId, max_request_id);
        if (message.authorization_token.has_value()) {
            append_parameter_delta(payload, message.draft, previous_parameter_type, kParamAuthorizationToken, *message.authorization_token);
        }
        if (include_native_quic_location) {
            const std::vector<std::uint8_t> authority = to_bytes(message.authority);
            append_parameter_delta(payload, message.draft, previous_parameter_type, kSetupParamAuthority, authority);
        }
    } else {
        append_parameter(payload, message.draft, kSetupParamMaxRequestId, max_request_id);
        if (message.authorization_token.has_value()) {
            append_parameter(payload, message.draft, kParamAuthorizationToken, *message.authorization_token);
        }
    }

    std::vector<std::uint8_t> message_bytes;
    append_moqint(message_bytes, message.draft, kClientSetupType);
    append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

bool decode_server_setup_message(std::span<const std::uint8_t> bytes, ServerSetupMessage& message) {
    std::size_t offset = 0;
    std::uint64_t message_type = 0;
    if (!decode_varint_impl(bytes, offset, message_type) ||
        (message_type != kServerSetupType && message_type != kSetupType)) {
        offset = 0;
        if (!decode_vi64_impl(bytes, offset, message_type) ||
            (message_type != kServerSetupType && message_type != kSetupType)) {
            return false;
        }
    }
    const DraftVersion message_draft = message_type == kSetupType ? DraftVersion::kDraft18 : DraftVersion::kDraft16;
    if (message_type != kServerSetupType && message_type != kSetupType) {
        return false;
    }

    if (offset + 2 > bytes.size()) {
        return false;
    }
    const std::size_t payload_length =
        (static_cast<std::size_t>(bytes[offset]) << 8) | static_cast<std::size_t>(bytes[offset + 1]);
    offset += 2;
    if (offset + payload_length > bytes.size()) {
        return false;
    }

    const std::size_t payload_end = offset + payload_length;
    const auto payload_bytes = bytes.subspan(0, payload_end);
    if (payload_end == offset && message_type != kSetupType) {
        return false;
    }

    if (message_type == kSetupType) {
        message.draft = DraftVersion::kDraft18;
        std::uint64_t previous_option_type = 0;
        while (offset < payload_end) {
            std::uint64_t option_type = 0;
            if (!decode_parameter_type(payload_bytes,
                                       offset,
                                       message_draft,
                                       previous_option_type,
                                       true,
                                       option_type)) {
                return false;
            }
            if ((option_type & 0x1ULL) == 0) {
                std::uint64_t option_value = 0;
                if (!decode_moqint_impl(payload_bytes, offset, message_draft, option_value)) {
                    return false;
                }
            } else {
                std::uint64_t option_length = 0;
                if (!decode_moqint_impl(payload_bytes, offset, message_draft, option_length) ||
                    option_length > payload_end - offset) {
                    return false;
                }
                offset += static_cast<std::size_t>(option_length);
            }
        }
        return offset == payload_end;
    }

    const std::uint8_t first_payload_byte = bytes[offset];
    if ((first_payload_byte & 0xc0) == 0xc0) {
        std::uint64_t selected_version = 0;
        if (!decode_varint_impl(payload_bytes, offset, selected_version)) {
            return false;
        }
        if (selected_version == kDraft14Version) {
            message.draft = DraftVersion::kDraft14;
        } else if (selected_version == kDraft16Version) {
            message.draft = DraftVersion::kDraft16;
        } else {
            return false;
        }
    } else {
        message.draft = DraftVersion::kDraft16;
    }

    std::uint64_t parameter_count = 0;
    if (!decode_moqint_impl(payload_bytes, offset, message.draft, parameter_count)) {
        return false;
    }

    std::uint64_t previous_parameter_type = 0;
    const bool delta_encoded = message.draft == DraftVersion::kDraft16;
    for (std::uint64_t parameter_index = 0; parameter_index < parameter_count; ++parameter_index) {
        std::uint64_t parameter_type = 0;
        if (!decode_parameter_type(payload_bytes, offset, message.draft, previous_parameter_type, delta_encoded, parameter_type)) {
            return false;
        }

        if ((parameter_type & 0x1ULL) == 0) {
            std::uint64_t value = 0;
            if (!decode_moqint_impl(payload_bytes, offset, message.draft, value)) {
                return false;
            }
            if (parameter_type == kSetupParamMaxRequestId) {
                message.max_request_id = value;
            }
            continue;
        }

        std::uint64_t parameter_length = 0;
        if (!decode_moqint_impl(payload_bytes, offset, message.draft, parameter_length) ||
            offset + parameter_length > payload_end) {
            return false;
        }
        offset += parameter_length;
    }

    return offset == payload_end;
}

bool decode_setup_response_message(std::span<const std::uint8_t> bytes,
                                   DraftVersion expected_draft,
                                   ServerSetupMessage& message) {
    std::size_t offset = 0;
    std::uint64_t message_type = 0;
    if (!decode_moqint_impl(bytes, offset, expected_draft, message_type)) {
        return false;
    }

    if (uses_moq_vi64(expected_draft)) {
        if (message_type != kSetupType) {
            return false;
        }
    } else if (message_type != kServerSetupType) {
        return false;
    }

    if (!decode_server_setup_message(bytes, message)) {
        return false;
    }
    if (uses_moq_vi64(expected_draft) && message.draft == DraftVersion::kDraft18) {
        message.draft = expected_draft;
    }
    return message.draft == expected_draft;
}

std::vector<std::uint8_t> encode_server_setup_message(const ServerSetupMessage& message) {
    if (uses_moq_vi64(message.draft)) {
        std::vector<std::uint8_t> message_bytes;
        append_moqint(message_bytes, message.draft, kSetupType);
        append_uint16(message_bytes, 0);
        return message_bytes;
    }

    std::vector<std::uint8_t> payload;
    if (message.draft == DraftVersion::kDraft14) {
        append_moqint(payload, message.draft, draft_version_number(message.draft));
    }

    append_moqint(payload, message.draft, 1);
    std::uint64_t previous_parameter_type = 0;
    std::vector<std::uint8_t> max_request_id;
    append_moqint(max_request_id, message.draft, message.max_request_id);
    if (message.draft == DraftVersion::kDraft16) {
        append_parameter_delta(payload, message.draft, previous_parameter_type, kSetupParamMaxRequestId, max_request_id);
    } else {
        append_parameter(payload, message.draft, kSetupParamMaxRequestId, max_request_id);
    }

    std::vector<std::uint8_t> message_bytes;
    append_moqint(message_bytes, message.draft, kServerSetupType);
    append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

bool decode_max_request_id_message(std::span<const std::uint8_t> bytes, MaxRequestIdMessage& message) {
    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    if (!parse_uint16_length_message(bytes, DraftVersion::kDraft16, kMaxRequestIdType, payload_offset, payload_length)) {
        return false;
    }
    std::size_t offset = payload_offset;
    return decode_moqint_impl(bytes, offset, DraftVersion::kDraft16, message.max_request_id) &&
           offset == payload_offset + payload_length;
}

std::vector<std::uint8_t> encode_namespace_message(const NamespaceMessage& message) {
    std::vector<std::uint8_t> payload;
    append_moqint(payload, message.draft, message.request_id);
    if (message.draft == DraftVersion::kDraft17) {
        append_moqint(payload, message.draft, 0);  // Required Request ID Delta: no dependency.
    }
    append_track_namespace(payload, message.draft, message.track_namespace);
    if (message.authorization_token.has_value()) {
        append_moqint(payload, message.draft, 1);
        append_parameter(payload, message.draft, kParamAuthorizationToken, *message.authorization_token);
    } else {
        append_moqint(payload, message.draft, 0);
    }

    std::vector<std::uint8_t> message_bytes;
    append_moqint(message_bytes, message.draft, kPublishNamespaceType);
    append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

std::vector<std::uint8_t> encode_request_ok_message(DraftVersion draft, std::uint64_t request_id) {
    std::vector<std::uint8_t> payload;
    if (!uses_moq_vi64(draft)) {
        append_moqint(payload, draft, request_id);
    }
    append_moqint(payload, draft, 0);

    std::vector<std::uint8_t> message_bytes;
    append_moqint(message_bytes, draft, kRequestOkType);
    append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

bool decode_request_ok(std::span<const std::uint8_t> bytes, DraftVersion draft, PublishNamespaceOk& message) {
    if (draft == DraftVersion::kDraft14) {
        return decode_publish_namespace_ok(bytes, message);
    }

    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    if (!parse_uint16_length_message(bytes, draft, kRequestOkType, payload_offset, payload_length)) {
        return false;
    }
    std::size_t offset = payload_offset;
    const std::size_t payload_end = payload_offset + payload_length;
    std::uint64_t parameter_count = 0;
    message.request_id = 0;

    if (!uses_moq_vi64(draft) && !decode_moqint_impl(bytes, offset, draft, message.request_id)) {
        return false;
    }
    if (!decode_moqint_impl(bytes, offset, draft, parameter_count)) {
        return false;
    }
    std::uint64_t previous_parameter_type = 0;
    for (std::uint64_t parameter_index = 0; parameter_index < parameter_count; ++parameter_index) {
        std::uint64_t parameter_type = 0;
        if (!decode_parameter_type(bytes, offset, draft, previous_parameter_type, true, parameter_type)) {
            return false;
        }
        if ((parameter_type & 0x1ULL) == 0) {
            std::uint64_t ignored_value = 0;
            if (!decode_moqint_impl(bytes, offset, draft, ignored_value)) {
                return false;
            }
        } else {
            std::uint64_t parameter_length = 0;
            if (!decode_moqint_impl(bytes, offset, draft, parameter_length) || offset + parameter_length > payload_end) {
                return false;
            }
            offset += static_cast<std::size_t>(parameter_length);
        }
    }

    // Track properties, if any, occupy the remaining payload and are not
    // consumed by this minimal request-ok parser.
    return offset <= payload_end;
}

bool decode_request_error(std::span<const std::uint8_t> bytes, DraftVersion draft, RequestError& message) {
    if (draft == DraftVersion::kDraft14) {
        PublishNamespaceError namespace_error;
        if (!decode_publish_namespace_error(bytes, namespace_error)) {
            return false;
        }
        message.request_id = namespace_error.request_id;
        message.error_code = namespace_error.error_code;
        message.retry_interval = 0;
        message.reason = std::move(namespace_error.reason);
        return true;
    }

    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    if (!parse_uint16_length_message(bytes, draft, kRequestErrorType, payload_offset, payload_length)) {
        return false;
    }
    std::size_t offset = payload_offset;
    const std::size_t payload_end = payload_offset + payload_length;
    if (draft == DraftVersion::kDraft16) {
        std::uint64_t parameter_count = 0;
        return decode_moqint_impl(bytes, offset, draft, message.request_id) &&
               decode_moqint_impl(bytes, offset, draft, message.error_code) &&
               decode_moqint_impl(bytes, offset, draft, message.retry_interval) &&
               decode_reason_phrase(bytes.subspan(0, payload_end), offset, draft, message.reason) &&
               decode_moqint_impl(bytes, offset, draft, parameter_count) && parameter_count == 0 && offset == payload_end;
    }

    if (!uses_moq_vi64(draft) && !decode_moqint_impl(bytes, offset, draft, message.request_id)) {
        return false;
    }
    if (uses_moq_vi64(draft)) {
        message.request_id = 0;
    }
    if (!decode_moqint_impl(bytes, offset, draft, message.error_code) ||
        !decode_moqint_impl(bytes, offset, draft, message.retry_interval) ||
        !decode_reason_phrase(bytes.subspan(0, payload_end), offset, draft, message.reason)) {
        return false;
    }

    if (offset == payload_end) {
        return true;
    }

    // Optional Redirect structure.
    std::uint64_t connect_uri_length = 0;
    std::vector<std::string> redirect_namespace;
    std::uint64_t track_name_length = 0;
    if (!decode_moqint_impl(bytes, offset, draft, connect_uri_length) || offset + connect_uri_length > payload_end) {
        return false;
    }
    offset += static_cast<std::size_t>(connect_uri_length);
    if (!decode_track_namespace(bytes.subspan(0, payload_end), offset, draft, redirect_namespace) ||
        !decode_moqint_impl(bytes, offset, draft, track_name_length) || offset + track_name_length > payload_end) {
        return false;
    }
    offset += static_cast<std::size_t>(track_name_length);
    return offset == payload_end;
}

bool decode_subscribe_namespace_message(std::span<const std::uint8_t> bytes,
                                        DraftVersion draft,
                                        SubscribeNamespaceMessage& message) {
    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    const std::uint64_t message_type =
        draft == DraftVersion::kDraft18 ? kSubscribeNamespaceTypeDraft18 : kSubscribeNamespaceType;
    const bool framed =
        draft == DraftVersion::kDraft14
            ? parse_varint_length_message(bytes, draft, message_type, payload_offset, payload_length)
            : parse_uint16_length_message(bytes, draft, message_type, payload_offset, payload_length);
    if (!framed) {
        return false;
    }
    std::size_t offset = payload_offset;
    const std::size_t payload_end = payload_offset + payload_length;
    std::uint64_t subscribe_options = 0;
    std::uint64_t parameters = 0;
    if (!decode_moqint_impl(bytes, offset, draft, message.request_id)) {
        return false;
    }
    if (draft == DraftVersion::kDraft17) {
        std::uint64_t required_request_id_delta = 0;
        if (!decode_moqint_impl(bytes, offset, draft, required_request_id_delta) ||
            required_request_id_delta * 2 > message.request_id) {
            return false;
        }
    }
    if (!decode_track_namespace(bytes.subspan(0, payload_end), offset, draft, message.track_namespace_prefix)) {
        return false;
    }
    if (draft == DraftVersion::kDraft16 &&
        (!decode_moqint_impl(bytes, offset, draft, subscribe_options) || subscribe_options > 2)) {
        return false;
    }
    if (!decode_moqint_impl(bytes, offset, draft, parameters)) {
        return false;
    }

    std::uint64_t previous_parameter_type = 0;
    for (std::uint64_t index = 0; index < parameters; ++index) {
        std::uint64_t parameter_type = 0;
        if (!decode_parameter_type(bytes, offset, draft, previous_parameter_type, true, parameter_type)) {
            return false;
        }
        if ((parameter_type & 0x1ULL) == 0) {
            std::uint64_t value = 0;
            if (!decode_moqint_impl(bytes, offset, draft, value)) {
                return false;
            }
            if (draft == DraftVersion::kDraft16 && parameter_type == 0x10 && value > 1) {
                return false;
            }
            if (draft == DraftVersion::kDraft16 && parameter_type != 0x10) {
                return false;
            }
            continue;
        }
        std::uint64_t parameter_length = 0;
        if (!decode_moqint_impl(bytes, offset, draft, parameter_length) || offset + parameter_length > payload_end) {
            return false;
        }
        if (draft == DraftVersion::kDraft16 && parameter_type != 0x03) {
            return false;
        }
        offset += static_cast<std::size_t>(parameter_length);
    }
    return offset == payload_end;
}

std::vector<std::uint8_t> encode_subscribe_namespace_ok_message(DraftVersion draft, std::uint64_t request_id) {
    if (draft == DraftVersion::kDraft16 || draft == DraftVersion::kDraft18) {
        return encode_request_ok_message(draft, request_id);
    }

    std::vector<std::uint8_t> payload;
    append_moqint(payload, draft, request_id);

    std::vector<std::uint8_t> message_bytes;
    append_moqint(message_bytes, draft, kSubscribeNamespaceOkType);
    append_moqint(message_bytes, draft, payload.size());
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

bool decode_subscribe_filter(std::span<const std::uint8_t> bytes,
                             std::size_t& offset,
                             std::size_t end,
                             DraftVersion draft,
                             SubscribeMessage& message) {
    if (!decode_moqint_impl(bytes, offset, draft, message.filter_type)) {
        return false;
    }
    if (message.filter_type < 0x01 || message.filter_type > 0x04) {
        return false;
    }
    if (message.filter_type == 0x03 || message.filter_type == 0x04) {
        std::uint64_t group_id = 0;
        std::uint64_t object_id = 0;
        if (!decode_moqint_impl(bytes, offset, draft, group_id) ||
            !decode_moqint_impl(bytes, offset, draft, object_id)) {
            return false;
        }
        message.start_group_id = static_cast<std::size_t>(group_id);
        message.start_object_id = static_cast<std::size_t>(object_id);
        if (message.filter_type == 0x04) {
            std::uint64_t end_group_id = 0;
            if (!decode_moqint_impl(bytes, offset, draft, end_group_id)) {
                return false;
            }
            message.end_group_id = static_cast<std::size_t>(end_group_id);
        }
    } else {
        message.start_group_id = 0;
        message.start_object_id = 0;
        message.end_group_id = 0;
    }
    return offset == end;
}

bool decode_subscribe_message(std::span<const std::uint8_t> bytes, DraftVersion draft, SubscribeMessage& message) {
    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    if (!parse_uint16_length_message(bytes, draft, kSubscribeType, payload_offset, payload_length)) {
        return false;
    }

    std::size_t offset = payload_offset;
    const std::size_t payload_end = payload_offset + payload_length;
    std::uint64_t track_name_length = 0;
    if (!decode_moqint_impl(bytes, offset, draft, message.request_id)) {
        return false;
    }
    if (draft == DraftVersion::kDraft17) {
        std::uint64_t required_request_id_delta = 0;
        if (!decode_moqint_impl(bytes, offset, draft, required_request_id_delta) ||
            required_request_id_delta * 2 > message.request_id) {
            return false;
        }
    }
    if (!decode_track_namespace(bytes.subspan(0, payload_end), offset, draft, message.track_namespace) ||
        !decode_moqint_impl(bytes, offset, draft, track_name_length) || offset + track_name_length > payload_end) {
        return false;
    }

    message.track_name.assign(reinterpret_cast<const char*>(bytes.data() + offset), static_cast<std::size_t>(track_name_length));
    offset += static_cast<std::size_t>(track_name_length);

    if (draft == DraftVersion::kDraft14) {
        if (offset + 3 > payload_end) {
            return false;
        }
        message.subscriber_priority = bytes[offset++];
        message.group_order = bytes[offset++];
        message.forward = bytes[offset++];
        if (message.group_order == 0 || message.group_order > 2 || message.forward > 1) {
            return false;
        }

        if (!decode_moqint_impl(bytes, offset, draft, message.filter_type)) {
            return false;
        }
        if (message.filter_type < 0x01 || message.filter_type > 0x04) {
            return false;
        }
        if (message.filter_type == 0x03 || message.filter_type == 0x04) {
            std::uint64_t group_id = 0;
            std::uint64_t object_id = 0;
            if (!decode_moqint_impl(bytes, offset, draft, group_id) ||
                !decode_moqint_impl(bytes, offset, draft, object_id)) {
                return false;
            }
            message.start_group_id = static_cast<std::size_t>(group_id);
            message.start_object_id = static_cast<std::size_t>(object_id);
            if (message.filter_type == 0x04) {
                std::uint64_t end_group_id = 0;
                if (!decode_moqint_impl(bytes, offset, draft, end_group_id)) {
                    return false;
                }
                message.end_group_id = static_cast<std::size_t>(end_group_id);
            }
        } else {
            message.start_group_id = 0;
            message.start_object_id = 0;
            message.end_group_id = 0;
        }

        std::uint64_t parameter_count = 0;
        return decode_moqint_impl(bytes, offset, draft, parameter_count) && parameter_count == 0 && offset == payload_end;
    }

    // Draft-16: fields moved to delta-encoded KVP parameters.
    // Defaults per spec: subscriber_priority=128, group_order=0, forward=1, no filter.
    message.subscriber_priority = 128;
    message.group_order = 0;
    message.forward = 1;
    message.filter_type = 0;
    message.start_group_id = 0;
    message.start_object_id = 0;
    message.end_group_id = 0;

    std::uint64_t parameter_count = 0;
    if (!decode_moqint_impl(bytes, offset, draft, parameter_count)) {
        return false;
    }

    std::uint64_t previous_parameter_type = 0;
    for (std::uint64_t i = 0; i < parameter_count; ++i) {
        std::uint64_t parameter_type = 0;
        if (!decode_parameter_type(bytes, offset, draft, previous_parameter_type, true, parameter_type)) {
            return false;
        }

        if ((parameter_type & 0x1ULL) == 0) {
            // Even type: varint value.
            std::uint64_t value = 0;
            if (!decode_moqint_impl(bytes, offset, draft, value)) {
                return false;
            }
            switch (parameter_type) {
                case 0x02:  // DELIVERY_TIMEOUT (draft-18: OBJECT_DELIVERY_TIMEOUT) — spec §9.2.2.2: value 0 is illegal on the wire.
                    if (value == 0) { return false; }
                    message.delivery_timeout_ms = std::max(message.delivery_timeout_ms, value);
                    break;
                case 0x06:  // draft-18 SUBGROUP_DELIVERY_TIMEOUT; unassigned (ignored) in earlier drafts.
                    if (draft == DraftVersion::kDraft18) {
                        message.delivery_timeout_ms = std::max(message.delivery_timeout_ms, value);
                    }
                    break;
                case 0x10:  // FORWARD
                    if (value > 1) { return false; }
                    message.forward = static_cast<std::uint8_t>(value);
                    break;
                case 0x20:  // SUBSCRIBER_PRIORITY
                    if (value > 255) { return false; }
                    message.subscriber_priority = static_cast<std::uint8_t>(value);
                    break;
                case 0x22:  // GROUP_ORDER — only Ascending (0x1) and Descending (0x2) are legal on the wire.
                    if (value != 0x1 && value != 0x2) { return false; }
                    message.group_order = static_cast<std::uint8_t>(value);
                    break;
                case 0x32:  // NEW_GROUP_REQUEST — ignored by publishers that don't support dynamic groups.
                    break;
                default:
                    // Spec §9.2 requires an unknown Message Parameter to close the
                    // session with PROTOCOL_VIOLATION. We surface the rejection to
                    // the caller; session close semantics are the caller's domain.
                    return false;
            }
            continue;
        }

        // Odd type: length-prefixed bytes.
        std::uint64_t parameter_length = 0;
        if (!decode_moqint_impl(bytes, offset, draft, parameter_length) || offset + parameter_length > payload_end) {
            return false;
        }
        switch (parameter_type) {
            case 0x03:  // AUTHORIZATION_TOKEN — defined in SUBSCRIBE, opaque to this publisher.
                break;
            case 0x21: {  // SUBSCRIPTION_FILTER
                std::size_t filter_offset = offset;
                const std::size_t filter_end = offset + static_cast<std::size_t>(parameter_length);
                if (!decode_subscribe_filter(bytes, filter_offset, filter_end, draft, message)) {
                    return false;
                }
                break;
            }
            default:
                // Spec §9.2 requires an unknown Message Parameter to close the
                // session with PROTOCOL_VIOLATION. We surface the rejection to
                // the caller; session close semantics are the caller's domain.
                return false;
        }
        offset += static_cast<std::size_t>(parameter_length);
    }

    return offset == payload_end;
}

bool decode_subscribe_tracks_message(std::span<const std::uint8_t> bytes,
                                     DraftVersion draft,
                                     SubscribeTracksMessage& message) {
    if (draft != DraftVersion::kDraft18) {
        return false;
    }

    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    if (!parse_uint16_length_message(bytes, draft, kSubscribeTracksType, payload_offset, payload_length)) {
        return false;
    }

    std::size_t offset = payload_offset;
    const std::size_t payload_end = payload_offset + payload_length;
    if (!decode_moqint_impl(bytes, offset, draft, message.request_id) ||
        !decode_track_namespace(bytes.subspan(0, payload_end), offset, draft, message.track_namespace_prefix) ||
        message.track_namespace_prefix.size() > 32) {
        return false;
    }

    message.forward = 1;
    std::uint64_t parameter_count = 0;
    if (!decode_moqint_impl(bytes, offset, draft, parameter_count)) {
        return false;
    }

    std::uint64_t previous_parameter_type = 0;
    for (std::uint64_t index = 0; index < parameter_count; ++index) {
        std::uint64_t parameter_type = 0;
        if (!decode_parameter_type(bytes, offset, draft, previous_parameter_type, true, parameter_type)) {
            return false;
        }
        if ((parameter_type & 0x1ULL) == 0) {
            std::uint64_t value = 0;
            if (!decode_moqint_impl(bytes, offset, draft, value)) {
                return false;
            }
            if (parameter_type == 0x10) {
                if (value > 1) {
                    return false;
                }
                message.forward = static_cast<std::uint8_t>(value);
            }
            continue;
        }

        std::uint64_t parameter_length = 0;
        if (!decode_moqint_impl(bytes, offset, draft, parameter_length) || offset + parameter_length > payload_end) {
            return false;
        }
        offset += static_cast<std::size_t>(parameter_length);
    }

    return offset == payload_end;
}

bool decode_subscribe_update_message(std::span<const std::uint8_t> bytes, SubscribeUpdateMessage& message) {
    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    if (!parse_uint16_length_message(bytes, DraftVersion::kDraft16, kSubscribeUpdateType, payload_offset, payload_length)) {
        return false;
    }

    std::size_t offset = payload_offset;
    const std::size_t payload_end = payload_offset + payload_length;
    std::uint64_t start_group_id = 0;
    std::uint64_t start_object_id = 0;
    std::uint64_t end_group_plus_one = 0;
    std::uint64_t parameter_count = 0;
    if (!decode_varint_impl(bytes, offset, message.request_id) ||
        !decode_varint_impl(bytes, offset, message.subscription_request_id) ||
        !decode_varint_impl(bytes, offset, start_group_id) ||
        !decode_varint_impl(bytes, offset, start_object_id) ||
        !decode_varint_impl(bytes, offset, end_group_plus_one) ||
        offset + 2 > payload_end) {
        return false;
    }

    message.start_group_id = static_cast<std::size_t>(start_group_id);
    message.start_object_id = static_cast<std::size_t>(start_object_id);
    message.end_group_plus_one = static_cast<std::size_t>(end_group_plus_one);
    message.subscriber_priority = bytes[offset++];
    message.forward = bytes[offset++];
    if (message.forward > 1) {
        return false;
    }

    return decode_varint_impl(bytes, offset, parameter_count) && parameter_count == 0 && offset == payload_end;
}

std::vector<std::uint8_t> encode_subscribe_ok_message(DraftVersion draft,
                                                      std::uint64_t request_id,
                                                      std::uint64_t track_alias,
                                                      std::size_t largest_group_id,
                                                      std::size_t largest_object_id,
                                                      bool content_exists) {
    std::vector<std::uint8_t> payload;
    if (!uses_moq_vi64(draft)) {
        append_moqint(payload, draft, request_id);
    }
    if (draft == DraftVersion::kDraft14) {
        append_moqint(payload, draft, track_alias);
        append_moqint(payload, draft, 0);
        payload.push_back(kGroupOrderAscending);
        payload.push_back(content_exists ? kContentExistsTrue : 0);
        if (content_exists) {
            append_location(payload, draft, largest_group_id, largest_object_id);
        }
        append_moqint(payload, draft, 0);
    } else {
        append_moqint(payload, draft, track_alias);
        std::vector<std::uint8_t> parameters;
        std::uint64_t previous_parameter_type = 0;
        std::uint64_t parameter_count = 0;
        if (content_exists) {
            std::vector<std::uint8_t> largest_object;
            append_location(largest_object, draft, largest_group_id, largest_object_id);
            append_parameter_delta(parameters, draft, previous_parameter_type, 0x09, largest_object);
            ++parameter_count;
        }
        append_moqint(payload, draft, parameter_count);
        payload.insert(payload.end(), parameters.begin(), parameters.end());
    }

    std::vector<std::uint8_t> message_bytes;
    append_moqint(message_bytes, draft, kSubscribeOkType);
    append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

std::vector<std::uint8_t> encode_subscribe_error_message(std::uint64_t request_id,
                                                         std::uint64_t error_code,
                                                         std::string_view reason) {
    std::vector<std::uint8_t> payload;
    append_varint(payload, request_id);
    append_varint(payload, error_code);
    append_string(payload, DraftVersion::kDraft16, reason);

    std::vector<std::uint8_t> message_bytes;
    append_varint(message_bytes, kSubscribeErrorType);
    append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

std::vector<std::uint8_t> encode_request_error_message(DraftVersion draft,
                                                       std::uint64_t request_id,
                                                       std::uint64_t error_code,
                                                       std::uint64_t retry_interval,
                                                       std::string_view reason) {
    if (draft == DraftVersion::kDraft14) {
        return encode_subscribe_error_message(request_id, error_code, reason);
    }

    std::vector<std::uint8_t> payload;
    if (!uses_moq_vi64(draft)) {
        append_moqint(payload, draft, request_id);
    }
    append_moqint(payload, draft, error_code);
    append_moqint(payload, draft, retry_interval);
    append_string(payload, draft, reason);
    if (!uses_moq_vi64(draft)) {
        append_moqint(payload, draft, 0);
    }

    std::vector<std::uint8_t> message_bytes;
    append_moqint(message_bytes, draft, kRequestErrorType);
    append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

std::vector<std::uint8_t> encode_track_message(const TrackMessage& message) {
    std::vector<std::uint8_t> payload;
    append_moqint(payload, message.draft, message.request_id);
    if (message.draft == DraftVersion::kDraft17) {
        append_moqint(payload, message.draft, 0);  // Required Request ID Delta: no dependency.
    }
    append_track_namespace(payload, message.draft, message.track_namespace);
    append_string(payload, message.draft, message.track_name);
    append_moqint(payload, message.draft, message.track_alias);

    if (message.draft == DraftVersion::kDraft14) {
        payload.push_back(kGroupOrderAscending);
        payload.push_back(message.content_exists ? kContentExistsTrue : 0);
        if (message.content_exists) {
            append_location(payload, message.draft, message.largest_group_id, message.largest_object_id);
        }
        payload.push_back(kForwardPreference);
    }

    if (message.authorization_token.has_value()) {
        append_moqint(payload, message.draft, 1);
        append_parameter(payload, message.draft, kParamAuthorizationToken, *message.authorization_token);
    } else {
        append_moqint(payload, message.draft, 0);
    }
    if (message.draft == DraftVersion::kDraft16) {
        // No Track Extensions are needed for the current draft-16 publish path.
    }

    std::vector<std::uint8_t> message_bytes;
    append_moqint(message_bytes, message.draft, kPublishType);
    if (message.draft == DraftVersion::kDraft14) {
        append_moqint(message_bytes, message.draft, payload.size());
    } else {
        append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    }
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

std::vector<std::uint8_t> encode_publish_done_message(DraftVersion draft,
                                                      std::uint64_t request_id,
                                                      std::uint64_t stream_count,
                                                      std::uint64_t status_code,
                                                      std::string_view reason) {
    std::vector<std::uint8_t> payload;
    if (!uses_moq_vi64(draft)) {
        append_moqint(payload, draft, request_id);
    }
    append_moqint(payload, draft, status_code);
    append_moqint(payload, draft, stream_count);
    append_string(payload, draft, reason);

    std::vector<std::uint8_t> message_bytes;
    append_moqint(message_bytes, draft, kPublishDoneType);
    append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

std::vector<std::uint8_t> encode_publish_namespace_done_message(const NamespaceMessage& message) {
    std::vector<std::uint8_t> payload;
    if (message.draft == DraftVersion::kDraft14) {
        append_track_namespace(payload, message.draft, message.track_namespace);
    } else if (!uses_moq_vi64(message.draft)) {
        append_moqint(payload, message.draft, message.request_id);
    } else {
        append_track_namespace(payload, message.draft, message.track_namespace);
    }

    std::vector<std::uint8_t> message_bytes;
    append_moqint(message_bytes,
                  message.draft,
                  uses_moq_vi64(message.draft) ? kNamespaceDoneType : kPublishNamespaceDoneType);
    append_uint16(message_bytes, static_cast<std::uint16_t>(payload.size()));
    message_bytes.insert(message_bytes.end(), payload.begin(), payload.end());
    return message_bytes;
}

std::vector<std::uint8_t> encode_subgroup_header(DraftVersion draft,
                                                 std::uint64_t track_alias,
                                                 std::uint64_t group_id,
                                                 std::uint64_t subgroup_id,
                                                 bool end_of_group) {
    // Current callers always serve subgroup_id = 0 and the publisher default
    // priority, which matches SubgroupIDMode=0 + DefaultPriority=1. That shape
    // is on the wire what most interop partners (mojito, the Akamai test
    // relay, etc.) expect. Packager work that assigns non-zero subgroup IDs
    // or per-subgroup priorities can switch to SubgroupIDExplicit here.
    static_cast<void>(subgroup_id);
    const std::uint64_t base_type =
        draft == DraftVersion::kDraft14 ? kObjectDatagramTypeDraft14 : kSubgroupHeaderType;
    const std::uint64_t stream_type = base_type | (end_of_group ? kSubgroupHeaderEndOfGroupBit : 0);
    std::vector<std::uint8_t> bytes;
    append_moqint(bytes, draft, stream_type);
    append_moqint(bytes, draft, track_alias);
    append_moqint(bytes, draft, group_id);
    return bytes;
}

std::vector<std::uint8_t> encode_subgroup_object(DraftVersion draft,
                                                 std::optional<std::uint64_t> previous_object_id,
                                                 std::uint64_t object_id,
                                                 std::span<const std::uint8_t> payload) {
    // Spec §10.4.2: the first Object on a Subgroup stream carries its
    // absolute Object ID; subsequent Objects encode an Object ID Delta
    // such that next.object_id = previous.object_id + delta + 1.
    const std::uint64_t object_id_delta =
        previous_object_id.has_value() ? (object_id - *previous_object_id - 1) : object_id;
    std::vector<std::uint8_t> bytes;
    append_moqint(bytes, draft, object_id_delta);
    append_moqint(bytes, draft, payload.size());
    if (uses_moq_vi64(draft) && payload.empty()) {
        // draft-18 only carries Object Status after a zero payload length.
        append_moqint(bytes, draft, 0);
    }
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

bool decode_publish_namespace_ok(std::span<const std::uint8_t> bytes, PublishNamespaceOk& message) {
    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    if (!parse_uint16_length_message(bytes, DraftVersion::kDraft16, kPublishNamespaceOkType, payload_offset, payload_length)) {
        return false;
    }
    std::size_t offset = payload_offset;
    return decode_varint_impl(bytes, offset, message.request_id) && offset == payload_offset + payload_length;
}

bool decode_publish_namespace_error(std::span<const std::uint8_t> bytes, PublishNamespaceError& message) {
    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    if (!parse_uint16_length_message(bytes, DraftVersion::kDraft16, kPublishNamespaceErrorType, payload_offset, payload_length)) {
        return false;
    }
    std::size_t offset = payload_offset;
    return decode_varint_impl(bytes, offset, message.request_id) && decode_varint_impl(bytes, offset, message.error_code) &&
           decode_reason_phrase(bytes.subspan(0, payload_offset + payload_length), offset, DraftVersion::kDraft16, message.reason) &&
           offset == payload_offset + payload_length;
}

bool decode_publish_ok(std::span<const std::uint8_t> bytes, DraftVersion draft, PublishOk& message) {
    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    const bool request_ok_alias = draft == DraftVersion::kDraft18;
    const bool parsed = request_ok_alias
                            ? parse_uint16_length_message(bytes, draft, kRequestOkType, payload_offset, payload_length)
                            : parse_publish_family_message(bytes, draft, kPublishOkType, payload_offset, payload_length);
    if (!parsed) {
        return false;
    }
    if (payload_offset + payload_length > bytes.size()) {
        return false;
    }
    std::size_t offset = payload_offset;
    const std::size_t payload_end = payload_offset + payload_length;
    message.request_id = 0;
    if (!request_ok_alias && !decode_moqint_impl(bytes, offset, draft, message.request_id)) {
        return false;
    }

    if (draft == DraftVersion::kDraft14) {
        std::uint64_t parameter_count = 0;
        if (offset + 3 > payload_end) {
            return false;
        }
        message.forward = bytes[offset++];
        message.subscriber_priority = bytes[offset++];
        message.group_order = bytes[offset++];
        if (message.group_order == 0 || message.group_order > 2 || message.forward > 1) {
            return false;
        }
        if (!decode_moqint_impl(bytes, offset, draft, message.filter_type)) {
            return false;
        }

        if (message.filter_type == 0x03 || message.filter_type == 0x04) {
            std::uint64_t start_group_id = 0;
            std::uint64_t start_object_id = 0;
            if (!decode_moqint_impl(bytes, offset, draft, start_group_id) ||
                !decode_moqint_impl(bytes, offset, draft, start_object_id)) {
                return false;
            }
            if (message.filter_type == 0x04) {
                std::uint64_t end_group_id = 0;
                if (!decode_moqint_impl(bytes, offset, draft, end_group_id)) {
                    return false;
                }
            }
        }

        if (!decode_moqint_impl(bytes, offset, draft, parameter_count)) {
            return false;
        }
        for (std::uint64_t index = 0; index < parameter_count; ++index) {
            std::uint64_t parameter_type = 0;
            std::uint64_t parameter_length = 0;
            if (!decode_moqint_impl(bytes, offset, draft, parameter_type) ||
                !decode_moqint_impl(bytes, offset, draft, parameter_length) ||
                offset + parameter_length > payload_end) {
                return false;
            }
            offset += static_cast<std::size_t>(parameter_length);
        }
        return offset == payload_end;
    }

    std::uint64_t parameter_count = 0;
    if (!decode_moqint_impl(bytes, offset, draft, parameter_count)) {
        return false;
    }
    message.forward = 1;
    message.subscriber_priority = 128;
    message.group_order = 0;
    message.filter_type = 0;

    std::uint64_t previous_parameter_type = 0;
    for (std::uint64_t index = 0; index < parameter_count; ++index) {
        std::uint64_t parameter_type = 0;
        if (!decode_parameter_type(bytes, offset, draft, previous_parameter_type, true, parameter_type)) {
            return false;
        }
        if ((parameter_type & 0x1ULL) == 0) {
            std::uint64_t value = 0;
            if (!decode_moqint_impl(bytes, offset, draft, value)) {
                return false;
            }
            switch (parameter_type) {
                case 0x02:  // DELIVERY_TIMEOUT (draft-18: OBJECT_DELIVERY_TIMEOUT)
                    if (value == 0) { return false; }
                    message.delivery_timeout_ms = std::max(message.delivery_timeout_ms, value);
                    break;
                case 0x06:  // draft-18 SUBGROUP_DELIVERY_TIMEOUT; unassigned (ignored) in earlier drafts.
                    if (draft == DraftVersion::kDraft18) {
                        message.delivery_timeout_ms = std::max(message.delivery_timeout_ms, value);
                    }
                    break;
                case 0x08:  // EXPIRES
                    break;
                case 0x10:  // FORWARD
                    if (value > 1) { return false; }
                    message.forward = static_cast<std::uint8_t>(value);
                    break;
                case 0x20:  // SUBSCRIBER_PRIORITY
                    if (value > 255) { return false; }
                    message.subscriber_priority = static_cast<std::uint8_t>(value);
                    break;
                case 0x22:  // GROUP_ORDER
                    if (value != 0x1 && value != 0x2) { return false; }
                    message.group_order = static_cast<std::uint8_t>(value);
                    break;
                case 0x32:  // NEW_GROUP_REQUEST
                    break;
                default:
                    return false;
            }
            continue;
        }

        std::uint64_t parameter_length = 0;
        if (!decode_moqint_impl(bytes, offset, draft, parameter_length) || offset + parameter_length > payload_end) {
            return false;
        }
        switch (parameter_type) {
            case 0x21: {  // SUBSCRIPTION_FILTER
                SubscribeMessage filter_message;
                std::size_t filter_offset = offset;
                const std::size_t filter_end = offset + static_cast<std::size_t>(parameter_length);
                if (!decode_subscribe_filter(bytes, filter_offset, filter_end, draft, filter_message)) {
                    return false;
                }
                message.filter_type = filter_message.filter_type;
                break;
            }
            default:
                return false;
        }
        offset += static_cast<std::size_t>(parameter_length);
    }
    return offset == payload_end;
}

bool decode_publish_error(std::span<const std::uint8_t> bytes, DraftVersion draft, PublishError& message) {
    std::size_t payload_offset = 0;
    std::size_t payload_length = 0;
    if (!parse_publish_family_message(bytes, draft, kPublishErrorType, payload_offset, payload_length)) {
        return false;
    }
    if (payload_offset + payload_length > bytes.size()) {
        return false;
    }
    std::size_t offset = payload_offset;
    const std::size_t payload_end = payload_offset + payload_length;
    return decode_varint_impl(bytes, offset, message.request_id) && decode_varint_impl(bytes, offset, message.error_code) &&
           decode_reason_phrase(bytes.subspan(0, payload_end), offset, draft, message.reason) && offset == payload_end;
}

}  // namespace openmoq::publisher::transport
