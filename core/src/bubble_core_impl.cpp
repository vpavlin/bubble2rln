#include "bubble_core_impl.h"
#include "check.h"
#include "pow.h"

#include <sodium.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using nlohmann::json;

namespace {

json defaultParams() { return {{"start", 10}, {"goal", 300}, {"chunk", 5}, {"minTicks", 1800}, {"maxTicks", 36000}}; }

json defaultPow() {
    int bits = 9;
    if (const char* e = std::getenv("BUBBLE_POW_BITS")) bits = std::atoi(e);
    return {{"bits", bits}, {"memKiB", 8192}};
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

struct Game {
    std::string id, seedHex;
    unsigned char seed[32];
    json params, pow;
    uint32_t chunks = 0;
    int64_t startedMs = 0;
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> next{0};
    std::atomic<uint64_t> hashes{0};
    std::mutex m;
    std::map<uint32_t, uint32_t> found;
    std::string checkState;
    json checkResult;
    std::vector<std::thread> workers;
    std::thread checker;

    void digest() {
        const int bits = pow["bits"].get<int>();
        const size_t mem = pow["memKiB"].get<size_t>();
        for (uint32_t c; !stop && (c = next++) < chunks;) {
            for (uint32_t n = 0; !stop; n++) {
                hashes++;
                if (bubble::digestOk(seed, c, n, bits, mem)) {
                    std::lock_guard<std::mutex> l(m);
                    found[c] = n;
                    break;
                }
            }
        }
    }

    void shutdown() {
        stop = true;
        for (auto& t : workers) if (t.joinable()) t.join();
        if (checker.joinable()) checker.join();
    }
};

}

struct BubbleCorePrivate {
    std::shared_ptr<Game> game;
    unsigned threads = 1;
};

BubbleCoreImpl::BubbleCoreImpl() : d(new BubbleCorePrivate) {
    if (sodium_init() < 0) std::abort();
    unsigned hw = std::thread::hardware_concurrency();
    d->threads = hw > 1 ? hw - 1 : 1;   // leave one core for Basecamp and the game
}

BubbleCoreImpl::~BubbleCoreImpl() {
    if (d->game) d->game->shutdown();
    delete d;
}

std::string BubbleCoreImpl::snapshot() {
    json j = {{"ok", true}, {"params", defaultParams()}, {"pow", defaultPow()}, {"threads", d->threads}};
    if (d->game) j["gameId"] = d->game->id;
    return j.dump();
}

std::string BubbleCoreImpl::newGame(const std::string& optionsJson) {
    (void)optionsJson;
    if (d->game) { d->game->shutdown(); d->game.reset(); }
    auto g = std::make_shared<Game>();
    unsigned char idb[8];
    randombytes_buf(idb, sizeof idb);
    g->id = hex(idb, sizeof idb);
    // Step 1: a random seed. With the allocator it becomes H(recent block || membership action).
    randombytes_buf(g->seed, 32);
    g->seedHex = hex(g->seed, 32);
    g->params = defaultParams();
    g->pow = defaultPow();
    const int start = g->params["start"], goal = g->params["goal"], chunk = g->params["chunk"];
    g->chunks = (uint32_t)((goal - start + chunk - 1) / chunk);
    g->startedMs = nowMs();
    for (unsigned i = 0; i < d->threads; i++) g->workers.emplace_back([g] { g->digest(); });
    d->game = g;
    return json{{"ok", true}, {"gameId", g->id}, {"seed", g->seedHex}, {"params", g->params}, {"pow", g->pow},
                {"chunks", g->chunks}, {"threads", d->threads}}.dump();
}

std::string BubbleCoreImpl::progress(const std::string& gameId) {
    auto g = d->game;
    if (!g || g->id != gameId) return json{{"ok", false}, {"error", "no such game"}}.dump();
    json found = json::array(), check;
    {
        std::lock_guard<std::mutex> l(g->m);
        for (const auto& [c, n] : g->found) found.push_back({c, n});
        check = {{"state", g->checkState}, {"result", g->checkResult}};
    }
    const int64_t ms = std::max<int64_t>(1, nowMs() - g->startedMs);
    const uint64_t h = g->hashes.load();
    return json{{"ok", true}, {"found", found}, {"chunks", g->chunks}, {"hashes", h},
                {"hashRate", (double)h * 1000.0 / (double)ms}, {"check", check}}.dump();
}

std::string BubbleCoreImpl::finish(const std::string& gameId, const std::string& recordJson) {
    auto g = d->game;
    if (!g || g->id != gameId) return json{{"ok", false}, {"error", "no such game"}}.dump();
    json rec = json::parse(recordJson, nullptr, false);
    if (!rec.is_object()) return json{{"ok", false}, {"error", "the record is not JSON"}}.dump();
    {
        std::lock_guard<std::mutex> l(g->m);
        if (g->checkState == "running") return json{{"ok", true}, {"pending", true}}.dump();
        g->checkState = "running";
        g->checkResult = nullptr;
    }
    if (g->checker.joinable()) g->checker.join();
    g->checker = std::thread([g, rec] {
        json r;
        try { r = bubble::checkGame(g->seedHex, g->params, g->pow, rec); }
        catch (const std::exception& e) { r = {{"ok", false}, {"reason", std::string("check failed: ") + e.what()}}; }
        std::lock_guard<std::mutex> l(g->m);
        g->checkState = "done";
        g->checkResult = r;
    });
    return json{{"ok", true}, {"pending", true}}.dump();
}

std::string BubbleCoreImpl::abandon(const std::string& gameId) {
    if (d->game && d->game->id == gameId) { d->game->shutdown(); d->game.reset(); }
    return json{{"ok", true}}.dump();
}

std::string BubbleCoreImpl::simSource() {
    return json{{"ok", true}, {"source", bubble::simSource()}}.dump();
}
