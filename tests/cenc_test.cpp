#include "openmoq/publisher/cenc.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

void append_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

std::vector<std::uint8_t> make_box(const std::string& type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    append_be32(out, static_cast<std::uint32_t>(8 + payload.size()));
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

}  // namespace

int main() {
    using namespace openmoq::publisher;

    bool ok = true;

    // A container holding three children: aaaa, bbbb, cccc.
    std::vector<std::uint8_t> payload;
    const auto a = make_box("aaaa", {1, 2, 3, 4});
    const auto b = make_box("bbbb", {5, 6});
    const auto c = make_box("cccc", {7, 8, 9});
    payload.insert(payload.end(), a.begin(), a.end());
    payload.insert(payload.end(), b.begin(), b.end());
    payload.insert(payload.end(), c.begin(), c.end());

    const auto found_b = find_child_box_span(payload, 0, payload.size(), 0, "bbbb");
    ok &= expect(found_b.has_value(), "expected to find bbbb");
    ok &= expect(found_b->offset == a.size(), "expected bbbb at the offset after aaaa");
    ok &= expect(found_b->size == b.size(), "expected bbbb's full box size");

    const auto missing = find_child_box_span(payload, 0, payload.size(), 0, "zzzz");
    ok &= expect(!missing.has_value(), "expected absent type to return nullopt");

    // A length-walk must NOT match a type string that appears inside another
    // box's PAYLOAD. A sliding byte scan would falsely match here.
    std::vector<std::uint8_t> decoy_payload;
    const auto decoy = make_box("aaaa", {'c', 'c', 'c', 'c', 0, 0, 0, 16});
    decoy_payload.insert(decoy_payload.end(), decoy.begin(), decoy.end());
    const auto not_found = find_child_box_span(decoy_payload, 0, decoy_payload.size(), 0, "cccc");
    ok &= expect(!not_found.has_value(),
                 "expected a length walk to ignore a type appearing inside a payload");

    // A container size larger than the buffer must not drive reads past the
    // end. This is the memory-safety case: the declared size is fabricated.
    const auto oversized = find_child_box_span(payload, 0, 0x40000000, 0, "zzzz");
    ok &= expect(!oversized.has_value(), "expected a fabricated container size to be clamped");

    // A child whose declared length runs past the container terminates the
    // walk rather than being skipped.
    std::vector<std::uint8_t> truncated;
    append_be32(truncated, 0x40000000);
    truncated.insert(truncated.end(), {'a', 'a', 'a', 'a'});
    const auto bad_len = find_child_box_span(truncated, 0, truncated.size(), 0, "aaaa");
    ok &= expect(!bad_len.has_value(), "expected an over-long child length to terminate the walk");

    // A zero or sub-header length must terminate rather than loop forever.
    std::vector<std::uint8_t> zero_len;
    append_be32(zero_len, 0);
    zero_len.insert(zero_len.end(), {'a', 'a', 'a', 'a'});
    const auto zero = find_child_box_span(zero_len, 0, zero_len.size(), 0, "aaaa");
    ok &= expect(!zero.has_value(), "expected a zero length to terminate the walk");

    return ok ? 0 : 1;
}
