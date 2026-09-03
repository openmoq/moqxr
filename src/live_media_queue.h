#pragma once

#include "openmoq/publisher/cmaf_segmenter.h"
#include "openmoq/publisher/transport/publisher_transport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace openmoq::publisher {

enum class LiveMediaAdmission {
    kAccepted,
    kShedToKeyframe,
    kNoDecodableBoundary,
};

enum class LiveMediaFragmentRole {
    kMedia,
    kInitialization,
};

inline constexpr std::size_t kDefaultLiveSrtQueueMaxBytes =
    16U * 1024U * 1024U;
inline constexpr std::chrono::microseconds
    kDefaultLiveSrtQueueMaxMediaDuration = std::chrono::seconds(2);

class LiveMediaQueue {
public:
    explicit LiveMediaQueue(
        std::size_t max_bytes = kDefaultLiveSrtQueueMaxBytes,
        std::chrono::microseconds max_media_duration =
            kDefaultLiveSrtQueueMaxMediaDuration)
        : max_bytes_(max_bytes),
          max_media_duration_(max_media_duration) {}

    LiveMediaAdmission push(
        MediaFragment fragment,
        LiveMediaFragmentRole role = LiveMediaFragmentRole::kMedia) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (resource_limit_exceeded_) {
            return LiveMediaAdmission::kNoDecodableBoundary;
        }
        if (role == LiveMediaFragmentRole::kMedia) {
            const bool is_video =
                video_tracks_.contains(fragment.track_name) ||
                is_video_fragment(fragment);
            if (is_video) {
                const auto [track_it, inserted] =
                    video_tracks_.insert(fragment.track_name);
                if (active_recovery_boundary_.has_value() && inserted) {
                    awaiting_recovery_keyframe_.insert(*track_it);
                }
            }
            if (active_recovery_boundary_.has_value() &&
                fragment.start_time_us <
                    active_recovery_boundary_->start_time_us) {
                return LiveMediaAdmission::kShedToKeyframe;
            }
            if (is_video &&
                awaiting_recovery_keyframe_.contains(fragment.track_name)) {
                if (!fragment.is_video_keyframe) {
                    return LiveMediaAdmission::kShedToKeyframe;
                }
                awaiting_recovery_keyframe_.erase(fragment.track_name);
            }
        }

        entries_.push_back(Entry{std::move(fragment), role});
        const QueueBounds appended_bounds = measure(entries_, std::nullopt);
        if (within_limits(appended_bounds)) {
            apply_bounds(appended_bounds);
            return LiveMediaAdmission::kAccepted;
        }

        std::optional<RecoveryBoundary> recovery;
        std::set<std::uint64_t> recovery_timestamps;
        for (const Entry& entry : entries_) {
            if (entry.role == LiveMediaFragmentRole::kMedia &&
                entry.fragment.is_video_keyframe) {
                recovery_timestamps.insert(entry.fragment.start_time_us);
            }
        }
        for (const std::uint64_t timestamp : recovery_timestamps) {
            const RecoveryBoundary candidate{.start_time_us = timestamp};
            const QueueBounds candidate_bounds = measure(entries_, candidate);
            if (within_limits(candidate_bounds)) {
                recovery = candidate;
                break;
            }
        }

        if (!recovery.has_value()) {
            entries_.pop_back();
            resource_limit_exceeded_ = true;
            return LiveMediaAdmission::kNoDecodableBoundary;
        }

        std::deque<Entry> retained;
        std::map<std::string, bool> video_started;
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            Entry& entry = entries_[index];
            if (should_retain(entry, *recovery, video_started)) {
                retained.push_back(std::move(entry));
            }
        }
        entries_ = std::move(retained);
        active_recovery_boundary_ = recovery;
        awaiting_recovery_keyframe_ = video_tracks_;
        for (const auto& [track_name, started] : video_started) {
            if (started) {
                awaiting_recovery_keyframe_.erase(track_name);
            }
        }
        apply_bounds(measure(entries_, std::nullopt));
        return LiveMediaAdmission::kShedToKeyframe;
    }

    std::optional<MediaFragment> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (entries_.empty()) {
            return std::nullopt;
        }
        MediaFragment fragment = std::move(entries_.front().fragment);
        entries_.pop_front();
        apply_bounds(measure(entries_, std::nullopt));
        return fragment;
    }

    void mark_eof() {
        std::lock_guard<std::mutex> lock(mutex_);
        eof_ = true;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.empty();
    }

    bool done() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return eof_ && entries_.empty();
    }

    bool resource_limit_exceeded() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return resource_limit_exceeded_;
    }

    std::size_t queued_bytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queued_bytes_;
    }

    std::chrono::microseconds queued_media_duration() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queued_media_duration_;
    }

private:
    struct Entry {
        MediaFragment fragment;
        LiveMediaFragmentRole role = LiveMediaFragmentRole::kMedia;
    };

    struct RecoveryBoundary {
        std::uint64_t start_time_us = 0;
    };

    struct QueueBounds {
        std::size_t bytes = 0;
        std::chrono::microseconds media_duration{0};
        bool bytes_overflow = false;
        bool media_time_overflow = false;
    };

    static std::size_t fragment_payload_size(const MediaFragment& fragment) {
        return fragment.payload.owned_bytes.empty()
                   ? fragment.payload.span.size
                   : fragment.payload.owned_bytes.size();
    }

    static bool is_video_fragment(const MediaFragment& fragment) {
        return fragment.is_video_keyframe ||
               (fragment.has_sap_type &&
                (fragment.sap_type == 0 || fragment.sap_type == 2));
    }

    bool should_retain(const Entry& entry,
                       const RecoveryBoundary& boundary,
                       std::map<std::string, bool>& video_started) const {
        if (entry.role == LiveMediaFragmentRole::kInitialization) {
            return true;
        }
        if (entry.fragment.start_time_us < boundary.start_time_us) {
            return false;
        }
        if (!video_tracks_.contains(entry.fragment.track_name)) {
            return true;
        }
        bool& started = video_started[entry.fragment.track_name];
        if (!started && !entry.fragment.is_video_keyframe) {
            return false;
        }
        started = true;
        return true;
    }

    QueueBounds measure(
        const std::deque<Entry>& entries,
        std::optional<RecoveryBoundary> boundary) const {
        QueueBounds bounds;
        std::optional<std::uint64_t> first_media_time;
        std::uint64_t last_media_end = 0;
        std::map<std::string, bool> video_started;

        for (std::size_t index = 0; index < entries.size(); ++index) {
            const Entry& entry = entries[index];
            if (boundary.has_value() &&
                !should_retain(entry, *boundary, video_started)) {
                continue;
            }

            const std::size_t payload_size = fragment_payload_size(entry.fragment);
            if (payload_size > (std::numeric_limits<std::size_t>::max)() - bounds.bytes) {
                bounds.bytes_overflow = true;
            } else {
                bounds.bytes += payload_size;
            }

            if (entry.role == LiveMediaFragmentRole::kInitialization) {
                continue;
            }
            first_media_time = first_media_time.has_value()
                                   ? (std::min)(*first_media_time,
                                                entry.fragment.start_time_us)
                                   : std::optional<std::uint64_t>(
                                         entry.fragment.start_time_us);
            const bool media_end_overflow =
                entry.fragment.duration_us >
                (std::numeric_limits<std::uint64_t>::max)() -
                    entry.fragment.start_time_us;
            bounds.media_time_overflow |= media_end_overflow;
            const std::uint64_t media_end = media_end_overflow
                                                ? (std::numeric_limits<std::uint64_t>::max)()
                                                : entry.fragment.start_time_us +
                                                      entry.fragment.duration_us;
            last_media_end = (std::max)(last_media_end, media_end);
        }

        if (first_media_time.has_value()) {
            const std::uint64_t span_us = last_media_end - *first_media_time;
            const auto max_rep = static_cast<std::uint64_t>(
                (std::chrono::microseconds::max)().count());
            bounds.media_duration = std::chrono::microseconds(
                static_cast<std::chrono::microseconds::rep>(
                    (std::min)(span_us, max_rep)));
        }
        return bounds;
    }

    bool within_limits(const QueueBounds& bounds) const {
        return !bounds.bytes_overflow && !bounds.media_time_overflow &&
               bounds.bytes <= max_bytes_ &&
               bounds.media_duration <= max_media_duration_;
    }

    void apply_bounds(const QueueBounds& bounds) {
        queued_bytes_ = bounds.bytes;
        queued_media_duration_ = bounds.media_duration;
    }

    const std::size_t max_bytes_;
    const std::chrono::microseconds max_media_duration_;
    mutable std::mutex mutex_;
    std::deque<Entry> entries_;
    std::set<std::string> video_tracks_;
    std::set<std::string> awaiting_recovery_keyframe_;
    std::optional<RecoveryBoundary> active_recovery_boundary_;
    std::size_t queued_bytes_ = 0;
    std::chrono::microseconds queued_media_duration_{0};
    bool eof_ = false;
    bool resource_limit_exceeded_ = false;
};

namespace transport {

inline constexpr std::string_view kLiveSrtResourceLimitDiagnostic =
    "live SRT resource limit exceeded: source is too far behind and "
    "no decodable keyframe boundary exists within 16 MiB/2 s";

class LiveSrtQueueAdapter {
public:
    explicit LiveSrtQueueAdapter(std::atomic<bool>& stop_requested)
        : stop_requested_(stop_requested) {}

    LiveMediaAdmission admit(MediaFragment fragment) {
        const LiveMediaAdmission admission = queue_.push(std::move(fragment));
        if (admission == LiveMediaAdmission::kNoDecodableBoundary) {
            stop_requested_.store(true, std::memory_order_release);
        }
        return admission;
    }

    std::optional<MediaFragment> try_pop() {
        return queue_.try_pop();
    }

    void mark_eof() {
        queue_.mark_eof();
    }

    bool done() const {
        return queue_.done();
    }

    TransportStatus publishing_status(TransportStatus current_status) const {
        if (!current_status.ok || !queue_.resource_limit_exceeded()) {
            return current_status;
        }
        return TransportStatus{
            .ok = false,
            .message = std::string(kLiveSrtResourceLimitDiagnostic),
        };
    }

private:
    std::atomic<bool>& stop_requested_;
    LiveMediaQueue queue_;
};

}  // namespace transport

}  // namespace openmoq::publisher
