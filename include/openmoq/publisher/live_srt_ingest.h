#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "openmoq/publisher/cmaf_segmenter.h"
#include "openmoq/publisher/mp4_box.h"
#include "openmoq/publisher/transport/publisher_transport.h"

namespace openmoq::publisher {

struct LiveSrtCallerRuntimeConfig {
    std::string id;
    std::string endpoint;
    bool fragment_on_keyframe = true;
    bool empty_moov = true;
    bool default_base_moof = true;
    bool separate_moof_per_track = true;
    std::uint32_t target_fragment_duration_ms = 1000;
    std::uint32_t latency_ms = 120;
    bool auto_detect_program = true;
    std::uint32_t program_number = 0;
    bool has_program_number = false;
    std::uint32_t video_pid = 0;
    bool has_video_pid = false;
    std::uint32_t audio_pid = 0;
    bool has_audio_pid = false;
};

struct LiveSrtBootstrap {
    std::vector<TrackDescription> tracks;
    std::vector<std::uint8_t> init_segment;
};

class LiveSrtIngestManager {
public:
    using FragmentSink = std::function<void(MediaFragment&&)>;

    LiveSrtIngestManager(std::vector<LiveSrtCallerRuntimeConfig> callers,
                         FragmentSink sink,
                         std::atomic<bool>& stop_requested);

    transport::TransportStatus start();
    void join();

    const LiveSrtBootstrap& bootstrap() const;

    static std::vector<std::uint8_t> build_synthetic_init_segment(const std::vector<TrackDescription>& tracks);

private:
    struct Impl;
    std::vector<LiveSrtCallerRuntimeConfig> callers_;
    FragmentSink sink_;
    std::atomic<bool>& stop_requested_;
    LiveSrtBootstrap bootstrap_;
    std::vector<std::thread> worker_threads_;
};

}  // namespace openmoq::publisher
