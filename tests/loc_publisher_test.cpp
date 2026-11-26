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
        {
            // VP9: no Video Config (WebCodecs defines no description), Codec String and marking still carried.
            std::istringstream input(fixture("loc-vp9-fragmented.mp4"));
            auto prepared = Publisher(config).prepare_stream(input, "loc-vp9-fragmented.mp4");
            std::size_t objects = 0;
            for (const auto& object : prepared.plan.objects) {
                if (object.kind != CmsfObjectKind::kMedia) continue;
                ++objects;
                check(!property(object, 13), "VP9 objects must not carry a Video Config");
                const auto* codec = property(object, 17);
                check(codec && std::get<std::vector<std::uint8_t>>(codec->value) ==
                          std::vector<std::uint8_t>({'v', 'p', '0', '9', '.', '0', '0', '.', '1', '0', '.', '0', '8'}),
                      "VP9 Codec String");
                const auto* marking = property(object, 9);
                check(marking && std::get<std::vector<std::uint8_t>>(marking->value) ==
                          std::vector<std::uint8_t>{static_cast<std::uint8_t>((objects - 1) % 4 == 0 ? 0xe0 : 0xc0)},
                      "VP9 frame marking follows bitstream keyframes");
                check(object.group_id == (objects - 1) / 4 && object.object_id == (objects - 1) % 4, "VP9 GOP grouping");
            }
            check(objects == 12, "VP9 fixture publishes every frame");
            const auto catalog = std::find_if(prepared.plan.objects.begin(), prepared.plan.objects.end(),
                [](const auto& object) { return object.track_name == "catalog"; });
            const std::string json(catalog->owned_payload.begin(), catalog->owned_payload.end());
            check(json.find("\"codec\":\"vp09.00.10.08\"") != std::string::npos && json.find("initRef") == std::string::npos,
                  "VP9 catalog carries the codec string and no init data");
        }
        {
            // MSF-02 publishes LOC decoder config as track-property init entries; MSF-01 stays inline.
            const auto catalog_json = [&](const char* name, MsfVersion version, MediaPackaging packaging) {
                auto local = config;
                local.msf_version = version;
                local.media_packaging = packaging;
                std::istringstream input(fixture(name));
                auto prepared = Publisher(local).prepare_stream(input, name);
                const auto catalog = std::find_if(prepared.plan.objects.begin(), prepared.plan.objects.end(),
                    [](const auto& object) { return object.track_name == "catalog"; });
                return std::string(catalog->owned_payload.begin(), catalog->owned_payload.end());
            };
            const auto count = [](const std::string& text, const std::string& needle) {
                std::size_t found = 0;
                for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++found;
                return found;
            };
            const auto hevc02 = catalog_json("loc-hevc-fragmented.mp4", MsfVersion::k02, MediaPackaging::kLoc);
            check(hevc02.find("\"version\":\"draft-02\"") != std::string::npos, "default LOC catalog is draft-02");
            check(hevc02.find("\"type\":\"track-property\"") != std::string::npos &&
                      hevc02.find("\"data\":\"0x0D\"") != std::string::npos &&
                      hevc02.find("\"type\":\"inline\"") == std::string::npos,
                  "LOC video config is a track-property entry naming VIDEO_CONFIG");
            check(hevc02.find("\"framerate\"") != std::string::npos && hevc02.find("\"bitrate\"") != std::string::npos,
                  "LOC video track carries the Table 6 fields");
            const auto opus02 = catalog_json("loc-opus.mp4", MsfVersion::k02, MediaPackaging::kLoc);
            check(opus02.find("\"type\":\"track-property\"") != std::string::npos && opus02.find("\"data\":\"0x0F\"") != std::string::npos &&
                      opus02.find("\"channelConfig\"") != std::string::npos &&
                      opus02.find("\"samplerate\"") != std::string::npos,
                  "LOC audio config is a track-property entry naming AUDIO_CONFIG with the Table 6 fields");
            const auto vp902 = catalog_json("loc-vp9-fragmented.mp4", MsfVersion::k02, MediaPackaging::kLoc);
            check(vp902.find("initRef") == std::string::npos && vp902.find("initDataList") == std::string::npos &&
                      vp902.find("\"codec\":\"vp09.00.10.08\"") != std::string::npos,
                  "VP9 has no init reference under 02");
            const auto hevc01 = catalog_json("loc-hevc-fragmented.mp4", MsfVersion::k01, MediaPackaging::kLoc);
            check(hevc01.find("\"version\":\"1\"") != std::string::npos && hevc01.find("\"type\":\"inline\"") != std::string::npos &&
                      hevc01.find("-property\"") == std::string::npos,
                  "MSF-01 keeps inline LOC init data");
            const auto cmaf02 = catalog_json("locmaf-publisher.mp4", MsfVersion::k02, MediaPackaging::kCmaf);
            check(count(cmaf02, "\"type\":\"inline\"") >= 1 && cmaf02.find("-property\"") == std::string::npos,
                  "CMAF keeps inline init data under 02");
            const auto locmaf02 = catalog_json("locmaf-publisher.mp4", MsfVersion::k02, MediaPackaging::kLocmaf);
            check(count(locmaf02, "\"type\":\"inline\"") >= 1 && locmaf02.find("-property\"") == std::string::npos,
                  "LOCMAF keeps inline init data under 02");
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
