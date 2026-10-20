#include "loop_timestamps.h"

#include "openmoq/publisher/cmsf_packager.h"
#include "openmoq/publisher/locmaf_encoder.h"
#include "openmoq/publisher/mp4_box.h"
#include "openmoq/publisher/publisher_api.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using openmoq::publisher::find_child_box;
using openmoq::publisher::LocmafEncoder;
using openmoq::publisher::loop_offset_ticks;
using openmoq::publisher::Mp4Box;
using openmoq::publisher::ObjectProperty;
using openmoq::publisher::parse_mp4_boxes;
using openmoq::publisher::rebase_cmaf_timestamps;
using openmoq::publisher::rebase_loc_timestamps;
using openmoq::publisher::rebase_locmaf_timestamps;
using Bytes = std::vector<std::uint8_t>;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

void append_be32(Bytes& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_be64(Bytes& out, std::uint64_t value) {
    append_be32(out, static_cast<std::uint32_t>(value >> 32U));
    append_be32(out, static_cast<std::uint32_t>(value));
}

std::uint32_t read_be32(const Bytes& bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) | static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint64_t read_be64(const Bytes& bytes, std::size_t offset) {
    return (static_cast<std::uint64_t>(read_be32(bytes, offset)) << 32U) | read_be32(bytes, offset + 4);
}

Bytes box(std::string_view type, const Bytes& payload) {
    Bytes out;
    append_be32(out, static_cast<std::uint32_t>(8 + payload.size()));
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Bytes full_box(std::string_view type, std::uint8_t version, std::uint32_t flags, const Bytes& payload) {
    Bytes body;
    append_be32(body, (static_cast<std::uint32_t>(version) << 24U) | flags);
    body.insert(body.end(), payload.begin(), payload.end());
    return box(type, body);
}

Bytes concat(const std::vector<Bytes>& parts) {
    Bytes out;
    for (const auto& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

struct TrafSpec {
    std::uint32_t track_id = 1;
    std::uint8_t tfdt_version = 0;
    std::uint64_t decode_time = 0;
    Bytes sample;
    bool with_saio = false;
};

// moof(mfhd, traf(tfhd default-base-is-moof, tfdt, trun with data offset,
// [saio pointing at the sample])...) followed by one mdat holding every
// traf's sample in order.
Bytes build_fragment(const std::vector<TrafSpec>& trafs) {
    const auto build = [&](const std::vector<std::uint32_t>& data_offsets) {
        std::vector<Bytes> children{full_box("mfhd", 0, 0, Bytes{0, 0, 0, 1})};
        for (std::size_t i = 0; i < trafs.size(); ++i) {
            const auto& spec = trafs[i];
            Bytes tfhd;
            append_be32(tfhd, spec.track_id);
            Bytes tfdt;
            if (spec.tfdt_version == 1) {
                append_be64(tfdt, spec.decode_time);
            } else {
                append_be32(tfdt, static_cast<std::uint32_t>(spec.decode_time));
            }
            Bytes trun;
            append_be32(trun, 1);  // sample_count
            append_be32(trun, data_offsets[i]);
            append_be32(trun, static_cast<std::uint32_t>(spec.sample.size()));
            std::vector<Bytes> traf_children{full_box("tfhd", 0, 0x020000, tfhd),
                                             full_box("tfdt", spec.tfdt_version, 0, tfdt),
                                             full_box("trun", 0, 0x000201, trun)};
            if (spec.with_saio) {
                Bytes saio;
                append_be32(saio, 1);
                append_be32(saio, data_offsets[i]);
                traf_children.push_back(full_box("saio", 0, 0, saio));
            }
            children.push_back(box("traf", concat(traf_children)));
        }
        return box("moof", concat(children));
    };
    std::vector<std::uint32_t> offsets(trafs.size(), 0);
    const std::size_t moof_size = build(offsets).size();
    std::uint32_t next = static_cast<std::uint32_t>(moof_size + 8);
    Bytes mdat_payload;
    for (std::size_t i = 0; i < trafs.size(); ++i) {
        offsets[i] = next;
        next += static_cast<std::uint32_t>(trafs[i].sample.size());
        mdat_payload.insert(mdat_payload.end(), trafs[i].sample.begin(), trafs[i].sample.end());
    }
    return concat({build(offsets), box("mdat", mdat_payload)});
}

struct TrafView {
    std::uint8_t tfdt_version = 0;
    std::uint64_t decode_time = 0;
    Bytes sample_via_trun;
    Bytes sample_via_saio;
};

// Re-parses a fragment and follows each traf's moof-relative offsets back to
// its sample bytes, so a rebase that breaks any offset or size shows up.
bool inspect(const Bytes& fragment, std::vector<TrafView>& views) {
    std::vector<Mp4Box> top;
    try {
        top = parse_mp4_boxes(fragment);
    } catch (const std::exception&) {
        return false;
    }
    if (top.size() != 2 || top[0].type != "moof" || top[1].type != "mdat" ||
        top[0].span.size + top[1].span.size != fragment.size()) {
        return false;
    }
    const Mp4Box& moof = top[0];
    for (const auto& traf : moof.children) {
        if (traf.type != "traf") {
            continue;
        }
        const Mp4Box* tfdt = find_child_box(traf, "tfdt");
        const Mp4Box* trun = find_child_box(traf, "trun");
        if (tfdt == nullptr || trun == nullptr) {
            return false;
        }
        TrafView view;
        view.tfdt_version = fragment[tfdt->payload.offset];
        view.decode_time = view.tfdt_version == 1 ? read_be64(fragment, tfdt->payload.offset + 4)
                                                  : read_be32(fragment, tfdt->payload.offset + 4);
        const std::size_t size = read_be32(fragment, trun->payload.offset + 12);
        const std::size_t start = moof.span.offset + read_be32(fragment, trun->payload.offset + 8);
        if (start + size > fragment.size()) {
            return false;
        }
        view.sample_via_trun.assign(fragment.begin() + static_cast<std::ptrdiff_t>(start),
                                    fragment.begin() + static_cast<std::ptrdiff_t>(start + size));
        if (const Mp4Box* saio = find_child_box(traf, "saio")) {
            const std::size_t aux = moof.span.offset + read_be32(fragment, saio->payload.offset + 8);
            if (aux + size > fragment.size()) {
                return false;
            }
            view.sample_via_saio.assign(fragment.begin() + static_cast<std::ptrdiff_t>(aux),
                                        fragment.begin() + static_cast<std::ptrdiff_t>(aux + size));
        }
        views.push_back(std::move(view));
    }
    return true;
}

// Layout facts that rebasing must preserve for any CMAF fragment: each trun
// data offset relative to the end of its moof, and the mdat bytes.
struct FragmentLayout {
    bool valid = false;
    std::vector<std::uint64_t> decode_times;
    std::vector<std::int64_t> data_offsets_past_moof;
    Bytes mdat;
};

FragmentLayout layout(const Bytes& fragment) {
    FragmentLayout result;
    std::vector<Mp4Box> top;
    try {
        top = parse_mp4_boxes(fragment);
    } catch (const std::exception&) {
        return result;
    }
    for (const auto& moof : top) {
        if (moof.type == "mdat") {
            result.mdat.insert(result.mdat.end(), fragment.begin() + static_cast<std::ptrdiff_t>(moof.span.offset),
                               fragment.begin() + static_cast<std::ptrdiff_t>(moof.span.offset + moof.span.size));
        }
        if (moof.type != "moof") {
            continue;
        }
        for (const auto& traf : moof.children) {
            if (traf.type != "traf") {
                continue;
            }
            const Mp4Box* tfdt = find_child_box(traf, "tfdt");
            if (tfdt == nullptr) {
                return result;
            }
            result.decode_times.push_back(fragment[tfdt->payload.offset] == 1
                                              ? read_be64(fragment, tfdt->payload.offset + 4)
                                              : read_be32(fragment, tfdt->payload.offset + 4));
            for (const auto& child : traf.children) {
                if (child.type == "trun" && (read_be32(fragment, child.payload.offset) & 1U) != 0) {
                    result.data_offsets_past_moof.push_back(
                        static_cast<std::int32_t>(read_be32(fragment, child.payload.offset + 8)) -
                        static_cast<std::int64_t>(moof.span.size));
                }
            }
        }
    }
    result.valid = true;
    return result;
}

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

bool test_offset_ticks() {
    bool ok = true;
    ok &= expect(loop_offset_ticks(10'000'000, 1, 90000) == 900000, "expected 10 s at 90 kHz to be 900000 ticks");
    ok &= expect(loop_offset_ticks(10'000'000, 3, 48000) == 1'440'000, "expected cycle 3 to scale linearly");
    // 33366 us is not a whole number of 44.1 kHz ticks; rounding happens on the
    // total, so the error stays under a tick however many cycles have passed.
    const std::uint64_t cycles = 100000;
    const std::uint64_t exact_numerator = 33366ULL * cycles * 44100ULL;
    const std::uint64_t ticks = loop_offset_ticks(33366, cycles, 44100);
    const std::uint64_t scaled = ticks * 1'000'000ULL;
    const std::uint64_t error = scaled > exact_numerator ? scaled - exact_numerator : exact_numerator - scaled;
    ok &= expect(error <= 500'000, "expected rounding error to stay within half a tick");
    ok &= expect(loop_offset_ticks(10'000'000, 0, 90000) == 0, "expected cycle 0 to have no offset");
    return ok;
}

bool test_cmaf() {
    bool ok = true;
    const Bytes sample_a{0xAA, 0xAB, 0xAC, 0xAD, 0xAE};
    const Bytes sample_b{0xBA, 0xBB, 0xBC};

    {
        Bytes fragment = build_fragment({{.tfdt_version = 1, .decode_time = 1000, .sample = sample_a}});
        const std::size_t size = fragment.size();
        ok &= expect(rebase_cmaf_timestamps(fragment, 5000), "expected v1 tfdt rebase to succeed");
        std::vector<TrafView> views;
        ok &= expect(inspect(fragment, views) && views.size() == 1, "expected rebased v1 fragment to parse");
        ok &= expect(fragment.size() == size, "expected v1 rebase to keep the fragment size");
        ok &= expect(!views.empty() && views[0].decode_time == 6000 && views[0].tfdt_version == 1,
                     "expected v1 tfdt to advance by the offset");
    }

    {
        Bytes fragment = build_fragment({{.tfdt_version = 0, .decode_time = 1000, .sample = sample_a}});
        const Bytes original = fragment;
        ok &= expect(rebase_cmaf_timestamps(fragment, 900000), "expected v0 tfdt rebase to succeed");
        std::vector<TrafView> views;
        ok &= expect(inspect(fragment, views) && views.size() == 1, "expected rebased v0 fragment to parse");
        ok &= expect(fragment.size() == original.size(), "expected an in-range v0 rebase to stay in place");
        ok &= expect(!views.empty() && views[0].decode_time == 901000 && views[0].tfdt_version == 0,
                     "expected v0 tfdt to advance by the offset");
    }

    {
        // Two trafs, both overflowing 32 bits: each tfdt widens to v1, and every
        // trun and saio offset must still find its own sample in the mdat.
        const std::uint64_t near_limit = std::numeric_limits<std::uint32_t>::max() - 10;
        Bytes fragment = build_fragment({{.track_id = 1, .decode_time = near_limit, .sample = sample_a, .with_saio = true},
                                         {.track_id = 2, .decode_time = 7, .sample = sample_b}});
        const Bytes original = fragment;
        const std::uint64_t delta = std::uint64_t{1} << 33U;
        ok &= expect(rebase_cmaf_timestamps(fragment, delta), "expected widening rebase to succeed");
        ok &= expect(fragment.size() == original.size() + 8, "expected two widened tfdt boxes to add 8 bytes");
        std::vector<TrafView> views;
        ok &= expect(inspect(fragment, views) && views.size() == 2, "expected widened fragment to parse");
        if (views.size() == 2) {
            ok &= expect(views[0].tfdt_version == 1 && views[0].decode_time == near_limit + delta,
                         "expected first tfdt widened to v1 with the full value");
            ok &= expect(views[1].tfdt_version == 1 && views[1].decode_time == 7 + delta,
                         "expected second tfdt widened to v1 with the full value");
            ok &= expect(views[0].sample_via_trun == sample_a && views[1].sample_via_trun == sample_b,
                         "expected trun data offsets to follow the widened moof");
            ok &= expect(views[0].sample_via_saio == sample_a, "expected saio offset to follow the widened moof");
        }
    }

    {
        Bytes garbage{0, 0, 0, 9, 'm', 'o', 'o', 'f'};
        const Bytes original = garbage;
        ok &= expect(!rebase_cmaf_timestamps(garbage, 1), "expected a malformed payload to be rejected");
        ok &= expect(garbage == original, "expected a rejected payload to be left unchanged");
    }
    return ok;
}

// Encoding then rebasing must give the same bytes as rebasing the CMAF chunk
// and then encoding it, across every fixture and varint length.
bool test_locmaf() {
    bool ok = true;
#ifdef LOCMAF_FIXTURE_DIR
    const std::filesystem::path root = LOCMAF_FIXTURE_DIR;
#else
    const std::filesystem::path root = "tests/fixtures/locmaf";
#endif
    std::size_t checked = 0;
    for (const auto& dir : std::filesystem::directory_iterator(root)) {
        if (!dir.is_directory() || !std::filesystem::exists(dir.path() / "init.mp4")) {
            continue;
        }
        const Bytes init = read_file(dir.path() / "init.mp4");
        for (const auto& entry : std::filesystem::directory_iterator(dir.path() / "canonical")) {
            if (entry.path().extension() != ".cmfc") {
                continue;
            }
            const Bytes chunk = read_file(entry.path());
            for (const std::uint64_t delta : {std::uint64_t{90000}, std::uint64_t{1} << 20U, std::uint64_t{1} << 40U}) {
                Bytes rebased_chunk = chunk;
                if (!rebase_cmaf_timestamps(rebased_chunk, delta)) {
                    continue;
                }
                Bytes encoded;
                Bytes expected;
                try {
                    encoded = LocmafEncoder(init).encode(chunk, 0, 0, true);
                    expected = LocmafEncoder(init).encode(rebased_chunk, 0, 0, true);
                } catch (const std::exception&) {
                    continue;
                }
                const bool rebased = rebase_locmaf_timestamps(encoded, delta);
                ok &= expect(rebased && encoded == expected,
                             "expected LOCMAF rebase to match re-encoding " + entry.path().string() +
                                 " with delta " + std::to_string(delta));
                ++checked;
            }
        }
    }
    ok &= expect(checked > 0, "expected LOCMAF fixtures to be exercised");

    Bytes truncated{2, 9};
    ok &= expect(!rebase_locmaf_timestamps(truncated, 1), "expected a truncated LOCMAF object to be rejected");
    Bytes delta_object{3, 0};
    ok &= expect(rebase_locmaf_timestamps(delta_object, 1) && delta_object == Bytes({3, 0}),
                 "expected a delta object, which has no base time, to be left unchanged");
    return ok;
}

bool test_loc() {
    std::vector<ObjectProperty> properties{{8, std::uint64_t{90000}}, {16, std::uint64_t{3003}}, {9, Bytes{1}}};
    rebase_loc_timestamps(properties, 900000);
    return expect(std::get<std::uint64_t>(properties[0].value) == 90000 &&
                      std::get<std::uint64_t>(properties[1].value) == 903003 &&
                      std::get<Bytes>(properties[2].value) == Bytes{1},
                  "expected only the LOC presentation timestamp to advance");
}

// Every media object the packagers produce from a real input must be
// rebasable, so --loop never falls back to resending cycle-0 timestamps.
bool test_fixture_plans() {
    using openmoq::publisher::CmsfObjectKind;
    using openmoq::publisher::MediaPackaging;
    using openmoq::publisher::Publisher;
    using openmoq::publisher::PublisherConfig;
    bool ok = true;
#ifdef LOCMAF_FIXTURE_DIR
    const std::filesystem::path fixtures = std::filesystem::path(LOCMAF_FIXTURE_DIR).parent_path();
#else
    const std::filesystem::path fixtures = "tests/fixtures";
#endif
    struct Case {
        std::string name;
        MediaPackaging packaging;
        bool split;
    };
    const std::vector<Case> cases{{"cmaf split", MediaPackaging::kCmaf, true},
                                  {"cmaf coalesced", MediaPackaging::kCmaf, false},
                                  {"locmaf", MediaPackaging::kLocmaf, true},
                                  {"loc", MediaPackaging::kLoc, true}};
    for (const char* input_name : {"locmaf-publisher.mp4", "loc-bframes-fragmented.mp4", "loc-bframes-progressive.mp4"}) {
        const Bytes input = read_file(fixtures / input_name);
        for (const auto& test_case : cases) {
            const std::string label = std::string(input_name) + " " + test_case.name;
            PublisherConfig config;
            config.media_packaging = test_case.packaging;
            config.split_cmaf_chunks = test_case.split;
            openmoq::publisher::PreparedPublish prepared;
            try {
                std::istringstream stream(std::string(input.begin(), input.end()));
                prepared = Publisher(config).prepare_stream(stream, input_name);
            } catch (const std::exception& error) {
                ok &= expect(false, label + ": could not prepare plan: " + error.what());
                continue;
            }
            const auto plan = openmoq::publisher::materialize_publish_plan(prepared.plan, prepared.input_bytes);
            std::map<std::string, std::string> packaging;
            for (const auto& track : plan.tracks) {
                packaging[track.track_name] = track.packaging;
            }
            std::size_t rebased = 0;
            for (const auto& object : plan.objects) {
                if (object.kind != CmsfObjectKind::kMedia) {
                    continue;
                }
                const std::string& track_packaging = packaging[object.track_name];
                for (const std::uint64_t delta : {std::uint64_t{90000}, std::uint64_t{1} << 33U}) {
                    Bytes payload = object.owned_payload;
                    if (track_packaging == "cmaf") {
                        ok &= expect(rebase_cmaf_timestamps(payload, delta), label + ": CMAF object not rebased");
                        const auto before = layout(object.owned_payload);
                        const auto after = layout(payload);
                        bool advanced = before.valid && after.valid && !before.decode_times.empty() &&
                                        before.decode_times.size() == after.decode_times.size();
                        for (std::size_t i = 0; advanced && i < before.decode_times.size(); ++i) {
                            advanced = after.decode_times[i] == before.decode_times[i] + delta;
                        }
                        ok &= expect(advanced, label + ": CMAF tfdt did not advance by the offset");
                        ok &= expect(after.data_offsets_past_moof == before.data_offsets_past_moof &&
                                         after.mdat == before.mdat,
                                     label + ": CMAF sample data moved relative to the rebased moof");
                    } else if (track_packaging == "locmaf") {
                        ok &= expect(rebase_locmaf_timestamps(payload, delta), label + ": LOCMAF object not rebased");
                        ok &= expect(payload != object.owned_payload, label + ": LOCMAF payload did not change");
                    } else if (track_packaging == "loc") {
                        auto properties = object.properties;
                        rebase_loc_timestamps(properties, delta);
                        const auto pts = [](const std::vector<ObjectProperty>& list) -> std::optional<std::uint64_t> {
                            for (const auto& property : list) {
                                if (property.id == 16) {
                                    return std::get<std::uint64_t>(property.value);
                                }
                            }
                            return std::nullopt;
                        };
                        const auto original = pts(object.properties);
                        ok &= expect(original.has_value() && pts(properties) == *original + delta,
                                     label + ": LOC presentation timestamp did not advance");
                    } else {
                        continue;
                    }
                    ++rebased;
                }
            }
            ok &= expect(rebased > 0, label + ": no media objects were rebased");
        }
    }
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= test_offset_ticks();
    ok &= test_cmaf();
    ok &= test_locmaf();
    ok &= test_loc();
    ok &= test_fixture_plans();
    return ok ? 0 : 1;
}
