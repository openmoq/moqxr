#include "openmoq/publisher/live_srt_config.h"

#include "json_reader.h"

#include <cctype>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace openmoq::publisher {

namespace {

using namespace openmoq::publisher::internal;

}  // namespace

LiveSrtConfig parse_live_srt_config_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("failed to open SRT config file: " + path.string());
    }

    std::string json_text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    JsonParser parser(std::move(json_text));
    const JsonValue root = parser.parse();
    const JsonObject& root_obj = expect_object(root, "root");

    const auto callers_it = root_obj.find("srt_callers");
    if (callers_it == root_obj.end()) {
        throw std::runtime_error("missing JSON key: srt_callers");
    }
    const JsonArray& callers = expect_array(callers_it->second, "srt_callers");

    LiveSrtConfig config;
    config.srt_callers.reserve(callers.size());

    for (const JsonValue& caller_value : callers) {
        const JsonObject& caller_obj = expect_object(caller_value, "srt_callers[]");
        SrtCallerIngestConfig caller;
        caller.id = expect_string(caller_obj, "id");

        const auto srt_it = caller_obj.find("srt");
        if (srt_it == caller_obj.end()) {
            throw std::runtime_error("missing JSON key: srt");
        }
        const JsonObject& srt_obj = expect_object(srt_it->second, "srt");
        caller.srt.mode = expect_string(srt_obj, "mode");
        if (caller.srt.mode != "caller") {
            throw std::runtime_error("unsupported srt.mode '" + caller.srt.mode + "': only 'caller' is supported");
        }
        caller.srt.host = expect_string(srt_obj, "host");
        const auto port = read_optional_u32(srt_obj, "port");
        if (!port.has_value() || *port == 0 || *port > 65535) {
            throw std::runtime_error("srt.port must be between 1 and 65535");
        }
        caller.srt.port = static_cast<std::uint16_t>(*port);
        caller.srt.latency_ms = read_u32_or_default(srt_obj, "latency_ms", 120);

        const auto mpegts_it = caller_obj.find("mpegts");
        if (mpegts_it != caller_obj.end()) {
            const JsonObject& mpegts_obj = expect_object(mpegts_it->second, "mpegts");
            caller.mpegts.auto_detect_program = read_bool_or_default(mpegts_obj, "auto_detect_program", true);
            caller.mpegts.program_number = read_optional_u32(mpegts_obj, "program_number");
            caller.mpegts.video_pid = read_optional_u32(mpegts_obj, "video_pid");
            caller.mpegts.audio_pid = read_optional_u32(mpegts_obj, "audio_pid");
        }

        const auto cmaf_it = caller_obj.find("cmaf");
        if (cmaf_it != caller_obj.end()) {
            const JsonObject& cmaf_obj = expect_object(cmaf_it->second, "cmaf");
            caller.cmaf.fragment_on_keyframe = read_bool_or_default(cmaf_obj, "fragment_on_keyframe", true);
            caller.cmaf.empty_moov = read_bool_or_default(cmaf_obj, "empty_moov", true);
            caller.cmaf.default_base_moof = read_bool_or_default(cmaf_obj, "default_base_moof", true);
            caller.cmaf.separate_moof_per_track = read_bool_or_default(cmaf_obj, "separate_moof_per_track", true);
            caller.cmaf.target_fragment_duration_ms =
                read_u32_or_default(cmaf_obj, "target_fragment_duration_ms", 1000);
        }

        config.srt_callers.push_back(std::move(caller));
    }

    return config;
}

}  // namespace openmoq::publisher
