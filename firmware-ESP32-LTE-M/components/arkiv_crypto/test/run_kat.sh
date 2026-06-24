#!/usr/bin/env bash
# Host cross-impl KAT for arkiv_crypto (ADR-0011 P3 / ESP-FW-6). No hardware.
#   1. gen_kat.mjs: golden vectors (OpenSSL/node + @noble/curves cross-check)
#   2. compile firmware micro-ecc + aead.c + kat_host.c on host
#      (HMAC via CommonCrypto, GCM via OpenSSL EVP -DARKIV_AEAD_HOST_GCM)
#   3. assert firmware code == golden vectors (ECDH, HKDF, GCM)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMP="$(cd "$HERE/.." && pwd)"                  # components/arkiv_crypto
MICROECC="$(cd "$COMP/../microecc" && pwd)"
REPO="$(cd "$COMP/../../../.." && pwd)"          # mono-repo root
OUT="$(mktemp -d)"

# @noble/curves (reader lib) for the optional cross-check.
NOBLE_PATH="$(find "$REPO/Web3-Pi-UPS-Panel/node_modules/.pnpm" \
  -path '*@noble/curves/esm/secp256k1.js' 2>/dev/null | sort | tail -1 || true)"
export NOBLE_PATH

# OpenSSL (host GCM oracle).
SSL=""
for d in /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3 /opt/homebrew/opt/openssl /usr/local/opt/openssl; do
  [ -e "$d/include/openssl/evp.h" ] && SSL="$d" && break
done
[ -z "$SSL" ] && { echo "ERROR: OpenSSL headers not found (brew install openssl@3)"; exit 2; }

echo "== 1. generate golden vectors =="
node "$HERE/gen_kat.mjs"

echo "== 2. compile micro-ecc + aead.c + kat_host.c =="
cc -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter \
  -DuECC_SUPPORTS_secp160r1=0 -DuECC_SUPPORTS_secp192r1=0 \
  -DuECC_SUPPORTS_secp224r1=0 -DuECC_SUPPORTS_secp256r1=0 \
  -DuECC_SUPPORTS_secp256k1=1 -DuECC_OPTIMIZATION_LEVEL=2 \
  -DuECC_SQUARE_FUNC=1 -DuECC_ENABLE_VLI_API=1 \
  -DARKIV_AEAD_HOST_GCM \
  -I"$MICROECC" -I"$COMP/include" -I"$HERE" -I"$SSL/include" \
  "$MICROECC/uECC.c" "$COMP/src/aead.c" "$HERE/kat_host.c" \
  -L"$SSL/lib" -lcrypto \
  -o "$OUT/kat_host"

echo "== 3. run =="
"$OUT/kat_host"
rc=$?
rm -rf "$OUT"
exit $rc
