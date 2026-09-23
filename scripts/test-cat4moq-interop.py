#!/usr/bin/env python3
"""Opt-in CAT compatibility tests with real issuers, protected relays and Playa.

No sibling checkout is modified. Runtime keys, tokens, media and logs stay in a
private temporary directory, which is retained for inspection. This is current
relay compatibility coverage, not C4M-01 conformance or secure-peering coverage.
"""

import argparse
import base64
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests/fixtures/cat4moq"
SECRET = "interop-test-only-signing-secret-0001"
CASES = ("valid", "valid-publish", "missing", "expired", "tampered", "wrong-key", "wrong-action", "wrong-namespace", "wrong-track", "profile-mismatch")


def run(argv, **kwargs):
    return subprocess.run([str(x) for x in argv], check=True, timeout=30, **kwargs)


def revision(path):
    try:
        return run(["git", "-C", path, "rev-parse", "HEAD"], capture_output=True).stdout.decode().strip()
    except (OSError, subprocess.SubprocessError):
        return None


@contextlib.contextmanager
def process(argv, log, **kwargs):
    with log.open("w") as output:
        child = subprocess.Popen([str(x) for x in argv], stdout=kwargs.pop("stdout", output), stderr=output,
                                 start_new_session=True, **kwargs)
        try:
            yield child
        finally:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGTERM)
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait()


def wait_log(child, log, pattern, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if re.search(pattern, log.read_text(errors="replace")):
            return
        if child.poll() is not None:
            raise RuntimeError(f"process exited {child.returncode}; inspect {log}")
        time.sleep(0.1)
    raise RuntimeError(f"readiness deadline; inspect {log}")


def issue(args, directory, issuer, case, namespace, *, subscriber=False):
    profile = "cose" if issuer == "red5-cose" else "moqx"
    secret = SECRET if case != "wrong-key" else SECRET + "-wrong"
    secret_path = directory / f"{issuer}-{case}-secret"
    secret_path.write_text(base64.b64encode(secret.encode()).decode() if profile == "cose" else secret)
    secret_path.chmod(0o600)
    actions = "client_setup,subscribe_namespace,subscribe,fetch,request_update" if subscriber else "client_setup,publish_namespace,publish"
    if case == "wrong-action":
        actions = "client_setup,publish_namespace,subscribe"
    granted_ns = namespace + "-wrong" if case == "wrong-namespace" else namespace
    ttl = "1" if case == "expired" else "3600"
    if issuer == "moqx":
        config = directory / f"issuer-{case}.yaml"
        config.write_text("listeners:\n  - name: issuer-unused\n    udp:\n      socket: {address: \"127.0.0.1\", port: 4433}\n    tls: {insecure: true}\n    endpoint: \"/moq\"\nservices:\n  live:\n    match:\n      - authority: {any: true}\n        path: {prefix: '/'}\n    cache: {enabled: true, max_tracks: 100, max_groups_per_track: 3}\n    auth:\n      enabled: true\n      hmac_keys:\n        - id: interop\n          secret: " + json.dumps(secret) + "\n")
        command = [args.moqx_issuer, "--config", config, "--auth_service", "live", "--auth_key_id", "interop",
                   "--auth_actions", actions, "--auth_namespace", granted_ns, "--auth_ttl_seconds", ttl]
        if case == "wrong-track":
            command += ["--auth_track", "never-published"]
    else:
        command = ["python3", args.red5 / "scripts/issue-cat-token.py", "--profile", profile,
                   "--secret-file", secret_path, "--key-id", "interop", "--actions", actions,
                   "--namespace", granted_ns, "--ttl-seconds", ttl]
        if case == "wrong-track":
            command += ["--track", "never-published"]
    output = run(command, capture_output=True, env=args.child_env).stdout.strip()
    if not output.startswith(b"base64:"):
        raise RuntimeError(f"{issuer} did not emit a base64 token")
    token = base64.b64decode(output[7:], validate=True)
    if case == "tampered":
        token = token[:-1] + bytes([token[-1] ^ 1])
    path = directory / f"{issuer}-{'subscriber' if subscriber else case}.cwt"
    path.write_bytes(token)
    path.chmod(0o600)
    return path


def verify_issuers(args, directory):
    classpath = f"{args.red5}/target/classes:{args.red5}/target/lib/*"
    run(["javac", "-cp", classpath, "-d", directory, FIXTURES / "VerifyToken.java"], capture_output=True)
    run(["javac", "-cp", classpath, "-d", directory, FIXTURES / "VerifyC4m01.java"], capture_output=True)
    checked = run(["java", "-cp", f"{directory}:{classpath}", "VerifyC4m01",
                   directory / "c4m01-controlled.cwt"], capture_output=True)
    print(checked.stdout.decode().strip(), flush=True)
    args.results.append({"target": "c4m01-controlled", "phase": "claim-fixture", "decisions": 18,
                         "result": "pass", "claim_label": -65537, "token_type": 1})
    for issuer in args.targets:
        group = directory / (issuer + "-issuer")
        group.mkdir()
        tokens = {case: issue(args, group, issuer, case, "cat4moq/issuer") for case in CASES
                  if case not in ("missing", "valid-publish", "profile-mismatch")}
        time.sleep(2)  # Expired tokens are genuinely signed with the issuer's minimum TTL.
        profile = "cose" if issuer == "red5-cose" else "moqx"
        secret = group / f"{issuer}-valid-secret"
        for case, token in tokens.items():
            run(["java", "-cp", f"{directory}:{classpath}", "VerifyToken", profile, secret, token,
                 "PUBLISH", "cat4moq/issuer", "video", str(case == "valid").lower(),
                 str(case not in ("expired", "tampered", "wrong-key")).lower()], capture_output=True)
        # A valid publisher grant must not authorize subscription or other resources.
        for action, namespace, expected in (("CLIENT_SETUP", "cat4moq/issuer", "true"),
                                             ("SUBSCRIBE", "cat4moq/issuer", "false"),
                                             ("PUBLISH", "cat4moq/other", "false")):
            run(["java", "-cp", f"{directory}:{classpath}", "VerifyToken", profile, secret, tokens["valid"],
                 action, namespace, "video", expected, "true"], capture_output=True)
        args.results.append({"target": issuer, "phase": "issuer-validation", "decisions": 10, "result": "pass"})
        print(f"PASS: {issuer} issuer -> Red5 {profile} validator: 10 signature/scope decisions", flush=True)


def runtime(args, directory, issuer):
    group = directory / issuer
    group.mkdir()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    cert, key = group / "cert.pem", group / "key.pem"
    run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
         "-nodes", "-days", "7", "-subj", "/CN=localhost", "-addext",
         "subjectAltName=DNS:localhost,IP:127.0.0.1", "-keyout", key, "-out", cert], capture_output=True)
    profile = "cose" if issuer == "red5-cose" else "moqx"
    if issuer == "moqx":
        config = group / "relay.yaml"
        config.write_text(f'''threads: 2
listeners:
  - name: local
    udp:
      socket: {{address: "127.0.0.1", port: {port}}}
    tls:
      insecure: false
      cert_file: {json.dumps(str(cert))}
      key_file: {json.dumps(str(key))}
    endpoint: "/moq"
    moqt_versions: [18]
services:
  live:
    match:
      - authority: {{any: true}}
        path: {{exact: "/moq"}}
    cache: {{enabled: true, max_tracks: 100, max_groups_per_track: 3}}
    auth:
      enabled: true
      token_type: 16
      hmac_keys:
        - id: interop
          secret: {json.dumps(SECRET)}
      require_setup_token: true
      allow_request_token_override: true
      strict_claims: false
''')
        relay = [args.moqx, "serve", "--config", config, "--logtostderr=1"]
        ready = r"Listening|listening|started|Started"
    else:
        secret = group / "relay-secret"
        secret.write_text(base64.b64encode(SECRET.encode()).decode() if profile == "cose" else SECRET)
        properties = (args.red5 / "src/main/resources/server.properties").read_text()
        properties += f"""
server.quic.implementation={args.red5_quic}
server.unified.enabled=true
auth.cat.enabled=true
auth.cat.allow.anonymous=false
auth.cat.profile={profile}
auth.cat.require.setup.token=true
auth.cat.expiration.tolerance=0
auth.cat.key.ids=interop
auth.cat.key.interop.secret.file={secret}
"""
        (group / "server.properties").write_text(properties)
        relay = ["java", f"-Djava.library.path={args.red5}/quiche-jni/build/lib:{args.red5}/target/native:{args.red5}/lib",
                 "-cp", f"{args.red5}/target/classes:{args.red5}/target/lib/*", "org.red5.server.net.picoquic.Main",
                 "-host", "127.0.0.1", "-port", str(port), "-cert", cert, "-key", key]
        ready = r"Unified MoQ Relay Server started|Created quiche unified server|started successfully"
    log = group / "relay.log"
    with process(relay, log, cwd=group, env=args.child_env) as server:
        if issuer == "moqx":
            time.sleep(1)
            if server.poll() is not None:
                raise RuntimeError(f"moqx exited before clients started; inspect {log}")
        else:
            wait_log(server, log, ready)
        failures = []
        for case in args.cases:
            try:
                namespace = f"cat4moq/{issuer}/{case}"
                token = None if case == "missing" else issue(args, group, issuer, case, namespace)
                subscriber = issue(args, group, issuer, "valid", namespace, subscriber=True)
                if case == "expired":
                    time.sleep(2)
                publog = group / f"{case}-publisher.log"
                sublog = group / f"{case}-subscriber.log"
                source_command = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-re",
                                  "-stream_loop", "-1", "-i", directory / "sample.mp4", "-c", "copy",
                                  "-movflags", "+frag_keyframe+empty_moov+default_base_moof+separate_moof",
                                  "-f", "mp4", "pipe:1"]
                publisher = [args.publisher, "--input", "-", "--transport", args.publisher_transport,
                             "--namespace", namespace, "--draft", "18", "--endpoint", f"{'https' if args.publisher_transport == 'webtransport' else 'moqt'}://127.0.0.1:{port}/moq",
                             "--insecure", "--forward", "1" if case in ("valid-publish", "wrong-action", "wrong-track") else "0", "--coalesce-cmaf-chunks", "--publish-catalog", "--catalog-republish-interval", "1",
                             "--timeout", "30"]
                if token:
                    # Each relay runs on its default token type: the cose profile follows
                    # draft-ietf-moq-c4m-01 (type 1), moqx and Red5's moqx profile use 16.
                    # The mismatch case presents the credential under the other type.
                    native_profile = "c4m-01" if profile == "cose" else "moqx-compat"
                    other_profile = "moqx-compat" if profile == "cose" else "c4m-01"
                    auth_profile = other_profile if case == "profile-mismatch" else native_profile
                    publisher += ["--auth-profile", auth_profile,
                                  "--auth-token-file", token]
                relay_offset = log.stat().st_size
                with process(source_command, group / f"{case}-ffmpeg.log", stdout=subprocess.PIPE) as source, \
                        process(publisher, publog, env=args.child_env, stdin=source.stdout) as pub:
                    source.stdout.close()
                    if case == "valid-publish":
                        # On draft 18 these markers follow a decoded PUBLISH_OK,
                        # not just a send; both publisher implementations check acceptance.
                        for track in ("vide_1", "soun_2"):
                            wait_log(pub, publog, rf"live: PUBLISH track={track} .*\(request stream\)|libmoq: publication accepted track={track}(?:\s|$)", timeout=args.timeout)
                        args.results.append({"target": issuer, "phase": "runtime", "case": case, "result": "pass", "evidence": "PUBLISH_OK for both media tracks; control only"})
                        print(f"PASS: {issuer}/{case}: explicit PUBLISH_OK for audio/video (control only)", flush=True)
                        continue
                    if case == "valid":
                        wait_log(pub, publog, r"namespace published|live: awaiting subscriptions|libmoq: sender ready", timeout=args.timeout)
                    else:
                        # Input discovery can take several seconds before SETUP.
                        # Wait for readiness or rejection before snapshotting the
                        # relay log, so setup evidence still belongs to this publisher.
                        deadline = time.monotonic() + args.timeout
                        while pub.poll() is None and time.monotonic() < deadline:
                            if re.search(r"namespace published|live: awaiting subscriptions|libmoq: sender ready", publog.read_text(errors="replace")):
                                break
                            time.sleep(0.05)
                    timeout = args.timeout if case == "valid" else 8
                    command = [args.node]
                    if args.quic_package:
                        command += ["--experimental-quic"]
                    command += [FIXTURES / "subscribe.mjs", args.playa, args.node_modules,
                                f"{'moqt' if args.quic_package else 'https'}://127.0.0.1:{port}/moq",
                                namespace, cert, subscriber, str(timeout), args.quic_package or "", args.subscriber_api,
                                "1" if profile == "cose" else "16"]
                    # Only pre-subscriber relay evidence can attribute a setup rejection
                    # to the publisher without relying on implementation-specific IDs.
                    publisher_relay_text = log.read_bytes()[relay_offset:].decode(errors="replace")
                    with process(command, sublog, env=args.child_env) as sub:
                        if case != "valid":
                            deadline = time.monotonic() + args.timeout
                            while pub.poll() is None and time.monotonic() < deadline:
                                if re.search(r'"event":"(catalog|media|payload)"', sublog.read_text(errors="replace")):
                                    raise RuntimeError(f"{issuer}/{case}: DENIED PUBLISHER DELIVERED PAYLOAD; inspect {sublog}")
                                time.sleep(0.05)
                            if pub.poll() is None:
                                raise RuntimeError(f"{issuer}/{case}: no bounded publisher rejection; inspect {publog}")
                            text = publog.read_text(errors="replace")
                            rejection = (r"(?i)unauthoriz|forbidden|denied|not permitted|authorization token does not permit|auth.*fail|"
                                 r"peer rejected publication authorization|"
                                 r"(MALFORMED|EXPIRED)_AUTH_TOKEN|UNKNOWN_AUTH_TOKEN_ALIAS")
                            if pub.returncode == 0 or not re.search(rejection, text + "\n" + publisher_relay_text):
                                raise RuntimeError(f"{issuer}/{case}: missing publisher-attributed authorization rejection; inspect {publog}")
                        result = sub.wait(timeout=timeout + 10)
                    evidence = sublog.read_text(errors="replace")
                    if '"event":"setup_accepted"' not in evidence:
                        raise RuntimeError(f"{issuer}/{case}: subscriber setup not accepted; inspect {sublog}")
                    if re.search(r"(?i)unauthoriz|not authorized|forbidden|denied|not permitted|authorization token does not permit|auth.*fail", evidence):
                        raise RuntimeError(f"{issuer}/{case}: subscriber authorization failed; inspect {sublog}")
                    if case == "valid":
                        if result != 0 or "PASS: Playa received" not in evidence or pub.poll() is not None:
                            raise RuntimeError(f"{issuer}/{case}: catalog/media proof missing; inspect {sublog} and {publog}")
                    elif re.search(r'"event":"(catalog|media|payload)"', evidence):
                        raise RuntimeError(f"{issuer}/{case}: denied publisher delivered payload; inspect {sublog}")
                    elif "catalog/media deadline exceeded" not in evidence:
                        raise RuntimeError(f"{issuer}/{case}: subscriber observation failed; inspect {sublog}")
                if server.poll() is not None:
                    raise RuntimeError(f"{issuer} relay died; inspect {log}")
                args.results.append({"target": issuer, "phase": "runtime", "case": case, "result": "pass"})
                print(f"PASS: {issuer}/{case}: " + ("subscriber catalog + progressing audio/video CMAF" if case == "valid" else "publisher rejected, no subscriber catalog/media"), flush=True)
            except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                args.results.append({"target": issuer, "phase": "runtime", "case": case, "result": "fail", "error": str(error)})
                print(f"FAIL: {error}", flush=True)
                failures.append(case)
                if case in ("valid", "valid-publish"):
                    raise
        if failures:
            raise RuntimeError(f"{issuer}: failed negative cases: {', '.join(failures)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--targets", nargs="+", choices=("moqx", "red5-moqx", "red5-cose"), default=["moqx", "red5-moqx", "red5-cose"])
    parser.add_argument("--cases", nargs="+", choices=CASES, default=list(CASES))
    parser.add_argument("--issuer-only", action="store_true", help="no sockets; actual issuer and Red5 validator checks only")
    parser.add_argument("--moqx", type=Path, default=ROOT.parent / "moqx/build-san/moqx")
    parser.add_argument("--moqx-issuer", type=Path, default=ROOT.parent / "moqx/build-san/moqx-issuer")
    parser.add_argument("--red5", type=Path, default=ROOT.parent.parent / "github-red5pro/red5-moq-relay")
    parser.add_argument("--red5-quic", choices=("quiche", "picoquic"), default="picoquic")
    parser.add_argument("--playa", type=Path, default=ROOT.parent / "moq-playa")
    parser.add_argument("--node-modules", type=Path, default=ROOT.parent / "moqx/test/playa", help="directory whose node_modules contains @fails-components/webtransport")
    parser.add_argument("--publisher", type=Path, default=ROOT / "build/openmoq-publisher")
    parser.add_argument("--publisher-transport", choices=("raw", "webtransport"), default="raw")
    parser.add_argument("--publisher-backend", choices=("native", "libmoq", "unspecified"), default="unspecified",
                        help="record the selected build backend; this does not change the executable")
    parser.add_argument("--subscriber-api", choices=("player", "connection"), default="player",
                        help="Playa player delivery or direct per-track connection delivery")
    parser.add_argument("--node", default="node")
    parser.add_argument("--quic-package", type=Path, help="built Playa quic/index.js; use raw QUIC receiver and Node >=26.8.1")
    parser.add_argument("--timeout", type=int, default=25)
    args = parser.parse_args()
    if any(case in args.cases for case in ("wrong-action", "wrong-track")) and "valid-publish" not in args.cases:
        args.cases.insert(1, "valid-publish")
    for field in ("moqx", "moqx_issuer", "red5", "playa", "node_modules", "publisher"):
        setattr(args, field, getattr(args, field).resolve())
    if args.quic_package:
        args.quic_package = args.quic_package.resolve()
    args.child_env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", PYTHONDONTWRITEBYTECODE="1")
    args.results = []
    directory = Path(tempfile.mkdtemp(prefix="cat4moq-interop-"))
    print(f"Artifacts: {directory}", flush=True)
    try:
        verify_issuers(args, directory)
        if not args.issuer_only:
            if args.cases[0] != "valid":
                raise RuntimeError("runtime cases must start with valid to prove subscriber/relay liveness")
            run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
                 "testsrc2=size=320x180:rate=24", "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000",
                 "-t", "8", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-g", "24",
                 "-sc_threshold", "0", "-c:a", "aac", "-b:a", "64k", "-movflags",
                 "+frag_keyframe+empty_moov+default_base_moof+separate_moof", directory / "sample.mp4"])
            failed = False
            for target in args.targets:
                try:
                    runtime(args, directory, target)
                except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                    args.results.append({"target": target, "phase": "runtime", "result": "fail", "error": str(error)})
                    print(f"FAIL: {error}", flush=True)
                    failed = True
            if failed:
                return 1
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        args.results.append({"result": "fail", "error": str(error)})
        print(f"FAIL: {error}", flush=True)
        return 1
    finally:
        metadata = {"publisher": str(args.publisher), "publisher_backend": args.publisher_backend,
                    "publisher_transport": args.publisher_transport, "draft": 18,
                    "subscriber_transport": "raw" if args.quic_package else "webtransport",
                    "subscriber_api": args.subscriber_api,
                    "red5_backend": args.red5_quic,
                    "revisions": {"moqxr": revision(ROOT), "moq5": revision(ROOT.parent / "moq5"),
                                  "moqx": revision(ROOT.parent / "moqx"), "red5": revision(args.red5),
                                  "playa": revision(args.playa)}}
        if not args.issuer_only and args.publisher.is_file():
            with args.publisher.open("rb") as binary:
                metadata["publisher_sha256"] = hashlib.file_digest(binary, "sha256").hexdigest()
        (directory / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
        (directory / "results.json").write_text(json.dumps(args.results, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
