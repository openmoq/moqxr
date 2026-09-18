#include "openmoq/publisher/locmaf_encoder.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
using namespace openmoq::publisher;
namespace fs = std::filesystem;
static std::vector<uint8_t> read(const fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  if (!f)
    throw std::runtime_error(p.string());
  return {std::istreambuf_iterator<char>(f), {}};
}
static void check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
static size_t find_type(const std::vector<uint8_t> &b,
                        const std::string &type) {
  auto it = std::search(b.begin(), b.end(), type.begin(), type.end());
  if (it == b.end())
    throw std::runtime_error("box type not found");
  return size_t(it - b.begin());
}
static void put32(std::vector<uint8_t> &b, size_t p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i)
    b.at(p + i) = uint8_t(v >> (24 - i * 8));
}
static uint32_t get32(const std::vector<uint8_t> &b, size_t p) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i)
    value = (value << 8) | b.at(p + i);
  return value;
}
template <class Exception = std::runtime_error, class F>
static void rejects(F f, const char *message) {
  try {
    f();
  } catch (const Exception &) {
    return;
  }
  throw std::runtime_error(message);
}
int main() {
  try {
#ifdef LOCMAF_FIXTURE_DIR
    const fs::path root = LOCMAF_FIXTURE_DIR;
#else
    const fs::path root = "tests/fixtures/locmaf";
#endif
    for (auto &dir : fs::directory_iterator(root)) {
      if (!dir.is_directory())
        continue;
      LocmafEncoder encoder(read(dir.path() / "init.mp4"));
      for (unsigned i = 0;; ++i) {
        const std::string stem = "g000_o00" + std::to_string(i);
        const auto input = dir.path() / "canonical" / (stem + ".cmfc");
        if (!fs::exists(input))
          break;
        const auto actual = encoder.encode(read(input), 0, i);
        const auto expected =
            read(dir.path() / "objects" / (stem + ".locmafobj"));
        if (actual != expected)
          throw std::runtime_error(dir.path().filename().string() + "/" + stem +
                                   " golden mismatch");
      }
    }
    const auto init = read(root / "single-sample/init.mp4");
    const auto chunk = read(root / "single-sample/canonical/g000_o000.cmfc");
    const auto next = read(root / "single-sample/canonical/g000_o001.cmfc");
    LocmafEncoder encoder(init);
    const auto full = encoder.encode(chunk, 7, 10);
    check(full.front() == 2, "first nonzero object must be full");
    check(encoder.encode(next, 7, 11).front() == 3,
          "contiguous chunk must use delta");
    check(encoder.encode(next, 8, 12).front() == 2, "new group must use full");
    encoder.reset();
    encoder.encode(chunk, 7, 10);
    check(encoder.encode(next, 7, 12).front() == 2, "object gap must reanchor");
    encoder.reset();
    encoder.encode(chunk, 7, 10);
    check(encoder.encode(next, 7, 11, true).front() == 2,
          "forced full must reanchor");
    encoder.reset();
    check(encoder.encode(chunk, 7, 10) == full, "reset must reproduce full");
    // Explicit per-sample duration must normalize exactly like the trex
    // default.
    auto explicit_duration = chunk;
    const auto tr = find_type(explicit_duration, "trun");
    check((get32(explicit_duration, tr + 4) & 0x100) == 0,
          "fixture must inherit duration");
    const auto old_tr_size = get32(explicit_duration, tr - 4);
    const auto moof = find_type(explicit_duration, "moof");
    const auto traf = find_type(explicit_duration, "traf");
    for (auto start : {moof - 4, traf - 4, tr - 4})
      put32(explicit_duration, start, get32(explicit_duration, start) + 4);
    put32(explicit_duration, tr + 4, get32(explicit_duration, tr + 4) | 0x100);
    put32(explicit_duration, tr + 12, get32(explicit_duration, tr + 12) + 4);
    explicit_duration.insert(explicit_duration.begin() + tr - 4 + old_tr_size,
                             {0, 0, 0x0b, 0xb8});
    encoder.reset();
    check(encoder.encode(explicit_duration, 7, 10) == full,
          "equivalent duration placement must normalize");
    auto raw = chunk;
    raw.insert(raw.end(), {0, 0, 0, 8, 'f', 'r', 'e', 'e'});
    auto expected_raw = raw;
    expected_raw.insert(expected_raw.begin(), 4);
    check(encoder.encode(raw, 7, 11) == expected_raw,
          "rawBoxes must preserve input");
    check(encoder.encode(next, 7, 12).front() == 2,
          "rawBoxes must reset chain");
    for (const std::string type : {"pssh", "sgpd", "sbgp", "subs"}) {
      auto excluded = chunk;
      excluded.insert(excluded.end(), {0, 0, 0, 8});
      excluded.insert(excluded.end(), type.begin(), type.end());
      rejects<LocmafIneligible>([&] { encoder.encode(excluded, 7, 13); },
                                "excluded box must reject track");
    }
    auto malformed = chunk;
    put32(malformed, 0, 1);
    rejects([&] { encoder.encode(malformed, 7, 13); },
            "largesize escape must reject");
    malformed = chunk;
    malformed.pop_back();
    rejects([&] { encoder.encode(malformed, 7, 13); },
            "truncated box must reject");
    malformed = chunk;
    put32(malformed, find_type(malformed, "trun") + 8, 0xffffffff);
    rejects([&] { encoder.encode(malformed, 7, 13); },
            "unbounded count must reject");
    rejects([&] { LocmafEncoder empty({}); }, "empty init must reject");
    // External auxiliary information must not disappear on constant-IV tracks.
    auto external = read(root / "cbcs-omit/canonical/g000_o000.cmfc");
    const auto external_moof = find_type(external, "moof");
    const auto external_traf = find_type(external, "traf");
    const auto external_trun = find_type(external, "trun");
    const auto insertion =
        external_moof - 4 + get32(external, external_moof - 4);
    const std::vector<uint8_t> saiz = {0, 0, 0, 17, 's', 'a', 'i', 'z', 0,
                                       0, 0, 0, 2,  0,   0,   0,   2};
    for (auto start : {external_moof - 4, external_traf - 4})
      put32(external, start, get32(external, start) + saiz.size());
    put32(external, external_trun + 12,
          get32(external, external_trun + 12) + saiz.size());
    external.insert(external.begin() + insertion, saiz.begin(), saiz.end());
    LocmafEncoder protected_encoder(read(root / "cbcs-omit/init.mp4"));
    check(protected_encoder.encode(external, 0, 0).front() == 4,
          "external auxiliary information must remain raw");
    // A missing tfdt is legal raw carriage outside the compact field model.
    auto no_tfdt = chunk;
    const auto tfdt_type = find_type(no_tfdt, "tfdt");
    std::copy_n("free", 4, no_tfdt.begin() + tfdt_type);
    check(encoder.encode(no_tfdt, 9, 0).front() == 4,
          "missing tfdt must fall back to raw");
    auto final_moof = chunk;
    final_moof.resize(find_type(final_moof, "mdat") - 4);
    check(encoder.encode(final_moof, 9, 0).front() == 4,
          "final moof must fall back safely");
    std::vector<uint8_t> deep = {0, 0, 0, 8, 'f', 'r', 'e', 'e'};
    for (unsigned i = 0; i < 256; ++i) {
      deep.insert(deep.begin(), {0, 0, 0, 0, 'm', 'o', 'o', 'f'});
      put32(deep, 0, deep.size());
    }
    rejects([&] { encoder.encode(deep, 9, 0); },
            "excessive nesting must reject");
    // Absolute tfdt values must retain all 64 bits, including vi64's ninth
    // byte.
    auto wide = chunk;
    const auto td = find_type(wide, "tfdt");
    check(wide.at(td + 4) == 1, "fixture tfdt must be version 1");
    std::fill(wide.begin() + td + 8, wide.begin() + td + 16, 0xff);
    const auto encoded = encoder.encode(wide, 9, 0);
    const std::vector<uint8_t> wide_field = {10,  255, 255, 255, 255,
                                             255, 255, 255, 255, 255};
    check(std::search(encoded.begin(), encoded.end(), wide_field.begin(),
                      wide_field.end()) != encoded.end(),
          "64-bit BMDT must use nine-byte vi64");
    std::cout << "LOCMAF golden vectors and boundary cases passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
