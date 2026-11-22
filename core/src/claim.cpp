#include "claim.h"
#include "pow.h"

#include <sodium.h>
#include <algorithm>
#include <cstring>
#include <numeric>

using nlohmann::json;

namespace bubble {

namespace {

void putU32(std::vector<unsigned char>& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((unsigned char)(v >> (8 * i))); }
void putU64(std::vector<unsigned char>& b, uint64_t v) { for (int i = 0; i < 8; i++) b.push_back((unsigned char)(v >> (8 * i))); }
uint32_t getU32(const unsigned char* p) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i); return v; }
uint64_t getU64(const unsigned char* p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); return v; }

std::string hexOf(const unsigned char* p, size_t n) {
    std::string s(n * 2 + 1, '\0');
    sodium_bin2hex(s.data(), s.size(), p, n);
    s.resize(n * 2);
    return s;
}

bool fromHex(std::string h, std::vector<unsigned char>& out) {
    if (h.rfind("0x", 0) == 0 || h.rfind("0X", 0) == 0) h = h.substr(2);
    if (h.size() % 2) return false;
    out.assign(h.size() / 2, 0);
    size_t len = 0;
    return sodium_hex2bin(out.data(), out.size(), h.data(), h.size(), nullptr, &len, nullptr) == 0 && len == out.size();
}

json refuse(const std::string& why) { return {{"ok", false}, {"reason", why}}; }
json error(const std::string& why) { return {{"error", why}}; }

bool validDomain(const std::string& d) {
    if (d.empty() || d.size() > 64) return false;
    for (char c : d) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    return true;
}

}

std::string gameSeed(const Beacon& b, const unsigned char pub[32]) {
    crypto_hash_sha256_state st;
    crypto_hash_sha256_init(&st);
    crypto_hash_sha256_update(&st, (const unsigned char*)"bubble/v2/seed", 14);
    unsigned char id[8];
    for (int i = 0; i < 8; i++) id[i] = (unsigned char)(b.id >> (8 * i));
    crypto_hash_sha256_update(&st, id, 8);
    crypto_hash_sha256_update(&st, b.hash, 32);
    crypto_hash_sha256_update(&st, pub, 32);
    unsigned char out[32];
    crypto_hash_sha256_final(&st, out);
    return hexOf(out, 32);
}

std::vector<unsigned char> encodeClaim(const Claim& c, bool withSig) {
    std::vector<unsigned char> b = {'B', 'B', 'L', '1'};
    putU64(b, c.beaconId);
    b.insert(b.end(), c.pub, c.pub + 32);
    putU32(b, c.end);
    b.push_back((unsigned char)c.chunks.size());
    for (const auto& p : c.chunks) { putU32(b, p.tick); putU32(b, p.state); putU32(b, p.nonce); }
    if (withSig) b.insert(b.end(), c.sig, c.sig + 64);
    return b;
}

bool decodeClaim(const std::vector<unsigned char>& b, Claim& c, std::string& why) {
    const size_t head = 4 + 8 + 32 + 4 + 1;
    if (b.size() < head + 64 || memcmp(b.data(), "BBL1", 4) != 0) { why = "not a bubble claim"; return false; }
    c.beaconId = getU64(&b[4]);
    memcpy(c.pub, &b[12], 32);
    c.end = getU32(&b[44]);
    const size_t n = b[48];
    if (b.size() != head + n * 12 + 64) { why = "claim has the wrong length"; return false; }
    c.chunks.clear();
    for (size_t i = 0; i < n; i++) {
        const unsigned char* p = &b[head + i * 12];
        c.chunks.push_back({getU32(p), getU32(p + 4), getU32(p + 8)});
    }
    memcpy(c.sig, &b[head + n * 12], 64);
    return true;
}

void bindingDigest(const std::string& domain, const std::string& authType, const unsigned char action[32],
                   const std::string& audience, const std::vector<unsigned char>& unsignedPayload, unsigned char out[32]) {
    unsigned char extra[32];
    crypto_hash_sha256(extra, unsignedPayload.data(), unsignedPayload.size());
    const std::string d = "logos/auth/" + domain + "/" + authType;
    crypto_hash_sha256_state st;
    crypto_hash_sha256_init(&st);
    crypto_hash_sha256_update(&st, (const unsigned char*)d.data(), d.size());
    crypto_hash_sha256_update(&st, action, 32);
    const unsigned char len = (unsigned char)audience.size();
    crypto_hash_sha256_update(&st, &len, 1);
    crypto_hash_sha256_update(&st, (const unsigned char*)audience.data(), audience.size());
    crypto_hash_sha256_update(&st, extra, 32);
    crypto_hash_sha256_final(&st, out);
}

bool GifterRules::fromJson(const json& j, GifterRules& o, std::string& why) {
    if (j.is_null()) return true;
    if (!j.is_object()) { why = "config must be an object"; return false; }
    auto num = [&](const char* k, int64_t& v, int64_t lo, int64_t hi) {
        if (!j.contains(k)) return true;
        if (!j[k].is_number_integer() || j[k].get<int64_t>() < lo || j[k].get<int64_t>() > hi) { why = std::string("config.") + k + " out of range"; return false; }
        v = j[k].get<int64_t>();
        return true;
    };
    int64_t bits = o.pow.bits, mem = (int64_t)o.pow.memKiB, cap = o.maxClaimsPerHour;
    if (!num("start", o.params.start, 1, 100000) || !num("goal", o.params.goal, 2, 100000) || !num("chunk", o.params.chunk, 1, 10000)
        || !num("minTicks", o.params.minTicks, 0, 1000000) || !num("maxTicks", o.params.maxTicks, 1, 1000000)
        || !num("bits", bits, 0, 24) || !num("memKiB", mem, 64, 262144) || !num("maxBeaconAgeS", o.maxBeaconAgeS, 60, 86400)
        || !num("maxClaimsPerHour", cap, 0, 1000000))
        return false;
    if (o.params.goal <= o.params.start || o.params.chunksNeeded() > 255) { why = "config: goal/start/chunk give an invalid number of proofs"; return false; }
    o.pow.bits = (int)bits; o.pow.memKiB = (size_t)mem; o.maxClaimsPerHour = (int)cap;
    if (j.contains("audience")) {
        if (!j["audience"].is_string() || j["audience"].get<std::string>().size() > 255) { why = "config.audience must be a string of at most 255 bytes"; return false; }
        o.audience = j["audience"];
    }
    if (j.contains("lezUrl")) {
        if (!j["lezUrl"].is_string() || j["lezUrl"].get<std::string>().empty()) { why = "config.lezUrl must be a URL"; return false; }
        o.lezUrl = j["lezUrl"];
    }
    return true;
}

json verifyClaim(const json& req, const BeaconLookup& lookup, int64_t nowMs) {
    // The request (problems here are the caller's or the operator's: {"error"}).
    if (!req.is_object() || !req.contains("auth_type") || !req["auth_type"].is_string() || !req.contains("domain")
        || !req["domain"].is_string() || !req.contains("action_hex") || !req["action_hex"].is_string()
        || !req.contains("payload_hex") || !req["payload_hex"].is_string())
        return error("verify_auth args: need auth_type, domain, action_hex, payload_hex");
    const std::string authType = req["auth_type"], domain = req["domain"];
    if (authType != kAuthType) return error("unknown auth_type '" + authType + "': this module verifies " + kAuthType);
    if (!validDomain(domain)) return error("domain \"" + domain + "\": expected 1 to 64 bytes of [a-z0-9-]");
    GifterRules rules;
    std::string why;
    if (!GifterRules::fromJson(req.value("config", json()), rules, why)) return error(why);

    // The requester's material (problems here are refusals).
    std::vector<unsigned char> action, payload;
    if (!fromHex(req["action_hex"], action)) return refuse("action hex: not hex");
    if (action.size() != 32) return refuse("action must be 32 bytes, got " + std::to_string(action.size()));
    if (!fromHex(req["payload_hex"], payload)) return refuse("payload hex: not hex");
    Claim c;
    if (!decodeClaim(payload, c, why)) return refuse(why);

    // 1. Shape: the right number of proofs, in an order a game can produce.
    const size_t need = (size_t)rules.params.chunksNeeded();
    if (c.chunks.size() != need) return refuse("the claim has " + std::to_string(c.chunks.size()) + " proofs, the goal needs " + std::to_string(need));
    if ((int64_t)c.end < rules.params.minTicks || (int64_t)c.end > rules.params.maxTicks) return refuse("the game's length is outside the rules");
    for (size_t i = 0; i < c.chunks.size(); i++) {
        if (c.chunks[i].tick >= c.end || (i && c.chunks[i].tick < c.chunks[i - 1].tick)) return refuse("the proofs are not in game order");
    }

    // 2. The signature of the game key over this membership request (cheap).
    unsigned char digest[32];
    bindingDigest(domain, authType, action.data(), rules.audience, encodeClaim(c, false), digest);
    if (crypto_sign_verify_detached(c.sig, digest, 32, c.pub) != 0) return refuse("signature verification failed");

    // 3. The beacon: a real LEZ block, recent, and old enough for the game's length.
    Beacon b;
    bool unreachable = false;
    if (!lookup(c.beaconId, b, why, unreachable)) return unreachable ? error("node unreachable or failing: " + why) : refuse("beacon: " + why);
    const int64_t ageMs = nowMs - b.tsMs;
    if (ageMs > rules.maxBeaconAgeS * 1000) return refuse("the game is too old (its beacon block is " + std::to_string(ageMs / 1000) + " s old)");
    if (ageMs < (int64_t)c.end * 1000 / kTickHz) return refuse("the game claims to be longer than the time since its beacon block");

    // 4. Every proof of work, in a random order so a partly computed claim fails early.
    unsigned char seed[32];
    const std::string seedHex = gameSeed(b, c.pub);
    seedFromHex(seedHex, seed);
    std::vector<size_t> order(c.chunks.size());
    std::iota(order.begin(), order.end(), 0);
    for (size_t i = order.size(); i > 1; i--) std::swap(order[i - 1], order[randombytes_uniform((uint32_t)i)]);
    for (size_t i : order) {
        const ChunkProof& p = c.chunks[i];
        if (!digestOk(seed, (uint32_t)i, p.tick, p.state, p.nonce, rules.pow.bits, rules.pow.memKiB))
            return refuse("proof of work " + std::to_string(i) + " fails");
    }
    return {{"ok", true}, {"identity", hexOf(c.pub, 32)}, {"scheme", "bubble"}, {"nullifier", seedHex}};
}

}
