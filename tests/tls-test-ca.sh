#!/bin/sh
# tests/tls-test-ca.sh OUTDIR — creates the test certificates for the QEMU HTTPS check.
#
#   OUTDIR/ca.der          the test CA (DER). Embedded in test builds only (HELIX_TLS_TEST).
#   OUTDIR/leaf.pem/.key   a certificate for the IP address 10.0.2.2 (the QEMU gateway,
#                          which reaches the host's 127.0.0.1), signed by the test CA.
#   OUTDIR/other_leaf.pem/.key  a certificate for the same address, signed by a second,
#                          unrelated CA that the kernel does not trust (the negative case).
#
# Validity is three days from now. Run by `make test`; the files are not committed.
set -e
OUT=$1
mkdir -p "$OUT"
EXT='subjectAltName=IP:10.0.2.2
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature'

mk_ca() {  # name
    openssl req -x509 -newkey rsa:2048 -nodes -keyout "$OUT/$1.key" -out "$OUT/$1.pem" -days 3 \
        -subj "/CN=Helix TLS test $1" -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
}
mk_leaf() {  # name ca-name
    openssl ecparam -name prime256v1 -genkey -noout -out "$OUT/$1.key" 2>/dev/null
    openssl req -new -key "$OUT/$1.key" -out "$OUT/$1.csr" -subj "/CN=10.0.2.2" 2>/dev/null
    printf '%s\n' "$EXT" > "$OUT/$1.ext"
    openssl x509 -req -in "$OUT/$1.csr" -CA "$OUT/$2.pem" -CAkey "$OUT/$2.key" -CAcreateserial \
        -days 3 -extfile "$OUT/$1.ext" -out "$OUT/$1.pem" 2>/dev/null
    rm -f "$OUT/$1.csr" "$OUT/$1.ext"
}
mk_ca test_ca
mk_ca other_ca
openssl x509 -in "$OUT/test_ca.pem" -outform DER -out "$OUT/ca.der"
mk_leaf leaf test_ca
mk_leaf other_leaf other_ca
rm -f "$OUT"/*.srl
echo "tls-test-ca: wrote $OUT/ca.der, leaf.pem, other_leaf.pem"
