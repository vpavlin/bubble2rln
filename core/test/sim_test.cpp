// The rules' tests: (1) replay the reference records (vectors.json: made by `sim_test
// vectors.json 0 regen` when the rules last changed, kSimVersion 3) to the same state hashes; (2) a greedy bot plays games, and each record
// replays to the live game's hash; (3) tampering is refused; (4) the proof of work round-trips.
// usage: sim_test vectors.json [games]
#include "sim.h"
#include "check.h"
#include "pow.h"
#include <sodium.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

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
        // A hunter on its tail and close: boost away.
        bool close = false;
        for (const auto& b : g.bots) if (b.hunt > 0 && std::llabs(b.x - g.me.x) + std::llabs(b.y - g.me.y) < radius(b.mass) + r + 150 * kU) close = true;
        return makeInput(dirOf(fx, fy), 0, close);
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

// The lazy way: cross the map corner to corner and back, eating whatever lies on the line.
static int sweep(const Game& g) {
    static int64_t tx = 0, ty = 0;
    if (g.tick == 0) { tx = 0; ty = 0; }
    if (std::llabs(g.me.x - tx) < 200 * kU && std::llabs(g.me.y - ty) < 200 * kU) {
        if (tx == 0 && ty == 0) { tx = kWorld; ty = kWorld; } else if (tx == kWorld && ty == kWorld) { tx = kWorld; ty = 0; }
        else if (tx == kWorld && ty == 0) { tx = 0; ty = kWorld; } else { tx = 0; ty = 0; }
    }
    return dirOf(tx - g.me.x, ty - g.me.y);
}

// The e2e script's bot: the nearest dot, full speed, a new decision every 12 ticks (logosctl lag).
static int dotBot(const Game& g) {
    static int last = kStop;
    if (g.tick % 12 != 0) return last;
    int64_t bx = 0, by = 0, best = INT64_MAX;
    for (const auto& p : g.pellets) { int64_t d = std::llabs(p.x - g.me.x) + std::llabs(p.y - g.me.y); if (d < best) { best = d; bx = p.x; by = p.y; } }
    return last = dirOf(bx - g.me.x, by - g.me.y);
}

static int64_t gGoal = 300;
static void playMany(const char* name, int (*pick)(const Game&), int games) {
    Params p; p.goal = gGoal; if (gGoal < 300) p.minTicks = 300;
    int wins = 0, eaten = 0; int64_t ticks = 0, best = 0;
    for (int i = 0; i < games; i++) {
        unsigned char s[32]; char hex[65];
        crypto_hash_sha256(s, (const unsigned char*)&i, sizeof i);
        sodium_bin2hex(hex, sizeof hex, s, 32);
        Game g(hex, p);
        while (!g.over) {
            int d = pick(g);
            std::vector<int64_t> ds;
            while (g.me.stomach >= p.chunk * (int64_t)(ds.size() + 1) && g.chunks + (int64_t)ds.size() < p.chunksNeeded()) ds.push_back(g.chunks + (int64_t)ds.size());
            int64_t before = g.deaths, mmb = g.mass();
            std::vector<Bot> bb = g.bots;
            g.step(d, ds);
            if (g.deaths > before && getenv("WHO")) {
                // the bot that grew by the most ate us
                size_t k = 0; int64_t gr = -1;
                for (size_t q = 0; q < bb.size(); q++) if (g.bots[q].mass - bb[q].mass > gr) { gr = g.bots[q].mass - bb[q].mass; k = q; }
                printf("eaten t=%lld mass=%lld by bot %zu hunter=%d hunt=%lld mass=%lld\n", (long long)g.tick, (long long)mmb, k, (int)bb[k].hunter, (long long)bb[k].hunt, (long long)bb[k].mass);
            }
        }
        if (g.won) { wins++; ticks += g.tick; }
        eaten += (int)g.deaths; best = std::max(best, g.me.body);
    }
    printf("%s: %d/%d won, mean %.0f s, eaten %.1f per game, best size %lld\n", name, wins, games, wins ? ticks / 30.0 / wins : 0.0, (double)eaten / games, (long long)best);
}

// Writes new reference records (the rules' regression vectors): the greedy bot on fixed seeds.
static int regen(const char* path) {
    json out = json::array();
    for (int i = 0; out.size() < 6; i++) {
        unsigned char s[32]; char hex[65];
        crypto_hash_sha256(s, (const unsigned char*)&i, sizeof i);
        sodium_bin2hex(hex, sizeof hex, s, 32);
        Params p; p.goal = i % 2 ? 200 : 300;
        Game g(hex, p); Record rec; std::string hash500;
        const std::string hash0 = g.stateHash();
        while (!g.over) {
            int d = decide(g);
            std::vector<std::pair<int64_t, uint32_t>> dig; std::vector<int64_t> ds;
            while (g.me.stomach >= p.chunk * (int64_t)(dig.size() + 1) && g.chunks + (int64_t)dig.size() < p.chunksNeeded()) {
                dig.push_back({g.chunks + (int64_t)dig.size(), 0}); ds.push_back(dig.back().first);
            }
            rec.add(g.tick, d, dig); g.step(d, ds);
            if (g.tick == 500) hash500 = g.stateHash();
        }
        rec.end = g.tick;
        if (!g.won) continue;   // a reference record replays to a won game
        out.push_back({{"seed", hex}, {"params", p.toJson()}, {"rec", rec.toJson(hex)}, {"hash", g.stateHash()}, {"deaths", g.deaths},
                       {"ticks", g.tick}, {"hash0", hash0}, {"hash500", hash500}});
    }
    std::ofstream(path) << out.dump() << "\n";
    printf("wrote %s\n", path);
    return 0;
}

int main(int argc, char** argv) {
    if (sodium_init() < 0) return 1;
    if (argc > 3 && std::string(argv[3]) == "regen") return regen(argv[1]);
    // (1) the reference records.
    std::ifstream f(argc > 1 ? argv[1] : "vectors.json");
    std::stringstream ss; ss << f.rdbuf();
    json vec = json::parse(ss.str());
    for (const auto& v : vec) {
        Params p; CHECK(Params::fromJson(v["params"], p), "params");
        std::string seed = v["seed"];
        CHECK(Game(seed, p).stateHash() == v["hash0"], "initial hash %s", seed.substr(0, 8).c_str());
        Record rec; std::string why;
        json rj = v["rec"];   // nonces are 0: the proof of work is checked in (4), not here
        CHECK(Record::fromJson(rj, rec, why), "record: %s", why.c_str());
        json r = replay(seed, p, rec);
        CHECK(r.value("ok", false) && r["hash"] == v["hash"] && r["deaths"] == v["deaths"],
              "replay %s: %s (want %s)", seed.substr(0, 8).c_str(), r.dump().c_str(), v["hash"].get<std::string>().c_str());
    }
    printf("reference records: %zu checked\n", vec.size());

    // (2) the bot.
    const int games = argc > 2 ? atoi(argv[2]) : 20;
    Params p;
    int wins = 0; int64_t ticks = 0;
    auto t0 = std::chrono::steady_clock::now();
    json sample;
    for (int i = 0; i < games; i++) {
        unsigned char s[32]; char hex[65];
        crypto_hash_sha256(s, (const unsigned char*)&i, sizeof i);
        sodium_bin2hex(hex, sizeof hex, s, 32);
        Game g(hex, p); Record rec;
        while (!g.over) {
            int d = decide(g);
            std::vector<std::pair<int64_t, uint32_t>> dig; std::vector<int64_t> ds;
            while (g.me.stomach >= p.chunk * (int64_t)(dig.size() + 1) && g.chunks + (int64_t)dig.size() < p.chunksNeeded()) {
                dig.push_back({g.chunks + (int64_t)dig.size(), 0}); ds.push_back(dig.back().first);
            }
            rec.add(g.tick, d, dig);
            std::string err = g.step(d, ds);
            CHECK(err.empty(), "step: %s", err.c_str());
            if (!err.empty()) break;
        }
        rec.end = g.tick;
        if (g.won) { wins++; ticks += g.tick; }
        json r = replay(hex, p, rec);
        CHECK(r.value("ok", false) == g.won && (!g.won || r["hash"] == g.stateHash()), "bot replay %d: %s", i, r.dump().c_str());
        if (i == 0) sample = rec.toJson(hex);
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("bot: %d/%d won, mean %.0f s of play, %.0f ms CPU per game (play + replay)\n", wins, games, wins ? ticks / 30.0 / wins : 0.0, ms / games);

    playMany("sweeper", sweep, games);
    playMany("greedy", decide, games);
    gGoal = 40; playMany("dot bot, goal 40", dotBot, games); gGoal = 300;

    // (3) tampering, (4) proof of work, on a short game with real nonces at 4 bits.
    {
        Params q; q.goal = 30; q.minTicks = 60;
        PowParams pw; pw.bits = 4;
        std::string seed(64, 'a');
        unsigned char sb[32]; seedFromHex(seed, sb);
        Game g(seed, q); Record rec;
        while (!g.over) {
            int d = decide(g);
            std::vector<std::pair<int64_t, uint32_t>> dig; std::vector<int64_t> ds;
            if (!g.queue.empty()) {
                const Queued& qd = g.queue.front();
                uint32_t n = 0; while (!digestOk(sb, (uint32_t)g.chunks, (uint32_t)qd.tick, qd.hash, n, pw.bits, pw.memKiB)) n++;
                dig.push_back({g.chunks, n}); ds.push_back(g.chunks);
            }
            rec.add(g.tick, d, dig); g.step(d, ds);
        }
        rec.end = g.tick;
        json good = rec.toJson(seed);
        json r = checkGame(seed, q, pw, good);
        CHECK(r.value("ok", false), "honest game refused: %s", r.dump().c_str());
        json bad = good; bad["digests"][1][2] = bad["digests"][1][2].get<uint32_t>() + 1;
        r = checkGame(seed, q, pw, bad);
        CHECK(!r.value("ok", true) && r["reason"].get<std::string>().find("proof of work") != std::string::npos, "bad nonce: %s", r.dump().c_str());
        json bad2 = good; bad2["inputs"][3][1] = (bad2["inputs"][3][1].get<int>() + 32) % 64;
        r = checkGame(seed, q, pw, bad2);
        CHECK(!r.value("ok", true), "changed move accepted");
        CHECK(!checkGame(std::string(64, 'b'), q, pw, good).value("ok", true), "another seed accepted");
        Params easier = q; easier.goal = 20;
        CHECK(!checkGame(seed, easier, pw, good).value("ok", true), "other rules accepted");
        printf("check: honest game ok, tampered nonce/move/seed/rules refused\n");
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
