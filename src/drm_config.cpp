#include "openmoq/publisher/drm_config.h"

#include "json_reader.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace openmoq::publisher {

namespace {

using namespace openmoq::publisher::internal;

// There is no optional-string accessor in json_reader.h (only
// read_optional_u32 exists, for numbers). laURL, laURLType, certURL,
// certURLType, and robustness are all optional strings, so this small helper
// stays local to this file rather than growing json_reader.h, which was
// extracted verbatim in a previous step specifically to remain
// behaviour-preserving.
std::optional<std::string> read_optional_string(const JsonObject& object, std::string_view key) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        return std::nullopt;
    }
    if (std::holds_alternative<std::nullptr_t>(it->second.value)) {
        return std::nullopt;
    }
    const auto* text = std::get_if<std::string>(&it->second.value);
    if (text == nullptr) {
        throw std::runtime_error("JSON key must be a string: " + std::string(key));
    }
    return *text;
}

}  // namespace

std::vector<DrmSystemConfig> parse_drm_config_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("drm config: failed to open " + path);
    }

    std::string json_text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    std::vector<DrmSystemConfig> systems;
    try {
        JsonParser parser(std::move(json_text));
        const JsonValue root = parser.parse();
        const JsonObject& root_obj = expect_object(root, "root");

        const auto systems_it = root_obj.find("systems");
        if (systems_it == root_obj.end()) {
            throw std::runtime_error("missing JSON key: systems");
        }
        const JsonArray& system_values = expect_array(systems_it->second, "systems");

        systems.reserve(system_values.size());
        for (const JsonValue& system_value : system_values) {
            const JsonObject& system_obj = expect_object(system_value, "systems[]");

            DrmSystemConfig system;
            system.system_id = expect_string(system_obj, "systemID");
            system.la_url = read_optional_string(system_obj, "laURL");
            system.la_url_type = read_optional_string(system_obj, "laURLType");
            system.cert_url = read_optional_string(system_obj, "certURL");
            system.cert_url_type = read_optional_string(system_obj, "certURLType");
            system.robustness = read_optional_string(system_obj, "robustness");
            systems.push_back(std::move(system));
        }
    } catch (const std::exception& error) {
        throw std::runtime_error("drm config: " + path + ": " + error.what());
    }

    return systems;
}

}  // namespace openmoq::publisher
