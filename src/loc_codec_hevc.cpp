#include "loc_codec_internal.h"

#include <algorithm>

namespace openmoq::publisher {
namespace {
using loc_detail::Reader;
using loc_detail::fail;

constexpr unsigned kVps = 32, kSps = 33, kPps = 34, kAud = 35;

// Validates an HEVCDecoderConfigurationRecord and returns every parameter set
// NAL unit it carries (spans into `record`).
std::vector<std::span<const std::uint8_t>> validate_hvcc(std::span<const std::uint8_t> record) {
    if (record.size() < 23 || record[0] != 1)
        fail("invalid HEVC configuration");
    if ((record[21] & 3) == 2)
        fail("unsupported HEVC NAL length size");
    Reader r{record};
    r.skip(22);
    const auto arrays = r.get(1);
    std::vector<std::span<const std::uint8_t>> parameter_sets;
    unsigned seen[3] = {0, 0, 0};
    for (std::uint64_t i = 0; i < arrays; ++i) {
        const auto type = static_cast<unsigned>(r.get(1) & 0x3f);
        const auto count = r.get(2);
        for (std::uint64_t j = 0; j < count; ++j) {
            const auto length = r.get(2);
            const auto start = r.pos;
            if (length < 2)
                fail("invalid HEVC parameter set");
            r.skip(static_cast<std::size_t>(length));
            const auto unit = r.b.subspan(start, static_cast<std::size_t>(length));
            if (((unit[0] >> 1) & 0x3f) != type)
                fail("HEVC parameter set type differs from its array");
            if (type >= kVps && type <= kPps) {
                ++seen[type - kVps];
                parameter_sets.push_back(unit);
            }
        }
    }
    r.end();
    if (!seen[0] || !seen[1] || !seen[2])
        fail("missing HEVC parameter sets");
    return parameter_sets;
}

class HevcLocProfile final : public LocCodecProfile {
public:
    bool matches(const TrackDescription &t) const override {
        return t.handler_type == "vide" && (t.sample_entry_type == "hvc1" || t.sample_entry_type == "hev1");
    }
    bool matches_codec(std::string_view codec, bool video) const override {
        return video && (codec.starts_with("hvc1.") || codec.starts_with("hev1."));
    }
    std::vector<std::uint8_t> extract_config(const TrackDescription &t) const override {
        std::string type;
        auto r = loc_detail::config_box_reader(t, type);
        if (type != "hvcC")
            fail("invalid HEVC configuration");
        validate_hvcc(r.b);
        return {r.b.begin(), r.b.end()};
    }
    LocSampleCheck check_sample(const TrackDescription &t, std::span<const std::uint8_t> data,
                                bool strip_matching_config) const override {
        const auto config = extract_config(t);
        const auto parameter_sets = validate_hvcc(config);
        const std::size_t width = (config[21] & 3) + 1;
        std::vector<std::uint8_t> normalized;
        Reader nal{data};
        bool picture_start = false, vcl = false, aud = false, independent = false;
        while (nal.pos < nal.b.size()) {
            const auto nal_start = nal.pos;
            const auto length = nal.get(width);
            if (length < 2 || length > nal.b.size() - nal.pos)
                fail("invalid HEVC sample NAL length");
            const auto unit = nal.b.subspan(nal.pos, static_cast<std::size_t>(length));
            if (unit[0] & 0x80)
                fail("invalid HEVC NAL header");
            const unsigned type = (unit[0] >> 1) & 0x3f;
            const unsigned layer = ((unit[0] & 1) << 5) | (unit[1] >> 3);
            if (layer != 0)
                fail("layered HEVC samples unsupported");
            if ((unit[1] & 7) == 0)
                fail("invalid HEVC NAL header");
            nal.skip(static_cast<std::size_t>(length));
            if (type >= kVps && type <= kPps) {
                if (!strip_matching_config)
                    fail("in-band HEVC configuration unsupported");
                const bool matches = std::any_of(parameter_sets.begin(), parameter_sets.end(), [&](auto known) {
                    return std::equal(unit.begin(), unit.end(), known.begin(), known.end());
                });
                if (!matches)
                    fail("in-band HEVC configuration differs from hvcC");
                continue;
            }
            if (type == kAud) {
                if (aud || vcl)
                    fail("multiple HEVC access units in one sample");
                aud = true;
            }
            if (type >= 22 && type <= 31)
                fail("reserved HEVC VCL NAL type");
            if (type <= 21) {
                if (length < 3)
                    fail("truncated HEVC slice header");
                if (unit[2] & 0x80) {
                    if (picture_start)
                        fail("multiple HEVC pictures in one sample");
                    picture_start = true;
                    independent = type >= 16;
                }
                vcl = true;
            }
            if (strip_matching_config) {
                const auto original = data.subspan(nal_start, nal.pos - nal_start);
                normalized.insert(normalized.end(), original.begin(), original.end());
            }
        }
        if (!vcl)
            fail("HEVC sample has no coded picture");
        if (!picture_start)
            fail("HEVC sample does not start a picture");
        LocSampleCheck check;
        check.independent = independent;
        if (strip_matching_config)
            check.normalized = std::move(normalized);
        return check;
    }
    bool accepts_live_track(std::string_view, std::span<const std::uint8_t> init_data, bool) const override {
        try {
            validate_hvcc(init_data);
        } catch (const std::runtime_error &) {
            return false;
        }
        return true;
    }
};
} // namespace
const LocCodecProfile &hevc_loc_profile() {
    static const HevcLocProfile profile;
    return profile;
}
} // namespace openmoq::publisher
