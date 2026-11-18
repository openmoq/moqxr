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
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
    std::cout << "loc codec profile tests passed\n";
}
