#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openmoq::publisher::examples::auth {

enum class TokenEncoding {
    kAuto,
    kRaw,
    kBase64,
    kHex,
};

struct CatapultTokenRequest {
    std::string action;
    std::string track_namespace;
    std::string track_name;
    std::string endpoint;
};

struct CatapultClientOptions {
    std::optional<std::filesystem::path> token_file;
    std::optional<std::string> command;
    TokenEncoding encoding = TokenEncoding::kAuto;
};

class CatapultClient {
public:
    explicit CatapultClient(CatapultClientOptions options);

    std::vector<std::uint8_t> issue_token(const CatapultTokenRequest& request) const;

private:
    CatapultClientOptions options_;
};

TokenEncoding parse_token_encoding(std::string_view value);

}  // namespace openmoq::publisher::examples::auth
