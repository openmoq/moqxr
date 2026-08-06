#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/mp4_box.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
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

    return ok ? 0 : 1;
}
