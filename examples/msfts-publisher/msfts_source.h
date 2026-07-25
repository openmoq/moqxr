#pragma once

#include "openmoq/publisher/live_object.h"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace openmoq::examples::msfts {

struct MsftsSourceConfig {
    std::filesystem::path input_path;
    std::string track_namespace = "msfts";
    std::string track_name = "transport";
    std::uint16_t requested_program = 0;
    std::size_t packets_per_object = 7;
    std::function<bool()> stop_requested;
};

struct MsftsStreamInfo {
    std::size_t packet_size = 0;
    std::uint16_t program_number = 0;
    std::uint16_t pmt_pid = 0;
    std::uint16_t pcr_pid = 0;
    std::vector<std::uint16_t> selected_pids;
    std::vector<std::uint8_t> init_data;
};

class MsftsSource {
public:
    static std::unique_ptr<MsftsSource> open(MsftsSourceConfig config,
                                             std::string& error);

    const MsftsStreamInfo& info() const noexcept;
    const std::string& error() const noexcept;
    publisher::LiveObjectSource live_source();

private:
    explicit MsftsSource(MsftsSourceConfig config);

    bool initialize();
    bool detect_packet_size(std::streamoff input_size);
    bool collect_initialization();
    bool read_packet(std::vector<std::uint8_t>& packet);
    std::optional<publisher::LiveObject> next_object();
    std::vector<std::uint8_t> make_catalog() const;

    MsftsSourceConfig config_;
    MsftsStreamInfo info_;
    std::ifstream input_;
    std::bitset<8192> selected_pids_;
    std::array<std::uint8_t, 8192> psi_continuity_{};
    std::string error_;
    std::uint64_t next_object_id_ = 0;
    bool catalog_emitted_ = false;
    bool finished_ = false;
};

}  // namespace openmoq::examples::msfts
