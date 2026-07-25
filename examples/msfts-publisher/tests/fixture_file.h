#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

class FixtureFile {
public:
    FixtureFile(const std::vector<std::uint8_t>& bytes,
                const std::string& suffix) {
        static std::atomic<std::uint64_t> next_id = 0;
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("openmoq-msfts-" + suffix + "-" +
                 std::to_string(timestamp) + "-" +
                 std::to_string(next_id.fetch_add(1)) + ".bin");

        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            throw std::runtime_error("failed to write MSFTS test fixture");
        }
    }

    ~FixtureFile() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    FixtureFile(const FixtureFile&) = delete;
    FixtureFile& operator=(const FixtureFile&) = delete;

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};
