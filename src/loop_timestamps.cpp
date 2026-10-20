#include "loop_timestamps.h"

#include "openmoq/publisher/mp4_box.h"

#include <cstddef>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace openmoq::publisher {
namespace {

constexpr std::uint64_t kMicrosecondsPerSecond = 1'000'000;

std::uint32_t read_be32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint64_t read_be64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return (static_cast<std::uint64_t>(read_be32(bytes, offset)) << 32U) | read_be32(bytes, offset + 4);
}

void write_be32_at(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 24U);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16U);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8U);
    bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

void write_be64_at(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    write_be32_at(bytes, offset, static_cast<std::uint32_t>(value >> 32U));
    write_be32_at(bytes, offset + 4, static_cast<std::uint32_t>(value));
}

bool has_compact_header(const Mp4Box& box) {
    return box.payload.offset - box.span.offset == 8;
}

std::uint32_t full_box_flags(const std::vector<std::uint8_t>& bytes, const Mp4Box& box) {
    return read_be32(bytes, box.payload.offset) & 0x00FFFFFFU;
}

// A version 0 tfdt that must become version 1: its box offset and new value.
struct TfdtWidening {
    std::size_t box_offset = 0;
    std::uint64_t value = 0;
};

// Number of widenings that land before `target`, i.e. shift it by 4 bytes each.
std::uint64_t widenings_before(const std::vector<TfdtWidening>& widenings, std::uint64_t target) {
    std::uint64_t count = 0;
    for (const auto& widening : widenings) {
        if (widening.box_offset < target) {
            ++count;
        }
    }
    return count;
}

// Rewrites the moof-relative offsets and box sizes that widening the given
// tfdt boxes would invalidate. Writes at the original byte positions; the
// caller inserts the extra bytes afterwards.
bool adjust_for_widenings(std::vector<std::uint8_t>& out,
                          const Mp4Box& moof,
                          const std::vector<TfdtWidening>& widenings) {
    if (!has_compact_header(moof)) {
        return false;
    }
    std::size_t traf_count = 0;
    for (const auto& child : moof.children) {
        traf_count += child.type == "traf" ? 1U : 0U;
    }
    for (const auto& traf : moof.children) {
        if (traf.type != "traf") {
            continue;
        }
        if (!has_compact_header(traf)) {
            return false;
        }
        const Mp4Box* tfhd = find_child_box(traf, "tfhd");
        if (tfhd == nullptr || tfhd->payload.size < 4) {
            return false;
        }
        // Offsets must be relative to the moof: an explicit base data offset
        // is absolute, and without default-base-is-moof only the first traf's
        // base is the moof.
        const std::uint32_t tfhd_flags = full_box_flags(out, *tfhd);
        if ((tfhd_flags & 0x000001U) != 0 || ((tfhd_flags & 0x020000U) == 0 && traf_count > 1)) {
            return false;
        }
        std::uint64_t traf_widenings = 0;
        for (const auto& widening : widenings) {
            if (widening.box_offset >= traf.span.offset && widening.box_offset < traf.span.offset + traf.span.size) {
                ++traf_widenings;
            }
        }
        write_be32_at(out, traf.span.offset,
                      static_cast<std::uint32_t>(traf.span.size + 4 * traf_widenings));

        for (const auto& child : traf.children) {
            if (child.type == "trun") {
                if (child.payload.size < 8) {
                    return false;
                }
                if ((full_box_flags(out, child) & 0x000001U) == 0) {
                    continue;
                }
                if (child.payload.size < 12) {
                    return false;
                }
                const std::size_t field = child.payload.offset + 8;
                const auto data_offset = static_cast<std::int32_t>(read_be32(out, field));
                const std::int64_t target = static_cast<std::int64_t>(moof.span.offset) + data_offset;
                if (target < 0) {
                    return false;
                }
                const std::int64_t shifted =
                    data_offset + 4 * static_cast<std::int64_t>(widenings_before(widenings, static_cast<std::uint64_t>(target)));
                if (shifted > std::numeric_limits<std::int32_t>::max()) {
                    return false;
                }
                write_be32_at(out, field, static_cast<std::uint32_t>(shifted));
            } else if (child.type == "saio") {
                if (child.payload.size < 8) {
                    return false;
                }
                const std::uint8_t version = out[child.payload.offset];
                std::size_t cursor = child.payload.offset + 4;
                if ((full_box_flags(out, child) & 0x000001U) != 0) {
                    cursor += 8;
                }
                const std::size_t end = child.payload.offset + child.payload.size;
                if (cursor + 4 > end) {
                    return false;
                }
                const std::uint32_t entry_count = read_be32(out, cursor);
                cursor += 4;
                const std::size_t entry_size = version == 0 ? 4 : 8;
                if ((end - cursor) / entry_size < entry_count) {
                    return false;
                }
                for (std::uint32_t i = 0; i < entry_count; ++i, cursor += entry_size) {
                    const std::uint64_t offset = version == 0 ? read_be32(out, cursor) : read_be64(out, cursor);
                    const std::uint64_t shifted = offset + 4 * widenings_before(widenings, moof.span.offset + offset);
                    if (version == 0) {
                        if (shifted > std::numeric_limits<std::uint32_t>::max()) {
                            return false;
                        }
                        write_be32_at(out, cursor, static_cast<std::uint32_t>(shifted));
                    } else {
                        write_be64_at(out, cursor, shifted);
                    }
                }
            }
        }
    }
    write_be32_at(out, moof.span.offset, static_cast<std::uint32_t>(moof.span.size + 4 * widenings.size()));
    return true;
}

std::optional<std::uint64_t> read_locmaf_varint(const std::vector<std::uint8_t>& bytes, std::size_t& cursor) {
    if (cursor >= bytes.size()) {
        return std::nullopt;
    }
    // Inverse of locmaf_encoder.cpp vi(): the leading one bits of the first
    // byte count the bytes that follow it.
    const std::uint8_t first = bytes[cursor];
    std::size_t length = 1;
    while (length < 9 && (first & (0x80U >> (length - 1))) != 0) {
        ++length;
    }
    if (bytes.size() - cursor < length) {
        return std::nullopt;
    }
    std::uint64_t value = length < 9 ? (first & (0xFFU >> length)) : 0;
    for (std::size_t i = 1; i < length; ++i) {
        value = (value << 8U) | bytes[cursor + i];
    }
    cursor += length;
    return value;
}

// Same encoding as locmaf_encoder.cpp vi().
std::vector<std::uint8_t> locmaf_varint(std::uint64_t value) {
    unsigned length = 1;
    while (length < 9 && value >= (std::uint64_t{1} << (7 * length))) {
        ++length;
    }
    std::vector<std::uint8_t> out(length);
    for (unsigned i = length; i > 0; --i) {
        out[i - 1] = static_cast<std::uint8_t>(value);
        value >>= 8U;
    }
    if (length > 1) {
        out[0] |= static_cast<std::uint8_t>(0xFFU << (9 - length));
    }
    return out;
}

}  // namespace

std::uint64_t loop_offset_ticks(std::uint64_t cycle_duration_us, std::uint64_t cycle, std::uint32_t timescale) {
    const std::uint64_t elapsed_us = cycle_duration_us * cycle;
    const std::uint64_t whole_seconds = elapsed_us / kMicrosecondsPerSecond;
    const std::uint64_t remainder_us = elapsed_us % kMicrosecondsPerSecond;
    return whole_seconds * timescale +
           (remainder_us * timescale + kMicrosecondsPerSecond / 2) / kMicrosecondsPerSecond;
}

bool rebase_cmaf_timestamps(std::vector<std::uint8_t>& payload, std::uint64_t delta_ticks) {
    std::vector<Mp4Box> top;
    try {
        top = parse_mp4_boxes(payload);
    } catch (const std::exception&) {
        return false;
    }

    std::vector<std::uint8_t> out = payload;
    std::vector<TfdtWidening> all_widenings;
    bool found = false;
    for (const auto& moof : top) {
        if (moof.type != "moof") {
            continue;
        }
        std::vector<TfdtWidening> widenings;
        for (const auto& traf : moof.children) {
            if (traf.type != "traf") {
                continue;
            }
            const Mp4Box* tfdt = find_child_box(traf, "tfdt");
            if (tfdt == nullptr || tfdt->payload.size < 4) {
                return false;
            }
            const std::uint8_t version = out[tfdt->payload.offset];
            const std::size_t field = tfdt->payload.offset + 4;
            if (version == 1 && tfdt->payload.size >= 12) {
                write_be64_at(out, field, read_be64(out, field) + delta_ticks);
            } else if (version == 0 && tfdt->payload.size >= 8) {
                const std::uint64_t value = read_be32(out, field) + delta_ticks;
                if (value <= std::numeric_limits<std::uint32_t>::max()) {
                    write_be32_at(out, field, static_cast<std::uint32_t>(value));
                } else if (has_compact_header(*tfdt) && tfdt->payload.size == 8) {
                    widenings.push_back({.box_offset = tfdt->span.offset, .value = value});
                } else {
                    return false;
                }
            } else {
                return false;
            }
            found = true;
        }
        if (!widenings.empty()) {
            if (!adjust_for_widenings(out, moof, widenings)) {
                return false;
            }
            all_widenings.insert(all_widenings.end(), widenings.begin(), widenings.end());
        }
    }
    if (!found) {
        return false;
    }

    // Insert from the back so earlier box offsets stay valid.
    for (auto it = all_widenings.rbegin(); it != all_widenings.rend(); ++it) {
        const std::size_t box = it->box_offset;
        write_be32_at(out, box, 20);
        out[box + 8] = 1;
        std::vector<std::uint8_t> value(8);
        write_be64_at(value, 0, it->value);
        out.erase(out.begin() + static_cast<std::ptrdiff_t>(box + 12), out.begin() + static_cast<std::ptrdiff_t>(box + 16));
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(box + 12), value.begin(), value.end());
    }
    payload = std::move(out);
    return true;
}

bool rebase_locmaf_timestamps(std::vector<std::uint8_t>& payload, std::uint64_t delta_ticks) {
    std::size_t cursor = 0;
    while (true) {
        const auto type = read_locmaf_varint(payload, cursor);
        if (!type.has_value()) {
            return false;
        }
        if (*type == 1) {
            // A CMAF box carried ahead of the object (styp, prft, emsg, ...).
            const auto length = read_locmaf_varint(payload, cursor);
            if (!length.has_value() || payload.size() - cursor < *length) {
                return false;
            }
            cursor += static_cast<std::size_t>(*length);
            continue;
        }
        if (*type == 3) {
            return true;
        }
        if (*type == 4) {
            std::vector<std::uint8_t> chunk(payload.begin() + static_cast<std::ptrdiff_t>(cursor), payload.end());
            if (!rebase_cmaf_timestamps(chunk, delta_ticks)) {
                return false;
            }
            payload.resize(cursor);
            payload.insert(payload.end(), chunk.begin(), chunk.end());
            return true;
        }
        if (*type != 2) {
            return false;
        }
        break;
    }

    const std::size_t length_offset = cursor;
    const auto properties_length = read_locmaf_varint(payload, cursor);
    if (!properties_length.has_value() || payload.size() - cursor < *properties_length) {
        return false;
    }
    const std::size_t properties_begin = cursor;
    const std::size_t properties_end = cursor + static_cast<std::size_t>(*properties_length);
    while (cursor < properties_end) {
        const auto id = read_locmaf_varint(payload, cursor);
        if (!id.has_value()) {
            return false;
        }
        if ((*id & 1U) != 0) {
            const auto length = read_locmaf_varint(payload, cursor);
            if (!length.has_value() || properties_end - cursor < *length) {
                return false;
            }
            cursor += static_cast<std::size_t>(*length);
            continue;
        }
        const std::size_t value_offset = cursor;
        const auto value = read_locmaf_varint(payload, cursor);
        if (!value.has_value() || cursor > properties_end) {
            return false;
        }
        if (*id != 10) {
            continue;
        }
        const std::vector<std::uint8_t> encoded = locmaf_varint(*value + delta_ticks);
        const std::size_t new_properties_length = (properties_end - properties_begin) - (cursor - value_offset) + encoded.size();
        const std::vector<std::uint8_t> encoded_length = locmaf_varint(new_properties_length);

        std::vector<std::uint8_t> out(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(length_offset));
        out.insert(out.end(), encoded_length.begin(), encoded_length.end());
        out.insert(out.end(), payload.begin() + static_cast<std::ptrdiff_t>(properties_begin),
                   payload.begin() + static_cast<std::ptrdiff_t>(value_offset));
        out.insert(out.end(), encoded.begin(), encoded.end());
        out.insert(out.end(), payload.begin() + static_cast<std::ptrdiff_t>(cursor), payload.end());
        payload = std::move(out);
        return true;
    }
    return false;
}

void rebase_loc_timestamps(std::vector<ObjectProperty>& properties, std::uint64_t delta_ticks) {
    for (auto& property : properties) {
        if (property.id == 16) {
            if (auto* value = std::get_if<std::uint64_t>(&property.value)) {
                *value += delta_ticks;
            }
        }
    }
}

}  // namespace openmoq::publisher
