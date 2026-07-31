#include "msfts_source.h"

#include "openmoq/publisher/msf_catalog.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace openmoq::examples::msfts {

namespace {

constexpr std::size_t kTsPacketSize = 188;
constexpr std::size_t kM2tsPacketSize = 192;
constexpr std::size_t kM2tsPrefixSize = 4;
constexpr std::size_t kMaxPsiScanPackets = 4096;
constexpr std::uint64_t kObjectDurationUs = 10'000;
constexpr std::uint64_t kPsiIntervalMs = 500;
constexpr std::uint64_t kPsiIntervalObjects =
    (kPsiIntervalMs * 1000) / kObjectDurationUs;

const std::uint8_t* ts_view(const std::vector<std::uint8_t>& packet,
                            std::size_t packet_size) {
    return packet.data() + (packet_size == kM2tsPacketSize ? kM2tsPrefixSize : 0);
}

std::uint8_t* ts_view(std::vector<std::uint8_t>& packet,
                      std::size_t packet_size) {
    return packet.data() + (packet_size == kM2tsPacketSize ? kM2tsPrefixSize : 0);
}

std::uint16_t packet_pid(const std::uint8_t* ts) {
    return static_cast<std::uint16_t>(((ts[1] & 0x1f) << 8) | ts[2]);
}

int payload_offset(const std::uint8_t* ts) {
    const int adaptation_control = (ts[3] >> 4) & 0x03;
    if (adaptation_control == 0 || adaptation_control == 2) {
        return -1;
    }
    int offset = 4;
    if (adaptation_control == 3) {
        offset += 1 + ts[4];
    }
    return offset < static_cast<int>(kTsPacketSize) ? offset : -1;
}

bool extract_psi_section(const std::uint8_t* ts,
                         const std::uint8_t*& section,
                         std::size_t& section_length) {
    if ((ts[1] & 0x40) == 0) {
        return false;
    }
    const int offset = payload_offset(ts);
    if (offset < 0 || offset >= static_cast<int>(kTsPacketSize)) {
        return false;
    }
    const int start = offset + 1 + ts[offset];
    if (start + 3 > static_cast<int>(kTsPacketSize)) {
        return false;
    }
    const std::size_t length =
        3 + static_cast<std::size_t>(((ts[start + 1] & 0x0f) << 8) |
                                     ts[start + 2]);
    if (length < 8 || start + static_cast<int>(length) >
                              static_cast<int>(kTsPacketSize)) {
        return false;
    }
    section = ts + start;
    section_length = length;
    return true;
}

bool select_pat_program(const std::uint8_t* section,
                        std::size_t length,
                        std::uint16_t requested_program,
                        std::uint16_t& program_number,
                        std::uint16_t& pmt_pid) {
    if (length < 12 || section[0] != 0x00) {
        return false;
    }
    const int section_length = ((section[1] & 0x0f) << 8) | section[2];
    const int entries_end = 3 + section_length - 4;
    if (entries_end > static_cast<int>(length)) {
        return false;
    }
    for (int offset = 8; offset + 4 <= entries_end; offset += 4) {
        const auto program =
            static_cast<std::uint16_t>((section[offset] << 8) | section[offset + 1]);
        const auto pid = static_cast<std::uint16_t>(
            ((section[offset + 2] & 0x1f) << 8) | section[offset + 3]);
        if (program != 0 &&
            (requested_program == 0 || requested_program == program)) {
            program_number = program;
            pmt_pid = pid;
            return true;
        }
    }
    return false;
}

bool parse_pmt(const std::uint8_t* section,
               std::size_t length,
               std::uint16_t& pcr_pid,
               std::bitset<8192>& selected_pids) {
    if (length < 16 || section[0] != 0x02) {
        return false;
    }
    const int section_length = ((section[1] & 0x0f) << 8) | section[2];
    const int section_end = 3 + section_length - 4;
    if (section_end > static_cast<int>(length) || section_end < 12) {
        return false;
    }
    pcr_pid = static_cast<std::uint16_t>(
        ((section[8] & 0x1f) << 8) | section[9]);
    selected_pids.set(pcr_pid);

    const int program_info_length =
        ((section[10] & 0x0f) << 8) | section[11];
    int offset = 12 + program_info_length;
    while (offset + 5 <= section_end) {
        const auto elementary_pid = static_cast<std::uint16_t>(
            ((section[offset + 1] & 0x1f) << 8) | section[offset + 2]);
        const int es_info_length =
            ((section[offset + 3] & 0x0f) << 8) | section[offset + 4];
        selected_pids.set(elementary_pid);
        offset += 5 + es_info_length;
    }
    return true;
}

std::uint32_t mpeg_crc32(const std::uint8_t* data, std::size_t length) {
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t index = 0; index < length; ++index) {
        crc ^= static_cast<std::uint32_t>(data[index]) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000u) != 0
                      ? (crc << 1) ^ 0x04c11db7u
                      : crc << 1;
        }
    }
    return crc;
}

bool rewrite_pat(std::vector<std::uint8_t>& packet,
                 std::size_t packet_size,
                 std::uint16_t program_number,
                 std::uint16_t pmt_pid) {
    std::uint8_t* ts = ts_view(packet, packet_size);
    if (ts[0] != 0x47 || packet_pid(ts) != 0 || (ts[1] & 0x40) == 0) {
        return false;
    }
    const int offset = payload_offset(ts);
    if (offset < 0 || offset + 17 > static_cast<int>(kTsPacketSize)) {
        return false;
    }

    std::array<std::uint8_t, 16> section{
        0x00, 0xb0, 0x0d, 0x00, 0x01, 0xc1, 0x00, 0x00,
        static_cast<std::uint8_t>(program_number >> 8),
        static_cast<std::uint8_t>(program_number),
        static_cast<std::uint8_t>(0xe0 | ((pmt_pid >> 8) & 0x1f)),
        static_cast<std::uint8_t>(pmt_pid),
        0x00, 0x00, 0x00, 0x00,
    };
    const std::uint32_t crc = mpeg_crc32(section.data(), 12);
    section[12] = static_cast<std::uint8_t>(crc >> 24);
    section[13] = static_cast<std::uint8_t>(crc >> 16);
    section[14] = static_cast<std::uint8_t>(crc >> 8);
    section[15] = static_cast<std::uint8_t>(crc);

    ts[offset] = 0;
    std::copy(section.begin(), section.end(), ts + offset + 1);
    std::fill(ts + offset + 1 + section.size(), ts + kTsPacketSize, 0xff);
    return true;
}

std::string base64_encode(const std::vector<std::uint8_t>& input) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < input.size(); index += 3) {
        std::uint32_t value = static_cast<std::uint32_t>(input[index]) << 16;
        const bool have_second = index + 1 < input.size();
        const bool have_third = index + 2 < input.size();
        if (have_second) {
            value |= static_cast<std::uint32_t>(input[index + 1]) << 8;
        }
        if (have_third) {
            value |= input[index + 2];
        }
        output.push_back(kAlphabet[(value >> 18) & 0x3f]);
        output.push_back(kAlphabet[(value >> 12) & 0x3f]);
        output.push_back(have_second ? kAlphabet[(value >> 6) & 0x3f] : '=');
        output.push_back(have_third ? kAlphabet[value & 0x3f] : '=');
    }
    return output;
}

}  // namespace

MsftsSource::MsftsSource(MsftsSourceConfig config)
    : config_(std::move(config)) {}

std::unique_ptr<MsftsSource> MsftsSource::open(MsftsSourceConfig config,
                                               std::string& error) {
    auto source = std::unique_ptr<MsftsSource>(new MsftsSource(std::move(config)));
    if (!source->initialize()) {
        error = source->error_;
        return nullptr;
    }
    error.clear();
    return source;
}

const MsftsStreamInfo& MsftsSource::info() const noexcept {
    return info_;
}

const std::string& MsftsSource::error() const noexcept {
    return error_;
}

bool MsftsSource::initialize() {
    if (config_.input_path.empty()) {
        error_ = "input path is required";
        return false;
    }
    if (config_.track_namespace.empty() || config_.track_name.empty()) {
        error_ = "namespace and track names must not be empty";
        return false;
    }
    if (config_.packets_per_object == 0) {
        error_ = "packets per object must be greater than zero";
        return false;
    }

    input_.open(config_.input_path, std::ios::binary);
    if (!input_) {
        error_ = "failed to open input: " + config_.input_path.string();
        return false;
    }
    input_.seekg(0, std::ios::end);
    const std::streamoff input_size = input_.tellg();
    if (input_size <= 0) {
        error_ = "input is empty or its size could not be read";
        return false;
    }
    input_.clear();
    input_.seekg(0);
    if (!detect_packet_size(input_size)) {
        return false;
    }
    if (input_size % static_cast<std::streamoff>(info_.packet_size) != 0) {
        error_ = "input contains a partial or unaligned source packet";
        return false;
    }
    input_.clear();
    input_.seekg(0);
    return collect_initialization();
}

bool MsftsSource::detect_packet_size(std::streamoff input_size) {
    std::array<std::uint8_t, kM2tsPacketSize * 2> probe{};
    input_.read(reinterpret_cast<char*>(probe.data()),
                static_cast<std::streamsize>(probe.size()));
    const std::size_t size = static_cast<std::size_t>(input_.gcount());
    input_.clear();
    input_.seekg(0);

    const bool aligned_m2ts =
        input_size % static_cast<std::streamoff>(kM2tsPacketSize) == 0 &&
        size >= kM2tsPacketSize && probe[kM2tsPrefixSize] == 0x47 &&
        (size < kM2tsPacketSize * 2 ||
         probe[kM2tsPacketSize + kM2tsPrefixSize] == 0x47);
    if (aligned_m2ts) {
        info_.packet_size = kM2tsPacketSize;
        return true;
    }
    const bool aligned_ts =
        input_size % static_cast<std::streamoff>(kTsPacketSize) == 0 &&
        size >= kTsPacketSize && probe[0] == 0x47 &&
        (size < kTsPacketSize * 2 || probe[kTsPacketSize] == 0x47);
    if (aligned_ts) {
        info_.packet_size = kTsPacketSize;
        return true;
    }
    error_ =
        "input has a partial or invalid 188-byte TS / 192-byte M2TS packet alignment";
    return false;
}

bool MsftsSource::read_packet(std::vector<std::uint8_t>& packet) {
    packet.resize(info_.packet_size);
    input_.read(reinterpret_cast<char*>(packet.data()),
                static_cast<std::streamsize>(packet.size()));
    const std::size_t size = static_cast<std::size_t>(input_.gcount());
    if (size == 0) {
        packet.clear();
        return false;
    }
    if (size != info_.packet_size) {
        error_ = "input ended with a partial source packet";
        packet.clear();
        return false;
    }
    if (ts_view(packet, info_.packet_size)[0] != 0x47) {
        error_ = "input contains a source packet without an MPEG-2 TS sync byte";
        packet.clear();
        return false;
    }
    return true;
}

bool MsftsSource::collect_initialization() {
    std::vector<std::uint8_t> pat;
    std::vector<std::uint8_t> pmt;
    for (std::size_t index = 0; index < kMaxPsiScanPackets; ++index) {
        std::vector<std::uint8_t> packet;
        if (!read_packet(packet)) {
            break;
        }
        const std::uint8_t* ts = ts_view(packet, info_.packet_size);
        const std::uint8_t* section = nullptr;
        std::size_t section_length = 0;
        const std::uint16_t pid = packet_pid(ts);
        if (pat.empty() && pid == 0 &&
            extract_psi_section(ts, section, section_length) &&
            select_pat_program(section,
                               section_length,
                               config_.requested_program,
                               info_.program_number,
                               info_.pmt_pid)) {
            selected_pids_.set(0);
            selected_pids_.set(info_.pmt_pid);
            if (!rewrite_pat(packet,
                             info_.packet_size,
                             info_.program_number,
                             info_.pmt_pid)) {
                error_ = "failed to rewrite PAT for selected program";
                return false;
            }
            pat = std::move(packet);
        } else if (!pat.empty() && pmt.empty() && pid == info_.pmt_pid &&
                   extract_psi_section(ts, section, section_length) &&
                   parse_pmt(section,
                             section_length,
                             info_.pcr_pid,
                             selected_pids_)) {
            pmt = std::move(packet);
        }
        if (!pat.empty() && !pmt.empty()) {
            break;
        }
    }

    if (pat.empty()) {
        error_ = config_.requested_program == 0
                     ? "failed to find a PAT program in the source"
                     : "failed to find requested program " +
                           std::to_string(config_.requested_program) + " in the PAT";
        return false;
    }
    if (pmt.empty() || selected_pids_.count() <= 2) {
        error_ = "failed to find a valid PMT for selected program " +
                 std::to_string(info_.program_number);
        return false;
    }

    info_.selected_pids.clear();
    for (std::size_t pid = 0; pid < selected_pids_.size(); ++pid) {
        if (selected_pids_.test(pid)) {
            info_.selected_pids.push_back(static_cast<std::uint16_t>(pid));
        }
    }
    info_.init_data.reserve(info_.packet_size * 2);
    info_.init_data.insert(info_.init_data.end(), pat.begin(), pat.end());
    info_.init_data.insert(info_.init_data.end(), pmt.begin(), pmt.end());
    input_.clear();
    input_.seekg(0);
    return true;
}

std::vector<std::uint8_t> MsftsSource::make_catalog() const {
    using openmoq::publisher::MsfCatalog;
    using openmoq::publisher::MsfInitData;
    using openmoq::publisher::MsfTrack;

    MsfCatalog catalog;
    catalog.generated_at_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    MsfTrack track;
    track.name = config_.track_name;
    track.name_space = config_.track_namespace;
    // "m2ts" is not one of MSF v1's Table 3 packaging values; it is defined by
    // draft-gregoire-moq-msfts section 6 ("Catalog"), which this example
    // implements. That draft tracks draft-ietf-moq-msf-01, so the catalog this
    // builds -- string "version", root initDataList plus track initRef -- is
    // the shape both documents now agree on. See docs/protocol-mapping.md
    // "MSF v1 catalog" for the caveat.
    track.packaging = "m2ts";
    track.role = "video";
    track.is_live = true;
    track.mime_type = "video/mp2t";
    track.target_latency_ms = 1000;
    // No measured rate is available from an M2TS source; 2 Mbps matches the
    // codec-class default resolve_bitrate applies elsewhere.
    track.bitrate = 2000000;
    track.init_ref = "m2ts-init";

    track.custom_fields["m2tsPacketSize"] = std::to_string(info_.packet_size);
    track.custom_fields["m2tsPacketsPerObject"] = std::to_string(config_.packets_per_object);
    track.custom_fields["m2tsProgramNumber"] = std::to_string(info_.program_number);
    track.custom_fields["m2tsPmtPid"] = std::to_string(info_.pmt_pid);
    track.custom_fields["m2tsPcrPid"] = std::to_string(info_.pcr_pid);
    track.custom_fields["m2tsPsiInterval"] = std::to_string(kPsiIntervalMs);
    track.custom_fields["m2tsRandomAccess"] = "false";
    if (info_.packet_size == kM2tsPacketSize) {
        // A raw JSON value, so the string must arrive already quoted.
        track.custom_fields["m2tsTimestampMode"] = "\"opaque\"";
    }
    catalog.tracks.push_back(std::move(track));

    catalog.init_data_list.push_back(MsfInitData{
        .id = "m2ts-init",
        .type = "inline",
        .data = base64_encode(info_.init_data),
    });

    const std::string json = openmoq::publisher::serialize_catalog(catalog);
    return {json.begin(), json.end()};
}

std::optional<publisher::LiveObject> MsftsSource::next_object() {
    if (finished_ || (config_.stop_requested && config_.stop_requested())) {
        finished_ = true;
        return std::nullopt;
    }
    if (!catalog_emitted_) {
        catalog_emitted_ = true;
        return publisher::LiveObject{
            .track_name = "catalog",
            .group_id = 0,
            .object_id = 0,
            .payload = make_catalog(),
        };
    }

    std::vector<std::uint8_t> payload;
    if (next_object_id_ % kPsiIntervalObjects == 0) {
        for (std::size_t offset = 0; offset < info_.init_data.size();
             offset += info_.packet_size) {
            std::vector<std::uint8_t> packet(
                info_.init_data.begin() + static_cast<std::ptrdiff_t>(offset),
                info_.init_data.begin() +
                    static_cast<std::ptrdiff_t>(offset + info_.packet_size));
            const std::uint16_t pid =
                packet_pid(ts_view(packet, info_.packet_size));
            std::uint8_t* ts = ts_view(packet, info_.packet_size);
            ts[3] = static_cast<std::uint8_t>(
                (ts[3] & 0xf0) | psi_continuity_[pid]);
            psi_continuity_[pid] =
                static_cast<std::uint8_t>((psi_continuity_[pid] + 1) & 0x0f);
            payload.insert(payload.end(), packet.begin(), packet.end());
        }
    }

    std::size_t selected_packet_count = 0;
    while (selected_packet_count < config_.packets_per_object) {
        std::vector<std::uint8_t> packet;
        if (!read_packet(packet)) {
            if (!error_.empty()) {
                finished_ = true;
                return std::nullopt;
            }
            break;
        }
        const std::uint16_t pid =
            packet_pid(ts_view(packet, info_.packet_size));
        if (pid != 0x1fff && !selected_pids_.test(pid)) {
            continue;
        }
        if (pid == 0 &&
            !rewrite_pat(packet,
                         info_.packet_size,
                         info_.program_number,
                         info_.pmt_pid)) {
            error_ = "failed to rewrite PAT while streaming";
            finished_ = true;
            return std::nullopt;
        }
        if (pid == 0 || pid == info_.pmt_pid) {
            std::uint8_t* ts = ts_view(packet, info_.packet_size);
            ts[3] = static_cast<std::uint8_t>(
                (ts[3] & 0xf0) | psi_continuity_[pid]);
            psi_continuity_[pid] =
                static_cast<std::uint8_t>((psi_continuity_[pid] + 1) & 0x0f);
        }
        payload.insert(payload.end(), packet.begin(), packet.end());
        ++selected_packet_count;
    }

    if (selected_packet_count == 0) {
        finished_ = true;
        return std::nullopt;
    }
    const std::uint64_t object_id = next_object_id_++;
    return publisher::LiveObject{
        .track_name = config_.track_name,
        .group_id = 0,
        .object_id = static_cast<std::size_t>(object_id),
        .media_time_us = object_id * kObjectDurationUs,
        .media_duration_us = kObjectDurationUs,
        .payload = std::move(payload),
    };
}

publisher::LiveObjectSource MsftsSource::live_source() {
    publisher::LiveTrack catalog_track;
    catalog_track.track_name = "catalog";
    publisher::LiveTrack media_track;
    media_track.track_name = config_.track_name;
    return publisher::LiveObjectSource{
        .tracks = {
            std::move(catalog_track),
            std::move(media_track),
        },
        .next_object = [this]() { return next_object(); },
        .is_finished = [this]() { return finished_; },
        .catalog_mode = publisher::LiveCatalogMode::kSourceObject,
    };
}

}  // namespace openmoq::examples::msfts
