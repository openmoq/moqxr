#pragma once

#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace openmoq::publisher {

struct ObjectProperty {
    std::uint64_t id;
    std::variant<std::uint64_t, std::vector<std::uint8_t>> value;
};

// Returns the draft-18 delta-encoded block, excluding its length. Throws
// std::invalid_argument for duplicate IDs, parity mismatch, or size limits.
std::vector<std::uint8_t> serialize_object_properties(std::span<const ObjectProperty> properties);

}  // namespace openmoq::publisher
