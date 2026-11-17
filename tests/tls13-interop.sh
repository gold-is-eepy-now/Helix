#!/bin/sh
# tests/tls13-interop.sh — the TLS 1.3 client (the same kernel code, built for the host)
# against OpenSSL s_server on localhost. Host-only; the kernel is not involved here.
#
# Three runs, each with its own server:
#   1. ECDSA P-256 leaf, trusted root                       -> must verify and get 200
#   2. RSA leaf (OpenSSL signs with RSA-PSS), trusted root  -> must verify and get 200
#   3. ECDSA leaf, but the client trusts a different root   -> must be refused (exit 3)
#   4. leaf for the IP address 127.0.0.1 (iPAddress SAN), host given as an address -> verify
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W=${TMPDIR:-/tmp}/helix-tls-interop.$$
mkdir -p "$W"
PIDS=""
cleanup() { for p in $PIDS; do kill "$p" 2>/dev/null || true; done; rm -rf "$W"; }
trap cleanup EXIT

EXT='subjectAltName=DNS:localhost
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature'

# A private CA and an unrelated second CA.
for ca in ca other; do
    openssl req -x509 -newkey rsa:2048 -nodes -keyout "$W/$ca.key" -out "$W/$ca.pem" -days 3 \
        -subj "/CN=Helix Interop $ca" -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
    openssl x509 -in "$W/$ca.pem" -outform DER -out "$W/$ca.der"
done

# leaf <name> <key type>: a "localhost" certificate signed by the first CA.
leaf() {
    if [ "$2" = ec ]; then
        openssl ecparam -name prime256v1 -genkey -noout -out "$W/$1.key" 2>/dev/null
    else
        openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$W/$1.key" 2>/dev/null
    fi
    openssl req -new -key "$W/$1.key" -out "$W/$1.csr" -subj "/CN=localhost" 2>/dev/null
    if [ -n "$3" ]; then printf '%s\n' "$3" > "$W/$1.ext"; else printf '%s\n' "$EXT" > "$W/$1.ext"; fi
    openssl x509 -req -in "$W/$1.csr" -CA "$W/ca.pem" -CAkey "$W/ca.key" -CAcreateserial \
        -days 3 -extfile "$W/$1.ext" -out "$W/$1.pem" 2>/dev/null
}
leaf leaf_ec ec
leaf leaf_rsa rsa
leaf leaf_ip ec "subjectAltName=IP:127.0.0.1
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature"

# Build the host harness from the same kernel sources the kernel build compiles.
cc -std=gnu11 -Wall -Wextra -DHELIX_HOST_TEST -I"$ROOT/kernel/src" -o "$W/tls13-interop" \
    "$ROOT/tests/tls13-interop.c" "$ROOT/kernel/src/tls13_client.c" "$ROOT/kernel/src/tls13.c" \
    "$ROOT/kernel/src/tls13_hello.c" "$ROOT/kernel/src/tls13_server.c" "$ROOT/kernel/src/tls13_record.c" \
    "$ROOT/kernel/src/tls13_verify.c" "$ROOT/kernel/src/x509.c" "$ROOT/kernel/src/x509_verify.c" \
    "$ROOT/kernel/src/ecdsa_p256.c" "$ROOT/kernel/src/rsa.c" "$ROOT/kernel/src/bignum.c" \
    "$ROOT/kernel/src/x25519.c" "$ROOT/kernel/src/chacha20poly1305.c" "$ROOT/kernel/src/hkdf.c" \
    "$ROOT/kernel/src/sha256.c" "$ROOT/kernel/src/kstring.c" "$ROOT/kernel/src/trust_roots.c"

# run <label> <port> <leaf name> <trusted root DER> <expected exit code> [host]
run() {
    label=$1; port=$2; name=$3; root=$4; want=$5; host=${6:-localhost}
    openssl s_server -quiet -accept "$port" -cert "$W/$name.pem" -key "$W/$name.key" \
        -tls1_3 -ciphersuites TLS_CHACHA20_POLY1305_SHA256 -groups X25519 -www \
        >"$W/server.$port.log" 2>&1 &
    pid=$!
    PIDS="$PIDS $pid"
    sleep 1
    set +e
    "$W/tls13-interop" "$port" "$root" "$host"
    got=$?
    set -e
    kill "$pid" 2>/dev/null || true
    if [ "$got" = "$want" ]; then
        echo "tls13-interop [$label]: PASS (exit $got)"
    else
        echo "tls13-interop [$label]: FAIL (exit $got, wanted $want)"
        exit 1
    fi
}
run "ECDSA leaf, trusted root" 44501 leaf_ec "$W/ca.der" 0
run "RSA leaf (RSA-PSS), trusted root" 44502 leaf_rsa "$W/ca.der" 0
run "ECDSA leaf, wrong root refused" 44503 leaf_ec "$W/other.der" 3
run "IP address host (iPAddress SAN)" 44504 leaf_ip "$W/ca.der" 0 127.0.0.1
echo "tls13-interop: all runs as expected"
