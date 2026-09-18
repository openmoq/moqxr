#include "openmoq/publisher/publisher_api.h"
#include "../src/loc_packager.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace openmoq::publisher;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::string fixture(const char* name) {
    std::ifstream stream(std::filesystem::path(__FILE__).parent_path() / "fixtures" / name, std::ios::binary);
    if (!stream) throw std::runtime_error("missing LOC fixture");
    return {std::istreambuf_iterator<char>(stream), {}};
}
const ObjectProperty* property(const CmsfObject& object, std::uint64_t id) {
    const auto it = std::find_if(object.properties.begin(), object.properties.end(), [id](const auto& p) { return p.id == id; });
    return it == object.properties.end() ? nullptr : &*it;
}
}
int main() {
    try {
        PublisherConfig config;
        config.media_packaging = MediaPackaging::kLoc;
        bool rejected = false;
        try { Publisher invalid(config); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "LOC must reject draft16 at API boundary");
        config.draft_version = DraftVersion::kDraft18;
        for (const char* name : {"locmaf-publisher.mp4", "locmaf-audio.mp4"}) {
            std::istringstream input(fixture(name));
            auto prepared = Publisher(config).prepare_stream(input, name);
            const auto catalog = std::find_if(prepared.plan.objects.begin(), prepared.plan.objects.end(),
                [](const auto& object) { return object.track_name == "catalog"; });
            check(catalog != prepared.plan.objects.end(), "missing LOC catalog");
            std::string json(catalog->owned_payload.begin(), catalog->owned_payload.end());
            check(json.find("\"packaging\":\"loc\"") != std::string::npos, "catalog must signal loc");
            check(json.find("locmafVersion") == std::string::npos, "LOC must not advertise locmafVersion");
            std::size_t samples = 0;
            for (const auto& object : prepared.plan.objects) {
                if (object.kind != CmsfObjectKind::kMedia) continue;
                ++samples;
                check(property(object, 8) && property(object, 16), "missing LOC04 timing");
                check(!property(object, 2) && !property(object, 4) && !property(object, 6), "legacy LOC IDs escaped");
                check(!object.owned_payload.empty(), "empty LOC media");
                if (std::string(name).find("publisher") != std::string::npos) {
                    const auto* marking = property(object, 9);
                    check(marking && std::holds_alternative<std::vector<std::uint8_t>>(marking->value), "LOC04 marking must be bytes");
                    check(property(object, 13), "video config absent");
                    check(object.group_id == (samples - 1) / 2 && object.object_id == (samples - 1) % 2, "video GOP ordering");
                } else {
                    check(property(object, 15), "audio config absent");
                    check(object.object_id == 0 && object.group_id == samples - 1, "audio object grouping");
                }
            }
            check(samples >= 4, "chunks were not split into samples");
            for (const auto& track : prepared.plan.tracks) {
                if (track.handler_type == "vide" || track.handler_type == "soun") check(track.packaging == "loc", "media mislabeled");
            }
        }
        // A missing decode interval invalidates dependent video until a new IDR.
        std::istringstream video_input(fixture("locmaf-publisher.mp4"));
        auto parsed = parse_mp4_stream(video_input, "video");
        auto samples = read_encoded_samples(parsed);
        const auto track = std::find_if(parsed.tracks.begin(), parsed.tracks.end(),
            [](const auto& item) { return item.handler_type == "vide"; });
        check(track != parsed.tracks.end(), "missing video track");
        LocTrackEncoder encoder(*track);
        auto first = samples.front();
        first.independent = true;
        auto push = [&encoder](const EncodedSample& sample) {
            return encoder.encode(std::span<const EncodedSample>(&sample, 1));
        };
        check(push(first).size() == 1, "initial IDR rejected");
        auto gap = first;
        gap.decode_time += 2 * first.duration;
        gap.presentation_time = static_cast<std::int64_t>(gap.decode_time);
        gap.independent = false;
        check(push(gap).empty(), "dependent frame after gap was published");
        gap.decode_time += gap.duration;
        gap.presentation_time = static_cast<std::int64_t>(gap.decode_time);
        gap.independent = true;
        auto resumed = push(gap);
        check(resumed.size() == 1 && resumed.front().group_id == 1 && resumed.front().object_id == 0,
              "recovery must start a fresh group");
        rejected = false;
        try { push(gap); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "decode regression accepted");
        LocTrackEncoder negative(*track);
        first.presentation_time = -1;
        rejected = false;
        try { negative.encode(std::span<const EncodedSample>(&first, 1)); }
        catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "negative presentation timestamp accepted");
        config.live_stream_per_object = true;
        rejected = false;
        try { Publisher invalid(config); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "LOC incompatible delivery accepted");
        std::cout << "LOC publisher tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
