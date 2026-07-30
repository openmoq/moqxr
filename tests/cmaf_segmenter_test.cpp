#include "openmoq/publisher/cmaf_segmenter.h"
#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/mp4_box.h"
#include "openmoq/publisher/publisher_api.h"

#include <cctype>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <array>
#include <sstream>
#include <string>
#include <vector>

namespace {

void append_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void append_be64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void append_ascii(std::vector<std::uint8_t>& out, const std::string& value) {
    out.insert(out.end(), value.begin(), value.end());
}

void patch_be32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value) {
    out[offset] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    out[offset + 2] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    out[offset + 3] = static_cast<std::uint8_t>(value & 0xFFU);
}

std::vector<std::uint8_t> make_box(const std::string& type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    append_be32(out, static_cast<std::uint32_t>(8 + payload.size()));
    append_ascii(out, type);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// ISO 14496-12 BitRateBox: bufferSizeDB, maxBitrate, avgBitrate.
std::vector<std::uint8_t> make_btrt(std::uint32_t buffer_size_db,
                                    std::uint32_t max_bitrate,
                                    std::uint32_t avg_bitrate) {
    std::vector<std::uint8_t> payload;
    append_be32(payload, buffer_size_db);
    append_be32(payload, max_bitrate);
    append_be32(payload, avg_bitrate);
    return make_box("btrt", payload);
}

// Packs an ISO-639-2/T language code the same way mdhd does: three 5-bit
// values (each letter minus 0x60) packed into the low 15 bits.
std::uint16_t pack_mdhd_language(const std::string& code) {
    std::uint16_t packed = 0;
    for (const char c : code) {
        packed = static_cast<std::uint16_t>((packed << 5U) | (static_cast<std::uint8_t>(c) - 0x60U));
    }
    return packed;
}

std::vector<std::uint8_t> be32_bytes(std::uint32_t value) {
    return {
        static_cast<std::uint8_t>((value >> 24U) & 0xFFU),
        static_cast<std::uint8_t>((value >> 16U) & 0xFFU),
        static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(value & 0xFFU),
    };
}

std::vector<std::uint8_t> make_full_box(const std::string& type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out(4, 0);
    out.insert(out.end(), payload.begin(), payload.end());
    return make_box(type, out);
}

std::vector<std::uint8_t> make_full_box_with_flags(const std::string& type,
                                                   std::uint8_t version,
                                                   std::uint32_t flags,
                                                   const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    out.push_back(version);
    out.push_back(static_cast<std::uint8_t>((flags >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((flags >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(flags & 0xFFU));
    out.insert(out.end(), payload.begin(), payload.end());
    return make_box(type, out);
}

std::vector<std::uint8_t> concat(std::initializer_list<std::vector<std::uint8_t>> boxes) {
    std::vector<std::uint8_t> out;
    for (const auto& box : boxes) {
        out.insert(out.end(), box.begin(), box.end());
    }
    return out;
}

std::uint32_t read_be32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint64_t read_be64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8U) | bytes[offset + index];
    }
    return value;
}

// Shared byte-layout constants for the live-fragment saio-correction
// fixtures below. Each is derived by hand from the box shapes
// make_saio_test_fragment builds, independent of materialize_live_trun_defaults
// (the code under test), so tests can assert exact numbers rather than
// merely "changed".
constexpr std::size_t kSaioFixtureTfhdSize = 28;         // header8 + ver/flags4 + track_id4+dur4+size4+flags4
constexpr std::size_t kSaioFixtureOriginalTrunSize = 16;  // header8 + ver/flags4 + sample_count4 (flags=0)
constexpr std::size_t kSaioFixtureNormalizedTrunSize = 32;  // header8 + ver/flags4 + count4 + data_offset4 + 12/sample
constexpr std::int64_t kSaioFixtureTrunGrowthDelta =
    static_cast<std::int64_t>(kSaioFixtureNormalizedTrunSize) -
    static_cast<std::int64_t>(kSaioFixtureOriginalTrunSize);
constexpr std::size_t kSaioFixtureSencSize = 16;   // header8 + 8 arbitrary payload bytes
constexpr std::size_t kSaioFixtureSaizSize = 17;   // header8 + ver/flags4 + default_size1 + sample_count4

// Where the ORIGINAL (pre-normalization) trun box ends, relative to the
// moof, in the default {tfhd, trun, senc, saiz, saio} traf child order
// make_saio_test_fragment builds. correct_saio_offsets only shifts an offset
// at or beyond this point; an offset before it (inside tfhd or trun itself)
// must not move, since the rebuild changes only trun's own size.
constexpr std::size_t kSaioFixtureTrunEnd = 8 /* moof header */ + 8 /* traf header */ +
                                           kSaioFixtureTfhdSize + kSaioFixtureOriginalTrunSize;

// Builds a `saio` (Sample Auxiliary Information Offsets) box, ISO/IEC
// 14496-12 section 8.7.13, with the given version and offset entries.
// `has_aux_info_type` controls whether flags & 1 is set and the
// aux_info_type/aux_info_type_parameter prefix is written.
std::vector<std::uint8_t> make_saio_box(std::uint8_t version,
                                        bool has_aux_info_type,
                                        const std::vector<std::uint64_t>& offsets) {
    std::vector<std::uint8_t> payload;
    if (has_aux_info_type) {
        append_be32(payload, 0x63656E63U);  // aux_info_type = 'cenc'
        append_be32(payload, 0);            // aux_info_type_parameter
    }
    append_be32(payload, static_cast<std::uint32_t>(offsets.size()));
    for (const std::uint64_t offset : offsets) {
        if (version == 1) {
            append_be64(payload, offset);
        } else {
            append_be32(payload, static_cast<std::uint32_t>(offset));
        }
    }
    const std::uint32_t flags = has_aux_info_type ? 0x000001U : 0U;
    return make_full_box_with_flags("saio", version, flags, payload);
}

// Builds a `saio` box whose entry_count field is a caller-chosen, possibly
// fabricated value, independent of how many offsets actually follow it. Used
// to simulate an attacker-controlled entry_count that claims far more
// entries than the box has room for.
std::vector<std::uint8_t> make_saio_box_with_declared_entry_count(std::uint8_t version,
                                                                   bool has_aux_info_type,
                                                                   std::uint32_t declared_entry_count,
                                                                   const std::vector<std::uint64_t>& actual_offsets) {
    std::vector<std::uint8_t> payload;
    if (has_aux_info_type) {
        append_be32(payload, 0x63656E63U);  // aux_info_type = 'cenc'
        append_be32(payload, 0);            // aux_info_type_parameter
    }
    append_be32(payload, declared_entry_count);
    for (const std::uint64_t offset : actual_offsets) {
        if (version == 1) {
            append_be64(payload, offset);
        } else {
            append_be32(payload, static_cast<std::uint32_t>(offset));
        }
    }
    const std::uint32_t flags = has_aux_info_type ? 0x000001U : 0U;
    return make_full_box_with_flags("saio", version, flags, payload);
}

// A minimal live (CTE/DASH) moof+mdat fragment: one track, one traf, a
// 1-sample minimal trun (the shape FFmpeg's DASH muxer emits, which
// materialize_live_trun_defaults normalizes), optionally senc/saiz, and an
// optional saio built by the caller. Sizes below are fixed and documented so
// tests can compute the exact trun-growth delta independently of the code
// under test.
struct SaioTestFragment {
    std::vector<std::uint8_t> moof;
    std::vector<std::uint8_t> mdat;
    std::vector<std::uint8_t> tfhd;  // the exact tfhd bytes placed in moof, for byte-exact comparisons
    std::size_t original_moof_size = 0;
    std::int64_t expected_delta = 0;
};

SaioTestFragment make_saio_test_fragment(const std::vector<std::uint8_t>& saio_box,
                                         std::size_t mdat_payload_size,
                                         bool include_senc = true,
                                         bool include_saiz = true,
                                         bool senc_before_trun = false) {
    // tfhd: FullBox header(4) + track_id(4) + default_duration(4) +
    // default_size(4) + default_flags(4) = 16 bytes payload -> 28 byte box.
    std::vector<std::uint8_t> tfhd_payload;
    append_be32(tfhd_payload, 1);             // track_id
    append_be32(tfhd_payload, 1000);          // default_sample_duration
    append_be32(tfhd_payload, 200);           // default_sample_size
    append_be32(tfhd_payload, 0x02000000U);   // default_sample_flags
    const auto tfhd = make_full_box_with_flags(
        "tfhd", 0, 0x000038U /* default-duration|default-size|default-flags */, tfhd_payload);

    // Minimal live trun: flags = 0, so only sample_count is present.
    // Box size: header(8) + version/flags(4) + sample_count(4) = 16 bytes.
    const auto trun = make_full_box_with_flags("trun", 0, 0, be32_bytes(1));

    std::vector<std::uint8_t> senc;
    if (include_senc) {
        senc = make_box("senc", {0, 0, 0, 0, 0, 0, 0, 0});
    }
    std::vector<std::uint8_t> saiz;
    if (include_saiz) {
        saiz = make_full_box("saiz", {8, 0, 0, 0, 1});
    }

    // senc_before_trun exercises the legal-but-less-common ISO/IEC 14496-12
    // box ordering ffmpeg produces: senc ahead of trun in the traf. Nothing
    // besides trun's position changes size, so original_moof_size and
    // expected_delta below stay valid for either ordering.
    const auto traf = senc_before_trun ? make_box("traf", concat({tfhd, senc, trun, saiz, saio_box}))
                                       : make_box("traf", concat({tfhd, trun, senc, saiz, saio_box}));
    const auto moof = make_box("moof", traf);
    const auto mdat = make_box("mdat", std::vector<std::uint8_t>(mdat_payload_size, 0xAB));

    // Nothing in traf besides trun changes size during normalization, so the
    // trun growth (see kSaioFixtureTrunGrowthDelta above) is the entire
    // moof-size delta.
    return SaioTestFragment{
        .moof = moof,
        .mdat = mdat,
        .tfhd = tfhd,
        .original_moof_size = moof.size(),
        .expected_delta = kSaioFixtureTrunGrowthDelta,
    };
}

// Computes a saio box's exact size from its shape alone (version, whether
// the aux_info_type prefix is present, and entry count) without building it,
// so tests can pick offsets relative to a moof size known in advance.
std::size_t saio_box_size(std::uint8_t version, bool has_aux_info_type, std::size_t entry_count) {
    const std::size_t entry_width = version == 1 ? 8 : 4;
    return 8 /* header */ + 4 /* version/flags */ + (has_aux_info_type ? 8 : 0) + 4 /* entry_count */ +
          entry_count * entry_width;
}

// Reads back the corrected offsets from a saio box in already-parsed output
// bytes, independent of materialize_live_trun_defaults.
std::vector<std::uint64_t> extract_saio_offsets(const std::vector<std::uint8_t>& bytes,
                                                const openmoq::publisher::Mp4Box& saio,
                                                bool has_aux_info_type) {
    const std::uint8_t version = bytes[saio.payload.offset];
    std::size_t cursor = saio.payload.offset + 4;
    if (has_aux_info_type) {
        cursor += 8;
    }
    const std::uint32_t count = read_be32(bytes, cursor);
    cursor += 4;
    std::vector<std::uint64_t> offsets;
    offsets.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        if (version == 1) {
            offsets.push_back(read_be64(bytes, cursor));
            cursor += 8;
        } else {
            offsets.push_back(read_be32(bytes, cursor));
            cursor += 4;
        }
    }
    return offsets;
}

std::vector<std::uint8_t> make_fragmented_test_mp4() {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = make_full_box("tkhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto mdhd = make_full_box("mdhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x5d, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0});
    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    auto visual_header = std::vector<std::uint8_t>(70, 0);
    visual_header[24] = 0x01;
    visual_header[25] = 0x40;
    visual_header[26] = 0x00;
    visual_header[27] = 0xf0;
    const auto sample_entry = make_box("avc1", concat({visual_header, make_box("avcC", {1, 100, 0, 12, 0xff})}));
    const auto stsd = make_full_box("stsd", concat({std::vector<std::uint8_t>{0, 0, 0, 1}, sample_entry}));
    const auto stbl = make_box("stbl", stsd);
    const auto minf = make_box("minf", stbl);
    const auto mdia = make_box("mdia", concat({mdhd, hdlr, minf}));
    const auto trak = make_box("trak", concat({tkhd, mdia}));
    const auto moov = make_box("moov", trak);
    const auto moof = make_box("moof", {'m', 'f', 'h', 'd'});
    const auto mdat = make_box("mdat", {1, 2, 3, 4, 5, 6});
    return concat({ftyp, moov, moof, mdat});
}

// CMSF section 4: an encv sample entry must still yield the pre-encryption
// codec string. frma("avc1"), schm("cenc"), and schi/tenc build the sinf
// Task 2's parse_track_protection expects; avcC sits alongside sinf (not
// inside it) so the codec string still needs the avcC profile bytes read
// through the effective sample entry, not just a bare "avc1".
std::vector<std::uint8_t> make_frma(const std::string& original_format) {
    return make_box("frma", std::vector<std::uint8_t>(original_format.begin(), original_format.end()));
}

std::vector<std::uint8_t> make_schm(const std::string& scheme_type) {
    std::vector<std::uint8_t> payload(scheme_type.begin(), scheme_type.end());
    append_be32(payload, 0x00010000);
    return make_full_box("schm", payload);
}

std::vector<std::uint8_t> make_tenc_box(std::uint8_t is_protected, std::uint8_t iv_size) {
    std::vector<std::uint8_t> payload{0, 0, is_protected, iv_size};
    const std::vector<std::uint8_t> kid{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    payload.insert(payload.end(), kid.begin(), kid.end());
    return make_full_box("tenc", payload);
}

std::vector<std::uint8_t> make_sinf(const std::string& original_format, const std::string& scheme_type) {
    const auto schi = make_box("schi", make_tenc_box(1, 8));
    return make_box("sinf", concat({make_frma(original_format), make_schm(scheme_type), schi}));
}

// A pssh box: FullBox header, then a 16-byte SystemID and a trailing 4-byte
// data-size/data pair, matching the layout parse_pssh_boxes (cenc.cpp) reads
// (SystemID at header+version/flags = offset 12).
std::vector<std::uint8_t> make_pssh(const std::vector<std::uint8_t>& system_id) {
    std::vector<std::uint8_t> payload(system_id.begin(), system_id.end());
    append_be32(payload, 4);
    payload.insert(payload.end(), {0xde, 0xad, 0xbe, 0xef});
    return make_full_box("pssh", payload);
}

// The Widevine common system ID, edef8ba9-79d6-4ace-a3c8-27dcd51d21ed, as raw
// bytes -- the same system ID used in msf_catalog_test.cpp.
std::vector<std::uint8_t> widevine_system_id() {
    return {0xed, 0xef, 0x8b, 0xa9, 0x79, 0xd6, 0x4a, 0xce,
            0xa3, 0xc8, 0x27, 0xdc, 0xd5, 0x1d, 0x21, 0xed};
}

std::vector<std::uint8_t> make_encrypted_fragmented_test_mp4(bool include_pssh = true) {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = make_full_box("tkhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto mdhd = make_full_box("mdhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x5d, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0});
    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    auto visual_header = std::vector<std::uint8_t>(70, 0);
    visual_header[24] = 0x01;
    visual_header[25] = 0x40;
    visual_header[26] = 0x00;
    visual_header[27] = 0xf0;
    const auto avcc = make_box("avcC", {1, 100, 0, 12, 0xff});
    const auto sinf = make_sinf("avc1", "cenc");
    const auto sample_entry = make_box("encv", concat({visual_header, avcc, sinf}));
    const auto stsd = make_full_box("stsd", concat({std::vector<std::uint8_t>{0, 0, 0, 1}, sample_entry}));
    const auto stbl = make_box("stbl", stsd);
    const auto minf = make_box("minf", stbl);
    const auto mdia = make_box("mdia", concat({mdhd, hdlr, minf}));
    const auto trak = make_box("trak", concat({tkhd, mdia}));
    // pssh boxes live directly under moov, as siblings of trak (ISO/IEC
    // 23001-7) -- this is the DRM system build_publish_plan's
    // collect_pssh_systems must find to emit a CMSF contentProtections
    // entry. include_pssh=false models ffmpeg's `-encryption_scheme
    // cenc-aes-ctr`, which writes sinf/tenc/senc/saiz/saio with no pssh
    // anywhere (pssh is only SHOULD-present per CMSF 4.1.1.4.5) -- the C2
    // regression fixture.
    const auto moov = include_pssh ? make_box("moov", concat({trak, make_pssh(widevine_system_id())}))
                                   : make_box("moov", trak);
    const auto moof = make_box("moof", {'m', 'f', 'h', 'd'});
    const auto mdat = make_box("mdat", {1, 2, 3, 4, 5, 6});
    return concat({ftyp, moov, moof, mdat});
}

// CMSF section 4, audio side of C1: an enca sample entry is byte-for-byte an
// AudioSampleEntry, so channel_count/sample_rate must be read through it the
// same way an unencrypted mp4a entry would be -- gating that extraction on
// the raw "enca" type (instead of the frma-resolved effective type) would
// silently publish samplerate/channelConfig as 0, which validate_track makes
// MUST-present, so that is a wrong value on the wire, not merely a missing
// one. channelcount=2 and samplerate=44100 (0xAC44) are encoded at the
// AudioSampleEntry's fixed offsets 16 and 24, matching the layout
// make_track_metadata_test_mp4's audio_header uses elsewhere in this file.
std::vector<std::uint8_t> make_encrypted_audio_fragmented_test_mp4() {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = make_full_box("tkhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto mdhd = make_full_box("mdhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x5d, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0});
    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 's', 'o', 'u', 'n', 0, 0, 0, 0});
    auto audio_header = std::vector<std::uint8_t>(28, 0);
    audio_header[16] = 0x00;
    audio_header[17] = 0x02;    // channelcount = 2
    audio_header[24] = 0xac;
    audio_header[25] = 0x44;    // samplerate = 44100 << 16, top bytes 0xAC44
    const auto esds = make_full_box(
        "esds", {0x00, 0x00, 0x00, 0x00, 0x03, 0x19, 0x00, 0x02, 0x00, 0x04, 0x11, 0x40, 0x15,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x02, 0x10, 0x10});
    const auto sinf = make_sinf("mp4a", "cenc");
    const auto sample_entry = make_box("enca", concat({audio_header, esds, sinf}));
    const auto stsd = make_full_box("stsd", concat({std::vector<std::uint8_t>{0, 0, 0, 1}, sample_entry}));
    const auto stbl = make_box("stbl", stsd);
    const auto minf = make_box("minf", stbl);
    const auto mdia = make_box("mdia", concat({mdhd, hdlr, minf}));
    const auto trak = make_box("trak", concat({tkhd, mdia}));
    const auto pssh = make_pssh(widevine_system_id());
    const auto moov = make_box("moov", concat({trak, pssh}));
    const auto moof = make_box("moof", {'m', 'f', 'h', 'd'});
    const auto mdat = make_box("mdat", {1, 2, 3, 4, 5, 6});
    return concat({ftyp, moov, moof, mdat});
}

// Regression for a heap over-read, originally in the sliding
// find_child_box_offset scan (mp4_box.cpp), now guarded by
// find_child_box_span: the lookup was bounded only by the stsd
// sample-entry's own declared box size, which extract_tracks reads from the
// file with no validation (read_be32 at the stsd's first child offset). A
// fabricated size far larger than the file drove the scan past the end of
// the buffer once a searched-for child box was absent. avcC is present here
// and found within the first ~80 bytes (as in a real file), so the avcC
// lookup terminates early; btrt is absent, as in most real files, so the
// unconditional per-track bitrate lookup is the one left walking to the
// fabricated bound. Reverting find_child_box_span's clamp makes this fixture
// heap-buffer-overflow under ASAN; with the clamp, extract_tracks returns
// cleanly and reports no bitrate.
std::vector<std::uint8_t> make_oversized_sample_entry_test_mp4() {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = make_full_box("tkhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto mdhd = make_full_box("mdhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x5d, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0});
    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    auto visual_header = std::vector<std::uint8_t>(70, 0);
    visual_header[24] = 0x01;
    visual_header[25] = 0x40;
    visual_header[26] = 0x00;
    visual_header[27] = 0xf0;
    const auto sample_entry = make_box("avc1", concat({visual_header, make_box("avcC", {1, 100, 0, 12, 0xff})}));
    const auto stsd = make_full_box("stsd", concat({std::vector<std::uint8_t>{0, 0, 0, 1}, sample_entry}));
    const auto stbl = make_box("stbl", stsd);
    const auto minf = make_box("minf", stbl);
    const auto mdia = make_box("mdia", concat({mdhd, hdlr, minf}));
    const auto trak = make_box("trak", concat({tkhd, mdia}));
    const auto moov = make_box("moov", trak);

    std::vector<std::uint8_t> file = concat({ftyp, moov});

    // The avc1 sample entry's own box-size field is the first 4 bytes of its
    // box, reached by walking past every box header/sibling that precedes it:
    // ftyp, then the moov header, the trak header, tkhd, the mdia header,
    // mdhd, hdlr, the minf header, the stbl header, and finally the stsd
    // header plus its 4-byte version/flags and 4-byte entry-count fields.
    const std::size_t sample_entry_size_offset =
        ftyp.size() + 8 + 8 + tkhd.size() + 8 + mdhd.size() + hdlr.size() + 8 + 8 + 16;
    // Matches the reviewer's ASAN repro: a small file with a declared
    // sample-entry size of 0x40000000 (1 GiB).
    patch_be32(file, sample_entry_size_offset, 0x40000000);
    return file;
}

// Regression for a heap over-read in mpeg4_audio_codec_string (mp4_box.cpp):
// its ESDS descriptor scan (the byte-by-byte search for tag 0x05, distinct
// from the find_child_box_span lookup that now locates the esds box itself)
// was bounded only by the stsd sample-entry's own declared box size
// (sample_entry.span.offset + sample_entry.span.size), which extract_tracks
// reads from the file with no validation, and it used that raw, unclamped
// sum directly as the scan bound, as the decode_descriptor_length end
// argument, and in the config-offset bounds check. The esds box below
// carries no DecSpecificInfo
// (tag 0x05) byte anywhere in its payload, so the byte-by-byte scan never
// finds a match and runs all the way to its bound; with the mp4a sample
// entry's box-size field patched to a size far larger than the file, that
// bound reaches past the end of the buffer. Reverting the clamp in
// mpeg4_audio_codec_string makes this fixture heap-buffer-overflow under
// ASAN; with the clamp, extract_tracks returns cleanly and falls back to
// the default "mp4a.40.2" codec string.
std::vector<std::uint8_t> make_oversized_audio_sample_entry_test_mp4() {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = make_full_box("tkhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto mdhd = make_full_box("mdhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x5d, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0});
    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 's', 'o', 'u', 'n', 0, 0, 0, 0});
    // Fixed-size AudioSampleEntry fields (reserved/data_reference_index,
    // reserved, channelcount, samplesize, pre_defined, reserved, samplerate):
    // 28 bytes, matching the "8 + 28" child_offset mpeg4_audio_codec_string
    // passes to find_child_box_span when locating the esds box.
    const auto audio_header = std::vector<std::uint8_t>(28, 0);
    // A FullBox esds payload with no byte equal to 0x05 (DecSpecificInfoTag)
    // anywhere, so the scan for it never terminates early.
    const auto esds = make_full_box("esds", std::vector<std::uint8_t>(20, 0));
    const auto sample_entry = make_box("mp4a", concat({audio_header, esds}));
    const auto stsd = make_full_box("stsd", concat({std::vector<std::uint8_t>{0, 0, 0, 1}, sample_entry}));
    const auto stbl = make_box("stbl", stsd);
    const auto minf = make_box("minf", stbl);
    const auto mdia = make_box("mdia", concat({mdhd, hdlr, minf}));
    const auto trak = make_box("trak", concat({tkhd, mdia}));
    const auto moov = make_box("moov", trak);

    std::vector<std::uint8_t> file = concat({ftyp, moov});

    // The mp4a sample entry's own box-size field is the first 4 bytes of its
    // box, reached the same way as the avc1 case above: ftyp, then the moov
    // header, the trak header, tkhd, the mdia header, mdhd, hdlr, the minf
    // header, the stbl header, and finally the stsd header plus its 4-byte
    // version/flags and 4-byte entry-count fields.
    const std::size_t sample_entry_size_offset =
        ftyp.size() + 8 + 8 + tkhd.size() + 8 + mdhd.size() + hdlr.size() + 8 + 8 + 16;
    patch_be32(file, sample_entry_size_offset, 0x40000000);
    return file;
}

// Fragmented single-track MP4 whose mdhd carries the given timescale,
// duration, and packed language (mdhd version 0 or 1, selectable so both
// 32-bit and 64-bit duration layouts are exercised), and whose video sample
// entry optionally carries a btrt box (MSF section 5.2.22 bitrate).
std::vector<std::uint8_t> make_track_metadata_test_mp4(bool include_btrt,
                                                       std::uint16_t packed_language,
                                                       std::uint32_t timescale,
                                                       std::uint64_t duration,
                                                       std::uint8_t mdhd_version = 0) {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = make_full_box("tkhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});

    std::vector<std::uint8_t> mdhd_payload;
    if (mdhd_version == 1) {
        append_be64(mdhd_payload, 0);  // creation_time
        append_be64(mdhd_payload, 0);  // modification_time
        append_be32(mdhd_payload, timescale);
        append_be64(mdhd_payload, duration);
    } else {
        append_be32(mdhd_payload, 0);  // creation_time
        append_be32(mdhd_payload, 0);  // modification_time
        append_be32(mdhd_payload, timescale);
        append_be32(mdhd_payload, static_cast<std::uint32_t>(duration));
    }
    mdhd_payload.push_back(static_cast<std::uint8_t>((packed_language >> 8U) & 0xFFU));
    mdhd_payload.push_back(static_cast<std::uint8_t>(packed_language & 0xFFU));
    mdhd_payload.push_back(0);  // pre_defined
    mdhd_payload.push_back(0);
    const auto mdhd = mdhd_version == 1 ? make_full_box_with_flags("mdhd", 1, 0, mdhd_payload)
                                       : make_full_box("mdhd", mdhd_payload);

    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    auto visual_header = std::vector<std::uint8_t>(70, 0);
    visual_header[24] = 0x01;
    visual_header[25] = 0x40;
    visual_header[26] = 0x00;
    visual_header[27] = 0xf0;
    const auto avcc = make_box("avcC", {1, 100, 0, 12, 0xff});
    const auto btrt = make_btrt(0, 5000000, 4000000);
    const auto sample_entry = include_btrt ? make_box("avc1", concat({visual_header, avcc, btrt}))
                                           : make_box("avc1", concat({visual_header, avcc}));
    const auto stsd = make_full_box("stsd", concat({std::vector<std::uint8_t>{0, 0, 0, 1}, sample_entry}));
    const auto stbl = make_box("stbl", stsd);
    const auto minf = make_box("minf", stbl);
    const auto mdia = make_box("mdia", concat({mdhd, hdlr, minf}));
    const auto trak = make_box("trak", concat({tkhd, mdia}));
    const auto moov = make_box("moov", trak);
    const auto moof = make_box("moof", {'m', 'f', 'h', 'd'});
    const auto mdat = make_box("mdat", {1, 2, 3, 4, 5, 6});
    return concat({ftyp, moov, moof, mdat});
}

std::vector<std::uint8_t> make_multitrack_fragmented_test_mp4() {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto moov = make_box("moov", {});

    auto make_fragment = [](std::uint32_t track_id,
                            std::uint32_t base_decode_time,
                            std::uint32_t sample_duration,
                            std::initializer_list<std::uint8_t> payload_bytes) {
        const auto tfhd = make_full_box("tfhd", be32_bytes(track_id));
        const auto tfdt = make_full_box("tfdt", be32_bytes(base_decode_time));
        const auto trun = make_full_box("trun", concat({be32_bytes(1), be32_bytes(sample_duration)}));
        const auto traf = make_box("traf", concat({tfhd, tfdt, trun}));
        const auto moof = make_box("moof", traf);
        const auto mdat = make_box("mdat", std::vector<std::uint8_t>(payload_bytes));
        return concat({moof, mdat});
    };

    return concat({
        ftyp,
        moov,
        make_fragment(1, 0, 1000, {0x10, 0x11}),
        make_fragment(2, 0, 1000, {0x20, 0x21}),
        make_fragment(1, 1000, 1000, {0x12, 0x13}),
        make_fragment(2, 1000, 1000, {0x22, 0x23}),
    });
}

std::vector<std::uint8_t> make_fragmented_non_sync_ept_test_mp4() {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = make_full_box("tkhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto mdhd = make_full_box("mdhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 232, 0, 0, 0, 0, 0, 0, 0, 0});
    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    auto visual_header = std::vector<std::uint8_t>(70, 0);
    visual_header[24] = 0x01;
    visual_header[25] = 0x40;
    visual_header[26] = 0x00;
    visual_header[27] = 0xf0;
    const auto sample_entry = make_box("avc1", concat({visual_header, make_box("avcC", {1, 100, 0, 12, 0xff})}));
    const auto stsd = make_full_box("stsd", concat({std::vector<std::uint8_t>{0, 0, 0, 1}, sample_entry}));
    const auto stbl = make_box("stbl", stsd);
    const auto minf = make_box("minf", stbl);
    const auto mdia = make_box("mdia", concat({mdhd, hdlr, minf}));
    const auto trak = make_box("trak", concat({tkhd, mdia}));
    const auto moov = make_box("moov", trak);
    const auto tfhd = make_full_box("tfhd", be32_bytes(1));
    const auto tfdt = make_full_box("tfdt", be32_bytes(0));
    const auto trun = make_full_box_with_flags("trun",
                                               1,
                                               0x000D00,
                                               concat({be32_bytes(1), be32_bytes(1000), be32_bytes(0x01010000), be32_bytes(500)}));
    const auto traf = make_box("traf", concat({tfhd, tfdt, trun}));
    const auto moof = make_box("moof", traf);
    const auto mdat = make_box("mdat", {0x01, 0x02, 0x03, 0x04});
    return concat({ftyp, moov, moof, mdat});
}

std::vector<std::uint8_t> make_progressive_test_mp4() {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = make_full_box("tkhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto mdhd = make_full_box("mdhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 232, 0, 0, 7, 208, 0, 0, 0, 0});
    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    auto visual_header = std::vector<std::uint8_t>(70, 0);
    visual_header[24] = 0x01;
    visual_header[25] = 0x40;
    visual_header[26] = 0x00;
    visual_header[27] = 0xf0;
    const auto sample_entry = make_box("avc1", concat({visual_header, make_box("avcC", {1, 100, 0, 12, 0xff})}));
    const auto stsd = make_full_box("stsd", concat({std::vector<std::uint8_t>{0, 0, 0, 1}, sample_entry}));
    const auto stts = make_full_box("stts", {0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 3, 232});
    const auto stsc = make_full_box("stsc", {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1});
    const auto stsz = make_full_box("stsz", {0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 4, 0, 0, 0, 4});
    auto stco = make_full_box("stco", {0, 0, 0, 1, 0, 0, 0, 0});
    const auto stss = make_full_box("stss", {0, 0, 0, 1, 0, 0, 0, 1});
    const auto stbl = make_box("stbl", concat({stsd, stts, stsc, stsz, stco, stss}));
    const auto minf = make_box("minf", stbl);
    const auto mdia = make_box("mdia", concat({mdhd, hdlr, minf}));
    const auto trak = make_box("trak", concat({tkhd, mdia}));
    const auto moov = make_box("moov", trak);
    const auto mdat = make_box("mdat", {1, 2, 3, 4, 5, 6, 7, 8});

    std::vector<std::uint8_t> file = concat({ftyp, moov, mdat});
    const std::uint32_t mdat_payload_offset = static_cast<std::uint32_t>(ftyp.size() + moov.size() + 8);
    const std::size_t stco_payload_offset =
        ftyp.size() + 8 + tkhd.size() + 8 + mdhd.size() + hdlr.size() + 8 + 8 + stsd.size() + stts.size() +
        stsc.size() + stsz.size() + 16;
    patch_be32(file, stco_payload_offset, mdat_payload_offset);
    return file;
}

// CMSF section 4: the progressive-remux path synthesises moof boxes from
// scratch, so it cannot carry senc/saiz/saio. An encv sample entry (CENC
// protection present) must be refused rather than silently remuxed into
// undecryptable-looking output. Mirrors make_progressive_test_mp4's box
// layout with sinf added to the sample entry, as make_encrypted_fragmented_
// test_mp4 does for the fragmented fixture above.
std::vector<std::uint8_t> make_encrypted_progressive_test_mp4() {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = make_full_box("tkhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto mdhd = make_full_box("mdhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 232, 0, 0, 7, 208, 0, 0, 0, 0});
    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    auto visual_header = std::vector<std::uint8_t>(70, 0);
    visual_header[24] = 0x01;
    visual_header[25] = 0x40;
    visual_header[26] = 0x00;
    visual_header[27] = 0xf0;
    const auto avcc = make_box("avcC", {1, 100, 0, 12, 0xff});
    const auto sinf = make_sinf("avc1", "cenc");
    const auto sample_entry = make_box("encv", concat({visual_header, avcc, sinf}));
    const auto stsd = make_full_box("stsd", concat({std::vector<std::uint8_t>{0, 0, 0, 1}, sample_entry}));
    const auto stts = make_full_box("stts", {0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 3, 232});
    const auto stsc = make_full_box("stsc", {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1});
    const auto stsz = make_full_box("stsz", {0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 4, 0, 0, 0, 4});
    auto stco = make_full_box("stco", {0, 0, 0, 1, 0, 0, 0, 0});
    const auto stss = make_full_box("stss", {0, 0, 0, 1, 0, 0, 0, 1});
    const auto stbl = make_box("stbl", concat({stsd, stts, stsc, stsz, stco, stss}));
    const auto minf = make_box("minf", stbl);
    const auto mdia = make_box("mdia", concat({mdhd, hdlr, minf}));
    const auto trak = make_box("trak", concat({tkhd, mdia}));
    const auto moov = make_box("moov", trak);
    const auto mdat = make_box("mdat", {1, 2, 3, 4, 5, 6, 7, 8});

    std::vector<std::uint8_t> file = concat({ftyp, moov, mdat});
    const std::uint32_t mdat_payload_offset = static_cast<std::uint32_t>(ftyp.size() + moov.size() + 8);
    const std::size_t stco_payload_offset =
        ftyp.size() + 8 + tkhd.size() + 8 + mdhd.size() + hdlr.size() + 8 + 8 + stsd.size() + stts.size() +
        stsc.size() + stsz.size() + 16;
    patch_be32(file, stco_payload_offset, mdat_payload_offset);
    return file;
}

// Progressive (non-fragmented) MP4 with `sample_count` video samples (each
// `sample_size` bytes, all in one chunk) and sync samples at the given 1-based
// indices. Exercises bounded per-GOP coalescing: multiple keyframes -> multiple
// groups; a long run between keyframes -> capped continuation objects. Mirrors
// make_progressive_test_mp4's box layout so the stco patch offset formula holds.
std::vector<std::uint8_t> make_progressive_gops_mp4(std::uint32_t sample_count,
                                                    const std::vector<std::uint32_t>& sync_numbers,
                                                    std::uint32_t sample_size = 2) {
    auto put = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
        const auto b = be32_bytes(x);
        v.insert(v.end(), b.begin(), b.end());
    };
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = make_full_box("tkhd", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto mdhd = make_full_box("mdhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 232, 0, 0, 7, 208, 0, 0, 0, 0});
    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    auto visual_header = std::vector<std::uint8_t>(70, 0);
    visual_header[24] = 0x01;
    visual_header[25] = 0x40;
    visual_header[26] = 0x00;
    visual_header[27] = 0xf0;
    const auto sample_entry = make_box("avc1", concat({visual_header, make_box("avcC", {1, 100, 0, 12, 0xff})}));
    const auto stsd = make_full_box("stsd", concat({std::vector<std::uint8_t>{0, 0, 0, 1}, sample_entry}));

    std::vector<std::uint8_t> stts_payload;  // one run: sample_count samples, delta 1000
    put(stts_payload, 1);
    put(stts_payload, sample_count);
    put(stts_payload, 1000);
    const auto stts = make_full_box("stts", stts_payload);

    std::vector<std::uint8_t> stsc_payload;  // all samples in one chunk
    put(stsc_payload, 1);              // entry_count
    put(stsc_payload, 1);              // first_chunk
    put(stsc_payload, sample_count);   // samples_per_chunk
    put(stsc_payload, 1);              // sample_description_index
    const auto stsc = make_full_box("stsc", stsc_payload);

    std::vector<std::uint8_t> stsz_payload;  // explicit per-sample sizes
    put(stsz_payload, 0);              // sample_size 0 -> per-sample table
    put(stsz_payload, sample_count);
    for (std::uint32_t i = 0; i < sample_count; ++i) {
        put(stsz_payload, sample_size);
    }
    const auto stsz = make_full_box("stsz", stsz_payload);

    auto stco = make_full_box("stco", concat({be32_bytes(1), be32_bytes(0)}));  // one chunk, patched below

    std::vector<std::uint8_t> stss_payload;  // sync sample numbers (1-based)
    put(stss_payload, static_cast<std::uint32_t>(sync_numbers.size()));
    for (const std::uint32_t n : sync_numbers) {
        put(stss_payload, n);
    }
    const auto stss = make_full_box("stss", stss_payload);

    const auto stbl = make_box("stbl", concat({stsd, stts, stsc, stsz, stco, stss}));
    const auto minf = make_box("minf", stbl);
    const auto mdia = make_box("mdia", concat({mdhd, hdlr, minf}));
    const auto trak = make_box("trak", concat({tkhd, mdia}));
    const auto moov = make_box("moov", trak);
    const std::vector<std::uint8_t> mdat_box =
        make_box("mdat", std::vector<std::uint8_t>(static_cast<std::size_t>(sample_count) * sample_size, 0x41));

    std::vector<std::uint8_t> file = concat({ftyp, moov, mdat_box});
    const std::uint32_t mdat_payload_offset = static_cast<std::uint32_t>(ftyp.size() + moov.size() + 8);
    const std::size_t stco_payload_offset =
        ftyp.size() + 8 + tkhd.size() + 8 + mdhd.size() + hdlr.size() + 8 + 8 + stsd.size() + stts.size() +
        stsc.size() + stsz.size() + 16;
    patch_be32(file, stco_payload_offset, mdat_payload_offset);
    return file;
}

// Largest trun sample_count across all fragments in a CMAF object's bytes.
// Scans for the trun FourCC (synthetic mdat payloads never contain it), reading
// the sample_count field 8 bytes past the type (after the 4-byte version/flags).
std::uint32_t max_trun_sample_count(const std::vector<std::uint8_t>& fragment) {
    std::uint32_t mx = 0;
    for (std::size_t i = 0; i + 12 <= fragment.size(); ++i) {
        if (fragment[i] == 't' && fragment[i + 1] == 'r' && fragment[i + 2] == 'u' && fragment[i + 3] == 'n') {
            const std::size_t p = i + 8;
            const std::uint32_t sc = (static_cast<std::uint32_t>(fragment[p]) << 24) |
                                     (static_cast<std::uint32_t>(fragment[p + 1]) << 16) |
                                     (static_cast<std::uint32_t>(fragment[p + 2]) << 8) |
                                     static_cast<std::uint32_t>(fragment[p + 3]);
            if (sc > mx) {
                mx = sc;
            }
        }
    }
    return mx;
}

std::vector<std::uint8_t> make_multitrack_init_mp4() {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});

    const auto video_tkhd = make_full_box("tkhd",
                                          {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto video_hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    auto video_header = std::vector<std::uint8_t>(70, 0);
    video_header[24] = 0x01;
    video_header[25] = 0x40;
    video_header[26] = 0x00;
    video_header[27] = 0xf0;
    const auto video_sample_entry =
        make_box("avc1", concat({video_header, make_box("avcC", {1, 100, 0, 12, 0xff})}));
    const auto video_stsd = make_full_box("stsd",
                                          concat({std::vector<std::uint8_t>{0, 0, 0, 1}, video_sample_entry}));
    const auto video_stbl = make_box("stbl", video_stsd);
    const auto video_minf = make_box("minf", video_stbl);
    const auto video_mdia = make_box("mdia", concat({video_hdlr, video_minf}));
    const auto video_trak = make_box("trak", concat({video_tkhd, video_mdia}));

    const auto audio_tkhd = make_full_box("tkhd",
                                          {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0});
    const auto audio_hdlr = make_full_box("hdlr", {0, 0, 0, 0, 's', 'o', 'u', 'n', 0, 0, 0, 0});
    auto audio_header = std::vector<std::uint8_t>(28, 0);
    audio_header[16] = 0x00;
    audio_header[17] = 0x02;
    audio_header[24] = 0xbb;
    audio_header[25] = 0x80;
    const auto audio_sample_entry =
        make_box("mp4a", concat({audio_header, make_box("esds", {0x00, 0x00, 0x00, 0x00, 0x03, 0x19, 0x00, 0x02, 0x00, 0x04, 0x11, 0x40, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x02, 0x10, 0x10})}));
    const auto audio_stsd = make_full_box("stsd",
                                          concat({std::vector<std::uint8_t>{0, 0, 0, 1}, audio_sample_entry}));
    const auto audio_stbl = make_box("stbl", audio_stsd);
    const auto audio_minf = make_box("minf", audio_stbl);
    const auto audio_mdia = make_box("mdia", concat({audio_hdlr, audio_minf}));
    const auto audio_trak = make_box("trak", concat({audio_tkhd, audio_mdia}));

    const auto trex_video =
        make_full_box("trex", {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    const auto trex_audio =
        make_full_box("trex", {0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    const auto mvex = make_box("mvex", concat({trex_video, trex_audio}));
    const auto moov = make_box("moov", concat({video_trak, audio_trak, mvex}));
    return concat({ftyp, moov});
}

std::vector<std::uint8_t> make_hevc_init_mp4(std::uint8_t general_profile_byte,
                                             std::uint32_t compatibility_flags,
                                             std::array<std::uint8_t, 6> constraint_bytes,
                                             std::uint8_t level_idc) {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = make_full_box("tkhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    auto video_header = std::vector<std::uint8_t>(70, 0);
    video_header[24] = 0x01;
    video_header[25] = 0x40;
    video_header[26] = 0x00;
    video_header[27] = 0xf0;

    std::vector<std::uint8_t> hvcc_payload = {
        0x01,
        general_profile_byte,
        static_cast<std::uint8_t>((compatibility_flags >> 24U) & 0xFFU),
        static_cast<std::uint8_t>((compatibility_flags >> 16U) & 0xFFU),
        static_cast<std::uint8_t>((compatibility_flags >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(compatibility_flags & 0xFFU),
        constraint_bytes[0],
        constraint_bytes[1],
        constraint_bytes[2],
        constraint_bytes[3],
        constraint_bytes[4],
        constraint_bytes[5],
        level_idc,
    };
    const auto sample_entry = make_box("hev1", concat({video_header, make_box("hvcC", hvcc_payload)}));
    const auto stsd = make_full_box("stsd", concat({std::vector<std::uint8_t>{0, 0, 0, 1}, sample_entry}));
    const auto stbl = make_box("stbl", stsd);
    const auto minf = make_box("minf", stbl);
    const auto mdia = make_box("mdia", concat({hdlr, minf}));
    const auto trak = make_box("trak", concat({tkhd, mdia}));
    const auto moov = make_box("moov", trak);
    return concat({ftyp, moov});
}

std::vector<std::uint8_t> hevc_length_prefixed_sample(std::initializer_list<std::vector<std::uint8_t>> nal_units) {
    std::vector<std::uint8_t> sample;
    for (const auto& nal : nal_units) {
        append_be32(sample, static_cast<std::uint32_t>(nal.size()));
        sample.insert(sample.end(), nal.begin(), nal.end());
    }
    return sample;
}

std::vector<std::uint8_t> make_progressive_hevc_mp4(const std::vector<std::uint8_t>& sample_payload) {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = make_full_box("tkhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto mdhd = make_full_box("mdhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 232, 0, 0, 3, 232, 0, 0, 0, 0});
    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    auto video_header = std::vector<std::uint8_t>(70, 0);
    video_header[24] = 0x01;
    video_header[25] = 0x40;
    video_header[26] = 0x00;
    video_header[27] = 0xf0;
    const std::vector<std::uint8_t> hvcc_payload = {
        0x01, 0x01, 0x60, 0x00, 0x00, 0x00, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 90,
    };
    const auto sample_entry = make_box("hev1", concat({video_header, make_box("hvcC", hvcc_payload)}));
    const auto stsd = make_full_box("stsd", concat({std::vector<std::uint8_t>{0, 0, 0, 1}, sample_entry}));
    const auto stts = make_full_box("stts", {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 3, 232});
    const auto stsc = make_full_box("stsc", {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1});
    const auto stsz = make_full_box("stsz",
                                    concat({std::vector<std::uint8_t>{0, 0, 0, 0},
                                            std::vector<std::uint8_t>{0, 0, 0, 1},
                                            be32_bytes(static_cast<std::uint32_t>(sample_payload.size()))}));
    auto stco = make_full_box("stco", {0, 0, 0, 1, 0, 0, 0, 0});
    const auto stss = make_full_box("stss", {0, 0, 0, 1, 0, 0, 0, 1});
    const auto stbl = make_box("stbl", concat({stsd, stts, stsc, stsz, stco, stss}));
    const auto minf = make_box("minf", stbl);
    const auto mdia = make_box("mdia", concat({mdhd, hdlr, minf}));
    const auto trak = make_box("trak", concat({tkhd, mdia}));
    const auto moov = make_box("moov", trak);
    const auto mdat = make_box("mdat", sample_payload);

    std::vector<std::uint8_t> file = concat({ftyp, moov, mdat});
    const std::uint32_t mdat_payload_offset = static_cast<std::uint32_t>(ftyp.size() + moov.size() + 8);
    const std::size_t stco_payload_offset =
        ftyp.size() + 8 + tkhd.size() + 8 + mdhd.size() + hdlr.size() + 8 + 8 + stsd.size() + stts.size() +
        stsc.size() + stsz.size() + 16;
    patch_be32(file, stco_payload_offset, mdat_payload_offset);
    return file;
}

std::vector<std::uint8_t> make_fragmented_hevc_mp4(const std::vector<std::uint8_t>& sample_payload) {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = make_full_box("tkhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto mdhd = make_full_box("mdhd",
                                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 232, 0, 0, 3, 232, 0, 0, 0, 0});
    const auto hdlr = make_full_box("hdlr", {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    auto video_header = std::vector<std::uint8_t>(70, 0);
    video_header[24] = 0x01;
    video_header[25] = 0x40;
    video_header[26] = 0x00;
    video_header[27] = 0xf0;
    const std::vector<std::uint8_t> hvcc_payload = {
        0x01, 0x01, 0x60, 0x00, 0x00, 0x00, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 90,
    };
    const auto sample_entry = make_box("hev1", concat({video_header, make_box("hvcC", hvcc_payload)}));
    const auto stsd = make_full_box("stsd", concat({std::vector<std::uint8_t>{0, 0, 0, 1}, sample_entry}));
    const auto stbl = make_box("stbl", stsd);
    const auto minf = make_box("minf", stbl);
    const auto mdia = make_box("mdia", concat({mdhd, hdlr, minf}));
    const auto trak = make_box("trak", concat({tkhd, mdia}));
    const auto trex = make_full_box("trex", {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 3, 232, 0, 0, 0, 0, 0, 0, 0, 0});
    const auto mvex = make_box("mvex", trex);
    const auto moov = make_box("moov", concat({trak, mvex}));

    const auto tfhd = make_full_box("tfhd", be32_bytes(1));
    const auto tfdt = make_full_box("tfdt", be32_bytes(0));
    const auto trun = make_full_box_with_flags("trun",
                                               0,
                                               0x000201,
                                               concat({be32_bytes(1),
                                                       be32_bytes(0),
                                                       be32_bytes(static_cast<std::uint32_t>(sample_payload.size()))}));
    const auto traf = make_box("traf", concat({tfhd, tfdt, trun}));
    const auto moof = make_box("moof", traf);
    const auto mdat = make_box("mdat", sample_payload);
    return concat({ftyp, moov, moof, mdat});
}

std::size_t find_after(std::string_view haystack, std::string_view needle, std::size_t start = 0) {
    const std::size_t pos = haystack.find(needle, start);
    return pos == std::string_view::npos ? pos : pos + needle.size();
}

// MSF v1 initData lookup: resolve a track's initRef into
// the root initDataList and return that entry's Base64 "data" value.
std::string msf_track_init_data(std::string_view catalog, std::string_view track_name) {
    const std::string name_key = std::string("\"name\":\"") + std::string(track_name) + "\"";
    const std::size_t name_pos = catalog.find(name_key);
    if (name_pos == std::string_view::npos) {
        return {};
    }
    const std::size_t ref_pos = find_after(catalog, "\"initRef\":\"", name_pos);
    if (ref_pos == std::string_view::npos) {
        return {};
    }
    const std::size_t ref_end = catalog.find('"', ref_pos);
    if (ref_end == std::string_view::npos) {
        return {};
    }
    const std::string init_id(catalog.substr(ref_pos, ref_end - ref_pos));

    const std::string id_key = "\"id\":\"" + init_id + "\"";
    const std::size_t id_pos = catalog.find(id_key);
    if (id_pos == std::string_view::npos) {
        return {};
    }
    const std::size_t data_pos = find_after(catalog, "\"data\":\"", id_pos);
    if (data_pos == std::string_view::npos) {
        return {};
    }
    const std::size_t data_end = catalog.find('"', data_pos);
    if (data_end == std::string_view::npos) {
        return {};
    }
    return std::string(catalog.substr(data_pos, data_end - data_pos));
}

// Collect every value that follows a given "key":" prefix, e.g. every
// "initRef":"..." or "id":"..." value in a serialized catalog.
std::vector<std::string> extract_all_values(std::string_view catalog, std::string_view key_prefix) {
    std::vector<std::string> values;
    std::size_t pos = 0;
    while (true) {
        const std::size_t found = catalog.find(key_prefix, pos);
        if (found == std::string_view::npos) {
            break;
        }
        const std::size_t value_start = found + key_prefix.size();
        const std::size_t value_end = catalog.find('"', value_start);
        if (value_end == std::string_view::npos) {
            break;
        }
        values.emplace_back(catalog.substr(value_start, value_end - value_start));
        pos = value_end + 1;
    }
    return values;
}

// True when every "initRef":"..." value in the catalog also appears as an
// "id":"..." value (i.e. an initDataList entry). Guards against a future
// refactor loosening the serializer's own dangling-reference check.
bool all_init_refs_resolve(std::string_view catalog) {
    const std::vector<std::string> init_refs = extract_all_values(catalog, "\"initRef\":\"");
    const std::vector<std::string> init_ids = extract_all_values(catalog, "\"id\":\"");
    for (const auto& ref : init_refs) {
        bool found = false;
        for (const auto& id : init_ids) {
            if (id == ref) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

// Parse the unsigned integer that follows a raw (unquoted) "key": prefix,
// e.g. "maxGrpSapStartingType":2. Returns -1 when the key is absent.
long long extract_uint_value(std::string_view catalog, std::string_view key_prefix) {
    const std::size_t found = catalog.find(key_prefix);
    if (found == std::string_view::npos) {
        return -1;
    }
    std::size_t value_start = found + key_prefix.size();
    std::size_t value_end = value_start;
    while (value_end < catalog.size() && std::isdigit(static_cast<unsigned char>(catalog[value_end]))) {
        ++value_end;
    }
    if (value_end == value_start) {
        return -1;
    }
    return std::stoll(std::string(catalog.substr(value_start, value_end - value_start)));
}

// True when the catalog text has no non-spec numeric "id" field on a track
// object. MSF's initDataList entries legitimately have a quoted string "id",
// so this only rejects a bare (unquoted, i.e. numeric) "id" value.
bool no_numeric_id_field(std::string_view catalog) {
    std::size_t pos = 0;
    while (true) {
        pos = catalog.find("\"id\":", pos);
        if (pos == std::string_view::npos) {
            return true;
        }
        const std::size_t value_pos = pos + 5;
        if (value_pos < catalog.size() && catalog[value_pos] != '"') {
            return false;
        }
        pos = value_pos;
    }
}

std::vector<std::uint8_t> base64_decode(std::string_view text) {
    std::map<char, std::uint8_t> table;
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t index = 0; index < alphabet.size(); ++index) {
        table.emplace(alphabet[index], static_cast<std::uint8_t>(index));
    }

    std::vector<std::uint8_t> decoded;
    for (std::size_t index = 0; index < text.size(); index += 4) {
        const std::uint32_t a = table.at(text[index]);
        const std::uint32_t b = table.at(text[index + 1]);
        const std::uint32_t c = text[index + 2] == '=' ? 0 : table.at(text[index + 2]);
        const std::uint32_t d = text[index + 3] == '=' ? 0 : table.at(text[index + 3]);
        const std::uint32_t word = (a << 18U) | (b << 12U) | (c << 6U) | d;
        decoded.push_back(static_cast<std::uint8_t>((word >> 16U) & 0xFFU));
        if (text[index + 2] != '=') {
            decoded.push_back(static_cast<std::uint8_t>((word >> 8U) & 0xFFU));
        }
        if (text[index + 3] != '=') {
            decoded.push_back(static_cast<std::uint8_t>(word & 0xFFU));
        }
    }
    return decoded;
}

std::string object_text(const openmoq::publisher::CmsfObject& object) {
    return std::string(object.owned_payload.begin(), object.owned_payload.end());
}

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

bool bytes_equal(const std::vector<std::uint8_t>& bytes, std::initializer_list<std::uint8_t> expected) {
    return std::vector<std::uint8_t>(expected) == bytes;
}

bool expect_contains(std::string_view haystack, std::string_view needle, const std::string& message) {
    return expect(haystack.find(needle) != std::string_view::npos, message);
}

bool expect_not_contains(std::string_view haystack, std::string_view needle, const std::string& message) {
    return expect(haystack.find(needle) == std::string_view::npos, message);
}

}  // namespace

int main() {
    using namespace openmoq::publisher;

    bool ok = true;

    const auto fragmented_bytes = make_fragmented_test_mp4();
    ParsedMp4 fragmented{
        .bytes = fragmented_bytes,
        .top_level_boxes = parse_mp4_boxes(fragmented_bytes),
        .tracks = {
            TrackDescription{.track_id = 1, .handler_type = "vide", .codec = "avc1.64000C", .sample_entry_type = "avc1", .track_name = "vide_1", .timescale = 24000, .duration_ms = 5000},
        },
    };

    const auto segmented = segment_for_cmaf(fragmented);
    const auto plan = build_publish_plan(segmented, DraftVersion::kDraft14);
    const auto sap_plan = build_publish_plan(segmented, DraftVersion::kDraft14, true);
    const auto msf_timeline_plan = build_publish_plan(segmented, DraftVersion::kDraft14, false, true);

    ok &= expect(fragmented.top_level_boxes.size() == 4, "expected 4 top-level boxes");
    ok &= expect(fragmented.tracks.size() == 1, "expected one extracted fragmented track");
    ok &= expect(fragmented.tracks.front().codec == "avc1.64000C", "expected RFC 6381 avc1 codec");
    ok &= expect(segmented.fragments.size() == 1, "expected one fragmented media fragment");
    ok &= expect(plan.objects.size() == 2, "expected catalog and one fragmented media object when SAP is disabled");
    ok &= expect(plan.objects.front().track_name == "catalog", "expected catalog object first");

    const std::string plan_catalog_text = object_text(plan.objects.front());
    ok &= expect_contains(plan_catalog_text, "\"version\":\"1\"",
                          "expected MSF v1 string version in publish plan catalog");
    ok &= expect_not_contains(plan_catalog_text, "\"format\"",
                              "expected no legacy format field");
    ok &= expect_not_contains(plan_catalog_text, "\"initData\":",
                              "expected initDataList instead of inline initData");
    ok &= expect_contains(plan_catalog_text, "\"initDataList\"",
                          "expected root initDataList");
    ok &= expect_contains(plan_catalog_text, "\"initRef\"",
                          "expected per-track initRef");
    ok &= expect(no_numeric_id_field(plan_catalog_text),
                "expected no non-spec numeric track id field");
    ok &= expect_not_contains(plan_catalog_text, "\"name\":\"catalog\"",
                              "expected the synthetic catalog track to be absent from its own tracks array");
    ok &= expect(all_init_refs_resolve(plan_catalog_text),
                "expected every initRef to resolve to an initDataList id");

    // CMSF section 3.5.2: the max SAP type Groups and Objects start with.
    ok &= expect_contains(plan_catalog_text, "\"maxGrpSapStartingType\":",
                          "expected maxGrpSapStartingType on a CMAF media track");
    ok &= expect_contains(plan_catalog_text, "\"maxObjSapStartingType\":",
                          "expected maxObjSapStartingType on a CMAF media track");

    ok &= expect(sap_plan.objects.size() == 3, "expected SAP-enabled plan to add one SAP timeline object");
    ok &= expect(sap_plan.objects.back().track_name == "vide_1_sap", "expected SAP timeline object for fragmented video");
    ok &= expect_contains(object_text(sap_plan.objects.back()), "\"l\":[0,0]", "expected fragmented SAP timeline location");
    ok &= expect_contains(object_text(sap_plan.objects.back()), "\"data\":[2,0]", "expected fragmented SAP type and EPT");
    ok &= expect(msf_timeline_plan.objects.size() == 3,
                 "expected MSF timeline-enabled plan to add one media timeline object");
    ok &= expect(msf_timeline_plan.objects.back().track_name == "timeline",
                 "expected MSF media timeline track name");
    ok &= expect_contains(object_text(msf_timeline_plan.objects.back()), "[0,[0,0],0]",
                         "expected MSF media timeline to map media time to MOQT location");
    ok &= expect(payload_size(segmented.initialization_segment) > 0, "expected fragmented init payload");
    ok &= expect(payload_size(segmented.fragments.front().payload) > 0, "expected fragmented media payload");

    std::stringstream fragmented_stream(std::ios::in | std::ios::out | std::ios::binary);
    fragmented_stream.write(reinterpret_cast<const char*>(fragmented_bytes.data()),
                            static_cast<std::streamsize>(fragmented_bytes.size()));
    fragmented_stream.seekg(0, std::ios::beg);
    const ParsedMp4 parsed_from_stream = parse_mp4_stream(fragmented_stream, "memory");
    ok &= expect(parsed_from_stream.bytes == fragmented_bytes, "expected stream parser to preserve input bytes");
    ok &= expect(parsed_from_stream.top_level_boxes.size() == 4, "expected stream parser to decode top-level boxes");
    ok &= expect(parsed_from_stream.tracks.size() == 1, "expected stream parser to extract track metadata");

    const auto multitrack_fragmented_bytes = make_multitrack_fragmented_test_mp4();
    ParsedMp4 multitrack_fragmented{
        .bytes = multitrack_fragmented_bytes,
        .top_level_boxes = parse_mp4_boxes(multitrack_fragmented_bytes),
        .tracks = {
            TrackDescription{.track_id = 1, .handler_type = "vide", .codec = "avc1.64000C", .sample_entry_type = "avc1", .track_name = "vide_1", .timescale = 1000},
            TrackDescription{.track_id = 2, .handler_type = "soun", .codec = "mp4a.40.2", .sample_entry_type = "mp4a", .track_name = "soun_2", .timescale = 1000},
        },
    };
    const auto segmented_multitrack = segment_for_cmaf(multitrack_fragmented);
    ok &= expect(segmented_multitrack.fragments.size() == 4, "expected four multitrack fragmented media fragments");
    ok &= expect(segmented_multitrack.fragments[0].track_name == "vide_1" && segmented_multitrack.fragments[0].group_id == 0,
                 "expected first video fragment group sequence to start at zero");
    ok &= expect(segmented_multitrack.fragments[1].track_name == "soun_2" && segmented_multitrack.fragments[1].group_id == 0,
                 "expected first audio fragment group sequence to start at zero");
    ok &= expect(segmented_multitrack.fragments[2].track_name == "vide_1" && segmented_multitrack.fragments[2].group_id == 1,
                 "expected second video fragment group sequence to advance independently");
    ok &= expect(segmented_multitrack.fragments[3].track_name == "soun_2" && segmented_multitrack.fragments[3].group_id == 1,
                 "expected second audio fragment group sequence to advance independently");

    const auto non_sync_fragmented_bytes = make_fragmented_non_sync_ept_test_mp4();
    ParsedMp4 non_sync_fragmented{
        .bytes = non_sync_fragmented_bytes,
        .top_level_boxes = parse_mp4_boxes(non_sync_fragmented_bytes),
        .tracks = {
            TrackDescription{.track_id = 1, .handler_type = "vide", .codec = "avc1.64000C", .sample_entry_type = "avc1", .track_name = "vide_1", .timescale = 1000},
        },
    };
    const auto non_sync_segmented = segment_for_cmaf(non_sync_fragmented);
    const auto non_sync_plan = build_publish_plan(non_sync_segmented, DraftVersion::kDraft14, true);
    ok &= expect(non_sync_segmented.fragments.size() == 1, "expected one non-sync fragmented media fragment");
    ok &= expect(non_sync_segmented.fragments.front().sap_type == 0, "expected non-sync fragmented SAP type 0");
    ok &= expect(non_sync_segmented.fragments.front().earliest_presentation_time_us == 500000,
                 "expected derived fragmented earliest presentation time");
    ok &= expect_contains(object_text(non_sync_plan.objects.back()), "\"data\":[0,500]",
                         "expected SAP timeline to expose non-sync fragment and EPT");

    const auto progressive_bytes = make_progressive_test_mp4();
    ParsedMp4 progressive{
        .bytes = progressive_bytes,
        .top_level_boxes = parse_mp4_boxes(progressive_bytes),
        .tracks = {},
    };
    progressive.tracks = extract_tracks(progressive.top_level_boxes, progressive.bytes);

    const auto remuxed = segment_for_cmaf(progressive);
    const auto remuxed_plan = build_publish_plan(remuxed, DraftVersion::kDraft14);
    const auto remuxed_sap_plan = build_publish_plan(remuxed, DraftVersion::kDraft14, true);

    ok &= expect(progressive.tracks.size() == 1, "expected one progressive track");
    ok &= expect(!remuxed.initialization_segment.owned_bytes.empty(), "expected synthesized init segment");
    ok &= expect(remuxed.fragments.size() == 2, "expected split remuxed samples to produce two media objects");
    ok &= expect(!remuxed.fragments.front().payload.owned_bytes.empty(), "expected synthesized media fragment");
    ok &= expect(!remuxed.fragments[1].payload.owned_bytes.empty(), "expected second synthesized media fragment");
    ok &= expect(remuxed_plan.objects.size() == 3, "expected catalog and two media objects for remuxed file by default");
    ok &= expect(remuxed_plan.objects.front().track_name == "catalog", "expected remuxed catalog object first");
    ok &= expect(remuxed_plan.objects[1].track_name == "vide_1", "expected remuxed track naming");
    ok &= expect(remuxed_plan.objects[2].track_name == "vide_1", "expected second remuxed media object");
    ok &= expect(remuxed_sap_plan.objects.size() == 4, "expected SAP-enabled remuxed plan to add timeline object");
    ok &= expect(remuxed_sap_plan.objects[3].track_name == "vide_1_sap", "expected remuxed SAP timeline track");
    ok &= expect(remuxed_plan.track_initializations.size() == 1, "expected one remuxed track init payload");
    ok &= expect(remuxed_plan.track_initializations.front().track_name == "vide_1",
                 "expected remuxed init payload to follow the media track name");
    ok &= expect(!remuxed_plan.track_initializations.front().codec_payload.empty(),
                 "expected remuxed codec init payload");
    ok &= expect(!remuxed_plan.track_initializations.front().init_segment.empty(),
                 "expected remuxed standalone init segment");

    // CMSF section 4: the progressive-remux path synthesises moof boxes from
    // scratch, so it cannot carry senc, saiz, or saio. An encv track must be
    // refused rather than silently remuxed into undecryptable-looking output.
    {
        const auto encrypted_progressive_bytes = make_encrypted_progressive_test_mp4();
        ParsedMp4 parsed_encrypted_progressive{
            .bytes = encrypted_progressive_bytes,
            .top_level_boxes = parse_mp4_boxes(encrypted_progressive_bytes),
            .tracks = {},
        };
        parsed_encrypted_progressive.tracks =
            extract_tracks(parsed_encrypted_progressive.top_level_boxes, parsed_encrypted_progressive.bytes);
        ok &= expect(parsed_encrypted_progressive.tracks.size() == 1 &&
                     parsed_encrypted_progressive.tracks.front().protection.has_value(),
                     "expected the encrypted progressive fixture to report CENC protection");

        bool refused_encrypted_remux = false;
        std::string refusal_message;
        try {
            (void)segment_for_cmaf(parsed_encrypted_progressive);
        } catch (const std::runtime_error& error) {
            refused_encrypted_remux = true;
            refusal_message = error.what();
        }
        ok &= expect(refused_encrypted_remux,
                     "expected encrypted progressive input to be refused rather than remuxed");
        ok &= expect(refusal_message.find("progressive remux") != std::string::npos,
                     "expected the refusal message to name the progressive remux path");
        ok &= expect(refusal_message.find(parsed_encrypted_progressive.tracks.front().track_name) != std::string::npos,
                     "expected the refusal message to name the offending track");
    }

    // Bounded per-GOP coalescing: a multi-keyframe progressive MP4 must become
    // multiple media objects (one group per GOP), never a single whole-track
    // object, and no fragment may exceed libmoq's 512-sample CMAF validator.
    {
        // 8 samples, keyframes at sample 1 and sample 5 -> two GOPs of 4 samples.
        const auto gops_bytes = make_progressive_gops_mp4(8, {1, 5});
        ParsedMp4 gops{.bytes = gops_bytes, .top_level_boxes = parse_mp4_boxes(gops_bytes), .tracks = {}};
        gops.tracks = extract_tracks(gops.top_level_boxes, gops.bytes);

        const auto coalesced = segment_for_cmaf(gops, CmafObjectMode::kCoalesced);
        const auto split = segment_for_cmaf(gops, CmafObjectMode::kSplit);

        ok &= expect(split.fragments.size() == 8, "expected split mode to emit one object per sample");
        ok &= expect(coalesced.fragments.size() == 2,
                     "expected coalesced multi-GOP MP4 to emit one object per GOP, not one whole-track object");
        ok &= expect(coalesced.fragments.size() > 1,
                     "expected coalesced mode to never emit a single whole-track object");

        bool groups_ok = true;
        bool sap_ok = true;
        bool bound_ok = true;
        for (std::size_t i = 0; i < coalesced.fragments.size(); ++i) {
            const auto& frag = coalesced.fragments[i];
            // One group per track (track_index 0), each GOP a sequential object.
            groups_ok = groups_ok && frag.group_id == 0 && frag.object_id == i;
            // Each GOP-start video object carries declared SAP type 2.
            sap_ok = sap_ok && frag.has_sap_type && frag.sap_type == 2;
            const std::uint32_t trun_samples = max_trun_sample_count(frag.payload.owned_bytes);
            bound_ok = bound_ok && trun_samples == 4 && trun_samples < 512;
        }
        ok &= expect(groups_ok, "expected one group per track with one sequential object per GOP");
        ok &= expect(sap_ok, "expected each GOP-start video object to declare SAP type 2");
        ok &= expect(bound_ok, "expected each per-GOP fragment to carry exactly its 4 samples (<512)");

        // A long single GOP must be split into capped continuation objects rather
        // than one oversized trun -- this is the >512-sample validator guard.
        const auto long_bytes = make_progressive_gops_mp4(520, {1});
        ParsedMp4 long_gop{.bytes = long_bytes, .top_level_boxes = parse_mp4_boxes(long_bytes), .tracks = {}};
        long_gop.tracks = extract_tracks(long_gop.top_level_boxes, long_gop.bytes);
        const auto long_coalesced = segment_for_cmaf(long_gop, CmafObjectMode::kCoalesced);

        ok &= expect(long_coalesced.fragments.size() > 1,
                     "expected a 520-sample GOP to be chunked into multiple capped objects");
        bool long_bound_ok = !long_coalesced.fragments.empty();
        bool single_group_ok = true;
        for (std::size_t i = 0; i < long_coalesced.fragments.size(); ++i) {
            const auto& frag = long_coalesced.fragments[i];
            single_group_ok = single_group_ok && frag.group_id == 0 && frag.object_id == i;
            const std::uint32_t trun_samples = max_trun_sample_count(frag.payload.owned_bytes);
            long_bound_ok = long_bound_ok && trun_samples > 0 && trun_samples < 512;
        }
        ok &= expect(long_bound_ok, "expected every chunked fragment trun to stay below the 512-sample validator");
        ok &= expect(single_group_ok, "expected a long GOP to stay one group with sequential continuation objects");
        ok &= expect(long_coalesced.fragments.front().sap_type == 2,
                     "expected the long GOP's first object to keep SAP type 2");
        ok &= expect(long_coalesced.fragments.back().sap_type == 0,
                     "expected long GOP continuation objects to be non-SAP");
    }

    const auto multitrack_init_bytes = make_multitrack_init_mp4();
    const SegmentedMp4 multitrack_segmented{
        .initialization_segment = {.span = {}, .owned_bytes = multitrack_init_bytes},
        .fragments = {},
        .tracks = {
            TrackDescription{.track_id = 1, .handler_type = "vide", .codec = "avc1.64000C", .sample_entry_type = "avc1", .track_name = "vide_1", .width = 320, .height = 240},
            TrackDescription{.track_id = 2, .handler_type = "soun", .codec = "mp4a.40.2", .sample_entry_type = "mp4a", .track_name = "soun_2", .channel_count = 2, .sample_rate = 48000},
        },
    };
    const auto multitrack_plan = build_publish_plan(multitrack_segmented, DraftVersion::kDraft14);
    const auto multitrack_sap_plan = build_publish_plan(multitrack_segmented, DraftVersion::kDraft14, true);
    const auto multitrack_msf_timeline_plan = build_publish_plan(multitrack_segmented, DraftVersion::kDraft14, false, true);
    const std::string catalog_text(multitrack_plan.objects.front().owned_payload.begin(),
                                   multitrack_plan.objects.front().owned_payload.end());
    const std::string sap_catalog_text(multitrack_sap_plan.objects.front().owned_payload.begin(),
                                       multitrack_sap_plan.objects.front().owned_payload.end());
    const std::string msf_timeline_catalog_text(multitrack_msf_timeline_plan.objects.front().owned_payload.begin(),
                                                multitrack_msf_timeline_plan.objects.front().owned_payload.end());
    const std::string video_init_data = msf_track_init_data(catalog_text, "vide_1");
    const std::string audio_init_data = msf_track_init_data(catalog_text, "soun_2");
    ok &= expect(!video_init_data.empty(), "expected video initData in catalog");
    ok &= expect(!audio_init_data.empty(), "expected audio initData in catalog");
    ok &= expect(video_init_data != audio_init_data, "expected per-track initData entries to differ");
    ok &= expect_contains(catalog_text, "\"role\":\"video\"", "expected video role in catalog");
    ok &= expect_contains(catalog_text, "\"role\":\"audio\"", "expected audio role in catalog");
    ok &= expect_contains(catalog_text, "\"codec\":\"avc1.64000C\"", "expected video codec string in catalog");
    ok &= expect_contains(catalog_text, "\"codec\":\"mp4a.40.2\"", "expected audio codec string in catalog");
    ok &= expect_contains(catalog_text, "\"width\":320", "expected video width in catalog");
    ok &= expect_contains(catalog_text, "\"height\":240", "expected video height in catalog");
    ok &= expect_contains(catalog_text, "\"samplerate\":48000", "expected audio sample rate in catalog");
    ok &= expect_contains(catalog_text, "\"channelConfig\":\"2\"", "expected audio channel count in catalog");
    ok &= expect_contains(catalog_text, "\"renderGroup\":1", "expected renderGroup in catalog");
    // The publisher is live, or simulating live, unless configured otherwise.
    ok &= expect_contains(catalog_text, "\"isLive\":true",
                          "expected batch publish to default to live");
    ok &= expect_contains(catalog_text, "\"generatedAt\":",
                          "expected generatedAt on a live batch catalog");
    ok &= expect_not_contains(catalog_text, "\"trackDuration\"",
                              "expected no trackDuration on a live batch catalog (MSF 5.2.35)");

    // Opt-in VOD retains the previous semantics.
    const auto vod_plan = build_publish_plan(segmented, DraftVersion::kDraft14,
                                             /*include_sap=*/false,
                                             /*include_msf_timeline=*/false,
                                             /*vod=*/true);
    const std::string vod_catalog_text = object_text(vod_plan.objects.front());
    ok &= expect_contains(vod_catalog_text, "\"isLive\":false",
                          "expected opt-in VOD to mark tracks not live");
    ok &= expect_not_contains(vod_catalog_text, "\"generatedAt\":",
                              "expected no generatedAt on a VOD catalog (MSF 5.1.2)");
    ok &= expect_contains(vod_catalog_text, "\"trackDuration\":5000",
                          "expected VOD catalog to include trackDuration with the test fixture value (MSF 5.2.35)");
    ok &= expect_contains(catalog_text, "\"bitrate\":2000000", "expected non-zero default video bitrate in catalog");
    ok &= expect_contains(catalog_text, "\"bitrate\":128000", "expected non-zero default audio bitrate in catalog");
    ok &= expect_not_contains(catalog_text, "\"name\":\"catalog\"",
                              "expected the synthetic catalog track to be absent from the batch catalog");
    ok &= expect(all_init_refs_resolve(catalog_text),
                "expected every initRef in the batch catalog to resolve to an initDataList id");
    ok &= expect_not_contains(catalog_text, "\"name\":\"vide_1_sap\"", "expected video SAP track to be absent from the default catalog");
    ok &= expect_not_contains(catalog_text, "\"name\":\"soun_2_sap\"", "expected audio SAP track to be absent from the default catalog");
    ok &= expect_not_contains(catalog_text, "\"name\":\"timeline\"", "expected MSF timeline track to be absent from the default catalog");
    // CMSF section 3.5.2: both fields are Optional and neither track here has
    // any fragments, so no SAP information exists to report.
    ok &= expect_not_contains(catalog_text, "\"maxGrpSapStartingType\":",
                              "expected no maxGrpSapStartingType on a track with no SAP information");
    ok &= expect_not_contains(catalog_text, "\"maxObjSapStartingType\":",
                              "expected no maxObjSapStartingType on a track with no SAP information");
    ok &= expect_contains(sap_catalog_text, "\"name\":\"vide_1_sap\"", "expected video SAP timeline track in SAP-enabled catalog");
    ok &= expect_contains(sap_catalog_text, "\"name\":\"soun_2_sap\"", "expected audio SAP timeline track in SAP-enabled catalog");
    ok &= expect_contains(sap_catalog_text, "\"packaging\":\"eventtimeline\"", "expected event timeline packaging in SAP-enabled catalog");
    ok &= expect_contains(sap_catalog_text, "\"eventType\":\"org.ietf.moq.cmsf.sap\"", "expected CMSF SAP event type in SAP-enabled catalog");
    ok &= expect_contains(sap_catalog_text, "\"mimeType\":\"application/json\"", "expected event timeline mime type in SAP-enabled catalog");
    ok &= expect_contains(sap_catalog_text, "\"depends\":[\"vide_1\"]", "expected video SAP timeline dependency");
    ok &= expect_contains(sap_catalog_text, "\"depends\":[\"soun_2\"]", "expected audio SAP timeline dependency");
    ok &= expect_contains(msf_timeline_catalog_text, "\"name\":\"timeline\"", "expected MSF media timeline track in catalog");
    ok &= expect_contains(msf_timeline_catalog_text, "\"role\":\"mediatimeline\"", "expected MSF media timeline role");
    ok &= expect_contains(msf_timeline_catalog_text, "\"packaging\":\"mediatimeline\"", "expected MSF media timeline packaging");
    ok &= expect_contains(msf_timeline_catalog_text, "\"mimeType\":\"application/json\"", "expected MSF media timeline mime type");
    ok &= expect_contains(msf_timeline_catalog_text, "\"depends\":[\"vide_1\",\"soun_2\"]",
                         "expected MSF media timeline to depend on all media tracks");
    ok &= expect(multitrack_plan.track_initializations.size() == 2, "expected per-track init payloads in plan");
    ok &= expect(multitrack_plan.objects.size() == 1, "expected only the catalog object in the default init-only plan");
    ok &= expect(multitrack_sap_plan.objects.size() == 3, "expected catalog plus per-track SAP timeline objects when SAP is enabled");
    ok &= expect(multitrack_msf_timeline_plan.objects.size() == 2,
                 "expected catalog plus one MSF media timeline object when MSF timeline is enabled");
    ok &= expect(multitrack_msf_timeline_plan.objects[1].track_name == "timeline",
                 "expected MSF media timeline object after catalog");
    ok &= expect_contains(object_text(multitrack_msf_timeline_plan.objects[1]), "[]",
                         "expected empty MSF media timeline for init-only plan");
    ok &= expect(multitrack_sap_plan.objects[1].track_name == "vide_1_sap", "expected video SAP object after catalog");
    ok &= expect(multitrack_sap_plan.objects[2].track_name == "soun_2_sap", "expected audio SAP object after catalog");
    ok &= expect_contains(object_text(multitrack_sap_plan.objects[1]), "[]", "expected empty SAP timeline for init-only video plan");
    ok &= expect_contains(object_text(multitrack_sap_plan.objects[2]), "[]", "expected empty SAP timeline for init-only audio plan");

    const auto video_init_bytes = base64_decode(video_init_data);
    const auto audio_init_bytes = base64_decode(audio_init_data);
    ok &= expect(multitrack_plan.track_initializations[0].init_segment == video_init_bytes,
                 "expected emitted video init segment to match catalog initData");
    ok &= expect(multitrack_plan.track_initializations[1].init_segment == audio_init_bytes,
                 "expected emitted audio init segment to match catalog initData");
    const auto video_init_boxes = parse_mp4_boxes(multitrack_plan.track_initializations[0].init_segment);
    const auto audio_init_boxes = parse_mp4_boxes(multitrack_plan.track_initializations[1].init_segment);
    const auto video_init_tracks = extract_tracks(video_init_boxes, multitrack_plan.track_initializations[0].init_segment);
    const auto audio_init_tracks = extract_tracks(audio_init_boxes, multitrack_plan.track_initializations[1].init_segment);
    ok &= expect(video_init_tracks.size() == 1, "expected one track in emitted video init segment");
    ok &= expect(audio_init_tracks.size() == 1, "expected one track in emitted audio init segment");
    ok &= expect(video_init_tracks.front().codec == "avc1.64000C", "expected avc1-only emitted video init segment");
    ok &= expect(audio_init_tracks.front().codec == "mp4a.40.2", "expected mp4a-only emitted audio init segment");
    ok &= expect(video_init_boxes.size() == 2, "expected video initData to contain top-level boxes");
    ok &= expect(audio_init_boxes.size() == 2, "expected audio initData to contain top-level boxes");
    ok &= expect(video_init_boxes[0].type == "ftyp" && video_init_boxes[1].type == "moov",
                 "expected video initData to contain ftyp and moov boxes");
    ok &= expect(audio_init_boxes[0].type == "ftyp" && audio_init_boxes[1].type == "moov",
                 "expected audio initData to contain ftyp and moov boxes");

    const auto live_catalog = build_live_catalog(multitrack_segmented.tracks, multitrack_init_bytes, true);
    const std::string live_catalog_text(live_catalog.catalog_payload.begin(), live_catalog.catalog_payload.end());
    const std::string live_video_init_data = msf_track_init_data(live_catalog_text, "vide_1");
    const std::string live_audio_init_data = msf_track_init_data(live_catalog_text, "soun_2");
    const auto live_video_init_bytes = base64_decode(live_video_init_data);
    const auto live_audio_init_bytes = base64_decode(live_audio_init_data);
    const auto live_video_init_tracks = extract_tracks(parse_mp4_boxes(live_video_init_bytes), live_video_init_bytes);
    const auto live_audio_init_tracks = extract_tracks(parse_mp4_boxes(live_audio_init_bytes), live_audio_init_bytes);
    ok &= expect(live_catalog.track_initializations.size() == 2, "expected live catalog to expose per-track init payloads");
    ok &= expect_contains(live_catalog_text, "\"version\":\"1\"",
                          "expected MSF v1 string version in live catalog");
    ok &= expect_contains(live_catalog_text, "\"isLive\":true",
                          "expected live tracks marked isLive");
    ok &= expect_contains(live_catalog_text, "\"generatedAt\":",
                          "expected generatedAt on a live catalog");
    ok &= expect_contains(live_catalog_text, "\"initDataList\"",
                          "expected root initDataList in live catalog");
    ok &= expect_not_contains(live_catalog_text, "\"format\"",
                              "expected no legacy format field in live catalog");
    ok &= expect_not_contains(live_catalog_text, "\"initData\":",
                              "expected no inline initData in live catalog");
    ok &= expect_not_contains(live_catalog_text, "\"trackDuration\"",
                              "expected no trackDuration in a live catalog");
    ok &= expect(all_init_refs_resolve(live_catalog_text),
                "expected every initRef in the live catalog to resolve to an initDataList id");
    ok &= expect(no_numeric_id_field(live_catalog_text),
                "expected no non-spec numeric id field in the live catalog");
    ok &= expect(!live_video_init_data.empty(), "expected live video initData in catalog");
    ok &= expect(!live_audio_init_data.empty(), "expected live audio initData in catalog");
    ok &= expect(live_video_init_data != live_audio_init_data, "expected live per-track initData entries to differ");
    ok &= expect(live_catalog.track_initializations[0].init_segment == live_video_init_bytes,
                 "expected live video init segment to match catalog initData");
    ok &= expect(live_catalog.track_initializations[1].init_segment == live_audio_init_bytes,
                 "expected live audio init segment to match catalog initData");
    ok &= expect(live_video_init_tracks.size() == 1, "expected one track in live video init segment");
    ok &= expect(live_audio_init_tracks.size() == 1, "expected one track in live audio init segment");
    ok &= expect(live_video_init_tracks.front().codec == "avc1.64000C", "expected live avc1-only init segment");
    ok &= expect(live_audio_init_tracks.front().codec == "mp4a.40.2", "expected live mp4a-only init segment");

    bool live_catalog_rejected_bad_track = false;
    try {
        std::vector<TrackDescription> bad_tracks = multitrack_segmented.tracks;
        bad_tracks.front().track_id = 99;
        (void)build_live_catalog(bad_tracks, multitrack_init_bytes, true);
    } catch (...) {
        live_catalog_rejected_bad_track = true;
    }
    ok &= expect(live_catalog_rejected_bad_track, "expected live catalog to reject missing track init data");

    const auto hevc_main_bytes = make_hevc_init_mp4(0x01, 0x60000000, {0xB0, 0x00, 0x00, 0x00, 0x00, 0x00}, 90);
    const auto hevc_high_bytes = make_hevc_init_mp4(0x22, 0x40000000, {0xB0, 0x00, 0x00, 0x00, 0x00, 0x00}, 150);
    const auto hevc_main_tracks = extract_tracks(parse_mp4_boxes(hevc_main_bytes), hevc_main_bytes);
    const auto hevc_high_tracks = extract_tracks(parse_mp4_boxes(hevc_high_bytes), hevc_high_bytes);
    ok &= expect(hevc_main_tracks.size() == 1, "expected one HEVC main-profile track");
    ok &= expect(hevc_high_tracks.size() == 1, "expected one HEVC high-tier track");
    ok &= expect(hevc_main_tracks.front().codec == "hev1.1.6.L90.B0",
                 "expected compact RFC 6381 HEVC main-profile codec string");
    ok &= expect(hevc_high_tracks.front().codec == "hev1.2.4.H150.B0",
                 "expected compact RFC 6381 HEVC high-tier codec string");

    const auto hevc_slice_only_bytes =
        make_fragmented_hevc_mp4(hevc_length_prefixed_sample({{0x26, 0x01, 0x80, 0x00}}));
    const auto hevc_inband_param_bytes =
        make_fragmented_hevc_mp4(hevc_length_prefixed_sample({{0x40, 0x01, 0x0C}, {0x26, 0x01, 0x80, 0x00}}));
    std::stringstream hevc_slice_only_stream(std::ios::in | std::ios::out | std::ios::binary);
    hevc_slice_only_stream.write(reinterpret_cast<const char*>(hevc_slice_only_bytes.data()),
                                 static_cast<std::streamsize>(hevc_slice_only_bytes.size()));
    hevc_slice_only_stream.seekg(0, std::ios::beg);
    const ParsedMp4 parsed_hevc_slice_only = parse_mp4_stream(hevc_slice_only_stream, "hevc-slice-only");
    ok &= expect(parsed_hevc_slice_only.tracks.size() == 1, "expected one HEVC track in slice-only fragmented MP4");
    ok &= expect(parsed_hevc_slice_only.tracks.front().sample_entry_type == "hvc1",
                 "expected hev1 source without in-band parameter sets to normalize to hvc1");
    ok &= expect(parsed_hevc_slice_only.tracks.front().codec == "hvc1.1.6.L90.B0",
                 "expected normalized HEVC codec string to advertise hvc1");

    const auto normalized_segmented = segment_for_cmaf(parsed_hevc_slice_only);
    const auto normalized_init_tracks =
        extract_tracks(parse_mp4_boxes(normalized_segmented.initialization_segment.owned_bytes),
                       normalized_segmented.initialization_segment.owned_bytes);
    ok &= expect(normalized_init_tracks.size() == 1, "expected one normalized HEVC track in init segment");
    ok &= expect(normalized_init_tracks.front().sample_entry_type == "hvc1",
                 "expected emitted init segment sample entry to be rewritten to hvc1");

    std::stringstream hevc_inband_param_stream(std::ios::in | std::ios::out | std::ios::binary);
    hevc_inband_param_stream.write(reinterpret_cast<const char*>(hevc_inband_param_bytes.data()),
                                   static_cast<std::streamsize>(hevc_inband_param_bytes.size()));
    hevc_inband_param_stream.seekg(0, std::ios::beg);
    const ParsedMp4 parsed_hevc_inband_param = parse_mp4_stream(hevc_inband_param_stream, "hevc-inband-param");
    ok &= expect(parsed_hevc_inband_param.tracks.size() == 1, "expected one HEVC track in in-band-param fragmented MP4");
    ok &= expect(parsed_hevc_inband_param.tracks.front().sample_entry_type == "hev1",
                 "expected in-band parameter sets to preserve hev1");
    ok &= expect(parsed_hevc_inband_param.tracks.front().codec == "hev1.1.6.L90.B0",
                 "expected in-band parameter set stream to keep hev1 codec string");

    // MSF section 5.2.22 requires bitrate; it comes from btrt when present.
    // MSF section 5.2.32 requires language, decoded from the packed mdhd field.
    // Timescale/duration are deliberately non-identity (2000/5000 -> 2500ms)
    // so a conversion that ignored timescale entirely could not pass by
    // accident (a bug caught in review round 1).
    {
        const auto btrt_bytes = make_track_metadata_test_mp4(true, pack_mdhd_language("eng"), 2000, 5000);
        const auto btrt_tracks = extract_tracks(parse_mp4_boxes(btrt_bytes), btrt_bytes);
        ok &= expect(btrt_tracks.size() == 1, "expected one track in btrt fixture");
        ok &= expect(btrt_tracks.front().max_bitrate == 5000000,
                     "expected maxBitrate parsed from btrt");
        ok &= expect(btrt_tracks.front().avg_bitrate == 4000000,
                     "expected avgBitrate parsed from btrt");
        ok &= expect(btrt_tracks.front().language == "en",
                     "expected ISO-639-2/T mdhd language mapped to its BCP 47 (ISO 639-1) form");
        ok &= expect(btrt_tracks.front().duration_ms == 2500,
                     "expected v0 mdhd duration converted to milliseconds using track timescale");

        const auto und_bytes = make_track_metadata_test_mp4(true, pack_mdhd_language("und"), 2000, 5000);
        const auto und_tracks = extract_tracks(parse_mp4_boxes(und_bytes), und_bytes);
        ok &= expect(und_tracks.size() == 1, "expected one track in und-language fixture");
        ok &= expect(und_tracks.front().language.empty(),
                     "expected und language code to decode to an empty string");

        const auto no_btrt_bytes = make_track_metadata_test_mp4(false, pack_mdhd_language("eng"), 2000, 5000);
        const auto no_btrt_tracks = extract_tracks(parse_mp4_boxes(no_btrt_bytes), no_btrt_bytes);
        ok &= expect(no_btrt_tracks.size() == 1, "expected one track in no-btrt fixture");
        ok &= expect(no_btrt_tracks.front().max_bitrate == 0,
                     "expected zero maxBitrate when btrt is absent");
        ok &= expect(no_btrt_tracks.front().avg_bitrate == 0,
                     "expected zero avgBitrate when btrt is absent");

        // Garbage-packed language bits (all 15 bits set -> three 0x1F groups,
        // decoding to '\x7F' which is outside 'a'..'z') must be rejected
        // rather than surfacing as garbage characters.
        const auto garbage_lang_bytes = make_track_metadata_test_mp4(false, 0x7FFF, 2000, 5000);
        const auto garbage_lang_tracks = extract_tracks(parse_mp4_boxes(garbage_lang_bytes), garbage_lang_bytes);
        ok &= expect(garbage_lang_tracks.size() == 1, "expected one track in garbage-language fixture");
        ok &= expect(garbage_lang_tracks.front().language.empty(),
                     "expected non-letter packed language bits to decode to an empty string");

        // mdhd version 1 (64-bit duration) was previously untested; exercise
        // its language and duration-conversion branches with a distinct,
        // non-identity timescale/duration pair (3000/9000 -> 3000ms).
        const auto v1_bytes = make_track_metadata_test_mp4(true, pack_mdhd_language("fra"), 3000, 9000, 1);
        const auto v1_tracks = extract_tracks(parse_mp4_boxes(v1_bytes), v1_bytes);
        ok &= expect(v1_tracks.size() == 1, "expected one track in v1 mdhd fixture");
        ok &= expect(v1_tracks.front().language == "fr",
                     "expected version-1 mdhd language to decode and map fra (639-2/T) to fr (BCP 47)");
        ok &= expect(v1_tracks.front().duration_ms == 3000,
                     "expected version-1 mdhd duration converted to milliseconds using track timescale");

        // A language with no ISO 639-1 two-letter code (Hawaiian) must pass
        // through unchanged: RFC 5646 permits a three-letter code when no
        // shorter form exists, so this is not a bug in the mapping.
        const auto no_two_letter_bytes = make_track_metadata_test_mp4(true, pack_mdhd_language("haw"), 2000, 5000);
        const auto no_two_letter_tracks = extract_tracks(parse_mp4_boxes(no_two_letter_bytes), no_two_letter_bytes);
        ok &= expect(no_two_letter_tracks.size() == 1, "expected one track in no-two-letter-code fixture");
        ok &= expect(no_two_letter_tracks.front().language == "haw",
                     "expected a language with no ISO 639-1 code (haw) to pass through unchanged");

        // A pathological version-1 duration near UINT64_MAX with a small
        // timescale must not silently wrap uint64_t during the *1000
        // conversion; it must fall back to the "unknown" (0) sentinel.
        const auto v1_overflow_bytes =
            make_track_metadata_test_mp4(true, pack_mdhd_language("eng"), 1,
                                         std::numeric_limits<std::uint64_t>::max(), 1);
        const auto v1_overflow_tracks = extract_tracks(parse_mp4_boxes(v1_overflow_bytes), v1_overflow_bytes);
        ok &= expect(v1_overflow_tracks.size() == 1, "expected one track in v1 overflow fixture");
        ok &= expect(v1_overflow_tracks.front().duration_ms == 0,
                     "expected pathological v1 duration*1000 overflow to fall back to 0, not wrap");
    }

    // I3 regression: bitrate_from_sample_entry (via find_child_box_span,
    // which superseded the sliding find_child_box_offset scan) must not
    // trust a fabricated sample-entry size and walk past the end of the
    // buffer when the searched-for child box (btrt) is absent. See
    // make_oversized_sample_entry_test_mp4 for the full explanation.
    {
        const auto oversized_bytes = make_oversized_sample_entry_test_mp4();
        const auto oversized_tracks = extract_tracks(parse_mp4_boxes(oversized_bytes), oversized_bytes);
        ok &= expect(oversized_tracks.size() == 1,
                    "expected extract_tracks to survive a fabricated oversized sample-entry size");
        ok &= expect(oversized_tracks.front().codec == "avc1.64000C",
                    "expected avcC (found early) to still decode correctly despite the fabricated bound");
        ok &= expect(oversized_tracks.front().max_bitrate == 0,
                    "expected no bitrate parsed when btrt is unreachable inside the fabricated bound");
    }

    // Heap over-read regression: mpeg4_audio_codec_string must not trust a
    // fabricated sample-entry size and scan past the end of the buffer when
    // no DecSpecificInfo (tag 0x05) byte is present. See
    // make_oversized_audio_sample_entry_test_mp4 for the full explanation.
    {
        const auto oversized_audio_bytes = make_oversized_audio_sample_entry_test_mp4();
        const auto oversized_audio_tracks = extract_tracks(parse_mp4_boxes(oversized_audio_bytes), oversized_audio_bytes);
        ok &= expect(oversized_audio_tracks.size() == 1,
                    "expected extract_tracks to survive a fabricated oversized mp4a sample-entry size");
        ok &= expect(oversized_audio_tracks.front().codec == "mp4a.40.2",
                    "expected the default AAC-LC codec string when no ESDS config is reachable within the fabricated bound");
    }

    // CMSF section 4: an encv sample entry must report the pre-encryption
    // codec string (resolved through frma), not the encv wrapper itself,
    // while sample_entry_type keeps the raw encv/enca type so a consumer can
    // still tell the track is protected.
    {
        const auto encrypted_bytes = make_encrypted_fragmented_test_mp4();
        const auto enc_tracks = extract_tracks(parse_mp4_boxes(encrypted_bytes), encrypted_bytes);
        ok &= expect(enc_tracks.size() == 1, "expected one track in the encrypted fixture");
        ok &= expect(enc_tracks.front().sample_entry_type == "encv",
                     "expected the raw sample entry type preserved");
        ok &= expect(enc_tracks.front().codec == "avc1.64000C",
                     "expected the codec string resolved through frma, with avcC profile bytes read "
                     "through the effective sample entry, not left as encv");
        // C1 regression: extract_tracks previously gated the width/height
        // extraction on the raw sample_entry_type ("encv"/"enca" for every
        // encrypted track), so an encrypted track's geometry silently came
        // out as 0x0 despite encv being byte-for-byte a VisualSampleEntry.
        // The fixture's visual_header (see make_encrypted_fragmented_test_mp4)
        // encodes 320x240 at offsets 24/26.
        ok &= expect(enc_tracks.front().width == 320,
                     "expected width read from an encv (encrypted) VisualSampleEntry, not left as 0");
        ok &= expect(enc_tracks.front().height == 240,
                     "expected height read from an encv (encrypted) VisualSampleEntry, not left as 0");
        ok &= expect(enc_tracks.front().protection.has_value(),
                     "expected protection parameters on an encrypted track");
        ok &= expect(enc_tracks.front().protection->scheme == "cenc",
                     "expected the cenc scheme recorded");
        ok &= expect(enc_tracks.front().protection->original_format == "avc1",
                     "expected the frma original_format recorded");
    }

    // C1 regression, audio side: an enca sample entry must report its real
    // channel_count/sample_rate, not 0. See make_encrypted_audio_fragmented_test_mp4
    // for why 0 is a wrong value on the wire (validate_track makes both
    // fields MUST-present for a sound track), not merely a missing one.
    {
        const auto encrypted_audio_bytes = make_encrypted_audio_fragmented_test_mp4();
        const auto enc_audio_tracks = extract_tracks(parse_mp4_boxes(encrypted_audio_bytes), encrypted_audio_bytes);
        ok &= expect(enc_audio_tracks.size() == 1, "expected one track in the encrypted audio fixture");
        ok &= expect(enc_audio_tracks.front().sample_entry_type == "enca",
                     "expected the raw enca sample entry type preserved");
        ok &= expect(enc_audio_tracks.front().channel_count == 2,
                     "expected channel_count read from an enca (encrypted) AudioSampleEntry, not left as 0");
        ok &= expect(enc_audio_tracks.front().sample_rate == 44100,
                     "expected sample_rate read from an enca (encrypted) AudioSampleEntry, not left as 0");
        ok &= expect(enc_audio_tracks.front().protection.has_value(),
                     "expected protection parameters on the encrypted audio track");
    }

    // C2 regression: a protected track (sinf/schm/schi/tenc present) whose
    // init segment carries no pssh anywhere must not silently publish as
    // unprotected. CMSF 4.1.2 -- "When this field is absent, the track
    // content is not protected by Common Encryption" -- so publishing
    // contentProtectionRefIDs-less output for genuinely encrypted content
    // would affirmatively assert the opposite of the truth. pssh is only
    // SHOULD-present (CMSF 4.1.1.4.5); ffmpeg's -encryption_scheme
    // cenc-aes-ctr is a real encoder that omits it entirely.
    {
        const auto no_pssh_bytes = make_encrypted_fragmented_test_mp4(/*include_pssh=*/false);
        ParsedMp4 no_pssh_parsed{
            .bytes = no_pssh_bytes,
            .top_level_boxes = parse_mp4_boxes(no_pssh_bytes),
            .tracks = {},
        };
        no_pssh_parsed.tracks = extract_tracks(no_pssh_parsed.top_level_boxes, no_pssh_parsed.bytes);
        ok &= expect(no_pssh_parsed.tracks.size() == 1, "expected one track in the no-pssh encrypted fixture");
        ok &= expect(no_pssh_parsed.tracks.front().protection.has_value(),
                     "expected the no-pssh fixture's track to still be detected as protected");
        const auto no_pssh_segmented = segment_for_cmaf(no_pssh_parsed);

        bool threw = false;
        std::string what;
        try {
            (void)build_publish_plan(no_pssh_segmented, DraftVersion::kDraft14,
                                     /*include_sap=*/false, /*include_msf_timeline=*/false,
                                     /*vod=*/false);
        } catch (const std::exception& e) {
            threw = true;
            what = e.what();
        }
        ok &= expect(threw,
                    "expected build_publish_plan to refuse a protected track with no pssh systems "
                    "found, rather than publish an unprotected-looking catalog");
        ok &= expect_contains(what, "vide_1",
                              "expected the thrown message to name the offending track");
    }

    // Task 6: build_publish_plan wires the pssh-derived DRM systems and each
    // protected track's CencTrackProtection into the catalog's
    // contentProtections, and applies deployment-configured fields (laURL,
    // etc.) only to systems the media actually carries a pssh for. A
    // configured system with no matching pssh must be ignored, not emitted.
    {
        const auto encrypted_bytes = make_encrypted_fragmented_test_mp4();
        ParsedMp4 encrypted{
            .bytes = encrypted_bytes,
            .top_level_boxes = parse_mp4_boxes(encrypted_bytes),
            .tracks = {},
        };
        encrypted.tracks = extract_tracks(encrypted.top_level_boxes, encrypted.bytes);
        const auto encrypted_segmented = segment_for_cmaf(encrypted);

        DrmSystemConfig widevine_config;
        widevine_config.system_id = "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed";
        widevine_config.la_url = "https://widevine.example.com/proxy";
        widevine_config.la_url_type = "widevine";
        widevine_config.robustness = "SW_SECURE_CRYPTO";

        // PlayReady's system ID has no pssh box anywhere in this fixture;
        // configuring it must not fabricate a contentProtections entry for
        // protection the media does not carry.
        DrmSystemConfig playready_config;
        playready_config.system_id = "9a04f079-9840-4286-ab92-e65be0885f95";
        playready_config.la_url = "https://playready.example.com/proxy";

        const auto enc_plan = build_publish_plan(encrypted_segmented, DraftVersion::kDraft14,
                                                 /*include_sap=*/false, /*include_msf_timeline=*/false,
                                                 /*vod=*/false, {widevine_config, playready_config});

        ok &= expect(enc_plan.objects.front().track_name == "catalog",
                     "expected the encrypted plan's catalog object first");
        const std::string enc_catalog_text = object_text(enc_plan.objects.front());

        ok &= expect_contains(enc_catalog_text, "\"contentProtections\"",
                              "expected an encrypted input to emit contentProtections");
        ok &= expect_contains(enc_catalog_text, "\"contentProtectionRefIDs\"",
                              "expected the protected track to reference an entry");
        ok &= expect_contains(enc_catalog_text, "\"codec\":\"avc1.64000C\"",
                              "expected the exact frma-resolved codec, not encv or a truncated prefix match");
        ok &= expect_contains(enc_catalog_text, "\"systemID\":\"edef8ba9-79d6-4ace-a3c8-27dcd51d21ed\"",
                              "expected the Widevine system ID found via pssh");
        ok &= expect_contains(enc_catalog_text,
                              "\"laURL\":{\"url\":\"https://widevine.example.com/proxy\"",
                              "expected the configured laURL applied to the matched system");
        ok &= expect_not_contains(enc_catalog_text, "9a04f079-9840-4286-ab92-e65be0885f95",
                                  "expected the unmatched PlayReady config entry to be ignored, not emitted");

        // Without any drm_systems configuration at all, the pssh-derived
        // contentProtections entry must still appear (attach_content_protection
        // does not depend on configuration existing), just without laURL.
        const auto enc_plan_unconfigured =
            build_publish_plan(encrypted_segmented, DraftVersion::kDraft14, false, false, false);
        const std::string enc_catalog_text_unconfigured = object_text(enc_plan_unconfigured.objects.front());
        ok &= expect_contains(enc_catalog_text_unconfigured, "\"contentProtections\"",
                              "expected contentProtections even with no DRM deployment configuration");
        ok &= expect_not_contains(enc_catalog_text_unconfigured, "\"laURL\"",
                                  "expected no laURL when no DRM configuration was supplied");
    }

    // CMSF section 3.5.2: maxGrpSapStartingType is a maximum over only the
    // first Object of each Group (object_id == 0), while maxObjSapStartingType
    // is a maximum over every Object of the track. Use a fixture where a
    // mid-group Object's SAP type exceeds the group-starting Object's SAP type
    // so the two fields genuinely differ -- if they were transposed, this is
    // the case that would catch it.
    {
        const SegmentedMp4 sap_levels_segmented{
            .initialization_segment = {.span = {}, .owned_bytes = multitrack_init_bytes},
            .fragments =
                {
                    MediaFragment{
                        .group_id = 0,
                        .object_id = 0,
                        .track_name = "vide_1",
                        .sap_type = 2,
                        .has_sap_type = true,
                    },
                    MediaFragment{
                        .group_id = 0,
                        .object_id = 1,
                        .track_name = "vide_1",
                        .sap_type = 3,
                        .has_sap_type = true,
                    },
                },
            .tracks =
                {
                    TrackDescription{.track_id = 1,
                                     .handler_type = "vide",
                                     .codec = "avc1.64000C",
                                     .sample_entry_type = "avc1",
                                     .track_name = "vide_1",
                                     .width = 320,
                                     .height = 240},
                    TrackDescription{.track_id = 2,
                                     .handler_type = "soun",
                                     .codec = "mp4a.40.2",
                                     .sample_entry_type = "mp4a",
                                     .track_name = "soun_2",
                                     .channel_count = 2,
                                     .sample_rate = 48000},
                },
        };
        const auto sap_levels_plan = build_publish_plan(sap_levels_segmented, DraftVersion::kDraft14);
        const std::string sap_levels_catalog_text = object_text(sap_levels_plan.objects.front());

        ok &= expect_contains(sap_levels_catalog_text, "\"maxGrpSapStartingType\":2",
                              "expected maxGrpSapStartingType to reflect only the first Object of each Group");
        ok &= expect_contains(sap_levels_catalog_text, "\"maxObjSapStartingType\":3",
                              "expected maxObjSapStartingType to reflect the maximum across all Objects");

        const long long grp_value = extract_uint_value(sap_levels_catalog_text, "\"maxGrpSapStartingType\":");
        const long long obj_value = extract_uint_value(sap_levels_catalog_text, "\"maxObjSapStartingType\":");
        ok &= expect(grp_value >= 0 && obj_value >= 0,
                    "expected both SAP starting type fields to be present in the emitted catalog");
        ok &= expect(grp_value <= obj_value,
                    "expected maxGrpSapStartingType <= maxObjSapStartingType since Grp is a maximum over a "
                    "subset of the fragments Obj maximizes over");

        // soun_2 has no fragments at all, so it must carry neither field --
        // both are Optional per CMSF section 3.5.2 and MUST NOT default to 0.
        const std::string soun_track_text =
            sap_levels_catalog_text.substr(sap_levels_catalog_text.find("\"name\":\"soun_2\""));
        ok &= expect_not_contains(soun_track_text, "\"maxGrpSapStartingType\":",
                                  "expected no maxGrpSapStartingType on a track with no SAP information");
        ok &= expect_not_contains(soun_track_text, "\"maxObjSapStartingType\":",
                                  "expected no maxObjSapStartingType on a track with no SAP information");
    }

    // --- Task 7: saio offset correction on the CTE (live) ingest path ---
    //
    // build_live_fragment (via materialize_live_trun_defaults) rebuilds each
    // live moof's trun into a fully-explicit form, which changes the moof's
    // size. saio holds moof-relative byte offsets into senc/mdat that a
    // decryptor needs, so it must shift by exactly that size delta or the
    // decryptor silently reads the wrong bytes. Every expected offset below
    // is computed from the fixture's own known byte layout, never by asking
    // the code under test what its answer was.
    {
        const std::vector<TrackDescription> saio_tracks = {
            TrackDescription{.track_id = 1, .handler_type = "vide", .track_name = "vide_1", .timescale = 1000},
        };

        // Case 1: version 0, no aux_info_type, a moof-relative offset that
        // lands inside senc (i.e. at or beyond the original trun's end) --
        // the only offsets a rebuild that changes only trun's size can move.
        {
            constexpr std::uint8_t version = 0;
            constexpr bool has_aux = false;
            const std::size_t saio_size = saio_box_size(version, has_aux, 1);
            const std::size_t original_moof_size = 8 + 8 + kSaioFixtureTfhdSize + kSaioFixtureOriginalTrunSize +
                                                   kSaioFixtureSencSize + kSaioFixtureSaizSize + saio_size;
            const std::uint64_t original_offset = kSaioFixtureTrunEnd + 2;
            ok &= expect(original_offset >= kSaioFixtureTrunEnd && original_offset < original_moof_size,
                        "case1 fixture sanity: offset must land at or after the original trun's end (in senc)");

            const auto saio_box = make_saio_box(version, has_aux, {original_offset});
            const auto fx = make_saio_test_fragment(saio_box, 32);
            ok &= expect(fx.original_moof_size == original_moof_size,
                        "case1: hand-computed moof size must match the fixture's actual size");

            const auto fragment = build_live_fragment(fx.moof, fx.mdat, saio_tracks, 0);
            const auto out_boxes = parse_mp4_boxes(fragment.payload.owned_bytes);
            const Mp4Box* out_traf = out_boxes.empty() ? nullptr : find_child_box(out_boxes.front(), "traf");
            const Mp4Box* out_saio = out_traf == nullptr ? nullptr : find_child_box(*out_traf, "saio");
            ok &= expect(out_saio != nullptr, "case1: expected saio to survive in the corrected output");
            if (out_saio != nullptr) {
                const auto corrected = extract_saio_offsets(fragment.payload.owned_bytes, *out_saio, has_aux);
                const std::uint64_t expected = original_offset + static_cast<std::uint64_t>(fx.expected_delta);
                ok &= expect(corrected.size() == 1 && corrected.front() == expected,
                            "case1: expected saio offset " + std::to_string(original_offset) + " corrected to " +
                                std::to_string(expected) + " (delta " + std::to_string(fx.expected_delta) + ")");
            }
        }

        // Case 2: version 1, 64-bit offsets.
        {
            constexpr std::uint8_t version = 1;
            constexpr bool has_aux = false;
            const std::size_t saio_size = saio_box_size(version, has_aux, 1);
            const std::size_t original_moof_size = 8 + 8 + kSaioFixtureTfhdSize + kSaioFixtureOriginalTrunSize +
                                                   kSaioFixtureSencSize + kSaioFixtureSaizSize + saio_size;
            const std::uint64_t original_offset = 64;
            ok &= expect(original_offset >= kSaioFixtureTrunEnd && original_offset < original_moof_size,
                        "case2 fixture sanity: offset must land at or after the original trun's end (in senc)");

            const auto saio_box = make_saio_box(version, has_aux, {original_offset});
            const auto fx = make_saio_test_fragment(saio_box, 32);
            ok &= expect(fx.original_moof_size == original_moof_size,
                        "case2: hand-computed moof size must match the fixture's actual size");

            const auto fragment = build_live_fragment(fx.moof, fx.mdat, saio_tracks, 0);
            const auto out_boxes = parse_mp4_boxes(fragment.payload.owned_bytes);
            const Mp4Box* out_traf = out_boxes.empty() ? nullptr : find_child_box(out_boxes.front(), "traf");
            const Mp4Box* out_saio = out_traf == nullptr ? nullptr : find_child_box(*out_traf, "saio");
            ok &= expect(out_saio != nullptr, "case2: expected saio to survive in the corrected output");
            if (out_saio != nullptr) {
                ok &= expect(fragment.payload.owned_bytes[out_saio->payload.offset] == 1,
                            "case2: expected the saio box to keep version 1");
                const auto corrected = extract_saio_offsets(fragment.payload.owned_bytes, *out_saio, has_aux);
                const std::uint64_t expected = original_offset + static_cast<std::uint64_t>(fx.expected_delta);
                ok &= expect(corrected.size() == 1 && corrected.front() == expected,
                            "case2: expected 64-bit saio offset " + std::to_string(original_offset) +
                                " corrected to " + std::to_string(expected) + " (delta " +
                                std::to_string(fx.expected_delta) + ")");
            }
        }

        // Case 3: flags & 1 set, so the aux_info_type/aux_info_type_parameter
        // prefix is present and shifts where entry_count and the offsets
        // begin. An implementation that ignores the flag reads entry_count
        // from the wrong place and would produce a differently-wrong number
        // here, not merely fail to change it.
        {
            constexpr std::uint8_t version = 0;
            constexpr bool has_aux = true;
            const std::size_t saio_size = saio_box_size(version, has_aux, 1);
            const std::size_t original_moof_size = 8 + 8 + kSaioFixtureTfhdSize + kSaioFixtureOriginalTrunSize +
                                                   kSaioFixtureSencSize + kSaioFixtureSaizSize + saio_size;
            const std::uint64_t original_offset = 70;
            ok &= expect(original_offset >= kSaioFixtureTrunEnd && original_offset < original_moof_size,
                        "case3 fixture sanity: offset must land at or after the original trun's end (in senc)");

            const auto saio_box = make_saio_box(version, has_aux, {original_offset});
            const auto fx = make_saio_test_fragment(saio_box, 32);
            ok &= expect(fx.original_moof_size == original_moof_size,
                        "case3: hand-computed moof size must match the fixture's actual size");

            const auto fragment = build_live_fragment(fx.moof, fx.mdat, saio_tracks, 0);
            const auto out_boxes = parse_mp4_boxes(fragment.payload.owned_bytes);
            const Mp4Box* out_traf = out_boxes.empty() ? nullptr : find_child_box(out_boxes.front(), "traf");
            const Mp4Box* out_saio = out_traf == nullptr ? nullptr : find_child_box(*out_traf, "saio");
            ok &= expect(out_saio != nullptr, "case3: expected saio to survive in the corrected output");
            if (out_saio != nullptr) {
                const auto corrected = extract_saio_offsets(fragment.payload.owned_bytes, *out_saio, has_aux);
                const std::uint64_t expected = original_offset + static_cast<std::uint64_t>(fx.expected_delta);
                ok &= expect(corrected.size() == 1 && corrected.front() == expected,
                            "case3: expected saio offset " + std::to_string(original_offset) +
                                " (with aux_info_type prefix) corrected to " + std::to_string(expected) +
                                " (delta " + std::to_string(fx.expected_delta) + ")");
            }
        }

        // Case 4: an mdat-relative offset (at or beyond the original moof
        // size, but within moof + mdat) is corrected, not refused.
        {
            constexpr std::uint8_t version = 0;
            constexpr bool has_aux = false;
            const std::size_t saio_size = saio_box_size(version, has_aux, 1);
            const std::size_t original_moof_size = 8 + 8 + kSaioFixtureTfhdSize + kSaioFixtureOriginalTrunSize +
                                                   kSaioFixtureSencSize + kSaioFixtureSaizSize + saio_size;
            const std::size_t mdat_payload_size = 64;
            const std::uint64_t original_offset = static_cast<std::uint64_t>(original_moof_size) + 4;
            ok &= expect(original_offset >= original_moof_size &&
                            original_offset < original_moof_size + 8 + mdat_payload_size,
                        "case4 fixture sanity: offset must land inside mdat");

            const auto saio_box = make_saio_box(version, has_aux, {original_offset});
            const auto fx = make_saio_test_fragment(saio_box, mdat_payload_size);
            ok &= expect(fx.original_moof_size == original_moof_size,
                        "case4: hand-computed moof size must match the fixture's actual size");

            const auto fragment = build_live_fragment(fx.moof, fx.mdat, saio_tracks, 0);
            const auto out_boxes = parse_mp4_boxes(fragment.payload.owned_bytes);
            const Mp4Box* out_traf = out_boxes.empty() ? nullptr : find_child_box(out_boxes.front(), "traf");
            const Mp4Box* out_saio = out_traf == nullptr ? nullptr : find_child_box(*out_traf, "saio");
            ok &= expect(out_saio != nullptr, "case4: expected saio to survive in the corrected output");
            if (out_saio != nullptr) {
                const auto corrected = extract_saio_offsets(fragment.payload.owned_bytes, *out_saio, has_aux);
                const std::uint64_t expected = original_offset + static_cast<std::uint64_t>(fx.expected_delta);
                ok &= expect(corrected.size() == 1 && corrected.front() == expected,
                            "case4: expected mdat-relative saio offset " + std::to_string(original_offset) +
                                " corrected to " + std::to_string(expected) + " (delta " +
                                std::to_string(fx.expected_delta) + ")");
            }
        }

        // Case 5: an absolute-looking offset (beyond moof + mdat) is
        // refused, with a fixture that is otherwise entirely valid so the
        // refusal is attributable only to the classification.
        {
            constexpr std::uint8_t version = 0;
            constexpr bool has_aux = false;
            const std::size_t saio_size = saio_box_size(version, has_aux, 1);
            const std::size_t original_moof_size = 8 + 8 + kSaioFixtureTfhdSize + kSaioFixtureOriginalTrunSize +
                                                   kSaioFixtureSencSize + kSaioFixtureSaizSize + saio_size;
            const std::size_t mdat_payload_size = 32;
            const std::size_t mdat_total_size = 8 + mdat_payload_size;
            const std::uint64_t original_offset =
                static_cast<std::uint64_t>(original_moof_size + mdat_total_size) + 1000;

            const auto saio_box = make_saio_box(version, has_aux, {original_offset});
            const auto fx = make_saio_test_fragment(saio_box, mdat_payload_size);
            ok &= expect(fx.original_moof_size == original_moof_size,
                        "case5: hand-computed moof size must match the fixture's actual size");
            ok &= expect(fx.mdat.size() == mdat_total_size, "case5: hand-computed mdat size must match the fixture");

            bool threw = false;
            std::string what;
            try {
                (void)build_live_fragment(fx.moof, fx.mdat, saio_tracks, 0);
            } catch (const std::exception& e) {
                threw = true;
                what = e.what();
            }
            ok &= expect(threw, "case5: expected an absolute-looking saio offset to be refused, not adjusted");
            ok &= expect_contains(what, std::to_string(original_offset),
                                  "case5: expected the thrown message to name the offending offset");
        }

        // Case 6: an unencrypted fragment (no saio/senc/saiz at all) passes
        // through unchanged aside from the pre-existing trun normalization --
        // the regression guard for every existing CTE user.
        {
            const auto fx = make_saio_test_fragment(/*saio_box=*/{}, /*mdat_payload_size=*/16,
                                                    /*include_senc=*/false, /*include_saiz=*/false);
            const auto fragment = build_live_fragment(fx.moof, fx.mdat, saio_tracks, 0);

            const std::size_t placeholder_moof_size =
                8 + 8 + kSaioFixtureTfhdSize + kSaioFixtureNormalizedTrunSize;
            const std::uint32_t expected_data_offset = static_cast<std::uint32_t>(placeholder_moof_size + 8);

            std::vector<std::uint8_t> expected_trun_payload;
            append_be32(expected_trun_payload, 1);                    // sample_count
            append_be32(expected_trun_payload, expected_data_offset); // data_offset
            append_be32(expected_trun_payload, 1000);                 // duration (tfhd default)
            append_be32(expected_trun_payload, 200);                  // size (tfhd default)
            append_be32(expected_trun_payload, 0x02000000U);          // flags (tfhd default)
            const auto expected_trun = make_full_box_with_flags("trun", 0, 0x000701U, expected_trun_payload);

            const auto expected_traf = make_box("traf", concat({fx.tfhd, expected_trun}));
            const auto expected_moof = make_box("moof", expected_traf);
            const auto expected_payload = concat({expected_moof, fx.mdat});

            ok &= expect(fragment.payload.owned_bytes == expected_payload,
                        "case6: expected an unencrypted live fragment to pass through byte-for-byte "
                        "unchanged aside from the pre-existing trun normalization");
        }

        // Case 7: a fabricated entry_count far larger than the box can hold
        // must be refused before any entry is read, not drive a
        // bounds-violating read. This is the sole barrier between an
        // untrusted 32-bit file field and an out-of-bounds read.
        {
            constexpr std::uint8_t version = 0;
            constexpr bool has_aux = false;
            // No actual offset entries follow -- entry_count alone claims far
            // more than the box (or the whole moof) could ever hold.
            const auto saio_box =
                make_saio_box_with_declared_entry_count(version, has_aux, 0x0FFFFFFFU, {});
            const auto fx = make_saio_test_fragment(saio_box, 32);

            bool threw = false;
            std::string what;
            try {
                (void)build_live_fragment(fx.moof, fx.mdat, saio_tracks, 0);
            } catch (const std::exception& e) {
                threw = true;
                what = e.what();
            }
            ok &= expect(threw, "case7: expected a fabricated entry_count to be refused before any "
                                "entry is read");
            ok &= expect_contains(what, "entry_count",
                                  "case7: expected the thrown message to name the entry_count problem");
        }

        // Case 8: a saio present with neither senc nor saiz in the same traf
        // is malformed -- there is nothing to validate the offset
        // classification against -- and must be refused.
        {
            constexpr std::uint8_t version = 0;
            constexpr bool has_aux = false;
            const auto saio_box = make_saio_box(version, has_aux, {50});
            const auto fx = make_saio_test_fragment(saio_box, 32, /*include_senc=*/false,
                                                     /*include_saiz=*/false);

            bool threw = false;
            std::string what;
            try {
                (void)build_live_fragment(fx.moof, fx.mdat, saio_tracks, 0);
            } catch (const std::exception& e) {
                threw = true;
                what = e.what();
            }
            ok &= expect(threw, "case8: expected a saio without senc or saiz in traf to be refused");
            ok &= expect_contains(what, "senc or saiz",
                                  "case8: expected the thrown message to name the missing senc/saiz");
        }

        // Case 9 (I1): senc placed BEFORE trun in the traf -- a legal
        // ISO/IEC 14496-12 ordering ffmpeg produces. rebuild_moof only ever
        // changes trun's own size, so an offset pointing into a senc that
        // sits entirely before trun (unlike case 1's senc, which sits after
        // trun) must NOT move. Applying delta unconditionally -- the bug --
        // would shift this offset to 66; verified by temporarily reverting
        // the trun_end guard in correct_saio_offsets and confirming this
        // case fails with corrected == 66 instead of 50.
        {
            constexpr std::uint8_t version = 0;
            constexpr bool has_aux = false;
            const std::size_t saio_size = saio_box_size(version, has_aux, 1);
            const std::size_t original_moof_size = 8 + 8 + kSaioFixtureTfhdSize + kSaioFixtureSencSize +
                                                   kSaioFixtureOriginalTrunSize + kSaioFixtureSaizSize + saio_size;
            // In this reordering, senc spans [44, 60) and trun spans
            // [60, 76); 50 lands inside senc, strictly before trun.
            const std::size_t senc_start = 8 + 8 + kSaioFixtureTfhdSize;
            const std::size_t reordered_trun_end = senc_start + kSaioFixtureSencSize + kSaioFixtureOriginalTrunSize;
            const std::uint64_t original_offset = 50;
            ok &= expect(original_offset >= senc_start && original_offset < senc_start + kSaioFixtureSencSize,
                        "case9 fixture sanity: offset must land inside senc");
            ok &= expect(original_offset < reordered_trun_end,
                        "case9 fixture sanity: offset must land before trun in this reordering");

            const auto saio_box = make_saio_box(version, has_aux, {original_offset});
            const auto fx = make_saio_test_fragment(saio_box, 32, /*include_senc=*/true, /*include_saiz=*/true,
                                                    /*senc_before_trun=*/true);
            ok &= expect(fx.original_moof_size == original_moof_size,
                        "case9: hand-computed moof size must match the fixture's actual size");

            const auto fragment = build_live_fragment(fx.moof, fx.mdat, saio_tracks, 0);
            const auto out_boxes = parse_mp4_boxes(fragment.payload.owned_bytes);
            const Mp4Box* out_traf = out_boxes.empty() ? nullptr : find_child_box(out_boxes.front(), "traf");
            const Mp4Box* out_saio = out_traf == nullptr ? nullptr : find_child_box(*out_traf, "saio");
            ok &= expect(out_saio != nullptr, "case9: expected saio to survive in the corrected output");
            if (out_saio != nullptr) {
                const auto corrected = extract_saio_offsets(fragment.payload.owned_bytes, *out_saio, has_aux);
                ok &= expect(corrected.size() == 1 && corrected.front() == original_offset,
                            "case9: expected a saio offset pointing into a senc BEFORE trun to stay " +
                                std::to_string(original_offset) + ", not shift by the trun-growth delta");
            }
        }

        // Case 10 (I2): a multi-traf moof where a traf OTHER than the first
        // (the only one materialize_live_trun_defaults rebuilds and
        // corrects saio inside) carries a saio. That saio's offsets are
        // computed against the moof's ORIGINAL size; copying that traf
        // verbatim after the first traf's rebuild changes the moof's size
        // would leave those offsets silently stale. Refuse instead.
        {
            std::vector<std::uint8_t> tfhd_a_payload;
            append_be32(tfhd_a_payload, 1);
            append_be32(tfhd_a_payload, 1000);
            append_be32(tfhd_a_payload, 200);
            append_be32(tfhd_a_payload, 0x02000000U);
            const auto tfhd_a = make_full_box_with_flags("tfhd", 0, 0x000038U, tfhd_a_payload);
            const auto trun_a = make_full_box_with_flags("trun", 0, 0, be32_bytes(1));
            const auto traf_a = make_box("traf", concat({tfhd_a, trun_a}));

            std::vector<std::uint8_t> tfhd_b_payload;
            append_be32(tfhd_b_payload, 2);
            append_be32(tfhd_b_payload, 1000);
            append_be32(tfhd_b_payload, 200);
            append_be32(tfhd_b_payload, 0x02000000U);
            const auto tfhd_b = make_full_box_with_flags("tfhd", 0, 0x000038U, tfhd_b_payload);
            const auto trun_b = make_full_box_with_flags("trun", 0, 0, be32_bytes(1));
            const auto senc_b = make_box("senc", {0, 0, 0, 0, 0, 0, 0, 0});
            const auto saiz_b = make_full_box("saiz", {8, 0, 0, 0, 1});
            const auto saio_b = make_saio_box(0, false, {50});
            const auto traf_b = make_box("traf", concat({tfhd_b, trun_b, senc_b, saiz_b, saio_b}));

            const auto moof = make_box("moof", concat({traf_a, traf_b}));
            const auto mdat = make_box("mdat", std::vector<std::uint8_t>(16, 0xAB));

            bool threw = false;
            std::string what;
            try {
                (void)build_live_fragment(moof, mdat, saio_tracks, 0);
            } catch (const std::exception& e) {
                threw = true;
                what = e.what();
            }
            ok &= expect(threw,
                        "case10: expected a saio in a non-first traf of a multi-traf moof to be refused");
            ok &= expect_contains(what, "traf",
                                  "case10: expected the thrown message to name the multi-traf limitation");
        }

        // Case 11 (I2 companion): a multi-traf moof where NO traf besides
        // the first carries a saio must keep working unchanged -- the
        // refusal in case 10 is scoped to saio specifically, not to
        // multiple trafs in general.
        {
            std::vector<std::uint8_t> tfhd_a_payload;
            append_be32(tfhd_a_payload, 1);
            append_be32(tfhd_a_payload, 1000);
            append_be32(tfhd_a_payload, 200);
            append_be32(tfhd_a_payload, 0x02000000U);
            const auto tfhd_a = make_full_box_with_flags("tfhd", 0, 0x000038U, tfhd_a_payload);
            const auto trun_a = make_full_box_with_flags("trun", 0, 0, be32_bytes(1));
            const auto traf_a = make_box("traf", concat({tfhd_a, trun_a}));

            std::vector<std::uint8_t> tfhd_b_payload;
            append_be32(tfhd_b_payload, 2);
            append_be32(tfhd_b_payload, 1000);
            append_be32(tfhd_b_payload, 200);
            append_be32(tfhd_b_payload, 0x02000000U);
            const auto tfhd_b = make_full_box_with_flags("tfhd", 0, 0x000038U, tfhd_b_payload);
            const auto trun_b = make_full_box_with_flags("trun", 0, 0, be32_bytes(1));
            const auto traf_b = make_box("traf", concat({tfhd_b, trun_b}));  // no senc/saiz/saio

            const auto moof = make_box("moof", concat({traf_a, traf_b}));
            const auto mdat = make_box("mdat", std::vector<std::uint8_t>(16, 0xAB));

            const auto fragment = build_live_fragment(moof, mdat, saio_tracks, 0);
            ok &= expect(fragment.track_name == "vide_1",
                        "case11: expected a multi-traf moof with no saio anywhere but the first traf "
                        "to still produce a fragment, not be refused");
        }
    }

    return ok ? 0 : 1;
}
