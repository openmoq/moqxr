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
    other.sample_entry_type = "hvc1";
    check(find_loc_profile(other) == nullptr, "hvc1 has no profile before the HEVC task");
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
    check(find_loc_profile_for_codec("hvc1.1.6.L120.B0", true) == nullptr, "hvc1 not yet supported");
    const auto *aac = find_loc_profile_for_codec("mp4a.40.2", false);
    check(aac != nullptr && aac->accepts_live_track("mp4a.40.2", Bytes{0x12, 0x10}, false), "aac accepted");
    check(!aac->accepts_live_track("mp4a.40.2", Bytes{0x12}, false), "short aac config rejected");
    check(find_loc_profile_for_codec("mp4a.40.2", true) == nullptr, "audio codec string on video track");
}
} // namespace
int main() {
    try {
        avc_profile_matches_avc1_only();
        aac_profile_matches_mp4a_only();
        find_profile_returns_null_for_vvc1();
        avc_check_sample_flags_idr_independent();
        avc_accepts_live_track_matches_existing_gate();
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
    std::cout << "loc codec profile tests passed\n";
}
