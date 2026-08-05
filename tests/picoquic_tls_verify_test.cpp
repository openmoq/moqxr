// TLS server-certificate verification tests for the picoquic transport.
//
// These tests spin up loopback picoquic servers using the test certificates
// shipped with the picoquic source tree (all issued by "picotls test ca",
// certs/test-ca.crt) and exercise the PicoquicClient verification behaviour:
//
//   1. verified handshake against the RSA test certificate succeeds
//   2. insecure (null-verifier) handshake against an Ed25519 certificate fails
//      with a decoded TLS handshake_failure alert -- the null-verifier
//      ClientHello does not offer the ed25519 signature algorithm
//   3. verified handshake against the same Ed25519 certificate succeeds,
//      because installing the certificate verifier broadens the offered
//      signature algorithms
//   4. verified handshake against the secp384r1 (P-384) certificate succeeds
//   5. a trust anchor that did not issue the server certificate is rejected
//   6. a host name that does not match the certificate is rejected
//   7. a ca_path pointing at a missing/non-PEM file fails fast with a clear
//      error, instead of silently downgrading to no verification
//   8. insecure handshake against the RSA certificate still succeeds
//      (backwards compatibility)

#include "openmoq/publisher/transport/picoquic_client.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <picoquic.h>
#include <picoquic_packet_loop.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using openmoq::publisher::transport::ConnectionState;
using openmoq::publisher::transport::EndpointConfig;
using openmoq::publisher::transport::PicoquicClient;
using openmoq::publisher::transport::TlsConfig;
using openmoq::publisher::transport::TransportStatus;

constexpr const char* kPicoquicSourceDir = OPENMOQ_PICOQUIC_SOURCE_DIR;
constexpr const char* kTestAlpn = "moq-00";

bool trace_enabled() {
    static const bool enabled = std::getenv("OPENMOQ_PICOQUIC_TRACE") != nullptr;
    return enabled;
}

void trace(const std::string& message) {
    if (trace_enabled()) {
        std::cerr << "[picoquic-tls-verify] " << message << std::endl;
    }
}

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    std::cerr << "ok: " << message << '\n';
    return true;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

struct TlsTestServer {
    picoquic_quic_t* quic = nullptr;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable condition;
    uint16_t port = 0;
    bool loop_ready = false;
    bool stop_requested = false;
    bool loop_exited = false;
    int loop_return_code = 0;
};

int tls_test_server_callback(picoquic_cnx_t* cnx,
                             uint64_t stream_id,
                             uint8_t* bytes,
                             size_t length,
                             picoquic_call_back_event_t event,
                             void* callback_ctx,
                             void* stream_ctx) {
    static_cast<void>(cnx);
    static_cast<void>(stream_id);
    static_cast<void>(bytes);
    static_cast<void>(length);
    static_cast<void>(event);
    static_cast<void>(callback_ctx);
    static_cast<void>(stream_ctx);
    // Handshake-only server: accept everything, serve nothing.
    return 0;
}

int tls_test_server_loop_callback(picoquic_quic_t* quic,
                                  picoquic_packet_loop_cb_enum cb_mode,
                                  void* callback_ctx,
                                  void* callback_arg) {
    static_cast<void>(quic);

    auto* server = static_cast<TlsTestServer*>(callback_ctx);
    if (server == nullptr) {
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }

    switch (cb_mode) {
        case picoquic_packet_loop_ready: {
            auto* options = static_cast<picoquic_packet_loop_options_t*>(callback_arg);
            if (options != nullptr) {
                options->do_time_check = 1;
            }
            std::lock_guard<std::mutex> lock(server->mutex);
            server->loop_ready = true;
            server->condition.notify_all();
            return 0;
        }
        case picoquic_packet_loop_port_update:
            if (callback_arg != nullptr) {
                auto* addr = static_cast<sockaddr*>(callback_arg);
                std::lock_guard<std::mutex> lock(server->mutex);
                if (addr->sa_family == AF_INET) {
                    server->port = reinterpret_cast<sockaddr_in*>(addr)->sin_port;
                } else if (addr->sa_family == AF_INET6) {
                    server->port = reinterpret_cast<sockaddr_in6*>(addr)->sin6_port;
                }
                server->condition.notify_all();
            }
            return 0;
        case picoquic_packet_loop_time_check: {
            auto* time_check = static_cast<packet_loop_time_check_arg_t*>(callback_arg);
            std::lock_guard<std::mutex> lock(server->mutex);
            if (time_check != nullptr && time_check->delta_t > 10000) {
                time_check->delta_t = 10000;
            }
            // Older picoquic versions honour a terminate code returned from
            // the time_check callback; keep it for those.
            return server->stop_requested ? PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP : 0;
        }
        case picoquic_packet_loop_after_receive:
        case picoquic_packet_loop_after_send: {
            // Current picoquic overwrites the time_check return code on the
            // poll-timeout path, so also terminate from the traffic
            // callbacks; stop_server() nudges the socket to guarantee one.
            std::lock_guard<std::mutex> lock(server->mutex);
            return server->stop_requested ? PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP : 0;
        }
        default:
            return 0;
    }
}

bool start_server(TlsTestServer& server, const std::string& cert_path, const std::string& key_path) {
    server.quic = picoquic_create(8, cert_path.c_str(), key_path.c_str(), nullptr, kTestAlpn,
                                  tls_test_server_callback, &server, nullptr, nullptr, nullptr,
                                  picoquic_current_time(), nullptr, nullptr, nullptr, 0);
    if (server.quic == nullptr) {
        std::cerr << "failed to create server context for cert " << cert_path << '\n';
        return false;
    }

    server.thread = std::thread([&server] {
        const int ret =
            picoquic_packet_loop(server.quic, server.port, AF_INET, 0, 0, 1, tls_test_server_loop_callback, &server);
        std::lock_guard<std::mutex> lock(server.mutex);
        server.loop_return_code = ret;
        server.loop_exited = true;
        server.condition.notify_all();
    });

    std::unique_lock<std::mutex> lock(server.mutex);
    const bool started = server.condition.wait_for(lock, std::chrono::seconds(5), [&] {
        return (server.loop_ready && server.port != 0) || server.loop_exited;
    });
    return started && server.loop_ready && server.port != 0 && !server.loop_exited;
}

void stop_server(TlsTestServer& server) {
    {
        std::lock_guard<std::mutex> lock(server.mutex);
        server.stop_requested = true;
    }

    // Wake the packet loop out of poll() so the stop request is observed: a
    // stray datagram triggers the after_receive callback, which returns the
    // terminate code. Without this, current picoquic overwrites the
    // time_check terminate code on the poll-timeout path and the loop spins
    // forever.
    if (server.port != 0) {
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd >= 0) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(server.port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            const std::uint8_t nudge = 0;
            for (int attempt = 0; attempt < 20; ++attempt) {
                (void)sendto(fd, &nudge, sizeof(nudge), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
                std::unique_lock<std::mutex> lock(server.mutex);
                if (server.condition.wait_for(lock, std::chrono::milliseconds(100), [&] { return server.loop_exited; })) {
                    break;
                }
            }
            close(fd);
        }
    }

    if (server.thread.joinable()) {
        server.thread.join();
    }

    if (server.quic != nullptr) {
        picoquic_free(server.quic);
        server.quic = nullptr;
    }
}

TransportStatus try_connect(uint16_t port, const std::string& sni, const TlsConfig& tls) {
    const EndpointConfig endpoint{
        .host = "127.0.0.1",
        .port = port,
        .alpn = kTestAlpn,
        .sni = sni,
    };

    PicoquicClient transport;
    TransportStatus status = transport.configure(endpoint, tls);
    if (!status.ok) {
        return status;
    }
    status = transport.connect();
    if (status.ok) {
        transport.close(0);
    } else {
        trace("connect failed: " + status.message);
    }
    return status;
}

struct ServerScenario {
    TlsTestServer server;
    bool started = false;
};

}  // namespace

int main() {
    bool ok = true;

    const std::string certs_dir = std::string(kPicoquicSourceDir) + "/certs";
    const std::string test_ca = certs_dir + "/test-ca.crt";
    const std::string rsa_cert = certs_dir + "/cert.pem";           // CN=test.example.com
    const std::string rsa_key = certs_dir + "/key.pem";
    const std::string ed_cert = certs_dir + "/ed25519/cert.pem";    // CN=ed25519.test.example.com
    const std::string ed_key = certs_dir + "/ed25519/key.pem";
    const std::string p384_cert = certs_dir + "/secp384r1/cert.pem";  // CN=secp384r1.test.example.com
    const std::string p384_key = certs_dir + "/secp384r1/key.pem";

    std::cerr << "tls verify test start (picoquic certs at " << certs_dir << ")" << std::endl;

    // ---- Servers -----------------------------------------------------------
    ServerScenario rsa_server;
    rsa_server.started = start_server(rsa_server.server, rsa_cert, rsa_key);
    ok &= expect(rsa_server.started, "RSA test server starts");

    ServerScenario ed_server;
    ed_server.started = start_server(ed_server.server, ed_cert, ed_key);
    ok &= expect(ed_server.started, "Ed25519 test server starts");

    ServerScenario p384_server;
    p384_server.started = start_server(p384_server.server, p384_cert, p384_key);
    ok &= expect(p384_server.started, "P-384 test server starts");

    if (!ok) {
        stop_server(rsa_server.server);
        stop_server(ed_server.server);
        stop_server(p384_server.server);
        return 1;
    }

    // ---- 1. verified handshake, RSA certificate ----------------------------
    {
        const TlsConfig tls{.ca_path = test_ca, .insecure_skip_verify = false};
        const TransportStatus status = try_connect(rsa_server.server.port, "test.example.com", tls);
        ok &= expect(status.ok, "verified handshake succeeds against the RSA certificate" +
                                    (status.ok ? std::string() : ": " + status.message));
    }

    // ---- 2. insecure handshake, Ed25519 certificate: must fail -------------
    // The null-verifier ClientHello does not offer the ed25519 signature
    // algorithm, so the server cannot sign CertificateVerify and aborts with
    // TLS alert 40 (handshake_failure), surfaced as QUIC crypto error 0x128.
    {
        const TlsConfig tls{.insecure_skip_verify = true};
        const TransportStatus status = try_connect(ed_server.server.port, "", tls);
        ok &= expect(!status.ok, "insecure handshake fails against the Ed25519 certificate");
        if (!status.ok) {
            ok &= expect(contains(status.message, "handshake_failure") || contains(status.message, "0x128"),
                         "Ed25519/insecure failure message decodes the TLS alert (got: \"" + status.message + "\")");
        }
    }

    // ---- 3. verified handshake, Ed25519 certificate: must succeed ----------
    {
        const TlsConfig tls{.ca_path = test_ca, .insecure_skip_verify = false};
        const TransportStatus status = try_connect(ed_server.server.port, "ed25519.test.example.com", tls);
        ok &= expect(status.ok, "verified handshake succeeds against the Ed25519 certificate" +
                                    (status.ok ? std::string() : ": " + status.message));
    }

    // ---- 4. verified handshake, P-384 certificate ---------------------------
    {
        const TlsConfig tls{.ca_path = test_ca, .insecure_skip_verify = false};
        const TransportStatus status = try_connect(p384_server.server.port, "secp384r1.test.example.com", tls);
        ok &= expect(status.ok, "verified handshake succeeds against the P-384 certificate" +
                                    (status.ok ? std::string() : ": " + status.message));
    }

    // ---- 5. wrong trust anchor: must fail ----------------------------------
    // Use an unrelated leaf certificate as the "CA"; chain building must fail.
    {
        const TlsConfig tls{.ca_path = p384_cert, .insecure_skip_verify = false};
        const TransportStatus status = try_connect(rsa_server.server.port, "test.example.com", tls);
        ok &= expect(!status.ok, "handshake fails when the trust anchor did not issue the server certificate");
        if (!status.ok) {
            ok &= expect(contains(status.message, "certificate") || contains(status.message, "unknown_ca") ||
                             contains(status.message, "TLS alert"),
                         "wrong-CA failure message mentions certificate verification (got: \"" + status.message +
                             "\")");
        }
    }

    // ---- 6. host name mismatch: must fail ----------------------------------
    // No SNI given: the client verifies the connect host (the IP literal
    // 127.0.0.1), which the certificate does not contain.
    {
        const TlsConfig tls{.ca_path = test_ca, .insecure_skip_verify = false};
        const TransportStatus status = try_connect(rsa_server.server.port, "", tls);
        ok &= expect(!status.ok, "handshake fails when the host does not match the certificate");
    }

    // ---- 7. missing/invalid ca_path fails fast -----------------------------
    {
        const TlsConfig tls{.ca_path = certs_dir + "/does-not-exist.pem", .insecure_skip_verify = false};
        const TransportStatus status = try_connect(rsa_server.server.port, "test.example.com", tls);
        ok &= expect(!status.ok, "connect fails fast for a missing ca_path");
        if (!status.ok) {
            ok &= expect(contains(status.message, "does-not-exist.pem"),
                         "missing-CA error names the offending file (got: \"" + status.message + "\")");
        }
        // A PEM private key is also not a certificate bundle.
        const TlsConfig key_as_ca{.ca_path = rsa_key, .insecure_skip_verify = false};
        const TransportStatus key_status = try_connect(rsa_server.server.port, "test.example.com", key_as_ca);
        ok &= expect(!key_status.ok, "connect fails fast when ca_path is not a certificate bundle");
    }

    // ---- 8. insecure handshake, RSA certificate: backwards compatible ------
    {
        const TlsConfig tls{.insecure_skip_verify = true};
        const TransportStatus status = try_connect(rsa_server.server.port, "", tls);
        ok &= expect(status.ok, "insecure handshake still succeeds against the RSA certificate" +
                                    (status.ok ? std::string() : ": " + status.message));
    }

    stop_server(rsa_server.server);
    stop_server(ed_server.server);
    stop_server(p384_server.server);

    std::cerr << (ok ? "tls verify test PASS" : "tls verify test FAIL") << std::endl;
    return ok ? 0 : 1;
}
