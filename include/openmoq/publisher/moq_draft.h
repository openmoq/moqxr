#pragma once

#include <string>

namespace openmoq::publisher {

enum class DraftVersion {
    kDraft14,
    kDraft16,
    kDraft17,
    kDraft18,
    kDraft21,
};

struct DraftProfile {
    DraftVersion version = DraftVersion::kDraft16;
    std::string subscribe_namespace_label;
    std::string track_alias_label;
    std::string object_status_label;
    std::string notes;
};

// Draft-21 keeps draft-18's control/request-stream model; behavior shared by
// both keys on this, and draft-21-only differences check kDraft21 directly.
constexpr bool is_draft18_or_later(DraftVersion version) {
    return version == DraftVersion::kDraft18 || version == DraftVersion::kDraft21;
}

DraftProfile draft_profile(DraftVersion version);
std::string to_string(DraftVersion version);
std::string default_alpn(DraftVersion version);

}  // namespace openmoq::publisher
