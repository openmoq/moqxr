#include "openmoq/publisher/publisher_api.h"

#include <chrono>
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

}  // namespace

int main() {
    bool ok = true;

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
