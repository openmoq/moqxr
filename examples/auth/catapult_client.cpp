#include "catapult_client.h"
#include "openmoq/publisher/cat4moq.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace openmoq::publisher::examples::auth {
namespace {

std::string_view trim_ascii(std::string_view text) {
    constexpr std::string_view whitespace = " \t\r\n";
    const auto first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("failed to open token file");
    std::vector<std::uint8_t> output;
    std::array<char, 4096> buffer{};
    while (input) {
        const auto limit = std::min(buffer.size(), cat4moq::kMaxEncodedCredentialBytes - output.size() + 1);
        input.read(buffer.data(), static_cast<std::streamsize>(limit));
        const auto bytes = static_cast<std::size_t>(input.gcount());
        if (bytes > cat4moq::kMaxEncodedCredentialBytes - output.size()) {
            throw std::runtime_error("token file exceeds credential size limit");
        }
        output.insert(output.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(bytes));
    }
    if (input.bad() || !input.eof()) throw std::runtime_error("failed reading token file");
    return output;
}

#ifndef _WIN32
std::string shell_quote(std::string_view value) {
    if (value.find('\0') != std::string_view::npos) throw std::runtime_error("token command argument contains NUL");
    std::string quoted = "'";
    for (const char c : value) {
        if (c == '\'') quoted += "'\\''";
        else quoted.push_back(c);
    }
    quoted.push_back('\'');
    return quoted;
}

std::string expand_command(std::string_view command, const CatapultTokenRequest& request) {
    const std::array<std::pair<std::string_view, std::string>, 4> replacements{{
        {"{action}", shell_quote(request.action)},
        {"{namespace}", shell_quote(request.track_namespace)},
        {"{track}", shell_quote(request.track_name)},
        {"{endpoint}", shell_quote(request.endpoint)},
    }};
    std::string expanded;
    // Scan only the template: substituted values may themselves contain placeholders.
    for (std::size_t pos = 0; pos < command.size();) {
        bool replaced = false;
        for (const auto& [placeholder, value] : replacements) {
            if (command.substr(pos).starts_with(placeholder)) {
                expanded += value;
                pos += placeholder.size();
                replaced = true;
                break;
            }
        }
        if (!replaced) expanded.push_back(command[pos++]);
    }
    return expanded;
}

std::vector<std::uint8_t> read_command_stdout(const std::string& command) {
    const auto close_pipe = [](FILE* pipe) { pclose(pipe); };
    std::unique_ptr<FILE, decltype(close_pipe)> pipe(popen(command.c_str(), "r"), close_pipe);
    if (!pipe) throw std::runtime_error("failed to run Catapult token command");
    std::vector<std::uint8_t> output;
    std::array<char, 4096> buffer{};
    while (true) {
        const auto limit = std::min(buffer.size(), cat4moq::kMaxEncodedCredentialBytes - output.size() + 1);
        const auto bytes = std::fread(buffer.data(), 1, limit, pipe.get());
        if (bytes > cat4moq::kMaxEncodedCredentialBytes - output.size()) {
            throw std::runtime_error("Catapult token command output exceeds credential size limit");
        }
        output.insert(output.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(bytes));
        if (bytes < limit) {
            if (std::ferror(pipe.get())) throw std::runtime_error("failed reading Catapult token command output");
            break;
        }
    }
    if (pclose(pipe.release()) != 0) throw std::runtime_error("Catapult token command exited non-zero");
    return output;
}
#endif

std::vector<std::uint8_t> decode_hex(std::string_view text) {
    text = trim_ascii(text);
    if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
    const std::string prefixed = "hex:" + std::string(text);
    return cat4moq::decode_credential_bytes(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(prefixed.data()), prefixed.size()));
}

std::vector<std::uint8_t> decode_token_bytes(std::vector<std::uint8_t> bytes, TokenEncoding encoding) {
    if (bytes.empty() || bytes.size() > cat4moq::kMaxEncodedCredentialBytes) {
        throw std::runtime_error("invalid token output size");
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto trimmed = trim_ascii(text);
    switch (encoding) {
        case TokenEncoding::kRaw: break;
        case TokenEncoding::kBase64: return cat4moq::decode_base64_token(trimmed);
        case TokenEncoding::kHex: return decode_hex(trimmed);
        case TokenEncoding::kAuto:
            if (trimmed.starts_with("base64:") || trimmed.starts_with("hex:")) {
                return cat4moq::decode_credential_bytes(std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(trimmed.data()), trimmed.size()));
            }
            if (trimmed.starts_with("0x") || trimmed.starts_with("0X")) return decode_hex(trimmed);
            break;
        default: throw std::runtime_error("unsupported token encoding");
    }
    if (bytes.size() > cat4moq::kMaxCredentialBytes) throw std::runtime_error("token exceeds credential size limit");
    return bytes;
}

}  // namespace

CatapultClient::CatapultClient(CatapultClientOptions options) : options_(std::move(options)) {
    if (options_.token_file.has_value() == options_.command.has_value()) {
        throw std::runtime_error("configure exactly one token file or Catapult command");
    }
    if (options_.token_file && (options_.token_file->empty() ||
        options_.token_file->native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)) {
        throw std::runtime_error("invalid token file path");
    }
    if (options_.command) {
        if (trim_ascii(*options_.command).empty() || options_.command->find('\0') != std::string::npos) {
            throw std::runtime_error("invalid Catapult token command");
        }
#ifdef _WIN32
        throw std::runtime_error("Catapult command mode is unsupported on Windows; use a token file");
#endif
    }
}

std::vector<std::uint8_t> CatapultClient::issue_token(const CatapultTokenRequest& request) const {
    if (options_.token_file) return decode_token_bytes(read_file(*options_.token_file), options_.encoding);
#ifndef _WIN32
    return decode_token_bytes(read_command_stdout(expand_command(*options_.command, request)), options_.encoding);
#else
    (void)request;
    throw std::runtime_error("Catapult command mode is unsupported on Windows; use a token file");
#endif
}

TokenEncoding parse_token_encoding(std::string_view value) {
    if (value == "auto") {
        return TokenEncoding::kAuto;
    }
    if (value == "raw") {
        return TokenEncoding::kRaw;
    }
    if (value == "base64") {
        return TokenEncoding::kBase64;
    }
    if (value == "hex") {
        return TokenEncoding::kHex;
    }
    throw std::runtime_error("unsupported token encoding: expected auto, raw, base64, or hex");
}

}  // namespace openmoq::publisher::examples::auth
