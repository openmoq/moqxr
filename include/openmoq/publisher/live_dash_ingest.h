#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "openmoq/publisher/cmaf_segmenter.h"
#include "openmoq/publisher/live_object.h"
#include "openmoq/publisher/transport/publisher_transport.h"

namespace openmoq::publisher {

class ChunkedBodyDecoder {
public:
    explicit ChunkedBodyDecoder(std::size_t max_chunk_size = 1024 * 1024);

    void append(std::span<const std::uint8_t> bytes);
    std::vector<std::uint8_t> take_decoded();
    bool complete() const;
    bool failed() const;
    const std::string& error() const;

private:
    enum class State {
        kSizeLine,
        kData,
        kDataCrLf,
        kTrailerLine,
        kComplete,
        kFailed,
    };

    void parse_available();
    void fail(std::string message);

    std::size_t max_chunk_size_ = 0;
    State state_ = State::kSizeLine;
    std::vector<std::uint8_t> input_;
    std::vector<std::uint8_t> decoded_;
    std::size_t current_chunk_size_ = 0;
    std::string error_;
};

struct LiveDashIngestConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
    std::string path_prefix = "/ingest";
    std::size_t queue_depth = 128;
    std::size_t max_chunk_size = 1024 * 1024;
};

class LiveDashIngestSession {
public:
    explicit LiveDashIngestSession(std::size_t queue_depth = 128);

    void ingest(std::string path, std::span<const std::uint8_t> bytes);
    void close();

    bool wait_for_tracks(std::chrono::milliseconds first_track_timeout,
                         std::chrono::milliseconds settle_timeout);
    LiveObjectSource source();
    std::optional<LiveObject> try_next_object();

private:
    struct PathState {
        StreamingMp4Reader reader;
        std::vector<std::uint8_t> init_bytes;
        std::vector<TrackDescription> tracks;
        std::vector<std::uint8_t> pending_moof;
        std::map<std::string, std::size_t> next_group_by_track;
        bool initialized = false;
    };

    std::optional<LiveObject> next_object_blocking();
    void process_box_locked(PathState& path_state,
                            std::string_view path,
                            const StreamingBoxResult& box);
    void enqueue_locked(LiveObject object);
    std::vector<LiveTrack> snapshot_tracks_locked() const;
    LiveObject build_catalog_locked() const;

    std::size_t queue_depth_ = 0;
    std::map<std::string, PathState> paths_;
    std::vector<TrackDescription> tracks_;
    std::deque<LiveObject> queue_;
    bool catalog_dirty_ = false;
    bool closed_ = false;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};

class LiveDashIngestServer {
public:
    explicit LiveDashIngestServer(LiveDashIngestConfig config);
    ~LiveDashIngestServer();

    transport::TransportStatus start();
    void stop();
    std::uint16_t bound_port() const;
    bool wait_for_tracks(std::chrono::milliseconds first_track_timeout,
                         std::chrono::milliseconds settle_timeout);
    LiveObjectSource source();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace openmoq::publisher
