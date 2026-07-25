#include "../msfts_source.h"
#include "../msfts_options.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using openmoq::examples::msfts::MsftsSource;
using openmoq::examples::msfts::MsftsSourceConfig;
using openmoq::examples::msfts::parse_msfts_options;
using openmoq::examples::msfts::parse_msfts_endpoint;
using openmoq::publisher::LiveCatalogMode;
using openmoq::publisher::DraftVersion;
using openmoq::publisher::transport::TransportKind;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> make_packet(std::uint16_t pid, std::uint8_t marker = 0xff) {
    std::vector<std::uint8_t> packet(188, 0xff);
    packet[0] = 0x47;
    packet[1] = static_cast<std::uint8_t>((pid >> 8) & 0x1f);
    packet[2] = static_cast<std::uint8_t>(pid);
    packet[3] = 0x10;
    packet[4] = marker;
    return packet;
}

std::vector<std::uint8_t> make_pat() {
    auto packet = make_packet(0);
    packet[1] |= 0x40;
    const std::uint8_t section[] = {
        0x00, 0x00, 0xb0, 0x11, 0x00, 0x01, 0xc1, 0x00, 0x00,
        0x00, 0x01, 0xe1, 0x00,
        0x00, 0x02, 0xe1, 0x10,
        0x00, 0x00, 0x00, 0x00,
    };
    std::copy(std::begin(section), std::end(section), packet.begin() + 4);
    return packet;
}

std::vector<std::uint8_t> make_pmt(std::uint16_t pid,
                                   std::uint16_t program,
                                   std::uint16_t elementary_pid) {
    auto packet = make_packet(pid);
    packet[1] |= 0x40;
    const std::uint8_t section[] = {
        0x00, 0x02, 0xb0, 0x12,
        static_cast<std::uint8_t>(program >> 8),
        static_cast<std::uint8_t>(program),
        0xc1, 0x00, 0x00,
        static_cast<std::uint8_t>(0xe0 | (elementary_pid >> 8)),
        static_cast<std::uint8_t>(elementary_pid),
        0xf0, 0x00,
        0x1b,
        static_cast<std::uint8_t>(0xe0 | (elementary_pid >> 8)),
        static_cast<std::uint8_t>(elementary_pid),
        0xf0, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    std::copy(std::begin(section), std::end(section), packet.begin() + 4);
    return packet;
}

void append_packet(std::vector<std::uint8_t>& bytes,
                   const std::vector<std::uint8_t>& packet,
                   std::size_t source_packet_size) {
    if (source_packet_size == 192) {
        bytes.insert(bytes.end(), {0x00, 0x00, 0x00, 0x01});
    }
    bytes.insert(bytes.end(), packet.begin(), packet.end());
}

std::filesystem::path write_fixture(const std::vector<std::uint8_t>& bytes,
                                    const std::string& suffix) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("openmoq-msfts-" + suffix + ".bin");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    return path;
}

std::uint16_t packet_pid(const std::uint8_t* packet, std::size_t packet_size) {
    const std::uint8_t* ts = packet + (packet_size == 192 ? 4 : 0);
    return static_cast<std::uint16_t>(((ts[1] & 0x1f) << 8) | ts[2]);
}

void test_source_catalog_and_filtering(std::size_t packet_size) {
    std::vector<std::uint8_t> fixture;
    append_packet(fixture, make_pat(), packet_size);
    append_packet(fixture, make_pmt(0x100, 1, 0x101), packet_size);
    append_packet(fixture, make_pmt(0x110, 2, 0x111), packet_size);
    append_packet(fixture, make_packet(0x101, 0x11), packet_size);
    append_packet(fixture, make_packet(0x111, 0x22), packet_size);
    append_packet(fixture, make_packet(0x101, 0x33), packet_size);
    const auto path = write_fixture(fixture, std::to_string(packet_size));

    std::string error;
    auto source = MsftsSource::open(MsftsSourceConfig{
        .input_path = path,
        .track_name = "transport",
        .requested_program = 1,
        .packets_per_object = 2,
    }, error);
    std::filesystem::remove(path);

    expect(source != nullptr, "expected valid " + std::to_string(packet_size) + "-byte source");
    if (source == nullptr) {
        std::cerr << error << '\n';
        return;
    }

    expect(source->info().packet_size == packet_size, "expected packet-size detection");
    expect(source->info().program_number == 1, "expected requested PAT program selection");
    expect(source->info().pmt_pid == 0x100, "expected selected PMT PID");
    expect(source->info().pcr_pid == 0x101, "expected selected PCR PID");
    expect(source->info().init_data.size() == packet_size * 2,
           "expected PAT and PMT source packets in initData");

    auto live = source->live_source();
    expect(live.catalog_mode == LiveCatalogMode::kSourceObject,
           "expected caller-supplied catalog mode");
    expect(live.tracks.size() == 2 && live.tracks[0].track_name == "catalog" &&
               live.tracks[1].track_name == "transport",
           "expected catalog and media tracks");

    const auto catalog = live.next_object();
    expect(catalog.has_value() && catalog->track_name == "catalog" &&
               !catalog->payload.empty(),
           "expected a non-empty catalog object first");
    if (catalog.has_value()) {
        const std::string text(catalog->payload.begin(), catalog->payload.end());
        expect(text.find("\"packaging\":\"m2ts\"") != std::string::npos,
               "expected m2ts packaging in catalog");
        expect(text.find("\"m2tsPacketSize\":" + std::to_string(packet_size)) !=
                   std::string::npos,
               "expected packet size in catalog");
        expect(text.find("\"m2tsProgramNumber\":1") != std::string::npos,
               "expected program number in catalog");
        expect(text.find("\"m2tsRandomAccess\":false") != std::string::npos,
               "expected random-access capability declaration");
        expect((packet_size == 192) ==
                   (text.find("\"m2tsTimestampMode\":\"opaque\"") != std::string::npos),
               "expected opaque timestamp mode only for M2TS source packets");
        expect(text.find("\"initData\":\"") != std::string::npos,
               "expected base64 initData");
    }

    const auto media = live.next_object();
    expect(media.has_value() && media->track_name == "transport",
           "expected media object after catalog");
    if (media.has_value()) {
        expect(media->group_id == 0 && media->object_id == 0,
               "expected group zero and first object identifier");
        expect(media->payload.size() % packet_size == 0,
               "expected whole source packets only");
        for (std::size_t offset = 0; offset < media->payload.size(); offset += packet_size) {
            const std::uint16_t pid = packet_pid(media->payload.data() + offset, packet_size);
            expect(pid == 0 || pid == 0x100 || pid == 0x101,
                   "expected non-selected program PID to be filtered");
        }
        expect(packet_pid(media->payload.data(), packet_size) == 0,
               "expected repeated PAT at the first media object");
        expect(packet_pid(media->payload.data() + packet_size, packet_size) == 0x100,
               "expected repeated PMT after PAT");
    }
}

void test_ambiguous_m2ts_prefix_is_detected_as_192_bytes() {
    auto pat = make_pat();
    pat[184] = 0x47;
    std::vector<std::uint8_t> fixture;
    append_packet(fixture, pat, 192);
    append_packet(fixture, make_pmt(0x100, 1, 0x101), 192);
    fixture[0] = 0x47;
    const auto path = write_fixture(fixture, "ambiguous-192");

    std::string error;
    auto source = MsftsSource::open(MsftsSourceConfig{.input_path = path}, error);
    std::filesystem::remove(path);
    expect(source != nullptr && source->info().packet_size == 192,
           "expected file-size and sync alignment to disambiguate M2TS");
}

void test_partial_packet_is_rejected() {
    std::vector<std::uint8_t> fixture;
    append_packet(fixture, make_pat(), 188);
    append_packet(fixture, make_pmt(0x100, 1, 0x101), 188);
    fixture.push_back(0x47);
    const auto path = write_fixture(fixture, "partial");

    std::string error;
    auto source = MsftsSource::open(MsftsSourceConfig{.input_path = path}, error);
    std::filesystem::remove(path);

    expect(source == nullptr, "expected partial source packet to be rejected");
    expect(error.find("partial") != std::string::npos,
           "expected partial-packet error message");
}

void test_corrupt_packet_does_not_publish_partial_object() {
    std::vector<std::uint8_t> fixture;
    append_packet(fixture, make_pat(), 188);
    append_packet(fixture, make_pmt(0x100, 1, 0x101), 188);
    auto corrupt = make_packet(0x101);
    corrupt[0] = 0;
    append_packet(fixture, corrupt, 188);
    const auto path = write_fixture(fixture, "corrupt");

    std::string error;
    auto source = MsftsSource::open(MsftsSourceConfig{
        .input_path = path,
        .packets_per_object = 4,
    }, error);
    std::filesystem::remove(path);
    expect(source != nullptr, "expected discovery before the corrupt packet");
    if (source == nullptr) {
        return;
    }

    auto live = source->live_source();
    static_cast<void>(live.next_object());
    const auto media = live.next_object();
    expect(!media.has_value(), "expected corrupt input to suppress a partial media object");
    expect(source->error().find("sync byte") != std::string::npos,
           "expected corrupt-packet error to be retained");
}

void test_program_discovery_errors() {
    {
        std::vector<std::uint8_t> fixture;
        append_packet(fixture, make_packet(0x101), 188);
        append_packet(fixture, make_packet(0x101), 188);
        const auto path = write_fixture(fixture, "missing-pat");
        std::string error;
        auto source = MsftsSource::open(MsftsSourceConfig{.input_path = path}, error);
        std::filesystem::remove(path);
        expect(source == nullptr && error.find("PAT") != std::string::npos,
               "expected a missing PAT to be rejected");
    }

    {
        std::vector<std::uint8_t> fixture;
        append_packet(fixture, make_pat(), 188);
        append_packet(fixture, make_pmt(0x100, 1, 0x101), 188);
        const auto path = write_fixture(fixture, "missing-program");
        std::string error;
        auto source = MsftsSource::open(
            MsftsSourceConfig{.input_path = path, .requested_program = 3},
            error);
        std::filesystem::remove(path);
        expect(source == nullptr && error.find("requested program 3") != std::string::npos,
               "expected an absent requested program to be rejected");
    }

    {
        std::vector<std::uint8_t> fixture;
        append_packet(fixture, make_pat(), 188);
        append_packet(fixture, make_pmt(0x100, 1, 0x101), 188);
        const auto path = write_fixture(fixture, "missing-pmt");
        std::string error;
        auto source = MsftsSource::open(
            MsftsSourceConfig{.input_path = path, .requested_program = 2},
            error);
        std::filesystem::remove(path);
        expect(source == nullptr && error.find("PMT") != std::string::npos,
               "expected a missing selected-program PMT to be rejected");
    }
}

void test_declared_psi_interval_is_honored() {
    std::vector<std::uint8_t> fixture;
    append_packet(fixture, make_pat(), 188);
    append_packet(fixture, make_pmt(0x100, 1, 0x101), 188);
    for (int index = 0; index < 102; ++index) {
        append_packet(fixture,
                      make_packet(0x101, static_cast<std::uint8_t>(index)),
                      188);
    }
    const auto path = write_fixture(fixture, "psi-interval");

    std::string error;
    auto source = MsftsSource::open(MsftsSourceConfig{
        .input_path = path,
        .packets_per_object = 2,
    }, error);
    std::filesystem::remove(path);
    expect(source != nullptr, "expected PSI interval fixture to open");
    if (source == nullptr) {
        return;
    }

    auto live = source->live_source();
    static_cast<void>(live.next_object());
    std::uint8_t expected_pat_continuity = 0;
    std::uint8_t expected_pmt_continuity = 0;
    for (std::size_t object_id = 0; object_id <= 50; ++object_id) {
        const auto object = live.next_object();
        expect(object.has_value(), "expected media through PSI interval boundary");
        if (!object.has_value()) {
            return;
        }
        const bool begins_with_psi =
            object->payload.size() >= 376 &&
            packet_pid(object->payload.data(), 188) == 0 &&
            packet_pid(object->payload.data() + 188, 188) == 0x100;
        expect(begins_with_psi == (object_id == 0 || object_id == 50),
               "expected PAT/PMT at the declared 500 ms interval");
        for (std::size_t offset = 0; offset < object->payload.size(); offset += 188) {
            const std::uint8_t* packet = object->payload.data() + offset;
            const std::uint16_t pid = packet_pid(packet, 188);
            if (pid == 0) {
                expect((packet[3] & 0x0f) == expected_pat_continuity,
                       "expected monotonic PAT continuity counters");
                expected_pat_continuity =
                    static_cast<std::uint8_t>((expected_pat_continuity + 1) & 0x0f);
            } else if (pid == 0x100) {
                expect((packet[3] & 0x0f) == expected_pmt_continuity,
                       "expected monotonic PMT continuity counters");
                expected_pmt_continuity =
                    static_cast<std::uint8_t>((expected_pmt_continuity + 1) & 0x0f);
            }
        }
    }
}

void test_command_line_contract() {
    const auto options = parse_msfts_options({
        "--endpoint", "moqt://relay.example.com:4443/contribute",
        "--input", "sample.m2ts",
        "--namespace", "example.msfts",
        "--track", "transport",
        "--program", "7",
        "--packets-per-object", "12",
        "--draft", "18",
        "--insecure",
    });
    expect(options.endpoint == "moqt://relay.example.com:4443/contribute",
           "expected endpoint option");
    expect(options.input_path == "sample.m2ts", "expected input option");
    expect(options.track_namespace == "example.msfts", "expected namespace option");
    expect(options.track_name == "transport", "expected track option");
    expect(options.program == 7, "expected program option");
    expect(options.packets_per_object == 12, "expected packets-per-object option");
    expect(options.draft == DraftVersion::kDraft18, "expected draft option");
    expect(options.insecure, "expected insecure option");

    const auto endpoint = parse_msfts_endpoint(options.endpoint);
    expect(endpoint.transport == TransportKind::kRawQuic,
           "expected moqt endpoint to select raw QUIC");
    expect(endpoint.host == "relay.example.com" && endpoint.port == 4443 &&
               endpoint.path == "/contribute",
           "expected endpoint authority and path parsing");

    const auto webtransport = parse_msfts_endpoint("https://127.0.0.1:4433/moq");
    expect(webtransport.transport == TransportKind::kWebTransport,
           "expected https endpoint to select WebTransport");
    expect(webtransport.path_explicit && webtransport.path == "/moq",
           "expected explicit WebTransport path");

    bool rejected = false;
    try {
        static_cast<void>(parse_msfts_options({"--input", "sample.ts", "--draft", "15"}));
    } catch (const std::exception& exception) {
        rejected = std::string(exception.what()).find("--draft") != std::string::npos;
    }
    expect(rejected, "expected unsupported draft to be rejected");
}

}  // namespace

int main() {
    test_source_catalog_and_filtering(188);
    test_source_catalog_and_filtering(192);
    test_ambiguous_m2ts_prefix_is_detected_as_192_bytes();
    test_partial_packet_is_rejected();
    test_corrupt_packet_does_not_publish_partial_object();
    test_program_discovery_errors();
    test_declared_psi_interval_is_honored();
    test_command_line_contract();
    return failures == 0 ? 0 : 1;
}
