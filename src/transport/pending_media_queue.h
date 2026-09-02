#ifndef OPENMOQ_PUBLISHER_TRANSPORT_PENDING_MEDIA_QUEUE_H
#define OPENMOQ_PUBLISHER_TRANSPORT_PENDING_MEDIA_QUEUE_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace openmoq::publisher::transport {

enum class MediaAdmission { kAccepted, kWouldBlock, kOversized };

struct PendingMediaWrite {
    std::uint64_t stream_id;
    std::vector<std::uint8_t> bytes;
    std::size_t offset = 0;
    bool fin = false;
    std::uint8_t transport_priority = 255;
    std::optional<std::chrono::steady_clock::time_point> object_deadline;
    std::optional<std::chrono::steady_clock::time_point> subgroup_deadline;
};

enum class PendingMediaFrontStatus { kAvailable, kEmpty, kExpired };

struct PendingMediaFront {
    PendingMediaFrontStatus status = PendingMediaFrontStatus::kEmpty;
    const PendingMediaWrite* write = nullptr;
};

inline constexpr std::uint64_t kDeliveryTimeoutErrorCode = 0x02;

struct DeliveryTimeoutReset {
    std::uint64_t stream_id;
    std::uint64_t error_code = kDeliveryTimeoutErrorCode;
};

class SubgroupDeadlineTracker {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    void arm(std::uint64_t stream_id, TimePoint deadline) {
        auto [it, inserted] = deadlines_.emplace(stream_id, deadline);
        if (!inserted && deadline < it->second) {
            it->second = deadline;
        }
    }

    void mark_all_data_committed(std::uint64_t stream_id) { deadlines_.erase(stream_id); }

    void cancel(std::uint64_t stream_id) { deadlines_.erase(stream_id); }

    std::vector<DeliveryTimeoutReset> take_expired(TimePoint now) {
        std::vector<DeliveryTimeoutReset> expired;
        for (auto it = deadlines_.begin(); it != deadlines_.end();) {
            if (now < it->second) {
                ++it;
                continue;
            }
            expired.push_back({it->first, kDeliveryTimeoutErrorCode});
            it = deadlines_.erase(it);
        }
        return expired;
    }

    void clear() { deadlines_.clear(); }

private:
    std::map<std::uint64_t, TimePoint> deadlines_;
};

class PendingMediaQueue {
public:
    explicit PendingMediaQueue(std::size_t capacity) : capacity_(capacity) {}

    MediaAdmission try_push(PendingMediaWrite write) {
        if (write.bytes.size() > capacity_) {
            return MediaAdmission::kOversized;
        }
        if (write.bytes.size() > capacity_ - queued_bytes_) {
            return MediaAdmission::kWouldBlock;
        }

        const std::uint64_t stream_id = write.stream_id;
        const std::size_t write_size = write.bytes.size();
        write.offset = 0;
        const bool new_stream = streams_.find(stream_id) == streams_.end();
        try {
            streams_[stream_id].push_back(std::move(write));
        } catch (...) {
            if (new_stream) {
                streams_.erase(stream_id);
            }
            throw;
        }
        queued_bytes_ += write_size;
        return MediaAdmission::kAccepted;
    }

    PendingMediaFront inspect_front(
        std::uint64_t stream_id,
        std::chrono::steady_clock::time_point now) {
        auto stream = streams_.find(stream_id);
        if (stream == streams_.end()) {
            return {};
        }

        auto& writes = stream->second;
        if (!writes.empty()) {
            const PendingMediaWrite& write = writes.front();
            if (write.offset == 0 && expired(write, now)) {
                clear_stream(stream_id);
                return {.status = PendingMediaFrontStatus::kExpired, .write = nullptr};
            }
            return {.status = PendingMediaFrontStatus::kAvailable, .write = &write};
        }

        streams_.erase(stream);
        return {};
    }

    const PendingMediaWrite* front(
        std::uint64_t stream_id,
        std::chrono::steady_clock::time_point now) {
        return inspect_front(stream_id, now).write;
    }

    std::size_t consume(std::uint64_t stream_id, std::size_t bytes) {
        auto stream = streams_.find(stream_id);
        if (stream == streams_.end() || stream->second.empty()) {
            return 0;
        }

        auto& writes = stream->second;
        PendingMediaWrite& write = writes.front();
        if (bytes == 0) {
            if (!write.bytes.empty() || !write.fin) {
                return 0;
            }
            writes.pop_front();
            if (writes.empty()) {
                streams_.erase(stream);
            }
            return 0;
        }

        const std::size_t remaining = remaining_bytes(write);
        const std::size_t consumed = bytes < remaining ? bytes : remaining;
        write.offset += consumed;
        queued_bytes_ -= consumed;

        if (write.offset == write.bytes.size()) {
            writes.pop_front();
        }
        if (writes.empty()) {
            streams_.erase(stream);
        }
        return consumed;
    }

    std::size_t clear_stream(std::uint64_t stream_id) {
        auto stream = streams_.find(stream_id);
        if (stream == streams_.end()) {
            return 0;
        }

        std::size_t removed = 0;
        for (const PendingMediaWrite& write : stream->second) {
            removed += remaining_bytes(write);
        }
        queued_bytes_ -= removed;
        streams_.erase(stream);
        return removed;
    }

    std::size_t clear_connection() {
        const std::size_t removed = queued_bytes_;
        streams_.clear();
        queued_bytes_ = 0;
        return removed;
    }

    std::size_t clear() { return clear_connection(); }

    std::size_t queued_bytes() const { return queued_bytes_; }

private:
    using StreamWrites = std::deque<PendingMediaWrite>;

    static std::size_t remaining_bytes(const PendingMediaWrite& write) {
        if (write.offset >= write.bytes.size()) {
            return 0;
        }
        return write.bytes.size() - write.offset;
    }

    static bool expired(const PendingMediaWrite& write,
                        std::chrono::steady_clock::time_point now) {
        return (write.object_deadline.has_value() && now >= *write.object_deadline) ||
               (write.subgroup_deadline.has_value() && now >= *write.subgroup_deadline);
    }

    std::size_t capacity_;
    std::size_t queued_bytes_ = 0;
    std::unordered_map<std::uint64_t, StreamWrites> streams_;
};

}  // namespace openmoq::publisher::transport

#endif  // OPENMOQ_PUBLISHER_TRANSPORT_PENDING_MEDIA_QUEUE_H
