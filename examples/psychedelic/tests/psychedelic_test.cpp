#define main psychedelic_example_main
#include "../Psychedelic.cpp"
#undef main

#include <vector>

int main() {
    int failures = 0;
    const auto rejects = [&](std::vector<std::string> arguments) {
        std::vector<char*> argv;
        for (auto& argument : arguments) {
            argv.push_back(argument.data());
        }
        try {
            static_cast<void>(parse_args(static_cast<int>(argv.size()), argv.data()));
            std::cerr << "accepted invalid argument: " << arguments.back() << '\n';
            ++failures;
        } catch (const std::runtime_error&) {
        }
    };
    rejects({"psychedelic", "--seconds", "2junk"});
    rejects({"psychedelic", "--seconds", "0"});
    rejects({"psychedelic", "--seconds", "-1"});
    rejects({"psychedelic", "--seconds", "999999999999999999999"});
    rejects({"psychedelic", "--namespace", ""});
    char executable[] = "psychedelic";
    char help[] = "--help";
    char insecure[] = "--insecure";
    char* secure_argv[] = {executable};
    char* insecure_argv[] = {executable, insecure};
    if (parse_args(1, secure_argv).insecure || !parse_args(2, insecure_argv).insecure) {
        std::cerr << "TLS verification must be disabled only with --insecure\n";
        ++failures;
    }
    char* help_argv[] = {executable, help};
    if (psychedelic_example_main(2, help_argv) != 0) {
        std::cerr << "--help should succeed without starting FFmpeg or connecting\n";
        ++failures;
    }
    return failures == 0 ? 0 : 1;
}
