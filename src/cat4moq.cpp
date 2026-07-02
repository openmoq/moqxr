#include "openmoq/publisher/cat4moq.h"

namespace openmoq::publisher::cat4moq {

namespace {

constexpr std::uint8_t kAliasUseValue = 0x03;
constexpr std::uint8_t kTokenTypeOutOfBand = 0x00;
constexpr std::uint8_t kTokenTypeCat = 0x10;

AuthorizationToken wrap_token(std::uint8_t token_type, std::span<const std::uint8_t> token_bytes) {
    AuthorizationToken token;
    token.bytes.reserve(token_bytes.size() + 2);
    token.bytes.push_back(kAliasUseValue);
    token.bytes.push_back(token_type);
    token.bytes.insert(token.bytes.end(), token_bytes.begin(), token_bytes.end());
    return token;
}

}  // namespace

AuthorizationToken wrap_cat_token(std::span<const std::uint8_t> cwt_bytes) {
    return wrap_token(kTokenTypeCat, cwt_bytes);
}

AuthorizationToken wrap_out_of_band_token(std::span<const std::uint8_t> token_bytes) {
    return wrap_token(kTokenTypeOutOfBand, token_bytes);
}

}  // namespace openmoq::publisher::cat4moq
