#pragma once

#include "openmoq/publisher/mp4_box.h"

#include <atomic>
#include <cstddef>
#include <istream>

namespace openmoq::publisher::transport {

// Reads live input for publish_live, appending to the streaming reader and
// returning the byte count (0 on end of stream or stop request). Defined in
// moqt_session.cpp.
std::size_t read_live_input(std::istream& input,
                            openmoq::publisher::StreamingMp4Reader& reader,
                            const std::atomic<bool>* stop);

}  // namespace openmoq::publisher::transport
