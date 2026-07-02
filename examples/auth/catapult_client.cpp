#include "catapult_client.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#ifdef _WIN32
#define OPENMOQ_AUTH_POPEN _popen
#define OPENMOQ_AUTH_PCLOSE _pclose
#define OPENMOQ_AUTH_POPEN_READ_MODE "rb"
#else
#define OPENMOQ_AUTH_POPEN popen
#define OPENMOQ_AUTH_PCLOSE pclose
#define OPENMOQ_AUTH_POPEN_READ_MODE "r"
#endif

namespace openmoq::publisher::examples::auth {

namespace {

std::string trim_ascii(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t first = 0;
    while (first < value.size() &&
           (value[first] == '\n' || value[first] == '\r' || value[first] == ' ' || value[first] == '\t')) {
        ++first;
    }
    if (first == 0) {
        return value;
    }
    return value.substr(first);
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open token file: " + path.string());
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), {});
}

std::string shell_quote(std::string_view value) {
    std::string quoted = "'";
    for (const char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(c);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

void replace_all(std::string& value, std::string_view needle, std::string_view replacement) {
    std::size_t pos = 0;
    while ((pos = value.find(needle, pos)) != std::string::npos) {
        value.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

std::string expand_command(std::string command, const CatapultTokenRequest& request) {
    replace_all(command, "{action}", shell_quote(request.action));
    replace_all(command, "{namespace}", shell_quote(request.track_namespace));
    replace_all(command, "{track}", shell_quote(request.track_name));
    replace_all(command, "{endpoint}", shell_quote(request.endpoint));
    return command;
}

std::vector<std::uint8_t> read_command_stdout(const std::string& command) {
    FILE* pipe = OPENMOQ_AUTH_POPEN(command.c_str(), OPENMOQ_AUTH_POPEN_READ_MODE);
    if (pipe == nullptr) {
        throw std::runtime_error("failed to run Catapult token command");
    }

    std::vector<std::uint8_t> output;
    std::array<char, 4096> buffer{};
    while (true) {
        const std::size_t bytes = std::fread(buffer.data(), 1, buffer.size(), pipe);
        if (bytes > 0) {
            output.insert(output.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(bytes));
        }
        if (bytes < buffer.size()) {
            if (std::ferror(pipe) != 0) {
                OPENMOQ_AUTH_PCLOSE(pipe);
                throw std::runtime_error("failed reading Catapult token command output");
            }
            break;
        }
    }

    const int exit_code = OPENMOQ_AUTH_PCLOSE(pipe);
    if (exit_code != 0) {
        throw std::runtime_error("Catapult token command exited non-zero");
    }
    return output;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::vector<std::uint8_t> decode_hex(std::string text) {
    text = trim_ascii(std::move(text));
    if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) {
        text = text.substr(2);
    }
    if ((text.size() % 2) != 0) {
        throw std::runtime_error("hex token output must have an even number of characters");
    }
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = hex_value(text[i]);
        const int low = hex_value(text[i + 1]);
        if (high < 0 || low < 0) {
            throw std::runtime_error("hex token output contains a non-hex character");
        }
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return out;
}

int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

std::vector<std::uint8_t> decode_base64(std::string text) {
    text = trim_ascii(std::move(text));
    std::vector<std::uint8_t> out;
    int value = 0;
    int bits = -8;
    for (const char c : text) {
        if (c == '=') {
            break;
        }
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            continue;
        }
        const int decoded = base64_value(c);
        if (decoded < 0) {
            throw std::runtime_error("base64 token output contains an invalid character");
        }
        value = (value << 6) | decoded;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<std::uint8_t>((value >> bits) & 0xff));
            bits -= 8;
        }
    }
    return out;
}

bool looks_textual_token(std::span<const std::uint8_t> bytes) {
    for (const std::uint8_t byte : bytes) {
        if (byte == '\n' || byte == '\r' || byte == '\t') {
            continue;
        }
        if (byte < 0x20 || byte > 0x7e) {
            return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> decode_token_bytes(std::vector<std::uint8_t> bytes, TokenEncoding encoding) {
    if (encoding == TokenEncoding::kRaw) {
        return bytes;
    }

    std::string text(bytes.begin(), bytes.end());
    const std::string trimmed = trim_ascii(text);
    if (encoding == TokenEncoding::kBase64) {
        return decode_base64(trimmed);
    }
    if (encoding == TokenEncoding::kHex) {
        return decode_hex(trimmed);
    }
    if (!looks_textual_token(bytes)) {
        return bytes;
    }

    if (trimmed.rfind("base64:", 0) == 0) {
        return decode_base64(trimmed.substr(7));
    }
    if (trimmed.rfind("hex:", 0) == 0) {
        return decode_hex(trimmed.substr(4));
    }
    if (trimmed.rfind("0x", 0) == 0 || trimmed.rfind("0X", 0) == 0) {
        return decode_hex(trimmed);
    }

    bytes.assign(trimmed.begin(), trimmed.end());
    return bytes;
}

}  // namespace

CatapultClient::CatapultClient(CatapultClientOptions options) : options_(std::move(options)) {}

std::vector<std::uint8_t> CatapultClient::issue_token(const CatapultTokenRequest& request) const {
    if (options_.token_file.has_value()) {
        return decode_token_bytes(read_file(*options_.token_file), options_.encoding);
    }
    if (options_.command.has_value()) {
        return decode_token_bytes(read_command_stdout(expand_command(*options_.command, request)), options_.encoding);
    }
    throw std::runtime_error("configure --token-file, --setup-token-file, --action-token-file, or --catapult-command");
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
