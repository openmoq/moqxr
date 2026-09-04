# Relay Interoperability

## Basic Relay Publish

To attempt a live publish against a relay:

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://relay.example.com:443/moq \
  --namespace interop \
  --forward 0 \
  --timeout 10 \
  --paced
```

## Relay Failover

List relays in preferred order by repeating `--endpoint`. `--retry N` retries
the current connection `N` times after the initial attempt, waiting one second
before each retry. Only after that budget is exhausted does the publisher move
to the next endpoint.

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --transport webtransport \
  --endpoint https://primary.example.com:443/moq \
  --endpoint https://backup.example.com:443/moq \
  --retry 2 \
  --namespace interop \
  --forward 1 \
  --paced
```

Transport closure, connection failure, readiness/subscriber timeout, and a
draft-18 `GOAWAY` migration signal are retryable. Namespace or track rejection
is endpoint-permanent and advances without spending retries. Fatal local errors
and cancellation do not fail over. The standard-error records identify each
attempt and transition:

```text
endpoint_attempt endpoint=primary.example.com:443 attempt=1/3
endpoint_retry endpoint=primary.example.com:443 retry=1/2 delay_ms=1000 reason="..."
endpoint_failover from=primary.example.com:443 to=backup.example.com:443 reason="..."
endpoints_exhausted count=2 reason="..."
```

All endpoints use the same command-line transport, ALPN, SNI, and TLS trust
configuration. WebTransport therefore requires an explicit path on every
endpoint, and an explicit `--sni` must be valid for every target. Without
`--sni`, each connection uses its endpoint host for server-name verification.

Publish the same stream with SAP timeline tracks included:

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://relay.example.com:443/moq \
  --namespace interop \
  --forward 0 \
  --timeout 10 \
  --paced \
  --sap
```

## SNI Override

If you need to connect to a relay by IP while still presenting the relay hostname in TLS SNI:

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint 203.0.113.10:443 \
  --sni relay.example.com \
  --namespace interop \
  --forward 0 \
  --timeout 10
```

Use `--insecure` only when intentionally testing a relay with an untrusted or self-signed certificate. Public relays should be exercised with normal TLS verification so certificate and SNI regressions are visible.

Server certificate verification is enforced for both the raw QUIC and WebTransport transports unless `--insecure` is passed. The trust anchors are resolved in this order: `--ca <bundle.pem>` if provided, then the `SSL_CERT_FILE` environment variable, then the platform's system CA bundle (e.g. `/etc/ssl/certs/ca-certificates.crt`). If none of these yields a usable PEM bundle, the connection fails with an explanatory error instead of silently skipping verification. Hostname (or IP address) checking is performed against `--sni` when given, otherwise against the host used to connect, so relays reached by IP need a certificate with a matching IP subjectAltName or an explicit `--sni` matching the certificate. Note that verified TLS also broadens the signature algorithms offered in the ClientHello (adding e.g. Ed25519), so relays with Ed25519 certificates require verification to be enabled.

## Verified-TLS WebTransport Examples

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input tmp-relay-test.mp4 \
  --transport webtransport \
  --endpoint https://<moqx-relay-host>:4433/moq-relay \
  --namespace live/paul1 \
  --forward 0 \
  --timeout 10 \
  --paced \
  --draft 16
```

```bash
OPENMOQ_PICOQUIC_TRACE=1 ./build/openmoq-publisher \
  --input tmp-relay-test.mp4 \
  --transport webtransport \
  --endpoint https://moq-relay.red5.net:4433/moq \
  --namespace live/paul1 \
  --forward 0 \
  --timeout 10 \
  --paced \
  --draft 16
```

`moq-relay.red5.net:4433` currently accepts WebTransport on `/moq`; `/moq-relay` returns HTTP `404` during CONNECT. The moqx relay examples use a placeholder hostname because those relay hostnames are not public yet; moqx uses `/moq-relay`.

## CAT4MOQ Authorization with moqx

For moqx services with auth enabled, use the auth example instead of the generic CLI. It obtains CAT4MOQ token bytes from a file or Catapult command, configures `PublisherConfig::authorization`, and publishes a deterministic live-object track.

Build the example:

```bash
cmake --build build --target openmoq-publisher-auth-example
```

Run against an already-started relay:

```bash
CAT4MOQ_TOKEN_FILE=/tmp/publish-token.cwt \
CAT4MOQ_ENDPOINT='https://127.0.0.1:4433/moq-relay' \
CAT4MOQ_NAMESPACE='cat4moq.example' \
CAT4MOQ_TRACK='video' \
./examples/auth/run-cat4moq-auth-example.sh
```

Run with separate setup/action tokens:

```bash
CAT4MOQ_SETUP_TOKEN_FILE=/tmp/setup.cwt \
CAT4MOQ_ACTION_TOKEN_FILE=/tmp/publish.cwt \
CAT4MOQ_ENDPOINT='https://127.0.0.1:4433/moq-relay' \
./examples/auth/run-cat4moq-auth-example.sh
```

Run with moqx as the Catapult/CAT4MOQ issuer command:

```bash
CATAPULT_CAT4MOQ_COMMAND='../moqx/build/moqx issue-cat-token --config /tmp/moqx-auth.yaml --auth-service live --auth-key-id cat-dev --auth-actions client_setup,publish_namespace,publish --auth-namespace {namespace} --auth-track {track}' \
CAT4MOQ_ENDPOINT='https://127.0.0.1:4433/moq-relay' \
./examples/auth/run-cat4moq-auth-example.sh
```

When using the default CAT wrapper, configure moqx service auth with
`token_type: 16`, matching the token type wrapped by moqxr. If using
`CAT4MOQ_TOKEN_WRAPPER=out-of-band`, configure `token_type: 0`. See
[examples/auth/README.md](../examples/auth/README.md) for the local moqx auth
config, token generation, token encoding, and focused-test workflow.

## Trace CSV

If you want a per-object CSV trace for pacing and enqueue correlation, set `OPENMOQ_PICOQUIC_TRACE_CSV` alongside `OPENMOQ_PICOQUIC_TRACE`:

```bash
OPENMOQ_PICOQUIC_TRACE=1 \
OPENMOQ_PICOQUIC_TRACE_CSV=/tmp/openmoq-publisher-trace.csv \
./build/openmoq-publisher \
  --input sample.mp4 \
  --endpoint moqt://relay.example.com:443/moq \
  --namespace interop \
  --forward 0 \
  --timeout 10 \
  --paced
```

Rows include `pacing_before`, `pacing_after`, `enqueue`, and `served`/`sent` events for media objects.

## Behavior Notes

- `--forward 0` waits for inbound `SUBSCRIBE` requests before sending matching media objects
- with `--forward 0`, subscribers are still expected to request tracks explicitly
- by default, subscribers should subscribe to `catalog` if they need track discovery
- `--publish-catalog` keeps `--forward 0` for media tracks but proactively publishes the `catalog` track through the normal `PUBLISH` / `PUBLISH_OK` path
- `--sap` adds per-track `*_sap` event timeline tracks and metadata objects
- media packaging defaults to lower-latency split MOQT objects per group when chunk/sample boundaries are available
- `--coalesce-cmaf-chunks` disables that split and falls back to one media object per group
- when multiple tracks are subscribed, matching objects are served in publish-plan order so time-aligned audio/video stay interleaved
- `--forward 1` proactively publishes tracks and objects after namespace setup completes
- `--timeout <seconds>` controls how long the publisher waits for inbound `SUBSCRIBE` requests; the default is 30 seconds
- repeated `--endpoint` options define the ordered failover list; `--retry <count>` is the number of retries after each endpoint's initial attempt
- `--sni <value>` overrides the TLS SNI sent to the relay, useful when `--endpoint` uses a raw IP address
- WebTransport still sends HTTP authority from the configured endpoint host
- `--paced` applies pacing only to media-object sends; setup and publish control messages are sent immediately
