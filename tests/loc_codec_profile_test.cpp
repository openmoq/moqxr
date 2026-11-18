#include "openmoq/publisher/loc_codec_profile.h"
#include <functional>
#include <iostream>
#include <stdexcept>
using namespace openmoq::publisher;
namespace {
using Bytes = std::vector<std::uint8_t>;
void check(bool v, const char *message) {
    if (!v)
        throw std::runtime_error(message);
}
void rejects(const std::function<void()> &f) {
    try {
        f();
    } catch (const std::runtime_error &) {
        return;
    }
    throw std::runtime_error("expected rejection");
}
void be(Bytes &b, std::uint32_t v) {
    for (int n = 24; n >= 0; n -= 8)
        b.push_back(v >> n);
}
Bytes box(const char *type, Bytes p) {
    Bytes b;
    be(b, p.size() + 8);
    b.insert(b.end(), type, type + 4);
    b.insert(b.end(), p.begin(), p.end());
    return b;
}
TrackDescription avc_track() {
    TrackDescription t;
    t.handler_type = "vide";
    t.sample_entry_type = "avc1";
    t.codec = "avc1.42E01E";
    t.timescale = 1000;
    t.codec_private = box("avcC", {1, 66, 0, 10, 252, 225, 0, 1, 0x67, 1, 0, 1, 0x68});
    return t;
}
TrackDescription aac_track() {
    TrackDescription t;
    t.handler_type = "soun";
    t.sample_entry_type = "mp4a";
    t.codec = "mp4a.40.2";
    t.timescale = 48000;
    return t;
}
void avc_profile_matches_avc1_only() {
    check(find_loc_profile(avc_track()) != nullptr, "avc1 video has a profile");
    auto other = avc_track();
    other.sample_entry_type = "avc3";
    check(find_loc_profile(other) == nullptr, "avc3 (in-band parameter sets) is not supported");
    auto audio = avc_track();
    audio.handler_type = "soun";
    check(find_loc_profile(audio) == nullptr, "avc1 sample entry on audio handler is not matched");
}
void aac_profile_matches_mp4a_only() {
    check(find_loc_profile(aac_track()) != nullptr, "mp4a audio has a profile");
    auto video = aac_track();
    video.handler_type = "vide";
    check(find_loc_profile(video) == nullptr, "mp4a on a video handler is not matched");
}
void find_profile_returns_null_for_vvc1() {
    auto t = avc_track();
    t.sample_entry_type = "vvc1";
    check(find_loc_profile(t) == nullptr, "vvc1 unsupported");
}
void avc_check_sample_flags_idr_independent() {
    const auto t = avc_track();
    const auto *p = find_loc_profile(t);
    const Bytes idr{2, 0x65, 0x80};
    const Bytes delta{2, 0x41, 0x80};
    check(p->check_sample(t, idr, false).independent, "IDR is independent");
    check(!p->check_sample(t, delta, false).independent, "non-IDR slice is not independent");
    rejects([&] { p->check_sample(t, Bytes{2, 0x67, 0x80, 2, 0x65, 0x80}, false); });
}
void avc_accepts_live_track_matches_existing_gate() {
    const Bytes avcc(7, 0);
    Bytes good = avcc;
    good[0] = 1;
    const auto *avc = find_loc_profile_for_codec("avc1.42E01E", true);
    check(avc != nullptr && avc->accepts_live_track("avc1.42E01E", good, true), "avc accepted");
    check(!avc->accepts_live_track("avc1.42E01E", Bytes(6, 1), true), "short avc config rejected");
    check(find_loc_profile_for_codec("vvc1.1.L51", true) == nullptr, "vvc1 not supported");
    const auto *aac = find_loc_profile_for_codec("mp4a.40.2", false);
    check(aac != nullptr && aac->accepts_live_track("mp4a.40.2", Bytes{0x12, 0x10}, false), "aac accepted");
    check(!aac->accepts_live_track("mp4a.40.2", Bytes{0x12}, false), "short aac config rejected");
    check(find_loc_profile_for_codec("mp4a.40.2", true) == nullptr, "audio codec string on video track");
}

// ---- HEVC ----
void append(Bytes &b, const Bytes &p) { b.insert(b.end(), p.begin(), p.end()); }
Bytes nal(std::initializer_list<std::uint8_t> bytes) { return Bytes(bytes); }
const Bytes kVps = {0x40, 0x01, 0xAA};
const Bytes kSps = {0x42, 0x01, 0xBB};
const Bytes kPps = {0x44, 0x01, 0xCC};
// HEVCDecoderConfigurationRecord with the given parameter set arrays.
Bytes hvcc_payload(std::uint8_t length_size_minus_one, const std::vector<std::pair<std::uint8_t, Bytes>> &arrays) {
    Bytes b = {1, 1, 0x60, 0, 0, 0, 0x90, 0, 0, 0, 0, 0, 120, 0xF0, 0, 0xFC, 0xFD, 0xF8, 0xF8, 0, 0};
    b.push_back(static_cast<std::uint8_t>(0x08 | length_size_minus_one)); // temporalIdNested + length size
    b.push_back(static_cast<std::uint8_t>(arrays.size()));
    for (const auto &[type, unit] : arrays) {
        b.push_back(0x80 | type);
        b.push_back(0);
        b.push_back(1);
        b.push_back(0);
        b.push_back(static_cast<std::uint8_t>(unit.size()));
        append(b, unit);
    }
    return b;
}
const std::vector<std::pair<std::uint8_t, Bytes>> kAllSets = {{32, kVps}, {33, kSps}, {34, kPps}};
TrackDescription hevc_track(Bytes payload = hvcc_payload(3, kAllSets)) {
    TrackDescription t;
    t.handler_type = "vide";
    t.sample_entry_type = "hvc1";
    t.codec = "hvc1.1.6.L120.B0";
    t.timescale = 1000;
    t.codec_private = box("hvcC", std::move(payload));
    return t;
}
// Four-byte length prefixed NAL units.
Bytes hevc_sample(const std::vector<Bytes> &nals) {
    Bytes b;
    for (const auto &unit : nals) {
        be(b, static_cast<std::uint32_t>(unit.size()));
        append(b, unit);
    }
    return b;
}
const Bytes kIdr = nal({0x26, 0x01, 0x80});      // IDR_W_RADL, first_slice_segment_in_pic_flag = 1
const Bytes kCra = nal({0x2A, 0x01, 0x80});      // CRA_NUT
const Bytes kTrail = nal({0x02, 0x01, 0x80});    // TRAIL_R
const Bytes kIdrContinuation = nal({0x26, 0x01, 0x00}); // second slice of the same picture
void hevc_extract_config_returns_hvcc_payload() {
    const auto payload = hvcc_payload(3, kAllSets);
    const auto t = hevc_track(payload);
    check(find_loc_profile(t) != nullptr, "hvc1 video has a profile");
    check(find_loc_profile(t)->extract_config(t) == payload, "Video Config is the hvcC payload");
    auto hev1 = t;
    hev1.sample_entry_type = "hev1";
    check(find_loc_profile(hev1) != nullptr, "hev1 video has a profile");
}
void hevc_rejects_missing_sps() {
    const auto t = hevc_track(hvcc_payload(3, {{32, kVps}, {34, kPps}}));
    rejects([&] { find_loc_profile(t)->extract_config(t); });
}
void hevc_rejects_length_size_3() {
    const auto t = hevc_track(hvcc_payload(2, kAllSets));
    rejects([&] { find_loc_profile(t)->extract_config(t); });
}
void hevc_rejects_truncated_array() {
    auto payload = hvcc_payload(3, kAllSets);
    payload.pop_back();
    const auto t = hevc_track(payload);
    rejects([&] { find_loc_profile(t)->extract_config(t); });
}
void hevc_idr_sample_is_independent() {
    const auto t = hevc_track();
    const auto *p = find_loc_profile(t);
    check(p->check_sample(t, hevc_sample({kIdr}), false).independent, "IDR is independent");
    check(p->check_sample(t, hevc_sample({kCra}), false).independent, "CRA is independent");
    check(p->check_sample(t, hevc_sample({kIdr, kIdrContinuation}), false).independent,
          "two slices of one picture are one picture");
}
void hevc_trail_sample_is_not_independent() {
    const auto t = hevc_track();
    check(!find_loc_profile(t)->check_sample(t, hevc_sample({kTrail}), false).independent, "TRAIL_R is not independent");
}
void hevc_rejects_two_pictures_in_one_sample() {
    const auto t = hevc_track();
    rejects([&] { find_loc_profile(t)->check_sample(t, hevc_sample({kIdr, kTrail}), false); });
}
void hevc_rejects_sample_without_picture() {
    const auto t = hevc_track();
    rejects([&] { find_loc_profile(t)->check_sample(t, hevc_sample({nal({0x4E, 0x01, 0x05})}), false); });
}
void hevc_rejects_layered_nal() {
    const auto t = hevc_track();
    rejects([&] { find_loc_profile(t)->check_sample(t, hevc_sample({nal({0x26, 0x09, 0x80})}), false); }); // nuh_layer_id 1
}
void hevc_rejects_bad_length_prefix() {
    const auto t = hevc_track();
    Bytes bad;
    be(bad, 99);
    append(bad, kIdr);
    rejects([&] { find_loc_profile(t)->check_sample(t, bad, false); });
}
void hevc_rejects_inband_parameter_sets_unless_matching_stripped() {
    const auto t = hevc_track();
    const auto *p = find_loc_profile(t);
    const auto with_sets = hevc_sample({kVps, kSps, kPps, kIdr});
    rejects([&] { p->check_sample(t, with_sets, false); });
    const auto stripped = p->check_sample(t, with_sets, true);
    check(stripped.independent && stripped.normalized && *stripped.normalized == hevc_sample({kIdr}),
          "matching parameter sets are stripped");
    const auto changed = hevc_sample({nal({0x42, 0x01, 0xEE}), kIdr});
    rejects([&] { p->check_sample(t, changed, true); });
}
void hevc_accepts_live_track_requires_hvc_prefix_and_hvcc() {
    const auto payload = hvcc_payload(3, kAllSets);
    const auto *hevc = find_loc_profile_for_codec("hvc1.1.6.L120.B0", true);
    check(hevc != nullptr && hevc->accepts_live_track("hvc1.1.6.L120.B0", payload, true), "hevc accepted");
    check(find_loc_profile_for_codec("hev1.1.6.L120.B0", true) == hevc, "hev1 uses the same profile");
    check(!hevc->accepts_live_track("hvc1.1.6.L120.B0", Bytes{1, 66, 0, 10, 252, 225, 0, 1, 0x67}, true),
          "avcC bytes are not an hvcC");
    check(find_loc_profile_for_codec("avc1.42E01E", true) != hevc, "avc1 string does not select HEVC");
}

// ---- Opus ----
Bytes dops(std::uint8_t channels, std::uint16_t pre_skip, std::uint32_t rate, std::int16_t gain, std::uint8_t family,
           const Bytes &mapping = {}) {
    Bytes b = {0, channels, static_cast<std::uint8_t>(pre_skip >> 8), static_cast<std::uint8_t>(pre_skip)};
    be(b, rate);
    b.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(gain) >> 8));
    b.push_back(static_cast<std::uint8_t>(gain));
    b.push_back(family);
    append(b, mapping);
    return b;
}
TrackDescription opus_track(Bytes payload) {
    TrackDescription t;
    t.handler_type = "soun";
    t.sample_entry_type = "Opus";
    t.codec = "opus";
    t.timescale = 48000;
    t.codec_private = box("dOps", std::move(payload));
    return t;
}
void opus_dops_converts_to_opushead_family0() {
    const auto t = opus_track(dops(2, 312, 48000, 0, 0));
    const auto *p = find_loc_profile(t);
    check(p != nullptr, "Opus audio has a profile");
    // Matches the OpusHead FFmpeg derives from the same dOps.
    const Bytes expected = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0x38, 0x01, 0x80, 0xBB, 0, 0, 0, 0, 0};
    check(p->extract_config(t) == expected, "family 0 OpusHead is little endian and 19 bytes");
    auto lower = t;
    lower.sample_entry_type = "opus";
    check(find_loc_profile(lower) != nullptr, "lowercase opus entry has a profile");
}
void opus_dops_converts_family1_with_mapping_table() {
    const Bytes mapping = {0, 4, 1, 2, 3, 5};
    Bytes payload = dops(6, 312, 44100, -256, 1, Bytes{4, 2});
    append(payload, mapping);
    const auto t = opus_track(payload);
    const Bytes expected = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 6, 0x38, 0x01, 0x44, 0xAC, 0, 0, 0x00, 0xFF, 1,
                            4, 2, 0, 4, 1, 2, 3, 5};
    check(find_loc_profile(t)->extract_config(t) == expected, "family 1 OpusHead carries stream counts and mapping");
}
void opus_rejects_malformed_dops() {
    auto truncated = dops(2, 312, 48000, 0, 0);
    truncated.pop_back();
    const auto short_track = opus_track(truncated);
    rejects([&] { find_loc_profile(short_track)->extract_config(short_track); });
    auto versioned = dops(2, 312, 48000, 0, 0);
    versioned[0] = 1;
    const auto version_track = opus_track(versioned);
    rejects([&] { find_loc_profile(version_track)->extract_config(version_track); });
    const auto three_channels_family0 = opus_track(dops(3, 312, 48000, 0, 0));
    rejects([&] { find_loc_profile(three_channels_family0)->extract_config(three_channels_family0); });
    const auto missing_table = opus_track(dops(6, 312, 48000, 0, 1, Bytes{4, 2}));
    rejects([&] { find_loc_profile(missing_table)->extract_config(missing_table); });
    auto trailing = dops(2, 312, 48000, 0, 0);
    trailing.push_back(0);
    const auto trailing_track = opus_track(trailing);
    rejects([&] { find_loc_profile(trailing_track)->extract_config(trailing_track); });
}
void opus_validates_channel_mapping() {
    auto track_for = [](std::uint8_t channels, std::uint8_t family, std::uint8_t streams, std::uint8_t coupled,
                        const Bytes &mapping) {
        Bytes tail = {streams, coupled};
        append(tail, mapping);
        return opus_track(dops(channels, 312, 48000, 0, family, tail));
    };
    auto accepted = [&](const TrackDescription &t) {
        try {
            find_loc_profile(t)->extract_config(t);
            return true;
        } catch (const std::runtime_error &) {
            return false;
        }
    };
    check(accepted(track_for(6, 1, 4, 2, {0, 4, 1, 2, 3, 5})), "valid 5.1 mapping");
    check(accepted(track_for(3, 255, 3, 0, {0, 1, 2})), "valid family 255 mapping");
    check(accepted(track_for(2, 1, 1, 1, {0, 255})), "255 marks a silent channel");
    check(!accepted(track_for(6, 1, 0, 3, {9, 9, 9, 9, 9, 9})), "zero streams and out of range entries");
    check(!accepted(track_for(2, 1, 1, 2, {0, 1})), "coupled streams exceed streams");
    check(!accepted(track_for(2, 1, 1, 1, {0, 2})), "mapping entry beyond streams plus coupled");
    check(!accepted(track_for(12, 1, 12, 0, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11})), "family 1 allows at most 8 channels");
    check(!accepted(track_for(4, 2, 4, 0, {0, 1, 2, 3})), "ambisonics families are not supported");
    // The explicit object API validates the same table inside an OpusHead.
    const auto good = track_for(6, 1, 4, 2, {0, 4, 1, 2, 3, 5});
    auto head = find_loc_profile(good)->extract_config(good);
    const auto *opus = find_loc_profile_for_codec("opus", false);
    check(opus->accepts_live_track("opus", head, false), "valid OpusHead accepted");
    head.back() = 9; // mapping entry beyond streams plus coupled
    check(!opus->accepts_live_track("opus", head, false), "OpusHead with an out of range mapping rejected");
}
void opus_sample_is_always_independent() {
    const auto t = opus_track(dops(2, 312, 48000, 0, 0));
    const auto *p = find_loc_profile(t);
    check(p->check_sample(t, Bytes{0xFC, 0xFF, 0xFE}, false).independent, "Opus packets are independent");
    rejects([&] { p->check_sample(t, Bytes{}, false); });
}
void opus_accepts_live_track_requires_codec_opus_and_opushead() {
    const auto t = opus_track(dops(2, 312, 48000, 0, 0));
    const auto head = find_loc_profile(t)->extract_config(t);
    const auto *opus = find_loc_profile_for_codec("opus", false);
    check(opus != nullptr && opus->accepts_live_track("opus", head, false), "opus accepted");
    check(find_loc_profile_for_codec("opus", true) == nullptr, "opus on a video track");
    check(!opus->accepts_live_track("opus", Bytes{1, 2, 3}, false), "non-OpusHead bytes rejected");
    auto bad_magic = head;
    bad_magic[0] = 'X';
    check(!opus->accepts_live_track("opus", bad_magic, false), "wrong magic rejected");
    check(!opus->accepts_live_track("opus", Bytes(head.begin(), head.end() - 1), false), "truncated OpusHead rejected");
}

// ---- AV1 ----
Bytes leb128(std::size_t v) {
    Bytes b;
    do {
        std::uint8_t byte = v & 0x7f;
        v >>= 7;
        if (v)
            byte |= 0x80;
        b.push_back(byte);
    } while (v);
    return b;
}
// One OBU in the low-overhead format (has_size_field = 1).
Bytes obu(std::uint8_t type, const Bytes &payload, int extension = -1) {
    Bytes b = {static_cast<std::uint8_t>((type << 3) | (extension >= 0 ? 4 : 0) | 2)};
    if (extension >= 0)
        b.push_back(static_cast<std::uint8_t>(extension));
    append(b, leb128(payload.size()));
    append(b, payload);
    return b;
}
Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes b;
    for (const auto &p : parts)
        append(b, p);
    return b;
}
const Bytes kSeqPayload = {0x00, 0x00, 0x00, 0x00};   // seq_profile 0, reduced_still_picture_header 0
const Bytes kReducedSeqPayload = {0x08, 0x00, 0x00, 0x00}; // reduced_still_picture_header 1
Bytes av1c_record(const Bytes &config_obus) { return cat({Bytes{0x81, 0x00, 0x0C, 0x00}, config_obus}); }
TrackDescription av1_track(Bytes record = av1c_record(obu(1, kSeqPayload))) {
    TrackDescription t;
    t.handler_type = "vide";
    t.sample_entry_type = "av01";
    t.codec = "av01.0.04M.08";
    t.timescale = 1000;
    t.codec_private = box("av1C", std::move(record));
    return t;
}
const Bytes kTd = obu(2, {});
const Bytes kKeyFrame = obu(6, {0x10, 0xAA});    // show_existing 0, KEY_FRAME, show_frame 1
const Bytes kInterFrame = obu(6, {0x30, 0xAA});  // INTER_FRAME, show_frame 1
const Bytes kHiddenFrame = obu(6, {0x20, 0xAA}); // INTER_FRAME, show_frame 0
const Bytes kShowExisting = obu(3, {0x80});      // show_existing_frame 1
LocSampleCheck av1_check(const Bytes &sample, bool strip = false, const TrackDescription &t = av1_track()) {
    return find_loc_profile(t)->check_sample(t, sample, strip);
}
void av1_extract_config_returns_av1c_payload() {
    const auto record = av1c_record(obu(1, kSeqPayload));
    const auto t = av1_track(record);
    check(find_loc_profile(t) != nullptr, "av01 video has a profile");
    check(find_loc_profile(t)->extract_config(t) == record, "Video Config is the av1C payload");
    const auto empty_config = av1_track(av1c_record({}));
    check(find_loc_profile(empty_config)->extract_config(empty_config) == av1c_record({}),
          "av1C without configOBUs is valid");
}
void av1_rejects_bad_config() {
    for (const Bytes &record : {Bytes{0x01, 0x00, 0x0C, 0x00}, Bytes{0x82, 0x00, 0x0C, 0x00}, Bytes{0x81, 0x00, 0x0C, 0x20},
                                Bytes{0x81, 0x00, 0x0C}, av1c_record(Bytes{0x0A, 0x05, 0x00})}) {
        const auto t = av1_track(record);
        rejects([&] { find_loc_profile(t)->extract_config(t); });
    }
    const auto unsized = av1_track(av1c_record(Bytes{0x08, 0x00})); // OBU_SEQUENCE_HEADER without a size field
    rejects([&] { find_loc_profile(unsized)->extract_config(unsized); });
}
void av1_key_frame_temporal_unit_is_independent() {
    check(av1_check(cat({kTd, kKeyFrame})).independent, "key frame is independent");
    check(av1_check(cat({kTd, obu(1, kSeqPayload), kKeyFrame})).independent, "identical in-band sequence header is accepted");
}
void av1_inter_frame_is_not_independent() {
    check(!av1_check(cat({kTd, kInterFrame})).independent, "inter frame is not independent");
    check(!av1_check(cat({kTd, obu(6, {0x50, 0xAA})})).independent, "intra-only frame is not independent");
}
void av1_show_existing_frame_is_not_independent() {
    check(!av1_check(cat({kTd, kShowExisting})).independent, "show_existing_frame is not independent");
}
void av1_hidden_plus_shown_frame_is_one_temporal_unit() {
    const auto check_result = av1_check(cat({kTd, kHiddenFrame, kShowExisting}));
    check(!check_result.independent, "hidden inter frame followed by a shown frame");
}
void av1_reduced_still_picture_header_is_independent() {
    const auto t = av1_track(av1c_record(obu(1, kReducedSeqPayload)));
    check(av1_check(cat({kTd, obu(6, {0x00, 0xAA})}), false, t).independent, "reduced still picture header is always a key frame");
}
void av1_rejects_two_shown_frames_in_one_sample() {
    rejects([&] { av1_check(cat({kTd, kKeyFrame, kInterFrame})); });
    rejects([&] { av1_check(cat({kTd, kHiddenFrame})); }); // no shown frame
    rejects([&] { av1_check(cat({kTd})); });               // no frame at all
}
void av1_rejects_multiple_temporal_units() {
    rejects([&] { av1_check(cat({kTd, kKeyFrame, kTd, kInterFrame})); });
}
void av1_checks_inband_sequence_header_against_av1c() {
    // FFmpeg's av1C sequence header differs from the in-band copy in trailing bits, so only
    // seq_profile, still_picture and reduced_still_picture_header must agree.
    check(av1_check(cat({kTd, obu(1, Bytes{0x00, 0x00, 0x00, 0x01}), kKeyFrame})).independent,
          "in-band sequence header with the same profile is accepted");
    rejects([&] { av1_check(cat({kTd, obu(1, Bytes{0x20, 0x00, 0x00, 0x00}), kKeyFrame})); }); // seq_profile 1
    rejects([&] { av1_check(cat({kTd, obu(1, Bytes{0x08, 0x00, 0x00, 0x00}), kKeyFrame})); }); // reduced header
    rejects([&] { av1_check(cat({kTd, obu(1, {}), kKeyFrame})); });                             // empty
}
void av1_rejects_layers_tile_lists_and_malformed_obus() {
    rejects([&] { av1_check(cat({kTd, obu(6, {0x10, 0xAA}, 0x08)})); }); // spatial_id 1
    check(av1_check(cat({kTd, obu(6, {0x10, 0xAA}, 0x20)})).independent, "temporal sublayers stay in one subgroup");
    rejects([&] { av1_check(cat({kTd, obu(8, {0x00}), kKeyFrame})); });  // OBU_TILE_LIST
    rejects([&] { av1_check(cat({kTd, kKeyFrame, Bytes{0x32, 0x09, 0x00}})); }); // size beyond the sample
    rejects([&] { av1_check(cat({kTd, Bytes{0x30, 0x10}})); });          // frame OBU without a size field
}
void av1_accepts_live_track_requires_av01_prefix() {
    const auto record = av1c_record(obu(1, kSeqPayload));
    const auto *av1 = find_loc_profile_for_codec("av01.0.04M.08", true);
    check(av1 != nullptr && av1->accepts_live_track("av01.0.04M.08", record, true), "av1 accepted");
    check(!av1->accepts_live_track("av01.0.04M.08", Bytes{1, 66, 0, 10, 252, 225, 0, 1, 0x67}, true), "avcC bytes are not an av1C");
    check(find_loc_profile_for_codec("av01.0.04M.08", false) == nullptr, "av01 on an audio track");
}

// ---- VP9 ----
// vpcC payload: version 1, flags 0, profile, level, bitDepth/chroma/range, colour fields, no init data.
Bytes vpcc_payload(std::uint8_t profile = 0, std::uint8_t level = 10, std::uint8_t depth_chroma = 0x82) {
    return {1, 0, 0, 0, profile, level, depth_chroma, 2, 2, 2, 0, 0};
}
TrackDescription vp9_track(Bytes payload = vpcc_payload()) {
    TrackDescription t;
    t.handler_type = "vide";
    t.sample_entry_type = "vp09";
    t.codec = "vp09.00.10.08";
    t.timescale = 1000;
    t.codec_private = box("vpcC", std::move(payload));
    return t;
}
LocSampleCheck vp9_check(const Bytes &sample) {
    const auto t = vp9_track();
    return find_loc_profile(t)->check_sample(t, sample, false);
}
void vp9_has_no_video_config() {
    const auto t = vp9_track();
    const auto *p = find_loc_profile(t);
    check(p != nullptr, "vp09 video has a profile");
    check(!p->has_config(), "WebCodecs defines no description for VP9");
    check(p->extract_config(t).empty(), "no Video Config bytes");
}
void vp9_validate_vpcc_rejects_bad_config() {
    auto bad_version = vpcc_payload();
    bad_version[0] = 0;
    auto with_init_data = vpcc_payload();
    with_init_data[11] = 1;
    with_init_data.push_back(0xAA);
    auto truncated = vpcc_payload();
    truncated.pop_back();
    auto trailing = vpcc_payload();
    trailing.push_back(0);
    for (const auto &payload : {bad_version, with_init_data, truncated, trailing}) {
        const auto t = vp9_track(payload);
        rejects([&] { find_loc_profile(t)->extract_config(t); });
    }
}
void vp9_key_frame_is_independent() {
    check(vp9_check(Bytes{0x82, 0x49, 0x83}).independent, "profile 0 key frame");
    check(vp9_check(Bytes{0x90, 0x00}).independent, "profile 2 key frame");
}
void vp9_inter_frame_is_not_independent() {
    check(!vp9_check(Bytes{0x86, 0x00}).independent, "profile 0 inter frame");
}
void vp9_show_existing_frame_is_not_independent() {
    check(!vp9_check(Bytes{0x88, 0x00}).independent, "show_existing_frame");
}
void vp9_profile3_reserved_bit_handled() {
    check(vp9_check(Bytes{0xB0, 0x00}).independent, "profile 3 key frame skips the reserved bit");
    check(!vp9_check(Bytes{0xB2, 0x00}).independent, "profile 3 inter frame");
}
void vp9_rejects_bad_frames() {
    rejects([&] { vp9_check(Bytes{}); });
    rejects([&] { vp9_check(Bytes{0x42, 0x00}); }); // frame_marker is not 2
}
void vp9_accepts_live_track_requires_vp09_prefix_without_init_data() {
    const auto *vp9 = find_loc_profile_for_codec("vp09.00.10.08", true);
    check(vp9 != nullptr && vp9->accepts_live_track("vp09.00.10.08", Bytes{}, true), "vp9 accepted without init data");
    check(!vp9->accepts_live_track("vp09.00.10.08", Bytes{1, 2, 3}, true), "init data is not defined for VP9");
    check(find_loc_profile_for_codec("vp09.00.10.08", false) == nullptr, "vp09 on an audio track");
}
} // namespace
int main() {
    try {
        avc_profile_matches_avc1_only();
        aac_profile_matches_mp4a_only();
        find_profile_returns_null_for_vvc1();
        avc_check_sample_flags_idr_independent();
        avc_accepts_live_track_matches_existing_gate();
        hevc_extract_config_returns_hvcc_payload();
        hevc_rejects_missing_sps();
        hevc_rejects_length_size_3();
        hevc_rejects_truncated_array();
        hevc_idr_sample_is_independent();
        hevc_trail_sample_is_not_independent();
        hevc_rejects_two_pictures_in_one_sample();
        hevc_rejects_sample_without_picture();
        hevc_rejects_layered_nal();
        hevc_rejects_bad_length_prefix();
        hevc_rejects_inband_parameter_sets_unless_matching_stripped();
        hevc_accepts_live_track_requires_hvc_prefix_and_hvcc();
        opus_dops_converts_to_opushead_family0();
        opus_dops_converts_family1_with_mapping_table();
        opus_rejects_malformed_dops();
        opus_validates_channel_mapping();
        opus_sample_is_always_independent();
        opus_accepts_live_track_requires_codec_opus_and_opushead();
        av1_extract_config_returns_av1c_payload();
        av1_rejects_bad_config();
        av1_key_frame_temporal_unit_is_independent();
        av1_inter_frame_is_not_independent();
        av1_show_existing_frame_is_not_independent();
        av1_hidden_plus_shown_frame_is_one_temporal_unit();
        av1_reduced_still_picture_header_is_independent();
        av1_rejects_two_shown_frames_in_one_sample();
        av1_rejects_multiple_temporal_units();
        av1_checks_inband_sequence_header_against_av1c();
        av1_rejects_layers_tile_lists_and_malformed_obus();
        av1_accepts_live_track_requires_av01_prefix();
        vp9_has_no_video_config();
        vp9_validate_vpcc_rejects_bad_config();
        vp9_key_frame_is_independent();
        vp9_inter_frame_is_not_independent();
        vp9_show_existing_frame_is_not_independent();
        vp9_profile3_reserved_bit_handled();
        vp9_rejects_bad_frames();
        vp9_accepts_live_track_requires_vp09_prefix_without_init_data();
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
    std::cout << "loc codec profile tests passed\n";
}
