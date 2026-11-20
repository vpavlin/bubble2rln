#pragma once
// Digestion proof of work: chunk k of a game is digested by a nonce n with
//   h = argon2id(pw = "bubble/v1/digest" || seed(32) || u32le k || u32le n,
//                salt = sha256("bubble/v1/salt" || seed)[0..16], ops 1, mem memKiB, 1 lane)
// and at least `bits` leading zero bits in h. Needs sodium_init() first.
#include <cstdint>
#include <cstddef>
#include <string>

namespace bubble {
bool seedFromHex(const std::string& hex, unsigned char out[32]);
bool digestOk(const unsigned char seed[32], uint32_t chunk, uint32_t nonce, int bits, size_t memKiB);
int leadingZeroBits(const unsigned char h[32]);
}
