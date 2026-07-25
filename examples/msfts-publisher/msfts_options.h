#pragma once

#include "openmoq/publisher/moq_draft.h"
#include "openmoq/publisher/transport/publisher_transport.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace openmoq::examples::msfts {

struct MsftsPublisherOptions {
    std::string endpoint = "https://127.0.0.1:4433/moq";
    std::filesystem::path input_path;
    std::string track_namespace = "msfts";
    std::string track_name = "transport";
    std::uint16_t program = 0;
    std::size_t packets_per_object = 7;
    publisher::DraftVersion draft = publisher::DraftVersion::kDraft17;
    bool insecure = false;
    bool help = false;
};

MsftsPublisherOptions parse_msfts_options(const std::vector<std::string>& arguments);
publisher::transport::EndpointConfig parse_msfts_endpoint(const std::string& value);
std::string msfts_usage(const char* executable);

}  // namespace openmoq::examples::msfts
