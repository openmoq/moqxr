#pragma once

#include "openmoq/publisher/moq_draft.h"

#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace cat4moq_test {

using openmoq::publisher::DraftVersion;

struct Reader {
    std::span<const std::uint8_t> bytes;
    DraftVersion draft;
    std::size_t offset = 0;

    std::uint8_t byte() {
        if (offset == bytes.size()) throw std::runtime_error("truncated test frame");
        return bytes[offset++];
    }
    std::uint64_t integer() {
        const auto first = byte();
        unsigned count = 0;
        std::uint64_t value = first;
        if (draft == DraftVersion::kDraft14 || draft == DraftVersion::kDraft16) {
            count = (1u << (first >> 6)) - 1;
            value &= 0x3f;
        } else {
            for (unsigned mask = 0x80; mask && (first & mask); mask >>= 1) ++count;
            value &= count == 8 ? 0 : (0x7f >> count);
        }
        while (count--) value = (value << 8) | byte();
        return value;
    }
    std::vector<std::uint8_t> blob() {
        const auto size = integer();
        if (size > bytes.size() - offset) throw std::runtime_error("truncated test field");
        std::vector<std::uint8_t> result(bytes.begin() + offset, bytes.begin() + offset + size);
        offset += size;
        return result;
    }
    std::string string() {
        const auto value = blob();
        return {value.begin(), value.end()};
    }
};

struct Message {
    std::uint64_t type = 0;
    std::uint64_t request_id = 0;
    std::vector<std::string> track_namespace;
    std::string track_name;
    std::uint64_t track_alias = 0;
    std::map<std::uint64_t, std::vector<std::uint8_t>> parameters;
    // Every AUTHORIZATION TOKEN (type 3) value in wire order; parameters holds the first.
    std::vector<std::vector<std::uint8_t>> authorization_tokens;
    std::map<std::uint64_t, std::uint64_t> numeric_parameters;
};

inline Message decode(std::span<const std::uint8_t> bytes, DraftVersion draft) {
    Reader reader{bytes, draft};
    Message message;
    message.type = reader.integer();
    std::uint64_t length = 0;
    if (message.type == 0x1d && draft == DraftVersion::kDraft14) {
        length = reader.integer();
    } else {
        const auto high = reader.byte();
        const auto low = reader.byte();
        length = (static_cast<std::uint64_t>(high) << 8) | low;
    }
    if (length != bytes.size() - reader.offset) throw std::runtime_error("incorrect frame length");
    const bool setup = message.type == 0x20 || message.type == 0x2f00;
    const bool modern = draft == DraftVersion::kDraft17 || draft == DraftVersion::kDraft18;
    if (setup && draft == DraftVersion::kDraft14) {
        if (reader.integer() != 1 || reader.integer() != 0xff00000e) throw std::runtime_error("incorrect setup version");
    }
    if (!setup) {
        if (message.type != 6 && message.type != 0x1d) throw std::runtime_error("unexpected message type");
        message.request_id = reader.integer();
        if (draft == DraftVersion::kDraft17 && reader.integer() != 0) throw std::runtime_error("unexpected request dependency");
        const auto components = reader.integer();
        if (components > 32) throw std::runtime_error("excess namespace components");
        for (std::size_t i = 0; i < components; ++i) message.track_namespace.push_back(reader.string());
        if (message.type == 0x1d) {
            message.track_name = reader.string();
            message.track_alias = reader.integer();
            if (draft == DraftVersion::kDraft14) {
                if (reader.byte() != 1) throw std::runtime_error("incorrect group order");
                const auto exists = reader.byte();
                if (exists > 1) throw std::runtime_error("invalid content existence");
                if (exists) { reader.integer(); reader.integer(); }
                if (reader.byte() != 1) throw std::runtime_error("incorrect forward state");
            }
        }
    }
    const bool uncounted = setup && modern;
    const auto count = uncounted ? bytes.size() : reader.integer();
    std::uint64_t previous = 0;
    for (std::size_t i = 0; i < count && (!uncounted || reader.offset < bytes.size()); ++i) {
        auto type = reader.integer();
        if (draft != DraftVersion::kDraft14) type += previous;  // draft-16+ Key-Value-Pair types are deltas
        previous = type;
        if (type == 3) {
            auto value = reader.blob();
            message.parameters.emplace(type, value);
            message.authorization_tokens.push_back(std::move(value));
        } else if ((type & 1) != 0) {
            if (!message.parameters.emplace(type, reader.blob()).second) throw std::runtime_error("duplicate test parameter");
        } else {
            if (!message.numeric_parameters.emplace(type, reader.integer()).second) throw std::runtime_error("duplicate test parameter");
        }
    }
    if (reader.offset != bytes.size()) throw std::runtime_error("trailing frame bytes");
    return message;
}

}  // namespace cat4moq_test
