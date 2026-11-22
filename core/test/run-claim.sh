#!/usr/bin/env bash
# Build and run the claim tests (no Qt, no Logos SDK, no network). usage: run-claim.sh [bits]
set -euo pipefail
cd "$(dirname "$0")/.."
SI=${SODIUM_INCLUDE:-/nix/store/b6h4f5dcyn0dbi8a9b6nk8z6k7isjrvk-libsodium-1.0.20-dev/include}
SL=${SODIUM_LIB:-/nix/store/0ncz9ijx5slwskb2bbbacygs8apnm6rg-libsodium-1.0.20/lib}
NJ=${JSON_INCLUDE:-/nix/store/r82wggrhgs0v2kkp0f2g9jplc4mfh2gq-nlohmann_json-3.11.3/include}
OUT=${TMPDIR:-/tmp}/bubble_claim_test
g++ -O2 -std=c++17 -Wall -Isrc -I"$SI" -I"$NJ" test/claim_test.cpp src/claim.cpp src/sim.cpp src/check.cpp src/pow.cpp -L"$SL" -lsodium -o "$OUT"
LD_LIBRARY_PATH="$SL" "$OUT" "${1:-6}"
