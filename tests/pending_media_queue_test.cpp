#include "pending_media_queue.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using openmoq::publisher::transport::MediaAdmission;
using openmoq::publisher::transport::PendingMediaQueue;
using openmoq::publisher::transport::PendingMediaWrite;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

PendingMediaWrite make_write(std::uint64_t stream_id, std::size_t size,
                             std::uint8_t first_byte = 0) {
    PendingMediaWrite write;
    write.stream_id = stream_id;
    write.bytes.resize(size);
    for (std::size_t index = 0; index < size; ++index) {
        write.bytes[index] = static_cast<std::uint8_t>(first_byte + index);
    }
    return write;
}

}  // namespace

int main() {
    using Clock = std::chrono::steady_clock;
    bool ok = true;

    {
        PendingMediaQueue queue(10);
        ok &= expect(queue.try_push(make_write(1, 8)) == MediaAdmission::kAccepted,
                     "expected a write within the remaining budget to be accepted");
        ok &= expect(queue.try_push(make_write(3, 3)) == MediaAdmission::kWouldBlock,
                     "expected a write over the remaining budget to would-block");
        ok &= expect(queue.queued_bytes() == 8, "expected rejected writes not to affect accounting");

        PendingMediaQueue exact(10);
        ok &= expect(exact.try_push(make_write(1, 10)) == MediaAdmission::kAccepted,
                     "expected a write exactly at capacity to be accepted");
        ok &= expect(exact.queued_bytes() == 10, "expected exact-capacity accounting");

        PendingMediaQueue oversized(10);
        ok &= expect(oversized.try_push(make_write(1, 11)) == MediaAdmission::kOversized,
                     "expected a write larger than capacity to be oversized");
        ok &= expect(oversized.queued_bytes() == 0, "expected an oversized write not to be queued");
    }

    {
        PendingMediaQueue queue(20);
        ok &= expect(queue.try_push(make_write(7, 4, 10)) == MediaAdmission::kAccepted,
                     "expected the first stream write to be accepted");
        ok &= expect(queue.try_push(make_write(7, 5, 20)) == MediaAdmission::kAccepted,
                     "expected the second stream write to be accepted");
        ok &= expect(queue.queued_bytes() == 9, "expected both stream writes in the byte count");

        const auto now = Clock::now();
        const PendingMediaWrite* front = queue.front(7, now);
        ok &= expect(front != nullptr && front->bytes.front() == 10,
                     "expected the first write at the stream front");
        ok &= expect(queue.consume(7, 2) == 2, "expected partial consumption to report two bytes");
        ok &= expect(queue.queued_bytes() == 7, "expected partial consumption to decrement accounting");
        front = queue.front(7, now);
        ok &= expect(front != nullptr && front->offset == 2 && front->bytes[front->offset] == 12,
                     "expected partial consumption to advance the front offset");

        ok &= expect(queue.consume(7, 2) == 2, "expected the rest of the first write to be consumed");
        ok &= expect(queue.queued_bytes() == 5, "expected completed writes removed from accounting");
        front = queue.front(7, now);
        ok &= expect(front != nullptr && front->bytes.front() == 20,
                     "expected FIFO order within a stream");
        ok &= expect(queue.consume(7, 99) == 5, "expected over-consumption to stop at remaining bytes");
        ok &= expect(queue.queued_bytes() == 0 && queue.front(7, now) == nullptr,
                     "expected a fully consumed stream to be empty");
    }

    {
        PendingMediaQueue queue(20);
        queue.try_push(make_write(1, 4));
        queue.try_push(make_write(2, 6));
        ok &= expect(queue.clear_stream(1) == 4, "expected stream clear to report removed bytes");
        ok &= expect(queue.queued_bytes() == 6 && queue.front(1, Clock::now()) == nullptr,
                     "expected stream clear to remove only the selected stream");
        ok &= expect(queue.clear_connection() == 6, "expected connection clear to report removed bytes");
        ok &= expect(queue.queued_bytes() == 0 && queue.front(2, Clock::now()) == nullptr,
                     "expected connection clear to remove every stream");
    }

    {
        PendingMediaQueue queue(20);
        const auto now = Clock::now();
        PendingMediaWrite expired = make_write(4, 3);
        expired.object_deadline = now - std::chrono::milliseconds(1);
        ok &= expect(queue.try_push(std::move(expired)) == MediaAdmission::kAccepted,
                     "expected an expired write to be admitted before eligibility is checked");
        ok &= expect(queue.front(4, now) == nullptr, "expected an expired front write to be discarded");
        ok &= expect(queue.queued_bytes() == 0, "expected expiry to decrement accounting");

        PendingMediaWrite partial = make_write(5, 4);
        partial.subgroup_deadline = now + std::chrono::milliseconds(1);
        ok &= expect(queue.try_push(std::move(partial)) == MediaAdmission::kAccepted,
                     "expected a write with a future subgroup deadline to be admitted");
        ok &= expect(queue.front(5, now) != nullptr, "expected a not-yet-expired write to be available");
        ok &= expect(queue.consume(5, 1) == 1, "expected one byte to be consumed before the deadline");
        ok &= expect(queue.front(5, now + std::chrono::seconds(1)) != nullptr,
                     "expected expiry not to discard a write after its first byte");
        ok &= expect(queue.clear_connection() == 3, "expected the partially consumed remainder to clear");
    }

    return ok ? 0 : 1;
}
