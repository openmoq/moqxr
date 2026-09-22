#include "openmoq/publisher/cat4moq.h"
#include "cat4moq_internal.h"

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

namespace {

void require_credential_size(std::size_t size) {
    if (size == 0 || size > kMaxCredentialBytes) {
        throw AuthorizationError("credential must contain 1 to 16384 bytes");
    }
}

}  // namespace

void append_integer(std::vector<std::uint8_t>& out, std::uint64_t value, DraftVersion draft) {
    if (draft == DraftVersion::kDraft14 || draft == DraftVersion::kDraft16) {
        if (value > ((std::uint64_t{1} << 62) - 1)) throw AuthorizationError("token type exceeds draft integer range");
        const unsigned size = value < 64 ? 1 : value < 16384 ? 2 : value < (std::uint64_t{1} << 30) ? 4 : 8;
        const std::uint8_t prefix = size == 1 ? 0 : size == 2 ? 0x40 : size == 4 ? 0x80 : 0xc0;
        for (unsigned remaining = size; remaining > 0; --remaining) {
            auto byte = static_cast<std::uint8_t>(value >> ((remaining - 1) * 8));
            out.push_back(remaining == size ? byte | prefix : byte);
        }
        return;
    }
    if (draft != DraftVersion::kDraft17 && draft != DraftVersion::kDraft18) throw AuthorizationError("unsupported credential draft");
    unsigned size = 1;
    while (size < 8 && value >= (std::uint64_t{1} << (size * 7))) ++size;
    if (size == 8 && value >= (std::uint64_t{1} << 56)) size = 9;
    if (size == 9) {
        out.push_back(0xff);
    } else {
        const auto prefix = static_cast<std::uint8_t>(0xffu << (9 - size));
        out.push_back(prefix | static_cast<std::uint8_t>(value >> ((size - 1) * 8)));
    }
    for (unsigned remaining = size - 1; remaining > 0; --remaining) {
        out.push_back(static_cast<std::uint8_t>(value >> ((remaining - 1) * 8)));
    }
}

namespace {

std::string_view trim_encoding(std::string_view text) {
    constexpr std::string_view whitespace = " \t\r\n";
    const auto first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

bool AuthorizationConfig::configured() const {
    return setup_token || action_token || setup_credential || action_credential || credential_provider;
}

AuthorizationToken encode_credential(const Credential& credential, DraftVersion draft) {
    require_credential_size(credential.cwt.size());
    std::uint64_t type = 0;
    switch (credential.profile) {
        case Profile::kC4m01: type = 1; break;
        case Profile::kMoqxCompat: type = credential.token_type.value_or(16); break;
        case Profile::kRed5CoseCompat: type = credential.token_type.value_or(16); break;
        default: throw AuthorizationError("unsupported credential profile");
    }
    if (credential.token_type && *credential.token_type != type) throw AuthorizationError("token type conflicts with credential profile");
    AuthorizationToken token;
    token.bytes.reserve(credential.cwt.size() + 10);
    append_integer(token.bytes, 3, draft);
    append_integer(token.bytes, type, draft);
    token.bytes.insert(token.bytes.end(), credential.cwt.begin(), credential.cwt.end());
    return token;
}

void validate_authorization(const AuthorizationConfig& config, DraftVersion draft) {
    if ((config.setup_token && config.setup_credential) || (config.action_token && config.action_credential)) {
        throw AuthorizationError("conflicting preencoded and structured credentials");
    }
    for (const auto* token : {&config.setup_token, &config.action_token}) {
        if (*token && ((*token)->bytes.empty() || (*token)->bytes.size() > kMaxCredentialBytes + 18)) {
            throw AuthorizationError("invalid preencoded credential size");
        }
    }
    if (config.setup_credential) encode_credential(*config.setup_credential, draft);
    if (config.action_credential) encode_credential(*config.action_credential, draft);
}

std::optional<AuthorizationToken> resolve_authorization(const AuthorizationConfig& config,
                                                       const Resource& resource, DraftVersion draft) {
    validate_authorization(config, draft);
    const bool setup = resource.action == Action::kClientSetup || resource.action == Action::kServerSetup;
    if (!setup && config.credential_provider) {
        try {
            return encode_credential(config.credential_provider(resource), draft);
        } catch (...) {
            // Provider diagnostics may contain the bearer token or issuer response.
            throw AuthorizationError("authorization credential provider failed");
        }
    }
    const auto& credential = setup ? config.setup_credential : config.action_credential;
    if (credential) return encode_credential(*credential, draft);
    return setup ? config.setup_token : config.action_token;
}

std::vector<std::uint8_t> decode_base64_token(std::string_view input, bool url_safe) {
    if (input.empty() || input.size() > kMaxEncodedCredentialBytes) throw AuthorizationError("invalid base64 credential size");
    const auto pad_start = input.find('=');
    const auto data_size = pad_start == std::string_view::npos ? input.size() : pad_start;
    const auto padding = input.size() - data_size;
    if (padding > 2 || (padding && (input.size() % 4 != 0 || input.substr(data_size).find_first_not_of('=') != std::string_view::npos)) ||
        data_size % 4 == 1 || (!url_safe && !padding && data_size % 4 != 0) ||
        (padding && padding != (4 - data_size % 4))) {
        throw AuthorizationError("invalid base64 credential padding");
    }
    const auto output_size = data_size * 6 / 8;
    require_credential_size(output_size);
    std::vector<std::uint8_t> output;
    output.reserve(output_size);
    unsigned accumulator = 0;
    unsigned bits = 0;
    for (const char ch : input.substr(0, data_size)) {
        int value = -1;
        if (ch >= 'A' && ch <= 'Z') value = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') value = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') value = ch - '0' + 52;
        else if (ch == (url_safe ? '-' : '+')) value = 62;
        else if (ch == (url_safe ? '_' : '/')) value = 63;
        if (value < 0) throw AuthorizationError("invalid base64 credential character");
        accumulator = (accumulator << 6) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<std::uint8_t>(accumulator >> bits));
            accumulator &= (1u << bits) - 1;
        }
    }
    if (accumulator != 0) throw AuthorizationError("invalid base64 credential trailing bits");
    return output;
}

std::vector<std::uint8_t> decode_credential_bytes(std::span<const std::uint8_t> input) {
    if (input.empty() || input.size() > kMaxEncodedCredentialBytes) throw AuthorizationError("invalid credential file size");
    const std::string_view text(reinterpret_cast<const char*>(input.data()), input.size());
    if (text.starts_with("base64:")) return decode_base64_token(trim_encoding(text.substr(7)));
    if (text.starts_with("hex:")) {
        const auto encoded = trim_encoding(text.substr(4));
        if (encoded.size() % 2 != 0) throw AuthorizationError("invalid hex credential length");
        require_credential_size(encoded.size() / 2);
        std::vector<std::uint8_t> output;
        output.reserve(encoded.size() / 2);
        for (std::size_t i = 0; i < encoded.size(); i += 2) {
            const auto high = hex_digit(encoded[i]);
            const auto low = hex_digit(encoded[i + 1]);
            if (high < 0 || low < 0) throw AuthorizationError("invalid hex credential character");
            output.push_back(static_cast<std::uint8_t>(high * 16 + low));
        }
        return output;
    }
    require_credential_size(input.size());
    return {input.begin(), input.end()};
}

}  // namespace openmoq::publisher::cat4moq
