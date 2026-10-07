#include "openmoq/publisher/live_srt_ingest.h"
#include "openmoq/publisher/transport/moqt_session.h"
#include "openmoq/publisher/transport/moqt_control_messages.h"

#include <srt/srt.h>

#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <future>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
using openmoq::publisher::LiveSrtCallerRuntimeConfig;
using openmoq::publisher::LiveSrtIngestManager;

namespace {
using openmoq::publisher::DraftVersion;
using openmoq::publisher::transport::ConnectionState;
using openmoq::publisher::transport::EndpointConfig;
using openmoq::publisher::transport::LiveIngestOptions;
using openmoq::publisher::transport::LiveSrtCallerOptions;
using openmoq::publisher::transport::MoqtSession;
using openmoq::publisher::transport::PublisherTransport;
using openmoq::publisher::transport::StreamDirection;
using openmoq::publisher::transport::TlsConfig;
using openmoq::publisher::transport::TransportStatus;

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

sockaddr_in reserve_address(SRTSOCKET& socket) {
    socket = srt_create_socket();
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (srt_bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SRT_ERROR) {
        throw std::runtime_error("could not reserve loopback port");
    }
    int size = sizeof(address);
    if (srt_getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) == SRT_ERROR) {
        throw std::runtime_error("could not read loopback port");
    }
    return address;
}

LiveSrtCallerRuntimeConfig listener(const sockaddr_in& address) {
    LiveSrtCallerRuntimeConfig config;
    config.id = "test";
    config.mode = "listener";
    config.endpoint = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    return config;
}

bool delayed_connection_gets_full_discovery_window() {
    srt_startup();
    SRTSOCKET reservation;
    auto address = reserve_address(reservation);
    srt_close(reservation);
    srt_cleanup();
    std::atomic<bool> stop{false};
    LiveSrtCallerRuntimeConfig caller;
    caller.id = "failed-caller";
    caller.endpoint = "invalid-endpoint";
    LiveSrtIngestManager manager({caller, listener(address)}, [](auto&&) {}, stop);
    auto started = std::async(std::launch::async, [&] { return manager.start(); });
    bool ok = expect(started.wait_for(6s) == std::future_status::timeout,
                     "listener must keep waiting for an encoder beyond five seconds");
    const auto sender = srt_create_socket();
    const auto connected_at = std::chrono::steady_clock::now();
    const bool connected = srt_connect(sender, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != SRT_ERROR;
    ok &= expect(connected, "delayed encoder must connect");
    if (connected) {
        ok &= expect(started.wait_for(4s) == std::future_status::timeout,
                     "accepted listener must get a fresh discovery window");
        ok &= expect(started.wait_for(3s) == std::future_status::ready,
                     "silent encoder must exhaust discovery timeout");
        ok &= expect(std::chrono::steady_clock::now() - connected_at >= 5s,
                     "discovery must allow five seconds after accept");
    }
    stop.store(true);
    started.get();
    srt_close(sender);
    manager.join();
    return ok;
}

bool waiting_listener_stops_promptly() {
    srt_startup();
    SRTSOCKET reservation;
    const auto address = reserve_address(reservation);
    srt_close(reservation);
    srt_cleanup();
    std::atomic<bool> stop{false};
    LiveSrtIngestManager manager({listener(address)}, [](auto&&) {}, stop);
    auto started = std::async(std::launch::async, [&] { return manager.start(); });
    bool ok = expect(started.wait_for(300ms) == std::future_status::timeout,
                     "listener should wait before shutdown");
    stop.store(true);
    ok &= expect(started.wait_for(1s) == std::future_status::ready,
                 "shutdown must interrupt waiting for an encoder");
    started.get();
    manager.join();
    return ok;
}

bool listener_setup_failure_finishes_discovery() {
    std::atomic<bool> stop{false};
    LiveSrtCallerRuntimeConfig config;
    config.mode = "listener";
    config.endpoint = "invalid-endpoint";
    LiveSrtIngestManager manager({config}, [](auto&&) {}, stop);
    auto started = std::async(std::launch::async, [&] { return manager.start(); });
    const bool ok = expect(started.wait_for(1s) == std::future_status::ready,
                           "failed listener must not wait for an impossible connection");
    stop.store(true);
    started.get();
    manager.join();
    return ok;
}

bool caller_retains_startup_discovery_window() {
    std::atomic<bool> stop{false};
    LiveSrtCallerRuntimeConfig config;
    config.endpoint = "invalid-endpoint";
    LiveSrtIngestManager manager({config}, [](auto&&) {}, stop);
    auto started = std::async(std::launch::async, [&] { return manager.start(); });
    bool ok = expect(started.wait_for(4s) == std::future_status::timeout,
                     "caller must retain its startup discovery window");
    ok &= expect(started.wait_for(2s) == std::future_status::ready,
                 "caller discovery must remain bounded without a connection");
    stop.store(true);
    started.get();
    manager.join();
    return ok;
}

// The ingest is a real SRT listener. This peer supplies deterministic MoQT
// requests so the test observes the SRT-specific session routing directly.
class DiscoveryPeer : public PublisherTransport {
public:
    struct Write { std::uint64_t stream; std::vector<std::uint8_t> bytes; };
    std::map<std::uint64_t, std::deque<std::vector<std::uint8_t>>> reads;
    std::set<std::uint64_t> accepted;
    std::vector<Write> writes;
    std::uint64_t close_code = 0;
    std::uint64_t next_bidi = 0;
    std::uint64_t next_uni = 2;
    ConnectionState connection_state = ConnectionState::kIdle;
    TransportStatus configure(const EndpointConfig&, const TlsConfig&) override { return TransportStatus::success(); }
    TransportStatus connect() override { connection_state = ConnectionState::kConnected; return TransportStatus::success(); }
    ConnectionState state() const override { return connection_state; }
    TransportStatus open_stream(StreamDirection direction, std::uint64_t& id) override {
        auto& next = direction == StreamDirection::kBidirectional ? next_bidi : next_uni;
        id = next; next += 4;
        return TransportStatus::success();
    }
    TransportStatus accept_stream(StreamDirection direction, std::uint64_t& id, std::chrono::milliseconds) override {
        for (const auto& [candidate, chunks] : reads) {
            const auto low_bits = direction == StreamDirection::kBidirectional ? 1ULL : 3ULL;
            if (!chunks.empty() && (candidate & 3) == low_bits && accepted.insert(candidate).second) {
                id = candidate;
                return TransportStatus::success();
            }
        }
        return TransportStatus::failure("timed out waiting for stream data");
    }
    TransportStatus write_stream(std::uint64_t id, std::span<const std::uint8_t> bytes, bool) override {
        writes.push_back({id, {bytes.begin(), bytes.end()}});
        if (!bytes.empty() && bytes[0] == 0x1d) {
            // PUBLISH_OK: no parameters, Forward defaults to 1.
            reads[id].push_back({0x07, 0, 1, 0});
        }
        return TransportStatus::success();
    }
    TransportStatus read_stream(std::uint64_t id, std::vector<std::uint8_t>& bytes,
                                bool& fin, std::chrono::milliseconds) override {
        auto it = reads.find(id);
        if (it == reads.end() || it->second.empty())
            return TransportStatus::failure("timed out waiting for stream data");
        bytes = std::move(it->second.front());
        it->second.pop_front();
        fin = false;
        return TransportStatus::success();
    }
    TransportStatus reset_stream(std::uint64_t, std::uint64_t) override { return TransportStatus::success(); }
    std::string connection_id() const override { return "SRT-discovery-peer"; }
    TransportStatus close(std::uint64_t code) override { close_code = code; return TransportStatus::success(); }
};

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (const char c : value) quoted += c == '\'' ? "'\\''" : std::string(1, c);
    return quoted + "'";
}

bool srt_session_discovery(const std::filesystem::path& media, bool namespace_update,
                           bool unknown_alias = false) {
    srt_startup();
    SRTSOCKET reservation;
    const auto address = reserve_address(reservation);
    srt_close(reservation);
    srt_cleanup();
    DiscoveryPeer peer;
    peer.reads[3].push_back(openmoq::publisher::transport::encode_setup_message({
        .draft = DraftVersion::kDraft22,
        .transport = openmoq::publisher::transport::TransportKind::kWebTransport,
        .max_request_id = 100,
    }));
    peer.reads[0].push_back({0x07, 0, 1, 0});
    if (namespace_update) {
        // SUBSCRIBE_NAMESPACE(media), then forbidden FORWARD in its update.
        peer.reads[1].push_back({0x50, 0, 9, 1, 1, 5, 'm', 'e', 'd', 'i', 'a', 0});
    } else {
        // SUBSCRIBE_TRACKS(media, Forward=0), then enable forwarding and
        // attempt an overlapping independent request.
        peer.reads[1].push_back({0x51, 0, 11, 1, 1, 5, 'm', 'e', 'd', 'i', 'a', 1, 0x10, 0});
        peer.reads[5].push_back({0x51, 0, 11, 5, 1, 5, 'm', 'e', 'd', 'i', 'a', 1, 0x10, 0});
        // A nonempty Range Filter was not negotiated by this session.
        peer.reads[9].push_back({0x03, 0, 23, 7, 1, 5, 'm', 'e', 'd', 'i', 'a',
                                10, 't', 'e', 's', 't', '_', 'v', 'i', 'd', 'e', 'o',
                                1, 0x25, 1, 0});
    }
    if (unknown_alias) {
        // SUBSCRIBE references an authorization-token alias never registered.
        peer.reads[5].push_back({0x03, 0, 24, 3, 1, 5, 'm', 'e', 'd', 'i', 'a',
                                10, 't', 'e', 's', 't', '_', 'v', 'i', 'd', 'e', 'o',
                                1, 0x03, 2, 0x02, 0});
    } else {
        peer.reads[1].push_back({0x02, 0, 4, 3, 1, 0x10, 1});
    }
    const auto endpoint = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    auto sender = std::async(std::launch::async, [&] {
        std::this_thread::sleep_for(300ms);
        const auto command = "ffmpeg -hide_banner -loglevel error -stream_loop 2 -re -i " +
            shell_quote(media.string()) + " -c copy -f mpegts " +
            shell_quote("srt://" + endpoint + "?mode=caller&latency=20") + " >/dev/null 2>&1";
        return std::system(command.c_str());
    });
    MoqtSession session(peer, "media", false, false, false, std::chrono::seconds(1));
    bool ok = expect(session.connect(EndpointConfig{.host = "127.0.0.1", .port = 4433, .alpn = "moqt-22"}, {}).ok,
                     "SRT discovery session must connect");
    LiveSrtCallerOptions source;
    source.id = "test"; source.endpoint = endpoint; source.mode = "listener";
    LiveIngestOptions ingest;
    ingest.srt_callers.push_back(source);
    if (!namespace_update) ingest.resume_group_floor = 7;
    const auto status = session.publish_live(ingest, nullptr, DraftVersion::kDraft22, true, false);
    const int sender_result = sender.get();
    if (namespace_update) {
        ok &= expect(std::any_of(peer.writes.begin(), peer.writes.end(), [](const auto& write) {
            return write.stream == 1 && !write.bytes.empty() && write.bytes[0] == 0x08;
        }), "SRT namespace discovery must report the matching NAMESPACE");
        ok &= expect(!status.ok && peer.close_code == (unknown_alias ? 0x17 : 3),
                     unknown_alias ? "SRT subscription with an unknown token alias must close with UNKNOWN_AUTH_TOKEN_ALIAS"
                                   : "SRT namespace update with FORWARD must close with PROTOCOL_VIOLATION");
    } else {
        ok &= expect(sender_result == 0, "SRT track discovery sender must deliver its complete input");
        ok &= expect(status.ok, "SRT track discovery and update must remain successful");
        std::set<std::uint64_t> inspected_streams;
        bool saw_resumed_media = false;
        for (const auto& write : peer.writes) {
            if ((write.stream & 3) != 2 || write.bytes.empty() || !inspected_streams.insert(write.stream).second)
                continue;
            std::size_t offset = 0;
            std::uint64_t type = 0, alias = 0, group = 0;
            using openmoq::publisher::transport::decode_vi64;
            if (decode_vi64(write.bytes, offset, type) && (type & 0x90) == 0x10 &&
                decode_vi64(write.bytes, offset, alias) && alias == 1 &&
                decode_vi64(write.bytes, offset, group)) {
                saw_resumed_media = true;
                ok &= expect(group >= 7, "SRT resumed media must preserve the new group floor");
            }
        }
        ok &= expect(saw_resumed_media, "SRT discovery must deliver media with the migration group floor");
        ok &= expect(std::count_if(peer.writes.begin(), peer.writes.end(), [](const auto& write) {
            return !write.bytes.empty() && write.bytes[0] == 0x1d;
        }) == 2, "SRT discovery update must publish catalog and media tracks exactly once");
        ok &= expect(std::any_of(peer.writes.begin(), peer.writes.end(), [](const auto& write) {
            return write.stream == 5 && write.bytes.size() >= 4 && write.bytes[0] == 0x05 && write.bytes[3] == 0x30;
        }), "SRT overlapping track discovery must receive PREFIX_OVERLAP");
        ok &= expect(std::any_of(peer.writes.begin(), peer.writes.end(), [](const auto& write) {
            return write.stream == 9 && write.bytes.size() >= 4 && write.bytes[0] == 0x05 && write.bytes[3] == 0x36;
        }), "SRT subscription with an unnegotiated Range Filter must receive INVALID_FILTER");
        ok &= expect(std::count_if(peer.writes.begin(), peer.writes.end(), [](const auto& write) {
            return write.stream == 1 && !write.bytes.empty() && write.bytes[0] == 0x07;
        }) == 2, "SRT discovery request and update must each receive one response");
    }
    return ok;
}

bool srt_session_discovery_regressions() {
    if (std::system("ffmpeg -version >/dev/null 2>&1") != 0) {
        std::cerr << "SKIP: SRT session discovery regression needs the ffmpeg executable\n";
        return true;
    }
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures/locmaf-publisher.mp4";
    const auto media = std::filesystem::temp_directory_path() /
        ("moqxr-srt-discovery-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ts");
    const auto convert = "ffmpeg -hide_banner -loglevel error -i " + shell_quote(fixture.string()) +
        " -c copy -f mpegts " + shell_quote(media.string()) + " -y";
    if (!expect(std::system(convert.c_str()) == 0, "SRT discovery MPEG-TS fixture conversion must succeed")) return false;
    bool ok = srt_session_discovery(media, true);
    ok &= srt_session_discovery(media, false);
    ok &= srt_session_discovery(media, true, true);
    std::filesystem::remove(media);
    return ok;
}
}  // namespace

int main() {
    bool ok = delayed_connection_gets_full_discovery_window();
    ok &= waiting_listener_stops_promptly();
    ok &= listener_setup_failure_finishes_discovery();
    ok &= caller_retains_startup_discovery_window();
    ok &= srt_session_discovery_regressions();
    return ok ? 0 : 1;
}
