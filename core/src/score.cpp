#include "score.h"
#include "check.h"
#include "pow.h"

#include <sodium.h>
#include <cstring>

using nlohmann::json;

namespace bubble {

Params scoreParams() { return Params::endlessDefaults(); }
PowParams scorePow() { return PowParams(); }

namespace {

void putVar(std::vector<unsigned char>& b, uint64_t v) {
    while (v >= 0x80) { b.push_back((unsigned char)(v | 0x80)); v >>= 7; }
    b.push_back((unsigned char)v);
}
bool getVar(const std::vector<unsigned char>& b, size_t& i, uint64_t& v) {
    v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (i >= b.size()) return false;
        const unsigned char c = b[i++];
        v |= (uint64_t)(c & 0x7f) << shift;
        if (!(c & 0x80)) return true;
    }
    return false;
}
uint64_t zig(int64_t v) { return ((uint64_t)v << 1) ^ (uint64_t)(v >> 63); }
int64_t unzig(uint64_t v) { return (int64_t)(v >> 1) ^ -(int64_t)(v & 1); }

std::string hexOf(const unsigned char* p, size_t n) {
    std::string s(n * 2 + 1, '\0');
    sodium_bin2hex(s.data(), s.size(), p, n);
    s.resize(n * 2);
    return s;
}
bool unhex(const std::string& h, unsigned char* out, size_t n) {
    size_t len = 0;
    return h.size() == n * 2 && sodium_hex2bin(out, n, h.c_str(), h.size(), nullptr, &len, nullptr) == 0 && len == n;
}

void sha256(const std::vector<unsigned char>& b, unsigned char out[32]) { crypto_hash_sha256(out, b.data(), b.size()); }

// What the game key signs: the block, the key, the result and the record.
// Version 2 adds the name at the end (and its own domain, so a v1 signature can't pass as v2).
void scoreDigest(int version, const Beacon& bc, const unsigned char pub[32], int64_t score, int64_t ticks, const std::string& ended,
                 int64_t whenMs, const unsigned char recHash[32], const std::string& name, unsigned char out[32]) {
    std::vector<unsigned char> m;
    const std::string dom = version == 1 ? "bubble-score/v1" : "bubble-score/v2";
    m.insert(m.end(), dom.begin(), dom.end());
    for (int i = 0; i < 8; i++) m.push_back((unsigned char)(bc.id >> (8 * i)));
    m.insert(m.end(), bc.hash, bc.hash + 32);
    for (int i = 0; i < 8; i++) m.push_back((unsigned char)((uint64_t)bc.tsMs >> (8 * i)));
    m.insert(m.end(), pub, pub + 32);
    for (int64_t v : {score, ticks, whenMs}) for (int i = 0; i < 8; i++) m.push_back((unsigned char)((uint64_t)v >> (8 * i)));
    putVar(m, ended.size());
    m.insert(m.end(), ended.begin(), ended.end());
    m.insert(m.end(), recHash, recHash + 32);
    if (version >= 2) { putVar(m, name.size()); m.insert(m.end(), name.begin(), name.end()); }
    sha256(m, out);
}

}  // namespace

std::string cleanName(const std::string& raw) {
    // Decode UTF-8, keep printable code points (no C0/C1 controls, no bidi overrides), squash spaces.
    std::vector<uint32_t> cps;
    size_t i = 0;
    while (i < raw.size()) {
        const unsigned char c = raw[i];
        uint32_t cp; int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { i++; continue; }
        if (i + len > raw.size()) break;
        bool ok = true;
        for (int k = 1; k < len; k++) { const unsigned char cc = raw[i + k]; if ((cc & 0xC0) != 0x80) { ok = false; break; } cp = (cp << 6) | (cc & 0x3F); }
        if (!ok) { i++; continue; }
        i += len;
        static const uint32_t minFor[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < minFor[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) continue;   // overlong / surrogate
        if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) cp = ' ';
        if ((cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0xFEFF) continue;
        if (cp == ' ' && (cps.empty() || cps.back() == ' ')) continue;
        cps.push_back(cp);
    }
    while (!cps.empty() && cps.back() == ' ') cps.pop_back();
    if (cps.size() > kNameMaxChars) { cps.resize(kNameMaxChars); while (!cps.empty() && cps.back() == ' ') cps.pop_back(); }
    std::string out;
    for (uint32_t cp : cps) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    return out;
}

std::vector<unsigned char> encodeRecordBin(const Record& r) {
    std::vector<unsigned char> b = {'B', 'R', 1};
    putVar(b, (uint64_t)r.end);
    putVar(b, r.inputs.size());
    int64_t t = 0;
    for (const auto& [tick, dir] : r.inputs) { putVar(b, (uint64_t)(tick - t)); putVar(b, zig(dir)); t = tick; }
    putVar(b, r.digests.size());
    t = 0;
    int64_t chunk = -1;
    for (const auto& [tick, c, n] : r.digests) {
        putVar(b, (uint64_t)(tick - t)); putVar(b, zig(c - (chunk + 1)));
        for (int i = 0; i < 4; i++) b.push_back((unsigned char)(n >> (8 * i)));
        t = tick; chunk = c;
    }
    return b;
}

bool decodeRecordBin(const std::vector<unsigned char>& b, Record& out, std::string& why) {
    why = "bad record encoding";
    out = Record();
    if (b.size() < 3 || b[0] != 'B' || b[1] != 'R' || b[2] != 1) return false;
    size_t i = 3;
    uint64_t end, n, v, w;
    if (!getVar(b, i, end) || end > 10000000) return false;
    out.end = (int64_t)end;
    if (!getVar(b, i, n) || n > 1000000) return false;
    int64_t t = 0;
    for (uint64_t k = 0; k < n; k++) {
        if (!getVar(b, i, v) || !getVar(b, i, w) || v > 10000000) return false;
        t += (int64_t)v;
        out.inputs.emplace_back(t, (int)unzig(w));
    }
    if (!getVar(b, i, n) || n > 100000) return false;
    t = 0;
    int64_t chunk = -1;
    for (uint64_t k = 0; k < n; k++) {
        if (!getVar(b, i, v) || !getVar(b, i, w) || v > 10000000 || i + 4 > b.size()) return false;
        t += (int64_t)v;
        chunk = chunk + 1 + unzig(w);
        uint32_t nonce = 0;
        for (int j = 0; j < 4; j++) nonce |= (uint32_t)b[i++] << (8 * j);
        out.digests.emplace_back(t, chunk, nonce);
    }
    if (i != b.size()) return false;
    why.clear();
    return true;
}

std::string toBase64(const std::vector<unsigned char>& b) {
    std::string s(sodium_base64_encoded_len(b.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
    sodium_bin2base64(s.data(), s.size(), b.data(), b.size(), sodium_base64_VARIANT_ORIGINAL);
    s.resize(std::strlen(s.c_str()));
    return s;
}

bool fromBase64(const std::string& s, std::vector<unsigned char>& out) {
    out.assign(s.size(), 0);
    size_t len = 0;
    if (sodium_base642bin(out.data(), out.size(), s.c_str(), s.size(), nullptr, &len, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0) return false;
    out.resize(len);
    return true;
}

json makeScore(const Beacon& beacon, const unsigned char pk[32], const unsigned char sk[64],
               const Record& rec, int64_t score, int64_t ticks, const std::string& ended, int64_t whenMs,
               const std::string& rawName) {
    const std::string name = cleanName(rawName);
    const std::vector<unsigned char> bin = encodeRecordBin(rec);
    unsigned char rh[32], dg[32], sig[64];
    sha256(bin, rh);
    scoreDigest(kScoreMsgVersion, beacon, pk, score, ticks, ended, whenMs, rh, name, dg);
    crypto_sign_detached(sig, nullptr, dg, 32, sk);
    return {{"t", "bubble-score"}, {"v", kScoreMsgVersion}, {"rules", kSimVersion},
            {"beacon", {{"id", beacon.id}, {"hash", hexOf(beacon.hash, 32)}, {"ts", beacon.tsMs}}},
            {"pub", hexOf(pk, 32)}, {"name", name}, {"score", score}, {"ticks", ticks}, {"ended", ended}, {"when", whenMs},
            {"rec", toBase64(bin)}, {"sig", hexOf(sig, 64)}};
}

json verifyScore(const json& m, const BeaconLookup& lookup, const PowParams& pow) {
    auto refuse = [](const std::string& why) { return json{{"ok", false}, {"reason", why}}; };
    if (!m.is_object() || m.value("t", std::string()) != "bubble-score") return refuse("not a score message");
    const int version = m.contains("v") && m["v"].is_number_integer() ? m["v"].get<int>() : 0;
    if (version != 1 && version != 2) return refuse("not a score message (version " + std::to_string(version) + ")");
    std::string name;
    if (version >= 2) {
        if (!m.contains("name") || !m["name"].is_string()) return refuse("bad name");
        name = m["name"].get<std::string>();
        if (cleanName(name) != name) return refuse("bad name");
    }
    if (m.value("rules", 0) != kSimVersion) return refuse("played with other rules (" + std::to_string(m.value("rules", 0)) + ")");
    const json& bj = m.value("beacon", json());
    if (!bj.is_object() || !bj.contains("id") || !bj["id"].is_number_unsigned() || !bj.contains("ts") || !bj["ts"].is_number_integer())
        return refuse("bad block");
    for (const char* k : {"score", "ticks", "when"}) if (!m.contains(k) || !m[k].is_number_integer()) return refuse(std::string("bad ") + k);
    Beacon claimed;
    claimed.id = bj["id"].get<uint64_t>();
    claimed.tsMs = bj["ts"].get<int64_t>();
    unsigned char pub[32], sig[64];
    if (!unhex(bj.value("hash", std::string()), claimed.hash, 32)) return refuse("bad block hash");
    if (!unhex(m.value("pub", std::string()), pub, 32)) return refuse("bad key");
    if (!unhex(m.value("sig", std::string()), sig, 64)) return refuse("bad signature");
    const int64_t score = m["score"].get<int64_t>(), ticks = m["ticks"].get<int64_t>(), when = m["when"].get<int64_t>();
    const std::string ended = m.value("ended", std::string());
    if (ended.size() > 16) return refuse("bad ending");
    std::vector<unsigned char> bin;
    if (!m.contains("rec") || !m["rec"].is_string() || m["rec"].get_ref<const std::string&>().size() > kScoreMaxBytes || !fromBase64(m["rec"], bin))
        return refuse("bad record");
    unsigned char rh[32], dg[32];
    sha256(bin, rh);
    scoreDigest(version, claimed, pub, score, ticks, ended, when, rh, name, dg);
    if (crypto_sign_verify_detached(sig, dg, 32, pub) != 0) return refuse("signature verification failed");
    // The block must be the chain's, so the seed can't be chosen.
    Beacon chain;
    std::string why;
    bool unreachable = false;
    if (!lookup(claimed.id, chain, why, unreachable)) return json{{"ok", false}, {"reason", "block: " + why}, {"retry", unreachable}};
    if (std::memcmp(chain.hash, claimed.hash, 32) != 0 || chain.tsMs != claimed.tsMs) return refuse("the block isn't the chain's");
    Record rec;
    if (!decodeRecordBin(bin, rec, why)) return refuse(why);
    const std::string seed = gameSeed(chain, pub);
    json r;
    try { r = checkGame(seed, scoreParams(), pow, rec.toJson(seed)); }
    catch (const std::exception& e) { return refuse(std::string("check failed: ") + e.what()); }
    if (!r.value("ok", false)) return refuse("the game doesn't check: " + r.value("reason", std::string()));
    if (r.value("score", -1) != score || r.value("ticks", -1) != ticks || r.value("ended", std::string()) != ended)
        return refuse("the record doesn't end at that score");
    unsigned char idh[32];
    std::vector<unsigned char> idIn(pub, pub + 32);
    idIn.insert(idIn.end(), rh, rh + 32);
    sha256(idIn, idh);
    return {{"ok", true}, {"reason", ""}, {"score", score}, {"ticks", ticks}, {"ended", ended}, {"when", when}, {"name", name},
            {"id", hexOf(idh, 8)}, {"block", claimed.id}};
}

}
