#!/bin/sh
# Regenerates the test TLS material for watt_testserver / watt_httpserver and the test suites:
#   ca.pem                      "watt test CA" (self-signed, P-256)
#   server.pem, server.key      CN=localhost, SAN localhost, watt.test, 127.0.0.1
#   client.pem, client.key      CN=watt test client (mTLS tests)
# Valid for 100 years. Test-only keys: never trust ca.pem outside the tests.
# Needs openssl (Git for Windows ships it). Run from this folder: sh make_certs.sh
set -e
export MSYS_NO_PATHCONV=1  # Git Bash: keep "/CN=..." as written
cd "$(dirname "$0")"
days=36500
tmp=.tmp-certs; rm -rf "$tmp"; mkdir "$tmp"
trap 'rm -rf "$tmp"' EXIT

openssl ecparam -name prime256v1 -genkey -noout -out "$tmp/ca.key"
openssl req -x509 -new -key "$tmp/ca.key" -sha256 -days $days -subj "/CN=watt test CA" \
  -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" -out ca.pem

openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out server.key
openssl req -new -key server.key -subj "/CN=localhost" -out "$tmp/server.csr"
printf 'subjectAltName=DNS:localhost,DNS:watt.test,IP:127.0.0.1\nbasicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\n' > "$tmp/server.ext"
openssl x509 -req -in "$tmp/server.csr" -CA ca.pem -CAkey "$tmp/ca.key" -CAcreateserial -sha256 -days $days \
  -extfile "$tmp/server.ext" -out server.pem

openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out client.key
openssl req -new -key client.key -subj "/CN=watt test client" -out "$tmp/client.csr"
printf 'basicConstraints=CA:FALSE\nextendedKeyUsage=clientAuth\n' > "$tmp/client.ext"
openssl x509 -req -in "$tmp/client.csr" -CA ca.pem -CAkey "$tmp/ca.key" -CAcreateserial -sha256 -days $days \
  -extfile "$tmp/client.ext" -out client.pem
rm -f ca.srl
echo "valid until: $(openssl x509 -in server.pem -noout -enddate)"
