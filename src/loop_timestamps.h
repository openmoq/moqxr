#pragma once

#include "openmoq/publisher/object_properties.h"

#include <cstdint>
#include <vector>

namespace openmoq::publisher {

// Rebasing of media timestamps for --loop. Each loop cycle replays the same
// source objects, so their media timestamps must advance by the cycle length
// or a subscriber's media timeline jumps back to the start on every loop.

// The offset for loop cycle `cycle`, in `timescale` ticks, rounded to the
// nearest tick. Computed from the whole elapsed time rather than as
// `cycle * per-cycle ticks`, so rounding error does not accumulate.
std::uint64_t loop_offset_ticks(std::uint64_t cycle_duration_us, std::uint64_t cycle, std::uint32_t timescale);

// Adds `delta_ticks` to the tfdt baseMediaDecodeTime of every moof/traf in a
// CMAF object (moof+mdat, with any leading boxes). A version 0 tfdt that
// would overflow is widened to version 1, adjusting the enclosing box sizes
// and the moof-relative trun data offsets and saio offsets. Returns false and
// leaves the payload unchanged if it cannot be parsed or safely rewritten.
bool rebase_cmaf_timestamps(std::vector<std::uint8_t>& payload, std::uint64_t delta_ticks);

// Adds `delta_ticks` to the baseMediaDecodeTime (field 10) of a LOCMAF
// object, or to the tfdt of a raw (type 4) object's embedded CMAF chunk.
// Delta objects (type 3) carry no base time and are left unchanged. Returns
// false and leaves the payload unchanged if it cannot be parsed.
bool rebase_locmaf_timestamps(std::vector<std::uint8_t>& payload, std::uint64_t delta_ticks);

// Adds `delta_ticks` to the LOC presentation timestamp (property 16).
void rebase_loc_timestamps(std::vector<ObjectProperty>& properties, std::uint64_t delta_ticks);

}  // namespace openmoq::publisher
