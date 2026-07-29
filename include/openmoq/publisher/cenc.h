#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "openmoq/publisher/mp4_box.h"

namespace openmoq::publisher {

// Find a child box by type inside a container, walking the box chain by each
// box's declared length rather than sliding a cursor one byte at a time.
//
// A sliding scan can match four bytes that merely LOOK like a box type inside
// another box's payload -- a real hazard when walking encrypted sample
// entries, where tenc key IDs and PSSH payloads are arbitrary bytes.
//
// container_size is taken from the file and is NOT trusted: it is clamped to
// the real buffer, with a guard against size_t wrap in the sum. A child whose
// declared length is under 8, or which would run past the clamped limit,
// terminates the walk -- a malformed length means the rest cannot be trusted
// to be a box chain at all.
//
// Returns the child's full box span, header included, or nullopt.
std::optional<ByteSpan> find_child_box_span(std::span<const std::uint8_t> bytes,
                                            std::size_t container_offset,
                                            std::size_t container_size,
                                            std::size_t first_child_offset,
                                            std::string_view type);

}  // namespace openmoq::publisher
