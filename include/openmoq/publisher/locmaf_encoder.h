#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>
namespace openmoq::publisher {
class LocmafIneligible : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
class LocmafEncoder {
public:
  explicit LocmafEncoder(std::span<const uint8_t> single_track_init);
  ~LocmafEncoder();
  LocmafEncoder(LocmafEncoder &&) noexcept;
  LocmafEncoder &operator=(LocmafEncoder &&) noexcept;
  std::vector<uint8_t> encode(std::span<const uint8_t> chunk, uint64_t group_id,
                              uint64_t object_id, bool force_full = false);
  void reset();

private:
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace openmoq::publisher
