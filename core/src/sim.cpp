#include "sim.h"
#include <cstdlib>

using nlohmann::json;

namespace bubble {

namespace {

const int COS[64] = {1024,1019,1004,980,946,903,851,792,724,650,569,483,392,297,200,100,0,-100,-200,-297,-392,-483,-569,-650,-724,-792,-851,-903,-946,-980,-1004,-1019,-1024,-1019,-1004,-980,-946,-903,-851,-792,-724,-650,-569,-483,-392,-297,-200,-100,0,100,200,297,392,483,569,650,724,792,851,903,946,980,1004,1019};
const int SIN[64] = {0,100,200,297,392,483,569,650,724,792,851,903,946,980,1004,1019,1024,1019,1004,980,946,903,851,792,724,650,569,483,392,297,200,100,0,-100,-200,-297,-392,-483,-569,-650,-724,-792,-851,-903,-946,-980,-1004,-1019,-1024,-1019,-1004,-980,-946,-903,-851,-792,-724,-650,-569,-483,-392,-297,-200,-100};

constexpr int64_t kMaxSpeed = 110;      // units per tick
constexpr int64_t kBotMaxSpeed = 80;
constexpr int64_t kRetarget = 12;       // ticks between a bot's decisions
constexpr int64_t kSight = 450 * kU;    // how far a bot looks
constexpr int64_t kShield = 90;         // ticks a respawned player can't be eaten
constexpr int64_t kCell = 128 * kU;     // pellet grid cell
constexpr int64_t kCells = (kWorld + kCell - 1) / kCell + 1;

int64_t sgn(int64_t v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }
int64_t clampW(int64_t v) { return v < 0 ? 0 : (v > kWorld ? kWorld : v); }
int64_t cellOf(int64_t x, int64_t y) { return (y / kCell) * kCells + x / kCell; }

// Bigger is slower. The player is never quicker than kMaxSpeed (a bubble of about 26) and a
// bot never quicker than kBotMaxSpeed, so a small player still outruns what chases it.
int64_t speed(int64_t mass) {
    int64_t v = 2400 / isqrt(mass * 16 + 64);
    return v < 6 ? 6 : (v > kMaxSpeed ? kMaxSpeed : v);
}
int64_t botSpeed(int64_t mass) { return std::min(speed(mass), kBotMaxSpeed); }

void moveAt(int64_t& x, int64_t& y, int dir, int64_t s) {
    if (dir == kStop) return;
    x = clampW(x + COS[dir] * s / 1024);
    y = clampW(y + SIN[dir] * s / 1024);
}




// a eats b when a is 10% bigger and b's centre is inside a.
bool eats(int64_t ax, int64_t ay, int64_t am, int64_t ra, int64_t bx, int64_t by, int64_t bm) {
    if (am * 10 < bm * 11) return false;
    int64_t dx = ax - bx, dy = ay - by;
    if (dx > ra || dx < -ra || dy > ra || dy < -ra) return false;
    return dx * dx + dy * dy < ra * ra;
}

uint32_t rotl(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

uint32_t hexWord(const std::string& h, size_t at) {
    return (uint32_t)std::strtoul(h.substr(at, 8).c_str(), nullptr, 16);
}

}

int64_t isqrt(int64_t n) {
    if (n < 2) return n;
    int64_t x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

int64_t radius(int64_t mass) { return isqrt(mass * 4096); }

int64_t dotQuarters(int64_t mass) { return mass < kDotFull ? 4 : mass < kDotHalf ? 2 : mass < kDotQuarter ? 1 : 0; }

int dirOf(int64_t dx, int64_t dy) {
    int best = 0;
    int64_t bestDot = INT64_MIN;
    for (int i = 0; i < kDirs; i++) {
        int64_t d = COS[i] * dx + SIN[i] * dy;
        if (d > bestDot) { bestDot = d; best = i; }
    }
    return best;
}

uint32_t Rng::next() {
    uint32_t result = rotl(s[1] * 5, 7) * 9;
    uint32_t t = s[1] << 9;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t; s[3] = rotl(s[3], 11);
    return result;
}

json Params::toJson() const {
    return {{"start", start}, {"goal", goal}, {"chunk", chunk}, {"minTicks", minTicks}, {"maxTicks", maxTicks}};
}

bool Params::fromJson(const json& j, Params& o) {
    if (!j.is_object()) return false;
    auto get = [&](const char* k, int64_t& v, int64_t lo, int64_t hi) {
        if (!j.contains(k) || !j[k].is_number_integer()) return false;
        v = j[k].get<int64_t>();
        return v >= lo && v <= hi;
    };
    return get("start", o.start, 1, 1000) && get("goal", o.goal, o.start + 1, 100000) && get("chunk", o.chunk, 1, 1000)
        && get("minTicks", o.minTicks, 0, 1000000) && get("maxTicks", o.maxTicks, 1, 1000000);
}

Game::Game(const std::string& seedHex, const Params& params) : p(params) {
    for (int i = 0; i < 4; i++) rng.s[i] = seedHex.size() >= 32 ? hexWord(seedHex, i * 8) : 0;
    if ((rng.s[0] | rng.s[1] | rng.s[2] | rng.s[3]) == 0) rng.s[0] = 1;
    me = {kWorld / 2, kWorld / 2, p.start, 0, kStop, kShield, 0, 0};
    for (int i = 0; i < kPellets; i++) pellets.push_back(spawnPellet());
    grid_.assign(kCells * kCells, {});
    eatenBy_.assign(kPellets, -1);
    for (int k = 0; k < kPellets; k++) gridAdd(k);
    for (int j = 0; j < kBots; j++) {
        int64_t m = j < 10 ? 4 + rng.below(8) : (j < 20 ? 12 + rng.below(48) : 60 + rng.below(100));
        bots.push_back(spawnBot(m));
        if (j >= kBots - kHunters) bots.back().hunter = true;
    }
}

Pellet Game::spawnPellet() {
    int64_t x = rng.below(kWorld);
    int64_t y = rng.below(kWorld);
    return {x, y};
}

// An eaten bubble comes back far from the player, smaller than half its size; a hunter comes back a hunter, half
// again the player's size, so there is always something to run from.
void Game::respawnBot(size_t i) {
    const bool hunter = bots[i].hunter;
    int64_t m = 4 + rng.below(std::max<int64_t>(40, mass() / 2));   // prey grows with the player
    if (hunter) m = mass() * 3 / 2 + 20 + rng.below(40);
    bots[i] = spawnBot(m);
    bots[i].hunter = hunter;
}

Bot Game::spawnBot(int64_t mass) {
    int64_t x, y;
    int tries = 0;
    do {
        x = rng.below(kWorld); y = rng.below(kWorld); tries++;
    } while (tries < 20 && std::llabs(x - me.x) + std::llabs(y - me.y) < 600 * kU);
    int dir = (int)rng.below(kDirs);
    int64_t wait = rng.below(kRetarget);
    return {x, y, mass, dir, wait};
}

void Game::gridAdd(int j) { grid_[cellOf(pellets[j].x, pellets[j].y)].push_back(j); }

void Game::gridRemove(int j) {
    auto& a = grid_[cellOf(pellets[j].x, pellets[j].y)];
    for (size_t i = 0; i < a.size(); i++) if (a[i] == j) { a[i] = a.back(); a.pop_back(); return; }
}

void Game::markPellets(int64_t x, int64_t y, int64_t r, int who) {
    int64_t c0 = std::max<int64_t>(0, x - r) / kCell, c1 = std::min<int64_t>(kWorld, x + r) / kCell;
    int64_t r0 = std::max<int64_t>(0, y - r) / kCell, r1 = std::min<int64_t>(kWorld, y + r) / kCell;
    for (int64_t row = r0; row <= r1; row++)
        for (int64_t col = c0; col <= c1; col++)
            for (int j : grid_[row * kCells + col]) {
                if (eatenBy_[j] != -1) continue;
                int64_t dx = x - pellets[j].x, dy = y - pellets[j].y;
                if (dx * dx + dy * dy < r * r) eatenBy_[j] = who;
            }
}

void Game::botThink(Bot& b, size_t idx) {
    if (b.hunter) {
        const int64_t mm = mass(), dx = me.x - b.x, dy = me.y - b.y;
        const bool canEat = b.mass * 10 >= mm * 11 && me.shield == 0 && mm >= kHuntFrom;   // small ones are left to graze
        if (b.hunt < 0) b.hunt++;
        else if (b.hunt > 0) {
            if (!canEat) b.hunt = -kRestTicks;
            else if (--b.hunt == 0) b.hunt = -kRestTicks;
            else {
                // Aim where the player will be (up to kLeadTicks ahead on its heading): a straight
                // line runs into a hunter, a turn shakes it off.
                if (b.hunt % 3 == 0) {
                    const int pdir = me.input % kDirs1;
                    int64_t lx = 0, ly = 0;
                    if (pdir != kStop) {
                        const int64_t ps = speed(mm) * 115 / 100, hs = std::max<int64_t>(1, ps * kHuntSpeed / 100);
                        const int64_t t = std::min<int64_t>(kLeadTicks, (std::llabs(dx) + std::llabs(dy)) / hs);
                        lx = COS[pdir] * ps / 1024 * t; ly = SIN[pdir] * ps / 1024 * t;
                    }
                    b.dir = dirOf(dx + lx, dy + ly);
                }
                return;
            }
        } else if (canEat && std::llabs(dx) < kHuntSight && std::llabs(dy) < kHuntSight) {
            b.hunt = kHuntTicks; b.dir = dirOf(dx, dy); return;
        }
    }
    if (b.wait > 0) { b.wait--; return; }
    b.wait = kRetarget;
    int64_t mm = mass();
    int64_t fx = 0, fy = 0, tx = 0, ty = 0, best = -1;
    bool threat = false;
    auto consider = [&](int64_t cx, int64_t cy, int64_t cm) {
        int64_t dx = cx - b.x, dy = cy - b.y;
        if (std::llabs(dx) > kSight || std::llabs(dy) > kSight) return;
        if (cm * 10 >= b.mass * 11) {
            fx -= sgn(dx) * (kSight - std::llabs(dx)); fy -= sgn(dy) * (kSight - std::llabs(dy)); threat = true;
        } else if (b.mass * 10 >= cm * 11) {
            int64_t score = cm * 1000 - (std::llabs(dx) + std::llabs(dy)) / kU;
            if (score > best) { best = score; tx = dx; ty = dy; }
        }
    };
    consider(me.x, me.y, mm);
    for (size_t i = 0; i < bots.size(); i++) if (i != idx) consider(bots[i].x, bots[i].y, bots[i].mass);
    b.fleeing = threat;
    if (threat) b.dir = dirOf(fx, fy);
    else if (best >= 0 && rng.below(4) != 0) b.dir = dirOf(tx, ty);
    else if (rng.below(3) == 0) b.dir = (int)rng.below(kDirs);
}

std::string Game::step(int input, const std::vector<int64_t>& digests) {
    if (over) return "the game is over";
    if (input != -1) {
        if (input < 0 || input > kInputMax) return "bad input at tick " + std::to_string(tick);
        me.input = input;
    }
    for (int64_t c : digests) {
        if (c != chunks) return "chunk " + std::to_string(c) + " out of order at tick " + std::to_string(tick);
        if (chunks >= p.chunksNeeded()) return "chunk " + std::to_string(c) + " beyond the goal at tick " + std::to_string(tick);
        if (me.stomach < p.chunk || queue.empty()) return "chunk " + std::to_string(c) + " with nothing to digest at tick " + std::to_string(tick);
        digested.push_back(queue.front());
        queue.erase(queue.begin());
        me.stomach -= p.chunk; me.body += p.chunk; chunks++;
    }
    {
        // The player is 15% quicker than a bot its size; slower with a full stomach, slower still
        // at a lower throttle; a boost burns food for a dash.
        const int dir = me.input % kDirs1, throttle = (me.input % kBoostBit) / kDirs1;   // throttle: 0 full .. 3 a quarter
        if (me.boost > 0) { if (--me.boost == 0) me.boost = -kBoostRest; }
        else if (me.boost < 0) me.boost++;
        if (me.boost == 0 && me.input >= kBoostBit && me.stomach >= kBoostMin && dir != kStop) {
            me.stomach -= std::max(kBoostMin, me.stomach / 10);
            while (!queue.empty() && me.stomach < p.chunk * (int64_t)queue.size()) queue.pop_back();
            me.boost = kBoostTicks;
        }
        const int64_t mm = mass();
        int64_t s = speed(mm) * 115 / 100;
        s = s * (100 - std::min(kFullSlowMax, me.stomach * kFullSlow / mm)) / 100;
        s = s * (4 - throttle) / 4;
        if (me.boost > 0) s = speed(mm) * 115 / 100 * kBoostSpeed / 100;
        moveAt(me.x, me.y, dir, s);
    }
    const int64_t huntSpeed = speed(mass()) * 115 / 100 * kHuntSpeed / 100;   // of the player's top speed
    const int64_t fleeSpeed = speed(mass()) * 115 / 100 * kFleeSpeed / 100;   // prey: a chaser catches up, a passer-by doesn't
    for (size_t i = 0; i < bots.size(); i++) {
        Bot& b = bots[i];
        botThink(b, i);
        if (b.hunt > 0) moveAt(b.x, b.y, b.dir, std::max(huntSpeed, botSpeed(b.mass)));
        else moveAt(b.x, b.y, b.dir, b.fleeing ? std::min(speed(b.mass), fleeSpeed) : botSpeed(b.mass));
    }

    // Pellets: each goes to the first mover (the player, then the bots in order) whose circle
    // holds it at the start of the pass; then they respawn in pellet order.
    markPellets(me.x, me.y, radius(mass()), 0);
    for (size_t q = 0; q < bots.size(); q++) markPellets(bots[q].x, bots[q].y, radius(bots[q].mass), (int)q + 1);
    for (int j = 0; j < (int)pellets.size(); j++) {
        int who = eatenBy_[j];
        if (who == -1) continue;
        eatenBy_[j] = -1;
        if (who == 0) { me.crumbs += dotQuarters(mass()); me.stomach += me.crumbs / 4; me.crumbs %= 4; } else bots[who - 1].mass += 1;
        gridRemove(j); pellets[j] = spawnPellet(); gridAdd(j);
    }
    // Bubbles.
    for (size_t a = 0; a < bots.size(); a++) {
        int64_t mm = mass();
        if (eats(me.x, me.y, mm, radius(mm), bots[a].x, bots[a].y, bots[a].mass)) {
            me.stomach += bots[a].mass;
            respawnBot(a);
            continue;
        }
        Bot& ba = bots[a];
        int64_t ra = radius(ba.mass);
        if (me.shield == 0 && eats(ba.x, ba.y, ba.mass, ra, me.x, me.y, mass())) {
            // Eaten: the bot gets what was still undigested; the digested body is safe.
            ba.mass += me.stomach; ra = radius(ba.mass);
            me.stomach = 0; me.crumbs = 0; deaths++; me.shield = kShield;
            queue.clear();   // the proofs being searched for are lost with the food
            me.x = rng.below(kWorld); me.y = rng.below(kWorld);
        }
        for (size_t c = 0; c < bots.size(); c++) {
            if (c == a) continue;
            if (eats(bots[a].x, bots[a].y, bots[a].mass, ra, bots[c].x, bots[c].y, bots[c].mass)) {
                bots[a].mass += bots[c].mass; ra = radius(bots[a].mass);
                respawnBot(c);
            }
        }
        // Huge bots slowly shrink, so the world never locks up.
        if (bots[a].mass > 600 && (tick + (int64_t)a) % 30 == 0) bots[a].mass -= 1;
    }
    if (me.shield > 0) me.shield--;
    // Hunger (past kDotFull; small ones graze in peace): what isn't a whole chunk yet slowly goes.
    if (tick % kHungerTicks == kHungerTicks - 1 && mass() >= kDotFull && me.stomach > p.chunk * (int64_t)queue.size()) me.stomach--;
    tick++;
    // Whole chunks in the stomach join the digestion queue, each bound to this tick and state.
    if (me.stomach >= p.chunk * (int64_t)(queue.size() + 1) && chunks + (int64_t)queue.size() < p.chunksNeeded()) {
        const uint32_t h = stateHash32();
        while (me.stomach >= p.chunk * (int64_t)(queue.size() + 1) && chunks + (int64_t)queue.size() < p.chunksNeeded())
            queue.push_back({tick, h});
    }
    if (!over && me.body >= p.goal && tick >= p.minTicks) { over = true; won = true; why = "goal"; }
    if (!over && tick >= p.maxTicks) { over = true; why = "time"; }
    return "";
}

std::string Game::stateHash() const {
    char buf[9];
    snprintf(buf, sizeof buf, "%08x", stateHash32());
    return buf;
}

uint32_t Game::stateHash32() const {
    uint32_t h = 0x811c9dc5u;
    auto mix = [&](int64_t v) { h = (h ^ (uint32_t)v) * 16777619u; };
    mix(tick); mix(deaths); mix(me.shield); mix(me.x); mix(me.y); mix(me.body); mix(me.stomach); mix(chunks); mix(me.crumbs); mix(me.input); mix(me.boost);
    for (int i = 0; i < 4; i++) mix(rng.s[i]);
    for (const auto& b : bots) { mix(b.x); mix(b.y); mix(b.mass); mix(b.dir); mix(b.hunt); mix(b.fleeing); }
    for (const auto& pl : pellets) { mix(pl.x); mix(pl.y); }
    return h;
}

void Record::add(int64_t tick, int dir, const std::vector<std::pair<int64_t, uint32_t>>& digested) {
    if (dir != lastDir) { inputs.emplace_back(tick, dir); lastDir = dir; }
    for (const auto& [c, n] : digested) digests.emplace_back(tick, c, n);
}

json Record::toJson(const std::string& seedHex) const {
    json in = json::array(), dg = json::array();
    for (const auto& [t, d] : inputs) in.push_back({t, d});
    for (const auto& [t, c, n] : digests) dg.push_back({t, c, n});
    return {{"v", kSimVersion}, {"seed", seedHex}, {"inputs", in}, {"digests", dg}, {"end", end}};
}

bool Record::fromJson(const json& j, Record& o, std::string& why) {
    why = "bad record";
    if (!j.is_object() || !j.contains("v") || j["v"] != kSimVersion) { why = "this game was played with other rules (record version " + (j.is_object() && j.contains("v") ? j["v"].dump() : std::string("?")) + ", this core plays " + std::to_string(kSimVersion) + "); update Bubble"; return false; }
    if (!j.contains("end") || !j["end"].is_number_integer()) return false;
    o.end = j["end"].get<int64_t>();
    const json& in = j.value("inputs", json::array());
    const json& dg = j.value("digests", json::array());
    if (!in.is_array() || !dg.is_array() || in.size() > 1000000 || dg.size() > 100000) return false;
    for (const auto& e : in) {
        if (!e.is_array() || e.size() != 2 || !e[0].is_number_integer() || !e[1].is_number_integer()) return false;
        o.inputs.emplace_back(e[0].get<int64_t>(), (int)e[1].get<int64_t>());
    }
    for (const auto& e : dg) {
        if (!e.is_array() || e.size() != 3 || !e[0].is_number_integer() || !e[1].is_number_integer() || !e[2].is_number_integer()) return false;
        const int64_t n = e[2].get<int64_t>();
        if (n < 0 || n > 0xffffffffll) return false;
        o.digests.emplace_back(e[0].get<int64_t>(), e[1].get<int64_t>(), (uint32_t)n);
    }
    return true;
}

json replay(const std::string& seedHex, const Params& p, const Record& rec, std::vector<Queued>* digested) {
    if (rec.end < 1 || rec.end > p.maxTicks) return {{"ok", false}, {"reason", "bad end tick"}};
    Game g(seedHex, p);
    size_t ii = 0, di = 0;
    std::vector<int64_t> ds;
    while (g.tick < rec.end && !g.over) {
        int input = -1;
        if (ii < rec.inputs.size() && rec.inputs[ii].first == g.tick) { input = rec.inputs[ii].second; ii++; }
        ds.clear();
        while (di < rec.digests.size() && std::get<0>(rec.digests[di]) == g.tick) { ds.push_back(std::get<1>(rec.digests[di])); di++; }
        std::string err = g.step(input, ds);
        if (!err.empty()) return {{"ok", false}, {"reason", err}};
    }
    if (ii != rec.inputs.size() || di != rec.digests.size()) return {{"ok", false}, {"reason", "entries after the end"}};
    if (!g.won) return {{"ok", false}, {"reason", g.over ? ("lost: " + g.why) : "goal not reached"}, {"ticks", g.tick}, {"body", g.me.body}};
    if (digested) *digested = g.digested;
    return {{"ok", true}, {"reason", ""}, {"ticks", g.tick}, {"body", g.me.body}, {"chunks", g.chunks}, {"deaths", g.deaths}, {"hash", g.stateHash()}};
}

}
