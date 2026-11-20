#include "bubble_core_impl.h"
#include "check.h"
#include "pow.h"
#include "sim.h"

#include <sodium.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using nlohmann::json;
using namespace bubble;

namespace {

// The rules of a game. With the allocator, its config decides them.
// BUBBLE_GOAL / BUBBLE_MIN_TICKS / BUBBLE_POW_BITS shorten a test game.
Params defaultParams() {
    Params p;
    if (const char* e = std::getenv("BUBBLE_GOAL")) p.goal = std::max(20, std::atoi(e));
    if (const char* e = std::getenv("BUBBLE_MIN_TICKS")) p.minTicks = std::max(0, std::atoi(e));
    return p;
}

PowParams defaultPow() {
    PowParams w;
    if (const char* e = std::getenv("BUBBLE_POW_BITS")) w.bits = std::clamp(std::atoi(e), 0, 24);
    return w;
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string hex(const unsigned char* p, size_t n) {
    std::string s(n * 2 + 1, '\0');
    sodium_bin2hex(s.data(), s.size(), p, n);
    s.resize(n * 2);
    return s;
}

// No frame for this long = the view is away (hidden, closed): the game pauses, so nothing moves
// unseen. BUBBLE_PAUSE_MS raises it for a slow test driver (logosctl calls).
int64_t pauseAfterMs() {
    static const int64_t v = std::getenv("BUBBLE_PAUSE_MS") ? std::max(100, std::atoi(std::getenv("BUBBLE_PAUSE_MS"))) : 400;
    return v;
}
constexpr double kTickMs = 1000.0 / kTickHz;

struct Live {
    std::string id, seedHex;
    unsigned char seed[32];
    Params params;
    PowParams pow;
    std::unique_ptr<Game> game;
    Record rec;
    int64_t startedMs = 0;

    std::mutex m;                       // guards everything below and game/rec
    int dir = kStop;
    int64_t lastFrameMs = 0;
    std::map<uint32_t, uint32_t> found;
    std::string checkState;
    json checkResult;

    std::atomic<bool> stop{false};
    std::atomic<uint32_t> next{0};
    std::atomic<uint64_t> hashes{0};
    std::vector<std::thread> workers;
    std::thread loop;

    void digest() {
        const uint32_t chunks = (uint32_t)params.chunksNeeded();
        for (uint32_t c; !stop && (c = next++) < chunks;) {
            for (uint32_t n = 0; !stop; n++) {
                hashes++;
                if (digestOk(seed, c, n, pow.bits, pow.memKiB)) {
                    std::lock_guard<std::mutex> l(m);
                    found[c] = n;
                    break;
                }
            }
        }
    }

    // One tick, under the lock.
    void tickLocked() {
        Game& g = *game;
        std::vector<std::pair<int64_t, uint32_t>> dig;
        std::vector<int64_t> ds;
        for (int64_t k = g.chunks; k < params.chunksNeeded() && g.me.stomach >= params.chunk * (int64_t)(dig.size() + 1); k++) {
            auto it = found.find((uint32_t)k);
            if (it == found.end()) break;
            dig.push_back({k, it->second});
            ds.push_back(k);
        }
        rec.add(g.tick, dir, dig);
        std::string err = g.step(dir, ds);
        if (!err.empty()) { g.over = true; g.why = "error: " + err; }
        if (g.over) rec.end = g.tick;
    }

    void run() {
        double due = 0;
        int64_t last = nowMs();
        while (!stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            int64_t now = nowMs();
            bool won = false, over = false;
            {
                std::lock_guard<std::mutex> l(m);
                if (now - lastFrameMs > pauseAfterMs()) { last = now; due = 0; continue; }
                due += (double)(now - last);
                last = now;
                for (int i = 0; i < 3 && due >= kTickMs && !game->over; i++) { tickLocked(); due -= kTickMs; }
                if (due > kTickMs * 3) due = 0;   // fell far behind: don't sprint
                over = game->over;
                won = game->won;
                if (won) checkState = "running";
            }
            if (!over) continue;
            if (won) {
                // The same check the allocator will make, on the recorded game.
                json recJson;
                { std::lock_guard<std::mutex> l(m); recJson = rec.toJson(seedHex); }
                json r;
                try { r = checkGame(seedHex, params, pow, recJson); }
                catch (const std::exception& e) { r = {{"ok", false}, {"reason", std::string("check failed: ") + e.what()}}; }
                std::lock_guard<std::mutex> l(m);
                checkState = "done";
                checkResult = r;
            }
            return;
        }
    }

    void shutdown() {
        stop = true;
        for (auto& t : workers) if (t.joinable()) t.join();
        if (loop.joinable()) loop.join();
    }
};

}

struct BubbleCorePrivate {
    std::shared_ptr<Live> live;
    unsigned threads = 1;
};

BubbleCoreImpl::BubbleCoreImpl() : d(new BubbleCorePrivate) {
    if (sodium_init() < 0) std::abort();
    unsigned hw = std::thread::hardware_concurrency();
    d->threads = hw > 2 ? hw - 2 : 1;   // leave room for Basecamp and the game loop
}

BubbleCoreImpl::~BubbleCoreImpl() {
    if (d->live) d->live->shutdown();
    delete d;
}

std::string BubbleCoreImpl::snapshot() {
    json j = {{"ok", true}, {"params", defaultParams().toJson()}, {"pow", defaultPow().toJson()}, {"threads", d->threads}};
    if (d->live) j["gameId"] = d->live->id;
    return j.dump();
}

std::string BubbleCoreImpl::newGame(const std::string& optionsJson) {
    (void)optionsJson;
    if (d->live) { d->live->shutdown(); d->live.reset(); }
    auto g = std::make_shared<Live>();
    unsigned char idb[8];
    randombytes_buf(idb, sizeof idb);
    g->id = hex(idb, sizeof idb);
    // Step 1: a random seed. With the allocator it becomes H(recent block || membership action).
    randombytes_buf(g->seed, 32);
    g->seedHex = hex(g->seed, 32);
    g->params = defaultParams();
    g->pow = defaultPow();
    g->game = std::make_unique<Game>(g->seedHex, g->params);
    g->startedMs = nowMs();
    for (unsigned i = 0; i < d->threads; i++) g->workers.emplace_back([g] { g->digest(); });
    g->loop = std::thread([g] { g->run(); });
    d->live = g;
    return json{{"ok", true}, {"gameId", g->id}, {"params", g->params.toJson()}, {"pow", g->pow.toJson()},
                {"chunks", g->params.chunksNeeded()}, {"threads", d->threads}}.dump();
}

std::string BubbleCoreImpl::frame(const std::string& gameId, int64_t dx, int64_t dy, const std::string& viewJson) {
    auto g = d->live;
    if (!g || g->id != gameId) return json{{"ok", false}, {"error", "no such game"}}.dump();
    json view = json::parse(viewJson, nullptr, false);
    double w = 800, h = 600;
    if (view.is_object()) {
        if (view.contains("w") && view["w"].is_number()) w = std::clamp(view["w"].get<double>(), 100.0, 8000.0);
        if (view.contains("h") && view["h"].is_number()) h = std::clamp(view["h"].get<double>(), 100.0, 8000.0);
    }
    std::lock_guard<std::mutex> l(g->m);
    g->lastFrameMs = nowMs();
    const Game& gm = *g->game;
    const int64_t mm = gm.mass();
    const double scale = std::min(1.4, h / (700.0 + 7.0 * (double)radius(mm) / kU));
    const double s = scale / kU, cx = w / 2 - gm.me.x * s, cy = h / 2 - gm.me.y * s;
    const double rMe = radius(mm) * s;

    // Steering: towards the pointer; near the middle of the bubble = stand still.
    if (!gm.over) {
        if ((double)(dx * dx + dy * dy) < rMe * rMe * 0.25) g->dir = kStop;
        else g->dir = dirOf(dx * 1000, dy * 1000);
    }

    json bots = json::array(), pellets = json::array();
    for (size_t j = 0; j < gm.bots.size(); j++) {
        const Bot& b = gm.bots[j];
        const double r = radius(b.mass) * s, x = cx + b.x * s, y = cy + b.y * s;
        if (x < -r || y < -r || x > w + r || y > h + r) continue;
        bots.push_back({std::lround(x), std::lround(y), std::lround(std::max(1.0, r)), (int)j, b.mass * 10 >= mm * 11 ? 1 : 0});
    }
    for (size_t j = 0; j < gm.pellets.size(); j++) {
        const double x = cx + gm.pellets[j].x * s, y = cy + gm.pellets[j].y * s;
        if (x < -5 || y < -5 || x > w + 5 || y > h + 5) continue;
        pellets.push_back(std::lround(x)); pellets.push_back(std::lround(y)); pellets.push_back((int)(j % 6));
    }
    const int64_t ms = std::max<int64_t>(1, nowMs() - g->startedMs);
    const uint64_t hs = g->hashes.load();
    const bool waiting = gm.me.stomach >= g->params.chunk && gm.chunks < g->params.chunksNeeded()
                         && !g->found.count((uint32_t)gm.chunks);
    json me = {{"r", std::lround(rMe)}, {"body", gm.me.body}, {"stomach", gm.me.stomach},
               {"stomachR", std::lround(mm ? rMe * std::sqrt((double)gm.me.stomach / (double)mm) : 0)},
               {"shield", gm.me.shield > 0}, {"deaths", gm.deaths}, {"start", g->params.start}, {"goal", g->params.goal},
               {"digesting", waiting}};
    return json{{"ok", true}, {"tick", gm.tick}, {"seconds", gm.tick / kTickHz}, {"over", gm.over}, {"won", gm.won},
                {"why", gm.why}, {"me", me}, {"scale", scale},
                {"world", {std::lround(cx), std::lround(cy), std::lround(kWorld * s), std::lround(100 * kU * s)}},
                {"bots", bots}, {"pellets", pellets},
                {"proofs", {{"found", g->found.size()}, {"need", g->params.chunksNeeded()}, {"hashRate", (double)hs * 1000.0 / (double)ms}}},
                {"check", {{"state", g->checkState}, {"result", g->checkResult}}}}.dump();
}

std::string BubbleCoreImpl::abandon(const std::string& gameId) {
    if (d->live && d->live->id == gameId) { d->live->shutdown(); d->live.reset(); }
    return json{{"ok", true}}.dump();
}

std::string BubbleCoreImpl::record(const std::string& gameId) {
    auto g = d->live;
    if (!g || g->id != gameId) return json{{"ok", false}, {"error", "no such game"}}.dump();
    std::lock_guard<std::mutex> l(g->m);
    Record r = g->rec;
    if (!g->game->over) r.end = g->game->tick;
    return json{{"ok", true}, {"seed", g->seedHex}, {"params", g->params.toJson()}, {"pow", g->pow.toJson()},
                {"record", r.toJson(g->seedHex)}}.dump();
}

std::string BubbleCoreImpl::checkRecord(const std::string& seedHex, const std::string& recordJson) {
    json rec = json::parse(recordJson, nullptr, false);
    if (!rec.is_object()) return json{{"ok", false}, {"reason", "the record is not JSON"}}.dump();
    try { return checkGame(seedHex, defaultParams(), defaultPow(), rec).dump(); }
    catch (const std::exception& e) { return json{{"ok", false}, {"reason", e.what()}}.dump(); }
}
