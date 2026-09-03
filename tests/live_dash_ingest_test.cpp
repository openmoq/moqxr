#include "openmoq/publisher/live_dash_ingest.h"
#include "openmoq/publisher/mp4_box.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using openmoq::publisher::ChunkedBodyDecoder;
using openmoq::publisher::LiveDashIngestConfig;
using openmoq::publisher::LiveDashIngestServer;
using openmoq::publisher::LiveDashIngestSession;
using openmoq::publisher::LiveObject;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

void append_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void append_ascii(std::vector<std::uint8_t>& out, std::string_view value) {
    out.insert(out.end(), value.begin(), value.end());
}

std::vector<std::uint8_t> make_box(std::string_view type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    append_be32(out, static_cast<std::uint32_t>(8 + payload.size()));
    append_ascii(out, type);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> concat(std::initializer_list<std::vector<std::uint8_t>> boxes) {
    std::vector<std::uint8_t> out;
    for (const auto& box : boxes) {
        out.insert(out.end(), box.begin(), box.end());
    }
    return out;
}

std::vector<std::uint8_t> full_box(std::string_view type,
                                   std::uint8_t version,
                                   std::uint32_t flags,
                                   const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> full_payload;
    full_payload.push_back(version);
    full_payload.push_back(static_cast<std::uint8_t>((flags >> 16U) & 0xFFU));
    full_payload.push_back(static_cast<std::uint8_t>((flags >> 8U) & 0xFFU));
    full_payload.push_back(static_cast<std::uint8_t>(flags & 0xFFU));
    full_payload.insert(full_payload.end(), payload.begin(), payload.end());
    return make_box(type, full_payload);
}

std::vector<std::uint8_t> be32(std::uint32_t value) {
    std::vector<std::uint8_t> out;
    append_be32(out, value);
    return out;
}

std::vector<std::uint8_t> be64(std::uint64_t value) {
    return concat({be32(static_cast<std::uint32_t>(value >> 32U)),
                   be32(static_cast<std::uint32_t>(value))});
}

std::uint32_t read_be32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::vector<std::uint8_t> make_init_segment(std::uint32_t track_id,
                                            std::string_view handler_type = "vide") {
    const bool is_audio = handler_type == "soun";
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = full_box("tkhd", 0, 0, concat({std::vector<std::uint8_t>(8, 0), be32(track_id), std::vector<std::uint8_t>(4, 0)}));
    const auto mdhd = full_box("mdhd", 0, 0, concat({std::vector<std::uint8_t>(8, 0), be32(is_audio ? 48000 : 1000), std::vector<std::uint8_t>(8, 0)}));
    const auto hdlr = full_box("hdlr", 0, 0,
                               is_audio
                                   ? std::vector<std::uint8_t>{0, 0, 0, 0, 's', 'o', 'u', 'n', 0, 0, 0, 0}
                                   : std::vector<std::uint8_t>{0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    std::vector<std::uint8_t> sample_entry;
    if (is_audio) {
        auto audio_header = std::vector<std::uint8_t>(28, 0);
        audio_header[16] = 0x00;
        audio_header[17] = 0x02;
        audio_header[24] = 0xbb;
        audio_header[25] = 0x80;
        sample_entry = make_box(
            "mp4a",
            concat({audio_header,
                    make_box("esds", {0x00, 0x00, 0x00, 0x00, 0x03, 0x19, 0x00, 0x02,
                                      0x00, 0x04, 0x11, 0x40, 0x15, 0x00, 0x00, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0x05, 0x02, 0x10, 0x10})}));
    } else {
        auto visual_header = std::vector<std::uint8_t>(78, 0);
        visual_header[24] = 0x01;
        visual_header[25] = 0x40;
        visual_header[26] = 0x00;
        visual_header[27] = 0xf0;
        sample_entry = make_box("avc1", concat({visual_header, make_box("avcC", {1, 100, 0, 12, 0xff})}));
    }
    const auto stsd = full_box("stsd", 0, 0, concat({be32(1), sample_entry}));
    const auto stbl = make_box("stbl", stsd);
    const auto minf = make_box("minf", stbl);
    const auto mdia = make_box("mdia", concat({mdhd, hdlr, minf}));
    const auto trak = make_box("trak", concat({tkhd, mdia}));
    const auto trex = full_box("trex", 0, 0, concat({be32(track_id), be32(1), be32(1000), be32(0), be32(0x02000000)}));
    const auto mvex = make_box("mvex", trex);
    const auto moov = make_box("moov", concat({trak, mvex}));
    return concat({ftyp, moov});
}

std::vector<std::uint8_t> make_media_fragment(std::uint32_t track_id,
                                              std::uint32_t decode_time,
                                              std::uint8_t payload_byte,
                                              std::uint32_t sample_flags = 0x02000000) {
    const auto tfhd = full_box(
        "tfhd", 0, 0x000038, concat({be32(track_id), be32(1000), be32(1), be32(sample_flags)}));
    const auto tfdt = full_box("tfdt", 0, 0, be32(decode_time));
    const auto trun = full_box("trun", 0, 0x000201, concat({be32(1), be32(16), be32(1)}));
    const auto traf = make_box("traf", concat({tfhd, tfdt, trun}));
    const auto moof = make_box("moof", traf);
    const auto mdat = make_box("mdat", {payload_byte});
    return concat({moof, mdat});
}

std::vector<std::uint8_t> make_dash_media_fragment(std::uint32_t track_id,
                                                   std::uint64_t decode_time,
                                                   std::vector<std::uint8_t> sample) {
    const auto tfhd = full_box(
        "tfhd", 0, 0x020038,
        concat({be32(track_id), be32(512), be32(static_cast<std::uint32_t>(sample.size())),
                be32(0x01010000)}));
    const auto tfdt = full_box("tfdt", 1, 0, be64(decode_time));
    const auto trun = full_box(
        "trun", 0, 0x000005,
        concat({be32(1), be32(112), be32(0x02000000)}));
    const auto traf = make_box("traf", concat({tfhd, tfdt, trun}));
    const auto moof = make_box("moof", concat({full_box("mfhd", 0, 0, be32(1)), traf}));
    const auto mdat = make_box("mdat", sample);
    return concat({moof, mdat});
}

std::string as_string(const std::vector<std::uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

// CENC fixture helpers ported from tests/cmaf_segmenter_test.cpp (around
// lines 320-356). Their byte layouts match what parse_track_protection and
// parse_pssh_boxes actually read; do not hand-roll replacements here.
std::vector<std::uint8_t> make_frma(const std::string& original_format) {
    return make_box("frma", std::vector<std::uint8_t>(original_format.begin(), original_format.end()));
}

std::vector<std::uint8_t> make_schm(const std::string& scheme_type) {
    std::vector<std::uint8_t> payload(scheme_type.begin(), scheme_type.end());
    append_be32(payload, 0x00010000);
    return full_box("schm", 0, 0, payload);
}

std::vector<std::uint8_t> make_tenc_box(std::uint8_t is_protected, std::uint8_t iv_size) {
    std::vector<std::uint8_t> payload{0, 0, is_protected, iv_size};
    const std::vector<std::uint8_t> kid{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    payload.insert(payload.end(), kid.begin(), kid.end());
    return full_box("tenc", 0, 0, payload);
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
    return full_box("pssh", 0, 0, payload);
}

// The Widevine common system ID, edef8ba9-79d6-4ace-a3c8-27dcd51d21ed, as raw
// bytes -- the same system ID used in msf_catalog_test.cpp.
std::vector<std::uint8_t> widevine_system_id() {
    return {0xed, 0xef, 0x8b, 0xa9, 0x79, 0xd6, 0x4a, 0xce,
            0xa3, 0xc8, 0x27, 0xdc, 0xd5, 0x1d, 0x21, 0xed};
}

// An encrypted DASH init segment: ftyp + moov, with the pssh a sibling of
// trak under moov (not inside sinf) -- mirrors make_init_segment's shape with
// an encv sample entry carrying sinf, plus a moov-level pssh. This is the
// full init segment collect_pssh_systems is parsed from at registration.
std::vector<std::uint8_t> make_encrypted_dash_init_with_pssh() {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = full_box("tkhd", 0, 0, concat({std::vector<std::uint8_t>(8, 0), be32(1), std::vector<std::uint8_t>(4, 0)}));
    const auto mdhd = full_box("mdhd", 0, 0, concat({std::vector<std::uint8_t>(8, 0), be32(1000), std::vector<std::uint8_t>(8, 0)}));
    const auto hdlr = full_box("hdlr", 0, 0, std::vector<std::uint8_t>{0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    auto visual_header = std::vector<std::uint8_t>(78, 0);
    visual_header[24] = 0x01;
    visual_header[25] = 0x40;
    visual_header[26] = 0x00;
    visual_header[27] = 0xf0;
    const auto avcc = make_box("avcC", {1, 100, 0, 12, 0xff});
    const auto sinf = make_sinf("avc1", "cenc");
    const auto sample_entry = make_box("encv", concat({visual_header, avcc, sinf}));
    const auto stsd = full_box("stsd", 0, 0, concat({be32(1), sample_entry}));
    const auto stbl = make_box("stbl", stsd);
    const auto minf = make_box("minf", stbl);
    const auto mdia = make_box("mdia", concat({mdhd, hdlr, minf}));
    const auto trak = make_box("trak", concat({tkhd, mdia}));
    const auto trex = full_box("trex", 0, 0, concat({be32(1), be32(1), be32(1000), be32(0), be32(0x02000000)}));
    const auto mvex = make_box("mvex", trex);
    const auto pssh = make_pssh(widevine_system_id());
    const auto moov = make_box("moov", concat({trak, mvex, pssh}));
    return concat({ftyp, moov});
}

bool expect_chunked_decodes(const std::string& encoded, const std::string& expected) {
    ChunkedBodyDecoder decoder(1024);
    for (const char ch : encoded) {
        const auto byte = static_cast<std::uint8_t>(ch);
        decoder.append(std::span<const std::uint8_t>(&byte, 1));
    }
    const std::vector<std::uint8_t> decoded = decoder.take_decoded();
    return expect(!decoder.failed(), "expected chunked decode to avoid failure") &&
           expect(decoder.complete(), "expected chunked decode to complete") &&
           expect(as_string(decoded) == expected, "expected decoded chunked content to match");
}

bool expect_chunked_rejects(const std::string& encoded) {
    ChunkedBodyDecoder decoder(1024);
    const auto* data = reinterpret_cast<const std::uint8_t*>(encoded.data());
    decoder.append(std::span<const std::uint8_t>(data, encoded.size()));
    return expect(decoder.failed(), "expected malformed chunked body to fail");
}

std::string send_chunked_put(std::uint16_t port,
                             const std::string& path,
                             std::span<const std::uint8_t> body) {
#if defined(_WIN32)
    static_cast<void>(port);
    static_cast<void>(path);
    static_cast<void>(body);
    return {};
#else
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return {};
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    static_cast<void>(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        static_cast<void>(::close(fd));
        return {};
    }

    std::ostringstream request;
    request << "PUT " << path << " HTTP/1.1\r\n"
            << "Host: 127.0.0.1\r\n"
            << "Transfer-Encoding: chunked\r\n"
            << "Content-Type: video/iso.segment\r\n"
            << "\r\n"
            << std::hex << body.size() << "\r\n";
    const std::string header = request.str();
    static_cast<void>(::send(fd, header.data(), header.size(), 0));
    static_cast<void>(::send(fd, body.data(), body.size(), 0));
    const std::string trailer = "\r\n0\r\n\r\n";
    static_cast<void>(::send(fd, trailer.data(), trailer.size(), 0));

    std::string response;
    std::array<char, 512> buffer{};
    const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (received > 0) {
        response.assign(buffer.data(), static_cast<std::size_t>(received));
    }
    static_cast<void>(::close(fd));
    return response;
#endif
}

int open_stalled_chunked_put(std::uint16_t port, const std::string& path) {
#if defined(_WIN32)
    static_cast<void>(port);
    static_cast<void>(path);
    return -1;
#else
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    static_cast<void>(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        static_cast<void>(::close(fd));
        return -1;
    }
    std::ostringstream request;
    request << "PUT " << path << " HTTP/1.1\r\n"
            << "Host: 127.0.0.1\r\n"
            << "Transfer-Encoding: chunked\r\n"
            << "Content-Type: video/iso.segment\r\n"
            << "\r\n"
            << "10\r\n"
            << "abc";
    const std::string bytes = request.str();
    static_cast<void>(::send(fd, bytes.data(), bytes.size(), 0));
    return fd;
#endif
}

void close_socket(int fd) {
#if !defined(_WIN32)
    if (fd >= 0) {
        static_cast<void>(::shutdown(fd, SHUT_RDWR));
        static_cast<void>(::close(fd));
    }
#else
    static_cast<void>(fd);
#endif
}

}  // namespace

int main() {
    bool ok = true;

    ok &= expect_chunked_decodes("4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n", "Wikipedia");
    ok &= expect_chunked_decodes("4;token=value\r\nWiki\r\n0\r\n\r\n", "Wiki");
    ok &= expect_chunked_rejects("FFFFFFFFFFFFFFFFF\r\nx\r\n0\r\n\r\n");

    {
        // FFmpeg's DASH muxer stores duration, size, and default flags in
        // tfhd and omits them from trun. The relay consumes trun samples
        // directly, so the live ingest must materialize those defaults.
        LiveDashIngestSession session(8);
        const auto init = make_init_segment(1);
        const auto dash_fragment = make_dash_media_fragment(1, 0, {0x11, 0x22, 0x33});
        session.ingest("/ingest/video", std::span<const std::uint8_t>(init.data(), init.size()));
        ok &= expect(session.wait_for_tracks(std::chrono::milliseconds(1), std::chrono::milliseconds(1)),
                     "expected track before DASH tfhd-default normalization test");
        const auto source = session.source();
        session.ingest("/ingest/video",
                       std::span<const std::uint8_t>(dash_fragment.data(), dash_fragment.size()));

        const std::optional<LiveObject> catalog = session.try_next_object();
        const std::optional<LiveObject> media = session.try_next_object();
        ok &= expect(catalog.has_value() && media.has_value(),
                     "expected catalog and normalized DASH media object");
        if (media.has_value()) {
            const auto boxes = openmoq::publisher::parse_mp4_boxes(media->payload);
            const auto* traf = boxes.empty() ? nullptr
                                              : openmoq::publisher::find_child_box(boxes.front(), "traf");
            const auto* trun = traf == nullptr ? nullptr
                                                : openmoq::publisher::find_child_box(*traf, "trun");
            ok &= expect(trun != nullptr, "expected normalized DASH trun");
            if (trun != nullptr) {
                const std::uint32_t flags = read_be32(media->payload, trun->payload.offset) & 0x00FFFFFFU;
                std::size_t cursor = trun->payload.offset + 4;
                const std::uint32_t sample_count = read_be32(media->payload, cursor);
                cursor += 4;
                const std::uint32_t data_offset = read_be32(media->payload, cursor);
                cursor += 4;
                if ((flags & 0x000004U) != 0) {
                    cursor += 4;
                }
                ok &= expect((flags & 0x000700U) == 0x000700U,
                             "expected trun to carry explicit sample duration, size, and flags");
                ok &= expect(sample_count == 1, "expected one normalized DASH sample");
                ok &= expect(read_be32(media->payload, cursor) == 512,
                             "expected tfhd default sample duration in trun");
                ok &= expect(read_be32(media->payload, cursor + 4) == 3,
                             "expected tfhd default sample size in trun");
                ok &= expect(read_be32(media->payload, cursor + 8) == 0x02000000,
                             "expected first-sample flags in the normalized trun sample");
                ok &= expect(data_offset == boxes.front().span.size + 8,
                             "expected normalized trun data offset to follow the resized moof");
            }
        }
    }

    {
        LiveDashIngestSession session(16);
        const auto video_init = make_init_segment(1);
        const auto audio_init = make_init_segment(1, "soun");
        session.ingest("/ingest/video0", std::span<const std::uint8_t>(video_init.data(), video_init.size()));
        session.ingest("/ingest/video1", std::span<const std::uint8_t>(audio_init.data(), audio_init.size()));
        ok &= expect(session.wait_for_tracks(std::chrono::milliseconds(1), std::chrono::milliseconds(1)),
                     "expected audio and video tracks before shared-group test");
        const auto source = session.source();

        const auto audio_before_video = make_media_fragment(1, 0, 0x21);
        const auto video_keyframe_a = make_media_fragment(1, 0, 0x11);
        const auto video_dependent = make_media_fragment(1, 1000, 0x12, 0x00010000);
        const auto audio_a = make_media_fragment(1, 1024, 0x22);
        const auto video_keyframe_b = make_media_fragment(1, 2000, 0x13);
        const auto audio_b = make_media_fragment(1, 2048, 0x23);
        session.ingest("/ingest/video1", std::span<const std::uint8_t>(audio_before_video.data(), audio_before_video.size()));
        session.ingest("/ingest/video0", std::span<const std::uint8_t>(video_keyframe_a.data(), video_keyframe_a.size()));
        session.ingest("/ingest/video0", std::span<const std::uint8_t>(video_dependent.data(), video_dependent.size()));
        session.ingest("/ingest/video1", std::span<const std::uint8_t>(audio_a.data(), audio_a.size()));
        session.ingest("/ingest/video0", std::span<const std::uint8_t>(video_keyframe_b.data(), video_keyframe_b.size()));
        session.ingest("/ingest/video1", std::span<const std::uint8_t>(audio_b.data(), audio_b.size()));

        const std::optional<LiveObject> catalog = session.try_next_object();
        const std::optional<LiveObject> video_a = session.try_next_object();
        const std::optional<LiveObject> video_delta = session.try_next_object();
        const std::optional<LiveObject> audio_group_a = session.try_next_object();
        const std::optional<LiveObject> video_b = session.try_next_object();
        const std::optional<LiveObject> audio_group_b = session.try_next_object();
        const std::optional<LiveObject> extra = session.try_next_object();
        ok &= expect(catalog.has_value() && catalog->track_name == "catalog",
                     "expected catalog before shared-group media");
        ok &= expect(video_a.has_value() && video_a->track_name == "video0_vide_1" &&
                         video_a->group_id == 0 && video_a->object_id == 0,
                     "expected first video keyframe in shared group 0");
        ok &= expect(video_delta.has_value() && video_delta->track_name == "video0_vide_1" &&
                         video_delta->group_id == 0 && video_delta->object_id == 1,
                     "expected dependent video in shared group 0");
        ok &= expect(audio_group_a.has_value() && audio_group_a->track_name == "video1_soun_1" &&
                         audio_group_a->group_id == 0 && audio_group_a->object_id == 0,
                     "expected audio to join active video group 0");
        ok &= expect(video_b.has_value() && video_b->track_name == "video0_vide_1" &&
                         video_b->group_id == 1 && video_b->object_id == 0,
                     "expected next video keyframe in shared group 1");
        ok &= expect(audio_group_b.has_value() && audio_group_b->track_name == "video1_soun_1" &&
                         audio_group_b->group_id == 1 && audio_group_b->object_id == 0,
                     "expected audio to join active video group 1");
        ok &= expect(!extra.has_value(), "expected audio before the first video keyframe to be dropped");
    }

    {
        LiveDashIngestSession session(8);
        const auto init = make_init_segment(1);
        const auto video_a = make_media_fragment(1, 0, 0x11);
        const auto audio_a = make_media_fragment(1, 0, 0x21);
        session.ingest("/ingest/video", std::span<const std::uint8_t>(init.data(), init.size()));
        session.ingest("/ingest/audio", std::span<const std::uint8_t>(init.data(), init.size()));
        ok &= expect(session.wait_for_tracks(std::chrono::milliseconds(1), std::chrono::milliseconds(1)),
                     "expected tracks after both init segments");
        const auto source = session.source();
        ok &= expect(source.tracks.size() == 3, "expected catalog plus two path-specific media tracks");
        ok &= expect(source.tracks[1].track_name == "video_vide_1", "expected video path track name");
        ok &= expect(source.tracks[2].track_name == "audio_vide_1", "expected audio path track name");

        session.ingest("/ingest/video", std::span<const std::uint8_t>(video_a.data(), video_a.size()));
        session.ingest("/ingest/audio", std::span<const std::uint8_t>(audio_a.data(), audio_a.size()));
        const std::optional<LiveObject> catalog = session.try_next_object();
        const std::optional<LiveObject> video = session.try_next_object();
        const std::optional<LiveObject> audio = session.try_next_object();
        ok &= expect(catalog.has_value() && catalog->track_name == "catalog", "expected catalog object first");
        ok &= expect(video.has_value() && video->track_name == "video_vide_1", "expected video media object");
        ok &= expect(audio.has_value() && audio->track_name == "audio_vide_1", "expected audio media object");
    }

    {
        LiveDashIngestSession session(8);
        const auto init = make_init_segment(1);
        const auto keyframe_a = make_media_fragment(1, 0, 0x11);
        const auto dependent_a = make_media_fragment(1, 1000, 0x12, 0x00010000);
        const auto dependent_b = make_media_fragment(1, 2000, 0x13, 0x00010000);
        const auto keyframe_b = make_media_fragment(1, 3000, 0x14);
        session.ingest("/ingest/video", std::span<const std::uint8_t>(init.data(), init.size()));
        ok &= expect(session.wait_for_tracks(std::chrono::milliseconds(1), std::chrono::milliseconds(1)),
                     "expected track before keyframe-grouping test");
        const auto source = session.source();
        session.ingest("/ingest/video", std::span<const std::uint8_t>(keyframe_a.data(), keyframe_a.size()));
        session.ingest("/ingest/video", std::span<const std::uint8_t>(dependent_a.data(), dependent_a.size()));
        session.ingest("/ingest/video", std::span<const std::uint8_t>(dependent_b.data(), dependent_b.size()));
        session.ingest("/ingest/video", std::span<const std::uint8_t>(keyframe_b.data(), keyframe_b.size()));

        const std::optional<LiveObject> catalog = session.try_next_object();
        const std::optional<LiveObject> first = session.try_next_object();
        const std::optional<LiveObject> second = session.try_next_object();
        const std::optional<LiveObject> third = session.try_next_object();
        const std::optional<LiveObject> fourth = session.try_next_object();
        ok &= expect(catalog.has_value() && catalog->track_name == "catalog",
                     "expected catalog before keyframe-grouped media");
        ok &= expect(first.has_value() && first->group_id == 0 && first->object_id == 0,
                     "expected first keyframe to start group 0");
        ok &= expect(second.has_value() && second->group_id == 0 && second->object_id == 1,
                     "expected first dependent frame to remain in group 0");
        ok &= expect(third.has_value() && third->group_id == 0 && third->object_id == 2,
                     "expected second dependent frame to remain in group 0");
        ok &= expect(fourth.has_value() && fourth->group_id == 1 && fourth->object_id == 0,
                     "expected next keyframe to start group 1");
        ok &= expect(first.has_value() && second.has_value() && third.has_value() && fourth.has_value() &&
                         !first->final_in_subgroup && !second->final_in_subgroup &&
                         !third->final_in_subgroup && !fourth->final_in_subgroup,
                     "expected DASH group objects to keep their subgroup stream open");
    }

    {
        // The catalog must embed each track's base64 CMAF init segment so
        // subscribers can initialize their decoders.
        LiveDashIngestSession session(8);
        const auto init = make_init_segment(1);
        session.ingest("/ingest/video", std::span<const std::uint8_t>(init.data(), init.size()));
        ok &= expect(session.wait_for_tracks(std::chrono::milliseconds(1), std::chrono::milliseconds(1)),
                     "expected tracks after init for initData test");
        const auto source = session.source();
        const std::optional<LiveObject> catalog = session.try_next_object();
        ok &= expect(catalog.has_value() && catalog->track_name == "catalog",
                     "expected catalog object for initData test");
        if (catalog.has_value()) {
            const std::string catalog_text = as_string(catalog->payload);
            ok &= expect(catalog_text.find("\"version\":\"1\"") != std::string::npos,
                         "expected MSF v1 string version in CTE catalog");
            ok &= expect(catalog_text.find("\"initDataList\"") != std::string::npos,
                         "expected root initDataList in CTE catalog");
            ok &= expect(catalog_text.find("\"initRef\"") != std::string::npos,
                         "expected per-track initRef in CTE catalog");
            ok &= expect(catalog_text.find("\"format\"") == std::string::npos,
                         "expected no legacy format field in CTE catalog");
            ok &= expect(catalog_text.find("\"initData\":\"") == std::string::npos,
                         "expected no inline initData field in CTE catalog");
            ok &= expect(catalog_text.find("\"isLive\":true") != std::string::npos,
                         "expected isLive true in CTE catalog");
            ok &= expect(catalog_text.find("\"renderGroup\":1") != std::string::npos,
                         "expected DASH catalog track to declare its render group");
            ok &= expect(catalog_text.find("\"width\":320") != std::string::npos &&
                             catalog_text.find("\"height\":240") != std::string::npos,
                         "expected DASH catalog video dimensions for renderer selection");
        }
    }

    {
        // Phase 5: an encrypted DASH init segment's catalog must carry
        // contentProtections/contentProtectionRefIDs for the protected track.
        LiveDashIngestSession session(8);
        const auto encrypted_init = make_encrypted_dash_init_with_pssh();
        session.ingest("/ingest/enc",
                       std::span<const std::uint8_t>(encrypted_init.data(), encrypted_init.size()));
        ok &= expect(session.wait_for_tracks(std::chrono::milliseconds(1), std::chrono::milliseconds(1)),
                     "expected a track from the encrypted DASH init segment");

        const std::optional<LiveObject> catalog = session.try_next_object();
        ok &= expect(catalog.has_value() && catalog->track_name == "catalog",
                     "expected a catalog object from the encrypted DASH init");
        if (catalog.has_value()) {
            const std::string catalog_text(catalog->payload.begin(), catalog->payload.end());
            ok &= expect(catalog_text.find("\"contentProtections\"") != std::string::npos,
                         "expected the DASH catalog to carry contentProtections for an encrypted init");
            ok &= expect(catalog_text.find("\"contentProtectionRefIDs\"") != std::string::npos,
                         "expected the protected DASH track to reference a contentProtections entry");
        }
    }

    {
        // A media fragment referencing a track that was never declared in the
        // init segment must be dropped, not crash the ingest via an uncaught
        // exception, and must not be enqueued as an object.
        LiveDashIngestSession session(8);
        const auto init = make_init_segment(1);
        const auto bad_fragment = make_media_fragment(99, 0, 0x11);
        session.ingest("/ingest/video", std::span<const std::uint8_t>(init.data(), init.size()));
        ok &= expect(session.wait_for_tracks(std::chrono::milliseconds(1), std::chrono::milliseconds(1)),
                     "expected tracks after init for malformed-fragment test");
        const auto source = session.source();
        session.ingest("/ingest/video",
                       std::span<const std::uint8_t>(bad_fragment.data(), bad_fragment.size()));
        const std::optional<LiveObject> catalog = session.try_next_object();
        const std::optional<LiveObject> media = session.try_next_object();
        ok &= expect(catalog.has_value() && catalog->track_name == "catalog",
                     "expected catalog after malformed fragment");
        ok &= expect(!media.has_value(),
                     "expected malformed fragment to be dropped rather than enqueued");
    }

    {
        // Once source() has frozen the announced track set, a path whose init
        // segment arrives afterwards is ignored rather than added (the publisher
        // could not alias it and would abort the whole session).
        LiveDashIngestSession session(8);
        const auto video_init = make_init_segment(1);
        session.ingest("/ingest/video", std::span<const std::uint8_t>(video_init.data(), video_init.size()));
        ok &= expect(session.wait_for_tracks(std::chrono::milliseconds(1), std::chrono::milliseconds(1)),
                     "expected tracks before freezing for late-path test");
        const auto snapshot = session.source();
        ok &= expect(snapshot.tracks.size() == 2, "expected catalog plus one track before late path");
        const auto audio_init = make_init_segment(2);
        session.ingest("/ingest/audio", std::span<const std::uint8_t>(audio_init.data(), audio_init.size()));
        const auto after = session.source();
        ok &= expect(after.tracks.size() == 2, "expected late path to be ignored after the snapshot froze");
    }

#if !defined(_WIN32)
    {
        LiveDashIngestConfig config;
        config.host = "127.0.0.1";
        config.port = 0;
        config.path_prefix = "/ingest";
        LiveDashIngestServer server(config);
        const auto status = server.start();
        ok &= expect(status.ok, "expected live DASH server to start: " + status.message);
        ok &= expect(server.bound_port() != 0, "expected live DASH server to expose a bound port");
        server.stop();
    }

    {
        LiveDashIngestConfig config;
        config.host = "127.0.0.1";
        config.port = 0;
        config.path_prefix = "/ingest";
        LiveDashIngestServer server(config);
        const auto status = server.start();
        ok &= expect(status.ok, "expected live DASH server to start for bind failure test: " + status.message);
        if (status.ok) {
            config.port = server.bound_port();
            LiveDashIngestServer conflicting_server(config);
            const auto conflicting_status = conflicting_server.start();
            const std::string expected_message =
                "failed to bind DASH ingest socket: " + std::system_category().message(EADDRINUSE);
            ok &= expect(!conflicting_status.ok, "expected conflicting DASH ingest server bind to fail");
            ok &= expect(conflicting_status.message == expected_message,
                         "expected bind failure to include OS error; got: " + conflicting_status.message);
        }
        server.stop();
    }

    {
        LiveDashIngestConfig config;
        config.host = "127.0.0.1";
        config.port = 0;
        config.path_prefix = "/ingest";
        LiveDashIngestServer server(config);
        const auto status = server.start();
        ok &= expect(status.ok, "expected live DASH server to start for PUT test: " + status.message);
        const std::uint16_t port = server.bound_port();

        const auto video_body = concat({make_init_segment(1), make_media_fragment(1, 0, 0x31)});
        const auto audio_body = concat({make_init_segment(1), make_media_fragment(1, 0, 0x41)});
        std::string video_response;
        std::string audio_response;
        std::thread video_thread([&]() {
            video_response = send_chunked_put(port, "/ingest/video", video_body);
        });
        std::thread audio_thread([&]() {
            audio_response = send_chunked_put(port, "/ingest/audio", audio_body);
        });
        video_thread.join();
        audio_thread.join();

        ok &= expect(video_response.find("204 No Content") != std::string::npos,
                     "expected video chunked PUT to receive 204");
        ok &= expect(audio_response.find("204 No Content") != std::string::npos,
                     "expected audio chunked PUT to receive 204");
        auto source = server.source();
        ok &= expect(source.tracks.size() == 3,
                     "expected HTTP ingest to discover both media paths");
        if (source.tracks.size() == 3) {
            const std::optional<LiveObject> catalog = source.next_object();
            const std::optional<LiveObject> first = source.next_object();
            const std::optional<LiveObject> second = source.next_object();
            ok &= expect(catalog.has_value() && catalog->track_name == "catalog",
                         "expected HTTP ingest catalog object");
            ok &= expect(first.has_value() && second.has_value() && first->track_name != second->track_name,
                         "expected HTTP ingest objects from two different paths");
        }
        server.stop();
    }

    {
        LiveDashIngestConfig config;
        config.host = "127.0.0.1";
        config.port = 0;
        config.path_prefix = "/ingest";
        LiveDashIngestServer server(config);
        const auto status = server.start();
        ok &= expect(status.ok, "expected live DASH server to start for FFmpeg-style representation test: " +
                                    status.message);
        const std::uint16_t port = server.bound_port();

        const auto video0_init = make_init_segment(1);
        const auto video1_init = make_init_segment(2);
        const auto video2_init = make_init_segment(3);
        ok &= expect(send_chunked_put(port, "/ingest/video0",
                                      std::span<const std::uint8_t>(video0_init.data(), video0_init.size()))
                         .find("204 No Content") != std::string::npos,
                     "expected FFmpeg video0 init request to receive 204");
        ok &= expect(send_chunked_put(port, "/ingest/video1",
                                      std::span<const std::uint8_t>(video1_init.data(), video1_init.size()))
                         .find("204 No Content") != std::string::npos,
                     "expected FFmpeg video1 init request to receive 204");
        ok &= expect(send_chunked_put(port, "/ingest/video2",
                                      std::span<const std::uint8_t>(video2_init.data(), video2_init.size()))
                         .find("204 No Content") != std::string::npos,
                     "expected FFmpeg video2 init request to receive 204");
        ok &= expect(server.wait_for_tracks(std::chrono::milliseconds(1), std::chrono::milliseconds(1)),
                     "expected FFmpeg-style representation init requests to discover tracks");
        auto source = server.source();
        ok &= expect(source.tracks.size() == 4,
                     "expected catalog plus three FFmpeg representation tracks");

        const auto video0_media = make_media_fragment(1, 0, 0x51);
        const auto video1_media = make_media_fragment(2, 0, 0x52);
        const auto video2_media = make_media_fragment(3, 0, 0x53);
        ok &= expect(send_chunked_put(port, "/ingest/video0",
                                      std::span<const std::uint8_t>(video0_media.data(), video0_media.size()))
                         .find("204 No Content") != std::string::npos,
                     "expected FFmpeg video0 media request to receive 204");
        ok &= expect(send_chunked_put(port, "/ingest/video1",
                                      std::span<const std::uint8_t>(video1_media.data(), video1_media.size()))
                         .find("204 No Content") != std::string::npos,
                     "expected FFmpeg video1 media request to receive 204");
        ok &= expect(send_chunked_put(port, "/ingest/video2",
                                      std::span<const std::uint8_t>(video2_media.data(), video2_media.size()))
                         .find("204 No Content") != std::string::npos,
                     "expected FFmpeg video2 media request to receive 204");
        const std::optional<LiveObject> catalog = source.next_object();
        const std::optional<LiveObject> first = source.next_object();
        const std::optional<LiveObject> second = source.next_object();
        const std::optional<LiveObject> third = source.next_object();
        ok &= expect(catalog.has_value() && catalog->track_name == "catalog",
                     "expected FFmpeg-style ingest catalog object");
        ok &= expect(first.has_value() && second.has_value() && third.has_value(),
                     "expected FFmpeg-style media objects for three representations");
        server.stop();
    }

    {
        LiveDashIngestConfig config;
        config.host = "127.0.0.1";
        config.port = 0;
        config.path_prefix = "/ingest";
        LiveDashIngestServer server(config);
        const auto status = server.start();
        ok &= expect(status.ok, "expected live DASH server to start for stalled client stop test: " +
                                    status.message);
        const int stalled_fd = open_stalled_chunked_put(server.bound_port(), "/ingest/stalled");
        ok &= expect(stalled_fd >= 0, "expected stalled chunked client to connect");

        auto stopped = std::async(std::launch::async, [&server]() {
            server.stop();
        });
        const bool stop_returned =
            stopped.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready;
        if (!stop_returned) {
            close_socket(stalled_fd);
            static_cast<void>(stopped.wait_for(std::chrono::seconds(2)));
        } else {
            close_socket(stalled_fd);
        }
        ok &= expect(stop_returned, "expected stop to close active stalled clients before joining workers");
    }
#endif

    return ok ? 0 : 1;
}
