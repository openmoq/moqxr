#include "openmoq/publisher/msf_catalog.h"
#include "openmoq/publisher/mp4_box.h"

#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

bool expect_contains(std::string_view haystack, std::string_view needle, const std::string& message) {
    return expect(haystack.find(needle) != std::string_view::npos, message);
}

bool expect_not_contains(std::string_view haystack, std::string_view needle, const std::string& message) {
    return expect(haystack.find(needle) == std::string_view::npos, message);
}

bool throws_runtime_error(const openmoq::publisher::MsfCatalog& catalog, const std::string& message) {
    try {
        openmoq::publisher::serialize_catalog(catalog);
        std::cerr << "FAIL: " << message << " (expected std::runtime_error but none was thrown)\n";
        return false;
    } catch (const std::runtime_error&) {
        return true;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << message << " (expected std::runtime_error but got: " << e.what() << ")\n";
        return false;
    }
}

}  // namespace

int main() {
    using namespace openmoq::publisher;

    bool ok = true;

    // MSF section 5.1.1: version is a String, not a Number.
    MsfCatalog minimal;
    MsfTrack video;
    video.name = "video";
    video.packaging = "cmaf";
    video.role = "video";
    video.is_live = true;
    video.codec = "avc1.640028";
    video.bitrate = 5000000;
    video.width = 1920;
    video.height = 1080;
    video.framerate = 30.0;
    minimal.tracks.push_back(video);

    const std::string json = serialize_catalog(minimal);
    ok &= expect_contains(json, "\"version\":\"1\"", "expected version as a JSON string");
    ok &= expect_not_contains(json, "\"version\":1", "expected no numeric version");
    ok &= expect_not_contains(json, "\"format\"", "expected no non-spec format field");
    ok &= expect_not_contains(json, "\"id\":", "expected no non-spec track id field");
    ok &= expect_contains(json, "\"packaging\":\"cmaf\"", "expected cmaf packaging");
    ok &= expect_contains(json, "\"bitrate\":5000000", "expected bitrate");
    ok &= expect_contains(json, "\"framerate\":30", "expected spec framerate spelling");
    ok &= expect_not_contains(json, "\"frameRate\"", "expected no legacy frameRate spelling");

    // Test: duplicate name within publishTracks
    MsfCatalog dup_pub;
    MsfTrack pub1;
    pub1.name = "pub";
    pub1.packaging = "cmaf";
    pub1.is_live = true;
    MsfTrack pub2;
    pub2.name = "pub";
    pub2.packaging = "cmaf";
    pub2.is_live = true;
    dup_pub.publish_tracks.push_back(pub1);
    dup_pub.publish_tracks.push_back(pub2);
    ok &= throws_runtime_error(dup_pub, "duplicate name in publishTracks should throw");

    // Test: dangling initRef in publishTracks
    MsfCatalog dangling_pub;
    MsfTrack pub_track;
    pub_track.name = "pub_with_ref";
    pub_track.packaging = "cmaf";
    pub_track.is_live = true;
    pub_track.init_ref = "nonexistent";
    dangling_pub.publish_tracks.push_back(pub_track);
    ok &= throws_runtime_error(dangling_pub, "dangling initRef in publishTracks should throw");

    // Test: cross-array name collision (same name+namespace in both tracks and publishTracks)
    MsfCatalog cross_collision;
    MsfTrack track1;
    track1.name = "shared";
    track1.packaging = "cmaf";
    track1.is_live = true;
    MsfTrack track2;
    track2.name = "shared";
    track2.packaging = "cmaf";
    track2.is_live = true;
    cross_collision.tracks.push_back(track1);
    cross_collision.publish_tracks.push_back(track2);
    ok &= throws_runtime_error(cross_collision, "cross-array name collision should throw");

    // Test: custom field named "connectionUri" should be rejected (spec-name collision)
    MsfCatalog uri_collision;
    MsfTrack uri_track;
    uri_track.name = "uri_test";
    uri_track.packaging = "cmaf";
    uri_track.is_live = true;
    uri_track.custom_fields["connectionUri"] = "\"moqt://example.com\"";
    uri_collision.tracks.push_back(uri_track);
    ok &= throws_runtime_error(uri_collision, "custom field \"connectionUri\" should collide with spec name");

    // CMSF section 5.1: simulcast video with a shared altGroup, plus audio,
    // with initialization data carried by initDataList and referenced by initRef.
    MsfCatalog simulcast;
    simulcast.generated_at_ms = 1746104606044ULL;

    for (const auto& quality : {std::string("hd"), std::string("md"), std::string("sd")}) {
        MsfTrack alt;
        alt.name = quality;
        alt.packaging = "cmaf";
        alt.role = "video";
        alt.is_live = true;
        alt.render_group = 1;
        alt.alt_group = 1;
        alt.target_latency_ms = 2000;
        alt.init_ref = "init-" + quality;
        alt.codec = "avc1.640028";
        alt.bitrate = 5000000;
        alt.framerate = 30.0;
        alt.width = 1920;
        alt.height = 1080;
        simulcast.tracks.push_back(alt);
        simulcast.init_data_list.push_back(MsfInitData{.id = "init-" + quality,
                                                       .type = "inline",
                                                       .data = "AAAAHGZ0eXBjbWYy"});
    }

    MsfTrack audio;
    audio.name = "audio";
    audio.packaging = "cmaf";
    audio.role = "audio";
    audio.is_live = true;
    audio.render_group = 1;
    audio.target_latency_ms = 2000;
    audio.init_ref = "init-audio";
    audio.codec = "mp4a.40.5";
    audio.bitrate = 67071;
    audio.samplerate = 48000;
    audio.channel_config = "2";
    simulcast.tracks.push_back(audio);
    simulcast.init_data_list.push_back(MsfInitData{.id = "init-audio",
                                                   .type = "inline",
                                                   .data = "AAAAHGZ0eXBjbWYy"});

    const std::string simulcast_json = serialize_catalog(simulcast);
    ok &= expect_contains(simulcast_json, "\"generatedAt\":1746104606044", "expected generatedAt");
    ok &= expect_contains(simulcast_json, "\"altGroup\":1", "expected shared altGroup");
    ok &= expect_contains(simulcast_json, "\"initRef\":\"init-hd\"", "expected initRef");
    ok &= expect_contains(simulcast_json, "\"channelConfig\":\"2\"", "expected channelConfig as a string");
    ok &= expect_not_contains(simulcast_json, "\"channelCount\"", "expected no legacy channelCount");
    ok &= expect_contains(simulcast_json, "\"samplerate\":48000", "expected spec samplerate spelling");
    ok &= expect_not_contains(simulcast_json, "\"sampleRate\"", "expected no legacy sampleRate spelling");
    ok &= expect_not_contains(simulcast_json, "\"initData\"", "expected no legacy inline initData");

    // MSF section 5.1.7: initDataList MUST be located after the tracks array.
    const std::size_t tracks_pos = simulcast_json.find("\"tracks\"");
    const std::size_t init_list_pos = simulcast_json.find("\"initDataList\"");
    ok &= expect(tracks_pos != std::string::npos && init_list_pos != std::string::npos,
                 "expected both tracks and initDataList present");
    ok &= expect(tracks_pos < init_list_pos, "expected initDataList to follow tracks");

    // MSF section 5.1.3: isComplete MUST NOT be included when false.
    MsfCatalog not_complete = simulcast;
    not_complete.is_complete = false;
    ok &= expect_not_contains(serialize_catalog(not_complete), "\"isComplete\"",
                              "expected isComplete omitted when false");

    // CMSF section 3.6: SAP event timeline track carries eventType and the
    // CMSF max SAP starting types (section 3.5.2).
    MsfCatalog with_sap;
    MsfTrack sap;
    sap.name = "video_sap";
    sap.packaging = "eventtimeline";
    sap.role = "eventtimeline";
    sap.is_live = true;
    sap.event_type = "org.ietf.moq.cmsf.sap";
    sap.mime_type = "application/json";
    sap.depends = {"video"};
    with_sap.tracks.push_back(sap);

    MsfTrack sapped_video;
    sapped_video.name = "video";
    sapped_video.packaging = "cmaf";
    sapped_video.role = "video";
    sapped_video.is_live = true;
    sapped_video.codec = "avc1.640028";
    sapped_video.bitrate = 5000000;
    sapped_video.max_grp_sap_starting_type = 2;
    sapped_video.max_obj_sap_starting_type = 3;
    with_sap.tracks.push_back(sapped_video);

    const std::string sap_json = serialize_catalog(with_sap);
    ok &= expect_contains(sap_json, "\"eventType\":\"org.ietf.moq.cmsf.sap\"", "expected SAP eventType");
    ok &= expect_contains(sap_json, "\"depends\":[\"video\"]", "expected depends array");
    ok &= expect_contains(sap_json, "\"maxGrpSapStartingType\":2", "expected maxGrpSapStartingType");
    ok &= expect_contains(sap_json, "\"maxObjSapStartingType\":3", "expected maxObjSapStartingType");

    // Validation: bitrate is a MUST for audio and video (section 5.2.22).
    MsfCatalog no_bitrate;
    MsfTrack bare_video;
    bare_video.name = "video";
    bare_video.packaging = "cmaf";
    bare_video.role = "video";
    bare_video.is_live = true;
    bare_video.codec = "avc1.640028";
    no_bitrate.tracks.push_back(bare_video);
    ok &= throws_runtime_error(no_bitrate, "expected throw when video track omits bitrate");

    // Validation: eventType outside eventtimeline packaging (section 5.2.5).
    MsfCatalog stray_event_type;
    MsfTrack stray = sapped_video;
    stray.event_type = "com.example.bogus";
    stray_event_type.tracks.push_back(stray);
    ok &= throws_runtime_error(stray_event_type, "expected throw for eventType on cmaf packaging");

    // Validation: trackDuration on a live track (section 5.2.35).
    MsfCatalog live_with_duration;
    MsfTrack durated = sapped_video;
    durated.track_duration_ms = 60000;
    live_with_duration.tracks.push_back(durated);
    ok &= throws_runtime_error(live_with_duration, "expected throw for trackDuration on a live track");

    // Validation: audio requires samplerate and channelConfig (5.2.28, 5.2.29).
    MsfCatalog bare_audio_catalog;
    MsfTrack bare_audio;
    bare_audio.name = "audio";
    bare_audio.packaging = "cmaf";
    bare_audio.role = "audio";
    bare_audio.is_live = true;
    bare_audio.codec = "mp4a.40.2";
    bare_audio.bitrate = 128000;
    bare_audio_catalog.tracks.push_back(bare_audio);
    ok &= throws_runtime_error(bare_audio_catalog, "expected throw when audio omits samplerate");

    // Same check, but with samplerate present and only channelConfig absent,
    // so a regression that dropped only the channelConfig half of the check
    // (5.2.29) could not pass by leaving samplerate absent too.
    MsfCatalog audio_missing_channel_config_catalog;
    MsfTrack audio_missing_channel_config = bare_audio;
    audio_missing_channel_config.samplerate = 48000;
    audio_missing_channel_config_catalog.tracks.push_back(audio_missing_channel_config);
    ok &= throws_runtime_error(audio_missing_channel_config_catalog,
                               "expected throw when audio has samplerate but omits channelConfig");

    // Validation: targetLatency and buffers are mutually exclusive (5.2.8, 5.2.9).
    MsfCatalog both_latency_forms;
    MsfTrack conflicted = sapped_video;
    conflicted.target_latency_ms = 2000;
    conflicted.buffers = MsfBuffers{.target_ms = 1500, .min_ms = std::nullopt, .max_ms = std::nullopt};
    both_latency_forms.tracks.push_back(conflicted);
    ok &= throws_runtime_error(both_latency_forms, "expected throw for targetLatency alongside buffers");

    // Validation: initRef must resolve to an initDataList entry (5.2.13).
    MsfCatalog dangling_ref;
    MsfTrack unresolved = sapped_video;
    unresolved.init_ref = "missing-init";
    dangling_ref.tracks.push_back(unresolved);
    ok &= throws_runtime_error(dangling_ref, "expected throw for initRef with no initDataList entry");

    // Validation: track names must be unique per namespace (5.2.3).
    MsfCatalog duplicated;
    duplicated.tracks.push_back(sapped_video);
    duplicated.tracks.push_back(sapped_video);
    ok &= throws_runtime_error(duplicated, "expected throw for duplicate track names in one namespace");

    // The same name in a different namespace is legal.
    MsfCatalog distinct_namespaces;
    MsfTrack ns_a = sapped_video;
    ns_a.name_space = "example.com/a";
    MsfTrack ns_b = sapped_video;
    ns_b.name_space = "example.com/b";
    distinct_namespaces.tracks.push_back(ns_a);
    distinct_namespaces.tracks.push_back(ns_b);
    ok &= expect_contains(serialize_catalog(distinct_namespaces), "\"namespace\":\"example.com/b\"",
                          "expected identical names in distinct namespaces to serialize");

    // Buffers serialize as an object with only the keys that are set.
    MsfCatalog buffered;
    MsfTrack with_buffers = sapped_video;
    with_buffers.buffers = MsfBuffers{.target_ms = 1500, .min_ms = 800, .max_ms = std::nullopt};
    buffered.tracks.push_back(with_buffers);
    const std::string buffered_json = serialize_catalog(buffered);
    ok &= expect_contains(buffered_json, "\"buffers\":{\"target\":1500,\"min\":800}",
                          "expected buffers object with only the set keys");

    // Section 5: custom fields are permitted and emitted as raw JSON.
    MsfCatalog with_custom;
    MsfTrack custom_track = sapped_video;
    custom_track.custom_fields["m2tsPacketSize"] = "188";
    custom_track.custom_fields["m2tsTimestampMode"] = "\"opaque\"";
    with_custom.tracks.push_back(custom_track);
    const std::string custom_json = serialize_catalog(with_custom);
    ok &= expect_contains(custom_json, "\"m2tsPacketSize\":188", "expected numeric custom field");
    ok &= expect_contains(custom_json, "\"m2tsTimestampMode\":\"opaque\"", "expected quoted custom field");

    // Section 5: custom field names MUST NOT collide with spec field names.
    MsfCatalog colliding;
    MsfTrack collider = sapped_video;
    collider.custom_fields["bitrate"] = "99";
    colliding.tracks.push_back(collider);
    ok &= throws_runtime_error(colliding, "expected throw for a custom field colliding with a spec name");

    // JSON escaping: control characters and quotes must not corrupt the document.
    MsfCatalog escaped;
    MsfTrack quoted = sapped_video;
    quoted.label = "cam \"A\"\n\x01";
    escaped.tracks.push_back(quoted);
    const std::string escaped_json = serialize_catalog(escaped);
    ok &= expect_contains(escaped_json, "\\\"A\\\"", "expected escaped quotes in label");
    ok &= expect_contains(escaped_json, "\\u0001", "expected escaped control character in label");

    // Bitrate precedence: btrt, then configured override, then computed,
    // then a codec-class default (design decision, Phase 1).
    TrackDescription btrt_track;
    btrt_track.handler_type = "vide";
    btrt_track.max_bitrate = 5000000;
    ok &= expect(resolve_bitrate(btrt_track, 900000, 700000) == 5000000,
                 "expected btrt to win over override and computed");

    TrackDescription no_btrt;
    no_btrt.handler_type = "vide";
    ok &= expect(resolve_bitrate(no_btrt, 900000, 700000) == 900000,
                 "expected configured override to win over computed");
    ok &= expect(resolve_bitrate(no_btrt, std::nullopt, 700000) == 700000,
                 "expected computed bitrate when no btrt or override");
    ok &= expect(resolve_bitrate(no_btrt, std::nullopt, 0) == 2000000,
                 "expected 2 Mbps video default as last resort");

    TrackDescription bare_sound;
    bare_sound.handler_type = "soun";
    ok &= expect(resolve_bitrate(bare_sound, std::nullopt, 0) == 128000,
                 "expected 128 kbps audio default as last resort");
    ok &= expect(bitrate_is_estimated(bare_sound, std::nullopt, 0),
                 "expected the codec-class default to report as estimated");
    ok &= expect(!bitrate_is_estimated(btrt_track, std::nullopt, 0),
                 "expected a btrt-derived bitrate not to report as estimated");

    // make_msf_track maps a parsed MP4 track onto the MSF track object.
    TrackDescription source_video;
    source_video.track_name = "video";
    source_video.handler_type = "vide";
    source_video.packaging = "cmaf";
    source_video.codec = "avc1.640028";
    source_video.width = 1920;
    source_video.height = 1080;
    source_video.frame_rate = 30.0;
    source_video.timescale = 90000;
    source_video.max_bitrate = 5000000;
    source_video.language = "eng";
    source_video.duration_ms = 60000;

    const MsfTrack built_live = make_msf_track(source_video, /*is_live=*/true);
    ok &= expect(built_live.role.value_or("") == "video", "expected vide handler to map to the video role");
    ok &= expect(built_live.bitrate.value_or(0) == 5000000, "expected btrt bitrate on the built track");
    ok &= expect(built_live.lang.value_or("") == "eng", "expected language mapped to lang");
    ok &= expect(built_live.render_group.value_or(0) == 1, "expected media tracks in render group 1");
    ok &= expect(!built_live.track_duration_ms.has_value(),
                 "expected no trackDuration on a live track");

    const MsfTrack built_vod = make_msf_track(source_video, /*is_live=*/false);
    ok &= expect(built_vod.track_duration_ms.value_or(0) == 60000,
                 "expected trackDuration on a non-live track");

    TrackDescription source_audio;
    source_audio.track_name = "audio";
    source_audio.handler_type = "soun";
    source_audio.packaging = "cmaf";
    source_audio.codec = "mp4a.40.2";
    source_audio.sample_rate = 48000;
    source_audio.channel_count = 2;

    const MsfTrack built_audio = make_msf_track(source_audio, /*is_live=*/true);
    ok &= expect(built_audio.role.value_or("") == "audio", "expected soun handler to map to the audio role");
    ok &= expect(built_audio.channel_config.value_or("") == "2",
                 "expected channelConfig rendered as a string");
    ok &= expect(built_audio.samplerate.value_or(0) == 48000, "expected samplerate mapped");
    ok &= expect(built_audio.bitrate.value_or(0) == 128000,
                 "expected the audio codec-class default when no rate is known");

    // Integration: a video track built by make_msf_track must satisfy
    // validate_track's invariants (bitrate is set for a/v roles).
    MsfCatalog built_video_catalog;
    built_video_catalog.tracks.push_back(built_live);
    const std::string built_video_json = serialize_catalog(built_video_catalog);
    ok &= expect_contains(built_video_json, "\"bitrate\":5000000",
                          "expected the builder's video track to serialize with its resolved bitrate");

    // Integration: a live track built from a source with a non-zero duration
    // must not carry trackDuration (5.2.35 forbids it on live tracks), and
    // the catalog must still serialize without throwing.
    MsfCatalog built_live_catalog;
    built_live_catalog.tracks.push_back(built_live);
    const std::string built_live_json = serialize_catalog(built_live_catalog);
    ok &= expect_not_contains(built_live_json, "\"trackDuration\"",
                              "expected no trackDuration in the serialized live track");

    // A meta-handler track (neither vide nor soun) gets no bitrate and no
    // renderGroup, and must still serialize -- timeline tracks take this
    // path in Task 5.
    TrackDescription source_meta;
    source_meta.track_name = "timeline";
    source_meta.handler_type = "meta";
    source_meta.packaging = "mediatimeline";

    const MsfTrack built_meta = make_msf_track(source_meta, /*is_live=*/true);
    ok &= expect(!built_meta.bitrate.has_value(), "expected no bitrate for a meta-handler track");
    ok &= expect(!built_meta.render_group.has_value(), "expected no renderGroup for a meta-handler track");
    ok &= expect(built_meta.role.value_or("") == "mediatimeline",
                 "expected mediatimeline packaging to map to the mediatimeline role");

    MsfCatalog built_meta_catalog;
    built_meta_catalog.tracks.push_back(built_meta);
    const std::string built_meta_json = serialize_catalog(built_meta_catalog);
    ok &= expect_not_contains(built_meta_json, "\"bitrate\"",
                              "expected no bitrate in the serialized meta-handler track");
    ok &= expect_not_contains(built_meta_json, "\"renderGroup\"",
                              "expected no renderGroup in the serialized meta-handler track");

    // Fix round 1: width/height are optional (5.2.26/5.2.27) and must be
    // omitted when unknown (0), not published as a literal 0x0. This matters
    // for codecs such as av01/vp09 that extract_tracks does not yet decode
    // dimensions for.
    TrackDescription source_video_no_dims;
    source_video_no_dims.track_name = "video_av1";
    source_video_no_dims.handler_type = "vide";
    source_video_no_dims.packaging = "cmaf";
    source_video_no_dims.codec = "av01.0.08M.10.0.110.09";
    source_video_no_dims.max_bitrate = 3000000;
    // width/height default to 0 (unknown).

    const MsfTrack built_no_dims = make_msf_track(source_video_no_dims, /*is_live=*/true);
    ok &= expect(!built_no_dims.width.has_value(), "expected no width when the source dimension is unknown");
    ok &= expect(!built_no_dims.height.has_value(), "expected no height when the source dimension is unknown");

    MsfCatalog no_dims_catalog;
    no_dims_catalog.tracks.push_back(built_no_dims);
    const std::string no_dims_json = serialize_catalog(no_dims_catalog);
    ok &= expect_not_contains(no_dims_json, "\"width\"", "expected no width key when the source dimension is unknown");
    ok &= expect_not_contains(no_dims_json, "\"height\"", "expected no height key when the source dimension is unknown");

    // A video track with known dimensions still emits both, with the actual values.
    ok &= expect(built_live.width.value_or(0) == 1920, "expected width mapped for a track with known dimensions");
    ok &= expect(built_live.height.value_or(0) == 1080, "expected height mapped for a track with known dimensions");
    ok &= expect_contains(built_video_json, "\"width\":1920", "expected width value in the serialized track");
    ok &= expect_contains(built_video_json, "\"height\":1080", "expected height value in the serialized track");

    // samplerate/channelConfig stay unconditional even when the sample rate is
    // unknown: validate_track makes both MUST-present for audio, so gating
    // them (unlike width/height) would turn a genuinely-unknown value into a
    // serialization throw instead of a graceful (if uninformative) 0.
    TrackDescription source_audio_no_rate;
    source_audio_no_rate.track_name = "audio_no_rate";
    source_audio_no_rate.handler_type = "soun";
    source_audio_no_rate.packaging = "cmaf";
    source_audio_no_rate.codec = "mp4a.40.2";
    source_audio_no_rate.channel_count = 2;
    // sample_rate defaults to 0 (unknown).

    const MsfTrack built_audio_no_rate = make_msf_track(source_audio_no_rate, /*is_live=*/true);
    MsfCatalog no_rate_catalog;
    no_rate_catalog.tracks.push_back(built_audio_no_rate);
    const std::string no_rate_json = serialize_catalog(no_rate_catalog);
    ok &= expect_contains(no_rate_json, "\"samplerate\":0",
                          "expected samplerate:0 to serialize rather than throw when the rate is unknown");
    ok &= expect_contains(no_rate_json, "\"channelConfig\":\"2\"",
                          "expected channelConfig to still serialize alongside an unknown samplerate");

    // MSF 11.3: ending a live broadcast.
    MsfCatalog live_now;
    live_now.generated_at_ms = 1751000000000ULL;
    MsfTrack live_v;
    live_v.name = "video";
    live_v.packaging = "cmaf";
    live_v.role = "video";
    live_v.is_live = true;
    live_v.codec = "avc1.640028";
    live_v.bitrate = 5000000;
    live_now.tracks.push_back(live_v);
    MsfTrack live_a;
    live_a.name = "audio";
    live_a.packaging = "cmaf";
    live_a.role = "audio";
    live_a.is_live = true;
    live_a.codec = "mp4a.40.2";
    live_a.bitrate = 128000;
    live_a.samplerate = 48000;
    live_a.channel_config = "2";
    live_now.tracks.push_back(live_a);

    // Converting to VOD: isLive flips to false AND trackDuration is added in
    // the same rebuild. Doing it in two steps would throw, because
    // validate_track rejects is_live together with track_duration_ms.
    const std::map<std::string, std::uint64_t> durations{{"video", 60000}, {"audio", 60000}};
    const MsfCatalog vod_cat =
        make_end_broadcast_catalog(live_now, EndBroadcastMode::kConvertToVod, durations);
    const std::string vod_json = serialize_catalog(vod_cat);
    ok &= expect_contains(vod_json, "\"isLive\":false", "expected VOD conversion to clear isLive");
    ok &= expect_contains(vod_json, "\"trackDuration\":60000", "expected trackDuration on VOD conversion");
    ok &= expect_not_contains(vod_json, "\"isLive\":true", "expected no track left live after conversion");
    ok &= expect_not_contains(vod_json, "\"generatedAt\":",
                              "expected generatedAt dropped on VOD conversion (MSF 5.1.2)");
    ok &= expect_not_contains(vod_json, "\"isComplete\"",
                              "expected no isComplete on a VOD conversion");

    // Terminating permanently: isComplete true and an EMPTY tracks array.
    const MsfCatalog term_cat =
        make_end_broadcast_catalog(live_now, EndBroadcastMode::kTerminate, {});
    const std::string term_json = serialize_catalog(term_cat);
    ok &= expect_contains(term_json, "\"isComplete\":true", "expected isComplete on termination");
    ok &= expect_contains(term_json, "\"tracks\":[]", "expected an empty tracks array on termination");
    ok &= expect_not_contains(term_json, "\"video\"", "expected no track entries after termination");

    // A track with no duration supplied still converts, just without the field.
    const MsfCatalog partial_cat = make_end_broadcast_catalog(
        live_now, EndBroadcastMode::kConvertToVod, {{"video", 60000}});
    const std::string partial_json = serialize_catalog(partial_cat);
    // Verify each track's properties specifically, not just that they exist somewhere.
    // Find the audio track object by locating its name field and extracting the
    // surrounding braces. The audio track is after the video track in the array.
    const size_t audio_name_pos = partial_json.find("\"name\":\"audio\"");
    ok &= expect(audio_name_pos != std::string::npos, "expected audio track in partial catalog");
    if (audio_name_pos != std::string::npos) {
        // Find the start of this track object by searching backward for an opening brace.
        size_t track_start = partial_json.rfind('{', audio_name_pos);
        // Find the end by searching forward for a closing brace.
        size_t track_end = partial_json.find('}', audio_name_pos);
        if (track_start != std::string::npos && track_end != std::string::npos) {
            std::string audio_track = partial_json.substr(track_start, track_end - track_start + 1);
            // Audio track must have isLive:false
            ok &= expect_contains(audio_track, "\"isLive\":false",
                                  "expected audio track marked isLive:false");
            // Audio track must NOT have trackDuration (it was not in the map)
            ok &= expect_not_contains(audio_track, "\"trackDuration\"",
                                      "expected audio track to have no trackDuration (not in map)");
        }
    }
    // Video track must have trackDuration:60000 (it was in the map)
    ok &= expect_contains(partial_json, "\"name\":\"video\"", "expected video track present");
    const size_t video_name_pos = partial_json.find("\"name\":\"video\"");
    if (video_name_pos != std::string::npos) {
        // Extract the video track similarly
        size_t track_start = partial_json.rfind('{', video_name_pos);
        size_t track_end = partial_json.find('}', video_name_pos);
        if (track_start != std::string::npos && track_end != std::string::npos) {
            std::string video_track = partial_json.substr(track_start, track_end - track_start + 1);
            ok &= expect_contains(video_track, "\"trackDuration\":60000",
                                  "expected video track to have trackDuration:60000");
            ok &= expect_contains(video_track, "\"isLive\":false",
                                  "expected video track marked isLive:false");
        }
    }
    // Globally: no "isLive":true should remain anywhere
    ok &= expect_not_contains(partial_json, "\"isLive\":true",
                              "expected no live tracks in partial VOD conversion");

    // MSF section 5: object 0 of every group holds an independent catalog,
    // and all catalog objects map to sub-group 0.
    CatalogPublisher pub;
    const auto first = pub.publish(live_now);
    ok &= expect(first.size() == 1, "expected one object for the first catalog");
    ok &= expect(first[0].group_id == 0, "expected the first catalog in group 0");
    ok &= expect(first[0].object_id == 0, "expected the first catalog at object 0");
    ok &= expect(first[0].subgroup_id == 0, "expected all catalog objects in sub-group 0");
    ok &= expect_contains(first[0].payload, "\"version\":\"1\"", "expected a serialized catalog");

    // Republishing an unchanged catalog emits nothing.
    const auto unchanged = pub.publish(live_now);
    ok &= expect(unchanged.empty(), "expected no objects when the catalog is unchanged");

    // Ending the broadcast emits one independent catalog in a NEW group.
    const auto ended = pub.end_broadcast(EndBroadcastMode::kTerminate, {});
    ok &= expect(ended.size() == 1, "expected one object for the end-of-broadcast catalog");
    ok &= expect(ended[0].group_id == 1, "expected the final catalog in a new group");
    ok &= expect(ended[0].object_id == 0, "expected the final catalog at object 0");
    ok &= expect_contains(ended[0].payload, "\"isComplete\":true", "expected isComplete on termination");
    ok &= expect(pub.ended(), "expected the publisher to report the broadcast ended");

    // Section 5.1.3 and 5.2.7 are one-way transitions: nothing may follow.
    bool threw_after_end = false;
    try {
        (void)pub.publish(live_now);
    } catch (const std::runtime_error&) {
        threw_after_end = true;
    }
    ok &= expect(threw_after_end, "expected publishing after end_broadcast to throw");

    // end_broadcast is itself a one-way transition: a second call must also throw.
    bool threw_second_end = false;
    try {
        (void)pub.end_broadcast(EndBroadcastMode::kTerminate, {});
    } catch (const std::runtime_error&) {
        threw_second_end = true;
    }
    ok &= expect(threw_second_end, "expected a second end_broadcast call to throw");

    // Fix round 2: prove tracks_equal is sensitive to fields that a hand-rolled
    // comparison could easily omit. Each case publishes a catalog, then
    // publishes a copy differing in exactly one field, and expects a new
    // object -- a silent no-op here would mean a real change never reaches
    // subscribers, which is the worst failure mode this class has.
    {
        // buffers.target_ms changed, with the optional engaged on both sides:
        // this specifically catches a comparison that only checks has_value().
        CatalogPublisher buffers_pub;
        MsfCatalog buffers_a;
        MsfTrack buffers_track_a = sapped_video;
        buffers_track_a.buffers = MsfBuffers{.target_ms = 1500, .min_ms = 800, .max_ms = std::nullopt};
        buffers_a.tracks.push_back(buffers_track_a);
        const auto buffers_first = buffers_pub.publish(buffers_a);
        ok &= expect(buffers_first.size() == 1, "expected the first buffers catalog to publish");

        MsfCatalog buffers_b = buffers_a;
        buffers_b.tracks[0].buffers->target_ms = 2000;
        const auto buffers_second = buffers_pub.publish(buffers_b);
        ok &= expect(!buffers_second.empty(),
                     "expected a change to buffers.target_ms to trigger a republish");
    }

    {
        // A custom_fields entry's value changed.
        CatalogPublisher custom_pub;
        MsfCatalog custom_a;
        MsfTrack custom_track_a = sapped_video;
        custom_track_a.custom_fields["m2tsPacketSize"] = "188";
        custom_a.tracks.push_back(custom_track_a);
        const auto custom_first = custom_pub.publish(custom_a);
        ok &= expect(custom_first.size() == 1, "expected the first custom_fields catalog to publish");

        MsfCatalog custom_b = custom_a;
        custom_b.tracks[0].custom_fields["m2tsPacketSize"] = "204";
        const auto custom_second = custom_pub.publish(custom_b);
        ok &= expect(!custom_second.empty(),
                     "expected a change to a custom_fields value to trigger a republish");
    }

    {
        // A depends entry changed.
        CatalogPublisher depends_pub;
        MsfCatalog depends_a;
        depends_a.tracks.push_back(sap);  // sap.depends == {"video"}
        const auto depends_first = depends_pub.publish(depends_a);
        ok &= expect(depends_first.size() == 1, "expected the first depends catalog to publish");

        MsfCatalog depends_b = depends_a;
        depends_b.tracks[0].depends = {"audio"};
        const auto depends_second = depends_pub.publish(depends_b);
        ok &= expect(!depends_second.empty(),
                     "expected a change to a depends entry to trigger a republish");
    }

    {
        // max_grp_sap_starting_type changed.
        CatalogPublisher sap_pub;
        MsfCatalog sap_a;
        sap_a.tracks.push_back(sapped_video);  // max_grp_sap_starting_type == 2
        const auto sap_first = sap_pub.publish(sap_a);
        ok &= expect(sap_first.size() == 1, "expected the first SAP-type catalog to publish");

        MsfCatalog sap_b = sap_a;
        sap_b.tracks[0].max_grp_sap_starting_type = 3;
        const auto sap_second = sap_pub.publish(sap_b);
        ok &= expect(!sap_second.empty(),
                     "expected a change to max_grp_sap_starting_type to trigger a republish");
    }

    // Adding a track produces a delta at object ID 1 of the CURRENT group.
    CatalogPublisher dpub;
    (void)dpub.publish(live_now);
    MsfCatalog plus_one = live_now;
    MsfTrack extra;
    extra.name = "video-low";
    extra.packaging = "cmaf";
    extra.role = "video";
    extra.is_live = true;
    extra.codec = "avc1.64000d";
    extra.bitrate = 500000;
    plus_one.tracks.push_back(extra);

    const auto added = dpub.publish(plus_one);
    ok &= expect(added.size() == 1, "expected one object for an add delta");
    ok &= expect(added[0].group_id == 0, "expected the delta to stay in the current group");
    ok &= expect(added[0].object_id == 1, "expected the delta at object ID 1");
    ok &= expect_contains(added[0].payload, "\"deltaUpdate\"", "expected a deltaUpdate field");
    ok &= expect_contains(added[0].payload, "\"op\":\"add\"", "expected an add operation");
    ok &= expect_contains(added[0].payload, "\"video-low\"", "expected the added track name");
    // Section 5.3 forbids a ROOT-level tracks field (5.1.4) and a version
    // field (5.1.1). It does NOT forbid the per-operation "tracks" array,
    // which section 5.1.6 REQUIRES inside every delta operation. So assert
    // the root has no tracks, not that the string never appears.
    ok &= expect(added[0].payload.rfind("{\"deltaUpdate\":[", 0) == 0,
                 "expected a delta payload to open directly with deltaUpdate");
    const std::size_t delta_pos = added[0].payload.find("\"deltaUpdate\":");
    const std::size_t first_tracks_pos = added[0].payload.find("\"tracks\":");
    ok &= expect(delta_pos != std::string::npos && first_tracks_pos != std::string::npos &&
                     first_tracks_pos > delta_pos,
                 "expected every tracks field to sit inside deltaUpdate, never at the root");
    ok &= expect_not_contains(added[0].payload, "\"version\":", "expected no version field in a delta");

    // Removing a track produces a remove carrying only name (and namespace).
    const auto removed = dpub.publish(live_now);
    ok &= expect(removed.size() == 1, "expected one object for a remove delta");
    ok &= expect(removed[0].object_id == 2, "expected the second delta at object ID 2");
    ok &= expect_contains(removed[0].payload, "\"op\":\"remove\"", "expected a remove operation");
    ok &= expect_contains(removed[0].payload, "\"video-low\"", "expected the removed track name");
    ok &= expect_not_contains(removed[0].payload, "\"bitrate\"",
                              "expected a remove entry to carry no attributes (MSF 5.1.6)");

    // Section 5.1.6: a remove entry's track object MUST include name, MAY
    // include namespace, and MUST NOT hold any other field. The differ
    // builds remove ops from previously-published tracks, which are
    // complete, so it is the serializer's narrowing (not the input) that
    // enforces this. Slice the remove op's track object out of the payload
    // and check its exact contents.
    {
        const std::size_t name_pos = removed[0].payload.find("\"name\":\"video-low\"");
        ok &= expect(name_pos != std::string::npos, "expected the removed track's name in the payload");
        const std::size_t track_start = removed[0].payload.rfind('{', name_pos);
        const std::size_t track_end = removed[0].payload.find('}', name_pos);
        ok &= expect(track_start != std::string::npos && track_end != std::string::npos,
                     "expected to locate the remove entry's track object braces");
        if (track_start != std::string::npos && track_end != std::string::npos) {
            const std::string remove_track =
                removed[0].payload.substr(track_start, track_end - track_start + 1);
            ok &= expect_contains(remove_track, "\"name\":\"video-low\"",
                                  "expected the remove entry to carry its track name");
            ok &= expect_not_contains(remove_track, "\"bitrate\"",
                                      "expected the remove entry to carry no bitrate");
            ok &= expect_not_contains(remove_track, "\"codec\"",
                                      "expected the remove entry to carry no codec");
            ok &= expect_not_contains(remove_track, "\"packaging\"",
                                      "expected the remove entry to carry no packaging");
        }
    }

    // Section 5.3 freezes attributes once a tuple is declared, so an attribute
    // change CANNOT be a delta. It must fall back to a full independent
    // catalog in a new group; emitting nothing would strand subscribers.
    MsfCatalog changed = live_now;
    changed.tracks[0].bitrate = 9000000;
    const auto rebuilt = dpub.publish(changed);
    ok &= expect(rebuilt.size() == 1, "expected one object for an attribute change");
    ok &= expect(rebuilt[0].object_id == 0, "expected an independent catalog, not a delta");
    ok &= expect(rebuilt[0].group_id == 1, "expected the rebuild in a new group");
    ok &= expect_contains(rebuilt[0].payload, "\"tracks\":", "expected a full catalog");
    ok &= expect_contains(rebuilt[0].payload, "\"bitrate\":9000000", "expected the new attribute");

    // Section 5.3: bound the deltas a joining subscriber must process.
    CatalogPublisher bounded;
    bounded.set_max_deltas_per_group(1);
    (void)bounded.publish(live_now);
    MsfCatalog b2 = live_now;
    b2.tracks.push_back(extra);
    const auto b_delta = bounded.publish(b2);
    ok &= expect(b_delta[0].object_id == 1, "expected the first delta at object 1");
    MsfCatalog b3 = b2;
    MsfTrack extra2 = extra;
    extra2.name = "video-mid";
    b3.tracks.push_back(extra2);
    const auto b_forced = bounded.publish(b3);
    ok &= expect(b_forced[0].object_id == 0, "expected the bound to force an independent catalog");
    ok &= expect(b_forced[0].group_id == 1, "expected the forced catalog in a new group");

    // Self-review finding: the delta path diffs desired.tracks through a
    // std::map keyed by namespace+name, which never runs desired through
    // validate_catalog the way the full-independent path does via
    // serialize_catalog. A desired catalog with a duplicate namespace+name
    // tuple must still throw, not be silently coalesced by std::map::emplace
    // into a single entry. The duplicate is paired with a genuinely new
    // track so the diff's added list is non-empty and the "no diff at all"
    // fallback (which happens to validate too, masking this bug) is not
    // what catches it -- only an explicit validate_catalog(desired) call
    // does.
    {
        CatalogPublisher dup_pub;
        (void)dup_pub.publish(live_now);
        MsfCatalog dup_desired = live_now;
        MsfTrack dup_of_video = live_now.tracks[0];
        dup_desired.tracks.push_back(dup_of_video);  // duplicate "video" entry, no namespace
        dup_desired.tracks.push_back(extra);         // plus a genuinely new track ("video-low")
        bool threw_on_duplicate = false;
        try {
            (void)dup_pub.publish(dup_desired);
        } catch (const std::runtime_error&) {
            threw_on_duplicate = true;
        }
        ok &= expect(threw_on_duplicate,
                     "expected a duplicate namespace+name tuple to throw even on the delta path");
    }

    // Self-review finding: a same-size publishTracks change happening in the
    // SAME publish() call as a track add/remove must still force a full
    // catalog. A size-only or track-only check would silently drop the
    // publishTracks change while still advancing `last_`, so it would never
    // reach the wire.
    {
        CatalogPublisher pt_pub;
        MsfCatalog pt_a = live_now;
        MsfTrack pub_v1;
        pub_v1.name = "upload";
        pub_v1.packaging = "cmaf";
        pub_v1.is_live = true;
        pt_a.publish_tracks.push_back(pub_v1);
        (void)pt_pub.publish(pt_a);

        MsfCatalog pt_b = pt_a;
        pt_b.tracks.push_back(extra);          // a track add, which alone would be a valid delta
        pt_b.publish_tracks[0].label = "new";  // same-size publishTracks, different content
        const auto pt_result = pt_pub.publish(pt_b);
        ok &= expect(pt_result.size() == 1, "expected one object when tracks and publishTracks both change");
        ok &= expect(pt_result[0].object_id == 0,
                     "expected a publishTracks content change to force a full catalog, not a delta");
        ok &= expect_contains(pt_result[0].payload, "\"label\":\"new\"",
                              "expected the publishTracks change to actually reach the wire");
    }

    // Self-review finding: same as above, but for a same-size initDataList
    // whose content differs.
    {
        CatalogPublisher id_pub;
        MsfCatalog id_a = live_now;
        id_a.tracks[0].init_ref = "init-video";
        id_a.init_data_list.push_back(MsfInitData{.id = "init-video", .type = "inline", .data = "AAAA"});
        (void)id_pub.publish(id_a);

        MsfCatalog id_b = id_a;
        id_b.tracks.push_back(extra);           // a track add, which alone would be a valid delta
        id_b.init_data_list[0].data = "BBBB";   // same-size initDataList, different content
        const auto id_result = id_pub.publish(id_b);
        ok &= expect(id_result.size() == 1, "expected one object when tracks and initDataList both change");
        ok &= expect(id_result[0].object_id == 0,
                     "expected an initDataList content change to force a full catalog, not a delta");
        ok &= expect_contains(id_result[0].payload, "\"data\":\"BBBB\"",
                              "expected the initDataList change to actually reach the wire");
    }

    return ok ? 0 : 1;
}
