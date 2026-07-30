#include "openmoq/publisher/cli_options.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using openmoq::publisher::CliOptions;
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

    // --drm-config combined with a live source must be refused: build_live_catalog
    // never calls attach_content_protection, so publishing would produce a
    // catalog with no contentProtections or contentProtectionRefIDs at all --
    // indistinguishable from unprotected content. See docs/status.md.
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

        std::error_code ec;
        std::filesystem::remove(config_path, ec);
    }

    {
        const std::filesystem::path config_path =
            write_drm_config_file("openmoq-drm-config-cli-live-dash-test.json");

        bool threw = false;
        std::string message;
        try {
            parse({"openmoq-publisher", "--live-source", "dash",
                   "--dash-listen", "127.0.0.1:8080",
                   "--endpoint", "https://relay.example.com:443/moq",
                   "--drm-config", config_path.string()});
        } catch (const std::runtime_error& error) {
            threw = true;
            message = error.what();
        }
        ok &= expect(threw, "expected --drm-config with --live-source dash to be refused");
        ok &= expect(message.find("--live-source dash") != std::string::npos,
                     "expected the refusal to name --live-source dash");

        std::error_code ec;
        std::filesystem::remove(config_path, ec);
    }

    {
        const std::filesystem::path config_path =
            write_drm_config_file("openmoq-drm-config-cli-live-stdin-test.json");

        bool threw = false;
        std::string message;
        try {
            parse({"openmoq-publisher", "--input", "-", "--endpoint", "localhost:4443",
                   "--drm-config", config_path.string()});
        } catch (const std::runtime_error& error) {
            threw = true;
            message = error.what();
        }
        ok &= expect(threw, "expected --drm-config with the default live stdin path to be refused");
        ok &= expect(message.find("live stdin") != std::string::npos,
                     "expected the refusal to name the live stdin path");

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
    // live catalog, so --drm-config there is not the misleading case this
    // guard targets.
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

    return ok ? 0 : 1;
}
