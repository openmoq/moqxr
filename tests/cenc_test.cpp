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

std::vector<std::uint8_t> make_full_box(const std::string& type,
                                        std::uint8_t version,
                                        std::uint32_t flags,
                                        const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    out.push_back(version);
    out.push_back(static_cast<std::uint8_t>((flags >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((flags >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(flags & 0xFFU));
    out.insert(out.end(), payload.begin(), payload.end());
    return make_box(type, out);
}

// 16 distinct bytes so a transposed read is visible.
std::vector<std::uint8_t> sample_kid() {
    return {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
            0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
}

// tenc: version 0, flags 0, reserved(1), reserved(1),
// default_isProtected(1), default_Per_Sample_IV_Size(1), default_KID(16).
std::vector<std::uint8_t> make_tenc(std::uint8_t is_protected, std::uint8_t iv_size) {
    std::vector<std::uint8_t> payload{0, 0, is_protected, iv_size};
    const auto kid = sample_kid();
    payload.insert(payload.end(), kid.begin(), kid.end());
    return make_full_box("tenc", 0, 0, payload);
}

// An encv sample entry: 8-byte header + 70-byte VisualSampleEntry body,
// then children. sinf holds frma, schm, and schi/tenc.
std::vector<std::uint8_t> make_encv_sample_entry(const std::string& original_format,
                                                 const std::string& scheme) {
    std::vector<std::uint8_t> schm_payload;
    schm_payload.insert(schm_payload.end(), scheme.begin(), scheme.end());
    append_be32(schm_payload, 0x00010000);

    std::vector<std::uint8_t> frma_payload(original_format.begin(), original_format.end());

    std::vector<std::uint8_t> schi_payload;
    const auto tenc = make_tenc(1, 8);
    schi_payload.insert(schi_payload.end(), tenc.begin(), tenc.end());

    std::vector<std::uint8_t> sinf_payload;
    const auto frma = make_box("frma", frma_payload);
    const auto schm = make_full_box("schm", 0, 0, schm_payload);
    const auto schi = make_box("schi", schi_payload);
    sinf_payload.insert(sinf_payload.end(), frma.begin(), frma.end());
    sinf_payload.insert(sinf_payload.end(), schm.begin(), schm.end());
    sinf_payload.insert(sinf_payload.end(), schi.begin(), schi.end());

    std::vector<std::uint8_t> body(78, 0);
    const auto sinf = make_box("sinf", sinf_payload);
    body.insert(body.end(), sinf.begin(), sinf.end());
    return make_box("encv", body);
}

std::vector<std::uint8_t> make_pssh(const std::vector<std::uint8_t>& system_id) {
    std::vector<std::uint8_t> payload(system_id.begin(), system_id.end());
    append_be32(payload, 4);
    payload.insert(payload.end(), {0xde, 0xad, 0xbe, 0xef});
    return make_full_box("pssh", 0, 0, payload);
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
    //
    // `payload` above is built by `insert`-based growth, which leaves
    // capacity slack past its logical size (measured: 33 logical / 44
    // capacity) -- a read just past `size()` can land in that allocated-but-
    // unused slack without ASAN flagging it, silently defeating this case as
    // a regression guard. Reconstructing from a begin/end iterator pair
    // (random-access iterators) makes libstdc++ allocate exactly the
    // required size with no slack, the same way `parse_mp4_stream` builds
    // real file buffers via `resize()` in src/mp4_box.cpp. This is the
    // buffer shape that must be used here so that, under ASAN, dropping the
    // clamp in find_child_box_span turns this case into an observed
    // heap-buffer-overflow rather than a silent pass.
    const std::vector<std::uint8_t> exact_sized_payload(payload.begin(), payload.end());
    const auto oversized = find_child_box_span(exact_sized_payload, 0, 0x40000000, 0, "zzzz");
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

    // sinf/frma/schm/schi/tenc parsing from an encv sample entry.
    const auto encv = make_encv_sample_entry("avc1", "cenc");
    const Mp4Box encv_box{
        .type = "encv",
        .span = {.offset = 0, .size = encv.size()},
        .payload = {.offset = 8, .size = encv.size() - 8},
        .children = {},
    };
    const auto protection = parse_track_protection(encv, encv_box);
    ok &= expect(protection.has_value(), "expected an encv sample entry to parse as protected");
    ok &= expect(protection->original_format == "avc1", "expected frma to yield avc1");
    ok &= expect(protection->scheme == "cenc", "expected schm to yield cenc");
    ok &= expect(protection->is_protected, "expected tenc default_isProtected");
    ok &= expect(protection->per_sample_iv_size == 8, "expected tenc IV size 8");
    ok &= expect(protection->default_kid == "01234567-89ab-cdef-0123-456789abcdef",
                 "expected the KID rendered as a UUID string");

    // cbcs is the other allowed scheme (CMSF 4.1.1.3).
    const auto encv_cbcs = make_encv_sample_entry("avc1", "cbcs");
    const Mp4Box cbcs_box{
        .type = "encv",
        .span = {.offset = 0, .size = encv_cbcs.size()},
        .payload = {.offset = 8, .size = encv_cbcs.size() - 8},
        .children = {},
    };
    const auto cbcs = parse_track_protection(encv_cbcs, cbcs_box);
    ok &= expect(cbcs.has_value() && cbcs->scheme == "cbcs", "expected cbcs scheme parsed");

    // An unencrypted sample entry has no sinf and must parse as unprotected.
    std::vector<std::uint8_t> plain_body(78, 0);
    const auto plain = make_box("avc1", plain_body);
    const Mp4Box plain_box{
        .type = "avc1",
        .span = {.offset = 0, .size = plain.size()},
        .payload = {.offset = 8, .size = plain.size() - 8},
        .children = {},
    };
    ok &= expect(!parse_track_protection(plain, plain_box).has_value(),
                 "expected an unencrypted sample entry to yield no protection");

    // Two pssh boxes yield two systems, each with its own base64 payload.
    const std::vector<std::uint8_t> widevine{0xed, 0xef, 0x8b, 0xa9, 0x79, 0xd6, 0x4a, 0xce,
                                             0xa3, 0xc8, 0x27, 0xdc, 0xd5, 0x1d, 0x21, 0xed};
    const std::vector<std::uint8_t> playready{0x9a, 0x04, 0xf0, 0x79, 0x98, 0x40, 0x42, 0x86,
                                              0xab, 0x92, 0xe6, 0x5b, 0xe0, 0x88, 0x5f, 0x95};
    std::vector<std::uint8_t> two_pssh;
    const auto p1 = make_pssh(widevine);
    const auto p2 = make_pssh(playready);
    two_pssh.insert(two_pssh.end(), p1.begin(), p1.end());
    two_pssh.insert(two_pssh.end(), p2.begin(), p2.end());
    std::vector<Mp4Box> pssh_boxes{
        Mp4Box{.type = "pssh", .span = {.offset = 0, .size = p1.size()},
               .payload = {.offset = 8, .size = p1.size() - 8}, .children = {}},
        Mp4Box{.type = "pssh", .span = {.offset = p1.size(), .size = p2.size()},
               .payload = {.offset = p1.size() + 8, .size = p2.size() - 8}, .children = {}},
    };
    const auto systems = parse_pssh_boxes(two_pssh, pssh_boxes);
    ok &= expect(systems.size() == 2, "expected two DRM systems");
    ok &= expect(systems[0].system_id == "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed",
                 "expected the Widevine system ID");
    ok &= expect(systems[1].system_id == "9a04f079-9840-4286-ab92-e65be0885f95",
                 "expected the PlayReady system ID");
    ok &= expect(!systems[0].pssh_base64.empty(), "expected base64 pssh bytes");
    ok &= expect(systems[0].pssh_base64 != systems[1].pssh_base64,
                 "expected distinct pssh payloads per system");
    // Independently computed (Python's base64.b64encode, not this code) from
    // p1's exact 36 bytes, so a swapped 6-bit-group or alphabet bug in
    // base64_encode can't hide behind only a non-empty/distinct check.
    ok &= expect(systems[0].pssh_base64 ==
                     "AAAAJHBzc2gAAAAA7e+LqXnWSs6jyCfc1R0h7QAAAATerb7v",
                 "expected the exact base64 encoding of the Widevine pssh box");

    // A pssh box with a declared length under 28 bytes is skipped, but a
    // valid box in the same list is still returned -- paired so the test
    // can't pass against a function that simply returned nothing at all.
    std::vector<Mp4Box> short_and_valid{
        Mp4Box{.type = "pssh", .span = {.offset = 0, .size = 16},
               .payload = {.offset = 8, .size = 8}, .children = {}},
        Mp4Box{.type = "pssh", .span = {.offset = 0, .size = p1.size()},
               .payload = {.offset = 8, .size = p1.size() - 8}, .children = {}},
    };
    const auto short_skip = parse_pssh_boxes(p1, short_and_valid);
    ok &= expect(short_skip.size() == 1, "expected the under-length pssh box to be skipped");
    ok &= expect(!short_skip.empty() && short_skip[0].system_id ==
                     "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed",
                 "expected the valid pssh box alongside it to still be returned");

    // A pssh box whose declared span runs past the buffer is skipped, again
    // paired with a valid box.
    std::vector<Mp4Box> truncated_and_valid{
        Mp4Box{.type = "pssh", .span = {.offset = 0, .size = 1000},
               .payload = {.offset = 8, .size = 992}, .children = {}},
        Mp4Box{.type = "pssh", .span = {.offset = 0, .size = p1.size()},
               .payload = {.offset = 8, .size = p1.size() - 8}, .children = {}},
    };
    const auto truncated_skip = parse_pssh_boxes(p1, truncated_and_valid);
    ok &= expect(truncated_skip.size() == 1,
                 "expected the past-the-buffer pssh box to be skipped");
    ok &= expect(!truncated_skip.empty() && truncated_skip[0].system_id ==
                     "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed",
                 "expected the valid pssh box alongside it to still be returned");

    return ok ? 0 : 1;
}
