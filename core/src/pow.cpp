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

bool digestOk(const unsigned char seed[32], uint32_t chunk, uint32_t nonce, int bits, size_t memKiB) {
    unsigned char pw[16 + 32 + 8];
    memcpy(pw, "bubble/v1/digest", 16);
    memcpy(pw + 16, seed, 32);
    for (int i = 0; i < 4; i++) { pw[48 + i] = (unsigned char)(chunk >> (8 * i)); pw[52 + i] = (unsigned char)(nonce >> (8 * i)); }
    unsigned char sb[14 + 32], salt[crypto_hash_sha256_BYTES];
    memcpy(sb, "bubble/v1/salt", 14);
    memcpy(sb + 14, seed, 32);
    crypto_hash_sha256(salt, sb, sizeof sb);
    unsigned char h[32];
    if (crypto_pwhash(h, sizeof h, reinterpret_cast<const char*>(pw), sizeof pw, salt, 1, memKiB * 1024,
                      crypto_pwhash_ALG_ARGON2ID13) != 0)
        return false;
    return leadingZeroBits(h) >= bits;
}

}
