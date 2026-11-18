#pragma once
#include "openmoq/publisher/loc_codec_profile.h"

#include <stdexcept>
#include <string>

namespace openmoq::publisher::loc_detail {
[[noreturn]] inline void fail(const char *s) { throw std::runtime_error(std::string("LOC sample reader: ") + s); }
struct Reader {
    std::span<const std::uint8_t> b;
    std::size_t pos = 0;
    std::uint64_t get(std::size_t n) {
        if (n > b.size() - pos)
            fail("truncated box");
        std::uint64_t v = 0;
        while (n--)
            v = (v << 8) | b[pos++];
        return v;
    }
    void skip(std::size_t n) {
        if (n > b.size() - pos)
            fail("truncated box");
        pos += n;
    }
    void end() {
        if (pos != b.size())
            fail("unexpected box fields");
    }
};
// The track's codec_private must be exactly one box; returns a reader over its
// payload and the box type.
inline Reader config_box_reader(const TrackDescription &t, std::string &type) {
    const auto boxes = parse_mp4_boxes(t.codec_private);
    if (boxes.size() != 1 || boxes[0].span.size != t.codec_private.size())
        fail("invalid codec configuration box");
    type = boxes[0].type;
    return {slice_bytes(t.codec_private, boxes[0].payload)};
}
} // namespace openmoq::publisher::loc_detail
