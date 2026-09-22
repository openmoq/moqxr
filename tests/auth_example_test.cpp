#define main auth_example_entrypoint
#include "../examples/auth/AuthPublisher.cpp"
#undef main

#include <fstream>
#include <functional>
#include <limits>

int main() {
    int failures = 0;
    const auto expect = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    };
    const auto parse = [](std::initializer_list<const char*> arguments) {
        std::vector<std::string> values{"auth-example"};
        for (auto argument : arguments) values.emplace_back(argument);
        std::vector<char*> argv;
        for (auto& value : values) argv.push_back(value.data());
        return parse_args(static_cast<int>(argv.size()), argv.data());
    };
    const auto rejected = [&](std::initializer_list<const char*> arguments) {
        try { static_cast<void>(parse(arguments)); return false; }
        catch (const std::exception&) { return true; }
    };
    expect(rejected({"--seconds", "3garbage"}), "reject partial duration");
    expect(rejected({"--seconds", "2147483647"}), "reject object-count overflow");
    expect(rejected({"--token-file", "a", "--action-token-file", "b"}), "reject common and separate sources");
    expect(rejected({"--token-file", "a", "--catapult-command", "issuer"}), "reject file and command sources");
    expect(rejected({"--token-file", "a", "--token-file", "b"}), "reject duplicate source");
    expect(rejected({"--token-file", ""}), "reject empty file path");
    try {
        static_cast<void>(parse({"--auth-profile", "c4m-01", "--token-file", "a"}));
    } catch (...) { expect(false, "accept explicit C4M-01 profile"); }
    expect(rejected({"--auth-profile", "c4m-01", "--token-wrapper", "cat", "--token-file", "a"}), "reject profile and legacy wrapper conflict");
    const auto token_path = std::filesystem::temp_directory_path() /
        ("moqxr-auth-example-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    { std::ofstream file(token_path, std::ios::binary); file << "hex:aabb"; }
    const auto token_path_string = token_path.string();
    for (const auto& [profile, type] : std::vector<std::pair<const char*, std::uint8_t>>{
            {"c4m-01", 1}, {"moqx-compat", 16}, {"red5-cose-compat", 16}}) {
        auto args = parse({"--auth-profile", profile, "--token-file", token_path_string.c_str()});
        auto auth = make_authorization(args);
        expect(auth.setup_credential && auth.action_credential && !auth.setup_token, "structured example credentials");
        const auto encoded = publisher::cat4moq::encode_credential(*auth.setup_credential, args.draft);
        expect(encoded.bytes == std::vector<std::uint8_t>({3, type, 0xaa, 0xbb}), "profile wire type and unchanged CWT");
    }
    auto legacy = make_authorization(parse({"--token-wrapper", "cat", "--token-file", token_path_string.c_str()}));
    expect(legacy.setup_token && legacy.setup_token->bytes == std::vector<std::uint8_t>({3, 16, 0xaa, 0xbb}), "legacy wrapper remains explicit");
    auto override_auth = make_authorization(parse({"--auth-profile", "moqx-compat", "--auth-token-type", "300", "--token-file", token_path_string.c_str()}));
    expect(override_auth.setup_credential->token_type == 300, "compatibility token type override");
    {
        const auto key_path = std::filesystem::temp_directory_path() /
            ("moqxr-auth-example-dpop-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        { std::ofstream file(key_path, std::ios::binary); file << "-----BEGIN PRIVATE KEY-----\n"
                  "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgU3h+w6OcQb7NjtQ7\n"
                  "6aieEEHRnxoVYpDlWpCL05Z90DqhRANCAASPODFHeV2mkUnOM2ZV8dKYrtDudx2H\n"
                  "3j9HRjWk4R/IkLcCEfFyvYPuTzwqIhB6kV8vu8VnscyTUEw/WNV6tvXV\n"
                  "-----END PRIVATE KEY-----\n"; }
        const auto key_path_string = key_path.string();
        auto bound = make_authorization(parse({"--auth-profile", "c4m-01", "--token-file", token_path_string.c_str(),
                                               "--dpop-key-file", key_path_string.c_str()}));
        expect(bound.dpop_signer && bound.dpop_signer->token_type == 17 && bound.dpop_signer->thumbprint_hex().size() == 64,
               "DPoP key file configures a signer with the default proof type");
        auto typed = make_authorization(parse({"--auth-profile", "c4m-01", "--token-file", token_path_string.c_str(),
                                               "--dpop-key-file", key_path_string.c_str(), "--dpop-token-type", "33"}));
        expect(typed.dpop_signer && typed.dpop_signer->token_type == 33, "proof token type override");
        expect(rejected({"--dpop-token-type", "33", "--token-file", "a"}), "reject proof type without a key");
        expect(rejected({"--dpop-key-file", "", "--token-file", "a"}), "reject empty key path");
        const auto print_only = parse({"--print-dpop-jkt", "--dpop-key-file", key_path_string.c_str()});
        expect(print_only.print_dpop_jkt && !print_only.token_file, "print mode needs only the key");
        expect(rejected({"--print-dpop-jkt"}), "print mode requires a key file");
        std::filesystem::remove(key_path);
    }
    std::filesystem::remove(token_path);
    auto source = make_source("video", 2);
    for (std::size_t n = 0; n < 20; ++n) {
        auto object = source.next_object();
        expect(object.has_value(), "all requested objects generated");
        if (!object) break;
        expect(object->group_id == n / 10 && object->object_id == n % 10, "stable object addressing");
        expect(object->final_in_subgroup == (n % 10 == 9), "subgroup closes only after its final object");
        expect(object->subgroup_contains_group_largest, "group-largest header property stable for subgroup");
    }
    expect(!source.next_object(), "finite source terminates");
    return failures ? 1 : 0;
}
