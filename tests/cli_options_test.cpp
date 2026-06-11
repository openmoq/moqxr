#include "openmoq/publisher/cli_options.h"

#include <chrono>
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

    return ok ? 0 : 1;
}
