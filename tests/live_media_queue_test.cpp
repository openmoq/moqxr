#include "live_media_queue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using openmoq::publisher::LiveMediaAdmission;
using openmoq::publisher::LiveMediaFragmentRole;
using openmoq::publisher::LiveMediaQueue;
using openmoq::publisher::MediaFragment;
using openmoq::publisher::transport::LiveSrtQueueAdapter;
using openmoq::publisher::transport::TransportStatus;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

MediaFragment make_fragment(std::string track_name,
                            std::size_t group_id,
                            std::size_t object_id,
                            std::uint64_t start_time_us,
                            std::uint64_t duration_us,
                            std::size_t payload_bytes,
                            bool keyframe = false) {
    MediaFragment fragment;
    fragment.group_id = group_id;
    fragment.object_id = object_id;
    fragment.track_name = std::move(track_name);
    fragment.start_time_us = start_time_us;
    fragment.duration_us = duration_us;
    fragment.is_video_keyframe = keyframe;
    if (fragment.track_name.starts_with("video")) {
        fragment.has_sap_type = true;
        fragment.sap_type = keyframe ? 2 : 0;
    }
    fragment.payload.owned_bytes.assign(payload_bytes,
                                        static_cast<std::uint8_t>(group_id + object_id + 1));
    return fragment;
}

std::vector<MediaFragment> drain(LiveMediaQueue& queue) {
    std::vector<MediaFragment> fragments;
    while (std::optional<MediaFragment> fragment = queue.try_pop()) {
        fragments.push_back(std::move(*fragment));
    }
    return fragments;
}

}  // namespace

int main() {
    bool ok = true;

    {
        LiveMediaQueue queue;
        MediaFragment exact_default =
            make_fragment("video", 0, 0, 0, 2'000'000, 0, true);
        exact_default.payload.span.size = 16U * 1024U * 1024U;
        ok &= expect(queue.push(std::move(exact_default)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected the default SRT queue to admit exactly 16 MiB and 2 seconds");
        ok &= expect(queue.queued_bytes() == 16U * 1024U * 1024U &&
                         queue.queued_media_duration() == std::chrono::seconds(2),
                     "expected the production default limits to be exact");
        ok &= expect(queue.push(make_fragment("video", 0, 1, 0, 1, 1)) ==
                         LiveMediaAdmission::kNoDecodableBoundary,
                     "expected one byte beyond the default 16 MiB limit to be rejected");

        LiveMediaQueue duration_default;
        ok &= expect(duration_default.push(
                         make_fragment("video", 0, 0, 0, 2'000'001, 1, true)) ==
                         LiveMediaAdmission::kNoDecodableBoundary,
                     "expected one microsecond beyond the default 2-second limit to be rejected");
    }

    {
        LiveMediaQueue queue(100, std::chrono::seconds(2));
        ok &= expect(queue.push(make_fragment("video", 0, 0, 0, 2'000'000, 60, true)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected the first exact-limit fragment to be accepted");
        ok &= expect(queue.push(make_fragment("audio", 0, 0, 0, 2'000'000, 40)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected literal 100-byte and 2-second limits to be inclusive");
        ok &= expect(queue.queued_bytes() == 100,
                     "expected exact queued byte accounting at the limit");
        ok &= expect(queue.queued_media_duration() == std::chrono::seconds(2),
                     "expected exact queued media span at the limit");
    }

    {
        LiveMediaQueue queue(100, std::chrono::seconds(2));
        ok &= expect(queue.push(make_fragment("init", 0, 0, 0, 0, 10),
                                LiveMediaFragmentRole::kInitialization) ==
                         LiveMediaAdmission::kAccepted,
                     "expected protected initialization to be admitted");
        ok &= expect(queue.push(make_fragment("video", 0, 0, 0, 500'000, 25, true)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected the oldest GOP keyframe to be admitted");
        ok &= expect(queue.push(make_fragment("audio", 0, 0, 0, 500'000, 15)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected aligned old-GOP audio to be admitted");
        ok &= expect(queue.push(make_fragment("video", 0, 1, 1'000'000, 500'000, 20)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected an old-GOP delta fragment to be admitted");
        ok &= expect(queue.push(make_fragment("audio", 0, 1, 1'000'000, 500'000, 10)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected later old-GOP audio to be admitted");
        ok &= expect(queue.push(make_fragment("audio", 1, 0, 1'500'000, 500'000, 20)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected recovery audio at the exact limit before its video keyframe");

        const LiveMediaAdmission recovery = queue.push(
            make_fragment("video", 1, 0, 1'500'000, 1'000'000, 30, true));
        ok &= expect(recovery == LiveMediaAdmission::kShedToKeyframe,
                     "expected simultaneous byte/time overflow to shed through the next keyframe");
        ok &= expect(queue.queued_bytes() == 60,
                     "expected shedding to retain initialization and aligned recovery media");
        ok &= expect(queue.queued_media_duration() == std::chrono::seconds(1),
                     "expected duration accounting to restart at the recovery keyframe");
        ok &= expect(queue.queued_bytes() <= 100 &&
                         queue.queued_media_duration() <= std::chrono::seconds(2),
                     "expected the recovered queue never to exceed either literal bound");

        const std::vector<MediaFragment> fragments = drain(queue);
        ok &= expect(fragments.size() == 3,
                     "expected initialization and both aligned recovery tracks");
        if (fragments.size() == 3) {
            ok &= expect(fragments[0].track_name == "init",
                         "expected initialization to survive media shedding");
            ok &= expect(fragments[1].track_name == "audio" && fragments[1].group_id == 1,
                         "expected timestamp-aligned audio queued before video to survive recovery");
            ok &= expect(fragments[2].track_name == "video" &&
                             fragments[2].group_id == 1 && fragments[2].is_video_keyframe,
                         "expected the first emitted video after recovery to be the keyframe");
        }
        ok &= expect(std::none_of(fragments.begin(), fragments.end(), [](const MediaFragment& fragment) {
                         return fragment.track_name != "init" && fragment.group_id == 0;
                     }),
                     "expected the complete old video GOP and aligned audio interval to be removed");
    }

    {
        LiveMediaQueue queue(100, std::chrono::seconds(2));
        ok &= expect(queue.push(make_fragment("init", 0, 0, 0, 0, 10),
                                LiveMediaFragmentRole::kInitialization) ==
                         LiveMediaAdmission::kAccepted,
                     "expected delayed-audio fixture initialization to be admitted");
        ok &= expect(queue.push(
                         make_fragment("video", 1, 0, 2'000'000, 500'000, 40, true)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected the recovery keyframe to arrive before delayed old audio");
        ok &= expect(queue.push(
                         make_fragment("audio", 1, 0, 2'000'000, 500'000, 20)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected aligned audio at the recovery timestamp to be admitted");
        ok &= expect(queue.push(
                         make_fragment("audio", 0, 0, 0, 500'000, 40)) ==
                         LiveMediaAdmission::kShedToKeyframe,
                     "expected a prior keyframe to remain a valid timestamp recovery boundary");

        const std::vector<MediaFragment> fragments = drain(queue);
        ok &= expect(fragments.size() == 3 && fragments[0].track_name == "init" &&
                         fragments[1].track_name == "video" &&
                         fragments[1].is_video_keyframe &&
                         fragments[2].track_name == "audio" &&
                         fragments[2].group_id == 1,
                     "expected delayed older audio to be shed while aligned recovery media remains");
    }

    {
        LiveMediaQueue queue(100, std::chrono::seconds(2));
        ok &= expect(queue.push(
                         make_fragment("video-a", 0, 0, 0, 500'000, 40, true)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected the first video track's old keyframe to be admitted");
        ok &= expect(queue.push(
                         make_fragment("video-b", 0, 0, 0, 500'000, 30, true)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected the second video track's old keyframe to be admitted");
        ok &= expect(queue.push(
                         make_fragment("audio", 0, 0, 0, 500'000, 10)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected old aligned audio to be admitted");
        ok &= expect(queue.push(make_fragment(
                         "video-a", 1, 0, 1'000'000, 500'000, 40, true)) ==
                         LiveMediaAdmission::kShedToKeyframe,
                     "expected overflow to recover at video-a's next keyframe");

        MediaFragment known_video_delta = make_fragment(
            "video-b", 1, 0, 1'000'000, 500'000, 10);
        known_video_delta.has_sap_type = false;
        ok &= expect(queue.push(std::move(known_video_delta)) ==
                         LiveMediaAdmission::kShedToKeyframe,
                     "expected persistent video-b classification to suppress deltas until its keyframe");
        ok &= expect(queue.push(make_fragment(
                         "video-new", 0, 0, 1'000'000, 500'000, 10)) ==
                         LiveMediaAdmission::kShedToKeyframe,
                     "expected a video track first seen as a delta during recovery to be suppressed");
        ok &= expect(queue.push(make_fragment(
                         "audio", 1, 0, 1'000'000, 500'000, 10)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected audio aligned to the recovery boundary to remain admitted");
        ok &= expect(queue.push(make_fragment(
                         "video-b", 1, 1, 1'100'000, 500'000, 20, true)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected video-b admission to resume at its own keyframe");
        ok &= expect(queue.push(make_fragment(
                         "video-new", 0, 1, 1'200'000, 500'000, 20, true)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected the newly classified video track to resume at its keyframe");
        ok &= expect(queue.queued_bytes() == 90 &&
                         queue.queued_media_duration() ==
                             std::chrono::microseconds(700'000),
                     "expected suppressed video deltas not to affect recovered queue accounting");

        const std::vector<MediaFragment> fragments = drain(queue);
        ok &= expect(fragments.size() == 4,
                     "expected only decodable post-recovery media to remain queued");
        if (fragments.size() == 4) {
            ok &= expect(fragments[0].track_name == "video-a" &&
                             fragments[0].is_video_keyframe,
                         "expected recovery to begin with video-a's keyframe");
            ok &= expect(fragments[1].track_name == "audio",
                         "expected aligned audio to remain in queue order");
            ok &= expect(fragments[2].track_name == "video-b" &&
                             fragments[2].is_video_keyframe,
                         "expected video-b's first retained object to be its keyframe");
            ok &= expect(fragments[3].track_name == "video-new" &&
                             fragments[3].is_video_keyframe,
                         "expected a newly discovered video's first retained object to be its keyframe");
        }
        ok &= expect(queue.queued_bytes() == 0 &&
                         queue.queued_media_duration() == std::chrono::microseconds(0),
                     "expected exact accounting after draining multi-video recovery");
    }

    {
        LiveMediaQueue bytes_only(100, std::chrono::seconds(2));
        ok &= expect(bytes_only.push(
                         make_fragment("video", 0, 0, 0, 500'000, 80, true)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected the byte-only fixture to start below both limits");
        ok &= expect(bytes_only.push(
                         make_fragment("audio", 0, 0, 0, 500'000, 21)) ==
                         LiveMediaAdmission::kNoDecodableBoundary,
                     "expected byte overflow alone to enforce the literal 100-byte bound");

        LiveMediaQueue duration_only(100, std::chrono::seconds(2));
        ok &= expect(duration_only.push(
                         make_fragment("video", 0, 0, 0, 1'000'000, 40, true)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected the duration-only fixture to start below both limits");
        ok &= expect(duration_only.push(
                         make_fragment("audio", 0, 0, 2'000'000, 1, 10)) ==
                         LiveMediaAdmission::kNoDecodableBoundary,
                     "expected duration overflow alone to enforce the literal 2-second bound");
    }

    {
        LiveMediaQueue queue(100, std::chrono::seconds(2));
        ok &= expect(queue.push(make_fragment(
                         "video", 0, 0,
                         (std::numeric_limits<std::uint64_t>::max)() - 100,
                         2'000'001, 10, true)) ==
                         LiveMediaAdmission::kNoDecodableBoundary,
                     "expected timestamp addition overflow never to understate media span");
        ok &= expect(queue.queued_bytes() == 0 && queue.resource_limit_exceeded(),
                     "expected overflowing media time to leave the valid queue unchanged");
    }

    {
        LiveMediaQueue queue(100, std::chrono::seconds(2));
        ok &= expect(queue.push(make_fragment("init", 0, 0, 0, 0, 10),
                                LiveMediaFragmentRole::kInitialization) ==
                         LiveMediaAdmission::kAccepted,
                     "expected no-boundary fixture initialization to be admitted");
        ok &= expect(queue.push(make_fragment("video", 0, 0, 0, 1'000'000, 40, true)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected no-boundary fixture keyframe to be admitted");
        ok &= expect(queue.push(make_fragment("audio", 0, 0, 0, 1'000'000, 20)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected no-boundary fixture audio to be admitted");

        const LiveMediaAdmission rejected = queue.push(
            make_fragment("video", 0, 1, 2'000'000, 1'000'000, 40));
        ok &= expect(rejected == LiveMediaAdmission::kNoDecodableBoundary,
                     "expected overflow without a later keyframe to be explicit");
        ok &= expect(queue.resource_limit_exceeded(),
                     "expected unrecoverable overflow to remain observable by the SRT publisher");
        ok &= expect(queue.push(
                         make_fragment("video", 1, 0, 3'000'000, 1'000'000, 10, true)) ==
                         LiveMediaAdmission::kNoDecodableBoundary,
                     "expected the no-decodable-boundary result to remain stable");
        ok &= expect(queue.queued_bytes() == 70,
                     "expected rejected media not to corrupt prior byte accounting");
        ok &= expect(queue.queued_media_duration() == std::chrono::seconds(1),
                     "expected rejected media not to corrupt prior duration accounting");
        const std::vector<MediaFragment> fragments = drain(queue);
        ok &= expect(fragments.size() == 3 && fragments[0].track_name == "init",
                     "expected a failed recovery not to discard initialization or valid queued media");
    }

    {
        constexpr std::size_t kFragmentCount = 1'000;
        LiveMediaQueue queue(2'000, std::chrono::seconds(2));
        std::atomic<bool> producer_ok = true;
        std::thread producer([&]() {
            for (std::size_t index = 0; index < kFragmentCount; ++index) {
                if (queue.push(make_fragment(
                        "audio", 0, index, index, 1, 1)) !=
                    LiveMediaAdmission::kAccepted) {
                    producer_ok.store(false, std::memory_order_release);
                    break;
                }
            }
            queue.mark_eof();
        });

        std::size_t consumed = 0;
        while (!queue.done()) {
            if (queue.try_pop().has_value()) {
                ++consumed;
            } else {
                std::this_thread::yield();
            }
        }
        producer.join();
        ok &= expect(producer_ok.load(std::memory_order_acquire) &&
                         consumed == kFragmentCount && queue.queued_bytes() == 0,
                     "expected concurrent producer/consumer accounting to remain exact");
    }

    {
        std::atomic<bool> stop_requested = false;
        LiveSrtQueueAdapter adapter(stop_requested);
        ok &= expect(adapter.admit(
                         make_fragment("video", 0, 0, 0, 2'000'000, 1, true)) ==
                         LiveMediaAdmission::kAccepted,
                     "expected the built-in SRT adapter to use its exact production duration limit");
        ok &= expect(adapter.admit(
                         make_fragment("audio", 0, 0, 2'000'000, 1, 1)) ==
                         LiveMediaAdmission::kNoDecodableBoundary &&
                         stop_requested.load(std::memory_order_acquire),
                     "expected adapter overflow to stop built-in SRT ingest");

        const TransportStatus resource_failure =
            adapter.publishing_status(TransportStatus{.ok = true});
        ok &= expect(!resource_failure.ok &&
                         resource_failure.message.find("resource limit") != std::string::npos &&
                         resource_failure.message.find("too far behind") != std::string::npos,
                     "expected the production adapter's resource-limit/too-far-behind diagnostic");

        const TransportStatus earlier_failure = adapter.publishing_status(
            TransportStatus{.ok = false, .message = "earlier transport failure"});
        ok &= expect(!earlier_failure.ok &&
                         earlier_failure.message == "earlier transport failure",
                     "expected an earlier transport failure to take precedence");
    }

    return ok ? 0 : 1;
}
