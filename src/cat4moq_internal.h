#pragma once

#include "openmoq/publisher/moq_draft.h"

#include <cstdint>
#include <vector>

namespace openmoq::publisher::cat4moq {

// Draft integer (QUIC varint through draft-16, MoQ vi64 from draft-17).
void append_integer(std::vector<std::uint8_t>& out, std::uint64_t value, DraftVersion draft);

}  // namespace openmoq::publisher::cat4moq
