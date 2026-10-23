#pragma once

#include <cstdint>
#include <string>

namespace openmoq::publisher {

enum class DraftVersion {
    kDraft14,
    kDraft16,
    kDraft17,
    kDraft18,
    kDraft21,
    kDraft22,
};

// Largest Group ID this publisher writes: the QUIC variable-length integer
// maximum, which every supported draft can encode.
inline constexpr std::uint64_t kMaxGroupId = (std::uint64_t{1} << 62) - 1;

struct DraftProfile {
    DraftVersion version = DraftVersion::kDraft18;
    std::string subscribe_namespace_label;
    std::string track_alias_label;
    std::string object_status_label;
    std::string notes;
};

// Modern drafts share the draft-18 control/request-stream model.
constexpr bool is_draft21_or_later(DraftVersion version) {
    return version == DraftVersion::kDraft21 || version == DraftVersion::kDraft22;
}

constexpr bool is_draft18_or_later(DraftVersion version) {
    return version == DraftVersion::kDraft18 || is_draft21_or_later(version);
}

DraftProfile draft_profile(DraftVersion version);
std::string to_string(DraftVersion version);
std::string default_alpn(DraftVersion version);

}  // namespace openmoq::publisher
