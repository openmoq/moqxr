#include "openmoq/publisher/cmaf_segmenter.h"
#include "openmoq/publisher/cli_options.h"
#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/endpoint_failover.h"
#include "openmoq/publisher/live_srt_config.h"
#include "openmoq/publisher/live_dash_ingest.h"
#include "openmoq/publisher/msf_url.h"
#include "openmoq/publisher/publisher_api.h"
#include "openmoq/publisher/transport/moqt_session.h"

#include <exception>
#include <chrono>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

#if !defined(_WIN32)
#include <signal.h>
#include <unistd.h>
#endif

namespace {

// How long a SIGTERM/SIGINT stop may take before the process exits anyway.
// Closing waits for in-flight stream data to be acknowledged (about one round
// trip) and then sends CONNECTION_CLOSE; two seconds covers that on any
// working path while keeping a supervisor restart prompt.
constexpr std::chrono::seconds kStopTimeout{2};

// Written by the signal handler and read by the monitor, retry and main
// threads, so it must be atomic; lock-free atomics are async-signal-safe.
std::atomic<int> stop_signal{0};
static_assert(std::atomic<int>::is_always_lock_free);

void handle_stop_signal(int signo) {
    if (stop_signal != 0) {
#if !defined(_WIN32)
        constexpr char kMessage[] = "publisher interrupted again; exiting now\n";
        static_cast<void>(::write(STDERR_FILENO, kMessage, sizeof(kMessage) - 1));
#endif
        std::_Exit(128 + signo);
    }
    stop_signal = signo;
}

void install_stop_signal_handlers() {
#if defined(_WIN32)
    std::signal(SIGINT, handle_stop_signal);
    std::signal(SIGTERM, handle_stop_signal);
#else
    struct sigaction action {};
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
#endif
}

const char* stop_signal_name(int signo) {
    return signo == SIGTERM ? "SIGTERM" : signo == SIGINT ? "SIGINT" : "signal";
}

int interrupted_exit_code() {
    std::cerr << "publisher interrupted by " << stop_signal_name(stop_signal) << std::endl;
    return 128 + stop_signal;
}

// Lets the stop monitor end a DASH ingest server the main thread is blocked
// on, without racing the main thread's own stop and destruction of it.
class DashStopTarget {
public:
    void set(openmoq::publisher::LiveDashIngestServer* server) {
        std::lock_guard<std::mutex> lock(mutex_);
        server_ = server;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (server_ != nullptr) {
            server_->stop();
        }
    }

private:
    std::mutex mutex_;
    openmoq::publisher::LiveDashIngestServer* server_ = nullptr;
};

class DashStopRegistration {
public:
    DashStopRegistration(DashStopTarget& target, openmoq::publisher::LiveDashIngestServer& server)
        : target_(target) {
        target_.set(&server);
    }
    ~DashStopRegistration() { target_.set(nullptr); }

private:
    DashStopTarget& target_;
};

// Watches for SIGTERM/SIGINT. The first signal closes the publisher's
// connection with NO_ERROR from another thread, as the main thread is blocked
// in a publish call, and stops DASH ingest; the publish then returns. If that
// takes longer than kStopTimeout, the process exits from here instead.
// disconnect() repeats until the publish returns, so it also stops a publish
// attempt that started just after the signal.
class StopMonitor {
public:
    StopMonitor(const openmoq::publisher::Publisher& publisher, DashStopTarget& dash)
        : publisher_(publisher), dash_(dash) {
        install_stop_signal_handlers();
        watcher_ = std::jthread([this](std::stop_token token) { watch(token); });
    }

private:
    void watch(std::stop_token token) {
        while (stop_signal == 0) {
            if (token.stop_requested()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto deadline = std::chrono::steady_clock::now() + kStopTimeout;
        std::jthread closer([this](std::stop_token closer_token) {
            while (!closer_token.stop_requested()) {
                static_cast<void>(publisher_.disconnect(0));
                dash_.stop();
                for (int i = 0; i < 10 && !closer_token.stop_requested(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        });
        while (!token.stop_requested()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::cerr << "publisher interrupted by " << stop_signal_name(stop_signal)
                          << "; did not stop within " << kStopTimeout.count()
                          << " s, exiting" << std::endl;
                std::_Exit(128 + stop_signal);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    const openmoq::publisher::Publisher& publisher_;
    DashStopTarget& dash_;
    std::jthread watcher_;
};

std::string endpoint_label(
    const openmoq::publisher::transport::EndpointConfig& endpoint) {
    return endpoint.host + ":" + std::to_string(endpoint.port);
}

openmoq::publisher::transport::TransportStatus publish_with_cli_failover(
    const openmoq::publisher::CliOptions& options,
    const openmoq::publisher::EndpointPublishAttempt& publish_attempt) {
    using namespace openmoq::publisher;

    // A stop request ends the publish: no retry, failover or new attempt.
    return publish_with_failover(
        options.endpoints,
        options.retry_count,
        [&publish_attempt](const transport::EndpointConfig& endpoint) {
            if (stop_signal != 0) {
                return transport::TransportStatus::failure("publisher interrupted",
                                                           transport::FailureKind::kCancelled);
            }
            auto status = publish_attempt(endpoint);
            if (!status.ok && stop_signal != 0) {
                status.failure_kind = transport::FailureKind::kCancelled;
            }
            return status;
        },
        [](std::chrono::milliseconds delay) {
            const auto until = std::chrono::steady_clock::now() + delay;
            while (stop_signal == 0 && std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        },
        [&options](const EndpointFailoverEvent& event) {
            const auto& endpoint = options.endpoints[event.endpoint_index];
            switch (event.kind) {
                case EndpointFailoverEventKind::kAttempt:
                    std::cerr << "endpoint_attempt endpoint=" << endpoint_label(endpoint)
                              << " attempt=" << event.attempt << "/" << event.max_attempts
                              << std::endl;
                    break;
                case EndpointFailoverEventKind::kRetry:
                    std::cerr << "endpoint_retry endpoint=" << endpoint_label(endpoint)
                              << " retry=" << (event.attempt - 1) << "/"
                              << (event.max_attempts - 1)
                              << " delay_ms=" << event.delay.count()
                              << " reason=\"" << event.status.message << "\"" << std::endl;
                    break;
                case EndpointFailoverEventKind::kAdvance:
                    std::cerr << "endpoint_failover from=" << endpoint_label(endpoint)
                              << " to=" << endpoint_label(options.endpoints[event.endpoint_index + 1])
                              << " reason=\"" << event.status.message << "\"" << std::endl;
                    break;
                case EndpointFailoverEventKind::kExhausted:
                    std::cerr << "endpoints_exhausted count=" << options.endpoints.size()
                              << " reason=\"" << event.status.message << "\"" << std::endl;
                    break;
            }
        });
}

}  // namespace

int main(int argc, char** argv) {
    using namespace openmoq::publisher;

    try {
        const CliOptions options = parse_cli_options(argc, argv);

        if (options.show_version) {
            std::cout << build_version_banner() << '\n';
            return 0;
        }

        if (options.print_msf_urls) {
            if (!options.endpoint.has_value()) {
                throw std::runtime_error("--print-msf-urls requires --endpoint or --url");
            }
            const auto connection = !options.transport_explicit
                                        ? ConnectionRequirement::kAny
                                    : options.transport == transport::TransportKind::kWebTransport
                                        ? ConnectionRequirement::kWebTransport
                                        : ConnectionRequirement::kRawQuic;
            for (const auto& endpoint : options.endpoints) {
                std::cout << build_track_msf_url(endpoint.host, endpoint.port,
                                                 endpoint.path, endpoint.path_explicit,
                                                 options.track_namespace, "catalog", connection)
                          << '\n';
            }
            std::cout.flush();
        }

        const bool stdin_source_available = options.input_source.kind == InputSourceKind::kStdin;
        const bool live_srt = options.live_source == LiveSourceKind::kSrt &&
                              options.srt_config_path.has_value() && options.endpoint.has_value();
        const bool live_dash = options.live_source == LiveSourceKind::kDash &&
                               options.dash_listen.has_value() &&
                               (options.endpoint.has_value() || options.dump_plan);
        const bool live_stdin = !live_srt &&
                                !live_dash &&
                                (options.live_source == LiveSourceKind::kAuto ||
                                 options.live_source == LiveSourceKind::kStdin) &&
                                stdin_source_available && options.endpoint.has_value();
        const PublisherConfig config{
            .draft_version = options.draft_version,
            .track_namespace = options.track_namespace,
            .forward = options.forward,
            .publish_catalog = options.publish_catalog,
            .preannounce_tracks = options.preannounce_tracks,
            .include_sap = options.include_sap,
            .include_msf_timeline = options.include_msf_timeline,
            .split_cmaf_chunks = options.split_cmaf_chunks,
            .live_stream_per_object = options.stream_per_object,
            .paced = options.paced,
            .loop = options.loop,
            .libmoq_backend = options.libmoq_backend,
            .vod = options.vod,
            .drm_systems = options.drm_systems,
            .catalog_republish_interval = options.catalog_republish_interval,
            .subscriber_timeout = options.subscriber_timeout,
            .authorization = options.authorization,
            .media_packaging = options.media_packaging,
        };
        Publisher publisher(config);
        DashStopTarget dash_stop;
        const StopMonitor stop_monitor(publisher, dash_stop);

        // --drm-config's laURL/certURL/robustness are deployment fields, not
        // protection detection: detection works on the DASH and stdin live
        // paths regardless of --drm-config, but silently dropping the parsed
        // deployment fields would be a trap for an operator who assumes they
        // reached the catalog. Warn on exactly the paths that cannot apply
        // them; --live-source srt with --drm-config is already refused above
        // in parse_cli_options, so it never reaches here.
        if (!options.drm_systems.empty()) {
            if (live_dash) {
                std::cerr << "warning: --drm-config was parsed (laURL/certURL/robustness) but the "
                             "DASH live path never applies deployment fields to its catalog "
                             "(detection and signalling still work); only the batch/VOD publish "
                             "path applies them."
                          << std::endl;
            } else if (live_stdin) {
                // Not backend-conditional. The libmoq backend passes
                // drm_systems into build_live_catalog but consumes only the
                // returned track_initializations, discarding the built
                // catalog, so the deployment fields never reach the wire
                // there either. Suppressing this warning on that build would
                // leave the one operator whose fields are silently dropped as
                // the only one not told about it.
                std::cerr << "warning: --drm-config was parsed (laURL/certURL/robustness) but the "
                             "stdin live path never applies deployment fields to its catalog on "
                             "either backend (detection and signalling still work); only the "
                             "batch/VOD publish path applies them."
                          << std::endl;
            }
        }

        if (live_dash) {
            std::cerr << "live_source=dash listen=" << options.dash_listen_host
                      << ":" << options.dash_listen_port
                      << " path=" << options.dash_path_prefix << std::endl;
            LiveDashIngestConfig dash_config;
            dash_config.media_packaging = options.media_packaging;
            dash_config.host = options.dash_listen_host;
            dash_config.port = options.dash_listen_port;
            dash_config.path_prefix = options.dash_path_prefix;
            dash_config.queue_depth = options.dash_queue_depth;
            LiveDashIngestServer dash_server(dash_config);
            const DashStopRegistration dash_stop_registration(dash_stop, dash_server);
            const auto start_status = dash_server.start();
            if (!start_status.ok) {
                throw std::runtime_error(start_status.message);
            }
            std::cerr << "dash_ingest_bound_port=" << dash_server.bound_port() << std::endl;
            // Give concurrent encoders a short settle window after the first init
            // segment so multiple paths can be included in the MoQ track snapshot.
            if (!dash_server.wait_for_tracks(std::chrono::seconds(30), std::chrono::milliseconds(500))) {
                dash_server.stop();
                throw std::runtime_error("DASH ingest did not receive any tracks before publishing");
            }
            auto source = dash_server.source();
            if (source.tracks.empty()) {
                dash_server.stop();
                throw std::runtime_error("DASH ingest did not receive any tracks before publishing");
            }
            if (!options.endpoint.has_value()) {
                // Dry run: render the live plan without touching the network.
                std::cout << "live publish plan (dash dry run)\n";
                for (const auto& track : source.tracks) {
                    std::cout << "track name=" << track.track_name << '\n';
                }
                std::cout.flush();
                while (true) {
                    auto object = source.next_object();
                    if (!object.has_value()) {
                        if (!source.is_finished || source.is_finished()) {
                            break;
                        }
                        continue;
                    }
                    std::cout << "object track=" << object->track_name
                              << " group=" << object->group_id
                              << " subgroup=" << object->subgroup_id
                              << " object=" << object->object_id
                              << " bytes=" << object->payload.size()
                              << " time_us=" << object->media_time_us
                              << " duration_us=" << object->media_duration_us
                              << std::endl;
                }
                dash_server.stop();
            } else {
                ReplayableLiveObjectSource replayable_source(
                    std::move(source), options.dash_queue_depth);
                const auto status = publish_with_cli_failover(
                    options,
                    [&](const transport::EndpointConfig& endpoint) {
                        replayable_source.begin_attempt();
                        return publisher.publish_live_objects(
                            replayable_source.source(),
                            endpoint,
                            options.tls,
                            options.endpoint_alpn_overridden);
                    });
                dash_server.stop();
                if (!status.ok && stop_signal == 0) {
                    throw std::runtime_error(status.message);
                }
                const auto disconnect_status = publisher.disconnect(0);
                if (!disconnect_status.ok) {
                    throw std::runtime_error(disconnect_status.message);
                }
            }
        } else if (live_stdin || live_srt) {
            LiveIngestConfig ingest;
            ingest.use_stdin = live_stdin;

            if (live_srt) {
                std::cerr << "live_source=srt" << std::endl;
                const LiveSrtConfig srt_config = parse_live_srt_config_file(*options.srt_config_path);
                for (const auto& caller : srt_config.srt_callers) {
                    LiveSrtCaller live_caller;
                    live_caller.id = caller.id;
                    live_caller.endpoint = caller.srt.host + ":" + std::to_string(caller.srt.port);
                    live_caller.mode = caller.srt.mode;
                    live_caller.fragment_on_keyframe = caller.cmaf.fragment_on_keyframe;
                    live_caller.empty_moov = caller.cmaf.empty_moov;
                    live_caller.default_base_moof = caller.cmaf.default_base_moof;
                    live_caller.separate_moof_per_track = caller.cmaf.separate_moof_per_track;
                    live_caller.target_fragment_duration_ms = caller.cmaf.target_fragment_duration_ms;
                    live_caller.latency_ms = caller.srt.latency_ms;
                    live_caller.auto_detect_program = caller.mpegts.auto_detect_program;
                    live_caller.program_number = caller.mpegts.program_number;
                    live_caller.video_pid = caller.mpegts.video_pid;
                    live_caller.audio_pid = caller.mpegts.audio_pid;
                    ingest.srt_callers.push_back(std::move(live_caller));
                }
            } else {
                std::cerr << "live_source=stdin" << std::endl;
            }

            std::istream* stdin_input = live_stdin ? &std::cin : nullptr;
            const auto status = publish_with_cli_failover(
                options,
                [&](const transport::EndpointConfig& endpoint) {
                    return publisher.publish_live(
                        ingest,
                        stdin_input,
                        endpoint,
                        options.tls,
                        options.endpoint_alpn_overridden);
                });
            if (!status.ok && stop_signal == 0) {
                throw std::runtime_error(status.message);
            }
            const auto disconnect_status = publisher.disconnect(0);
            if (!disconnect_status.ok) {
                throw std::runtime_error(disconnect_status.message);
            }
        } else {
            // Original batch mode: read entire file/stdin, segment, plan, publish.
            const PreparedPublish prepared = options.input_source.kind == InputSourceKind::kStdin
                                                 ? publisher.prepare_stream(std::cin, "stdin")
                                                 : publisher.prepare_file(options.input_source.path);

            if (options.dump_plan || !options.emit_dir.has_value()) {
                std::cout << publisher.render_plan(prepared);
            }

            if (options.emit_dir.has_value()) {
                publisher.emit_objects(prepared, *options.emit_dir);
            }

            if (options.endpoint.has_value()) {
                const auto status = publish_with_cli_failover(
                    options,
                    [&](const transport::EndpointConfig& endpoint) {
                        return publisher.publish(
                            prepared,
                            endpoint,
                            options.tls,
                            options.endpoint_alpn_overridden);
                    });
                if (!status.ok && stop_signal == 0) {
                    throw std::runtime_error(status.message);
                }
                const auto disconnect_status = publisher.disconnect(0);
                if (!disconnect_status.ok) {
                    throw std::runtime_error(disconnect_status.message);
                }
            }
        }
    } catch (const std::exception& exception) {
        if (stop_signal != 0) {
            return interrupted_exit_code();
        }
        if (std::string_view(exception.what()).empty()) {
            std::cerr << build_usage(argv[0]) << '\n';
            return 0;
        }

        std::cerr << "error: " << exception.what() << '\n';
        std::cerr << build_usage(argv[0]) << '\n';
        return 1;
    }

    if (stop_signal != 0) {
        return interrupted_exit_code();
    }
    return 0;
}
