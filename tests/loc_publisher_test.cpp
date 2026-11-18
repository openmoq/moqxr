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
        check(config.draft_version == DraftVersion::kDraft22, "publisher defaults to draft22");
        config.draft_version = DraftVersion::kDraft16;
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
                check(property(object, 8) && property(object, 16), "missing LOC05 timing");
                check(!property(object, 2) && !property(object, 4) && !property(object, 6), "legacy LOC IDs escaped");
                check(!object.owned_payload.empty(), "empty LOC media");
                const auto media_track = std::find_if(prepared.plan.tracks.begin(), prepared.plan.tracks.end(),
                    [&](const auto& track) { return track.track_name == object.track_name; });
                const auto* codec = property(object, 17);
                check(media_track != prepared.plan.tracks.end() && !media_track->codec.empty() && codec &&
                      std::get<std::vector<std::uint8_t>>(codec->value) ==
                          std::vector<std::uint8_t>(media_track->codec.begin(), media_track->codec.end()),
                      "LOC05 codec string must match catalog codec");
                if (std::string(name).find("publisher") != std::string::npos) {
                    const auto* marking = property(object, 9);
                    check(marking && std::holds_alternative<std::vector<std::uint8_t>>(marking->value), "LOC05 marking must be bytes");
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
        {
            // HEVC publishes through the same sample-to-object mapping with an hvcC Video Config.
            std::istringstream input(fixture("loc-hevc-fragmented.mp4"));
            auto prepared = Publisher(config).prepare_stream(input, "loc-hevc-fragmented.mp4");
            std::istringstream parse_input(fixture("loc-hevc-fragmented.mp4"));
            const auto hevc_parsed = parse_mp4_stream(parse_input, "hevc");
            const auto hevc_config = loc_codec_config(hevc_parsed.tracks.front());
            std::size_t objects = 0;
            for (const auto& object : prepared.plan.objects) {
                if (object.kind != CmsfObjectKind::kMedia) continue;
                ++objects;
                const auto* video_config = property(object, 13);
                check(video_config && std::get<std::vector<std::uint8_t>>(video_config->value) == hevc_config,
                      "HEVC Video Config must be the hvcC payload");
                const auto* codec = property(object, 17);
                check(codec && std::get<std::vector<std::uint8_t>>(codec->value).size() > 5 &&
                          std::get<std::vector<std::uint8_t>>(codec->value)[0] == 'h',
                      "HEVC codec string missing");
                const auto* marking = property(object, 9);
                const bool keyframe = (objects - 1) % 4 == 0;
                check(marking && std::get<std::vector<std::uint8_t>>(marking->value) ==
                          std::vector<std::uint8_t>{static_cast<std::uint8_t>(keyframe ? 0xe0 : 0xc0)},
                      "HEVC frame marking follows bitstream keyframes");
                check(object.group_id == (objects - 1) / 4 && object.object_id == (objects - 1) % 4, "HEVC GOP grouping");
            }
            check(objects == 8, "HEVC fixture publishes every sample");
        }
        {
            // Opus: one packet per group, OpusHead as Audio Config, "opus" as Codec String.
            std::istringstream input(fixture("loc-opus.mp4"));
            auto prepared = Publisher(config).prepare_stream(input, "loc-opus.mp4");
            std::size_t objects = 0;
            for (const auto& object : prepared.plan.objects) {
                if (object.kind != CmsfObjectKind::kMedia) continue;
                ++objects;
                const auto* audio_config = property(object, 15);
                check(audio_config && std::get<std::vector<std::uint8_t>>(audio_config->value).size() == 19 &&
                          std::string(std::get<std::vector<std::uint8_t>>(audio_config->value).begin(),
                                      std::get<std::vector<std::uint8_t>>(audio_config->value).begin() + 8) == "OpusHead",
                      "Opus Audio Config must be an OpusHead");
                const auto* codec = property(object, 17);
                check(codec && std::get<std::vector<std::uint8_t>>(codec->value) == std::vector<std::uint8_t>{'o', 'p', 'u', 's'},
                      "Opus Codec String");
                check(!property(object, 9) && !property(object, 13), "Opus carries no video properties");
                check(object.object_id == 0 && object.group_id == objects - 1, "Opus one sample per group");
            }
            check(objects >= 40, "Opus fixture publishes every packet");
        }
        {
            // AV1: av1C Video Config, "av01." Codec String, bitstream-derived frame marking.
            std::istringstream input(fixture("loc-av1-fragmented.mp4"));
            auto prepared = Publisher(config).prepare_stream(input, "loc-av1-fragmented.mp4");
            std::size_t objects = 0;
            for (const auto& object : prepared.plan.objects) {
                if (object.kind != CmsfObjectKind::kMedia) continue;
                ++objects;
                const auto* video_config = property(object, 13);
                check(video_config && std::get<std::vector<std::uint8_t>>(video_config->value)[0] == 0x81, "AV1 Video Config must be the av1C payload");
                const auto* codec = property(object, 17);
                const auto& codec_bytes = std::get<std::vector<std::uint8_t>>(codec->value);
                check(codec && std::string(codec_bytes.begin(), codec_bytes.begin() + 5) == "av01.", "AV1 Codec String");
                const auto* marking = property(object, 9);
                check(marking && std::get<std::vector<std::uint8_t>>(marking->value) ==
                          std::vector<std::uint8_t>{static_cast<std::uint8_t>((objects - 1) % 4 == 0 ? 0xe0 : 0xc0)},
                      "AV1 frame marking follows bitstream keyframes");
                check(object.group_id == (objects - 1) / 4 && object.object_id == (objects - 1) % 4, "AV1 GOP grouping");
            }
            check(objects == 12, "AV1 fixture publishes every temporal unit");
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
