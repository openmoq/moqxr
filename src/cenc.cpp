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

// 16 raw bytes to xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx, the form CMSF
// section 4.1.1.2 requires.
std::string uuid_string(std::span<const std::uint8_t> id) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        out.push_back(kHex[(id[i] >> 4U) & 0x0FU]);
        out.push_back(kHex[id[i] & 0x0FU]);
    }
    return out;
}

std::string base64_encode(std::span<const std::uint8_t> bytes) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t b0 = bytes[i];
        const std::uint32_t b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const std::uint32_t b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const std::uint32_t word = (b0 << 16U) | (b1 << 8U) | b2;
        out.push_back(kAlphabet[(word >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(word >> 12U) & 0x3FU]);
        out.push_back(i + 1 < bytes.size() ? kAlphabet[(word >> 6U) & 0x3FU] : '=');
        out.push_back(i + 2 < bytes.size() ? kAlphabet[word & 0x3FU] : '=');
    }
    return out;
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

    // Written as limit >= 8 && cursor <= limit - 8 rather than
    // cursor + 8 <= limit: the latter can wrap if cursor is ever within 8 of
    // SIZE_MAX, which would falsely look "in bounds". Unreachable from
    // today's callers (real parsed offsets plus small constants), but this
    // function exists to defend against untrusted offsets, so it must not
    // depend on callers staying that way.
    while (limit >= 8 && cursor <= limit - 8) {
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

std::optional<CencTrackProtection> parse_track_protection(std::span<const std::uint8_t> bytes,
                                                          const Mp4Box& sample_entry) {
    if (sample_entry.type != "encv" && sample_entry.type != "enca") {
        return std::nullopt;
    }
    // Children begin past the sample entry header: 8 + 70 for visual,
    // 8 + 28 for audio. Same offsets build_track_codec_init_data uses.
    const std::size_t child_offset = sample_entry.type == "encv" ? 8 + 70 : 8 + 28;

    const auto sinf = find_child_box_span(bytes, sample_entry.span.offset,
                                          sample_entry.span.size, child_offset, "sinf");
    if (!sinf.has_value()) {
        return std::nullopt;
    }

    CencTrackProtection out;

    const auto frma = find_child_box_span(bytes, sinf->offset, sinf->size, 8, "frma");
    if (!frma.has_value() || frma->size < 12) {
        return std::nullopt;
    }
    out.original_format.assign(reinterpret_cast<const char*>(bytes.data() + frma->offset + 8), 4);

    // schm is a FullBox: 8-byte header + 4-byte version/flags, then
    // scheme_type(4) and scheme_version(4).
    const auto schm = find_child_box_span(bytes, sinf->offset, sinf->size, 8, "schm");
    if (!schm.has_value() || schm->size < 20) {
        return std::nullopt;
    }
    out.scheme.assign(reinterpret_cast<const char*>(bytes.data() + schm->offset + 12), 4);

    const auto schi = find_child_box_span(bytes, sinf->offset, sinf->size, 8, "schi");
    if (!schi.has_value()) {
        return std::nullopt;
    }
    // tenc is a FullBox: header(8) + version/flags(4) + reserved(1) +
    // reserved(1) + default_isProtected(1) + default_Per_Sample_IV_Size(1)
    // + default_KID(16) = 32 bytes minimum.
    const auto tenc = find_child_box_span(bytes, schi->offset, schi->size, 8, "tenc");
    if (!tenc.has_value() || tenc->size < 32) {
        return std::nullopt;
    }
    out.is_protected = bytes[tenc->offset + 14] != 0;
    out.per_sample_iv_size = bytes[tenc->offset + 15];
    out.default_kid = uuid_string(bytes.subspan(tenc->offset + 16, 16));

    return out;
}

std::vector<CencSystem> parse_pssh_boxes(std::span<const std::uint8_t> bytes,
                                         const std::vector<Mp4Box>& pssh_boxes) {
    std::vector<CencSystem> out;
    for (const auto& box : pssh_boxes) {
        if (box.type != "pssh") {
            continue;
        }
        // header(8) + version/flags(4) + SystemID(16) = 28 bytes minimum.
        // box.span is caller-supplied and not trusted: guard the sum against
        // size_t wrap before comparing, the same idiom find_child_box_span
        // uses for container_offset + container_size above.
        const std::size_t box_end = box.span.offset + box.span.size;
        if (box.span.size < 28 || box_end < box.span.offset || box_end > bytes.size()) {
            continue;
        }
        CencSystem system;
        system.system_id = uuid_string(bytes.subspan(box.span.offset + 12, 16));
        system.pssh_base64 = base64_encode(bytes.subspan(box.span.offset, box.span.size));
        out.push_back(std::move(system));
    }
    return out;
}

}  // namespace openmoq::publisher
