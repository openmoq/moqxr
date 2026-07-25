// Unit coverage for translating a PublishPlan into libmoq track/object configs.
// Pure translation only -- no endpoint, no sender, no network.

#include "openmoq/publisher/transport/libmoq_publisher.h"

#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/mp4_box.h"
#include "openmoq/publisher/moq_draft.h"
#include "openmoq/publisher/transport/publisher_transport.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace openmoq::publisher;
using namespace openmoq::publisher::transport;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

TrackDescription make_video_track() {
    TrackDescription td;
    td.track_id = 1;
    td.handler_type = "vide";
    td.codec = "avc1.64001f";
    td.track_name = "video";
    td.packaging = "cmaf";
    td.timescale = 90000;
    td.width = 1920;
    td.height = 1080;
    td.frame_rate = 30.0;
    return td;
}

TrackDescription make_audio_track() {
    TrackDescription td;
    td.track_id = 2;
    td.handler_type = "soun";
    td.codec = "mp4a.40.2";
    td.track_name = "audio";
    td.packaging = "cmaf";
    td.timescale = 48000;
    td.sample_rate = 48000;
    td.channel_count = 2;
    return td;
}

TrackDescription make_catalog_track() {
    TrackDescription td;
    td.track_id = 0;
    td.handler_type = "meta";
    td.codec = "catalog";
    td.sample_entry_type = "catalog";
    td.track_name = "catalog";
    td.packaging = "catalog";
    return td;
}

TrackDescription make_mediatimeline_track() {
    TrackDescription td;
    td.track_id = 10;
    td.handler_type = "meta";
    td.sample_entry_type = "mediatimeline";
    td.track_name = "video.timeline";
    td.packaging = "mediatimeline";
    td.mime_type = "application/json";
    return td;
}

TrackDescription make_eventtimeline_track() {
    TrackDescription td;
    td.track_id = 11;
    td.handler_type = "meta";
    td.sample_entry_type = "eventtimeline";
    td.track_name = "video.sap";
    td.packaging = "eventtimeline";
    td.event_type = "org.ietf.moq.cmsf.sap";
    td.mime_type = "application/json";
    return td;
}

CmsfObject media_object(const std::string& track, std::size_t group, std::size_t object,
                        std::uint64_t time_us, std::vector<std::uint8_t> bytes) {
    CmsfObject o;
    o.kind = CmsfObjectKind::kMedia;
    o.track_name = track;
    o.group_id = group;
    o.object_id = object;
    o.media_time_us = time_us;
    o.media_duration_us = 33333;
    o.owned_payload = std::move(bytes);
    return o;
}

PublishPlan make_plan() {
    PublishPlan plan;
    plan.draft = draft_profile(DraftVersion::kDraft16);
    // Mirrors build_publish_plan: a synthetic "catalog" track at the front and
    // generated timeline tracks alongside the real media tracks. Only the
    // "vide"/"soun" tracks should survive translation.
    plan.tracks = {
        make_catalog_track(),
        make_video_track(),
        make_audio_track(),
        make_mediatimeline_track(),
        make_eventtimeline_track(),
    };
    plan.track_initializations = {
        TrackInitialization{.track_name = "video", .codec_payload = {}, .init_segment = {1, 2, 3, 4}},
        TrackInitialization{.track_name = "audio", .codec_payload = {}, .init_segment = {9, 8, 7}},
    };

    // A catalog object (kInitialization) and a timeline object (kMetadata) that
    // MUST be dropped by the translation.
    CmsfObject catalog;
    catalog.kind = CmsfObjectKind::kInitialization;
    catalog.track_name = "catalog";
    catalog.owned_payload = {'{', '}'};
    CmsfObject timeline;
    timeline.kind = CmsfObjectKind::kMetadata;
    timeline.track_name = "video";
    timeline.owned_payload = {0x00};

    plan.objects = {
        catalog,
        media_object("video", 0, 0, 0, {0xaa, 0xbb}),
        media_object("video", 0, 1, 33333, {0xcc}),
        media_object("video", 1, 0, 1000000, {0xdd, 0xee, 0xff}),
        timeline,
        media_object("audio", 0, 0, 0, {0x11, 0x22}),
    };
    return plan;
}

}  // namespace

int main() {
    bool ok = true;

    const PublishPlan plan = make_plan();
    const LibmoqPlanTranslation tr = translate_plan_for_libmoq(plan);

    // -- Tracks --------------------------------------------------------------
    // Only the real "vide"/"soun" media tracks are configured; the synthetic
    // catalog and timeline tracks (handler "meta") are dropped -- libmoq owns
    // catalog publication.
    ok &= expect(tr.tracks.size() == 2,
                 "expected only the two media tracks (catalog + timelines skipped)");
    for (const auto& t : tr.tracks) {
        ok &= expect(t.name != "catalog" && t.name != "video.timeline" && t.name != "video.sap",
                     "expected no synthetic catalog/timeline track to be configured as media");
    }
    if (tr.tracks.size() == 2) {
        const LibmoqTrackTranslation& v = tr.tracks[0];
        ok &= expect(v.name == "video", "expected first track named 'video'");
        ok &= expect(v.media_type == MOQ_MEDIA_TYPE_VIDEO, "expected video media type");
        ok &= expect(v.packaging == MOQ_MEDIA_PACKAGING_CMAF, "expected CMAF packaging for video");
        ok &= expect(v.codec == "avc1.64001f", "expected video codec carried through");
        ok &= expect(v.init_data == std::vector<std::uint8_t>({1, 2, 3, 4}),
                     "expected video init segment used as init_data");
        ok &= expect(v.width == 1920 && v.height == 1080, "expected video geometry carried");
        ok &= expect(v.framerate_millis == 30000, "expected framerate millis = fps*1000");
        ok &= expect(!v.is_live, "expected batch tracks to be VOD (is_live=false)");
        ok &= expect(v.bitrate > 0, "expected a non-zero derived bitrate (MSF-01 5.2.22)");

        const LibmoqTrackTranslation& a = tr.tracks[1];
        ok &= expect(a.name == "audio", "expected second track named 'audio'");
        ok &= expect(a.media_type == MOQ_MEDIA_TYPE_AUDIO, "expected audio media type");
        ok &= expect(a.samplerate == 48000, "expected audio samplerate carried");
        ok &= expect(a.channel_config == "2", "expected audio channel config from channel count");
        ok &= expect(a.init_data == std::vector<std::uint8_t>({9, 8, 7}),
                     "expected audio init segment used as init_data");
        ok &= expect(a.bitrate > 0, "expected a non-zero derived audio bitrate");

        // Borrowing cfg() points into the owning translation object.
        const moq_media_track_cfg_t c = v.cfg();
        ok &= expect(c.name.len == v.name.size() &&
                         c.name.data == reinterpret_cast<const std::uint8_t*>(v.name.data()),
                     "expected cfg().name to borrow the track name");
        ok &= expect(c.codec.len == v.codec.size(), "expected cfg().codec length to match");
        ok &= expect(c.init_data.len == v.init_data.size() && c.init_data.data == v.init_data.data(),
                     "expected cfg().init_data to borrow the init segment");
        ok &= expect(c.media_type == MOQ_MEDIA_TYPE_VIDEO && c.packaging == MOQ_MEDIA_PACKAGING_CMAF,
                     "expected cfg() to carry media type and packaging");
        ok &= expect(c.bitrate == v.bitrate, "expected cfg() bitrate to match");
    }

    // -- Objects (catalog + timeline dropped) --------------------------------
    ok &= expect(tr.objects.size() == 4, "expected four media objects (catalog + timeline skipped)");
    if (tr.objects.size() == 4) {
        const LibmoqObjectTranslation& v0 = tr.objects[0];  // video g0 o0
        ok &= expect(v0.track_name == "video" && v0.group_id == 0 && v0.object_id == 0,
                     "expected first object to be video group 0 object 0");
        ok &= expect(v0.starts_group, "expected object_id==0 to start a group");
        ok &= expect(v0.is_sync, "expected a group start to be a sync point");
        ok &= expect(!v0.ends_group, "expected group 0 not to end at object 0 (object 1 follows)");
        ok &= expect(v0.decode_time_us == 0 && v0.presentation_time_us == 0,
                     "expected timing from media_time_us");

        const LibmoqObjectTranslation& v1 = tr.objects[1];  // video g0 o1
        ok &= expect(!v1.starts_group, "expected object_id!=0 not to start a group");
        ok &= expect(v1.ends_group, "expected the last object in group 0 to end it");
        ok &= expect(!v1.is_sync, "expected a non-group-start not to be a sync point");
        ok &= expect(v1.presentation_time_us == 33333, "expected presentation time from media_time_us");

        const LibmoqObjectTranslation& v2 = tr.objects[2];  // video g1 o0
        ok &= expect(v2.group_id == 1 && v2.starts_group && v2.ends_group,
                     "expected a single-object group to both start and end");

        const LibmoqObjectTranslation& a0 = tr.objects[3];  // audio g0 o0
        ok &= expect(a0.track_name == "audio" && a0.starts_group && a0.ends_group,
                     "expected the lone audio object to start and end its group");

        // object() builds the send-object with typed fields and a NULL payload.
        const moq_media_send_object_t so = v0.object();
        ok &= expect(so.struct_size == sizeof(so), "expected object() to stamp struct_size");
        ok &= expect(so.payload == nullptr, "expected object() to leave payload for the caller");
        ok &= expect(so.properties == nullptr, "expected CMAF object to carry no extra property block");
        ok &= expect(so.is_sync && so.starts_group && !so.ends_group,
                     "expected object() to carry the grouping flags");
    }

    // -- Live track translation ----------------------------------------------
    {
        const std::vector<std::uint8_t> vinit = {1, 2, 3, 4};
        const LibmoqTrackTranslation v = make_libmoq_live_track(make_video_track(), vinit);
        ok &= expect(v.name == "video", "expected live video track name");
        ok &= expect(v.media_type == MOQ_MEDIA_TYPE_VIDEO, "expected live video media type");
        ok &= expect(v.packaging == MOQ_MEDIA_PACKAGING_CMAF, "expected live CMAF packaging");
        ok &= expect(v.codec == "avc1.64001f", "expected live video codec carried");
        ok &= expect(v.init_data == vinit, "expected live track init segment used as init_data");
        ok &= expect(v.is_live, "expected live tracks to be isLive=true");
        ok &= expect(v.bitrate > 0, "expected a non-zero fallback bitrate for live video");

        const std::vector<std::uint8_t> ainit = {9, 8, 7};
        const LibmoqTrackTranslation a = make_libmoq_live_track(make_audio_track(), ainit);
        ok &= expect(a.media_type == MOQ_MEDIA_TYPE_AUDIO, "expected live audio media type");
        ok &= expect(a.samplerate == 48000, "expected live audio samplerate carried");
        ok &= expect(a.channel_config == "2", "expected live audio channel config");
        ok &= expect(a.is_live, "expected live audio track to be isLive=true");
        ok &= expect(a.bitrate > 0, "expected a non-zero fallback bitrate for live audio");
    }

    // -- Live object (MediaFragment) translation ------------------------------
    {
        // A video keyframe at group start.
        MediaFragment kf;
        kf.group_id = 5;
        kf.object_id = 0;
        kf.track_name = "video";
        kf.start_time_us = 100000;
        kf.earliest_presentation_time_us = 110000;
        kf.is_video_keyframe = true;
        kf.sap_type = 2;          // moqxr computed SAP type 2 for the keyframe
        kf.has_sap_type = true;
        kf.payload.owned_bytes = {0xde, 0xad, 0xbe, 0xef};
        const LibmoqObjectTranslation o = make_libmoq_live_object(kf);
        ok &= expect(o.track_name == "video" && o.group_id == 5 && o.object_id == 0,
                     "expected live object identity carried from fragment");
        ok &= expect(o.starts_group, "expected object_id==0 to start a group");
        ok &= expect(o.is_sync, "expected a video keyframe to be a sync point");
        ok &= expect(!o.ends_group, "expected live objects never to set ends_group");
        ok &= expect(o.decode_time_us == 100000, "expected decode time from fragment start time");
        ok &= expect(o.presentation_time_us == 110000,
                     "expected presentation time from earliest presentation time");
        ok &= expect(o.payload == std::vector<std::uint8_t>({0xde, 0xad, 0xbe, 0xef}),
                     "expected payload from the fragment's owned bytes");
        ok &= expect(o.has_sap_type && o.sap_type == MOQ_SAP_TYPE_2,
                     "expected live fragment SAP type 2 carried into the translation");
        const moq_media_send_object_t so = o.object();
        ok &= expect(so.payload == nullptr, "expected object() to leave payload for the caller");
        ok &= expect(so.is_sync && so.starts_group && !so.ends_group,
                     "expected object() to carry live grouping flags");
        ok &= expect(so.has_sap_type && so.sap_type == MOQ_SAP_TYPE_2,
                     "expected object() to declare the SAP type to libmoq (§3.4 group start)");

        // A fragment with no computed SAP must NOT declare one (no fabrication).
        MediaFragment undecl = kf;
        undecl.has_sap_type = false;
        const moq_media_send_object_t uso = make_libmoq_live_object(undecl).object();
        ok &= expect(!uso.has_sap_type, "expected no SAP declaration when moqxr did not compute one");

        // A mid-group non-keyframe video fragment is not a sync point.
        MediaFragment p;
        p.group_id = 5;
        p.object_id = 1;
        p.track_name = "video";
        p.start_time_us = 133000;
        p.is_video_keyframe = false;
        p.payload.owned_bytes = {0x01};
        const LibmoqObjectTranslation po = make_libmoq_live_object(p);
        ok &= expect(!po.starts_group, "expected object_id!=0 not to start a group");
        ok &= expect(!po.is_sync, "expected a mid-group non-keyframe not to be a sync point");
        ok &= expect(po.presentation_time_us == 133000,
                     "expected presentation time to fall back to start time when EPT is 0");

        // A *declared* SAP type marks a sync point even mid-group.
        MediaFragment sap;
        sap.group_id = 5;
        sap.object_id = 2;
        sap.track_name = "audio";
        sap.sap_type = 1;
        sap.has_sap_type = true;
        sap.payload.owned_bytes = {0x02};
        const LibmoqObjectTranslation sapo = make_libmoq_live_object(sap);
        ok &= expect(sapo.is_sync, "expected a declared SAP type to be a sync point");
        ok &= expect(sapo.has_sap_type && sapo.sap_type == MOQ_SAP_TYPE_1,
                     "expected the declared SAP type carried through");

        // A non-key video fragment declares NONE (type 0), not a SAP -- and a
        // mid-group NONE is not a sync point.
        MediaFragment nonkey;
        nonkey.group_id = 5;
        nonkey.object_id = 3;
        nonkey.track_name = "video";
        nonkey.is_video_keyframe = false;
        nonkey.sap_type = 0;  // NONE, as the SRT/segmenter paths now declare for P/B
        nonkey.has_sap_type = true;
        nonkey.payload.owned_bytes = {0x03};
        const LibmoqObjectTranslation nko = make_libmoq_live_object(nonkey);
        ok &= expect(nko.has_sap_type && nko.sap_type == MOQ_SAP_NONE,
                     "expected non-key video to declare MOQ_SAP_NONE, not a SAP type");
        ok &= expect(!nko.is_sync, "expected a mid-group NONE not to be a sync point");
        ok &= expect(nko.object().has_sap_type && nko.object().sap_type == MOQ_SAP_NONE,
                     "expected object() to carry the NONE declaration");

        // A stale nonzero sap_type with has_sap_type=false must NOT declare a SAP
        // and must NOT imply a sync point.
        MediaFragment stale;
        stale.group_id = 5;
        stale.object_id = 4;
        stale.track_name = "video";
        stale.is_video_keyframe = false;
        stale.sap_type = 2;  // stale/garbage value...
        stale.has_sap_type = false;  // ...but not actually declared
        stale.payload.owned_bytes = {0x04};
        const LibmoqObjectTranslation sto = make_libmoq_live_object(stale);
        ok &= expect(!sto.has_sap_type, "expected no SAP declaration when has_sap_type is false");
        ok &= expect(!sto.object().has_sap_type, "expected object() to declare no SAP for stale value");
        ok &= expect(!sto.is_sync, "expected stale sap_type not to imply a sync point");
    }

    // -- Batch: CMAF SAP type preserved CmsfObject -> moq_media_send_object ---
    {
        PublishPlan plan;
        plan.draft = draft_profile(DraftVersion::kDraft16);
        plan.tracks = {make_video_track()};
        CmsfObject mo;
        mo.kind = CmsfObjectKind::kMedia;
        mo.track_name = "video";
        mo.group_id = 0;
        mo.object_id = 0;
        mo.sap_type = 2;  // moqxr computed SAP type 2 for the coalesced group start
        mo.has_sap_type = true;
        mo.owned_payload = {0x11, 0x22};
        plan.objects = {mo};

        const LibmoqPlanTranslation tr = translate_plan_for_libmoq(plan);
        ok &= expect(tr.objects.size() == 1, "expected one translated media object");
        if (tr.objects.size() == 1) {
            const LibmoqObjectTranslation& o = tr.objects[0];
            ok &= expect(o.starts_group && o.has_sap_type && o.sap_type == MOQ_SAP_TYPE_2,
                         "expected batch CmsfObject SAP type 2 carried into the translation");
            const moq_media_send_object_t so = o.object();
            ok &= expect(so.has_sap_type && so.sap_type == MOQ_SAP_TYPE_2,
                         "expected object() to declare the batch SAP type to libmoq (§3.4)");
        }
    }

    // -- SRT bootstrap -> live track configs; SRT fragment -> live object -----
    {
        // The SRT ingest manager bootstraps real media TrackDescriptions and a
        // synthetic init segment; both flow through the same live helpers.
        const std::vector<TrackDescription> bootstrap_tracks = {make_video_track(),
                                                                make_audio_track()};
        const std::vector<std::uint8_t> synthetic_init =
            LiveSrtIngestManager::build_synthetic_init_segment(bootstrap_tracks);
        ok &= expect(!synthetic_init.empty(),
                     "expected a non-empty synthetic init segment from bootstrap tracks");

        const LibmoqTrackTranslation v = make_libmoq_live_track(bootstrap_tracks[0], synthetic_init);
        ok &= expect(v.media_type == MOQ_MEDIA_TYPE_VIDEO && v.packaging == MOQ_MEDIA_PACKAGING_CMAF,
                     "expected SRT bootstrap video track to map to a CMAF media track");
        ok &= expect(v.is_live, "expected SRT bootstrap track to be isLive=true");
        ok &= expect(v.init_data == synthetic_init,
                     "expected SRT track init_data to be the synthetic init segment");

        // SRT fragments arrive with group_id/object_id already assigned by the
        // manager -- the SAME live object translation maps them.
        MediaFragment kf;
        kf.group_id = 3;
        kf.object_id = 0;
        kf.track_name = "video";
        kf.start_time_us = 90000;
        kf.earliest_presentation_time_us = 99000;
        kf.is_video_keyframe = true;
        kf.payload.owned_bytes = {0x11, 0x22, 0x33};
        const LibmoqObjectTranslation o = make_libmoq_live_object(kf);
        ok &= expect(o.group_id == 3 && o.object_id == 0,
                     "expected SRT-assigned group/object carried through");
        ok &= expect(o.starts_group && o.is_sync && !o.ends_group,
                     "expected SRT keyframe to start a group, be sync, and not end the group");
        ok &= expect(o.decode_time_us == 90000 && o.presentation_time_us == 99000,
                     "expected SRT fragment timing carried through");
        ok &= expect(o.payload == std::vector<std::uint8_t>({0x11, 0x22, 0x33}),
                     "expected SRT fragment payload from owned bytes");

        MediaFragment mid;
        mid.group_id = 3;
        mid.object_id = 2;
        mid.track_name = "audio";
        mid.payload.owned_bytes = {0x44};
        const LibmoqObjectTranslation mo = make_libmoq_live_object(mid);
        ok &= expect(!mo.starts_group, "expected a non-zero SRT object_id not to start a group");
    }

    // -- LiveObjectSource: metadata gate + track/object translation ----------
    {
        // Bare legacy track (name only) lacks media metadata.
        ok &= expect(!live_track_has_media_metadata(LiveTrack{.track_name = "events"}),
                     "expected a bare LiveTrack to lack libmoq media metadata");

        // Video RAW track.
        LiveTrack vtrack;
        vtrack.track_name = "video";
        vtrack.media_type = LiveMediaType::kVideo;
        vtrack.packaging = LivePackaging::kRaw;
        vtrack.codec = "av01";
        vtrack.bitrate = 1500000;
        vtrack.width = 1280;
        vtrack.height = 720;
        ok &= expect(live_track_has_media_metadata(vtrack),
                     "expected a video track with codec to have sufficient metadata");
        const LibmoqTrackTranslation vt = make_libmoq_live_object_track(vtrack);
        ok &= expect(vt.media_type == MOQ_MEDIA_TYPE_VIDEO, "expected video media type");
        ok &= expect(vt.packaging == MOQ_MEDIA_PACKAGING_RAW, "expected RAW packaging for the video track");
        ok &= expect(vt.codec == "av01", "expected video codec carried");
        ok &= expect(vt.width == 1280 && vt.height == 720, "expected video geometry carried");
        ok &= expect(vt.bitrate == 1500000, "expected explicit video bitrate carried");
        ok &= expect(vt.is_live, "expected LiveObjectSource tracks to be isLive=true");

        // Audio RAW track (bitrate omitted -> fallback).
        LiveTrack atrack;
        atrack.track_name = "audio";
        atrack.media_type = LiveMediaType::kAudio;
        atrack.packaging = LivePackaging::kRaw;
        atrack.codec = "opus";
        atrack.sample_rate = 48000;
        atrack.channel_count = 2;
        ok &= expect(live_track_has_media_metadata(atrack),
                     "expected an audio track with samplerate+channels to have metadata");
        ok &= expect(!live_track_has_media_metadata(LiveTrack{.track_name = "a",
                                                              .media_type = LiveMediaType::kAudio,
                                                              .codec = "opus"}),
                     "expected an audio track missing samplerate/channels to lack metadata");
        const LibmoqTrackTranslation at = make_libmoq_live_object_track(atrack);
        ok &= expect(at.media_type == MOQ_MEDIA_TYPE_AUDIO, "expected audio media type");
        ok &= expect(at.packaging == MOQ_MEDIA_PACKAGING_RAW, "expected RAW packaging for the audio track");
        ok &= expect(at.samplerate == 48000, "expected audio samplerate carried");
        ok &= expect(at.channel_config == "2", "expected audio channel config from channel count");
        ok &= expect(at.bitrate == 128000, "expected audio bitrate fallback when unset");

        // CMAF track carries init_data.
        LiveTrack ctrack;
        ctrack.track_name = "cmaf-video";
        ctrack.media_type = LiveMediaType::kVideo;
        ctrack.packaging = LivePackaging::kCmaf;
        ctrack.codec = "avc1.640028";
        ctrack.init_data = {0x66, 0x74, 0x79, 0x70};  // 'ftyp' (illustrative)
        const LibmoqTrackTranslation ct = make_libmoq_live_object_track(ctrack);
        ok &= expect(ct.packaging == MOQ_MEDIA_PACKAGING_CMAF, "expected CMAF packaging");
        ok &= expect(ct.init_data == std::vector<std::uint8_t>({0x66, 0x74, 0x79, 0x70}),
                     "expected CMAF init_data carried into the track config");

        // LiveObject -> send-object mapping.
        LiveObject start;
        start.track_name = "video";
        start.group_id = 7;
        start.object_id = 0;
        start.media_time_us = 250000;
        start.payload = {0x01, 0x02};
        start.final_in_subgroup = true;
        start.subgroup_contains_group_largest = true;
        const LibmoqObjectTranslation so = make_libmoq_live_source_object(start);
        ok &= expect(so.starts_group && so.is_sync, "expected object_id==0 to start a group and be sync");
        ok &= expect(so.ends_group,
                     "expected ends_group when final_in_subgroup && subgroup_contains_group_largest");
        ok &= expect(so.decode_time_us == 250000 && so.presentation_time_us == 250000,
                     "expected timing from media_time_us");
        ok &= expect(so.payload == std::vector<std::uint8_t>({0x01, 0x02}),
                     "expected payload carried from the LiveObject");

        LiveObject mid;
        mid.track_name = "video";
        mid.group_id = 7;
        mid.object_id = 1;
        mid.final_in_subgroup = true;
        mid.subgroup_contains_group_largest = false;  // not the group's largest subgroup
        const LibmoqObjectTranslation mo = make_libmoq_live_source_object(mid);
        ok &= expect(!mo.starts_group && !mo.is_sync,
                     "expected a non-zero object_id not to start a group / not sync");
        ok &= expect(!mo.ends_group,
                     "expected no ends_group when the subgroup is not the group's largest");
    }

    // -- Driver guard: object source is NOT consumed before the driver runs --
    // Network-free: with cancel already set, publish_live_objects_via_libmoq
    // returns cleanly (success) before connecting, and crucially never calls
    // source.next_object() -- the app's source is not consumed with no demand.
    {
        LibmoqLiveHandle live;
        live.cancel.store(true);
        int next_calls = 0;
        LiveObjectSource source;
        source.tracks = {LiveTrack{.track_name = "video",
                                   .media_type = LiveMediaType::kVideo,
                                   .codec = "av01"}};
        source.next_object = [&next_calls]() {
            ++next_calls;
            return std::optional<LiveObject>{};
        };

        PublisherConfig config;
        config.draft_version = DraftVersion::kDraft16;

        EndpointConfig endpoint;
        endpoint.transport = TransportKind::kRawQuic;
        endpoint.host = "192.0.2.1";  // TEST-NET-1: must never be dialed here
        endpoint.port = 4443;

        LibmoqPublishStats stats;
        const TransportStatus st = publish_live_objects_via_libmoq(
            source, config, endpoint, TlsConfig{}, stats, &live);
        ok &= expect(st.ok,
                     "expected a pre-set cancel to short-circuit publish_live_objects (no connect)");
        ok &= expect(next_calls == 0,
                     "expected the object source NOT to be consumed before the driver proceeds");
    }

    // -- Cancellation DURING readiness (the bug this fixes) ------------------
    // Network-free: libmoq_wait_ready() is the readiness primitive the drivers
    // use. With injected ops we simulate disconnect() firing mid-wait (as
    // request_cancel() does) and assert the wait returns kCancelled promptly --
    // not kTimeout. The endpoint interrupt latch is what makes the real wait()
    // return at once; here the fake wait() returns immediately each tick.
    {
        std::atomic<bool> cancel{false};
        int waits = 0;
        LibmoqReadyOps ops;
        ops.is_ready = [] { return false; };  // never becomes ready on its own
        ops.is_fatal = [] { return false; };
        ops.wait = [&waits, &cancel](std::uint64_t) -> int {
            if (++waits == 3) {
                cancel.store(true);  // disconnect() fires mid-wait
            }
            return 0;  // MOQ_OK
        };
        const LibmoqReadyOutcome outcome =
            libmoq_wait_ready(&cancel, /*timeout_us=*/0, /*step_us=*/1000, ops);
        ok &= expect(outcome == LibmoqReadyOutcome::kCancelled,
                     "expected readiness wait to return kCancelled when cancel fires mid-wait");
        ok &= expect(waits == 3, "expected the wait to stop at the cancel, not spin further");
    }

    // libmoq_wait_ready outcome coverage: ready / timeout / closed.
    {
        LibmoqReadyOps ready_ops{[] { return true; }, [] { return false; },
                                 [](std::uint64_t) { return 0; }};
        ok &= expect(libmoq_wait_ready(nullptr, 0, 1000, ready_ops) == LibmoqReadyOutcome::kReady,
                     "expected kReady when is_ready() is already true");

        int t_waits = 0;
        LibmoqReadyOps timeout_ops{[] { return false; }, [] { return false; },
                                   [&t_waits](std::uint64_t) {
                                       ++t_waits;
                                       return 0;
                                   }};
        ok &= expect(libmoq_wait_ready(nullptr, 3000, 1000, timeout_ops) ==
                         LibmoqReadyOutcome::kTimeout,
                     "expected kTimeout when readiness never arrives within the budget");

        LibmoqReadyOps closed_ops{[] { return false; }, [] { return false; },
                                  [](std::uint64_t) { return MOQ_ERR_CLOSED; }};
        ok &= expect(libmoq_wait_ready(nullptr, 0, 1000, closed_ops) == LibmoqReadyOutcome::kClosed,
                     "expected kClosed when the endpoint wait reports closed");
    }

    // -- Demand-wait primitive (libmoq_wait_demand) --------------------------
    // Network-free coverage of the subscriber-demand gate the drivers use to
    // avoid hanging/consuming sources against a lazy relay.
    {
        // Immediate demand: a subscriber already present returns at once.
        LibmoqDemandOps now{[] { return true; }, [] { return false; },
                            [] { return false; }, [](std::uint64_t) {}};
        ok &= expect(libmoq_wait_demand(nullptr, 0, 1000, now) == LibmoqDemandOutcome::kSubscriber,
                     "expected kSubscriber when demand already exists");

        // Callback wake: no demand until the 3rd wait, then a subscriber appears
        // (as the on_subscriber_joined callback would surface it).
        bool has = false;
        int waits = 0;
        LibmoqDemandOps wake{[&has] { return has; }, [] { return false; },
                             [] { return false; },
                             [&has, &waits](std::uint64_t) {
                                 if (++waits == 3) has = true;
                             }};
        ok &= expect(libmoq_wait_demand(nullptr, 0, 1000, wake) == LibmoqDemandOutcome::kSubscriber,
                     "expected kSubscriber after the demand callback wakes the wait");
        ok &= expect(waits == 3, "expected the wait to return as soon as demand appears");

        // Timeout: demand never arrives within the budget.
        LibmoqDemandOps never{[] { return false; }, [] { return false; },
                              [] { return false; }, [](std::uint64_t) {}};
        ok &= expect(libmoq_wait_demand(nullptr, 3000, 1000, never) == LibmoqDemandOutcome::kTimeout,
                     "expected kTimeout when no media subscriber arrives in time");

        // Fatal / closed.
        LibmoqDemandOps fatal{[] { return false; }, [] { return true; },
                              [] { return false; }, [](std::uint64_t) {}};
        ok &= expect(libmoq_wait_demand(nullptr, 0, 1000, fatal) == LibmoqDemandOutcome::kFatal,
                     "expected kFatal when the sender/endpoint is fatal");
        LibmoqDemandOps closed{[] { return false; }, [] { return false; },
                               [] { return true; }, [](std::uint64_t) {}};
        ok &= expect(libmoq_wait_demand(nullptr, 0, 1000, closed) == LibmoqDemandOutcome::kClosed,
                     "expected kClosed when the endpoint is closed");

        // Cancel: a live cancel returns kCancelled.
        std::atomic<bool> cancel{true};
        LibmoqDemandOps c{[] { return false; }, [] { return false; },
                          [] { return false; }, [](std::uint64_t) {}};
        ok &= expect(libmoq_wait_demand(&cancel, 0, 1000, c) == LibmoqDemandOutcome::kCancelled,
                     "expected kCancelled when a live cancel is set");
    }

    // -- Bounded blocking-retry primitive (libmoq_retry_blocking) ------------
    // Network-free: this is what bounds the batch write / end_track WOULD_BLOCK
    // loops so a stalled queue can no longer hang the publish.
    {
        const auto no_wait = [](std::uint64_t) {};
        const auto no = [] { return false; };
        int out = -1;

        // Immediate success, code captured.
        LibmoqRetryOps r_ok{[] { return MOQ_OK; }, no, no, {}, no_wait};
        ok &= expect(libmoq_retry_blocking(nullptr, 0, 1000, r_ok, &out) == LibmoqRetryOutcome::kOk &&
                         out == MOQ_OK,
                     "expected kOk when the op succeeds immediately");

        // Succeeds after two WOULD_BLOCKs (the retry actually retries).
        int attempts = 0;
        LibmoqRetryOps r_blockok{
            [&attempts] { return ++attempts < 3 ? MOQ_ERR_WOULD_BLOCK : MOQ_OK; }, no, no, {},
            no_wait};
        ok &= expect(libmoq_retry_blocking(nullptr, 0, 1000, r_blockok, nullptr) ==
                             LibmoqRetryOutcome::kOk &&
                         attempts == 3,
                     "expected kOk after retrying through WOULD_BLOCK");

        // A genuine (non-WOULD_BLOCK) error returns kError immediately with code.
        LibmoqRetryOps r_err{[] { return MOQ_ERR_INVAL; }, no, no, {}, no_wait};
        ok &= expect(libmoq_retry_blocking(nullptr, 0, 1000, r_err, &out) ==
                             LibmoqRetryOutcome::kError &&
                         out == MOQ_ERR_INVAL,
                     "expected kError (with the code) for a non-WOULD_BLOCK error");

        // WOULD_BLOCK forever -> bounded by timeout.
        LibmoqRetryOps r_block{[] { return MOQ_ERR_WOULD_BLOCK; }, no, no, {}, no_wait};
        ok &= expect(libmoq_retry_blocking(nullptr, 3000, 1000, r_block, nullptr) ==
                         LibmoqRetryOutcome::kTimeout,
                     "expected kTimeout when WOULD_BLOCK never clears");

        // WOULD_BLOCK + demand gone -> kNoDemand (batch write semantics).
        LibmoqRetryOps r_nd{[] { return MOQ_ERR_WOULD_BLOCK; }, no, no, no, no_wait};
        ok &= expect(libmoq_retry_blocking(nullptr, 0, 1000, r_nd, nullptr) ==
                         LibmoqRetryOutcome::kNoDemand,
                     "expected kNoDemand when the subscriber leaves mid-retry");

        // WOULD_BLOCK + fatal / closed / cancel.
        LibmoqRetryOps r_ft{[] { return MOQ_ERR_WOULD_BLOCK; }, [] { return true; }, no, {}, no_wait};
        ok &= expect(libmoq_retry_blocking(nullptr, 0, 1000, r_ft, nullptr) ==
                         LibmoqRetryOutcome::kFatal,
                     "expected kFatal during a WOULD_BLOCK retry");
        LibmoqRetryOps r_cl{[] { return MOQ_ERR_WOULD_BLOCK; }, no, [] { return true; }, {}, no_wait};
        ok &= expect(libmoq_retry_blocking(nullptr, 0, 1000, r_cl, nullptr) ==
                         LibmoqRetryOutcome::kClosed,
                     "expected kClosed during a WOULD_BLOCK retry");
        std::atomic<bool> rcancel{true};
        LibmoqRetryOps r_cn{[] { return MOQ_ERR_WOULD_BLOCK; }, no, no, {}, no_wait};
        ok &= expect(libmoq_retry_blocking(&rcancel, 0, 1000, r_cn, nullptr) ==
                         LibmoqRetryOutcome::kCancelled,
                     "expected kCancelled during a WOULD_BLOCK retry");
    }

    // -- Endpoint URL mapping ------------------------------------------------
    {
        EndpointConfig raw;
        raw.transport = TransportKind::kRawQuic;
        raw.host = "relay.example.com";
        raw.port = 4443;
        raw.path = "/moq";
        ok &= expect(libmoq_endpoint_url(raw) == "moqt://relay.example.com:4443/moq",
                     "expected raw QUIC URL to use moqt:// scheme");

        EndpointConfig wt;
        wt.transport = TransportKind::kWebTransport;
        wt.host = "relay.example.com";
        wt.port = 443;
        wt.path = "/moq";
        ok &= expect(libmoq_endpoint_url(wt) == "https://relay.example.com:443/moq",
                     "expected WebTransport URL to use https:// scheme");

        EndpointConfig no_path;
        no_path.transport = TransportKind::kRawQuic;
        no_path.host = "h";
        no_path.port = 1;
        ok &= expect(libmoq_endpoint_url(no_path) == "moqt://h:1/",
                     "expected empty path to default to '/'");
    }

    if (!ok) {
        std::cerr << "libmoq translation tests FAILED\n";
        return 1;
    }
    std::cout << "libmoq translation tests passed\n";
    return 0;
}
