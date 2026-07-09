#include "openmoq/publisher/cmaf_segmenter.h"
#include "openmoq/publisher/cli_options.h"
#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/live_srt_config.h"
#include "openmoq/publisher/publisher_api.h"
#include "openmoq/publisher/transport/moqt_session.h"

#include <exception>
#include <iostream>
#include <utility>

int main(int argc, char** argv) {
    using namespace openmoq::publisher;

    try {
        const CliOptions options = parse_cli_options(argc, argv);

        const bool stdin_source_available = options.input_source.kind == InputSourceKind::kStdin;
        const bool live_srt = options.live_source == LiveSourceKind::kSrt &&
                              options.srt_config_path.has_value() && options.endpoint.has_value();
        const bool live_stdin = !live_srt &&
                                (options.live_source == LiveSourceKind::kAuto ||
                                 options.live_source == LiveSourceKind::kStdin) &&
                                stdin_source_available && options.endpoint.has_value();
        const PublisherConfig config{
            .draft_version = options.draft_version,
            .track_namespace = options.track_namespace,
            .forward = options.forward,
            .publish_catalog = options.publish_catalog,
            .include_sap = options.include_sap,
            .include_msf_timeline = options.include_msf_timeline,
            .split_cmaf_chunks = options.split_cmaf_chunks,
            .live_stream_per_object = options.stream_per_object,
            .paced = options.paced,
            .loop = options.loop,
            .subscriber_timeout = options.subscriber_timeout,
        };
        Publisher publisher(config);

        if (live_stdin || live_srt) {
            LiveIngestConfig ingest;
            ingest.use_stdin = live_stdin;

            if (live_srt) {
                std::cerr << "live_source=srt" << std::endl;
                const LiveSrtConfig srt_config = parse_live_srt_config_file(*options.srt_config_path);
                for (const auto& caller : srt_config.srt_callers) {
                    LiveSrtCaller live_caller;
                    live_caller.id = caller.id;
                    live_caller.endpoint = caller.srt.host + ":" + std::to_string(caller.srt.port);
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
            const auto status = publisher.publish_live(
                ingest,
                stdin_input,
                *options.endpoint,
                options.tls,
                options.endpoint_alpn_overridden);
            if (!status.ok) {
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
                const auto status = publisher.publish(
                    prepared,
                    *options.endpoint,
                    options.tls,
                    options.endpoint_alpn_overridden);
                if (!status.ok) {
                    throw std::runtime_error(status.message);
                }
                const auto disconnect_status = publisher.disconnect(0);
                if (!disconnect_status.ok) {
                    throw std::runtime_error(disconnect_status.message);
                }
            }
        }
    } catch (const std::exception& exception) {
        if (std::string_view(exception.what()).empty()) {
            std::cerr << build_usage(argv[0]) << '\n';
            return 0;
        }

        std::cerr << "error: " << exception.what() << '\n';
        std::cerr << build_usage(argv[0]) << '\n';
        return 1;
    }

    return 0;
}
