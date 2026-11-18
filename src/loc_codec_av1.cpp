#include "loc_codec_internal.h"

#include <algorithm>
#include <optional>

namespace openmoq::publisher {
namespace {
using loc_detail::Reader;
using loc_detail::fail;

enum ObuType : unsigned {
    kSequenceHeader = 1,
    kTemporalDelimiter = 2,
    kFrameHeader = 3,
    kTileGroup = 4,
    kMetadata = 5,
    kFrame = 6,
    kRedundantFrameHeader = 7,
    kTileList = 8,
    kPadding = 15,
};

struct Obu {
    unsigned type = 0;
    unsigned spatial_id = 0;
    std::span<const std::uint8_t> payload;
};

// Low-overhead OBUs (has_size_field = 1), as stored in MP4 samples and av1C.
std::vector<Obu> parse_obus(std::span<const std::uint8_t> data) {
    std::vector<Obu> obus;
    Reader r{data};
    while (r.pos < r.b.size()) {
        const auto header = r.get(1);
        if ((header & 0x80) || (header & 1) || !(header & 2))
            fail("invalid AV1 OBU header");
        Obu obu;
        obu.type = static_cast<unsigned>((header >> 3) & 15);
        if (header & 4) {
            const auto extension = r.get(1);
            obu.spatial_id = static_cast<unsigned>((extension >> 3) & 3);
        }
        std::uint64_t size = 0;
        for (unsigned i = 0;; ++i) {
            if (i == 8)
                fail("invalid AV1 OBU size");
            const auto byte = r.get(1);
            size |= static_cast<std::uint64_t>(byte & 0x7f) << (7 * i);
            if (!(byte & 0x80))
                break;
        }
        const auto start = r.pos;
        r.skip(static_cast<std::size_t>(size));
        obu.payload = r.b.subspan(start, static_cast<std::size_t>(size));
        obus.push_back(obu);
    }
    return obus;
}

struct Av1Config {
    // Sequence header OBU payload carried in configOBUs, when present.
    std::optional<std::span<const std::uint8_t>> sequence_header;
};

Av1Config validate_av1c(std::span<const std::uint8_t> record) {
    if (record.size() < 4 || record[0] != 0x81 || (record[3] & 0xe0))
        fail("invalid AV1 configuration");
    Av1Config config;
    for (const auto &obu : parse_obus(record.subspan(4))) {
        if (obu.type == kSequenceHeader && !config.sequence_header)
            config.sequence_header = obu.payload;
    }
    return config;
}

// reduced_still_picture_header follows seq_profile (3 bits) and still_picture (1 bit).
bool reduced_still_picture_header(std::span<const std::uint8_t> sequence_header) {
    if (sequence_header.empty())
        fail("truncated AV1 sequence header");
    return (sequence_header[0] >> 3) & 1;
}

class Av1LocProfile final : public LocCodecProfile {
public:
    bool matches(const TrackDescription &t) const override {
        return t.handler_type == "vide" && t.sample_entry_type == "av01";
    }
    bool matches_codec(std::string_view codec, bool video) const override {
        return video && codec.starts_with("av01.");
    }
    std::vector<std::uint8_t> extract_config(const TrackDescription &t) const override {
        std::string type;
        auto r = loc_detail::config_box_reader(t, type);
        if (type != "av1C")
            fail("invalid AV1 configuration");
        validate_av1c(r.b);
        return {r.b.begin(), r.b.end()};
    }
    LocSampleCheck check_sample(const TrackDescription &t, std::span<const std::uint8_t> data, bool) const override {
        const auto record = extract_config(t);
        const auto config = validate_av1c(record);
        // Without a sequence header in av1C, an in-band one decides the header layout;
        // with neither, a regular (non-still-picture) header is assumed.
        bool reduced = config.sequence_header ? reduced_still_picture_header(*config.sequence_header) : false;
        bool temporal_delimiter = false, frames = false, first_frame_header_seen = false, independent = false;
        unsigned shown = 0;
        for (const auto &obu : parse_obus(data)) {
            if (obu.spatial_id != 0)
                fail("layered AV1 samples unsupported");
            switch (obu.type) {
            case kSequenceHeader:
                if (config.sequence_header) {
                    // The av1C copy is not always byte-identical to the in-band one, so only
                    // the leading seq_profile/still_picture/reduced_still_picture_header bits
                    // are compared.
                    if (obu.payload.empty() || (obu.payload[0] & 0xf8) != ((*config.sequence_header)[0] & 0xf8))
                        fail("in-band AV1 sequence header differs from av1C");
                } else {
                    reduced = reduced_still_picture_header(obu.payload);
                }
                break;
            case kTemporalDelimiter:
                if (temporal_delimiter || frames)
                    fail("multiple AV1 temporal units in one sample");
                temporal_delimiter = true;
                break;
            case kFrameHeader:
            case kFrame: {
                frames = true;
                bool show_existing = false, key = reduced, show_frame = reduced;
                if (!reduced) {
                    if (obu.payload.empty())
                        fail("truncated AV1 frame header");
                    const auto first = obu.payload[0];
                    show_existing = first & 0x80;
                    if (!show_existing) {
                        key = ((first >> 5) & 3) == 0;
                        show_frame = (first >> 4) & 1;
                    }
                }
                if (show_existing || show_frame)
                    ++shown;
                if (!first_frame_header_seen) {
                    first_frame_header_seen = true;
                    independent = !show_existing && key;
                }
                break;
            }
            case kTileGroup:
            case kMetadata:
            case kRedundantFrameHeader:
            case kPadding:
                break;
            default:
                fail("unsupported AV1 OBU type");
            }
        }
        if (!frames)
            fail("AV1 sample has no frame");
        if (shown != 1)
            fail("AV1 sample must contain exactly one shown frame");
        LocSampleCheck check;
        check.independent = independent;
        return check;
    }
    bool accepts_live_track(std::string_view, std::span<const std::uint8_t> init_data, bool) const override {
        try {
            validate_av1c(init_data);
        } catch (const std::runtime_error &) {
            return false;
        }
        return true;
    }
};
} // namespace
const LocCodecProfile &av1_loc_profile() {
    static const Av1LocProfile profile;
    return profile;
}
} // namespace openmoq::publisher
