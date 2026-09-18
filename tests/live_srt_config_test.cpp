#include "openmoq/publisher/live_srt_config.h"
#include "openmoq/publisher/cmaf_segmenter.h"
#include "openmoq/publisher/encoded_sample.h"

#include <algorithm>

#include <array>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace openmoq::publisher::live_srt_internal {
std::vector<TrackDescription> codec_tracks_for_testing();
std::vector<MediaFragment> demux_fragments_for_testing(std::span<const std::uint8_t> bytes, bool discover = false);
}

namespace {

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

std::array<std::uint8_t, 188> pes_packet(std::uint16_t pid, std::uint64_t pts,
                                         std::optional<std::uint64_t> dts, bool valid = true,
                                         std::vector<std::uint8_t> payload = {0, 0, 0, 1, 0x65, 0x88}) {
    std::vector<std::uint8_t> pes{0, 0, 1, 0xe0, 0, 0, 0x80,
        static_cast<std::uint8_t>(dts ? 0xc0 : 0x80), static_cast<std::uint8_t>(dts ? 10 : 5)};
    const auto append_clock = [&](std::uint64_t value, std::uint8_t prefix) {
        pes.push_back(static_cast<std::uint8_t>((prefix << 4) | (((value >> 30) & 7) << 1) | 1));
        pes.push_back(static_cast<std::uint8_t>(value >> 22));
        pes.push_back(static_cast<std::uint8_t>((((value >> 15) & 127) << 1) | 1));
        pes.push_back(static_cast<std::uint8_t>(value >> 7));
        pes.push_back(static_cast<std::uint8_t>(((value & 127) << 1) | 1));
    };
    append_clock(pts, dts ? 3 : 2);
    if (dts) append_clock(*dts, 1);
    if (!valid) pes[9] &= 0xfe;
    pes.insert(pes.end(), payload.begin(), payload.end());
    std::array<std::uint8_t, 188> packet{};
    packet.fill(0xff);
    packet[0] = 0x47;
    packet[1] = static_cast<std::uint8_t>(0x40 | (pid >> 8));
    packet[2] = static_cast<std::uint8_t>(pid);
    packet[3] = 0x30;
    packet[4] = static_cast<std::uint8_t>(183 - pes.size());
    packet[5] = 0;
    std::copy(pes.begin(), pes.end(), packet.end() - static_cast<std::ptrdiff_t>(pes.size()));
    return packet;
}

bool adts_configuration_is_per_sample() {
    std::vector<std::uint8_t> ts;
    const auto append = [&](auto packet) { ts.insert(ts.end(), packet.begin(), packet.end()); };
    const auto psi = [&](std::uint16_t pid, std::vector<std::uint8_t> payload) {
        std::array<std::uint8_t, 188> packet{};
        packet.fill(0xff);
        packet[0] = 0x47;
        packet[1] = static_cast<std::uint8_t>(0x40 | (pid >> 8));
        packet[2] = static_cast<std::uint8_t>(pid);
        packet[3] = 0x10;
        std::copy(payload.begin(), payload.end(), packet.begin() + 4);
        append(packet);
    };
    psi(0, {0, 0, 0xb0, 13, 0, 1, 0xc1, 0, 0, 0, 1, 0xe0, 100, 0, 0, 0, 0});
    psi(100, {0, 2, 0xb0, 18, 0, 1, 0xc1, 0, 0, 0xe1, 1, 0xf0, 0,
              0x0f, 0xe1, 1, 0xf0, 0, 0, 0, 0, 0});
    const std::vector<std::uint8_t> aac48{0xff, 0xf1, 0x4c, 0x80, 1, 0x1f, 0xfc, 0xab};
    const std::vector<std::uint8_t> aac44{0xff, 0xf1, 0x50, 0x80, 1, 0x1f, 0xfc, 0xab};
    auto multiple = aac48;
    multiple.insert(multiple.end(), aac48.begin(), aac48.end());
    append(pes_packet(257, 90000, std::nullopt, true, multiple));
    append(pes_packet(257, 93840, std::nullopt, true, aac44));
    auto unsupported = aac44;
    unsupported[6] |= 1; // two raw_data_blocks cannot be called one AAC access unit
    append(pes_packet(257, 95930, std::nullopt, true, unsupported));
    append(pes_packet(257, 100110, std::nullopt, true, aac44));
    const auto fragments = openmoq::publisher::live_srt_internal::demux_fragments_for_testing(ts, true);
    if (!expect(fragments.size() == 4, "ADTS PES fixture sample count")) return false;
    const auto& a = fragments[0].source_timing;
    const auto& b = fragments[1].source_timing;
    const auto& changed = fragments[2].source_timing;
    bool ok = expect(a && b && changed, "all valid ADTS frames carry timing");
    if (!a || !b || !changed) return false;
    ok &= expect(a->codec_config == std::vector<std::uint8_t>({0x11, 0x90}) && b->codec_config == a->codec_config,
                 "every split ADTS frame carries its actual ASC");
    ok &= expect(b->decode_time == 91920 && b->duration == 1920, "48kHz AAC source tick interpolation");
    ok &= expect(changed->codec_config == std::vector<std::uint8_t>({0x12, 0x10}) && changed->duration == 2090,
                 "changed rate must update ASC and source duration for explicit LOC refusal");
    ok &= expect(!fragments[3].source_timing, "multiple raw_data_blocks must not be described as one LOC sample");
    return ok;
}

bool source_timing_survives_synthetic_cmaf() {
    const auto tracks = openmoq::publisher::live_srt_internal::codec_tracks_for_testing();
    const auto avcc = openmoq::publisher::loc_codec_config(tracks[0]);
    const auto asc = openmoq::publisher::loc_codec_config(tracks[1]);
    bool configs_ok = expect(avcc.size() == tracks[0].codec_private.size() - 8 && avcc[0] == 1,
                             "SRT AVC configuration must parse as LOC decoder extradata");
    configs_ok &= expect(asc == std::vector<std::uint8_t>({0x11, 0x90}),
                         "expanded SRT ES descriptors must unwrap to AAC-LC 48kHz stereo ASC");
    std::vector<std::uint8_t> ts;
    const auto append = [&](auto packet) { ts.insert(ts.end(), packet.begin(), packet.end()); };
    append(pes_packet(256, 99000, 90000));
    append(pes_packet(256, 93000, 93000));  // B-frame presentation order differs.
    append(pes_packet(256, 96000, 96000, false));
    append(pes_packet(256, 102000, 99000)); // flush malformed-clock sample
    append(pes_packet(257, 94500, std::nullopt));
    append(pes_packet(257, 96420, std::nullopt));
    const auto fragments = openmoq::publisher::live_srt_internal::demux_fragments_for_testing(ts);
    bool ok = configs_ok;
    ok &= expect(fragments.size() == 4, "PES fixture must emit four samples");
    if (fragments.size() != 4) return false;
    const auto& a = fragments[0].source_timing;
    const auto& b = fragments[1].source_timing;
    const auto& audio = fragments[3].source_timing;
    ok &= expect(a && a->decode_time == 90000 && a->presentation_time == 99000 && a->timescale == 90000,
                 "PES video PTS and DTS must remain distinct and unre-based");
    ok &= expect(b && b->decode_time == 93000 && b->presentation_time == 93000 && b->sequence == 1,
                 "reordered PTS must preserve decode order and source sequence");
    ok &= expect(a && a->duration == 0, "video PES duration must remain unknown");
    ok &= expect(!fragments[2].source_timing, "invalid PES clock markers must not produce LOC timing");
    ok &= expect(audio && audio->decode_time == 94500 && audio->presentation_time == 94500 && audio->sequence == 0,
                 "PTS-only audio must preserve source AV offset and independent sequence");
    // The additive source clock must not alter the legacy CMAF payload path.
    auto invalid_first = ts;
    const auto first = pes_packet(256, 99000, 90000, false);
    std::copy(first.begin(), first.end(), invalid_first.begin());
    const auto baseline = openmoq::publisher::live_srt_internal::demux_fragments_for_testing(invalid_first);
    ok &= expect(baseline[0].payload.owned_bytes == fragments[0].payload.owned_bytes,
                 "source timing metadata must leave existing CMAF bytes unchanged");
    return ok;
}

}  // namespace

int main() {
    bool ok = source_timing_survives_synthetic_cmaf();
    ok &= adts_configuration_is_per_sample();

    const std::filesystem::path config_path =
        std::filesystem::temp_directory_path() / "openmoq-live-srt-config-test.json";

    {
        std::ofstream out(config_path);
        out << R"({
  "srt_callers": [
    {
      "id": "bbb",
      "srt": {
        "mode": "caller",
        "host": "10.0.0.11",
        "port": 9000,
        "latency_ms": 120
      },
      "mpegts": {
        "auto_detect_program": true,
        "program_number": 1,
        "video_pid": null,
        "audio_pid": null
      },
      "cmaf": {
        "fragment_on_keyframe": true,
        "empty_moov": true,
        "default_base_moof": true,
        "separate_moof_per_track": true,
        "target_fragment_duration_ms": 1000
      }
    }
  ]
})";
    }

    const openmoq::publisher::LiveSrtConfig config =
        openmoq::publisher::parse_live_srt_config_file(config_path);
    ok &= expect(config.srt_callers.size() == 1, "expected one SRT caller");
    ok &= expect(config.srt_callers.front().id == "bbb", "expected SRT id");
    ok &= expect(config.srt_callers.front().srt.host == "10.0.0.11", "expected SRT host");
    ok &= expect(config.srt_callers.front().srt.port == 9000, "expected SRT port");
    ok &= expect(config.srt_callers.front().mpegts.program_number.has_value(), "expected program_number");
    ok &= expect(config.srt_callers.front().mpegts.program_number.value_or(0) == 1,
                 "expected program_number=1");

    for (const std::string mode : {"listener", "caller", "rendezvous"}) {
        {
            std::ofstream out(config_path);
            out << "{\"srt_callers\":[{\"id\":\"cam\",\"srt\":{\"mode\":\""
                << mode << "\",\"host\":\"0.0.0.0\",\"port\":9000}}]}";
        }
        try {
            const auto parsed = openmoq::publisher::parse_live_srt_config_file(config_path);
            ok &= expect(mode != "rendezvous", "unsupported mode must be rejected");
            ok &= expect(parsed.srt_callers.front().srt.mode == mode, "SRT mode must be preserved");
        } catch (const std::exception& e) {
            ok &= expect(mode == "rendezvous", mode + " should be accepted: " + e.what());
        }
    }

    std::error_code ec;
    std::filesystem::remove(config_path, ec);

    return ok ? 0 : 1;
}
