#include "openmoq/publisher/publisher_api.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace openmoq::publisher;

namespace {
std::string read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("missing fixture: " + path.string());
    return {std::istreambuf_iterator<char>(in), {}};
}
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() {
    try {
        const auto input = read(std::filesystem::path(LOCMAF_FIXTURE_DIR).parent_path() / "locmaf-publisher.mp4");
        std::istringstream cmaf_input(input);
        const auto cmaf = Publisher().prepare_stream(cmaf_input, "fixture");
        PublisherConfig config;
        config.media_packaging = MediaPackaging::kLocmaf;
        const PublisherConfig legacy_config{DraftVersion::kDraft18};
        require(legacy_config.media_packaging == MediaPackaging::kCmaf,
                "legacy positional configuration must retain CMAF");
        for (bool per_object : {false, true}) {
            auto invalid = config;
            invalid.live_stream_per_object = per_object;
            invalid.split_cmaf_chunks = per_object;
            bool refused = false;
            try { Publisher rejected(invalid); }
            catch (const std::runtime_error&) { refused = true; }
            require(refused, "API accepted incompatible LOCMAF delivery options");
        }
        std::istringstream locmaf_input(input);
        const auto compact = Publisher(config).prepare_stream(locmaf_input, "fixture");
        const auto catalog = std::find_if(compact.plan.objects.begin(), compact.plan.objects.end(),
            [](const auto& object) { return object.track_name == "catalog"; });
        require(catalog != compact.plan.objects.end(), "missing catalog");
        const std::string json(catalog->owned_payload.begin(), catalog->owned_payload.end());
        require(json.find("\"packaging\":\"locmaf\"") != std::string::npos, "LOCMAF opt-in not applied");
        require(json.find("\"locmafVersion\":\"0.3\"") != std::string::npos, "missing LOCMAF version");
        require(compact.plan.track_initializations[0].init_segment == cmaf.plan.track_initializations[0].init_segment,
                "LOCMAF changed initialization bytes");
        std::size_t media_count = 0;
        for (const auto& object : compact.plan.objects) {
            if (object.kind != CmsfObjectKind::kMedia) continue;
            ++media_count;
            require(!object.owned_payload.empty() && object.owned_payload[0] == 2,
                    "published LOCMAF objects must be independently anchored");
        }
        require(media_count == 2, "lost media chunks");
        require(std::any_of(cmaf.plan.tracks.begin(), cmaf.plan.tracks.end(), [](const auto& track) {
                    return track.handler_type == "vide" && track.packaging == "cmaf";
                }), "default video track no longer uses CMAF");

        // A track-wide exclusion in a later chunk must roll back earlier
        // conversions, including the catalog's packaging declaration.
        auto parsed = parse_mp4_boxes(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(input.data()), input.size()));
        const auto moofs = find_boxes(parsed, "moof");
        require(moofs.size() == 2, "fixture must contain two chunks");
        const auto* traf = find_child_box(*moofs[0], "traf");
        require(traf != nullptr, "fixture must contain traf");
        auto multiplexed = input;
        multiplexed.insert(moofs[0]->span.offset + moofs[0]->span.size,
                           input.substr(traf->span.offset, traf->span.size));
        const auto new_size = static_cast<std::uint32_t>(moofs[0]->span.size + traf->span.size);
        for (unsigned i = 0; i < 4; ++i) {
            multiplexed[moofs[0]->span.offset + i] = static_cast<char>(new_size >> (24 - 8 * i));
        }
        std::istringstream multiplexed_input(multiplexed);
        bool refused_multiplexed = false;
        try { static_cast<void>(Publisher(config).prepare_stream(multiplexed_input, "multiplexed")); }
        catch (const std::runtime_error& error) {
            refused_multiplexed = std::string(error.what()).find("demux") != std::string::npos;
        }
        require(refused_multiplexed, "multiplexed moof must require demux before LOCMAF publication");
        std::string excluded_box(32, '\0');
        excluded_box[3] = 32;
        excluded_box.replace(4, 4, "pssh");
        auto excluded = input;
        excluded.insert(moofs[1]->span.offset, excluded_box);
        std::istringstream excluded_input(excluded);
        const auto fallback = Publisher(config).prepare_stream(excluded_input, "excluded");
        for (const auto& track : fallback.plan.tracks) {
            require(track.packaging != "locmaf", "late exclusion left track partially LOCMAF");
        }
        const auto fallback_bytes = materialize_publish_plan(fallback.plan, fallback.input_bytes);
        for (const auto& object : fallback_bytes.objects) {
            if (object.kind == CmsfObjectKind::kMedia) {
                require(object.owned_payload.size() >= 8 && object.owned_payload[0] == 0,
                        "fallback media is still LOCMAF encoded");
            }
        }

        std::string uuid(24, '\0');
        uuid[3] = 24;
        uuid.replace(4, 4, "uuid");
        auto auxiliary = input;
        auxiliary.insert(moofs[0]->span.offset, uuid);
        std::istringstream auxiliary_input(auxiliary);
        const auto with_auxiliary = Publisher(config).prepare_stream(auxiliary_input, "auxiliary");
        const auto first_media = std::find_if(with_auxiliary.plan.objects.begin(), with_auxiliary.plan.objects.end(),
            [](const auto& object) { return object.kind == CmsfObjectKind::kMedia; });
        require(first_media != with_auxiliary.plan.objects.end() && first_media->owned_payload.front() == 1,
                "pre-moof uuid box was lost rather than carried as genBox");

        const auto out = std::filesystem::temp_directory_path() /
            ("moqxr-locmaf-emission-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Publisher(config).emit_objects(compact, out);
        std::size_t emitted_media = 0;
        for (const auto& file : std::filesystem::directory_iterator(out)) {
            if (file.path().extension() == ".locmafobj") ++emitted_media;
            require(file.path().filename().string().find("_probe.mp4") == std::string::npos,
                    "LOCMAF must not be emitted as an unplayable MP4 probe");
        }
        std::filesystem::remove_all(out);
        require(emitted_media == media_count, "LOCMAF emission extension/count mismatch");

        const auto progressive = read(std::filesystem::path(LOCMAF_FIXTURE_DIR).parent_path() / "locmaf-progressive.mp4");
        std::istringstream progressive_input(progressive);
        const auto remuxed = Publisher(config).prepare_stream(progressive_input, "progressive");
        const auto& init_bytes = remuxed.plan.track_initializations[0].init_segment;
        const auto init_boxes = parse_mp4_boxes(init_bytes);
        const auto* node = find_first_box(init_boxes, "moov");
        for (const auto* type : {"trak", "mdia", "minf", "stbl", "stsz"}) {
            require(node != nullptr, "missing progressive initialization box");
            node = find_child_box(*node, type);
        }
        require(node != nullptr && node->payload.size == 12,
                "progressive initialization retained original sample entries");
        require(std::all_of(init_bytes.begin() + node->payload.offset,
                            init_bytes.begin() + node->payload.offset + node->payload.size,
                            [](auto byte) { return byte == 0; }),
                "progressive initialization retained sample count or size");
        std::cout << "LOCMAF publisher tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
