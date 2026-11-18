#pragma once

#include <optional>
#include <string_view>

namespace openmoq::publisher {

// The MSF draft a catalog conforms to. The catalog `version` string selects the
// profile (MSF section 5.1.1), so MsfCatalog::version stays the single source of
// truth and a delta catalog, which omits `version` on the wire, still knows it.
enum class MsfVersion {
    k01,  // draft-ietf-moq-msf-01, version "1"
    k02,  // draft-ietf-moq-msf-02, version "draft-02"
};

constexpr std::string_view msf_version_string(MsfVersion version) {
    return version == MsfVersion::k01 ? "1" : "draft-02";
}

constexpr std::optional<MsfVersion> msf_version_from_string(std::string_view text) {
    if (text == "1") {
        return MsfVersion::k01;
    }
    if (text == "draft-02") {
        return MsfVersion::k02;
    }
    return std::nullopt;
}

}  // namespace openmoq::publisher
