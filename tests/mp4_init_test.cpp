#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/mp4_box.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

#include "ffmpeg_avc_init_fixture.inc"

using namespace openmoq::publisher;

static_assert(sizeof(kFfmpegAvcInitData) == kFfmpegAvcInitDataSize);

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

bool expect_contains(const std::string& text, const std::string& expected, const std::string& message) {
    return expect(text.find(expected) != std::string::npos, message);
}

std::uint32_t read_be32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) | static_cast<std::uint32_t>(bytes[offset + 3]);
}

void add_to_box_size(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t amount) {
    const std::uint32_t value = read_be32(bytes, offset) + amount;
    bytes[offset] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    bytes[offset + 3] = static_cast<std::uint8_t>(value & 0xFFU);
}

std::vector<std::uint8_t> make_nonstandard_visual_preamble(std::span<const std::uint8_t> init_segment) {
    std::vector<std::uint8_t> bytes(init_segment.begin(), init_segment.end());
    const auto boxes = parse_mp4_boxes(bytes);
    const Mp4Box* moov = find_first_box(boxes, "moov");
    const Mp4Box* trak = moov == nullptr ? nullptr : find_child_box(*moov, "trak");
    const Mp4Box* mdia = trak == nullptr ? nullptr : find_child_box(*trak, "mdia");
    const Mp4Box* minf = mdia == nullptr ? nullptr : find_child_box(*mdia, "minf");
    const Mp4Box* stbl = minf == nullptr ? nullptr : find_child_box(*minf, "stbl");
    const Mp4Box* stsd = stbl == nullptr ? nullptr : find_child_box(*stbl, "stsd");
    if (stsd == nullptr) {
        throw std::runtime_error("fixture has no stsd");
    }

    const std::size_t sample_entry_offset = stsd->payload.offset + 8;
    const std::size_t child_offset = sample_entry_offset + kVisualSampleEntryChildOffset;
    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(child_offset), 8, 0);

    constexpr std::uint32_t kInsertedPreambleBytes = 8;
    for (const Mp4Box* box : {moov, trak, mdia, minf, stbl, stsd}) {
        add_to_box_size(bytes, box->span.offset, kInsertedPreambleBytes);
    }
    add_to_box_size(bytes, sample_entry_offset, kInsertedPreambleBytes);
    return bytes;
}

}  // namespace

void append_be32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (const int shift : {24, 16, 8, 0}) bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
}

std::vector<std::uint8_t> make_box(const char* type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> box;
    append_be32(box, static_cast<std::uint32_t>(8 + payload.size()));
    box.insert(box.end(), type, type + 4);
    box.insert(box.end(), payload.begin(), payload.end());
    return box;
}

// A moof with one traf for track_id: tfhd optionally carrying a default
// sample duration, and a trun with the given per-sample durations, or
// sample_count samples using the defaults when durations is empty.
std::vector<std::uint8_t> make_moof(std::uint32_t track_id,
                                    std::optional<std::uint32_t> tfhd_default_duration,
                                    const std::vector<std::uint32_t>& durations,
                                    std::uint32_t sample_count = 1) {
    std::vector<std::uint8_t> tfhd;
    append_be32(tfhd, tfhd_default_duration ? 0x020008U : 0x020000U);
    append_be32(tfhd, track_id);
    if (tfhd_default_duration) append_be32(tfhd, *tfhd_default_duration);
    std::vector<std::uint8_t> trun;
    append_be32(trun, durations.empty() ? 0x000000U : 0x000100U);
    append_be32(trun, durations.empty() ? sample_count : static_cast<std::uint32_t>(durations.size()));
    for (const auto duration : durations) append_be32(trun, duration);
    std::vector<std::uint8_t> traf = make_box("tfhd", tfhd);
    const auto trun_box = make_box("trun", trun);
    traf.insert(traf.end(), trun_box.begin(), trun_box.end());
    return make_box("moof", make_box("traf", traf));
}

// MSF 5.2.20 framerate for a fragmented track: its empty_moov init segment has
// no sample durations, so the rate comes from its fragments (ISO/IEC 14496-12
// 8.8): trun durations, else the tfhd default, else the trex default.
bool test_fragment_frame_rates(std::span<const std::uint8_t> init_segment) {
    bool ok = true;
    const auto init_tracks = extract_tracks(parse_mp4_boxes(init_segment), init_segment);
    if (!expect(init_tracks.size() == 1, "expected one track for the fragment frame rate tests")) return false;
    const std::uint32_t track_id = init_tracks.front().track_id;
    ok &= expect(init_tracks.front().frame_rate == 0.0 && needs_fragment_frame_rate(init_tracks.front()),
                 "an empty_moov init segment alone carries no frame rate");

    const auto rate_after = [&](const std::vector<std::uint8_t>& moof, std::uint32_t timescale) {
        auto tracks = init_tracks;
        tracks.front().timescale = timescale;
        apply_fragment_frame_rates(tracks, parse_mp4_boxes(moof), moof);
        return tracks.front().frame_rate;
    };
    ok &= expect(rate_after(make_moof(track_id, 512, {}), 15360) == 30.0,
                 "a tfhd default sample duration of 512 at timescale 15360 is 30 fps");
    const double ntsc = rate_after(make_moof(track_id, std::nullopt, {1001, 1001, 1001}), 30000);
    ok &= expect(ntsc > 29.9700 && ntsc < 29.9701, "trun sample durations of 1001 at timescale 30000 are 29.97 fps");
    ok &= expect(rate_after(make_moof(track_id, 9999, {500, 524}), 15360) == 30.0,
                 "trun sample durations take precedence over the tfhd default");
    ok &= expect(rate_after(make_moof(track_id + 1, 512, {}), 15360) == 0.0,
                 "a fragment of another track leaves the frame rate unknown");

    auto trex_tracks = init_tracks;
    trex_tracks.front().timescale = 12000;
    trex_tracks.front().fragment_defaults = TrackFragmentDefaults{.sample_duration = 400};
    apply_fragment_frame_rates(trex_tracks, {}, {});
    ok &= expect(trex_tracks.front().frame_rate == 30.0, "a trex default sample duration alone gives the frame rate");

    auto catalog_tracks = init_tracks;
    const auto moof = make_moof(track_id, 512, {});
    apply_fragment_frame_rates(catalog_tracks, parse_mp4_boxes(moof), moof);
    const auto catalog = build_live_catalog(catalog_tracks, init_segment, true);
    const std::string catalog_text(catalog.catalog_payload.begin(), catalog.catalog_payload.end());
    ok &= expect_contains(catalog_text, "\"framerate\":24",
                          "the live catalog advertises the fragment frame rate (512 at timescale 12288)");
    return ok;
}

int main() {
    bool ok = true;
    const std::span<const std::uint8_t> init_segment(kFfmpegAvcInitData, kFfmpegAvcInitDataSize);

    const auto boxes = parse_mp4_boxes(init_segment);
    const auto tracks = extract_tracks(boxes, init_segment);

    ok &= expect(tracks.size() == 1, "expected one track in the independently muxed init segment");
    if (tracks.size() != 1) {
        return 1;
    }

    const auto& track = tracks.front();
    ok &= expect(track.codec == "avc1.64000C", "expected the complete RFC 6381 AVC codec string, not a bare fourcc");
    ok &= expect(track.sample_entry_type == "avc1", "expected the avc1 sample-entry type");
    ok &= expect(track.width == 320 && track.height == 180, "expected 320x180 geometry from the VisualSampleEntry");
    ok &= expect(track.timescale == 12288, "expected the independently muxed track timescale");

    const auto live_catalog = build_live_catalog(tracks, init_segment, true);
    const std::string catalog_text(live_catalog.catalog_payload.begin(), live_catalog.catalog_payload.end());
    ok &= expect_contains(catalog_text, "\"codec\":\"avc1.64000C\"",
                          "expected the live catalog to advertise the complete codec string");
    ok &= expect_contains(catalog_text, "\"initDataList\"", "expected the live catalog to include initialization data");
    ok &= expect(live_catalog.track_initializations.size() == 1 &&
                     !live_catalog.track_initializations.front().codec_payload.empty() &&
                     !live_catalog.track_initializations.front().init_segment.empty(),
                 "expected one track initialization with codec and CMAF init payloads");

    const auto malformed_init = make_nonstandard_visual_preamble(init_segment);
    const auto malformed_tracks = extract_tracks(parse_mp4_boxes(malformed_init), malformed_init);
    ok &= expect(malformed_tracks.size() == 1 && malformed_tracks.front().codec == "avc1",
                 "expected the shifted decoder configuration to expose the "
                 "bare-fourcc failure mode");

    bool malformed_rejected = false;
    std::string malformed_error;
    try {
        (void)build_live_catalog(malformed_tracks, malformed_init, true);
    } catch (const std::runtime_error& error) {
        malformed_rejected = true;
        malformed_error = error.what();
    }
    ok &= expect(malformed_rejected, "expected live catalog generation to reject "
                                     "a nonstandard visual preamble");
    ok &= expect_contains(malformed_error, "codec initData box",
                          "expected rejection to identify the missing codec initialization box");

    ok &= test_fragment_frame_rates(init_segment);

    return ok ? 0 : 1;
}
