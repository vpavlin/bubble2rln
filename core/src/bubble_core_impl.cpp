#include "bubble_core_impl.h"
#include "beacon.h"
#include "claim.h"
#include "check.h"
#include "pow.h"
#include "sim.h"

#include "logos_lp_client.h"

#include <sodium.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
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

int64_t wallMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The gifter this build claims from by default: its libp2p peer id and address, and the
// registry (config account, base58) it serves. BUBBLE_GIFTER_PEER / BUBBLE_GIFTER_ADDR /
// BUBBLE_REGISTRY override them; claim()'s options override everything.
// The default is the public gifter on the VPS (deploy/vps): it registers on its own devnet
// registry, so the claim uses the demo identity there.
struct GifterDefaults {
    std::string peer = "16Uiu2HAkx4HRjCAN4HCWZyqXn7ZxnDGhxQhcpDuBhANBhx3VXP88";
    std::string addr = "/ip4/128.140.55.128/tcp/9101";
    std::string registry = "APtni2RgeHkW6HChE1VL1jgdnUr1jyd1smV13JjDPk74";
    std::string network = "local", identity = "demo";
};
GifterDefaults gifterDefaults() {
    GifterDefaults g;
    // BUBBLE_IDENTITY=rln claims with the player's own RLN identity (rln_module), on
    // BUBBLE_NETWORK's registry ("testnet" = the logos.test one; "local" = a devnet).
    if (const char* e = std::getenv("BUBBLE_IDENTITY")) g.identity = e;
    if (const char* e = std::getenv("BUBBLE_NETWORK")) g.network = e;
    if (const char* e = std::getenv("BUBBLE_GIFTER_PEER")) g.peer = e;
    if (const char* e = std::getenv("BUBBLE_GIFTER_ADDR")) g.addr = e;
    if (const char* e = std::getenv("BUBBLE_REGISTRY")) g.registry = e;
    return g;
}

// The LEZ sequencer a game takes its beacon block from. BUBBLE_LEZ_URL overrides it.
std::string defaultLezUrl() {
    if (const char* e = std::getenv("BUBBLE_LEZ_URL")) return e;
    return "https://testnet.lez.logos.co";
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

// Logos Delivery's RLN application identifier (logos-delivery-module rln_presets.h): a
// membership registered under it is the one the player's Delivery node proves with.
constexpr const char* kDeliveryRlnIdentifier = "5e269b6a19fce081f5808b13442dcbc3522197638dd38df5a28bc4e55236b977";

// base58 (bitcoin alphabet) of a 32-byte account id -> 64 hex; "" if it isn't one.
std::string b58ToHex32(const std::string& b58) {
    static const std::string alpha = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    std::vector<unsigned char> n(32, 0);
    for (char ch : b58) {
        size_t v = alpha.find(ch);
        if (v == std::string::npos) return "";
        unsigned carry = (unsigned)v;
        for (int i = 31; i >= 0; i--) { carry += 58u * n[i]; n[i] = carry & 0xff; carry >>= 8; }
        if (carry) return "";
    }
    std::string out(65, '\0');
    sodium_bin2hex(out.data(), out.size(), n.data(), 32);
    out.resize(64);
    return out;
}

json parseReply(const json& r) {
    if (r.is_string()) return json::parse(r.get<std::string>(), nullptr, false);
    return r;
}

struct Live {
    std::string id, seedHex;
    unsigned char seed[32];
    // The claim binding: the beacon block the seed is made from and the game's own key.
    // Without a beacon (the sequencer unreachable) the game is practice: playable, not claimable.
    bool claimable = false;
    std::string beaconWhy;
    Beacon beacon;
    unsigned char pk[crypto_sign_PUBLICKEYBYTES];
    unsigned char sk[crypto_sign_SECRETKEYBYTES];
    Params params;
    PowParams pow;
    std::unique_ptr<Game> game;
    Record rec;
    int64_t startedMs = 0;

    // A chunk in the stomach waiting for its proof (mirrors game->queue).
    struct Job { int64_t chunk; Queued q; uint32_t next = 0; bool done = false; uint32_t nonce = 0; };

    std::mutex m;                       // guards everything below and game/rec
    std::condition_variable cv;         // a new job, or stop
    int dir = kStop;
    int64_t lastFrameMs = 0;
    std::vector<Job> jobs;
    std::string checkState;
    json checkResult;
    std::string claimState;   // "" | "asking" | "granted" | "refused" | "failed"
    json claimResult;
    int64_t rateMs = 0;
    uint64_t rateHashes = 0;
    double rate = 0;

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> hashes{0};
    std::vector<std::thread> workers;
    std::thread loop;

    static bool same(const Job& a, int64_t chunk, const Queued& q) { return a.chunk == chunk && a.q.tick == q.tick && a.q.hash == q.hash; }

    // Every thread works on the oldest chunk still without a proof, so the stomach empties in order.
    void digest() {
        std::unique_lock<std::mutex> l(m);
        while (!stop) {
            Job* j = nullptr;
            for (auto& x : jobs) if (!x.done) { j = &x; break; }
            if (!j) { cv.wait_for(l, std::chrono::milliseconds(200)); continue; }
            const int64_t chunk = j->chunk;
            const Queued q = j->q;
            const uint32_t n = j->next++;
            l.unlock();
            hashes++;
            const bool ok = digestOk(seed, (uint32_t)chunk, (uint32_t)q.tick, q.hash, n, pow.bits, pow.memKiB);
            l.lock();
            if (!ok) continue;
            for (auto& x : jobs) if (same(x, chunk, q) && !x.done) { x.done = true; x.nonce = n; }
        }
    }

    // After a tick: the jobs follow the game's queue (digested ones leave, new ones come,
    // and on a death the stomach's work is lost).
    void syncJobsLocked() {
        const Game& g = *game;
        std::vector<Job> nj;
        bool added = false;
        for (size_t i = 0; i < g.queue.size(); i++) {
            const int64_t chunk = g.chunks + (int64_t)i;
            auto it = std::find_if(jobs.begin(), jobs.end(), [&](const Job& x) { return same(x, chunk, g.queue[i]); });
            if (it != jobs.end()) nj.push_back(*it);
            else { nj.push_back(Job{chunk, g.queue[i]}); added = true; }
        }
        jobs.swap(nj);
        if (added) cv.notify_all();
    }

    // One tick, under the lock.
    void tickLocked() {
        Game& g = *game;
        std::vector<std::pair<int64_t, uint32_t>> dig;
        std::vector<int64_t> ds;
        for (const auto& j : jobs) {
            if (!j.done) break;
            dig.push_back({j.chunk, j.nonce});
            ds.push_back(j.chunk);
        }
        rec.add(g.tick, dir, dig);
        std::string err = g.step(dir, ds);
        if (!err.empty()) { g.over = true; g.why = "error: " + err; }
        if (g.over) { rec.end = g.tick; jobs.clear(); }
        else syncJobsLocked();
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

    ~Live() { sodium_memzero(sk, sizeof sk); }

    void shutdown() {
        { std::lock_guard<std::mutex> l(m); stop = true; }
        cv.notify_all();
        for (auto& t : workers) if (t.joinable()) t.join();
        if (loop.joinable()) loop.join();
    }
};

}

struct BubbleCorePrivate {
    std::shared_ptr<Live> live;
    unsigned threads = 1;
    std::mutex capMu;
    std::deque<int64_t> accepted;   // when the claims this gifter accepted were verified (the hourly cap)
    std::unique_ptr<logos::LpClient> allocator;   // membership_allocation_client_module, by name
    std::unique_ptr<logos::LpClient> libp2p;      // libp2p_module, by name
    std::unique_ptr<logos::LpClient> rln;         // rln_module, by name (the player's own RLN identity)
    std::vector<std::thread> claimThreads;
};

BubbleCoreImpl::BubbleCoreImpl() : d(new BubbleCorePrivate) {
    if (sodium_init() < 0) std::abort();
    unsigned hw = std::thread::hardware_concurrency();
    d->threads = hw > 2 ? hw - 2 : 1;   // leave room for Basecamp and the game loop
}

BubbleCoreImpl::~BubbleCoreImpl() {
    if (d->live) d->live->shutdown();
    for (auto& t : d->claimThreads) if (t.joinable()) t.join();
    delete d;
}

std::string BubbleCoreImpl::snapshot() {
    const GifterDefaults gd = gifterDefaults();
    json j = {{"ok", true}, {"params", defaultParams().toJson()}, {"pow", defaultPow().toJson()}, {"threads", d->threads},
              {"gifter", {{"peer", gd.peer}, {"addr", gd.addr}, {"registry", gd.registry}}}};
    if (d->live) j["gameId"] = d->live->id;
    return j.dump();
}

std::string BubbleCoreImpl::newGame(const std::string& optionsJson) {
    json opts = json::parse(optionsJson, nullptr, false);
    std::string lezUrl = defaultLezUrl();
    if (opts.is_object() && opts.contains("lezUrl") && opts["lezUrl"].is_string()) lezUrl = opts["lezUrl"];
    if (d->live) { d->live->shutdown(); d->live.reset(); }
    auto g = std::make_shared<Live>();
    unsigned char idb[8];
    randombytes_buf(idb, sizeof idb);
    g->id = hex(idb, sizeof idb);
    // The seed comes from the newest LEZ block and this game's key (claim.h), so no game can be
    // prepared before that block. Without a sequencer the game is practice.
    crypto_sign_keypair(g->pk, g->sk);
    std::string why;
    if (latestBeacon(lezUrl, g->beacon, why)) {
        g->claimable = true;
        g->seedHex = gameSeed(g->beacon, g->pk);
        seedFromHex(g->seedHex, g->seed);
    } else {
        g->beaconWhy = "practice game: no LEZ block from " + lezUrl + " (" + why + ")";
        randombytes_buf(g->seed, 32);
        g->seedHex = hex(g->seed, 32);
    }
    g->params = defaultParams();
    g->pow = defaultPow();
    g->game = std::make_unique<Game>(g->seedHex, g->params);
    g->startedMs = nowMs();
    for (unsigned i = 0; i < d->threads; i++) g->workers.emplace_back([g] { g->digest(); });
    g->loop = std::thread([g] { g->run(); });
    d->live = g;
    json beacon = g->claimable ? json{{"id", g->beacon.id}, {"hash", hex(g->beacon.hash, 32)}, {"ts", g->beacon.tsMs}} : json();
    return json{{"ok", true}, {"gameId", g->id}, {"params", g->params.toJson()}, {"pow", g->pow.toJson()},
                {"chunks", g->params.chunksNeeded()}, {"threads", d->threads}, {"claimable", g->claimable},
                {"beacon", beacon}, {"why", g->beaconWhy}}.dump();
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
    // Hashes per second over the last second or so (the threads idle while the stomach is empty).
    const int64_t now = nowMs();
    if (now - g->rateMs >= 1000) {
        const uint64_t hs = g->hashes.load();
        if (g->rateMs) g->rate = (double)(hs - g->rateHashes) * 1000.0 / (double)(now - g->rateMs);
        g->rateMs = now; g->rateHashes = hs;
    }
    int64_t ready = 0;
    for (const auto& j : g->jobs) ready += j.done ? 1 : 0;
    const bool waiting = !g->jobs.empty() && !g->jobs.front().done;
    json me = {{"r", std::lround(rMe)}, {"body", gm.me.body}, {"stomach", gm.me.stomach},
               {"stomachR", std::lround(mm ? rMe * std::sqrt((double)gm.me.stomach / (double)mm) : 0)},
               {"shield", gm.me.shield > 0}, {"deaths", gm.deaths}, {"start", g->params.start}, {"goal", g->params.goal},
               {"digesting", waiting}};
    return json{{"ok", true}, {"tick", gm.tick}, {"seconds", gm.tick / kTickHz}, {"over", gm.over}, {"won", gm.won},
                {"why", gm.why}, {"me", me}, {"scale", scale},
                {"world", {std::lround(cx), std::lround(cy), std::lround(kWorld * s), std::lround(100 * kU * s)}},
                {"bots", bots}, {"pellets", pellets},
                {"proofs", {{"found", gm.chunks + ready}, {"need", g->params.chunksNeeded()}, {"queued", (int64_t)g->jobs.size()}, {"hashRate", g->rate}}},
                {"check", {{"state", g->checkState}, {"result", g->checkResult}}},
                {"claimable", g->claimable}, {"claim", {{"state", g->claimState}, {"result", g->claimResult}}}}.dump();
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
    json beacon = g->claimable ? json{{"id", g->beacon.id}, {"hash", hex(g->beacon.hash, 32)}, {"ts", g->beacon.tsMs}} : json();
    return json{{"ok", true}, {"seed", g->seedHex}, {"params", g->params.toJson()}, {"pow", g->pow.toJson()},
                {"record", r.toJson(g->seedHex)}, {"claimable", g->claimable}, {"beacon", beacon},
                {"gameKey", hex(g->pk, 32)}}.dump();
}

std::string BubbleCoreImpl::checkRecord(const std::string& seedHex, const std::string& recordJson) {
    json rec = json::parse(recordJson, nullptr, false);
    if (!rec.is_object()) return json{{"ok", false}, {"reason", "the record is not JSON"}}.dump();
    try { return checkGame(seedHex, defaultParams(), defaultPow(), rec).dump(); }
    catch (const std::exception& e) { return json{{"ok", false}, {"reason", e.what()}}.dump(); }
}

std::string BubbleCoreImpl::verify_auth(const std::string& requestJson) {
    json req = json::parse(requestJson, nullptr, false);
    json cfg = req.is_object() ? req.value("config", json()) : json();
    GifterRules rules;
    std::string why;
    if (!GifterRules::fromJson(cfg, rules, why)) return json{{"error", why}}.dump();
    json verdict;
    try {
        verdict = verifyClaim(req, [&](uint64_t id, Beacon& out, std::string& w, bool& unreachable) {
            return beaconById(rules.lezUrl, id, out, w, unreachable);
        }, wallMs());
    } catch (const std::exception& e) {
        return json{{"error", std::string("verify_auth: ") + e.what()}}.dump();
    }
    // The gifter's hourly cap on accepted claims: the hard bound on what bots can take.
    if (verdict.value("ok", false) && rules.maxClaimsPerHour > 0) {
        std::lock_guard<std::mutex> l(d->capMu);
        const int64_t now = nowMs();
        while (!d->accepted.empty() && now - d->accepted.front() > 3600 * 1000) d->accepted.pop_front();
        if ((int)d->accepted.size() >= rules.maxClaimsPerHour)
            return json{{"ok", false}, {"reason", "this gifter has given its " + std::to_string(rules.maxClaimsPerHour) + " memberships for this hour; try again later"}}.dump();
        d->accepted.push_back(now);
    }
    return verdict.dump();
}

std::string BubbleCoreImpl::produce_auth(const std::string& requestJson) {
    json req = json::parse(requestJson, nullptr, false);
    if (!req.is_object() || !req.contains("action_hex") || !req["action_hex"].is_string() || !req.contains("domain")
        || !req["domain"].is_string() || req.value("auth_type", std::string()) != kAuthType)
        return json{{"error", std::string("produce_auth args: need auth_type \"") + kAuthType + "\", domain, action_hex"}}.dump();
    json args = req.value("args", json::object());
    if (args.is_string()) args = json::parse(args.get<std::string>(), nullptr, false);
    if (!args.is_object()) args = json::object();
    std::string action = req["action_hex"];
    if (action.rfind("0x", 0) == 0 || action.rfind("0X", 0) == 0) action = action.substr(2);
    unsigned char act[32];
    size_t len = 0;
    if (action.size() != 64 || sodium_hex2bin(act, 32, action.data(), action.size(), nullptr, &len, nullptr) != 0 || len != 32)
        return json{{"error", "action_hex: expected 64 hex chars (32 bytes)"}}.dump();
    auto g = d->live;
    if (!g || (args.contains("gameId") && args["gameId"] != g->id)) return json{{"error", "no such game"}}.dump();
    std::lock_guard<std::mutex> l(g->m);
    if (!g->claimable) return json{{"error", g->beaconWhy.empty() ? "a practice game can't be claimed" : g->beaconWhy}}.dump();
    if (!g->game->won || g->checkState != "done" || !g->checkResult.value("ok", false))
        return json{{"error", "the game is not won and checked yet"}}.dump();
    Claim c;
    c.beaconId = g->beacon.id;
    memcpy(c.pub, g->pk, 32);
    c.end = (uint32_t)g->rec.end;
    if (g->game->digested.size() != g->rec.digests.size()) return json{{"error", "the record and the game disagree"}}.dump();
    for (size_t i = 0; i < g->rec.digests.size(); i++)
        c.chunks.push_back({(uint32_t)g->game->digested[i].tick, g->game->digested[i].hash, std::get<2>(g->rec.digests[i])});
    const std::string audience = args.contains("audience") && args["audience"].is_string() ? args["audience"].get<std::string>() : "";
    unsigned char digest[32];
    bindingDigest(req["domain"], kAuthType, act, audience, encodeClaim(c, false), digest);
    crypto_sign_detached(c.sig, nullptr, digest, 32, g->sk);
    const auto bytes = encodeClaim(c, true);
    return json{{"payload_hex", hex(bytes.data(), bytes.size())}}.dump();
}

// The claim with the player's own RLN identity: rln_module makes the identity (the secret never
// leaves its keystore) and asks the gifter through the allocator client, which calls our
// produce_auth for the payload. The membership then backs the player's Delivery node.
static std::string claimWithRln(BubbleCorePrivate* d, const std::shared_ptr<Live>& g, const json& o,
                                const GifterDefaults& gd, const std::string& me) {
    const std::string network = o.value("network", gd.network);
    const std::string configHex = b58ToHex32(o["config"].get<std::string>());
    if (configHex.empty()) return json{{"ok", false}, {"error", "the registry is not a base58 account id"}}.dump();
    const std::string registryId = "logos:" + network + ":" + configHex;
    const std::string rlnId = o.value("rlnIdentifier", std::string(kDeliveryRlnIdentifier));
    json opts = json::array();
    auto kv = [&](const std::string& k, const std::string& v) { opts.push_back({{"key", k}, {"value", v}}); };
    kv("delegated", "true");
    kv("rate_limit", std::to_string(o.value("rate", 100)));
    kv("auth_type", kAuthType);
    kv("auth_provider", me);
    kv("auth_args", json{{"gameId", g->id}, {"audience", o.value("audience", std::string())}}.dump());
    if (o.contains("allocatorPeerId")) {
        kv("discover", "false");
        kv("allocator_peer_id", o["allocatorPeerId"].get<std::string>());
        kv("allocator_multiaddr", o.value("allocatorMultiaddr", std::string()));
    }
    {
        std::lock_guard<std::mutex> l(g->m);
        g->claimResult = json{{"demoIdentity", false}, {"registryId", registryId}};
    }
    if (!d->rln) d->rln = std::make_unique<logos::LpClient>("rln_module", me);
    if (!d->libp2p) d->libp2p = std::make_unique<logos::LpClient>("libp2p_module", me);
    std::weak_ptr<Live> weak = g;
    logos::LpClient* rln = d->rln.get();
    logos::LpClient* p2p = d->libp2p.get();
    d->claimThreads.erase(std::remove_if(d->claimThreads.begin(), d->claimThreads.end(), [](std::thread& t) {
        return !t.joinable(); }), d->claimThreads.end());
    d->claimThreads.emplace_back([weak, rln, p2p, registryId, rlnId, opts] {
        auto finish = [&](const std::string& state, json result, const std::string& message) {
            if (auto g = weak.lock()) {
                std::lock_guard<std::mutex> l(g->m);
                result["demoIdentity"] = false;
                result["registryId"] = registryId;
                if (!message.empty()) result["message"] = message;
                g->claimState = state;
                g->claimResult = result;
            }
        };
        logos::CallError err;
        p2p->invoke("start", json::array(), &err, 20000);
        // A fresh keystore unlocks itself (rln_module's auto-unlock); a user-password one must be unlocked by its owner.
        json u = parseReply(rln->invoke("unlock_keystore_auto", json::array(), &err, 30000));
        json r = parseReply(rln->invoke("register_membership", json::array({registryId, rlnId, opts.dump()}), &err, 60000));
        if (!r.is_object() || r.contains("error")) {
            std::string why = r.is_object() && r["error"].is_object() ? r["error"].value("message", r["error"].dump())
                            : r.is_object() ? r["error"].dump() : (err.message.empty() ? "no reply from rln_module" : err.message);
            if (u.is_object() && u.contains("error")) why += " (keystore: " + u["error"].dump() + ")";
            finish("failed", r.is_object() ? r : json::object(), "rln_module could not start the registration: " + why);
            return;
        }
        // Poll the membership until the gifter's registration shows on chain (or fails).
        for (int i = 0; i < 120; i++) {
            if (weak.expired()) return;
            json st = parseReply(rln->invoke("get_membership_state", json::array({registryId, rlnId}), &err, 20000));
            const std::string state = st.is_object() ? st.value("state", std::string()) : "";
            if (state == "active") { finish("granted", st, ""); return; }
            if (state == "failed") {
                const std::string why = st.value("failed_reason", std::string("the registration failed"));
                const bool used = why.find("credential already used") != std::string::npos || why.find("credential_used") != std::string::npos;
                finish(used ? "refused" : "failed", st, used ? "This game has already been claimed. Play another one to claim again." : why);
                return;
            }
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
        finish("failed", json::object(), "the membership did not become active within 6 minutes");
    });
    return json{{"ok", true}, {"state", "asking"}}.dump();
}

std::string BubbleCoreImpl::claim(const std::string& gameId, const std::string& optionsJson) {
    auto g = d->live;
    if (!g || g->id != gameId) return json{{"ok", false}, {"error", "no such game"}}.dump();
    json o = json::parse(optionsJson, nullptr, false);
    if (!o.is_object()) o = json::object();
    const GifterDefaults gd = gifterDefaults();
    if (!o.contains("config") && !gd.registry.empty()) o["config"] = gd.registry;
    if (!o.contains("allocatorPeerId") && !gd.peer.empty()) { o["allocatorPeerId"] = gd.peer; o["allocatorMultiaddr"] = gd.addr; }
    if (!o.contains("config") || !o["config"].is_string() || o["config"].get<std::string>().empty())
        return json{{"ok", false}, {"error", "no gifter is configured (set BUBBLE_REGISTRY and BUBBLE_GIFTER_PEER/ADDR)"}}.dump();
    {
        std::lock_guard<std::mutex> l(g->m);
        if (!g->claimable) return json{{"ok", false}, {"error", g->beaconWhy.empty() ? "a practice game can't be claimed" : g->beaconWhy}}.dump();
        if (!g->game->won || g->checkState != "done" || !g->checkResult.value("ok", false))
            return json{{"ok", false}, {"error", "the game is not won and checked yet"}}.dump();
        if (g->claimState == "asking" || g->claimState == "granted") return json{{"ok", true}, {"state", g->claimState}}.dump();
        g->claimState = "asking";
        g->claimResult = json();
    }
    const std::string me = moduleName().empty() ? std::string("bubble_core") : moduleName();
    if (o.value("identity", gd.identity) == "rln") return claimWithRln(d, g, o, gd, me);

    // Without an identity of the player's own, a demo one: a random commitment, so the
    // membership is real on the registry but has no secret behind it to send with.
    bool demoIdentity = false;
    if (!o.contains("identityCommitment") || !o["identityCommitment"].is_string()) {
        unsigned char idc[32];
        randombytes_buf(idc, 31);
        idc[31] = 0;   // little-endian below 2^248 < the BN254 modulus
        o["identityCommitment"] = hex(idc, 32);
        demoIdentity = true;
    }
    // The allocator client asks the gifter over libp2p; it calls back our produce_auth for the
    // payload, and answers once the membership is on chain (up to ~190 s).
    json args = {{"config", o["config"]}, {"identityCommitment", o["identityCommitment"]}, {"rate", o.value("rate", 100)},
                 {"authType", kAuthType}, {"authProvider", moduleName().empty() ? std::string("bubble_core") : moduleName()},
                 {"authArgs", {{"gameId", gameId}, {"audience", o.value("audience", std::string())}}},
                 {"discover", o.value("discover", !o.contains("allocatorPeerId"))}};
    if (o.contains("allocatorPeerId")) { args["allocatorPeerId"] = o["allocatorPeerId"]; args["allocatorMultiaddr"] = o.value("allocatorMultiaddr", std::string()); }
    if (!d->allocator) d->allocator = std::make_unique<logos::LpClient>("membership_allocation_client_module", me);
    if (!d->libp2p) d->libp2p = std::make_unique<logos::LpClient>("libp2p_module", me);
    std::weak_ptr<Live> weak = g;
    {
        std::lock_guard<std::mutex> l(g->m);
        g->claimResult = json{{"demoIdentity", demoIdentity}, {"identityCommitment", o["identityCommitment"]}};
    }
    // The claim travels over this node's libp2p node: make sure it runs ("already started" is fine).
    logos::CallError ignored;
    d->libp2p->invoke("start", json::array(), &ignored, 20000);
    d->allocator->invokeAsync("request", json::array({args.dump()}), [weak](json r) {
        auto g = weak.lock();
        if (!g) return;
        json reply = r.is_string() ? json::parse(r.get<std::string>(), nullptr, false) : r;
        std::lock_guard<std::mutex> l(g->m);
        if (reply.is_object() && reply.contains("leaf_index")) g->claimState = "granted";
        else if (reply.is_object() && reply.value("kind", std::string()) == "refused") g->claimState = "refused";
        else g->claimState = "failed";
        const bool demo = g->claimResult.value("demoIdentity", false);
        g->claimResult = reply.is_discarded() || reply.is_null() ? json{{"error", "no reply from membership_allocation_client_module"}} : reply;
        g->claimResult["demoIdentity"] = demo;
        // What to tell the player, in words (the view shows "message").
        const std::string reason = reply.is_object() ? reply.value("reason", std::string()) : "";
        const std::string err = reply.is_object() ? reply.value("error", std::string()) : "";
        std::string msg;
        if (g->claimState == "granted") msg = "";
        else if (reason == "credential_used") msg = "This game has already been claimed. Play another one to claim again.";
        else if (reply.is_object() && reply.value("kind", std::string()) == "no_provider") msg = "No gifter answered. It may be offline; try again later.";
        else if (reply.is_object() && reply.value("kind", std::string()) == "unconfirmed") msg = "The gifter sent your registration, but the chain had not confirmed it in time. It may still land.";
        else msg = err.empty() ? (reason.empty() ? "no reason given" : reason) : err;
        if (!msg.empty()) g->claimResult["message"] = msg;
    }, 240000);
    return json{{"ok", true}, {"state", "asking"}}.dump();
}

std::string BubbleCoreImpl::claimStatus(const std::string& gameId) {
    auto g = d->live;
    if (!g || g->id != gameId) return json{{"ok", false}, {"error", "no such game"}}.dump();
    std::lock_guard<std::mutex> l(g->m);
    return json{{"ok", true}, {"state", g->claimState}, {"result", g->claimResult}}.dump();
}

