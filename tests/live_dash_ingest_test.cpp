#include "openmoq/publisher/live_dash_ingest.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string>
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

std::vector<std::uint8_t> make_init_segment(std::uint32_t track_id) {
    const auto ftyp = make_box("ftyp", {'i', 's', 'o', '6', 0, 0, 0, 1, 'i', 's', 'o', '6', 'c', 'm', 'f', 'c'});
    const auto tkhd = full_box("tkhd", 0, 0, concat({std::vector<std::uint8_t>(8, 0), be32(track_id), std::vector<std::uint8_t>(4, 0)}));
    const auto mdhd = full_box("mdhd", 0, 0, concat({std::vector<std::uint8_t>(8, 0), be32(1000), std::vector<std::uint8_t>(8, 0)}));
    const auto hdlr = full_box("hdlr", 0, 0, {0, 0, 0, 0, 'v', 'i', 'd', 'e', 0, 0, 0, 0});
    auto visual_header = std::vector<std::uint8_t>(70, 0);
    visual_header[24] = 0x01;
    visual_header[25] = 0x40;
    visual_header[26] = 0x00;
    visual_header[27] = 0xf0;
    const auto sample_entry = make_box("avc1", concat({visual_header, make_box("avcC", {1, 100, 0, 12, 0xff})}));
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
                                              std::uint8_t payload_byte) {
    const auto tfhd = full_box("tfhd", 0, 0x000038, concat({be32(track_id), be32(1000), be32(1), be32(0x02000000)}));
    const auto tfdt = full_box("tfdt", 0, 0, be32(decode_time));
    const auto trun = full_box("trun", 0, 0x000201, concat({be32(1), be32(16), be32(1)}));
    const auto traf = make_box("traf", concat({tfhd, tfdt, trun}));
    const auto moof = make_box("moof", traf);
    const auto mdat = make_box("mdat", {payload_byte});
    return concat({moof, mdat});
}

std::string as_string(const std::vector<std::uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
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

}  // namespace

int main() {
    bool ok = true;

    ok &= expect_chunked_decodes("4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n", "Wikipedia");
    ok &= expect_chunked_decodes("4;token=value\r\nWiki\r\n0\r\n\r\n", "Wiki");
    ok &= expect_chunked_rejects("FFFFFFFFFFFFFFFFF\r\nx\r\n0\r\n\r\n");

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

    return ok ? 0 : 1;
}
