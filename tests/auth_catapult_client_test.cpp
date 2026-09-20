#include "../examples/auth/catapult_client.h"
#include "openmoq/publisher/cat4moq.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

using namespace openmoq::publisher::examples::auth;
namespace cat4moq = openmoq::publisher::cat4moq;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <class F>
void rejects(F&& function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("expected rejection");
}

struct TokenFile {
    std::filesystem::path directory;
    std::filesystem::path path;
    TokenFile() {
        std::random_device random;
        do {
            directory = std::filesystem::temp_directory_path() /
                        ("moqxr-auth-test-" + std::to_string(random()));
        } while (!std::filesystem::create_directory(directory));
        path = directory / "token";
    }
    ~TokenFile() { std::error_code error; std::filesystem::remove_all(directory, error); }
    std::vector<std::uint8_t> decode(std::string_view value, TokenEncoding encoding = TokenEncoding::kAuto) {
        std::ofstream output(path, std::ios::binary);
        output.write(value.data(), static_cast<std::streamsize>(value.size()));
        output.close();
        require(static_cast<bool>(output), "test token file write failed");
        return CatapultClient({path, std::nullopt, encoding}).issue_token({});
    }
};

std::string as_string(const std::vector<std::uint8_t>& bytes) { return {bytes.begin(), bytes.end()}; }
}

int main() {
    int failures = 0;
    const auto test = [&](const char* name, const std::function<void()>& body) {
        try { body(); } catch (const std::exception& error) {
            ++failures;
            std::cerr << name << ": " << error.what() << '\n';
        }
    };
    TokenFile file;
    test("auto and raw preserve all bytes", [&] {
        const std::string token(" \tCAT\0bytes\r\n", 13);
        require(as_string(file.decode(token)) == token, "auto changed binary credential");
        require(as_string(file.decode(" CAT \n")) == " CAT \n", "auto stripped raw whitespace");
        require(as_string(file.decode("hex:41\n", TokenEncoding::kRaw)) == "hex:41\n", "raw decoded prefix");
    });
    test("supported encoding compatibility", [&] {
        for (const auto token : {"hex:0041ff\n", "0x0041ff", " 0X0041ff\n", "base64:AEH/\n"}) {
            require(file.decode(token) == std::vector<std::uint8_t>({0, 65, 255}), "auto decoding mismatch");
        }
        require(as_string(file.decode(" QQ==\n", TokenEncoding::kBase64)) == "A", "explicit base64 mismatch");
        require(as_string(file.decode(" 0x41\n", TokenEncoding::kHex)) == "A", "explicit hex mismatch");
    });
    test("reject malformed encodings", [&] {
        for (const auto token : {"A", "QQ", "QR==", "QQ===", "QQ==trailer", "Q Q==", "=", ""}) {
            rejects([&] { file.decode(token, TokenEncoding::kBase64); });
        }
        for (const auto token : {"", "0x", "a", "xx"}) rejects([&] { file.decode(token, TokenEncoding::kHex); });
        rejects([&] { file.decode("base64:QR=="); });
        rejects([&] { file.decode("hex:4g"); });
    });
    test("reject empty and oversized credentials", [&] {
        rejects([&] { file.decode(""); });
        rejects([&] { file.decode("", TokenEncoding::kRaw); });
        const std::string maximum(cat4moq::kMaxCredentialBytes, 'x');
        require(as_string(file.decode(maximum, TokenEncoding::kRaw)) == maximum, "raw maximum rejected");
        rejects([&] { file.decode(maximum + "x", TokenEncoding::kRaw); });
        rejects([&] { file.decode(maximum + "x"); });
        require(file.decode("hex:" + std::string(cat4moq::kMaxCredentialBytes * 2, '4')).size() == cat4moq::kMaxCredentialBytes,
                "maximum decoded hex rejected");
        rejects([&] { file.decode("hex:" + std::string((cat4moq::kMaxCredentialBytes + 1) * 2, '4')); });
        rejects([&] { file.decode(std::string(21848, 'A'), TokenEncoding::kBase64); });
        rejects([&] { file.decode(std::string(cat4moq::kMaxEncodedCredentialBytes + 1, 'A'), TokenEncoding::kBase64); });
    });
    test("reject conflicting or empty sources", [&] {
        rejects([&] { CatapultClient({file.path, "printf token"}).issue_token({}); });
        rejects([&] { CatapultClient({}).issue_token({}); });
        rejects([&] { CatapultClient({std::nullopt, ""}).issue_token({}); });
        rejects([&] { CatapultClient({std::nullopt, " \t\r\n"}).issue_token({}); });
        rejects([&] { CatapultClient({std::filesystem::path{}, std::nullopt}).issue_token({}); });
        rejects([&] { CatapultClient({std::nullopt, std::string("printf token\0ignored", 20)}).issue_token({}); });
    });
    test("file failures are reported", [&] {
        rejects([&] { CatapultClient({file.directory / "missing", std::nullopt}).issue_token({}); });
        rejects([&] { CatapultClient({file.directory, std::nullopt}).issue_token({}); });
        rejects([&] { parse_token_encoding("invalid"); });
    });
#ifndef _WIN32
    test("placeholder values remain literal and are expanded once", [&] {
        const CatapultTokenRequest request{"{namespace}'$(printf INJECTED)", "space ; value", "a'b", "https://host/$HOME"};
        const CatapultClient client({std::nullopt, "printf '%s|%s|%s|%s' {action} {namespace} {track} {endpoint}", TokenEncoding::kRaw});
        require(as_string(client.issue_token(request)) == request.action + "|" + request.track_namespace + "|" + request.track_name + "|" + request.endpoint,
                "placeholder substitution changed literal data");
    });
    test("reject embedded NUL in request fields", [&] {
        CatapultTokenRequest request;
        request.action = std::string("a\0b", 3);
        rejects([&] { CatapultClient({std::nullopt, "printf '%s' {action}"}).issue_token(request); });
    });
    test("command failures and bounded output", [&] {
        rejects([&] { CatapultClient({std::nullopt, "printf token; exit 7"}).issue_token({}); });
        rejects([&] { CatapultClient({std::nullopt, "printf ''"}).issue_token({}); });
        rejects([&] { CatapultClient({std::nullopt, "head -c 65537 /dev/zero", TokenEncoding::kRaw}).issue_token({}); });
    });
#else
    test("Windows rejects unsupported command mode", [&] {
        rejects([&] { CatapultClient({std::nullopt, "echo token"}).issue_token({}); });
    });
#endif
    return failures == 0 ? 0 : 1;
}
