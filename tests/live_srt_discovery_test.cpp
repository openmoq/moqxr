#include "openmoq/publisher/live_srt_ingest.h"

#include <srt/srt.h>

#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;
using openmoq::publisher::LiveSrtCallerRuntimeConfig;
using openmoq::publisher::LiveSrtIngestManager;

namespace {
bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

sockaddr_in reserve_address(SRTSOCKET& socket) {
    socket = srt_create_socket();
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (srt_bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SRT_ERROR) {
        throw std::runtime_error("could not reserve loopback port");
    }
    int size = sizeof(address);
    if (srt_getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) == SRT_ERROR) {
        throw std::runtime_error("could not read loopback port");
    }
    return address;
}

LiveSrtCallerRuntimeConfig listener(const sockaddr_in& address) {
    LiveSrtCallerRuntimeConfig config;
    config.id = "test";
    config.mode = "listener";
    config.endpoint = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    return config;
}

bool delayed_connection_gets_full_discovery_window() {
    srt_startup();
    SRTSOCKET reservation;
    auto address = reserve_address(reservation);
    srt_close(reservation);
    srt_cleanup();
    std::atomic<bool> stop{false};
    LiveSrtCallerRuntimeConfig caller;
    caller.id = "failed-caller";
    caller.endpoint = "invalid-endpoint";
    LiveSrtIngestManager manager({caller, listener(address)}, [](auto&&) {}, stop);
    auto started = std::async(std::launch::async, [&] { return manager.start(); });
    bool ok = expect(started.wait_for(6s) == std::future_status::timeout,
                     "listener must keep waiting for an encoder beyond five seconds");
    const auto sender = srt_create_socket();
    const auto connected_at = std::chrono::steady_clock::now();
    const bool connected = srt_connect(sender, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != SRT_ERROR;
    ok &= expect(connected, "delayed encoder must connect");
    if (connected) {
        ok &= expect(started.wait_for(4s) == std::future_status::timeout,
                     "accepted listener must get a fresh discovery window");
        ok &= expect(started.wait_for(3s) == std::future_status::ready,
                     "silent encoder must exhaust discovery timeout");
        ok &= expect(std::chrono::steady_clock::now() - connected_at >= 5s,
                     "discovery must allow five seconds after accept");
    }
    stop.store(true);
    started.get();
    srt_close(sender);
    manager.join();
    return ok;
}

bool waiting_listener_stops_promptly() {
    srt_startup();
    SRTSOCKET reservation;
    const auto address = reserve_address(reservation);
    srt_close(reservation);
    srt_cleanup();
    std::atomic<bool> stop{false};
    LiveSrtIngestManager manager({listener(address)}, [](auto&&) {}, stop);
    auto started = std::async(std::launch::async, [&] { return manager.start(); });
    bool ok = expect(started.wait_for(300ms) == std::future_status::timeout,
                     "listener should wait before shutdown");
    stop.store(true);
    ok &= expect(started.wait_for(1s) == std::future_status::ready,
                 "shutdown must interrupt waiting for an encoder");
    started.get();
    manager.join();
    return ok;
}

bool listener_setup_failure_finishes_discovery() {
    std::atomic<bool> stop{false};
    LiveSrtCallerRuntimeConfig config;
    config.mode = "listener";
    config.endpoint = "invalid-endpoint";
    LiveSrtIngestManager manager({config}, [](auto&&) {}, stop);
    auto started = std::async(std::launch::async, [&] { return manager.start(); });
    const bool ok = expect(started.wait_for(1s) == std::future_status::ready,
                           "failed listener must not wait for an impossible connection");
    stop.store(true);
    started.get();
    manager.join();
    return ok;
}

bool caller_retains_startup_discovery_window() {
    std::atomic<bool> stop{false};
    LiveSrtCallerRuntimeConfig config;
    config.endpoint = "invalid-endpoint";
    LiveSrtIngestManager manager({config}, [](auto&&) {}, stop);
    auto started = std::async(std::launch::async, [&] { return manager.start(); });
    bool ok = expect(started.wait_for(4s) == std::future_status::timeout,
                     "caller must retain its startup discovery window");
    ok &= expect(started.wait_for(2s) == std::future_status::ready,
                 "caller discovery must remain bounded without a connection");
    stop.store(true);
    started.get();
    manager.join();
    return ok;
}
}  // namespace

int main() {
    bool ok = delayed_connection_gets_full_discovery_window();
    ok &= waiting_listener_stops_promptly();
    ok &= listener_setup_failure_finishes_discovery();
    ok &= caller_retains_startup_discovery_window();
    return ok ? 0 : 1;
}
