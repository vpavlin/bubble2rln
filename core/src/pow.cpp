#include "pow.h"
#include <sodium.h>
#include <cstring>

namespace bubble {

bool seedFromHex(const std::string& hex, unsigned char out[32]) {
    if (hex.size() != 64) return false;
    size_t len = 0;
    return sodium_hex2bin(out, 32, hex.data(), hex.size(), nullptr, &len, nullptr) == 0 && len == 32;
}

int leadingZeroBits(const unsigned char h[32]) {
    int n = 0;
    for (int i = 0; i < 32; i++) {
        if (!h[i]) { n += 8; continue; }
        for (int b = 7; b >= 0 && !(h[i] >> b & 1); b--) n++;
        break;
    }
    return n;
}

bool digestOk(const unsigned char seed[32], uint32_t chunk, uint32_t tick, uint32_t state, uint32_t nonce, int bits, size_t memKiB) {
    unsigned char pw[16 + 32 + 16];
    memcpy(pw, "bubble/v2/digest", 16);
    memcpy(pw + 16, seed, 32);
    const uint32_t v[4] = {chunk, tick, state, nonce};
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < 4; i++) pw[48 + 4 * k + i] = (unsigned char)(v[k] >> (8 * i));
    unsigned char sb[14 + 32], salt[crypto_hash_sha256_BYTES];
    memcpy(sb, "bubble/v2/salt", 14);
    memcpy(sb + 14, seed, 32);
    crypto_hash_sha256(salt, sb, sizeof sb);
    unsigned char h[32];
    if (crypto_pwhash(h, sizeof h, reinterpret_cast<const char*>(pw), sizeof pw, salt, 1, memKiB * 1024,
                      crypto_pwhash_ALG_ARGON2ID13) != 0)
        return false;
    return leadingZeroBits(h) >= bits;
}

}
