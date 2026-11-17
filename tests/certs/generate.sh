#!/bin/sh
# Regenerates the test PKI used by tests/picoquic_tls_verify_test.cpp.
#
# Host names are carried in subjectAltName. OpenSSL 4 no longer falls back to
# the subject CN when matching a host name, so certificates that name their
# host only in the CN (like picoquic's certs/) fail verification there.
#
# The leaves deliberately carry no IP subjectAltName: the host-mismatch
# scenario connects to 127.0.0.1 and expects verification to fail.
#
# Usage: tests/certs/generate.sh [openssl-binary]
set -eu

OPENSSL="${1:-openssl}"
DAYS=36500
cd "$(dirname "$0")"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

"$OPENSSL" genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out test-ca.key
"$OPENSSL" req -new -x509 -key test-ca.key -subj "/CN=openmoq test ca" -days "$DAYS" -sha256 \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -addext "subjectKeyIdentifier=hash" \
    -out test-ca.crt

# leaf <dir> <host> <genpkey args...>
leaf() {
    dir="$1"; host="$2"; shift 2
    mkdir -p "$dir"
    "$OPENSSL" genpkey "$@" -out "$dir/key.pem"
    "$OPENSSL" req -new -key "$dir/key.pem" -subj "/CN=$host" -out "$tmp/leaf.csr"
    cat > "$tmp/leaf.ext" <<EOF
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = DNS:$host
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
EOF
    "$OPENSSL" x509 -req -in "$tmp/leaf.csr" -CA test-ca.crt -CAkey test-ca.key \
        -CAcreateserial -CAserial "$tmp/ca.srl" -days "$DAYS" -sha256 \
        -extfile "$tmp/leaf.ext" -out "$dir/cert.pem"
}

leaf rsa test.example.com -algorithm RSA -pkeyopt rsa_keygen_bits:2048
leaf ed25519 ed25519.test.example.com -algorithm ED25519
leaf secp384r1 secp384r1.test.example.com -algorithm EC -pkeyopt ec_paramgen_curve:P-384

# The CA key is only needed to issue the leaves; the tests never use it.
rm -f test-ca.key
