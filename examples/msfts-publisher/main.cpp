#include "msfts_options.h"
#include "msfts_source.h"

#include "openmoq/publisher/publisher_api.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t interrupted = 0;

void handle_signal(int) {
    interrupted = 1;
}

}  // namespace

int main(int argc, char** argv) {
    using openmoq::examples::msfts::MsftsSource;
    using openmoq::examples::msfts::MsftsSourceConfig;
    using openmoq::examples::msfts::msfts_usage;
    using openmoq::examples::msfts::parse_msfts_endpoint;
    using openmoq::examples::msfts::parse_msfts_options;
    using openmoq::publisher::Publisher;
    using openmoq::publisher::PublisherConfig;
    using openmoq::publisher::transport::TlsConfig;

    try {
        std::vector<std::string> arguments;
        arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
        for (int index = 1; index < argc; ++index) {
            arguments.emplace_back(argv[index]);
        }

        const auto options = parse_msfts_options(arguments);
        if (options.help) {
            std::cout << msfts_usage(argv[0]);
            return 0;
        }

        std::signal(SIGINT, handle_signal);

        std::string source_error;
        auto source = MsftsSource::open(
            MsftsSourceConfig{
                .input_path = options.input_path,
                .track_namespace = options.track_namespace,
                .track_name = options.track_name,
                .requested_program = options.program,
                .packets_per_object = options.packets_per_object,
                .stop_requested = []() { return interrupted != 0; },
            },
            source_error);
        if (source == nullptr) {
            std::cerr << "MSFTS source error: " << source_error << '\n';
            return 1;
        }

        const auto endpoint = parse_msfts_endpoint(options.endpoint);
        const auto& info = source->info();
        std::cerr << "Publishing " << options.input_path
                  << " as " << info.packet_size << "-byte source packets"
                  << ", program=" << info.program_number
                  << ", pmt_pid=" << info.pmt_pid
                  << ", pcr_pid=" << info.pcr_pid
                  << ", packets_per_object=" << options.packets_per_object
                  << '\n';

        PublisherConfig config;
        config.draft_version = options.draft;
        config.track_namespace = options.track_namespace;
        config.paced = true;
        Publisher publisher(config);

        TlsConfig tls;
        tls.insecure_skip_verify = options.insecure;

        std::atomic<bool> publish_finished = false;
        std::jthread signal_monitor(
            [&](std::stop_token stop_token) {
                while (!stop_token.stop_requested() &&
                       !publish_finished.load(std::memory_order_acquire)) {
                    if (interrupted != 0) {
                        static_cast<void>(publisher.disconnect());
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            });

        auto live_source = source->live_source();
        const auto status =
            publisher.publish_live_objects(live_source, endpoint, tls);
        publish_finished.store(true, std::memory_order_release);
        signal_monitor.request_stop();

        if (interrupted != 0) {
            std::cerr << "Publishing interrupted\n";
            return 130;
        }
        if (!status.ok) {
            std::cerr << "Publish failed: " << status.message << '\n';
            return 1;
        }
        if (!source->error().empty()) {
            std::cerr << "MSFTS source error: " << source->error() << '\n';
            return 1;
        }

        const auto stats = publisher.stats();
        std::cerr << "Published " << stats.objects_published << " objects ("
                  << stats.bytes_published << " bytes)\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << "\n\n" << msfts_usage(argv[0]);
        return 2;
    }
}
