#include "pending_media_queue.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using openmoq::publisher::transport::MediaAdmission;
using openmoq::publisher::transport::PendingMediaFrontStatus;
using openmoq::publisher::transport::PendingMediaQueue;
using openmoq::publisher::transport::PendingMediaWrite;
using openmoq::publisher::transport::SubgroupDeadlineTracker;

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
        PendingMediaQueue queue(4);
        PendingMediaWrite offset_write = make_write(8, 4);
        offset_write.offset = 3;
        ok &= expect(queue.try_push(std::move(offset_write)) == MediaAdmission::kAccepted,
                     "expected a write with caller-supplied offset to be admitted");
        ok &= expect(queue.front(8, Clock::now()) != nullptr &&
                         queue.front(8, Clock::now())->offset == 0,
                     "expected admission to normalize the write offset to zero");
        ok &= expect(queue.consume(8, 4) == 4,
                     "expected full consumption to account for all admitted bytes");
        ok &= expect(queue.queued_bytes() == 0,
                     "expected full consumption to restore the complete capacity");
        ok &= expect(queue.try_push(make_write(8, 4)) == MediaAdmission::kAccepted,
                     "expected capacity to be reusable after consuming an offset write");
    }

    {
        PendingMediaQueue queue(4);
        PendingMediaWrite fin = make_write(9, 0);
        fin.fin = true;
        ok &= expect(queue.try_push(std::move(fin)) == MediaAdmission::kAccepted,
                     "expected an empty FIN to be admitted");
        ok &= expect(queue.try_push(make_write(9, 2)) == MediaAdmission::kAccepted,
                     "expected a write after an empty FIN to be admitted");
        ok &= expect(queue.front(9, Clock::now()) != nullptr && queue.front(9, Clock::now())->fin,
                     "expected the empty FIN at the stream front");
        ok &= expect(queue.consume(9, 0) == 0,
                     "expected callback-style zero-byte consumption to retire an empty FIN");
        ok &= expect(queue.front(9, Clock::now()) != nullptr &&
                         queue.front(9, Clock::now())->bytes.size() == 2,
                     "expected the following write after the empty FIN to become visible");
        ok &= expect(queue.consume(9, 2) == 2 && queue.front(9, Clock::now()) == nullptr,
                     "expected the stream to empty after consuming the following write");
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
        const auto expired_front = queue.inspect_front(4, now);
        ok &= expect(expired_front.status == PendingMediaFrontStatus::kExpired &&
                         expired_front.write == nullptr,
                     "expected an expired front write to report expiry explicitly");
        ok &= expect(queue.queued_bytes() == 0, "expected expiry to decrement accounting");
        ok &= expect(queue.inspect_front(4, now).status == PendingMediaFrontStatus::kEmpty,
                     "expected expiry to be reported exactly once");

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

    {
        PendingMediaQueue queue(4);
        const auto now = Clock::now();
        PendingMediaWrite expired_subgroup = make_write(6, 3);
        expired_subgroup.subgroup_deadline = now - std::chrono::milliseconds(1);
        ok &= expect(queue.try_push(std::move(expired_subgroup)) == MediaAdmission::kAccepted,
                     "expected a subgroup-expired write to be admitted before eligibility is checked");
        ok &= expect(queue.front(6, now) == nullptr,
                     "expected an offset-zero write with an expired subgroup deadline to be discarded");
        ok &= expect(queue.queued_bytes() == 0,
                     "expected subgroup expiry to remove all of the queued bytes");
    }

    {
        PendingMediaQueue queue(4);
        const auto now = Clock::now();
        PendingMediaWrite expired_fin = make_write(17, 0);
        expired_fin.fin = true;
        expired_fin.object_deadline = now;
        ok &= expect(queue.try_push(std::move(expired_fin)) == MediaAdmission::kAccepted,
                     "expected an empty FIN with a deadline to be admitted");
        ok &= expect(queue.try_push(make_write(17, 4)) == MediaAdmission::kAccepted,
                     "expected a later write for the same stream to be admitted");
        const auto front = queue.inspect_front(17, now);
        ok &= expect(front.status == PendingMediaFrontStatus::kExpired && front.write == nullptr,
                     "expected an expired empty FIN to remain distinguishable from no data");
        ok &= expect(queue.queued_bytes() == 0,
                     "expected one expiry to clear every queued object for the stream");
        ok &= expect(queue.inspect_front(17, now).status == PendingMediaFrontStatus::kEmpty,
                     "expected an expired empty FIN to be reported exactly once");
    }

    {
        SubgroupDeadlineTracker tracker;
        const auto now = Clock::now();
        tracker.arm(41, now + std::chrono::milliseconds(100));
        ok &= expect(tracker.take_expired(now + std::chrono::milliseconds(99)).empty(),
                     "expected an unacknowledged subgroup not to expire early");
        const auto resets = tracker.take_expired(now + std::chrono::milliseconds(100));
        ok &= expect(resets.size() == 1 && resets.front().stream_id == 41 &&
                         resets.front().error_code == 0x02,
                     "expected subgroup expiry to request the exact stream reset and error code");
        ok &= expect(tracker.take_expired(now + std::chrono::milliseconds(101)).empty(),
                     "expected an expired subgroup reset to be reported exactly once");

        tracker.arm(43, now + std::chrono::milliseconds(100));
        tracker.mark_all_data_committed(43);
        ok &= expect(tracker.take_expired(now + std::chrono::milliseconds(101)).empty(),
                     "expected all-data-committed notification to cancel subgroup expiry");
    }

    {
        PendingMediaQueue queue(10);
        ok &= expect(queue.try_push(make_write(11, 8)) == MediaAdmission::kAccepted,
                     "expected a media write to occupy the literal transport-test budget");
        ok &= expect(queue.try_push(make_write(13, 3)) == MediaAdmission::kWouldBlock,
                     "expected combined media writes over the literal transport-test budget to block");
        ok &= expect(queue.clear_connection() == 8,
                     "expected connection close cleanup to report every queued byte");
        ok &= expect(queue.try_push(make_write(15, 10)) == MediaAdmission::kAccepted,
                     "expected connection close cleanup to wake full-budget admission");
    }

    return ok ? 0 : 1;
}
