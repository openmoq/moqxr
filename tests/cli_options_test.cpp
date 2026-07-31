#include "openmoq/publisher/cli_options.h"
#include "openmoq/publisher/msf_url.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using openmoq::publisher::CliOptions;
using openmoq::publisher::ConnectionRequirement;
using openmoq::publisher::build_track_msf_url;
using openmoq::publisher::parse_cli_options;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

CliOptions parse(std::vector<std::string> args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    return parse_cli_options(static_cast<int>(argv.size()), argv.data());
}

bool parse_throws(std::vector<std::string> args, const std::string& fragment,
                  const std::string& message) {
    try {
        parse(std::move(args));
    } catch (const std::runtime_error& error) {
        const std::string what = error.what();
        if (what.find(fragment) != std::string::npos) {
            return true;
        }
        std::cerr << "FAIL: " << message << " (threw, but message was \"" << what
                  << "\" and did not contain \"" << fragment << "\")\n";
        return false;
    }
    std::cerr << "FAIL: " << message << " (did not throw)\n";
    return false;
}

// A minimal, valid single-system --drm-config file, written to a fresh path
// each call so parallel test blocks never race on the same file.
std::filesystem::path write_drm_config_file(std::string_view name) {
    const std::filesystem::path config_path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(config_path);
    out << R"({
  "systems": [
    {
      "systemID": "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed",
      "laURL": "https://widevine.example.com/proxy"
    }
  ]
})";
    return config_path;
}

}  // namespace

int main() {
    bool ok = true;

    {
        const CliOptions options = parse({"openmoq-publisher", "--input", "sample.mp4"});
        ok &= expect(options.subscriber_timeout == std::chrono::seconds(30),
                     "expected default subscriber timeout to be 30 seconds");
        ok &= expect(options.split_cmaf_chunks, "expected chunk splitting to be enabled by default");
        ok &= expect(options.input_source.kind == openmoq::publisher::InputSourceKind::kFile,
                     "expected file input to remain the default input source kind");
        ok &= expect(options.input_source.path == "sample.mp4",
                     "expected file input path to be preserved");
        ok &= expect(options.transport == openmoq::publisher::transport::TransportKind::kRawQuic,
                     "expected raw transport to remain the default");
        ok &= expect(!options.include_sap, "expected SAP track creation to be disabled by default");
        ok &= expect(!options.include_msf_timeline, "expected MSF timeline track creation to be disabled by default");
    }

    {
        const CliOptions options =
            parse({"openmoq-publisher", "--input", "sample.mp4", "--transport", "webtransport",
                   "--endpoint", "https://relay.example.com:443/moq"});
        ok &= expect(options.transport == openmoq::publisher::transport::TransportKind::kWebTransport,
                     "expected --transport webtransport to select WebTransport mode");
        ok &= expect(options.endpoint.has_value(), "expected WebTransport endpoint to be parsed");
        ok &= expect(options.endpoint->transport == openmoq::publisher::transport::TransportKind::kWebTransport,
                     "expected endpoint transport kind to follow the CLI transport selection");
        ok &= expect(options.endpoint->path == "/moq", "expected WebTransport endpoint path to be preserved");
    }

    {
        const CliOptions options =
            parse({"openmoq-publisher", "--input", "sample.mp4", "--timeout", "9", "--forward", "0"});
        ok &= expect(options.subscriber_timeout == std::chrono::seconds(9),
                     "expected --timeout to override subscriber timeout");
    }

    {
        const CliOptions options =
            parse({"openmoq-publisher", "--input", "sample.mp4", "--draft", "18"});
        ok &= expect(options.draft_version == openmoq::publisher::DraftVersion::kDraft18,
                     "expected --draft 18 to select draft-18 mode");
    }

    {
        const CliOptions options = parse(
            {"openmoq-publisher", "--input", "sample.mp4", "--endpoint", "203.0.113.10:443", "--sni", "moq-relay.red5.net"});
        ok &= expect(options.endpoint.has_value(), "expected endpoint to be present when parsing --sni");
        ok &= expect(options.endpoint->sni == "moq-relay.red5.net", "expected --sni to populate endpoint SNI");
    }

    {
        bool threw = false;
        try {
            static_cast<void>(parse({"openmoq-publisher", "--input", "sample.mp4", "--sni", "moq-relay.red5.net"}));
        } catch (const std::runtime_error& error) {
            threw = std::string(error.what()) == "--alpn and --sni require --endpoint to be provided first";
        }
        ok &= expect(threw, "expected --sni without --endpoint to be rejected");
    }

    {
        bool threw = false;
        try {
            static_cast<void>(parse({"openmoq-publisher", "--input", "sample.mp4", "--transport", "webtransport",
                                     "--endpoint", "relay.example.com:443"}));
        } catch (const std::runtime_error& error) {
            threw = std::string(error.what()) ==
                    "--transport webtransport requires an endpoint path such as https://host:port/moq";
        }
        ok &= expect(threw, "expected WebTransport transport mode to reject endpoints without a path");
    }

    {
        const CliOptions options =
            parse({"openmoq-publisher", "--input", "sample.mp4", "--publish-catalog"});
        ok &= expect(options.publish_catalog, "expected --publish-catalog to enable proactive catalog publish");
    }

    {
        const CliOptions options =
            parse({"openmoq-publisher", "--input", "sample.mp4", "--sap"});
        ok &= expect(options.include_sap, "expected --sap to enable SAP track creation");
    }

    {
        const CliOptions options =
            parse({"openmoq-publisher", "--input", "sample.mp4", "--msf-timeline"});
        ok &= expect(options.include_msf_timeline, "expected --msf-timeline to enable MSF timeline track creation");
    }

    {
        const CliOptions options =
            parse({"openmoq-publisher", "--input", "sample.mp4", "--coalesce-cmaf-chunks"});
        ok &= expect(!options.split_cmaf_chunks, "expected --coalesce-cmaf-chunks to disable default chunk splitting");
    }

    {
        const CliOptions options =
            parse({"openmoq-publisher", "--input", "sample.mp4", "--loop"});
        ok &= expect(options.loop, "expected --loop to keep publishing after the file reaches EOF");
    }

    {
        bool threw = false;
        try {
            static_cast<void>(parse({"openmoq-publisher", "--input", "sample.mp4", "--timeout", "-1"}));
        } catch (const std::runtime_error& error) {
            threw = std::string(error.what()) == "subscriber timeout must be zero or greater";
        }
        ok &= expect(threw, "expected negative --timeout to be rejected");
    }

    {
        const CliOptions options = parse({"openmoq-publisher", "--input", "sample.mp4"});
        ok &= expect(!options.vod, "expected live to remain the default (no --vod)");
        ok &= expect(options.catalog_republish_interval == std::chrono::seconds(0),
                     "expected catalog republication to be disabled by default");
    }

    {
        const CliOptions options =
            parse({"openmoq-publisher", "--input", "sample.mp4", "--vod"});
        ok &= expect(options.vod, "expected --vod to opt into VOD semantics");
    }

    {
        const CliOptions options = parse(
            {"openmoq-publisher", "--input", "sample.mp4", "--catalog-republish-interval", "45"});
        ok &= expect(options.catalog_republish_interval == std::chrono::seconds(45),
                     "expected --catalog-republish-interval to override the republish interval");
    }

    {
        bool threw = false;
        try {
            static_cast<void>(parse(
                {"openmoq-publisher", "--input", "sample.mp4", "--catalog-republish-interval", "-1"}));
        } catch (const std::runtime_error& error) {
            threw = std::string(error.what()) == "--catalog-republish-interval must be zero or greater";
        }
        ok &= expect(threw, "expected negative --catalog-republish-interval to be rejected");
    }

    {
        bool threw = false;
        try {
            static_cast<void>(parse(
                {"openmoq-publisher", "--input", "sample.mp4", "--catalog-republish-interval", "abc"}));
        } catch (const std::runtime_error& error) {
            threw = std::string(error.what()) == "--catalog-republish-interval must be a valid integer";
        }
        ok &= expect(threw, "expected a non-numeric --catalog-republish-interval to be rejected");
    }

    {
        const CliOptions options = parse({"openmoq-publisher", "--input", "-"});
        ok &= expect(options.input_source.kind == openmoq::publisher::InputSourceKind::kStdin,
                     "expected --input - to select stdin");
        ok &= expect(options.input_source.path.empty(),
                     "expected stdin input source to avoid storing a file path");
    }

    // --live-source srt --srt-config /tmp/foo.json
    {
        const CliOptions options = parse({"openmoq-publisher", "--live-source", "srt", "--srt-config", "/tmp/foo.json",
                                          "--endpoint", "localhost:4443", "--namespace", "ns"});
        ok &= expect(options.live_source == openmoq::publisher::LiveSourceKind::kSrt,
                     "expected --live-source srt");
        ok &= expect(options.srt_config_path == "/tmp/foo.json",
                     "expected --srt-config path");
    }

    // --live-source srt without --srt-config should fail
    {
        bool threw = false;
        try {
            parse({"openmoq-publisher", "--live-source", "srt", "--namespace", "ns"});
        } catch (...) {
            threw = true;
        }
        ok &= expect(threw, "expected --live-source srt without --srt-config to fail");
    }

    {
        const CliOptions options =
            parse({"openmoq-publisher", "--live-source", "dash",
                   "--dash-listen", "127.0.0.1:8080",
                   "--dash-path", "/ingest",
                   "--dash-queue-depth", "32",
                   "--endpoint", "https://relay.example.com:443/moq"});
        ok &= expect(options.live_source == openmoq::publisher::LiveSourceKind::kDash,
                     "expected --live-source dash");
        ok &= expect(options.dash_listen_host == "127.0.0.1",
                     "expected --dash-listen host");
        ok &= expect(options.dash_listen_port == 8080,
                     "expected --dash-listen port");
        ok &= expect(options.dash_path_prefix == "/ingest",
                     "expected --dash-path prefix");
        ok &= expect(options.dash_queue_depth == 32,
                     "expected --dash-queue-depth value");
    }

    {
        bool threw = false;
        try {
            parse({"openmoq-publisher", "--live-source", "dash",
                   "--endpoint", "https://relay.example.com:443/moq"});
        } catch (const std::runtime_error& error) {
            threw = std::string(error.what()) == "--live-source dash requires --dash-listen";
        }
        ok &= expect(threw, "expected --live-source dash without --dash-listen to fail");
    }

    {
        bool threw = false;
        try {
            parse({"openmoq-publisher", "--live-source", "dash",
                   "--dash-listen", "127.0.0.1:8080"});
        } catch (const std::runtime_error& error) {
            threw = std::string(error.what()) == "--live-source dash requires --endpoint or --dump-plan";
        }
        ok &= expect(threw, "expected --live-source dash without --endpoint to fail");
    }

    {
        const CliOptions options =
            parse({"openmoq-publisher", "--live-source", "dash",
                   "--dash-listen", "127.0.0.1:8080",
                   "--dump-plan"});
        ok &= expect(options.live_source == openmoq::publisher::LiveSourceKind::kDash,
                     "expected --live-source dash with --dump-plan to parse");
        ok &= expect(options.dump_plan,
                     "expected --dump-plan to be set for dash dry run");
        ok &= expect(!options.endpoint.has_value(),
                     "expected dash dry run to omit --endpoint");
    }

    {
        bool threw = false;
        try {
            parse({"openmoq-publisher", "--input", "sample.mp4", "--dash-path", "/ingest"});
        } catch (const std::runtime_error& error) {
            threw = std::string(error.what()) == "--dash-path requires --live-source dash";
        }
        ok &= expect(threw, "expected --dash-path without --live-source dash to fail");
    }

    {
        bool threw = false;
        try {
            parse({"openmoq-publisher", "--input", "sample.mp4", "--dash-queue-depth", "32"});
        } catch (const std::runtime_error& error) {
            threw = std::string(error.what()) == "--dash-queue-depth requires --live-source dash";
        }
        ok &= expect(threw, "expected --dash-queue-depth without --live-source dash to fail");
    }

    {
        bool threw = false;
        try {
            parse({"openmoq-publisher", "--live-source", "dash",
                   "--dash-listen", "127.0.0.1:80abc",
                   "--endpoint", "relay.example.com:443"});
        } catch (const std::runtime_error& error) {
            threw = std::string(error.what()) == "--dash-listen port must be a valid integer";
        }
        ok &= expect(threw, "expected trailing garbage in --dash-listen port to be rejected");
    }

    {
        bool threw = false;
        try {
            parse({"openmoq-publisher", "--live-source", "dash",
                   "--dash-listen", "127.0.0.1:8080",
                   "--dash-queue-depth", "32junk",
                   "--endpoint", "relay.example.com:443"});
        } catch (const std::runtime_error& error) {
            threw = std::string(error.what()) == "--dash-queue-depth must be a valid integer";
        }
        ok &= expect(threw, "expected trailing garbage in --dash-queue-depth to be rejected");
    }

    // --drm-config <path> populates drm_systems, parsed eagerly.
    {
        const std::filesystem::path config_path =
            std::filesystem::temp_directory_path() / "openmoq-drm-config-cli-test.json";
        {
            std::ofstream out(config_path);
            out << R"({
  "systems": [
    {
      "systemID": "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed",
      "laURL": "https://widevine.example.com/proxy",
      "laURLType": "widevine",
      "certURL": "https://widevine.example.com/cert",
      "robustness": "SW_SECURE_CRYPTO"
    }
  ]
})";
        }

        const CliOptions options = parse({"openmoq-publisher", "--input", "sample.mp4",
                                          "--drm-config", config_path.string()});
        ok &= expect(options.drm_systems.size() == 1, "expected one configured DRM system");
        if (options.drm_systems.size() == 1) {
            const auto& system = options.drm_systems.front();
            ok &= expect(system.system_id == "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed",
                         "expected the configured system ID");
            ok &= expect(system.la_url.value_or("") == "https://widevine.example.com/proxy",
                         "expected the configured laURL");
            ok &= expect(system.la_url_type.value_or("") == "widevine",
                         "expected the configured laURLType");
            ok &= expect(system.cert_url.value_or("") == "https://widevine.example.com/cert",
                         "expected the configured certURL");
            ok &= expect(system.robustness.value_or("") == "SW_SECURE_CRYPTO",
                         "expected the configured robustness");
        }

        std::error_code ec;
        std::filesystem::remove(config_path, ec);
    }

    // A missing --drm-config file must fail parsing with a clear error, not
    // publish with partial DRM configuration.
    {
        bool threw = false;
        std::string message;
        try {
            parse({"openmoq-publisher", "--input", "sample.mp4",
                   "--drm-config", "/nonexistent/openmoq-drm-config.json"});
        } catch (const std::runtime_error& error) {
            threw = true;
            message = error.what();
        }
        ok &= expect(threw, "expected a missing --drm-config file to fail rather than crash");
        ok &= expect(message.find("/nonexistent/openmoq-drm-config.json") != std::string::npos,
                     "expected the error to name the offending path");
    }

    // --drm-config combined with --live-source srt must be refused: SRT
    // carries MPEG-TS, from which the publisher synthesises a CMAF init
    // segment with no sinf/tenc/pssh boxes, so there is nothing for
    // --drm-config to describe. See docs/status.md.
    {
        const std::filesystem::path config_path =
            write_drm_config_file("openmoq-drm-config-cli-live-srt-test.json");

        bool threw = false;
        std::string message;
        try {
            parse({"openmoq-publisher", "--live-source", "srt", "--srt-config", "/tmp/foo.json",
                   "--endpoint", "localhost:4443", "--namespace", "ns",
                   "--drm-config", config_path.string()});
        } catch (const std::runtime_error& error) {
            threw = true;
            message = error.what();
        }
        ok &= expect(threw, "expected --drm-config with --live-source srt to be refused");
        ok &= expect(message.find("--live-source srt") != std::string::npos,
                     "expected the refusal to name --live-source srt");
        ok &= expect(message.find("MPEG-TS") != std::string::npos,
                     "expected the refusal to explain that MPEG-TS carries no CENC metadata");

        std::error_code ec;
        std::filesystem::remove(config_path, ec);
    }

    // Phase 5: detection now works on the dash and stdin live paths, so
    // --drm-config is accepted there.
    {
        const std::filesystem::path config_path =
            write_drm_config_file("openmoq-drm-config-cli-live-dash-test.json");

        const CliOptions options = parse({"openmoq-publisher", "--live-source", "dash",
                                          "--dash-listen", "127.0.0.1:8080",
                                          "--endpoint", "https://relay.example.com:443/moq",
                                          "--drm-config", config_path.string()});
        ok &= expect(options.drm_systems.size() == 1,
                     "expected --drm-config to be accepted with --live-source dash");

        std::error_code ec;
        std::filesystem::remove(config_path, ec);
    }

    {
        const std::filesystem::path config_path =
            write_drm_config_file("openmoq-drm-config-cli-live-stdin-test.json");

        const CliOptions options = parse({"openmoq-publisher", "--input", "-", "--endpoint", "localhost:4443",
                                          "--drm-config", config_path.string()});
        ok &= expect(options.drm_systems.size() == 1,
                     "expected --drm-config to be accepted with the default live stdin path");

        std::error_code ec;
        std::filesystem::remove(config_path, ec);
    }

    // --drm-config with a file input (batch/VOD mode) must remain unaffected:
    // only the live publish paths lack content-protection signalling, and the
    // batch plan path already attaches it via attach_content_protection.
    {
        const std::filesystem::path config_path =
            write_drm_config_file("openmoq-drm-config-cli-batch-test.json");

        const CliOptions options = parse({"openmoq-publisher", "--input", "sample.mp4",
                                          "--endpoint", "localhost:4443",
                                          "--drm-config", config_path.string()});
        ok &= expect(options.drm_systems.size() == 1,
                     "expected --drm-config with file input (batch mode) to remain unaffected");

        std::error_code ec;
        std::filesystem::remove(config_path, ec);
    }

    // Dash dry run (--dump-plan, no --endpoint) never actually publishes a
    // live catalog. --drm-config is accepted here too, same as the live dash
    // path above -- there is no dash-specific refusal left to guard against.
    {
        const std::filesystem::path config_path =
            write_drm_config_file("openmoq-drm-config-cli-dash-dry-run-test.json");

        const CliOptions options = parse({"openmoq-publisher", "--live-source", "dash",
                                          "--dash-listen", "127.0.0.1:8080",
                                          "--dump-plan",
                                          "--drm-config", config_path.string()});
        ok &= expect(options.drm_systems.size() == 1,
                     "expected --drm-config with a dash dry run (no --endpoint) to remain unaffected");

        std::error_code ec;
        std::filesystem::remove(config_path, ec);
    }

    // --url configures the endpoint and namespace from one argument.
    // (--input is required whenever --live-source defaults to "auto",
    // regardless of --url; unrelated to what this block tests.)
    {
        const auto options = parse(
            {"prog", "--input", "sample.mp4", "--url",
             "moqt://relay.example:4433/moq#msf:customerID-broadcastID--catalog"});
        ok &= expect(options.endpoint.has_value(), "expected --url to produce an endpoint");
        ok &= expect(options.endpoint->host == "relay.example", "expected host from --url");
        ok &= expect(options.endpoint->port == 4433, "expected port from --url");
        ok &= expect(options.endpoint->path == "/moq", "expected path from --url");
        ok &= expect(options.endpoint->path_explicit, "expected an explicit path from --url");
        ok &= expect(options.track_namespace == "customerID/broadcastID",
                     "expected the tuple joined with '/' into the flat namespace");
    }

    // connection=wt selects the transport.
    {
        const auto options = parse(
            {"prog", "--input", "sample.mp4", "--url",
             "moqt://relay.example/moq#msf:ns--catalog&connection=wt"});
        ok &= expect(options.transport == openmoq::publisher::transport::TransportKind::kWebTransport,
                     "expected connection=wt to select WebTransport");
    }

    // A c4m token is captured.
    {
        const auto options = parse(
            {"prog", "--input", "sample.mp4", "--url",
             "moqt://relay.example/moq#msf:ns--catalog&c4m=abc123"});
        ok &= expect(options.msf_c4m_token.value_or("") == "abc123", "expected the c4m token captured");
    }

    // An agreeing --transport is accepted.
    {
        const auto options = parse(
            {"prog", "--input", "sample.mp4", "--transport", "webtransport", "--url",
             "moqt://relay.example/moq#msf:ns--catalog&connection=wt"});
        ok &= expect(options.transport == openmoq::publisher::transport::TransportKind::kWebTransport,
                     "expected an agreeing --transport to be accepted");
    }

    ok &= parse_throws({"prog", "--url", "moqt://h/p#msf:ns--t", "--endpoint", "h:443"},
                            "--endpoint",
                            "expected --url with --endpoint to be refused");
    // The conflict is now resolved after the loop (see url_required_transport
    // below), which runs after the --input requirement check, so this needs
    // --input just like the success-path --url blocks above.
    ok &= parse_throws({"prog", "--input", "sample.mp4", "--transport", "raw", "--url",
                             "moqt://h/p#msf:ns--t&connection=wt"},
                            "connection",
                            "expected a disagreeing --transport and connection to be refused");
    ok &= parse_throws({"prog", "--url", "moqt://h/p#msf:a.2fb--t"},
                            "slash",
                            "expected a tuple element containing a slash to be refused");

    // The reverse order (--endpoint before --url) must also be refused.
    ok &= parse_throws({"prog", "--endpoint", "h:443", "--url", "moqt://h/p#msf:ns--t"},
                            "--endpoint",
                            "expected --endpoint followed by --url to be refused");

    // --alpn (or --sni) constructs an EndpointConfig eagerly, before any
    // --endpoint or --url flag is seen. A guard written as
    // options.endpoint.has_value() would misfire here and reject this
    // perfectly legal command line, which never mentions --url at all.
    {
        const CliOptions options =
            parse({"openmoq-publisher", "--input", "sample.mp4", "--alpn", "moq-99",
                   "--endpoint", "relay.example.com:443"});
        ok &= expect(options.endpoint.has_value(),
                     "expected --alpn then --endpoint (no --url) to still produce an endpoint");
        ok &= expect(options.endpoint->host == "relay.example.com",
                     "expected --endpoint to still set the host after a prior --alpn");
    }

    // The --transport/connection agreement and conflict checks must be
    // order-independent: resolved once after the loop, not only when
    // --transport happens to come before --url.

    // Disagreeing pair, --url first (the --transport-first case is covered
    // above, under "expected a disagreeing --transport and connection to be refused").
    ok &= parse_throws({"prog", "--input", "sample.mp4", "--url",
                             "moqt://h/p#msf:ns--t&connection=wt", "--transport", "raw"},
                            "connection",
                            "expected a connection requirement followed by a disagreeing --transport to be refused");

    // Agreeing pair, --url first (the --transport-first case is covered
    // above, under "An agreeing --transport is accepted").
    {
        const auto options = parse(
            {"prog", "--input", "sample.mp4", "--url",
             "moqt://relay.example/moq#msf:ns--catalog&connection=wt", "--transport", "webtransport"});
        ok &= expect(options.transport == openmoq::publisher::transport::TransportKind::kWebTransport,
                     "expected an agreeing connection requirement followed by --transport to be accepted");
    }

    // A --url with no connection parameter must not disturb an explicit
    // --transport, in either order.
    {
        const auto options = parse(
            {"prog", "--input", "sample.mp4", "--transport", "webtransport", "--url",
             "moqt://relay.example/moq#msf:ns--catalog"});
        ok &= expect(options.transport == openmoq::publisher::transport::TransportKind::kWebTransport,
                     "expected --transport before a connection-less --url to remain intact");
    }
    {
        const auto options = parse(
            {"prog", "--input", "sample.mp4", "--url", "moqt://relay.example/moq#msf:ns--catalog",
             "--transport", "webtransport"});
        ok &= expect(options.transport == openmoq::publisher::transport::TransportKind::kWebTransport,
                     "expected --transport after a connection-less --url to remain intact");
    }

    // --url must update host/port/path in place rather than replacing the
    // whole EndpointConfig, so an alpn/sni set by an earlier flag survives.
    {
        const auto options = parse(
            {"prog", "--input", "sample.mp4", "--alpn", "moq-99", "--url",
             "moqt://relay.example/moq#msf:ns--catalog"});
        ok &= expect(options.endpoint.has_value(), "expected --alpn then --url to produce an endpoint");
        ok &= expect(options.endpoint->alpn == "moq-99",
                     "expected --alpn set before --url to survive the --url endpoint update");
        ok &= expect(options.endpoint->host == "relay.example",
                     "expected --url to still set the host after a prior --alpn");
    }
    {
        const auto options = parse(
            {"prog", "--input", "sample.mp4", "--url", "moqt://relay.example/moq#msf:ns--catalog",
             "--alpn", "moq-99"});
        ok &= expect(options.endpoint.has_value(), "expected --url then --alpn to produce an endpoint");
        ok &= expect(options.endpoint->alpn == "moq-99",
                     "expected --alpn set after --url to take effect");
    }

    {
        const auto options = parse({"prog", "--input", "sample.mp4", "--endpoint", "h:443", "--print-msf-urls"});
        ok &= expect(options.print_msf_urls, "expected --print-msf-urls to set the flag");
        ok &= expect(!options.transport_explicit,
                     "expected transport_explicit to default to false without --transport or --url");
        const auto defaulted = parse({"prog", "--input", "sample.mp4", "--endpoint", "h:443"});
        ok &= expect(!defaulted.print_msf_urls, "expected --print-msf-urls to default off");
    }

    // transport_explicit distinguishes an operator-chosen transport from the
    // unstated kRawQuic default, so --print-msf-urls never asserts a
    // connection requirement the operator never expressed.
    {
        const auto options =
            parse({"prog", "--input", "sample.mp4", "--endpoint", "h:443", "--transport", "raw"});
        ok &= expect(options.transport_explicit, "expected --transport to mark transport_explicit");
    }
    {
        const auto options = parse({"prog", "--input", "sample.mp4", "--url",
                                    "moqt://h/p#msf:ns--catalog&connection=wt"});
        ok &= expect(options.transport_explicit,
                     "expected a --url connection requirement to mark transport_explicit");
    }
    {
        const auto options =
            parse({"prog", "--input", "sample.mp4", "--url", "moqt://h/p#msf:ns--catalog"});
        ok &= expect(!options.transport_explicit,
                     "expected a --url with no connection requirement to leave transport_explicit false");
    }

    // Item 1: --url's query must not be silently discarded. The fragment is
    // explicitly not sent (draft line ~3320), but the query carries
    // connection-init parameters, so it is folded into endpoint.path exactly
    // as --endpoint's own parser already does (parse_endpoint never splits on
    // '?' at all, so a query there just rides along inside path). Assert both
    // that the query survives into endpoint.path and that it reappears when
    // printed the same way main.cpp's --print-msf-urls does.
    {
        const auto options = parse(
            {"prog", "--input", "sample.mp4", "--url",
             "moqt://h.example:4433/moq?token=abc&x=1#msf:a-b--catalog"});
        ok &= expect(options.endpoint.has_value(), "expected --url to produce an endpoint");
        ok &= expect(options.endpoint->path == "/moq?token=abc&x=1",
                     "expected the query folded into endpoint.path, matching --endpoint");
        ok &= expect(options.endpoint->path_explicit,
                     "expected a query-bearing path to be marked explicit");

        const std::string printed = build_track_msf_url(
            options.endpoint->host, options.endpoint->port, options.endpoint->path,
            options.endpoint->path_explicit, options.track_namespace, "catalog",
            ConnectionRequirement::kAny);
        ok &= expect(printed == "moqt://h.example:4433/moq?token=abc&x=1#msf:a-b--catalog",
                     "expected --print-msf-urls output to reproduce the query");
    }

    // A query with no path at all must still fold in, with the endpoint
    // defaulting to path "/" the same way --endpoint would if a path were
    // given explicitly.
    {
        const auto options =
            parse({"prog", "--input", "sample.mp4", "--url", "moqt://h.example?a=1#msf:ns--catalog"});
        ok &= expect(options.endpoint.has_value(), "expected --url to produce an endpoint");
        ok &= expect(options.endpoint->path == "/?a=1",
                     "expected the query folded onto the default '/' path");
        ok &= expect(options.endpoint->path_explicit,
                     "expected a query with no path to still mark the path explicit");
    }

    // Item 2: passing both --url and --namespace is a self-contradiction (the
    // printed/derived namespace would silently disagree with one of the two
    // inputs), so it must be refused in both orders, matching --endpoint's
    // existing treatment.
    ok &= parse_throws(
        {"prog", "--input", "sample.mp4", "--url", "moqt://h/p#msf:ns--t", "--namespace", "zzz"},
        "--namespace", "expected --url followed by --namespace to be refused");
    ok &= parse_throws(
        {"prog", "--input", "sample.mp4", "--namespace", "zzz", "--url", "moqt://h/p#msf:ns--t"},
        "--namespace", "expected --namespace followed by --url to be refused");

    // Item 3: a repeated --url must report its own message rather than
    // blaming --endpoint, which the user never passed.
    ok &= parse_throws(
        {"prog", "--input", "sample.mp4", "--url", "moqt://h/p#msf:ns--t", "--url",
         "moqt://h2/p2#msf:ns2--t2"},
        "already given", "expected a repeated --url to report its own dedicated message");

    return ok ? 0 : 1;
}
