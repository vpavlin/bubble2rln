#pragma once
// The game rules. The core runs them live, and the check (in the core before a claim, and in
// the membership allocator's verifier) replays a record with the same code.
//
// Deterministic by construction: integers only, its own PRNG (xoshiro128**) seeded from the
// game seed, a fixed 30 Hz tick, and a fixed update order. Positions are in 1/16 px.

#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace bubble {

constexpr int kSimVersion = 2;
constexpr int kDirs = 64;
constexpr int kStop = 64;              // input: stand still
constexpr int64_t kU = 16;             // units per px
constexpr int64_t kWorld = 2400 * kU;  // square world side
constexpr int kPellets = 320;
constexpr int kBots = 24;
constexpr int kTickHz = 30;

struct Params {
    int64_t start = 10, goal = 200, chunk = 5, minTicks = 1800, maxTicks = 36000;
    int64_t chunksNeeded() const { return (goal - start + chunk - 1) / chunk; }
    nlohmann::json toJson() const;
    static bool fromJson(const nlohmann::json& j, Params& out);
};

struct Me { int64_t x, y, body, stomach; int dir; int64_t shield; };
struct Bot { int64_t x, y, mass; int dir; int64_t wait; };
struct Pellet { int64_t x, y; };

struct Rng {
    uint32_t s[4];
    uint32_t next();
    int64_t below(int64_t n) { return (int64_t)(next() % (uint32_t)n); }
};

// A chunk waiting in the stomach for its proof of work. Its proof is bound to the tick it was
// swallowed at and the game state then, so no proof can be searched for before the food is eaten.
struct Queued { int64_t tick; uint32_t hash; };

class Game {
public:
    // seedHex: at least 32 hex digits; the first 32 seed the PRNG.
    Game(const std::string& seedHex, const Params& p);

    // One tick. input: 0..63 heading, 64 stop, -1 keep the last one. digests: the chunk numbers
    // whose proof of work arrives at this tick, in order. Returns "" or why the step is invalid.
    std::string step(int input, const std::vector<int64_t>& digests);

    std::string stateHash() const;
    uint32_t stateHash32() const;
    int64_t mass() const { return me.body + me.stomach; }

    Params p;
    Rng rng;
    int64_t tick = 0, chunks = 0, deaths = 0;
    Me me;
    std::vector<Bot> bots;
    std::vector<Pellet> pellets;
    std::vector<Queued> queue;      // the stomach's whole chunks, in order: chunk = chunks + i
    std::vector<Queued> digested;   // what each digested chunk was queued as, in chunk order
    bool over = false, won = false;
    std::string why;

private:
    std::vector<std::vector<int>> grid_;   // pellet indices per cell (a speed-up only)
    std::vector<int> eatenBy_;
    Pellet spawnPellet();
    Bot spawnBot(int64_t mass);
    void gridAdd(int j);
    void gridRemove(int j);
    void markPellets(int64_t x, int64_t y, int64_t r, int who);
    void botThink(Bot& b, size_t idx);
};

int64_t isqrt(int64_t n);
int64_t radius(int64_t mass);
int dirOf(int64_t dx, int64_t dy);

// The record of a game: the heading changes and the digestions, tick by tick.
struct Record {
    std::vector<std::pair<int64_t, int>> inputs;                    // (tick, dir), changes only
    std::vector<std::tuple<int64_t, int64_t, uint32_t>> digests;    // (tick, chunk, nonce)
    int64_t end = 0;
    int lastDir = -2;
    void add(int64_t tick, int dir, const std::vector<std::pair<int64_t, uint32_t>>& digested);
    nlohmann::json toJson(const std::string& seedHex) const;
    static bool fromJson(const nlohmann::json& j, Record& out, std::string& why);
};

// Replays a record: {"ok", "reason", "ticks", "body", "chunks", "deaths", "hash"}. Moves only;
// the proof-of-work nonces are checked by checkGame (check.h).
// digested (if given) gets what each digested chunk was queued as, for the proof-of-work check.
nlohmann::json replay(const std::string& seedHex, const Params& p, const Record& rec, std::vector<Queued>* digested = nullptr);

}
