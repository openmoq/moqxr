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
