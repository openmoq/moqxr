#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace openmoq::publisher {

struct SrtSocketConfig {
    std::string mode = "caller";
    std::string host;
    std::uint16_t port = 0;
    std::uint32_t latency_ms = 120;
};

struct MpegTsProgramConfig {
    bool auto_detect_program = true;
    std::optional<std::uint32_t> program_number;
    std::optional<std::uint32_t> video_pid;
    std::optional<std::uint32_t> audio_pid;
};

struct CmafFragmentPolicy {
    bool fragment_on_keyframe = true;
    bool empty_moov = true;
    bool default_base_moof = true;
    bool separate_moof_per_track = true;
    std::uint32_t target_fragment_duration_ms = 1000;
};

struct SrtCallerIngestConfig {
    std::string id;
    SrtSocketConfig srt;
    MpegTsProgramConfig mpegts;
    CmafFragmentPolicy cmaf;
};

struct LiveSrtConfig {
    std::vector<SrtCallerIngestConfig> srt_callers;
};

LiveSrtConfig parse_live_srt_config_file(const std::filesystem::path& path);

}  // namespace openmoq::publisher
