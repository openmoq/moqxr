#include "openmoq/publisher/live_dash_ingest.h"

#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/mp4_box.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace openmoq::publisher {
namespace {

std::string path_slug(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    std::string slug = std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
    if (slug.empty()) {
        slug = "dash";
    }
    for (char& ch : slug) {
        const bool alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        const bool digit = ch >= '0' && ch <= '9';
        if (!alpha && !digit) {
            ch = '_';
        }
    }
    return slug;
}

std::string json_escape(std::string_view value) {
    std::string out;
    for (const char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string role_for_handler(std::string_view handler_type) {
    if (handler_type == "vide") {
        return "video";
    }
    if (handler_type == "soun") {
        return "audio";
    }
    return "data";
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.substr(0, prefix.size()) == prefix;
}

bool path_matches_prefix(std::string_view path, std::string_view prefix) {
    if (!starts_with(path, prefix)) {
        return false;
    }
    return path.size() == prefix.size() || prefix.ends_with('/') || path[prefix.size()] == '/';
}

std::string trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::string lower_ascii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool send_all(int fd, std::string_view bytes) {
#if !defined(_WIN32)
    while (!bytes.empty()) {
        int flags = 0;
#if defined(MSG_NOSIGNAL)
        flags = MSG_NOSIGNAL;
#endif
        const ssize_t sent = ::send(fd, bytes.data(), bytes.size(), flags);
        if (sent <= 0) {
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
#else
    static_cast<void>(fd);
    static_cast<void>(bytes);
    return false;
#endif
}

void close_fd(int fd) {
#if !defined(_WIN32)
    if (fd >= 0) {
        static_cast<void>(::close(fd));
    }
#else
    static_cast<void>(fd);
#endif
}

struct ParsedRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
};

std::optional<ParsedRequest> parse_request_headers(std::string_view text) {
    const std::size_t line_end = text.find("\r\n");
    if (line_end == std::string_view::npos) {
        return std::nullopt;
    }
    std::istringstream request_line(std::string(text.substr(0, line_end)));
    ParsedRequest request;
    std::string version;
    request_line >> request.method >> request.path >> version;
    if (request.method.empty() || request.path.empty() || version.rfind("HTTP/", 0) != 0) {
        return std::nullopt;
    }

    std::size_t cursor = line_end + 2;
    while (cursor < text.size()) {
        const std::size_t next = text.find("\r\n", cursor);
        if (next == std::string_view::npos || next == cursor) {
            break;
        }
        const std::string_view line = text.substr(cursor, next - cursor);
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        request.headers.emplace(lower_ascii(trim(line.substr(0, colon))), trim(line.substr(colon + 1)));
        cursor = next + 2;
    }
    return request;
}

}  // namespace

ChunkedBodyDecoder::ChunkedBodyDecoder(std::size_t max_chunk_size)
    : max_chunk_size_(max_chunk_size) {}

void ChunkedBodyDecoder::append(std::span<const std::uint8_t> bytes) {
    if (state_ == State::kFailed || state_ == State::kComplete) {
        return;
    }
    input_.insert(input_.end(), bytes.begin(), bytes.end());
    parse_available();
}

std::vector<std::uint8_t> ChunkedBodyDecoder::take_decoded() {
    std::vector<std::uint8_t> out;
    out.swap(decoded_);
    return out;
}

bool ChunkedBodyDecoder::complete() const {
    return state_ == State::kComplete;
}

bool ChunkedBodyDecoder::failed() const {
    return state_ == State::kFailed;
}

const std::string& ChunkedBodyDecoder::error() const {
    return error_;
}

void ChunkedBodyDecoder::fail(std::string message) {
    state_ = State::kFailed;
    error_ = std::move(message);
}

void ChunkedBodyDecoder::parse_available() {
    while (state_ != State::kFailed && state_ != State::kComplete) {
        if (state_ == State::kSizeLine || state_ == State::kTrailerLine) {
            constexpr std::array<std::uint8_t, 2> kCrLf{'\r', '\n'};
            auto crlf = std::search(input_.begin(), input_.end(), kCrLf.begin(), kCrLf.end());
            if (crlf == input_.end()) {
                if (input_.size() > 8192) {
                    fail("chunk header line too long");
                }
                return;
            }
            const std::string line(input_.begin(), crlf);
            input_.erase(input_.begin(), crlf + 2);
            if (state_ == State::kTrailerLine) {
                if (line.empty()) {
                    state_ = State::kComplete;
                }
                continue;
            }

            const std::size_t extension = line.find(';');
            const std::string size_text = line.substr(0, extension);
            if (size_text.empty()) {
                fail("empty chunk size");
                return;
            }
            std::size_t value = 0;
            for (const char ch : size_text) {
                int digit = -1;
                if (ch >= '0' && ch <= '9') {
                    digit = ch - '0';
                } else if (ch >= 'a' && ch <= 'f') {
                    digit = 10 + ch - 'a';
                } else if (ch >= 'A' && ch <= 'F') {
                    digit = 10 + ch - 'A';
                } else {
                    fail("invalid chunk size");
                    return;
                }
                if (value > (std::numeric_limits<std::size_t>::max() - static_cast<std::size_t>(digit)) / 16U) {
                    fail("chunk size overflow");
                    return;
                }
                value = value * 16U + static_cast<std::size_t>(digit);
            }
            if (value > max_chunk_size_) {
                fail("chunk exceeds maximum size");
                return;
            }
            current_chunk_size_ = value;
            state_ = value == 0 ? State::kTrailerLine : State::kData;
            continue;
        }

        if (state_ == State::kData) {
            if (input_.size() < current_chunk_size_) {
                return;
            }
            decoded_.insert(decoded_.end(), input_.begin(),
                            input_.begin() + static_cast<std::ptrdiff_t>(current_chunk_size_));
            input_.erase(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(current_chunk_size_));
            state_ = State::kDataCrLf;
            continue;
        }

        if (state_ == State::kDataCrLf) {
            if (input_.size() < 2) {
                return;
            }
            if (input_[0] != '\r' || input_[1] != '\n') {
                fail("missing chunk data terminator");
                return;
            }
            input_.erase(input_.begin(), input_.begin() + 2);
            state_ = State::kSizeLine;
        }
    }
}

LiveDashIngestSession::LiveDashIngestSession(std::size_t queue_depth)
    : queue_depth_(queue_depth == 0 ? 1 : queue_depth) {}

void LiveDashIngestSession::ingest(std::string path, std::span<const std::uint8_t> bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = paths_.find(path);
    if (it == paths_.end()) {
        if (tracks_frozen_) {
            return;
        }
        it = paths_.emplace(path, PathState{}).first;
    }
    PathState& path_state = it->second;
    path_state.reader.append(bytes.data(), bytes.size());
    while (std::optional<StreamingBoxResult> box = path_state.reader.next_box()) {
        process_box_locked(path_state, path, *box);
    }
    cv_.notify_all();
}

void LiveDashIngestSession::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    cv_.notify_all();
}

bool LiveDashIngestSession::wait_for_tracks(std::chrono::milliseconds first_track_timeout,
                                            std::chrono::milliseconds settle_timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (tracks_.empty()) {
        if (!cv_.wait_for(lock, first_track_timeout, [&]() { return !tracks_.empty() || closed_; })) {
            return false;
        }
    }
    if (tracks_.empty()) {
        return false;
    }
    std::size_t observed_count = tracks_.size();
    while (!closed_) {
        const bool changed = cv_.wait_for(lock, settle_timeout, [&]() {
            return closed_ || tracks_.size() != observed_count;
        });
        if (!changed || closed_) {
            break;
        }
        observed_count = tracks_.size();
    }
    return !tracks_.empty();
}

LiveObjectSource LiveDashIngestSession::source() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Freeze the announced track set: the publisher builds a fixed alias table
    // from this snapshot, so any path that appears afterwards must be dropped
    // rather than enqueued against an unknown track.
    tracks_frozen_ = true;
    return LiveObjectSource{
        .tracks = snapshot_tracks_locked(),
        .next_object = [this]() {
            return next_object_blocking();
        },
        .is_finished = [this]() {
            return finished();
        },
    };
}

bool LiveDashIngestSession::finished() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

std::optional<LiveObject> LiveDashIngestSession::try_next_object() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (catalog_dirty_ && !tracks_.empty()) {
        catalog_dirty_ = false;
        return build_catalog_locked();
    }
    if (queue_.empty()) {
        return std::nullopt;
    }
    LiveObject object = std::move(queue_.front());
    queue_.pop_front();
    return object;
}

std::optional<LiveObject> LiveDashIngestSession::next_object_blocking() {
    // Bounded wait so the publisher periodically regains control to service
    // subscribe/control messages during media gaps instead of parking here.
    // A gap returns nullopt with closed_ == false, which the publisher treats
    // as "keep polling" via the source's is_finished predicate; only close()
    // ends the stream.
    constexpr auto kPollInterval = std::chrono::milliseconds(200);
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, kPollInterval,
                 [&]() { return closed_ || catalog_dirty_ || !queue_.empty(); });
    if (catalog_dirty_ && !tracks_.empty()) {
        catalog_dirty_ = false;
        return build_catalog_locked();
    }
    if (queue_.empty()) {
        return std::nullopt;
    }
    LiveObject object = std::move(queue_.front());
    queue_.pop_front();
    return object;
}

void LiveDashIngestSession::process_box_locked(PathState& path_state,
                                               std::string_view path,
                                               const StreamingBoxResult& box) {
    if (box.type == "ftyp" || box.type == "moov") {
        path_state.init_bytes.insert(path_state.init_bytes.end(), box.bytes.begin(), box.bytes.end());
        if (box.type == "moov" && !path_state.initialized) {
            // The announced track set is frozen once publishing starts; a path
            // whose init segment arrives afterwards cannot be added, so ignore
            // it rather than emit media the publisher would reject.
            if (tracks_frozen_) {
                std::cerr << "[dash-ingest] ignoring late path '" << path
                          << "': track set already published" << std::endl;
                return;
            }
            path_state.tracks = extract_tracks(parse_mp4_boxes(path_state.init_bytes), path_state.init_bytes);
            const std::string prefix = path_slug(path);
            for (std::size_t index = 0; index < path_state.tracks.size(); ++index) {
                TrackDescription& track = path_state.tracks[index];
                track.track_name = prefix + "_" + track.track_name;
                std::string init_data;
                try {
                    init_data = track_init_data_base64(path_state.init_bytes, track, index);
                } catch (const std::exception& error) {
                    std::cerr << "[dash-ingest] track '" << track.track_name
                              << "' has no usable init segment: " << error.what() << std::endl;
                }
                tracks_.push_back(RegisteredTrack{.description = track,
                                                  .init_data_base64 = std::move(init_data)});
            }
            path_state.initialized = true;
            catalog_dirty_ = true;
        }
        return;
    }
    if (!path_state.initialized) {
        return;
    }
    if (box.type == "moof") {
        path_state.pending_moof = box.bytes;
        return;
    }
    if (box.type != "mdat" || path_state.pending_moof.empty()) {
        return;
    }

    MediaFragment fragment;
    try {
        fragment = build_live_fragment(path_state.pending_moof, box.bytes, path_state.tracks, 0);
    } catch (const std::exception& error) {
        // A malformed fragment (e.g. a moof referencing an unknown track) must
        // not take down the ingest thread; drop it and keep serving the path.
        std::cerr << "[dash-ingest] dropping fragment on path '" << path
                  << "': " << error.what() << std::endl;
        path_state.pending_moof.clear();
        return;
    }
    path_state.pending_moof.clear();
    if (!track_published_locked(fragment.track_name)) {
        return;
    }
    fragment.group_id = path_state.next_group_by_track[fragment.track_name]++;
    enqueue_locked(LiveObject{
        .track_name = fragment.track_name,
        .group_id = fragment.group_id,
        .subgroup_id = 0,
        .object_id = fragment.object_id,
        .media_time_us = fragment.start_time_us,
        .media_duration_us = fragment.duration_us,
        .payload = std::move(fragment.payload.owned_bytes),
    });
}

bool LiveDashIngestSession::track_published_locked(std::string_view track_name) const {
    for (const auto& track : tracks_) {
        if (track.description.track_name == track_name) {
            return true;
        }
    }
    return false;
}

void LiveDashIngestSession::enqueue_locked(LiveObject object) {
    while (queue_.size() >= queue_depth_) {
        queue_.pop_front();
    }
    queue_.push_back(std::move(object));
}

std::vector<LiveTrack> LiveDashIngestSession::snapshot_tracks_locked() const {
    std::vector<LiveTrack> out;
    if (!tracks_.empty()) {
        out.push_back(LiveTrack{.track_name = "catalog"});
    }
    for (const auto& track : tracks_) {
        out.push_back(LiveTrack{.track_name = track.description.track_name});
    }
    return out;
}

LiveObject LiveDashIngestSession::build_catalog_locked() {
    std::ostringstream catalog;
    catalog << "{\"version\":1,\"format\":\"cmsf\",\"tracks\":[";
    bool first = true;
    for (const auto& registered : tracks_) {
        const TrackDescription& track = registered.description;
        if (!first) {
            catalog << ',';
        }
        first = false;
        catalog << "{\"name\":\"" << json_escape(track.track_name) << "\","
                << "\"id\":" << track.track_id << ','
                << "\"role\":\"" << role_for_handler(track.handler_type) << "\","
                << "\"packaging\":\"cmaf\","
                << "\"renderGroup\":1,"
                << "\"isLive\":true";
        if (!track.codec.empty()) {
            catalog << ",\"codec\":\"" << json_escape(track.codec) << "\"";
        }
        if (track.handler_type == "vide") {
            catalog << ",\"width\":" << track.width
                    << ",\"height\":" << track.height;
            if (track.frame_rate > 0.0) {
                catalog << ",\"frameRate\":" << std::setprecision(6) << track.frame_rate;
            }
        } else if (track.handler_type == "soun") {
            catalog << ",\"sampleRate\":" << track.sample_rate
                    << ",\"channelCount\":" << track.channel_count;
        }
        if (!registered.init_data_base64.empty()) {
            catalog << ",\"initData\":\"" << registered.init_data_base64 << "\"";
        }
        catalog << '}';
    }
    catalog << "]}";
    const std::string payload = catalog.str();
    // A fresh group per emission keeps re-published catalogs (a new track set)
    // from colliding with the object IDs of an already-delivered catalog.
    return LiveObject{
        .track_name = "catalog",
        .group_id = catalog_group_id_++,
        .subgroup_id = 0,
        .object_id = 0,
        .media_time_us = 0,
        .media_duration_us = 0,
        .payload = std::vector<std::uint8_t>(payload.begin(), payload.end()),
    };
}

struct LiveDashIngestServer::Impl {
    explicit Impl(LiveDashIngestConfig server_config)
        : config(std::move(server_config)),
          session(config.queue_depth) {}

    LiveDashIngestConfig config;
    LiveDashIngestSession session;
    std::thread accept_thread;
    struct WorkerThread {
        std::thread thread;
        std::shared_ptr<std::atomic_bool> done;
    };
    std::vector<WorkerThread> worker_threads;
    std::set<int> active_client_fds;
    mutable std::mutex mutex;
    bool stop_requested = false;
    int listen_fd = -1;
    std::uint16_t bound_port = 0;

    void accept_loop();
    void reap_finished_workers_locked();
    void shutdown_active_clients_locked();
    bool stop_requested_now() const;
    void handle_client(int client_fd);
};

LiveDashIngestServer::LiveDashIngestServer(LiveDashIngestConfig config)
    : impl_(std::make_shared<Impl>(std::move(config))) {}

LiveDashIngestServer::~LiveDashIngestServer() {
    stop();
}

transport::TransportStatus LiveDashIngestServer::start() {
#if defined(_WIN32)
    return transport::TransportStatus::failure("DASH ingest server is not supported on Windows yet");
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (impl_->listen_fd < 0) {
        return transport::TransportStatus::failure("failed to create DASH ingest socket");
    }

    int reuse = 1;
    static_cast<void>(setsockopt(impl_->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(impl_->config.port);
    if (::inet_pton(AF_INET, impl_->config.host.c_str(), &address.sin_addr) != 1) {
        close_fd(impl_->listen_fd);
        impl_->listen_fd = -1;
        return transport::TransportStatus::failure("invalid DASH ingest listen host");
    }
    if (::bind(impl_->listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_fd(impl_->listen_fd);
        impl_->listen_fd = -1;
        return transport::TransportStatus::failure("failed to bind DASH ingest socket");
    }
    if (::listen(impl_->listen_fd, 16) != 0) {
        close_fd(impl_->listen_fd);
        impl_->listen_fd = -1;
        return transport::TransportStatus::failure("failed to listen on DASH ingest socket");
    }
    // Non-blocking so accept() cannot park if a ready connection is aborted
    // between the accept loop's poll() wakeup and the accept() call.
    const int listen_flags = ::fcntl(impl_->listen_fd, F_GETFL, 0);
    if (listen_flags >= 0) {
        static_cast<void>(::fcntl(impl_->listen_fd, F_SETFL, listen_flags | O_NONBLOCK));
    }
    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(impl_->listen_fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
        impl_->bound_port = ntohs(bound.sin_port);
    }
    impl_->accept_thread = std::thread([impl = impl_]() {
        impl->accept_loop();
    });
    return transport::TransportStatus::success();
#endif
}

void LiveDashIngestServer::stop() {
    int listen_fd_to_close = -1;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->stop_requested) {
            return;
        }
        impl_->stop_requested = true;
#if !defined(_WIN32)
        if (impl_->listen_fd >= 0) {
            static_cast<void>(::shutdown(impl_->listen_fd, SHUT_RDWR));
        }
        impl_->shutdown_active_clients_locked();
#endif
        // Keep listen_fd valid (shutdown, not closed) until the accept thread
        // has exited. Closing it now would let another thread reuse the number
        // while accept_loop is still calling ::accept() on it.
        listen_fd_to_close = impl_->listen_fd;
    }
    impl_->session.close();
    if (impl_->accept_thread.joinable()) {
        impl_->accept_thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->listen_fd = -1;
    }
    close_fd(listen_fd_to_close);
    std::vector<LiveDashIngestServer::Impl::WorkerThread> workers;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        workers.swap(impl_->worker_threads);
    }
    for (auto& worker : workers) {
        if (worker.thread.joinable()) {
            worker.thread.join();
        }
    }
}

std::uint16_t LiveDashIngestServer::bound_port() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->bound_port;
}

bool LiveDashIngestServer::wait_for_tracks(std::chrono::milliseconds first_track_timeout,
                                           std::chrono::milliseconds settle_timeout) {
    return impl_->session.wait_for_tracks(first_track_timeout, settle_timeout);
}

LiveObjectSource LiveDashIngestServer::source() {
    return impl_->session.source();
}

void LiveDashIngestServer::Impl::accept_loop() {
#if !defined(_WIN32)
    // shutdown() on a listening socket does not wake a blocked accept() on
    // macOS/BSD (it fails with ENOTCONN), so never park in accept(): poll with
    // a bounded timeout and re-check stop_requested, mirroring the 100ms
    // receive cadence used by the client workers.
    constexpr int kAcceptPollTimeoutMsec = 100;
    while (true) {
        if (stop_requested_now()) {
            break;
        }
        pollfd poll_fd{};
        poll_fd.fd = listen_fd;
        poll_fd.events = POLLIN;
        const int ready = ::poll(&poll_fd, 1, kAcceptPollTimeoutMsec);
        if (ready <= 0) {
            continue;
        }
        int client_fd = ::accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            std::lock_guard<std::mutex> lock(mutex);
            if (stop_requested) {
                break;
            }
            continue;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (stop_requested) {
            close_fd(client_fd);
            break;
        }
        reap_finished_workers_locked();
        active_client_fds.insert(client_fd);
        auto done = std::make_shared<std::atomic_bool>(false);
        worker_threads.push_back(WorkerThread{
            .thread = std::thread([this, client_fd, done]() {
                // Last-resort guard so no exception can escape the thread and
                // abort the process; handle_client already reports and closes
                // on its own error paths.
                try {
                    handle_client(client_fd);
                } catch (const std::exception& error) {
                    std::cerr << "[dash-ingest] client handler error: " << error.what() << std::endl;
                } catch (...) {
                    std::cerr << "[dash-ingest] client handler error: unknown exception" << std::endl;
                }
                done->store(true, std::memory_order_release);
            }),
            .done = done,
        });
    }
#endif
}

void LiveDashIngestServer::Impl::reap_finished_workers_locked() {
    auto worker = worker_threads.begin();
    while (worker != worker_threads.end()) {
        if (!worker->done->load(std::memory_order_acquire)) {
            ++worker;
            continue;
        }
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
        worker = worker_threads.erase(worker);
    }
}

void LiveDashIngestServer::Impl::shutdown_active_clients_locked() {
#if !defined(_WIN32)
    for (const int client_fd : active_client_fds) {
        if (client_fd >= 0) {
            static_cast<void>(::shutdown(client_fd, SHUT_RDWR));
        }
    }
#endif
}

bool LiveDashIngestServer::Impl::stop_requested_now() const {
    std::lock_guard<std::mutex> lock(mutex);
    return stop_requested;
}

void LiveDashIngestServer::Impl::handle_client(int client_fd) {
#if defined(_WIN32)
    close_fd(client_fd);
#else
    auto close_client = [&]() {
        // Deregister before closing: otherwise the accept loop could reuse this
        // fd number for a new client and this erase would drop that client's
        // registration, so stop() would never shut it down.
        {
            std::lock_guard<std::mutex> lock(mutex);
            active_client_fds.erase(client_fd);
        }
        close_fd(client_fd);
    };
    struct ReceiveResult {
        ssize_t count;
        bool stopped;
    };
    const auto recv_or_stop = [&](std::span<std::uint8_t> destination) -> ReceiveResult {
        while (true) {
            const ssize_t count = ::recv(client_fd, destination.data(), destination.size(), 0);
            if (count >= 0) {
                return ReceiveResult{.count = count, .stopped = false};
            }
            const int recv_errno = errno;
            if (recv_errno == EINTR) {
                continue;
            }
            if (recv_errno == EAGAIN || recv_errno == EWOULDBLOCK) {
                if (!stop_requested_now()) {
                    continue;
                }
                return ReceiveResult{.count = 0, .stopped = true};
            }
            return ReceiveResult{.count = -1, .stopped = false};
        }
    };
    auto finish = [&](std::string_view status) {
        const std::string response = std::string("HTTP/1.1 ") + std::string(status) +
                                     "\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        static_cast<void>(send_all(client_fd, response));
        close_client();
    };

    // BSD-derived systems hand accepted sockets the listener's O_NONBLOCK;
    // clear it so the SO_RCVTIMEO cadence below governs the receive loop
    // instead of a hot EAGAIN spin.
    const int client_flags = ::fcntl(client_fd, F_GETFL, 0);
    if (client_flags >= 0 && (client_flags & O_NONBLOCK) != 0) {
        static_cast<void>(::fcntl(client_fd, F_SETFL, client_flags & ~O_NONBLOCK));
    }

    constexpr suseconds_t kReceiveTimeoutUsec = 100 * 1000;
    timeval receive_timeout{};
    receive_timeout.tv_sec = 0;
    receive_timeout.tv_usec = kReceiveTimeoutUsec;
    static_cast<void>(::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                                   sizeof(receive_timeout)));

    std::vector<std::uint8_t> received;
    std::array<std::uint8_t, 4096> buffer{};
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const ReceiveResult received_chunk = recv_or_stop(buffer);
        if (received_chunk.count <= 0) {
            if (received_chunk.stopped) {
                close_client();
            } else {
                finish("400 Bad Request");
            }
            return;
        }
        received.insert(received.end(), buffer.begin(), buffer.begin() + received_chunk.count);
        const std::string view(received.begin(), received.end());
        header_end = view.find("\r\n\r\n");
        if (received.size() > 64 * 1024) {
            finish("400 Bad Request");
            return;
        }
    }

    const std::string header_text(received.begin(), received.begin() + static_cast<std::ptrdiff_t>(header_end + 4));
    std::optional<ParsedRequest> parsed = parse_request_headers(header_text);
    if (!parsed.has_value()) {
        finish("400 Bad Request");
        return;
    }
    if (parsed->method != "PUT" && parsed->method != "POST") {
        finish("405 Method Not Allowed");
        return;
    }
    if (!path_matches_prefix(parsed->path, config.path_prefix)) {
        finish("404 Not Found");
        return;
    }
    const auto transfer_encoding = parsed->headers.find("transfer-encoding");
    if (transfer_encoding == parsed->headers.end() ||
        lower_ascii(transfer_encoding->second).find("chunked") == std::string::npos) {
        finish("400 Bad Request");
        return;
    }

    ChunkedBodyDecoder decoder(config.max_chunk_size);
    auto feed_decoder = [&](std::span<const std::uint8_t> bytes) -> bool {
        decoder.append(bytes);
        std::vector<std::uint8_t> decoded = decoder.take_decoded();
        if (!decoded.empty()) {
            // Never let a parse error from client-controlled bytes escape the
            // worker thread: an uncaught exception here would call std::terminate
            // and take down the whole publisher.
            try {
                session.ingest(parsed->path, decoded);
            } catch (const std::exception& error) {
                std::cerr << "[dash-ingest] ingest error on '" << parsed->path
                          << "': " << error.what() << std::endl;
                return false;
            }
        }
        return !decoder.failed();
    };

    if (header_end + 4 < received.size()) {
        if (!feed_decoder(std::span<const std::uint8_t>(received.data() + header_end + 4,
                                                        received.size() - header_end - 4))) {
            finish("400 Bad Request");
            return;
        }
    }
    while (!decoder.complete()) {
        const ReceiveResult received_chunk = recv_or_stop(buffer);
        if (received_chunk.count <= 0) {
            if (received_chunk.stopped) {
                close_client();
            } else {
                finish("400 Bad Request");
            }
            return;
        }
        if (!feed_decoder(
                std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(received_chunk.count)))) {
            finish("400 Bad Request");
            return;
        }
    }
    finish("204 No Content");
#endif
}

}  // namespace openmoq::publisher
