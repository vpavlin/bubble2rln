// The claim (claim.h): a won game's proofs, signed by the game key for one membership action,
// accepted by the gifter's check; tampered, re-aimed, stale and too-early claims refused.
// usage: claim_test [bits]   (default 6; the payload size and the check time are printed)
#include "claim.h"
#include "pow.h"
#include <sodium.h>
#include <chrono>
#include <cstdio>
#include <cstring>
using namespace bubble;
using nlohmann::json;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
// A greedy bot: chase the best nearby food, run from anything bigger, keep off the walls.
static int decide(const Game& g) {
    const int64_t mm = g.mass(), r = radius(mm);
    int64_t fx = 0, fy = 0; bool threat = false;
    for (const auto& b : g.bots) {
        int64_t dx = b.x - g.me.x, dy = b.y - g.me.y, d = std::llabs(dx) + std::llabs(dy);
        if (b.mass * 10 >= mm * 11 && d < r * 6 + 300 * kU) {
            int64_t w = std::max<int64_t>(1, 1000000 - d) * (1 + b.mass / mm);
            fx -= ((dx > 0) - (dx < 0)) * w; fy -= ((dy > 0) - (dy < 0)) * w; threat = true;
        }
    }
    if (threat) {
        const int64_t m = 400 * kU;
        if (g.me.x < m) fx += (m - g.me.x) * 4000;
        if (kWorld - g.me.x < m) fx -= (m - (kWorld - g.me.x)) * 4000;
        if (g.me.y < m) fy += (m - g.me.y) * 4000;
        if (kWorld - g.me.y < m) fy -= (m - (kWorld - g.me.y)) * 4000;
        return dirOf(fx, fy);
    }
    int64_t bx = 0, by = 0, best = INT64_MIN; bool any = false;
    for (const auto& b : g.bots) {
        if (mm * 10 < b.mass * 11) continue;
        int64_t s = b.mass * 4000 - (std::llabs(b.x - g.me.x) + std::llabs(b.y - g.me.y));
        if (s > best) { best = s; bx = b.x; by = b.y; any = true; }
    }
    for (const auto& p : g.pellets) {
        int64_t s = 4000 - (std::llabs(p.x - g.me.x) + std::llabs(p.y - g.me.y));
        if (s > best) { best = s; bx = p.x; by = p.y; any = true; }
    }
    return any ? dirOf(bx - g.me.x, by - g.me.y) : kStop;
}

static std::string hex(const unsigned char* p, size_t n) { std::string s(n * 2 + 1, 0); sodium_bin2hex(s.data(), s.size(), p, n); s.resize(n * 2); return s; }

int main(int argc, char** argv) {
    if (sodium_init() < 0) return 1;
    const int bits = argc > 1 ? atoi(argv[1]) : 6;
    // A beacon block 20 minutes old, and the game key.
    const int64_t now = 1795300000000;
    Beacon b; b.id = 84846; randombytes_buf(b.hash, 32); b.tsMs = now - 20 * 60 * 1000;
    unsigned char pk[32], sk[64]; crypto_sign_keypair(pk, sk);
    const std::string seedHex = gameSeed(b, pk);
    unsigned char seed[32]; seedFromHex(seedHex, seed);

    // Play a full game with the greedy bot, digesting each queued chunk with a real proof.
    Params p; PowParams pw; pw.bits = bits;
    Game g(seedHex, p);
    std::vector<uint32_t> nonces;
    auto t0 = std::chrono::steady_clock::now();
    while (!g.over) {
        std::vector<int64_t> ds;
        if (!g.queue.empty()) {
            const Queued& q = g.queue.front();
            uint32_t n = 0; while (!digestOk(seed, (uint32_t)g.chunks, (uint32_t)q.tick, q.hash, n, pw.bits, pw.memKiB)) n++;
            nonces.push_back(n); ds.push_back(g.chunks);
        }
        g.step(decide(g), ds);
    }
    const double mineS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(g.won, "the bot lost: %s", g.why.c_str());
    Claim c; c.beaconId = b.id; memcpy(c.pub, pk, 32); c.end = (uint32_t)g.tick;
    for (size_t i = 0; i < g.digested.size(); i++) c.chunks.push_back({(uint32_t)g.digested[i].tick, g.digested[i].hash, nonces[i]});

    unsigned char action[32]; randombytes_buf(action, 32);
    auto sign = [&](Claim& cl, const unsigned char* act, const std::string& aud) {
        unsigned char dg[32]; bindingDigest("rln-membership", kAuthType, act, aud, encodeClaim(cl, false), dg);
        crypto_sign_detached(cl.sig, nullptr, dg, 32, sk);
    };
    sign(c, action, "");
    const auto bytes = encodeClaim(c, true);
    json cfg = {{"bits", bits}, {"maxBeaconAgeS", 3600}};
    auto req = [&](const std::vector<unsigned char>& pl, const unsigned char* act) {
        return json{{"auth_type", kAuthType}, {"domain", "rln-membership"}, {"action_hex", hex(act, 32)},
                    {"payload_hex", hex(pl.data(), pl.size())}, {"config", cfg}};
    };
    BeaconLookup lookup = [&](uint64_t id, Beacon& out, std::string& why, bool& unreachable) {
        unreachable = false;
        if (id != b.id) { why = "block not found"; return false; }
        out = b; return true;
    };

    auto t1 = std::chrono::steady_clock::now();
    json v = verifyClaim(req(bytes, action), lookup, now);
    const double checkMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    CHECK(v.value("ok", false) && v["nullifier"] == seedHex, "honest claim: %s", v.dump().c_str());
    printf("game: %lld ticks (%.0f s), %zu proofs at %d bits mined in %.1f s; payload %zu bytes; gifter check %.0f ms\n",
           (long long)g.tick, g.tick / 30.0, c.chunks.size(), bits, mineS, bytes.size(), checkMs);

    // Refusals.
    auto refused = [&](const json& r, const char* what, const char* needle) {
        const std::string why = r.value("reason", r.value("error", std::string()));
        CHECK(!r.value("ok", false) && why.find(needle) != std::string::npos, "%s: %s", what, r.dump().c_str());
    };
    unsigned char other[32]; randombytes_buf(other, 32);
    refused(verifyClaim(req(bytes, other), lookup, now), "re-aimed at another membership", "signature");
    { Claim t = c; t.chunks[5].nonce ^= 1; sign(t, action, ""); refused(verifyClaim(req(encodeClaim(t, true), action), lookup, now), "wrong nonce (re-signed)", "proof of work"); }
    { Claim t = c; t.chunks[7].state ^= 1; sign(t, action, ""); refused(verifyClaim(req(encodeClaim(t, true), action), lookup, now), "wrong state (re-signed)", "proof of work"); }
    { Claim t = c; t.chunks[3].nonce ^= 1; refused(verifyClaim(req(encodeClaim(t, true), action), lookup, now), "changed after signing", "signature"); }
    { Claim t = c; t.chunks.pop_back(); sign(t, action, ""); refused(verifyClaim(req(encodeClaim(t, true), action), lookup, now), "a proof short", "proofs"); }
    { Claim t = c; t.beaconId++; sign(t, action, ""); refused(verifyClaim(req(encodeClaim(t, true), action), lookup, now), "unknown beacon", "beacon"); }
    refused(verifyClaim(req(bytes, action), lookup, b.tsMs + 2 * 3600 * 1000), "stale beacon", "too old");
    refused(verifyClaim(req(bytes, action), lookup, b.tsMs + 30 * 1000), "claimed sooner than the game could last", "longer than the time");
    { auto t = bytes; t.resize(100); refused(verifyClaim(req(t, action), lookup, now), "truncated", "not a bubble claim"); }
    { json r = req(bytes, action); r["auth_type"] = "peer-any"; refused(verifyClaim(r, lookup, now), "other auth type", "unknown auth_type"); }
    { json r = req(bytes, action); r["config"]["audience"] = "gifter-2"; refused(verifyClaim(r, lookup, now), "other audience", "signature"); }
    { json r = req(bytes, action); r["config"]["goal"] = 300; refused(verifyClaim(r, lookup, now), "other goal", "proofs"); }
    { json r = req(bytes, action); refused(verifyClaim(r, [](uint64_t, Beacon&, std::string& w, bool& u) { w = "timeout"; u = true; return false; }, now), "node down", "unreachable"); }

    // Junk costs little: a claim with no work in it fails at its first proof.
    { Claim t = c; for (auto& x : t.chunks) x.nonce = 0xdeadbeef; sign(t, action, "");
      auto t2 = std::chrono::steady_clock::now();
      json r = verifyClaim(req(encodeClaim(t, true), action), lookup, now);
      printf("junk claim refused in %.1f ms: %s\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count(), r.value("reason", std::string()).c_str()); }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
