#include "openmoq/publisher/encoded_sample.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace openmoq::publisher {
namespace {
constexpr std::size_t kMaxSamples = 1000000;
constexpr std::uint64_t kMaxSampleBytes = 512ULL * 1024 * 1024;
struct Samples {
    std::vector<EncodedSample> values;
    std::uint64_t byte_count = 0;
    std::size_t size() const { return values.size(); }
    void push_back(EncodedSample sample) { values.push_back(std::move(sample)); }
};
[[noreturn]] void fail(const char *s) { throw std::runtime_error(std::string("LOC sample reader: ") + s); }
struct Reader {
    std::span<const std::uint8_t> b;
    std::size_t pos = 0;
    std::uint64_t get(std::size_t n) {
        if (n > b.size() - pos)
            fail("truncated box");
        std::uint64_t v = 0;
        while (n--)
            v = (v << 8) | b[pos++];
        return v;
    }
    void skip(std::size_t n) {
        if (n > b.size() - pos)
            fail("truncated box");
        pos += n;
    }
    void end() {
        if (pos != b.size())
            fail("unexpected box fields");
    }
};
Reader reader(const Mp4Box &box, std::span<const std::uint8_t> bytes) { return {slice_bytes(bytes, box.payload)}; }
std::uint64_t add(std::uint64_t a, std::uint64_t b) {
    if (b > UINT64_MAX - a)
        fail("integer overflow");
    return a + b;
}
std::uint64_t offset(std::uint64_t a, std::int64_t b) {
    if (b >= 0)
        return add(a, static_cast<std::uint64_t>(b));
    const auto magnitude = static_cast<std::uint64_t>(-(b + 1)) + 1;
    if (a < magnitude)
        fail("negative data offset");
    return a - magnitude;
}
std::int64_t pts(std::uint64_t dts, std::int64_t cto) {
    if (dts > INT64_MAX)
        fail("decode timestamp exceeds signed timeline");
    const auto d = static_cast<std::int64_t>(dts);
    if (cto > 0 && d > INT64_MAX - cto)
        fail("presentation timestamp overflow");
    return d + cto;
}
const Mp4Box &child(const Mp4Box &b, const char *type) {
    const auto *p = find_child_box(b, type);
    if (!p)
        fail("missing required box");
    return *p;
}
void clear_track(const TrackDescription &t) {
    if (t.protection || t.sample_entry_type == "encv" || t.sample_entry_type == "enca")
        fail("encrypted input unsupported");
    if (!t.timescale)
        fail("zero timescale");
    if (!((t.handler_type == "vide" && t.sample_entry_type == "avc1") ||
          (t.handler_type == "soun" && t.sample_entry_type == "mp4a")))
        fail("unsupported codec/sample description");
}
// Read only first_mb_in_slice, needed to reject multiple coded pictures in a
// single MP4 sample. This is not a full H.264 slice-header/decoder validator.
std::uint32_t first_macroblock(std::span<const std::uint8_t> ebsp) {
    std::size_t cursor = 0;
    unsigned zeros = 0, bits_left = 0;
    std::uint8_t current = 0;
    auto bit = [&]() -> unsigned {
        if (!bits_left) {
            if (cursor == ebsp.size())
                fail("truncated AVC slice header");
            current = ebsp[cursor++];
            if (zeros >= 2 && current == 3) {
                if (cursor == ebsp.size() || ebsp[cursor] > 3)
                    fail("invalid AVC emulation prevention");
                current = ebsp[cursor++];
                zeros = 0;
            }
            zeros = current == 0 ? zeros + 1 : 0;
            bits_left = 8;
        }
        return (current >> --bits_left) & 1;
    };
    unsigned prefix = 0;
    while (!bit()) {
        if (++prefix > 31)
            fail("AVC first macroblock overflow");
    }
    std::uint32_t value = 1;
    for (unsigned i = 0; i < prefix; ++i)
        value = (value << 1) | bit();
    return value - 1;
}
void push(Samples &out, const TrackDescription &t, std::span<const std::uint8_t> bytes,
          const std::vector<Mp4Box> &boxes, std::uint64_t at, std::uint32_t size, std::uint64_t dts, std::int64_t cto,
          std::uint32_t duration, bool sync, bool strip_matching_avc_config = false) {
    if (out.size() >= kMaxSamples || !size || !duration)
        fail("sample resource limit or missing size/duration");
    if (size > kMaxSampleBytes - out.byte_count)
        fail("sample byte resource limit");
    out.byte_count += size;
    const auto end = add(at, size);
    bool inside = false;
    for (const auto &b : boxes)
        if (b.type == "mdat" && at >= b.payload.offset && end <= add(b.payload.offset, b.payload.size))
            inside = true;
    if (!inside || end > bytes.size())
        fail("sample outside mdat");
    auto data = bytes.subspan(static_cast<std::size_t>(at), size);
    std::vector<std::uint8_t> normalized;
    if (t.handler_type == "vide") {
        const auto config = loc_codec_config(t);
        const std::size_t width = (config[4] & 3) + 1;
        std::vector<std::span<const std::uint8_t>> parameter_sets;
        if (strip_matching_avc_config) {
            Reader sets{config};
            sets.skip(5);
            const auto sps_count = sets.get(1) & 31;
            auto read_sets = [&](std::uint64_t count) {
                for (std::uint64_t i = 0; i < count; ++i) {
                    const auto length = sets.get(2);
                    const auto start = sets.pos;
                    sets.skip(static_cast<std::size_t>(length));
                    parameter_sets.push_back(sets.b.subspan(start, static_cast<std::size_t>(length)));
                }
            };
            read_sets(sps_count);
            read_sets(sets.get(1));
            normalized.reserve(data.size());
        }
        Reader nal{data};
        bool idr = false, vcl = false, aud = false, picture_start = false;
        while (nal.pos < nal.b.size()) {
            const auto nal_start = nal.pos;
            const auto length = nal.get(width);
            if (!length || length > nal.b.size() - nal.pos)
                fail("invalid AVC sample NAL length");
            const auto type = nal.b[nal.pos] & 31;
            if (type == 7 || type == 8) {
                if (!strip_matching_avc_config)
                    fail("in-band AVC configuration unsupported");
                const auto candidate = nal.b.subspan(nal.pos, static_cast<std::size_t>(length));
                const bool matches = std::any_of(parameter_sets.begin(), parameter_sets.end(), [&](auto known) {
                    return std::equal(candidate.begin(), candidate.end(), known.begin(), known.end());
                });
                if (!matches)
                    fail("in-band AVC configuration differs from avcC");
                nal.skip(static_cast<std::size_t>(length));
                continue;
            }
            if (type == 13 || type == 15)
                fail("in-band AVC configuration unsupported");
            if (type == 14 || type == 20 || type == 21)
                fail("layered AVC samples unsupported");
            if (nal.b[nal.pos] & 128)
                fail("invalid AVC NAL header");
            if (type == 9) {
                if (aud || vcl)
                    fail("multiple AVC access units in one sample");
                aud = true;
            }
            if (type >= 2 && type <= 4)
                fail("partitioned AVC slices unsupported");
            if (type == 1 || type == 5) {
                const auto first_mb =
                    first_macroblock(nal.b.subspan(nal.pos + 1, static_cast<std::size_t>(length - 1)));
                if (first_mb == 0) {
                    if (picture_start)
                        fail("multiple AVC pictures in one sample");
                    picture_start = true;
                }
                vcl = true;
                if (type == 5)
                    idr = true;
            }
            nal.skip(static_cast<std::size_t>(length));
            if (strip_matching_avc_config) {
                const auto original_nal = data.subspan(nal_start, nal.pos - nal_start);
                normalized.insert(normalized.end(), original_nal.begin(), original_nal.end());
            }
        }
        if (strip_matching_avc_config)
            data = normalized;
        if (!vcl)
            fail("AVC sample has no coded picture");
        sync = sync && idr;
    }
    out.push_back({t.track_name, t.timescale, dts, pts(dts, cto), duration, sync, {data.begin(), data.end()}});
}
void fragment(Samples &out, const Mp4Box &moof, const std::vector<Mp4Box> &boxes, std::span<const std::uint8_t> bytes,
              std::span<const TrackDescription> tracks, bool live, bool strip_matching_avc_config = false) {
    std::uint64_t inherited_base = moof.span.offset;
    for (const auto &traf : moof.children) {
        if (traf.type != "traf")
            continue;
        for (const auto &b : traf.children)
            if (b.type == "senc" || b.type == "saiz" || b.type == "saio" || b.type == "sbgp" || b.type == "sgpd")
                fail("encrypted or grouped sample layout unsupported");
        auto h = reader(child(traf, "tfhd"), bytes);
        if (h.get(1) != 0)
            fail("unsupported tfhd version");
        const auto flags = h.get(3);
        const auto id = h.get(4);
        if (flags & ~0x03003bULL)
            fail("unsupported tfhd flags");
        const auto it = std::find_if(tracks.begin(), tracks.end(), [id](const auto &t) { return t.track_id == id; });
        if (it == tracks.end())
            fail("unknown fragment track");
        const auto &t = *it;
        clear_track(t);
        auto defaults = t.fragment_defaults.value_or(TrackFragmentDefaults{});
        auto base = (flags & 0x020000) ? moof.span.offset : inherited_base;
        if (flags & 1) {
            if (live)
                fail("absolute live data offset unsupported");
            base = h.get(8);
        }
        if ((flags & 2) && h.get(4) != 1)
            fail("sample description changes unsupported");
        if (flags & 8)
            defaults.sample_duration = h.get(4);
        if (flags & 16)
            defaults.sample_size = h.get(4);
        if (flags & 32)
            defaults.sample_flags = h.get(4);
        h.end();
        auto time = reader(child(traf, "tfdt"), bytes);
        const auto version = time.get(1);
        if (version > 1 || time.get(3))
            fail("invalid tfdt");
        auto dts = time.get(version == 1 ? 8 : 4);
        time.end();
        std::uint64_t cursor = base;
        bool have_run = false;
        for (const auto &run : traf.children) {
            if (run.type != "trun")
                continue;
            have_run = true;
            auto r = reader(run, bytes);
            const auto v = r.get(1);
            const auto f = r.get(3);
            if (v > 1 || (f & ~0x000f05ULL) || ((f & 4) && (f & 0x400)))
                fail("unsupported trun flags/version");
            const auto count = r.get(4);
            if (count > kMaxSamples - out.size())
                fail("sample count limit");
            if ((flags & 0x010000) && count)
                fail("samples in duration-empty fragment");
            if (f & 1)
                cursor = offset(base, std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(r.get(4))));
            const auto first_flags = (f & 4) ? r.get(4) : defaults.sample_flags;
            for (std::uint64_t i = 0; i < count; ++i) {
                const auto duration = static_cast<std::uint32_t>((f & 0x100) ? r.get(4) : defaults.sample_duration);
                const auto size = static_cast<std::uint32_t>((f & 0x200) ? r.get(4) : defaults.sample_size);
                const auto sf = (f & 0x400) ? r.get(4) : (i == 0 ? first_flags : defaults.sample_flags);
                std::int64_t cto = 0;
                if (f & 0x800) {
                    auto raw = static_cast<std::uint32_t>(r.get(4));
                    cto = v == 1 ? std::int64_t(std::bit_cast<std::int32_t>(raw)) : std::int64_t(raw);
                }
                push(out, t, bytes, boxes, cursor, size, dts, cto, duration,
                     t.handler_type == "soun" || (!(sf & 0x10000) && ((sf >> 24) & 3) != 1), strip_matching_avc_config);
                cursor = add(cursor, size);
                dts = add(dts, duration);
            }
            r.end();
        }
        if (!have_run)
            fail("missing trun");
        inherited_base = cursor;
    }
}
void validate_init(const ParsedMp4 &p, bool fragmented_only) {
    for (const auto &t : p.tracks) {
        clear_track(t);
        (void)loc_codec_config(t);
    }
    const auto *moov = find_first_box(p.top_level_boxes, "moov");
    if (!moov)
        fail("missing moov");
    if (const auto *mvex = find_child_box(*moov, "mvex")) {
        for (const auto &trex : mvex->children)
            if (trex.type == "trex") {
                auto r = reader(trex, p.bytes);
                if (r.get(4))
                    fail("trex version/flags");
                r.get(4);
                if (r.get(4) != 1)
                    fail("trex sample description unsupported");
                r.skip(12);
                r.end();
            }
    }
    for (const auto &trak : moov->children)
        if (trak.type == "trak") {
            if (const auto *edts = find_child_box(trak, "edts")) {
                auto r = reader(child(*edts, "elst"), p.bytes);
                const auto v = r.get(1);
                if (v > 1 || r.get(3) || r.get(4) != 1)
                    fail("unsupported edit list");
                const auto edit_duration = r.get(v == 1 ? 8 : 4);
                if (r.get(v == 1 ? 8 : 4) != 0 || r.get(4) != 0x10000)
                    fail("unsupported edit list: media time/rate");
                r.end();
                auto movie = reader(child(*moov, "mvhd"), p.bytes);
                const auto movie_version = movie.get(1);
                if (movie_version > 1)
                    fail("mvhd version");
                movie.skip(3 + (movie_version ? 16 : 8));
                const auto movie_scale = movie.get(4);
                auto media = reader(child(child(trak, "mdia"), "mdhd"), p.bytes);
                const auto media_version = media.get(1);
                if (media_version > 1)
                    fail("mdhd version");
                media.skip(3 + (media_version ? 16 : 8));
                const auto media_scale = media.get(4);
                const auto media_duration = media.get(media_version ? 8 : 4);
                if (!movie_scale || !media_scale || edit_duration > UINT64_MAX / media_scale ||
                    media_duration > UINT64_MAX / movie_scale ||
                    edit_duration * media_scale != media_duration * movie_scale)
                    fail("unsupported edit list: duration trims media");
            }
            const auto &stbl = child(child(child(trak, "mdia"), "minf"), "stbl");
            if (fragmented_only || find_first_box(p.top_level_boxes, "moof")) {
                if (find_child_box(stbl, "stz2"))
                    fail("compact sample-size tables unsupported with fragments");
                auto sizes = reader(child(stbl, "stsz"), p.bytes);
                if (sizes.get(4))
                    fail("stsz version/flags");
                sizes.get(4);
                if (sizes.get(4) != 0)
                    fail("mixed progressive and fragmented samples unsupported");
                sizes.end();
            }
            for (const auto &b : stbl.children)
                if (b.type == "senc" || b.type == "saiz" || b.type == "saio" || b.type == "sbgp" || b.type == "sgpd")
                    fail("encrypted or grouped sample layout unsupported");
            auto sd = reader(child(stbl, "stsd"), p.bytes);
            if (sd.get(4) || sd.get(4) != 1)
                fail("multiple sample descriptions unsupported");
        }
}
void progressive(Samples &out, const ParsedMp4 &p) {
    const auto &moov = *find_first_box(p.top_level_boxes, "moov");
    for (const auto &trak : moov.children) {
        if (trak.type != "trak")
            continue;
        auto tk = reader(child(trak, "tkhd"), p.bytes);
        const auto v = tk.get(1);
        if (v > 1)
            fail("tkhd version");
        tk.skip(3 + (v ? 16 : 8));
        const auto id = tk.get(4);
        auto it = std::find_if(p.tracks.begin(), p.tracks.end(), [id](const auto &t) { return t.track_id == id; });
        if (it == p.tracks.end())
            fail("unknown track");
        const auto &t = *it;
        const auto &stbl = child(child(child(trak, "mdia"), "minf"), "stbl");
        auto sz = reader(child(stbl, "stsz"), p.bytes);
        if (sz.get(4))
            fail("stsz version/flags");
        const auto fixed = sz.get(4), n = sz.get(4);
        if (n > kMaxSamples - out.size())
            fail("sample count limit");
        std::vector<std::uint32_t> sizes;
        sizes.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            sizes.push_back(fixed ? fixed : sz.get(4));
        sz.end();
        std::vector<std::uint32_t> durations;
        std::vector<std::int64_t> ctos(n, 0);
        auto timing = [&](const Mp4Box &box, bool composition) {
            auto r = reader(box, p.bytes);
            const auto ver = r.get(1);
            if (ver > (composition ? 1U : 0U) || r.get(3))
                fail("timing table version/flags");
            const auto count = r.get(4);
            std::size_t index = 0;
            for (std::uint64_t j = 0; j < count; ++j) {
                const auto run = r.get(4);
                const auto raw = static_cast<std::uint32_t>(r.get(4));
                if (!run || run > n - index)
                    fail("timing table count mismatch");
                for (std::size_t k = 0; k < run; ++k, ++index) {
                    if (composition)
                        ctos[index] = ver ? std::int64_t(std::bit_cast<std::int32_t>(raw)) : std::int64_t(raw);
                    else
                        durations.push_back(raw);
                }
            }
            if (index != n)
                fail("timing table count mismatch");
            r.end();
        };
        timing(child(stbl, "stts"), false);
        if (const auto *ct = find_child_box(stbl, "ctts"))
            timing(*ct, true);
        std::vector<bool> sync(n, true);
        if (const auto *ss = find_child_box(stbl, "stss")) {
            std::fill(sync.begin(), sync.end(), false);
            auto r = reader(*ss, p.bytes);
            if (r.get(4))
                fail("stss flags");
            const auto count = r.get(4);
            std::uint64_t prev = 0;
            for (std::uint64_t j = 0; j < count; ++j) {
                const auto index = r.get(4);
                if (index <= prev || index > n)
                    fail("invalid sync sample index");
                sync[index - 1] = true;
                prev = index;
            }
            r.end();
        }
        const auto *co = find_child_box(stbl, "stco");
        bool wide = false;
        if (!co) {
            co = find_child_box(stbl, "co64");
            wide = true;
        }
        if (!co)
            fail("missing chunk offsets");
        auto cr = reader(*co, p.bytes);
        if (cr.get(4))
            fail("chunk offset flags");
        const auto chunks = cr.get(4);
        auto sc = reader(child(stbl, "stsc"), p.bytes);
        if (sc.get(4))
            fail("stsc flags");
        const auto entries = sc.get(4);
        struct Mapping {
            std::uint64_t first, count;
        };
        std::vector<Mapping> map;
        for (std::uint64_t j = 0; j < entries; ++j) {
            const auto first = sc.get(4), count = sc.get(4), desc = sc.get(4);
            if (!first || first > chunks || !count || desc != 1 ||
                (map.empty() ? first != 1 : first <= map.back().first))
                fail("invalid chunk mapping");
            map.push_back({first, count});
        }
        sc.end();
        if (chunks && map.empty())
            fail("missing chunk mapping");
        std::size_t index = 0, mapping = 0;
        std::uint64_t dts = 0;
        for (std::uint64_t c = 1; c <= chunks; ++c) {
            auto at = cr.get(wide ? 8 : 4);
            while (mapping + 1 < map.size() && map[mapping + 1].first <= c)
                ++mapping;
            if (map[mapping].count > n - index)
                fail("chunk sample count mismatch");
            for (std::uint64_t j = 0; j < map[mapping].count; ++j, ++index) {
                push(out, t, p.bytes, p.top_level_boxes, at, sizes[index], dts, ctos[index], durations[index],
                     sync[index]);
                at = add(at, sizes[index]);
                dts = add(dts, durations[index]);
            }
        }
        cr.end();
        if (index != n)
            fail("chunk sample count mismatch");
    }
}
// MPEG-4 descriptors use a variable length field independent of MP4 box
// lengths.
std::span<const std::uint8_t> descriptor(Reader &r, unsigned tag) {
    if (r.get(1) != tag)
        fail("unexpected AAC descriptor");
    std::size_t size = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const auto v = r.get(1);
        size = (size << 7) | (v & 127);
        if (!(v & 128)) {
            const auto start = r.pos;
            r.skip(size);
            return r.b.subspan(start, size);
        }
    }
    fail("invalid AAC descriptor length");
}
} // namespace
std::vector<std::uint8_t> loc_codec_config(const TrackDescription &t) {
    clear_track(t);
    const auto boxes = parse_mp4_boxes(t.codec_private);
    if (boxes.size() != 1 || boxes[0].span.size != t.codec_private.size())
        fail("invalid codec configuration box");
    auto r = reader(boxes[0], t.codec_private);
    if (t.sample_entry_type == "avc1") {
        if (boxes[0].type != "avcC" || r.b.size() < 7 || r.b[0] != 1 || (r.b[4] & 3) == 2)
            fail("invalid AVC configuration");
        r.skip(5);
        const auto sps_count = r.get(1) & 31;
        auto parameter_sets = [&](std::uint64_t count, unsigned type) {
            if (!count)
                fail("missing AVC parameter sets");
            for (std::uint64_t i = 0; i < count; ++i) {
                const auto size = r.get(2);
                if (!size || size > r.b.size() - r.pos || (r.b[r.pos] & 31) != type)
                    fail("invalid AVC parameter set");
                r.skip(static_cast<std::size_t>(size));
            }
        };
        parameter_sets(sps_count, 7);
        parameter_sets(r.get(1), 8);
        // High profile records may append chroma/bit-depth and SPS extensions.
        if (r.pos < r.b.size()) {
            const auto profile = r.b[1];
            if (profile != 100 && profile != 110 && profile != 122 && profile != 144)
                fail("unexpected AVC configuration extension");
            r.skip(3);
            const auto count = r.get(1);
            if (count)
                parameter_sets(count, 13);
        }
        r.end();
        return {r.b.begin(), r.b.end()};
    }
    if (boxes[0].type != "esds" || r.get(4))
        fail("invalid AAC configuration");
    Reader es{descriptor(r, 3)};
    es.skip(2);
    const auto flags = es.get(1);
    if (flags & 128)
        es.skip(2);
    if (flags & 64) {
        auto n = es.get(1);
        es.skip(n);
    }
    if (flags & 32)
        es.skip(2);
    Reader dec{descriptor(es, 4)};
    if (dec.get(1) != 0x40 || (dec.get(1) >> 2) != 5)
        fail("unsupported AAC object/stream type");
    dec.skip(11);
    const auto config = descriptor(dec, 5);
    if (config.size() < 2 || (config[0] >> 3) != 2 || (((config[0] & 7) << 1) | (config[1] >> 7)) >= 13 ||
        ((config[1] >> 3) & 15) == 0 || ((config[1] >> 3) & 15) > 7)
        fail("only AAC-LC configuration supported");
    return {config.begin(), config.end()};
}
void validate_loc_init(const ParsedMp4 &p, bool fragmented_only) { validate_init(p, fragmented_only); }

std::vector<EncodedSample> read_encoded_samples(const ParsedMp4 &p) {
    validate_loc_init(p);
    Samples out;
    bool fragmented = false;
    for (const auto &b : p.top_level_boxes)
        if (b.type == "moof") {
            fragmented = true;
            fragment(out, b, p.top_level_boxes, p.bytes, p.tracks, false);
        }
    if (!fragmented)
        progressive(out, p);
    return std::move(out.values);
}
std::vector<EncodedSample> read_fragment_samples(std::span<const std::uint8_t> moof, std::span<const std::uint8_t> mdat,
                                                 std::span<const TrackDescription> tracks,
                                                 bool strip_matching_avc_config) {
    if (moof.size() > kMaxSampleBytes || mdat.size() > kMaxSampleBytes - moof.size())
        fail("live fragment resource limit");
    std::vector<std::uint8_t> bytes(moof.begin(), moof.end());
    bytes.insert(bytes.end(), mdat.begin(), mdat.end());
    const auto boxes = parse_mp4_boxes(bytes);
    if (boxes.size() != 2 || boxes[0].type != "moof" || boxes[0].span.size != moof.size() || boxes[1].type != "mdat")
        fail("expected complete moof/mdat pair");
    Samples out;
    fragment(out, boxes[0], boxes, bytes, tracks, true, strip_matching_avc_config);
    return std::move(out.values);
}
} // namespace openmoq::publisher
