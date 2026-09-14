#include "openmoq/publisher/live_srt_config.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    bool ok = true;

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
