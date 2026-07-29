#include "openmoq/publisher/cenc.h"

#include <algorithm>
#include <cstring>

namespace openmoq::publisher {

namespace {

std::uint32_t read_be32_at(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

}  // namespace

std::optional<ByteSpan> find_child_box_span(std::span<const std::uint8_t> bytes,
                                            std::size_t container_offset,
                                            std::size_t container_size,
                                            std::size_t first_child_offset,
                                            std::string_view type) {
    if (type.size() != 4) {
        return std::nullopt;
    }

    // container_size is attacker-controlled. Guard the sum against size_t wrap
    // before clamping to the real buffer.
    std::size_t container_end = container_offset + container_size;
    if (container_end < container_offset) {
        container_end = bytes.size();
    }
    const std::size_t limit = std::min(container_end, bytes.size());

    std::size_t cursor = container_offset + first_child_offset;
    if (cursor < container_offset) {
        return std::nullopt;
    }

    while (cursor + 8 <= limit) {
        const std::uint32_t box_size = read_be32_at(bytes, cursor);
        // A length below the 8-byte header, or one running past the limit,
        // means the remaining bytes are not a valid box chain. Stop rather
        // than skip: skipping is what turns a walk back into a scan.
        if (box_size < 8) {
            return std::nullopt;
        }
        const std::size_t box_end = cursor + box_size;
        if (box_end < cursor || box_end > limit) {
            return std::nullopt;
        }

        if (std::memcmp(bytes.data() + cursor + 4, type.data(), 4) == 0) {
            return ByteSpan{.offset = cursor, .size = box_size};
        }

        cursor = box_end;
    }

    return std::nullopt;
}

}  // namespace openmoq::publisher
