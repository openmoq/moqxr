#pragma once

// The error code of a STOP_SENDING the peer sent on a stream. picoquic keeps
// it in stream->remote_stop_error but has no public getter, and the
// picoquic_callback_stop_sending event does not pass it, so read it from the
// stream head. Only valid on the packet-loop thread, like the other picoquic
// stream calls.

#include <picoquic.h>
#include <picoquic_internal.h>

#include <cstdint>

namespace openmoq::publisher::transport {

inline std::uint64_t picoquic_remote_stop_error(picoquic_cnx_t* cnx, std::uint64_t stream_id) {
    const picoquic_stream_head_t* stream = picoquic_find_stream(cnx, stream_id);
    return stream != nullptr ? stream->remote_stop_error : 0;
}

}  // namespace openmoq::publisher::transport
