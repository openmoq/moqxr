#include "openmoq/publisher/publisher_api.h"
#include "openmoq/publisher/mp4_box.h"
#include "openmoq/publisher/transport/moqt_control_messages.h"

#include <algorithm>
#include <set>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

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
        bool closed = false;
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
    bool setup_read = false;
    std::uint64_t next_stream = 0;
    TransportStatus configure(const EndpointConfig& endpoint, const TlsConfig& tls) override {
        record->endpoint = endpoint;
        record->tls = tls;
        return TransportStatus::success();
    }
    TransportStatus connect() override { return TransportStatus::success(); }
    ConnectionState state() const override { return ConnectionState::kConnected; }
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
        return TransportStatus::failure("timed out waiting for stream data");
    }
    TransportStatus write_stream(std::uint64_t stream, std::span<const std::uint8_t> bytes, bool) override {
        if (media_gate && !bytes.empty() && media_streams.erase(stream)) {
            std::size_t offset=0;
            std::uint64_t type=0,alias=0,group=0;
            using openmoq::publisher::transport::decode_vi64;
            if (!decode_vi64(bytes,offset,type) || !decode_vi64(bytes,offset,alias) || !decode_vi64(bytes,offset,group))
                return TransportStatus::failure("malformed test media header");
            record->media_groups.push_back(group);
            if (target) {
                *source_reads=1;

            }
        }
        record->written_bytes.insert(record->written_bytes.end(), bytes.begin(), bytes.end());
        return TransportStatus::success();
    }
    TransportStatus read_stream(std::uint64_t stream, std::vector<std::uint8_t>& bytes, bool& fin,
                                std::chrono::milliseconds) override {
        fin = false;
        if (stream == 3 && !setup_read) {
            setup_read = true;
            bytes = openmoq::publisher::transport::encode_server_setup_message({.draft = DraftVersion::kDraft22});
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
    TransportStatus close(std::uint64_t) override {
        record->closed = true;
        return TransportStatus::success();
    }
};

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

bool test_live_stdin_migration_keeps_object_identity() {
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
    config.subscriber_timeout=std::chrono::seconds(1);
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
    return ok;
}

}  // namespace

int main() {
    bool ok = test_goaway_recreates_publisher_transport();
    ok &= test_goaway_migration_bound_and_uri_validation();
    ok &= test_live_source_goaway_continues_and_replays_catalog();
    ok &= test_live_source_goaway_continues_and_replays_catalog(true);
    ok &= test_catalog_replay_limit_only_applies_to_migration();
    ok &= test_live_stdin_goaway_replays_parser_residual();
    ok &= test_live_stdin_migration_keeps_object_identity();
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

    // end_broadcast on a publisher that never connected reports failure
    // rather than throwing, matching how disconnect() behaves.
    Publisher idle_publisher;
    const auto end_status = idle_publisher.end_broadcast(EndBroadcastMode::kTerminate);
    ok &= expect(!end_status.ok, "expected end_broadcast without a session to fail cleanly");

    return ok ? 0 : 1;
}
