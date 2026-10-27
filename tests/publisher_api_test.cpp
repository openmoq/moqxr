#include "openmoq/publisher/publisher_api.h"
#include "openmoq/publisher/endpoint_failover.h"
#include "openmoq/publisher/mp4_box.h"
#include "openmoq/publisher/transport/moqt_control_messages.h"

#include <algorithm>
#include <atomic>
#include <set>
#include <thread>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

using openmoq::publisher::DraftVersion;
using openmoq::publisher::EndBroadcastMode;
using openmoq::publisher::LiveCatalogMode;
using openmoq::publisher::LiveObject;
using openmoq::publisher::LiveObjectSource;
using openmoq::publisher::LiveTrack;
using openmoq::publisher::PreparedPublish;
using openmoq::publisher::PublishPlan;
using openmoq::publisher::Publisher;
using openmoq::publisher::PublisherConfig;
using openmoq::publisher::draft_profile;
using openmoq::publisher::transport::ConnectionState;
using openmoq::publisher::transport::EndpointConfig;
using openmoq::publisher::transport::FailureKind;
using openmoq::publisher::transport::PublisherTransport;
using openmoq::publisher::transport::StreamDirection;
using openmoq::publisher::transport::TlsConfig;
using openmoq::publisher::transport::TransportKind;
using openmoq::publisher::transport::TransportStatus;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

struct MockTransport final : PublisherTransport {
    struct State {
        EndpointConfig configured_endpoint;
        TlsConfig configured_tls;
        bool configure_called = false;
    };

    explicit MockTransport(std::shared_ptr<State> state) : shared_state(std::move(state)) {}

    std::shared_ptr<State> shared_state;
    std::string connect_error = "mock connect failure";

    TransportStatus configure(const EndpointConfig& endpoint, const TlsConfig& tls) override {
        shared_state->configured_endpoint = endpoint;
        shared_state->configured_tls = tls;
        shared_state->configure_called = true;
        return TransportStatus::success();
    }

    TransportStatus connect() override {
        return TransportStatus::failure(connect_error);
    }

    ConnectionState state() const override {
        return ConnectionState::kIdle;
    }

    TransportStatus open_stream(StreamDirection, std::uint64_t&) override {
        return TransportStatus::failure("not implemented");
    }

    TransportStatus accept_stream(StreamDirection, std::uint64_t&, std::chrono::milliseconds) override {
        return TransportStatus::failure("not implemented");
    }

    TransportStatus write_stream(std::uint64_t, std::span<const std::uint8_t>, bool) override {
        return TransportStatus::failure("not implemented");
    }

    TransportStatus read_stream(std::uint64_t, std::vector<std::uint8_t>&, bool&, std::chrono::milliseconds) override {
        return TransportStatus::failure("not implemented");
    }

    TransportStatus reset_stream(std::uint64_t, std::uint64_t) override {
        return TransportStatus::failure("not implemented");
    }

    std::string connection_id() const override {
        return "mock";
    }

    TransportStatus close(std::uint64_t) override {
        return TransportStatus::success();
    }
};

struct MediaInputGate {
    std::mutex mutex;
    std::condition_variable condition;
    bool released = false;
};

class StagedMediaInputBuffer final : public std::streambuf {
public:
    StagedMediaInputBuffer(std::vector<std::uint8_t> init, std::vector<std::uint8_t> first,
                          std::vector<std::uint8_t> remaining, std::shared_ptr<MediaInputGate> gate)
        : chunks_{std::move(init),std::move(first),std::move(remaining)},gate_(std::move(gate)) {}
protected:
    std::streamsize xsgetn(char* output,std::streamsize count) override {
        if (chunk_ == chunks_.size()) return 0;
        if (chunk_ == 2) {
            std::unique_lock lock(gate_->mutex);
            gate_->condition.wait(lock,[&] { return gate_->released; });
        }
        const auto& bytes=chunks_[chunk_++];
        const auto copied=std::min<std::size_t>(bytes.size(),static_cast<std::size_t>(count));
        std::memcpy(output,bytes.data(),copied);
        return static_cast<std::streamsize>(copied);
    }
private:
    std::vector<std::vector<std::uint8_t>> chunks_;
    std::size_t chunk_=0;
    std::shared_ptr<MediaInputGate> gate_;
};

struct MigrationTransport final : PublisherTransport {
    struct Record {
        EndpointConfig endpoint;
        TlsConfig tls;
        std::atomic<bool> closed{false};
        std::atomic<int> close_calls{0};
        std::atomic<std::uint64_t> close_code{0};
        std::atomic<std::uint64_t> first_close_code{0};
        std::atomic<std::size_t> writes_after_close{0};
        std::vector<std::uint8_t> written_bytes;
        std::vector<std::uint64_t> media_groups;
    };
    explicit MigrationTransport(std::shared_ptr<Record> record, std::optional<std::string> target)
        : record(std::move(record)), target(std::move(target)) {}
    std::shared_ptr<Record> record;
    std::optional<std::string> target;
    std::shared_ptr<std::size_t> source_reads;
    std::size_t migration_threshold = 2;
    std::set<std::uint64_t> request_responses;
    std::set<std::uint64_t> media_streams;
    std::shared_ptr<MediaInputGate> media_gate;
    // Runs just before read_stream reports the GOAWAY.
    std::function<void()> before_goaway;
    // Instead of a GOAWAY, open a request stream carrying a SUBSCRIBE whose
    // parameters do not decode (§9.20: PROTOCOL_VIOLATION).
    bool invalid_subscribe = false;
    bool invalid_subscribe_sent = false;
    static constexpr std::uint64_t kInvalidSubscribeStream = 101;
    bool setup_read = false;
    std::uint64_t next_stream = 0;
    TransportStatus configure(const EndpointConfig& endpoint, const TlsConfig& tls) override {
        record->endpoint = endpoint;
        record->tls = tls;
        return TransportStatus::success();
    }
    TransportStatus connect() override { return TransportStatus::success(); }
    ConnectionState state() const override {
        return record->closed ? ConnectionState::kClosed : ConnectionState::kConnected;
    }
    TransportStatus open_stream(StreamDirection direction, std::uint64_t& stream) override {
        stream = next_stream;
        next_stream += 4;
        if (direction == StreamDirection::kBidirectional)
            request_responses.insert(stream);
        else if (stream != 0) media_streams.insert(stream);
        return TransportStatus::success();
    }
    TransportStatus accept_stream(StreamDirection direction, std::uint64_t& stream,
                                  std::chrono::milliseconds) override {
        if (direction == StreamDirection::kUnidirectional && !setup_read) {
            stream = 3;
            return TransportStatus::success();
        }
        if (direction == StreamDirection::kBidirectional && invalid_subscribe && !invalid_subscribe_sent &&
            source_reads && *source_reads >= migration_threshold) {
            invalid_subscribe_sent = true;
            if (media_gate) {
                std::lock_guard lock(media_gate->mutex);
                media_gate->released = true;
                media_gate->condition.notify_all();
            }
            stream = kInvalidSubscribeStream;
            return TransportStatus::success();
        }
        return TransportStatus::failure("timed out waiting for stream data");
    }
    TransportStatus write_stream(std::uint64_t stream, std::span<const std::uint8_t> bytes, bool) override {
        if (record->closed) {
            ++record->writes_after_close;
        }
        if (media_gate && !bytes.empty() && media_streams.erase(stream)) {
            std::size_t offset=0;
            std::uint64_t type=0,alias=0,group=0;
            using openmoq::publisher::transport::decode_vi64;
            if (!decode_vi64(bytes,offset,type) || !decode_vi64(bytes,offset,alias) || !decode_vi64(bytes,offset,group))
                return TransportStatus::failure("malformed test media header");
            record->media_groups.push_back(group);
            if (target || invalid_subscribe) {
                *source_reads=1;

            }
        }
        record->written_bytes.insert(record->written_bytes.end(), bytes.begin(), bytes.end());
        return TransportStatus::success();
    }
    TransportStatus read_stream(std::uint64_t stream, std::vector<std::uint8_t>& bytes, bool& fin,
                                std::chrono::milliseconds) override {
        fin = false;
        if (record->closed) {
            return TransportStatus::failure("transport closed");
        }
        if (stream == 3 && !setup_read) {
            setup_read = true;
            bytes = openmoq::publisher::transport::encode_server_setup_message({.draft = DraftVersion::kDraft22});
            return TransportStatus::success();
        }
        if (stream == kInvalidSubscribeStream) {
            // SUBSCRIBE "interop"/"catalog" with one parameter of unknown
            // type 0x3E.
            const std::vector<std::uint8_t> payload{
                0x01, 0x01, 0x07, 'i', 'n', 't', 'e', 'r', 'o', 'p', 0x07, 'c', 'a', 't', 'a', 'l', 'o', 'g',
                0x01, 0x3e, 0x00};
            bytes = {0x03, 0x00, static_cast<std::uint8_t>(payload.size())};
            bytes.insert(bytes.end(), payload.begin(), payload.end());
            return TransportStatus::success();
        }
        if (source_reads) {
            if (request_responses.erase(stream)) {
                bytes = openmoq::publisher::transport::encode_request_ok_message(DraftVersion::kDraft22, 0);
                return TransportStatus::success();
            }
            if (stream != 3 || !target || *source_reads < migration_threshold) {
                return TransportStatus::failure("timed out waiting for stream data");
            }
        }
        if (target) {
            if (media_gate) {
                std::lock_guard lock(media_gate->mutex);
                media_gate->released = true;
                media_gate->condition.notify_all();
            }
            if (before_goaway) {
                before_goaway();
            }
            auto status =
                TransportStatus::failure("GOAWAY migration", openmoq::publisher::transport::FailureKind::kRetryable);
            status.migration_uri = target;
            status.migration_timeout_ms = 0;
            return status;
        }
        return TransportStatus::failure("migration target reached", openmoq::publisher::transport::FailureKind::kFatal);
    }
    TransportStatus reset_stream(std::uint64_t, std::uint64_t) override { return TransportStatus::success(); }
    std::string connection_id() const override { return "migration-test"; }
    TransportStatus close(std::uint64_t application_error_code) override {
        record->close_code = application_error_code;
        if (record->close_calls++ == 0) record->first_close_code = application_error_code;
        record->closed = true;
        return TransportStatus::success();
    }
};

// A transport that completes SETUP and then has nothing to read until it is
// closed, as picoquic does while a live publish waits for its first
// subscriber. After close() a read reports the closed connection. close()
// counts its calls and holds for a moment, as picoquic's close drain does.
struct ClosingTransport final : PublisherTransport {
    struct Record {
        std::atomic<int> close_calls{0};
        std::atomic<bool> setup_read{false};
        std::atomic<bool> closed{false};
        std::atomic<std::uint64_t> close_code{0};
        std::atomic<int> transports{0};
        // While set, connect() waits for it to clear (bounded).
        std::atomic<bool> hold_connect{false};
        std::atomic<bool> connecting{false};
        std::atomic<bool> closed_during_connect{false};
    };
    explicit ClosingTransport(std::shared_ptr<Record> record) : record(std::move(record)) { ++this->record->transports; }
    std::shared_ptr<Record> record;
    std::uint64_t next_stream = 0;
    bool setup_accepted = false;
    TransportStatus configure(const EndpointConfig&, const TlsConfig&) override { return TransportStatus::success(); }
    TransportStatus connect() override {
        record->connecting = true;
        for (int i = 0; i < 2000 && record->hold_connect; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        record->connecting = false;
        return TransportStatus::success();
    }
    // A closed connection reads as kClosed, which makes a publish failure on
    // it retryable unless the publish was stopped.
    ConnectionState state() const override {
        return record->closed ? ConnectionState::kClosed : ConnectionState::kConnected;
    }
    TransportStatus open_stream(StreamDirection, std::uint64_t& stream) override {
        stream = next_stream;
        next_stream += 4;
        return TransportStatus::success();
    }
    TransportStatus accept_stream(StreamDirection direction, std::uint64_t& stream,
                                  std::chrono::milliseconds) override {
        if (direction == StreamDirection::kUnidirectional && !setup_accepted) {
            setup_accepted = true;
            stream = 3;
            return TransportStatus::success();
        }
        return idle();
    }
    TransportStatus write_stream(std::uint64_t, std::span<const std::uint8_t>, bool) override {
        return record->closed ? TransportStatus::failure("transport closed") : TransportStatus::success();
    }
    TransportStatus read_stream(std::uint64_t stream, std::vector<std::uint8_t>& bytes, bool& fin,
                                std::chrono::milliseconds) override {
        fin = false;
        if (stream == 3 && !record->setup_read) {
            bytes = openmoq::publisher::transport::encode_server_setup_message({.draft = DraftVersion::kDraft22});
            record->setup_read = true;
            return TransportStatus::success();
        }
        return idle();
    }
    TransportStatus reset_stream(std::uint64_t, std::uint64_t) override { return TransportStatus::success(); }
    std::string connection_id() const override { return "closing-test"; }
    TransportStatus close(std::uint64_t application_error_code) override {
        if (record->connecting) {
            record->closed_during_connect = true;
        }
        record->close_code = application_error_code;
        ++record->close_calls;
        record->closed = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return TransportStatus::success();
    }

private:
    TransportStatus idle() {
        if (record->closed) {
            return TransportStatus::failure("transport closed");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return TransportStatus::failure("timed out waiting for stream data");
    }
};

// disconnect() from another thread interrupts a live publish that waits for
// its first subscriber. The publish then fails on the closed transport, and
// its failure path must not close the session a second time: the transport
// frees its QUIC context in close().
bool test_disconnect_during_live_publish_closes_once() {
    bool ok = true;
    PublisherConfig config;
    config.draft_version = DraftVersion::kDraft22;
    config.track_namespace = "app";
    auto record = std::make_shared<ClosingTransport::Record>();
    Publisher publisher(config, [record](TransportKind) -> std::unique_ptr<PublisherTransport> {
        return std::make_unique<ClosingTransport>(record);
    });
    LiveObjectSource source{
        .tracks = {LiveTrack{.track_name = "catalog"}, LiveTrack{.track_name = "video"}},
        .next_object = []() -> std::optional<LiveObject> {
            return LiveObject{.track_name = "video", .payload = {0x00}};
        },
    };
    EndpointConfig endpoint;
    endpoint.transport = TransportKind::kRawQuic;
    endpoint.host = "relay.example";
    endpoint.port = 443;

    std::thread publishing([&] { static_cast<void>(publisher.publish_live_objects(source, endpoint)); });
    for (int i = 0; i < 2000 && !record->setup_read; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ok &= expect(record->setup_read.load(), "expected the live publish to complete SETUP");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    static_cast<void>(publisher.disconnect(0));
    publishing.join();
    ok &= expect(record->close_calls == 1,
                 "expected one transport close when disconnect() interrupts a live publish, got " +
                     std::to_string(record->close_calls.load()));
    return ok;
}

// A publish that disconnect() stopped is not a failure: its reads fail on the
// closed connection, which would otherwise be retryable, and retrying it
// through publish_with_failover would reconnect a publisher asked to stop.
bool test_disconnect_during_publish_is_not_retried() {
    bool ok = true;
    PublisherConfig config;
    config.draft_version = DraftVersion::kDraft22;
    config.forward = false;
    config.subscriber_timeout = std::chrono::seconds(30);
    std::vector<std::shared_ptr<MigrationTransport::Record>> records;
    Publisher publisher(config, [&](TransportKind) -> std::unique_ptr<PublisherTransport> {
        auto record = std::make_shared<MigrationTransport::Record>();
        records.push_back(record);
        auto transport = std::make_unique<MigrationTransport>(record, std::nullopt);
        transport->source_reads = std::make_shared<std::size_t>(0);
        return transport;
    });
    const PreparedPublish prepared = publisher.prepare_file(
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / "locmaf-publisher.mp4");
    EndpointConfig endpoint;
    endpoint.host = "relay.example";
    endpoint.port = 443;
    const std::vector<EndpointConfig> endpoints{endpoint, endpoint};

    std::atomic<std::size_t> attempts{0};
    TransportStatus status = TransportStatus::failure("not run");
    std::thread publishing([&] {
        status = openmoq::publisher::publish_with_failover(
            endpoints, 3,
            [&](const EndpointConfig& attempt_endpoint) {
                ++attempts;
                return publisher.publish(prepared, attempt_endpoint);
            },
            [](std::chrono::milliseconds) {});
    });
    // The publish waits for a SUBSCRIBE that never comes.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ok &= expect(attempts == 1, "expected the publish to be waiting for a subscriber");
    static_cast<void>(publisher.disconnect(0));
    publishing.join();
    ok &= expect(status.ok, "expected a disconnect()-stopped publish to report success: " + status.message);
    ok &= expect(attempts == 1 && records.size() == 1,
                 "expected no retry or failover after disconnect(), got attempts=" +
                     std::to_string(attempts.load()) + " transports=" + std::to_string(records.size()));
    ok &= expect(!records.empty() && records[0]->close_calls == 1 && records[0]->close_code == 0,
                 "expected one transport close with code 0");
    return ok;
}

// disconnect() while connect() is still running must not close the transport
// under it (the QUIC context would be freed mid-handshake). The publish closes
// the session itself once connect() returns, and publishes nothing.
bool test_disconnect_during_connect_waits_for_connect() {
    bool ok = true;
    PublisherConfig config;
    config.draft_version = DraftVersion::kDraft22;
    config.track_namespace = "app";
    auto record = std::make_shared<ClosingTransport::Record>();
    record->hold_connect = true;
    Publisher publisher(config, [record](TransportKind) -> std::unique_ptr<PublisherTransport> {
        return std::make_unique<ClosingTransport>(record);
    });
    LiveObjectSource source{
        .tracks = {LiveTrack{.track_name = "catalog"}, LiveTrack{.track_name = "video"}},
        .next_object = []() -> std::optional<LiveObject> { return std::nullopt; },
        .is_finished = []() { return false; },
    };
    EndpointConfig endpoint;
    endpoint.host = "relay.example";
    endpoint.port = 443;

    TransportStatus status = TransportStatus::failure("not run");
    std::thread publishing([&] { status = publisher.publish_live_objects(source, endpoint); });
    for (int i = 0; i < 2000 && !record->connecting; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ok &= expect(record->connecting.load(), "expected the publish to be connecting");
    ok &= expect(publisher.disconnect(0).ok, "expected disconnect() during connect to succeed");
    record->hold_connect = false;
    publishing.join();
    ok &= expect(status.ok, "expected the stopped publish to report success: " + status.message);
    ok &= expect(!record->closed_during_connect, "expected no transport close while connect() runs");
    ok &= expect(record->close_calls == 1 && record->close_code == 0,
                 "expected the publish to close the transport once with code 0 after connect");
    ok &= expect(!record->setup_read, "expected no MoQT SETUP after a stop during connect");
    return ok;
}

// A GOAWAY that arrives as disconnect() closes the session must not start a
// migration to a new connection.
bool test_disconnect_does_not_follow_goaway() {
    bool ok = true;
    PublisherConfig config;
    config.draft_version = DraftVersion::kDraft22;
    std::vector<std::shared_ptr<MigrationTransport::Record>> records;
    Publisher* publisher_ptr = nullptr;
    Publisher publisher(config, [&](TransportKind) -> std::unique_ptr<PublisherTransport> {
        auto record = std::make_shared<MigrationTransport::Record>();
        records.push_back(record);
        auto transport = std::make_unique<MigrationTransport>(record, std::optional<std::string>("moqt://next.example/new"));
        transport->before_goaway = [&] { std::thread([&] { static_cast<void>(publisher_ptr->disconnect(0)); }).join(); };
        return transport;
    });
    publisher_ptr = &publisher;
    PreparedPublish prepared;
    prepared.plan.draft = draft_profile(DraftVersion::kDraft22);
    EndpointConfig endpoint;
    endpoint.host = "first.example";
    endpoint.port = 443;
    const auto status = publisher.publish(prepared, endpoint);
    ok &= expect(status.ok, "expected a stopped publish to report success: " + status.message);
    ok &= expect(records.size() == 1, "expected no migration reconnect after disconnect(), got " +
                                          std::to_string(records.size()) + " transports");
    ok &= expect(!records.empty() && records[0]->close_calls == 1 && records[0]->close_code == 0,
                 "expected the original connection closed once with code 0");
    return ok;
}

#if !defined(_WIN32)
// A live stdin publish fed by a pipe that stays open but delivers no data, as
// an idle encoder FIFO does. disconnect() from another thread must end the
// publish promptly, close with code 0 and write nothing after the close (no
// PUBLISH_DONE or other wind-down). Covers the wait for ftyp+moov and, with
// an init segment written, the forward and await-subscribe publish loops.
bool test_disconnect_stops_live_stdin_on_idle_input(bool send_init, bool forward) {
    const std::string label = std::string("idle stdin (") + (send_init ? "after init" : "before init") +
                              (forward ? ", forward" : ", await-subscribe") + "): ";
    std::vector<std::uint8_t> init;
    if (send_init) {
        const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "locmaf-publisher.mp4";
        std::ifstream file(path, std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
        for (const auto& box : openmoq::publisher::parse_mp4_boxes(bytes)) {
            if (box.type == "ftyp" || box.type == "moov") {
                init.insert(init.end(), bytes.begin() + box.span.offset,
                            bytes.begin() + box.span.offset + box.span.size);
            }
        }
        if (!expect(!init.empty() && init.size() < 16384, label + "fixture init segment fits a pipe buffer")) {
            return false;
        }
    }

    int fds[2];
    if (!expect(::pipe(fds) == 0, label + "pipe created")) {
        return false;
    }
    const int saved_stdin = ::dup(STDIN_FILENO);
    ::dup2(fds[0], STDIN_FILENO);
    ::close(fds[0]);
    if (!init.empty()) {
        static_cast<void>(::write(fds[1], init.data(), init.size()));
    }

    PublisherConfig config;
    config.draft_version = DraftVersion::kDraft22;
    config.forward = forward;
    config.subscriber_timeout = std::chrono::seconds(30);
    std::vector<std::shared_ptr<MigrationTransport::Record>> records;
    Publisher publisher(config, [&](TransportKind) -> std::unique_ptr<PublisherTransport> {
        auto record = std::make_shared<MigrationTransport::Record>();
        records.push_back(record);
        auto transport = std::make_unique<MigrationTransport>(record, std::nullopt);
        transport->source_reads = std::make_shared<std::size_t>(0);
        return transport;
    });
    EndpointConfig endpoint;
    endpoint.host = "relay.example";
    endpoint.port = 443;

    std::atomic<bool> finished{false};
    TransportStatus status = TransportStatus::failure("not run");
    std::thread publishing([&] {
        status = publisher.publish_live(std::cin, endpoint);
        finished = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    bool ok = expect(!finished, label + "expected the publish to wait for input");
    const auto stop_started = std::chrono::steady_clock::now();
    static_cast<void>(publisher.disconnect(0));
    for (int i = 0; i < 2000 && !finished; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_started;
    ok &= expect(finished.load(), label + "expected publish_live to return within 2 s of disconnect()");
    if (!finished) {
        // Unblock the reader so the test can finish and report.
        ::close(fds[1]);
        fds[1] = -1;
    }
    publishing.join();
    if (fds[1] >= 0) {
        ::close(fds[1]);
    }
    ::dup2(saved_stdin, STDIN_FILENO);
    ::close(saved_stdin);

    ok &= expect(stop_elapsed < std::chrono::seconds(1),
                 label + "expected the stop to take under 1 s, took " +
                     std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(stop_elapsed).count()) +
                     " ms");
    ok &= expect(status.ok, label + "expected a stopped publish to report success: " + status.message);
    ok &= expect(records.size() == 1 && records[0]->close_calls == 1 && records[0]->close_code == 0,
                 label + "expected one connection, closed once with code 0");
    ok &= expect(!records.empty() && records[0]->writes_after_close == 0,
                 label + "expected no writes after the close");
    return ok;
}
#endif

bool test_goaway_recreates_publisher_transport() {
    bool ok = true;
    for (const auto kind : {TransportKind::kRawQuic, TransportKind::kWebTransport}) {
        for (const auto& target :
             {std::string(),
              kind == TransportKind::kRawQuic ? std::string("moqt://relay-next.example:4443/moq-next?q=1")
                                              : std::string("https://relay-next.example:4443/moq-next?q=1"),
              kind == TransportKind::kRawQuic ? std::string("https://relay-next.example:4443/moq-next?q=1")
                                              : std::string("moqt://relay-next.example:4443/moq-next?q=1")}) {
            PublisherConfig config;
            config.draft_version = DraftVersion::kDraft22;
            config.authorization.setup_credential = openmoq::publisher::cat4moq::Credential{{0xde, 0xad, 0xbe, 0xef}};
            std::vector<std::shared_ptr<MigrationTransport::Record>> records;
            Publisher publisher(config, [&](TransportKind) -> std::unique_ptr<PublisherTransport> {
                auto record = std::make_shared<MigrationTransport::Record>();
                records.push_back(record);
                return std::make_unique<MigrationTransport>(record,
                                                            records.size() == 1 ? std::optional(target) : std::nullopt);
            });
            PreparedPublish prepared;
            prepared.plan.draft = draft_profile(DraftVersion::kDraft22);
            EndpointConfig endpoint;
            endpoint.transport = kind;
            endpoint.host = "relay.example";
            endpoint.port = 443;
            endpoint.sni = "original-sni";
            endpoint.path = "/moq";
            TlsConfig tls;
            tls.ca_path = "test-ca.pem";
            tls.certificate_path = "client.pem";
            tls.private_key_path = "client-key.pem";
            const auto status = publisher.publish(prepared, endpoint, tls);
            ok &= expect(!status.ok && status.message.find("migration target reached") != std::string::npos,
                         "migration reconnect reaches new transport");
            ok &= expect(records.size() == 2, "GOAWAY creates a fresh transport");
            if (records.size() != 2)
                continue;
            ok &= expect(records[0]->closed, "migration closes old connection");
            ok &= expect(records[1]->endpoint.host == (target.empty() ? endpoint.host : "relay-next.example") &&
                             records[1]->endpoint.port == (target.empty() ? 443 : 4443) &&
                             records[1]->endpoint.path == (target.empty() ? "/moq" : "/moq-next?q=1") &&
                             records[1]->endpoint.sni == (target.empty() ? "original-sni" : "relay-next.example"),
                         "GOAWAY resolves host port path and TLS SNI");
            ok &= expect(records[1]->tls.ca_path == tls.ca_path &&
                             records[1]->tls.private_key_path == tls.private_key_path &&
                             records[1]->endpoint.alpn == (target.empty()                   ? records[0]->endpoint.alpn
                                                           : target.starts_with("https://") ? "h3"
                                                                                            : "moqt-22"),
                         "migration retains TLS client credentials and protocol offer");
            ok &= expect(records[1]->endpoint.transport == (target.empty() ? kind
                                                            : target.starts_with("https://")
                                                                ? TransportKind::kWebTransport
                                                                : TransportKind::kRawQuic),
                         "migration selects transport required by supplied URI");
            const std::vector<std::uint8_t> credential{0xde, 0xad, 0xbe, 0xef};
            for (const auto& record : records) {
                ok &= expect(std::search(record->written_bytes.begin(), record->written_bytes.end(), credential.begin(),
                                         credential.end()) != record->written_bytes.end(),
                             "migration preserves the SETUP authorization credential");
            }
        }
    }
    return ok;
}

bool test_goaway_migration_bound_and_uri_validation() {
    bool ok = true;
    for (const auto& target :
         {std::string("moqt://[::1]/next"), std::string("ftp://other.example/"), std::string("moqt://host:bad/"),
          std::string("moqt://user@host/"), std::string("moqt://loop.example/next")}) {
        PublisherConfig config;
        config.draft_version = DraftVersion::kDraft22;
        std::vector<std::shared_ptr<MigrationTransport::Record>> records;
        const bool loop = target.find("loop.example") != std::string::npos;
        Publisher publisher(config, [&](TransportKind) -> std::unique_ptr<PublisherTransport> {
            auto record = std::make_shared<MigrationTransport::Record>();
            records.push_back(record);
            return std::make_unique<MigrationTransport>(record, records.size() == 1 || loop ? std::optional(target)
                                                                                            : std::nullopt);
        });
        PreparedPublish prepared;
        prepared.plan.draft = draft_profile(DraftVersion::kDraft22);
        EndpointConfig endpoint;
        endpoint.host = "first.example";
        endpoint.port = 443;
        const auto status = publisher.publish(prepared, endpoint);
        if (loop) {
            ok &=
                expect(!status.ok && records.size() == 9 && status.message.find("migration limit") != std::string::npos,
                       "GOAWAY migration loops stop after eight redirects");
        } else if (target.find("[::1]") != std::string::npos) {
            ok &= expect(records.size() == 2 && records.back()->endpoint.host == "::1" &&
                             records.back()->endpoint.port == 443 && records.back()->endpoint.path == "/next",
                         "GOAWAY resolves IPv6 URI and default port");
        } else {
            ok &= expect(!status.ok && records.size() == 1 &&
                             status.failure_kind == openmoq::publisher::transport::FailureKind::kFatal &&
                             status.message.find("migration URI") != std::string::npos,
                         "invalid or incompatible GOAWAY URI fails clearly");
        }
    }
    return ok;
}

bool test_live_source_goaway_continues_and_replays_catalog(bool with_delta = false) {
    PublisherConfig config;
    config.draft_version = DraftVersion::kDraft22;
    config.forward = true;
    auto reads = std::make_shared<std::size_t>(0);
    std::vector<std::shared_ptr<MigrationTransport::Record>> records;
    Publisher publisher(config, [&](TransportKind) -> std::unique_ptr<PublisherTransport> {
        auto record = std::make_shared<MigrationTransport::Record>();
        records.push_back(record);
        auto transport = std::make_unique<MigrationTransport>(
            record, records.size() == 1 ? std::optional<std::string>("moqt://next.example:4443/new") : std::nullopt);
        transport->source_reads = reads;
        transport->migration_threshold = with_delta ? 3 : 2;
        return transport;
    });
    LiveObjectSource source;
    source.catalog_mode = LiveCatalogMode::kSourceObject;
    source.tracks = {{.track_name = "catalog"}, {.track_name = "video"}};
    source.next_object = [reads, with_delta]() -> std::optional<LiveObject> {
        if (with_delta && *reads == 1) {
            ++*reads;
            return LiveObject{
                .track_name = "catalog", .object_id = 1, .payload = {'D', 'E', 'L', 'T', 'A', 'C', 'A', 'T'}};
        }
        auto index = (*reads)++;
        if (with_delta && index > 1)
            --index;
        switch (index) {
        case 0:
            return LiveObject{.track_name = "catalog",
                              .payload = {'B', 'A', 'S', 'E', 'C', 'A', 'T'},
                              .final_in_subgroup = !with_delta};
        case 1:
            return LiveObject{.track_name = "video", .group_id = 1, .payload = {'M', '1'}};
        case 2:
            return LiveObject{.track_name = "video", .group_id = 2, .payload = {'M', '2'}};
        default:
            return std::nullopt;
        }
    };
    EndpointConfig endpoint;
    endpoint.host = "first.example";
    endpoint.port = 443;
    const auto status = publisher.publish_live_objects(source, endpoint);
    bool ok = expect(status.ok, "live source migration completes remaining objects: " + status.message);
    ok &= expect(records.size() == 2 && *reads == (with_delta ? 5 : 4),
                 "live source callback continues once without restarting");
    if (records.size() == 2) {
        const auto contains = [&](std::size_t index, std::string_view payload) {
            const auto& bytes = records[index]->written_bytes;
            return std::search(bytes.begin(), bytes.end(), payload.begin(), payload.end()) != bytes.end();
        };
        ok &= expect(contains(0, "BASECAT") && contains(0, "M1"),
                     "first session delivered source catalog and initial media");
        ok &= expect(contains(1, "BASECAT") && contains(1, "M2"),
                     "migrated session replays latest catalog and sends remaining media");
        if (with_delta) {
            ok &= expect(contains(0, "DELTACAT") && contains(1, "DELTACAT"),
                         "migration replays catalog delta after independent catalog");
            using openmoq::publisher::transport::encode_subgroup_header;
            using openmoq::publisher::transport::encode_subgroup_object;
            auto expected=encode_subgroup_header(DraftVersion::kDraft22,0,0,0,true,false,true);
            const std::vector<std::uint8_t> base={'B','A','S','E','C','A','T'};
            const std::vector<std::uint8_t> delta={'D','E','L','T','A','C','A','T'};
            const auto base_wire=encode_subgroup_object(DraftVersion::kDraft22,std::nullopt,0,base);
            const auto delta_wire=encode_subgroup_object(DraftVersion::kDraft22,0,1,delta);
            expected.insert(expected.end(),base_wire.begin(),base_wire.end());
            expected.insert(expected.end(),delta_wire.begin(),delta_wire.end());
            for (const auto& record:records) {
                ok &= expect(std::search(record->written_bytes.begin(),record->written_bytes.end(),
                                         expected.begin(),expected.end())!=record->written_bytes.end(),
                             "catalog replay preserves group subgroup object IDs and delta order");
            }

        }
    }
    return ok;
}

bool test_catalog_replay_limit_only_applies_to_migration() {
    bool ok = true;
    for (bool migrate : {false, true}) {
        PublisherConfig config;
        config.draft_version = DraftVersion::kDraft22;
        config.forward = true;
        auto reads = std::make_shared<std::size_t>(0);
        std::size_t transports = 0;
        Publisher publisher(config, [&](TransportKind) -> std::unique_ptr<PublisherTransport> {
            ++transports;
            auto transport = std::make_unique<MigrationTransport>(
                std::make_shared<MigrationTransport::Record>(),
                migrate ? std::optional<std::string>("moqt://next.example/new") : std::nullopt);
            transport->source_reads = reads;
            transport->migration_threshold = 66;
            return transport;
        });
        LiveObjectSource source;
        source.catalog_mode = LiveCatalogMode::kSourceObject;
        source.tracks = {{.track_name = "catalog"}, {.track_name = "video"}};
        source.next_object = [reads]() -> std::optional<LiveObject> {
            const auto index = (*reads)++;
            if (index < 65)
                return LiveObject{
                    .track_name = "catalog", .object_id = index, .payload = {'C'}, .final_in_subgroup = false};
            if (index == 65)
                return LiveObject{.track_name = "video", .payload = {'M'}};
            return std::nullopt;
        };
        EndpointConfig endpoint;
        endpoint.host = "first.example";
        endpoint.port = 443;
        const auto status = publisher.publish_live_objects(source, endpoint);
        ok &= expect(transports == 1 &&
                         (migrate ? !status.ok && status.message.find("fresh independent catalog") != std::string::npos
                                  : status.ok),
                     "bounded catalog history only declines migration, not ordinary publishing");
    }
    return ok;
}

bool test_live_stdin_goaway_replays_parser_residual() {
    PublisherConfig config;
    config.draft_version = DraftVersion::kDraft22;
    config.forward = true;
    config.subscriber_timeout = std::chrono::seconds(1);
    auto ready = std::make_shared<std::size_t>(2);
    std::vector<std::shared_ptr<MigrationTransport::Record>> records;
    Publisher publisher(config, [&](TransportKind) -> std::unique_ptr<PublisherTransport> {
        auto record = std::make_shared<MigrationTransport::Record>();
        records.push_back(record);
        auto transport = std::make_unique<MigrationTransport>(
            record, records.size() == 1 ? std::optional<std::string>("moqt://next.example:4443/new") : std::nullopt);
        transport->source_reads = ready;
        return transport;
    });
    const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "locmaf-publisher.mp4";
    std::ifstream input(path, std::ios::binary);
    if (!expect(input.good(), "stdin migration fixture opens"))
        return false;
    EndpointConfig endpoint;
    endpoint.host = "first.example";
    endpoint.port = 443;
    const auto status = publisher.publish_live(input, endpoint);
    bool ok = expect(status.ok, "stdin migration resumes parser and completes: " + status.message);
    ok &= expect(records.size() == 2, "stdin migration recreates session");
    if (records.size() == 2) {
        const auto& bytes = records[1]->written_bytes;
        const std::string_view marker = "moof";
        ok &= expect(std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end()) != bytes.end(),
                     "stdin migration publishes buffered CMAF media on new transport");
    }
    return ok;
}

bool test_live_stdin_migration_keeps_object_identity(std::uint64_t initial_group_id = 0) {
    const auto path=std::filesystem::path(__FILE__).parent_path()/"fixtures"/"locmaf-publisher.mp4";
    std::ifstream file(path,std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
    const auto boxes=openmoq::publisher::parse_mp4_boxes(bytes);
    std::vector<std::uint8_t> init,first,remaining;
    bool saw_first=false;
    for (const auto& box:boxes) {
        if (box.type=="ftyp" || box.type=="moov") init.insert(init.end(),bytes.begin()+box.span.offset,bytes.begin()+box.span.offset+box.span.size);
        if (box.type=="moof" || box.type=="mdat") {
            auto& destination=saw_first ? remaining : first;
            destination.insert(destination.end(),bytes.begin()+box.span.offset,bytes.begin()+box.span.offset+box.span.size);
            if (box.type=="mdat") saw_first=true;
        }
    }
    // A third independently decodable fragment remains even if an in-flight
    // second fragment is discarded while retiring the old session.
    remaining.insert(remaining.end(),first.begin(),first.end());
    const auto pad=[](std::vector<std::uint8_t>& chunk) {
        const std::size_t size=16384-chunk.size();
        chunk.insert(chunk.end(),{static_cast<std::uint8_t>(size>>24),static_cast<std::uint8_t>(size>>16),
                                 static_cast<std::uint8_t>(size>>8),static_cast<std::uint8_t>(size),'f','r','e','e'});
        chunk.resize(16384);
    };
    if (!expect(!init.empty() && !first.empty() && !remaining.empty() && init.size()<16376 && first.size()<16376 &&
                remaining.size()<16384,"staged stdin migration fixture has bounded complete chunks")) return false;
    pad(init);pad(first);
    auto gate=std::make_shared<MediaInputGate>();
    StagedMediaInputBuffer buffer(std::move(init),std::move(first),std::move(remaining),gate);
    std::istream input(&buffer);
    auto ready=std::make_shared<std::size_t>(0);
    std::vector<std::shared_ptr<MigrationTransport::Record>> records;
    PublisherConfig config;config.draft_version=DraftVersion::kDraft22;config.forward=true;
    config.subscriber_timeout=std::chrono::seconds(1);config.initial_group_id=initial_group_id;
    Publisher publisher(config,[&](TransportKind) -> std::unique_ptr<PublisherTransport> {
        auto record=std::make_shared<MigrationTransport::Record>();records.push_back(record);
        auto transport=std::make_unique<MigrationTransport>(record,records.size()==1
            ? std::optional<std::string>("moqt://next.example/new") : std::nullopt);
        transport->source_reads=ready;transport->migration_threshold=1;transport->media_gate=gate;
        return transport;
    });
    EndpointConfig endpoint;endpoint.host="first.example";endpoint.port=443;
    const auto status=publisher.publish_live(input,endpoint);
    bool ok=expect(status.ok,"stdin migration continues after delivering old media: "+status.message);
    ok &= expect(records.size()==2 && records[0]->media_groups.size()==1 && !records[1]->media_groups.empty(),
                 "migration delivers media before and after reconnection");
    if (records.size()==2 && !records[0]->media_groups.empty()) {
        for (const auto group:records[1]->media_groups)
            ok &= expect(group>records[0]->media_groups.back(),"new media cannot reuse old session group/object IDs");
    }
    if (initial_group_id != 0 && records.size()==2 && !records[0]->media_groups.empty()) {
        // MoqTransport section 2.5: the first session starts at the initial
        // Group ID, and the migrated one still resumes above its groups.
        ok &= expect(records[0]->media_groups.front()==initial_group_id,
                     "first live media group must equal the initial Group ID");
    }
    return ok;
}


// A peer SUBSCRIBE that does not decode closes the session with
// PROTOCOL_VIOLATION, as §9.20 requires. The publisher then retries the live
// publish; the retry replays the stdin input and keeps its Group IDs
// above those of the failed session (MoqTransport section 2.5) instead of
// restarting at the initial Group ID.
bool test_live_stdin_retry_after_protocol_violation_resumes(std::uint64_t initial_group_id) {
    const auto path=std::filesystem::path(__FILE__).parent_path()/"fixtures"/"locmaf-publisher.mp4";
    std::ifstream file(path,std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
    const auto boxes=openmoq::publisher::parse_mp4_boxes(bytes);
    std::vector<std::uint8_t> init,first,remaining;
    bool saw_first=false;
    for (const auto& box:boxes) {
        if (box.type=="ftyp" || box.type=="moov") init.insert(init.end(),bytes.begin()+box.span.offset,bytes.begin()+box.span.offset+box.span.size);
        if (box.type=="moof" || box.type=="mdat") {
            auto& destination=saw_first ? remaining : first;
            destination.insert(destination.end(),bytes.begin()+box.span.offset,bytes.begin()+box.span.offset+box.span.size);
            if (box.type=="mdat") saw_first=true;
        }
    }
    remaining.insert(remaining.end(),first.begin(),first.end());
    const auto pad=[](std::vector<std::uint8_t>& chunk) {
        const std::size_t size=16384-chunk.size();
        chunk.insert(chunk.end(),{static_cast<std::uint8_t>(size>>24),static_cast<std::uint8_t>(size>>16),
                                 static_cast<std::uint8_t>(size>>8),static_cast<std::uint8_t>(size),'f','r','e','e'});
        chunk.resize(16384);
    };
    if (!expect(!init.empty() && !first.empty() && !remaining.empty() && init.size()<16376 && first.size()<16376 &&
                remaining.size()<16384,"staged stdin retry fixture has bounded complete chunks")) return false;
    pad(init);pad(first);
    auto gate=std::make_shared<MediaInputGate>();
    StagedMediaInputBuffer buffer(std::move(init),std::move(first),std::move(remaining),gate);
    std::istream input(&buffer);
    auto ready=std::make_shared<std::size_t>(0);
    std::vector<std::shared_ptr<MigrationTransport::Record>> records;
    PublisherConfig config;config.draft_version=DraftVersion::kDraft22;config.forward=true;
    config.subscriber_timeout=std::chrono::seconds(1);config.initial_group_id=initial_group_id;
    Publisher publisher(config,[&](TransportKind) -> std::unique_ptr<PublisherTransport> {
        auto record=std::make_shared<MigrationTransport::Record>();records.push_back(record);
        auto transport=std::make_unique<MigrationTransport>(record,std::nullopt);
        transport->source_reads=ready;transport->migration_threshold=1;transport->media_gate=gate;
        transport->invalid_subscribe=records.size()==1;
        return transport;
    });
    EndpointConfig endpoint;endpoint.host="first.example";endpoint.port=443;
    const std::vector<EndpointConfig> endpoints{endpoint};
    openmoq::publisher::LiveResumeState resume;
    openmoq::publisher::LiveIngestConfig ingest;ingest.use_stdin=true;
    std::vector<TransportStatus> attempts;
    const auto status=openmoq::publisher::publish_with_failover(
        endpoints, 1,
        [&](const EndpointConfig& attempt_endpoint) {
            attempts.push_back(publisher.publish_live(ingest,&input,attempt_endpoint,{},false,&resume));
            return attempts.back();
        },
        {}, {});
    const std::string label="initial Group ID "+std::to_string(initial_group_id)+": ";
    bool ok=expect(status.ok,label+"live publish retried after PROTOCOL_VIOLATION completes: "+status.message);
    ok &= expect(attempts.size()==2 &&
                     attempts[0].failure_kind==openmoq::publisher::transport::FailureKind::kRetryable,
                 label+"a peer PROTOCOL_VIOLATION is a retryable live publish failure");
    ok &= expect(records.size()==2 && records[0]->first_close_code==3,
                 label+"an undecodable SUBSCRIBE closes the first session with PROTOCOL_VIOLATION");
    // The init segment was read by the first session, so the retry can only
    // publish media if it was replayed from the resume state.
    ok &= expect(records.size()==2 && !records[0]->media_groups.empty() && !records[1]->media_groups.empty(),
                 label+"media is published before and after the retry");
    if (records.size()==2 && !records[0]->media_groups.empty()) {
        ok &= expect(records[0]->media_groups.front()==initial_group_id,
                     label+"the first session starts at the initial Group ID");
        for (const auto group:records[1]->media_groups)
            ok &= expect(group>records[0]->media_groups.back(),
                         label+"the retry must not reuse or go below the failed session's Group IDs");
    }
    ok &= expect(resume.bytes.empty() && resume.group_floor==0,
                 label+"a successful publish clears the resume state");
    return ok;
}
}  // namespace

int main() {
    bool ok = test_goaway_recreates_publisher_transport();
    ok &= test_disconnect_during_live_publish_closes_once();
    ok &= test_disconnect_during_publish_is_not_retried();
    ok &= test_disconnect_during_connect_waits_for_connect();
    ok &= test_disconnect_does_not_follow_goaway();
    ok &= test_live_stdin_retry_after_protocol_violation_resumes(0);
    ok &= test_live_stdin_retry_after_protocol_violation_resumes(1000);
#if !defined(_WIN32)
    ok &= test_disconnect_stops_live_stdin_on_idle_input(false, true);
    ok &= test_disconnect_stops_live_stdin_on_idle_input(true, true);
    ok &= test_disconnect_stops_live_stdin_on_idle_input(true, false);
#endif
    ok &= test_goaway_migration_bound_and_uri_validation();
    ok &= test_live_source_goaway_continues_and_replays_catalog();
    ok &= test_live_source_goaway_continues_and_replays_catalog(true);
    ok &= test_catalog_replay_limit_only_applies_to_migration();
    ok &= test_live_stdin_goaway_replays_parser_residual();
    ok &= test_live_stdin_migration_keeps_object_identity();
    ok &= test_live_stdin_migration_keeps_object_identity(1700000000000ULL);
    ok &= expect(PublisherConfig{}.draft_version == DraftVersion::kDraft18,
                 "publisher API defaults to draft-18");

    {
        PublisherConfig invalid;
        invalid.authorization.setup_credential = openmoq::publisher::cat4moq::Credential{};
        bool rejected = false;
        try {
            Publisher publisher(invalid);
        } catch (const openmoq::publisher::cat4moq::AuthorizationError&) {
            rejected = true;
        }
        ok &= expect(rejected, "publisher must reject empty structured credentials before connecting");

        Publisher publisher;
        rejected = false;
        try {
            publisher.set_config(invalid);
        } catch (const openmoq::publisher::cat4moq::AuthorizationError&) {
            rejected = true;
        }
        ok &= expect(rejected && !publisher.config().authorization.configured(),
                     "invalid auth update must leave previous publisher configuration intact");
    }

    {
        PublisherConfig config;
        config.draft_version = DraftVersion::kDraft16;
        config.track_namespace = "app";

        const auto state = std::make_shared<MockTransport::State>();
        Publisher publisher(
            config,
            [state](TransportKind kind) -> std::unique_ptr<PublisherTransport> {
                if (kind != TransportKind::kRawQuic) {
                    return nullptr;
                }
                return std::make_unique<MockTransport>(state);
            });

        PreparedPublish prepared;
        prepared.plan = PublishPlan{.draft = draft_profile(DraftVersion::kDraft16)};

        EndpointConfig endpoint;
        endpoint.transport = TransportKind::kRawQuic;
        endpoint.host = "relay.example.com";
        endpoint.port = 443;

        const TransportStatus status = publisher.publish(prepared, endpoint);
        ok &= expect(!status.ok, "expected mock connect failure to propagate");
        ok &= expect(status.message == "transport connect failed: mock connect failure",
                     "expected connect failure message to be wrapped");
        ok &= expect(status.failure_kind == openmoq::publisher::transport::FailureKind::kRetryable,
                     "expected connection failures to be eligible for endpoint retry");
        ok &= expect(state->configure_called,
                     "expected transport configure to be invoked");
        ok &= expect(state->configured_endpoint.alpn == "moqt-16",
                     "expected default ALPN for draft-16 raw transport");
        ok &= expect(state->configured_endpoint.application_protocol == "moqt-16",
                     "expected application protocol to follow draft default for raw transport");
    }

    {
        PublisherConfig config;
        config.draft_version = DraftVersion::kDraft14;

        const auto state = std::make_shared<MockTransport::State>();
        Publisher publisher(
            config,
            [state](TransportKind kind) -> std::unique_ptr<PublisherTransport> {
                if (kind != TransportKind::kWebTransport) {
                    return nullptr;
                }
                return std::make_unique<MockTransport>(state);
            });

        PreparedPublish prepared;
        prepared.plan = PublishPlan{.draft = draft_profile(DraftVersion::kDraft14)};

        EndpointConfig endpoint;
        endpoint.transport = TransportKind::kWebTransport;
        endpoint.host = "relay.example.com";
        endpoint.port = 443;
        endpoint.path = "/moq";
        endpoint.path_explicit = true;

        const TransportStatus status = publisher.publish(prepared, endpoint);
        ok &= expect(!status.ok, "expected mock connect failure to propagate for webtransport");
        ok &= expect(state->configured_endpoint.alpn == "h3",
                     "expected default ALPN h3 for webtransport");
        ok &= expect(state->configured_endpoint.application_protocol.empty(),
                     "expected draft-14 webtransport to offer empty application protocol");
    }

    {
        PublisherConfig config;
        config.draft_version = DraftVersion::kDraft16;

        const auto state = std::make_shared<MockTransport::State>();
        Publisher publisher(
            config,
            [state](TransportKind kind) -> std::unique_ptr<PublisherTransport> {
                if (kind != TransportKind::kWebTransport) {
                    return nullptr;
                }
                return std::make_unique<MockTransport>(state);
            });

        PreparedPublish prepared;
        prepared.plan = PublishPlan{.draft = draft_profile(DraftVersion::kDraft16)};

        EndpointConfig endpoint;
        endpoint.transport = TransportKind::kWebTransport;
        endpoint.host = "relay.example.com";
        endpoint.port = 443;
        endpoint.path = "/moq";
        endpoint.path_explicit = true;

        const TransportStatus status = publisher.publish(prepared, endpoint);
        ok &= expect(!status.ok, "expected mock connect failure to propagate for draft-16 webtransport");
        ok &= expect(state->configured_endpoint.alpn == "h3",
                     "expected default ALPN h3 for draft-16 webtransport");
        ok &= expect(state->configured_endpoint.application_protocol == "\"moqt-16\"",
                     "expected draft-16 webtransport to offer a structured WT protocol token");
    }

    {
        PublisherConfig config;
        config.draft_version = DraftVersion::kDraft18;

        const auto state = std::make_shared<MockTransport::State>();
        Publisher publisher(
            config,
            [state](TransportKind kind) -> std::unique_ptr<PublisherTransport> {
                if (kind != TransportKind::kWebTransport) {
                    return nullptr;
                }
                return std::make_unique<MockTransport>(state);
            });

        PreparedPublish prepared;
        prepared.plan = PublishPlan{.draft = draft_profile(DraftVersion::kDraft18)};

        EndpointConfig endpoint;
        endpoint.transport = TransportKind::kWebTransport;
        endpoint.host = "relay.example.com";
        endpoint.port = 443;
        endpoint.path = "/moq";
        endpoint.path_explicit = true;

        const TransportStatus status = publisher.publish(prepared, endpoint);
        ok &= expect(!status.ok, "expected mock connect failure to propagate for draft-18 webtransport");
        ok &= expect(state->configured_endpoint.alpn == "h3",
                     "expected default ALPN h3 for draft-18 webtransport");
        ok &= expect(state->configured_endpoint.application_protocol == "\"moqt-18\"",
                     "expected draft-18 webtransport to offer a structured WT protocol token");
    }

    for (const auto draft : {DraftVersion::kDraft21, DraftVersion::kDraft22}) {
        for (const TransportKind kind : {TransportKind::kWebTransport, TransportKind::kRawQuic}) {
            PublisherConfig config;
            config.draft_version = draft;

            const auto state = std::make_shared<MockTransport::State>();
            Publisher publisher(
                config,
                [state, kind](TransportKind requested) -> std::unique_ptr<PublisherTransport> {
                    if (requested != kind) {
                        return nullptr;
                    }
                    return std::make_unique<MockTransport>(state);
                });

            PreparedPublish prepared;
            prepared.plan = PublishPlan{.draft = draft_profile(draft)};

            EndpointConfig endpoint;
            endpoint.transport = kind;
            endpoint.host = "relay.example.com";
            endpoint.port = 443;
            endpoint.path = "/moq";
            endpoint.path_explicit = true;

            const TransportStatus status = publisher.publish(prepared, endpoint);
            ok &= expect(!status.ok, "expected mock connect failure to propagate for selected draft");
            if (kind == TransportKind::kWebTransport) {
                ok &= expect(state->configured_endpoint.alpn == "h3", "expected default ALPN h3 for selected draft webtransport");
                ok &= expect(state->configured_endpoint.application_protocol == "\"" + default_alpn(draft) + "\"",
                             "expected selected draft webtransport to offer the selected WT protocol token");
            } else {
                ok &= expect(state->configured_endpoint.alpn == default_alpn(draft), "expected selected draft raw QUIC ALPN");
            }
        }
    }

    {
        PublisherConfig config;
        config.draft_version = DraftVersion::kDraft16;

        const auto state = std::make_shared<MockTransport::State>();
        Publisher publisher(
            config,
            [state](TransportKind kind) -> std::unique_ptr<PublisherTransport> {
                if (kind != TransportKind::kRawQuic) {
                    return nullptr;
                }
                return std::make_unique<MockTransport>(state);
            });

        LiveObjectSource source{
            .tracks = {LiveTrack{.track_name = "events"}},
            .next_object = []() { return std::nullopt; },
        };

        EndpointConfig endpoint;
        endpoint.transport = TransportKind::kRawQuic;
        endpoint.host = "relay.example.com";
        endpoint.port = 443;

        const TransportStatus status = publisher.publish_live_objects(source, endpoint);
        ok &= expect(!status.ok, "expected live-object publish mock connect failure to propagate");
        ok &= expect(status.message == "transport connect failed: mock connect failure",
                     "expected live-object connect failure message to be wrapped");
        ok &= expect(status.failure_kind == openmoq::publisher::transport::FailureKind::kRetryable,
                     "expected live-object connection failures to be eligible for endpoint retry");
        ok &= expect(state->configured_endpoint.alpn == "moqt-16",
                     "expected live-object publish to preserve default raw draft ALPN");
    }

    {
        Publisher publisher(PublisherConfig{});

        LiveObjectSource source{
            .tracks = {LiveTrack{.track_name = "video"}},
            .next_object = []() { return std::nullopt; },
            .catalog_mode = LiveCatalogMode::kSourceObject,
        };

        EndpointConfig endpoint;
        endpoint.transport = TransportKind::kRawQuic;
        endpoint.host = "relay.example.com";
        endpoint.port = 443;

        const TransportStatus status = publisher.publish_live_objects(source, endpoint);
        ok &= expect(!status.ok, "expected source-catalog mode without a catalog track to fail");
        ok &= expect(status.message.find("catalog track") != std::string::npos,
                     "expected missing catalog track failure to identify the contract");
    }

    // Backend-selection gate. When libmoq is the selected publish backend
    // (OPENMOQ_USE_LIBMOQ_PUBLISHER=ON), a non-injected publish_live_objects with
    // a bare/legacy LiveTrack is rejected up front by libmoq's media-metadata gate
    // -- a deterministic, network-free signal that the libmoq route was taken.
    // When the gate is OFF the libmoq route is compiled out and non-injected
    // publishing stays on the MoqtSession path (which would attempt a real
    // connection, so it is not exercised here -- the injected-factory cases above
    // already cover the old path, and they force it regardless of the gate).
#ifdef OPENMOQ_ENABLE_LIBMOQ_PUBLISHER
    {
        Publisher publisher(PublisherConfig{});  // no injected factory

        LiveObjectSource source{
            .tracks = {LiveTrack{.track_name = "events"}},
            .next_object = []() { return std::nullopt; },
        };

        EndpointConfig endpoint;
        endpoint.transport = TransportKind::kRawQuic;
        endpoint.host = "relay.example.com";
        endpoint.port = 443;

        const TransportStatus status = publisher.publish_live_objects(source, endpoint);
        ok &= expect(!status.ok, "expected bare-track libmoq publish_live_objects to fail");
        ok &= expect(status.message.find("media metadata") != std::string::npos,
                     "expected a clear media-metadata failure on the libmoq route");
    }

    {
        // libmoq runs its own WebTransport client, so the reset_stream_at
        // opt-out would be silently ignored; it is refused instead.
        Publisher publisher(PublisherConfig{});  // no injected factory

        LiveObjectSource source{
            .tracks = {LiveTrack{.track_name = "events"}},
            .next_object = []() { return std::nullopt; },
        };

        EndpointConfig endpoint;
        endpoint.transport = TransportKind::kWebTransport;
        endpoint.host = "relay.example.com";
        endpoint.port = 443;
        endpoint.path = "/moq";
        endpoint.path_explicit = true;
        endpoint.webtransport_allow_missing_reset_stream_at = true;

        const TransportStatus status = publisher.publish_live_objects(source, endpoint);
        ok &= expect(!status.ok, "expected the reset_stream_at opt-out to fail on the libmoq route");
        ok &= expect(status.message.find("--webtransport-allow-missing-reset-stream-at") != std::string::npos,
                     "expected the libmoq route to name the unsupported reset_stream_at opt-out");
    }

    {
        Publisher publisher(PublisherConfig{});  // no injected factory

        LiveObjectSource source{
            .tracks = {
                LiveTrack{.track_name = "catalog"},
                LiveTrack{.track_name = "transport"},
            },
            .next_object = []() -> std::optional<LiveObject> {
                return LiveObject{
                    .track_name = "catalog",
                    .payload = {'{', '}'},
                };
            },
            .catalog_mode = LiveCatalogMode::kSourceObject,
        };

        EndpointConfig endpoint;
        endpoint.transport = static_cast<TransportKind>(0xff);

        const TransportStatus status = publisher.publish_live_objects(source, endpoint);
        ok &= expect(!status.ok, "expected unsupported source-catalog transport to fail");
        ok &= expect(status.message == "failed to create requested transport",
                     "expected source-catalog mode to bypass the libmoq metadata gate");
    }
#endif

    // The republish interval is off by default: existing deployments keep
    // their current wire behaviour.
    PublisherConfig default_config;
    ok &= expect(default_config.catalog_republish_interval == std::chrono::seconds(0),
                 "expected catalog republication disabled by default");
    ok &= expect(!default_config.vod, "expected the publisher to default to live");
    ok &= expect(default_config.initial_group_id == 0, "expected live Group IDs to start at 0 by default");
    {
        const auto before = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        const auto group_id = openmoq::publisher::time_based_initial_group_id();
        ok &= expect(group_id >= before && group_id <= openmoq::publisher::kMaxGroupId,
                     "expected a time-based initial Group ID in milliseconds since the epoch");
        PublisherConfig oversized;
        oversized.initial_group_id = openmoq::publisher::kMaxGroupId + 1;
        bool rejected = false;
        try {
            Publisher publisher(oversized);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        ok &= expect(rejected, "expected an initial Group ID above 2^62-1 to be rejected");
        Publisher publisher;
        rejected = false;
        try {
            publisher.set_config(oversized);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        ok &= expect(rejected && publisher.config().initial_group_id == 0,
                     "expected set_config to reject an initial Group ID above 2^62-1");
        PublisherConfig largest;
        largest.initial_group_id = openmoq::publisher::kMaxGroupId;
        publisher.set_config(largest);
        ok &= expect(publisher.config().initial_group_id == openmoq::publisher::kMaxGroupId,
                     "expected an initial Group ID of 2^62-1 to be accepted");
    }

    // end_broadcast on a publisher that never connected reports failure
    // rather than throwing, matching how disconnect() behaves.
    Publisher idle_publisher;
    const auto end_status = idle_publisher.end_broadcast(EndBroadcastMode::kTerminate);
    ok &= expect(!end_status.ok, "expected end_broadcast without a session to fail cleanly");

    return ok ? 0 : 1;
}
