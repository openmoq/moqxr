#include "openmoq/publisher/encoded_sample.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
using namespace openmoq::publisher;
namespace {
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
using Bytes = std::vector<std::uint8_t>;
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
void append(Bytes &b, const Bytes &p) { b.insert(b.end(), p.begin(), p.end()); }
Bytes words(std::initializer_list<std::uint32_t> values) {
    Bytes b;
    for (auto v : values)
        be(b, v);
    return b;
}
Bytes fragment_box(std::uint32_t flags, std::uint32_t base, std::uint32_t cto) {
    Bytes traf;
    append(traf, box("tfhd", words({0x020000, 1})));
    append(traf, box("tfdt", words({0, base})));
    append(traf, box("trun", words({flags, 2, 92, cto, 0})));
    return box("moof", box("traf", traf));
}
void synthetic() {
    TrackDescription t;
    t.track_id = 1;
    t.track_name = "video";
    t.handler_type = "vide";
    t.sample_entry_type = "avc1";
    t.timescale = 1000;
    t.codec_private = box("avcC", {1, 66, 0, 10, 252, 225, 0, 1, 0x67, 1, 0, 1, 0x68});
    t.fragment_defaults = TrackFragmentDefaults{10, 3, 0x02000000};
    const std::vector<TrackDescription> tracks{t};
    auto moof = fragment_box(0x01000801, 0,
                             0xfffffffb); // Signed -5 CTO and trex defaults.
    auto mdat = box("mdat", {2, 0x65, 0x80, 2, 0x41, 0x80});
    // moof length is intentionally used, not an assumed header length.
    const auto data_offset = static_cast<std::uint32_t>(moof.size() + 8);
    for (int n = 0; n < 4; ++n)
        moof[moof.size() - 12 + n] = data_offset >> (24 - 8 * n);
    const auto s = read_fragment_samples(moof, mdat, tracks);
    check(s.size() == 2 && s[0].presentation_time == -5 && s[1].presentation_time == 10, "signed CTO and DTS");
    check(s[0].bytes == Bytes({2, 0x65, 0x80}) && s[1].bytes == Bytes({2, 0x41, 0x80}) && s[0].duration == 10 &&
              s[0].independent,
          "defaults and per-sample payload");
    auto nonzero = moof;
    const auto boxes = parse_mp4_boxes(nonzero);
    const auto *time = find_child_box(*find_child_box(boxes[0], "traf"), "tfdt");
    nonzero[time->payload.offset + 7] = 100;
    check(read_fragment_samples(nonzero, mdat, tracks)[0].presentation_time == 95, "nonzero start preserved");
    auto unsigned_cto = moof;
    const auto parsed = parse_mp4_boxes(unsigned_cto);
    const auto *run = find_child_box(*find_child_box(parsed[0], "traf"), "trun");
    unsigned_cto[run->payload.offset] = 0;
    check(read_fragment_samples(unsigned_cto, mdat, tracks)[0].presentation_time == 4294967291LL,
          "unsigned version-zero CTO");
    auto bad = moof;
    bad[bad.size() - 12] = 0x7f;
    rejects([&] { read_fragment_samples(bad, mdat, tracks); });
    // Keep a valid VCL NAL in both samples so rejection cannot be explained by
    // the older no-coded-picture check.
    auto mixed_tracks = tracks;
    mixed_tracks.front().fragment_defaults->sample_size = 5;
    for (const std::uint8_t type : {7, 8, 13, 14, 15, 20, 21}) {
        const auto mixed = box("mdat", {1, type, 2, 0x65, 0x80, 2, 0x41, 0x80, 1, 6});
        rejects([&] { read_fragment_samples(moof, mixed, mixed_tracks); });
    }
    auto access_unit_samples = [&](const Bytes &sample) {
        auto au_tracks = tracks;
        au_tracks.front().fragment_defaults->sample_size = sample.size();
        Bytes payload = sample;
        append(payload, sample);
        return read_fragment_samples(moof, box("mdat", payload), au_tracks);
    };
    rejects([&] { access_unit_samples({2, 0x09, 0xf0, 2, 0x65, 0x80, 2, 0x09, 0xf0, 2, 0x65, 0x80}); });
    rejects([&] { access_unit_samples({2, 0x65, 0x80, 2, 0x65, 0x80}); });
    rejects([&] { access_unit_samples({2, 0x62, 0x80, 2, 0x65, 0x80}); });
    check(access_unit_samples({2, 0x65, 0x80, 2, 0x65, 0x40}).front().independent,
          "one picture with first_mb zero then nonzero slices is accepted");
    auto srt_tracks = tracks;
    srt_tracks.front().codec_private[12] = 255; // Four-byte AVCC lengths.
    srt_tracks.front().fragment_defaults->sample_size = 16;
    const auto srt_mdat = box("mdat", {0, 0, 0, 1, 0x67, 0,    0, 0, 1, 0x68, 0, 0, 0, 2, 0x65, 0x80,
                                       0, 0, 0, 2, 0x41, 0x80, 0, 0, 0, 6,    6, 0, 0, 0, 0,    0});
    const auto normalized = read_fragment_samples(moof, srt_mdat, srt_tracks, true);
    check(normalized.size() == 2 && normalized[0].bytes == Bytes({0, 0, 0, 2, 0x65, 0x80}) &&
              normalized[1].bytes.size() == 16 && normalized[0].presentation_time == -5,
          "SRT strips only identical SPS/PPS while preserving sample boundaries and timing");
    rejects([&] { read_fragment_samples(moof, srt_mdat, srt_tracks); });
    auto changed_sps = srt_mdat;
    changed_sps[12] = 0x27; // Same NAL type, different bytes from the avcC SPS.
    rejects([&] { read_fragment_samples(moof, changed_sps, srt_tracks, true); });
    for (const std::uint8_t type : {13, 14, 15, 20, 21}) {
        auto unsupported = srt_mdat;
        unsupported[12] = type;
        rejects([&] { read_fragment_samples(moof, unsupported, srt_tracks, true); });
    }
    mdat.pop_back();
    rejects([&] { read_fragment_samples(moof, mdat, tracks); });
    auto bad_config = t;
    bad_config.codec_private = box("avcC", {1, 66, 0, 10, 252, 224, 0});
    rejects([&] { loc_codec_config(bad_config); });
    t.protection = CencTrackProtection{};
    rejects([&] { loc_codec_config(t); });
}
void init_validation() {
    const auto source = parse_mp4_file("tests/fixtures/locmaf-progressive.mp4");
    ParsedMp4 init;
    init.tracks = source.tracks;
    for (const auto &b : source.top_level_boxes)
        if (b.type == "ftyp" || b.type == "moov") {
            const auto data = slice_bytes(source.bytes, b.span);
            init.bytes.insert(init.bytes.end(), data.begin(), data.end());
        }
    init.top_level_boxes = parse_mp4_boxes(init.bytes);
    check(find_first_box(init.top_level_boxes, "mdat") == nullptr, "init preflight needs no media data");
    validate_loc_init(init);
    rejects([&] { validate_loc_init(init, true); });
    auto fragmented_init = parse_mp4_file("tests/fixtures/locmaf-publisher.mp4");
    auto &top = fragmented_init.top_level_boxes;
    top.erase(
        std::remove_if(top.begin(), top.end(), [](const Mp4Box &b) { return b.type != "ftyp" && b.type != "moov"; }),
        top.end());
    validate_loc_init(fragmented_init, true);
    const auto *moov = find_first_box(init.top_level_boxes, "moov");
    const auto *trak = find_child_box(*moov, "trak");
    const auto *edts = find_child_box(*trak, "edts");
    check(edts != nullptr, "progressive fixture carries identity edit");
    const auto *edit = find_child_box(*edts, "elst");
    check(edit != nullptr && init.bytes[edit->payload.offset] == 0, "version-zero edit fixture");
    auto bad_edit = init;
    bad_edit.bytes[edit->payload.offset + 15] = 1;
    rejects([&] { validate_loc_init(bad_edit); });
    const auto *stbl = find_child_box(*find_child_box(*find_child_box(*trak, "mdia"), "minf"), "stbl");
    const auto *stsd = find_child_box(*stbl, "stsd");
    auto multiple_descriptions = init;
    multiple_descriptions.bytes[stsd->payload.offset + 7] = 2;
    rejects([&] { validate_loc_init(multiple_descriptions); });
}
void mixed_layout() {
    auto p = parse_mp4_file("tests/fixtures/locmaf-progressive.mp4");
    // A moof must not cause the populated initial sample tables to disappear.
    append(p.bytes, box("moof", {}));
    p.top_level_boxes = parse_mp4_boxes(p.bytes);
    rejects([&] { read_encoded_samples(p); });
}
void absolute_offsets() {
    const auto original = parse_mp4_file("tests/fixtures/locmaf-publisher.mp4");
    ParsedMp4 p;
    p.tracks = original.tracks;
    for (const auto &b : original.top_level_boxes)
        if (b.type == "ftyp" || b.type == "moov") {
            const auto data = slice_bytes(original.bytes, b.span);
            p.bytes.insert(p.bytes.end(), data.begin(), data.end());
        }
    const auto build = [&](std::uint32_t base) {
        Bytes traf;
        append(traf, box("tfhd", words({0x39, p.tracks.front().track_id, 0, base, 10, 6, 0x02000000})));
        append(traf, box("tfdt", words({0, 100})));
        append(traf, box("trun", words({1, 1, 0xfffffffb})));
        append(traf, box("trun", words({0, 1})));
        return box("moof", box("traf", traf));
    };
    const auto placeholder = build(0);
    const auto moof = build(p.bytes.size() + placeholder.size() + 8 + 5);
    const auto mdat = box("mdat", {0, 0, 0, 2, 0x65, 0x80, 0, 0, 0, 2, 0x41, 0x80});
    append(p.bytes, moof);
    append(p.bytes, mdat);
    p.top_level_boxes = parse_mp4_boxes(p.bytes);
    const auto samples = read_encoded_samples(p);
    check(samples.size() == 2 && samples[0].decode_time == 100 && samples[1].decode_time == 110,
          "absolute tfhd base plus negative trun offset and run continuation");
    check(samples[0].independent && !samples[1].independent,
          "actual IDR required despite inherited independent sample flags");
    rejects([&] { read_fragment_samples(moof, mdat, p.tracks); });
}
void bframes() {
    const auto f = parse_mp4_file("tests/fixtures/loc-bframes-fragmented.mp4");
    const auto p = parse_mp4_file("tests/fixtures/loc-bframes-progressive.mp4");
    const auto a = read_encoded_samples(f), b = read_encoded_samples(p);
    const std::int64_t times[]{8192, 20480, 12288, 16384};
    const std::size_t positions[]{921, 3026, 3838, 4375};
    const std::size_t sizes[]{2105, 812, 537, 436};
    check(a.size() == 4 && b.size() == 4, "B-frame count");
    for (std::size_t i = 0; i < 4; ++i) {
        check(a[i].decode_time == i * 4096 && a[i].presentation_time == times[i] && a[i].duration == 4096,
              "ffprobe B-frame timestamps");
        check(a[i].bytes == b[i].bytes && a[i].presentation_time == b[i].presentation_time &&
                  a[i].bytes.size() == sizes[i],
              "B-frame progressive/fragmented equality");
        check(std::equal(a[i].bytes.begin(), a[i].bytes.end(), f.bytes.begin() + positions[i]),
              "ffprobe B-frame byte positions");
    }
}
void fixtures() {
    const auto p = parse_mp4_file("tests/fixtures/locmaf-publisher.mp4");
    const auto progressive = parse_mp4_file("tests/fixtures/locmaf-progressive.mp4");
    const auto samples = read_encoded_samples(p), plain = read_encoded_samples(progressive);
    const std::uint32_t sizes[]{1066, 75, 462, 59};
    const std::size_t positions[]{895, 1961, 2156, 2618};
    check(samples.size() == 4 && plain.size() == 4, "ffprobe packet count");
    for (std::size_t i = 0; i < 4; ++i) {
        const auto &s = samples[i];
        check(s.bytes.size() == sizes[i] && s.decode_time == i * 4096 &&
                  s.presentation_time == std::int64_t(i * 4096) && s.duration == 4096 && s.independent == (i % 2 == 0),
              "ffprobe packet timing/size/flags");
        check(std::equal(s.bytes.begin(), s.bytes.end(), p.bytes.begin() + positions[i]),
              "ffprobe packet byte positions");
        check(s.bytes == plain[i].bytes && s.presentation_time == plain[i].presentation_time,
              "progressive matches fragmented packets");
    }
    check(loc_codec_config(p.tracks.front()).front() == 1, "AVC config omits box header");
    std::size_t count = 0;
    for (std::size_t i = 0; i < p.top_level_boxes.size(); ++i)
        if (p.top_level_boxes[i].type == "moof") {
            const auto live = read_fragment_samples(slice_bytes(p.bytes, p.top_level_boxes[i].span),
                                                    slice_bytes(p.bytes, p.top_level_boxes[i + 1].span), p.tracks);
            for (const auto &s : live) {
                check(s.bytes == samples[count].bytes && s.presentation_time == samples[count].presentation_time,
                      "live equals batch");
                ++count;
            }
        }
    check(count == 4, "live sample count");
    const auto audio = parse_mp4_file("tests/fixtures/locmaf-audio.mp4");
    const auto a = read_encoded_samples(audio);
    check(a.size() == 48 && a[0].bytes.size() == 142 && a[0].duration == 1024 && a[47].duration == 896,
          "AAC packet extraction");
    const auto config = loc_codec_config(audio.tracks.front());
    check(config.size() >= 2 && config[0] == 0x11 && config[1] == 0x88, "AAC AudioSpecificConfig");
}
} // namespace
int main() {
    try {
        init_validation();
        mixed_layout();
        synthetic();
        absolute_offsets();
        fixtures();
        bframes();
        std::cout << "Encoded sample tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
