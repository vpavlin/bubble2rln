#include "bubble_core_impl.h"
#include "beacon.h"
#include "claim.h"
#include "check.h"
#include "pow.h"
#include "score.h"
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
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
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
// The default is the public gifter on the VPS (deploy/vps): it registers on the LEZ testnet
// registry (the one Logos Delivery's logos.test preset trusts) through liblogos_lez_rln_module;
// the claim uses a demo identity unless BUBBLE_IDENTITY=rln.
struct GifterDefaults {
    std::string peer = "16Uiu2HAkx4HRjCAN4HCWZyqXn7ZxnDGhxQhcpDuBhANBhx3VXP88";
    std::string addr = "/ip4/128.140.55.128/tcp/9101";
    std::string registry = "J3NXPWUspX2zqJU3tUbR6MHmm83aJc5AzvidEaBBKnat";   // the LEZ testnet registry Delivery trusts
    std::string network = "testnet", identity = "rln";
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
    // The default gifter's own chain, through its read-only beacon proxy (deploy/vps): one
    // dependency for the claim instead of two. (The LEZ testnet rewrote blocks under a game
    // on 2026-11-22: a game seeded from a block that later changes can never be claimed.)
    return "http://128.140.55.128:3041/";
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
    double viewScale = 0;      // the camera's zoom, eased towards the size's (drawing only)
    int64_t viewScaleMs = 0;

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
            // Digestion runs only while the game does: a paused game (no frames) doesn't digest,
            // or pausing would grow you without the clock moving.
            if (!j || nowMs() - lastFrameMs > pauseAfterMs()) { cv.wait_for(l, std::chrono::milliseconds(100)); continue; }
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
                if (won || (over && params.endless)) checkState = "running";
            }
            if (!over) continue;
            if (won || params.endless) {
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

// The endless-mode leaderboard: scores posted on Logos Delivery (score.h), each checked here by
// replaying its game before it is listed. One background thread: it brings up (or joins) this
// Basecamp's Delivery node, subscribes to the score topic, checks what arrives, keeps the board on
// disk and re-posts this player's best now and then (Delivery's Store catch-up returns old posts to
// a node that subscribes later, and the re-posts keep them there).
struct ScoreBoard {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::pair<std::string, std::vector<unsigned char>>> inbox;   // (message hash, payload)
    std::set<std::string> seen;                 // message hashes already handled
    std::map<std::string, json> entries;        // game id -> {score, ticks, ended, when, id, mine}
    std::map<std::string, json> mine;           // game id -> this player's signed score message
    std::deque<json> outbox;                    // score messages to send once Delivery is up
    std::string status = "starting";            // starting | joining | online | error: …
    std::string persist;
    int64_t lastRepostMs = 0, checked = 0, refused = 0;
    bool up = false, dirty = false;
    std::atomic<bool> stop{false};
    std::thread worker;
    std::unique_ptr<logos::LpClient> delivery;
    logos::LpSubscription sub;
};

struct BubbleCorePrivate {
    std::shared_ptr<Live> live;
    unsigned threads = 1;
    std::mutex capMu;
    std::deque<int64_t> accepted;   // when the claims this gifter accepted were verified (the hourly cap)
    std::unique_ptr<logos::LpClient> allocator;   // membership_allocation_client_module, by name
    std::unique_ptr<logos::LpClient> libp2p;      // libp2p_module, by name
    std::unique_ptr<logos::LpClient> rln;         // rln_module, by name (the player's own RLN identity)
    std::vector<std::thread> claimThreads;
    // rlnStatus: a watcher thread that reads the player's RLN wallet and membership while someone asks.
    std::mutex rwMu;
    json rwLast;                       // the last reading, served as is
    int64_t rwAskedMs = 0;             // the watcher reads while this is under a minute old
    std::thread rwThread;
    std::atomic<bool> rwStop{false};
    std::string persistPath;           // this module's own data dir: the wallet lives in a sibling's
    std::shared_ptr<ScoreBoard> board;   // the endless-mode leaderboard, started on first use
};

BubbleCoreImpl::BubbleCoreImpl() : d(new BubbleCorePrivate) {
    if (sodium_init() < 0) std::abort();
    unsigned hw = std::thread::hardware_concurrency();
    d->threads = hw > 2 ? hw - 2 : 1;   // leave room for Basecamp and the game loop
}

BubbleCoreImpl::~BubbleCoreImpl() {
    if (d->live) d->live->shutdown();
    if (d->board) {
        { std::lock_guard<std::mutex> l(d->board->m); d->board->stop = true; }
        d->board->cv.notify_all();
        d->board->sub.cancel();
        if (d->board->worker.joinable()) d->board->worker.join();
    }
    d->rwStop = true;
    if (d->rwThread.joinable()) d->rwThread.join();
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

static json lastRln(BubbleCorePrivate* d);

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
    const bool endless = opts.is_object() && opts.value("mode", std::string()) == "endless";
    if (endless) {
        // Endless games open once this Basecamp has an RLN membership: their scores are posted with it.
        const json r = lastRln(d);
        const bool member = r.is_object() && r.value("membership", json::object()).value("state", std::string()) == "active";
        const char* any = std::getenv("BUBBLE_ENDLESS_ANY");
        if (!member && !(any && *any)) {
            g->shutdown();
            return json{{"ok", false}, {"error", "Endless mode opens once your Basecamp has an RLN membership: win a game and claim one first."}}.dump();
        }
        g->params = scoreParams();
        g->pow = scorePow();
    } else {
        g->params = defaultParams();
        g->pow = defaultPow();
    }
    g->game = std::make_unique<Game>(g->seedHex, g->params);
    g->startedMs = nowMs();
    for (unsigned i = 0; i < d->threads; i++) g->workers.emplace_back([g] { g->digest(); });
    g->loop = std::thread([g] { g->run(); });
    d->live = g;
    json beacon = g->claimable ? json{{"id", g->beacon.id}, {"hash", hex(g->beacon.hash, 32)}, {"ts", g->beacon.tsMs}} : json();
    return json{{"ok", true}, {"gameId", g->id}, {"mode", endless ? "endless" : "claim"}, {"params", g->params.toJson()}, {"pow", g->pow.toJson()},
                {"chunks", g->params.chunksNeeded()}, {"threads", d->threads}, {"claimable", g->claimable},
                {"beacon", beacon}, {"why", g->beaconWhy}}.dump();
}

std::string BubbleCoreImpl::frame(const std::string& gameId, int64_t dx, int64_t dy, const std::string& viewJson) {
    auto g = d->live;
    if (!g || g->id != gameId) return json{{"ok", false}, {"error", "no such game"}}.dump();
    json view = json::parse(viewJson, nullptr, false);
    double w = 800, h = 600;
    bool boost = false;
    if (view.is_object()) {
        if (view.contains("boost") && view["boost"].is_boolean()) boost = view["boost"].get<bool>();
        if (view.contains("w") && view["w"].is_number()) w = std::clamp(view["w"].get<double>(), 100.0, 8000.0);
        if (view.contains("h") && view["h"].is_number()) h = std::clamp(view["h"].get<double>(), 100.0, 8000.0);
    }
    std::lock_guard<std::mutex> l(g->m);
    g->lastFrameMs = nowMs();
    const Game& gm = *g->game;
    const int64_t mm = gm.mass();
    // The zoom follows the size, eased over ~0.4 s, so eating a bubble doesn't jolt the picture.
    const double target = std::min(1.4, h / (700.0 + 7.0 * (double)radius(mm) / kU));
    const int64_t nowF = nowMs();
    if (g->viewScale <= 0 || nowF - g->viewScaleMs > 2000) g->viewScale = target;
    else g->viewScale += (target - g->viewScale) * (1.0 - std::exp(-(double)(nowF - g->viewScaleMs) / 400.0));
    g->viewScaleMs = nowF;
    const double scale = g->viewScale;
    const double s = scale / kU, cx = w / 2 - gm.me.x * s, cy = h / 2 - gm.me.y * s;
    const double rMe = radius(mm) * s;

    // Steering: towards the pointer; near the middle of the bubble = stand still; the further the
    // pointer, the faster (four steps over max(2 radii, 160 px)); boost while the view asks for it.
    if (!gm.over) {
        const double dist = std::sqrt((double)(dx * dx + dy * dy));
        if (dist < rMe * 0.5) g->dir = makeInput(kStop, 0, false);
        else {
            const double frac = (dist - rMe * 0.5) / std::max(2.0 * rMe, 160.0);
            const int slower = 3 - std::min(3, (int)(frac * 4.0));
            g->dir = makeInput(dirOf(dx * 1000, dy * 1000), slower, boost);
        }
    }

    json bots = json::array(), pellets = json::array();
    for (size_t j = 0; j < gm.bots.size(); j++) {
        const Bot& b = gm.bots[j];
        const double r = radius(b.mass) * s, x = cx + b.x * s, y = cy + b.y * s;
        if (x < -r || y < -r || x > w + r || y > h + r) continue;
        bots.push_back({std::lround(x), std::lround(y), std::lround(std::max(1.0, r)), (int)j, b.hunt > 0 ? 2 : (b.mass * 10 >= mm * 11 ? 1 : 0)});   // 2: hunting you
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
               {"digesting", waiting}, {"dotFeeds", dotQuarters(mm) * 25},
               {"boost", gm.me.boost > 0 ? "on" : gm.me.boost < 0 ? "rest" : (gm.me.stomach >= kBoostMin ? "ready" : "empty")},
               {"slowedBy", std::min(kFullSlowMax, mm ? gm.me.stomach * kFullSlow / mm : 0)},
               {"speed", 4 - (g->dir % kBoostBit) / kDirs1}};   // % slowed by a full stomach; speed in quarters   // % of a dot that still feeds you
    if (g->params.endless) { me["score"] = gm.me.body; me["endless"] = true; }
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
    // The claim's proofs are the last life's: chunk i's nonce is the last one the record has for i.
    std::vector<uint32_t> nonceOf(g->game->digested.size(), 0);
    std::vector<bool> seen(g->game->digested.size(), false);
    for (const auto& [t, chunk, nonce] : g->rec.digests)
        if (chunk >= 0 && (size_t)chunk < nonceOf.size()) { nonceOf[(size_t)chunk] = nonce; seen[(size_t)chunk] = true; }
    for (size_t i = 0; i < g->game->digested.size(); i++) {
        if (!seen[i]) return json{{"error", "the record and the game disagree"}}.dump();
        c.chunks.push_back({(uint32_t)g->game->digested[i].tick, g->game->digested[i].hash, nonceOf[i]});
    }
    const std::string audience = args.contains("audience") && args["audience"].is_string() ? args["audience"].get<std::string>() : "";
    unsigned char digest[32];
    bindingDigest(req["domain"], kAuthType, act, audience, encodeClaim(c, false), digest);
    crypto_sign_detached(c.sig, nullptr, digest, 32, g->sk);
    const auto bytes = encodeClaim(c, true);
    return json{{"payload_hex", hex(bytes.data(), bytes.size())}}.dump();
}

// The RLN module that holds Delivery's keystore: liblogos_rln_module (BUBBLE_RLN_MODULE overrides).
// ── What the player's Basecamp waits on before a claim can land: liblogos_lez_rln_module's wallet
// syncing the LEZ testnet (a first start reads every block, ~15 min), then the membership itself.
// The wallet saves its last synced block to wallet-home/storage.json after every block, in the
// module's data dir next to ours (<data>/<module>/<instance>/), so the progress is a file read.
namespace {

int64_t walletSyncedBlock(const std::string& ourPersistPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (ourPersistPath.empty()) return -1;
    const fs::path dir = fs::path(ourPersistPath).parent_path().parent_path() / "liblogos_lez_rln_module";
    fs::path best;
    fs::file_time_type bestT{};
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path f = it->path() / "wallet-home" / "storage.json";
        auto t = fs::last_write_time(f, ec);
        if (ec) { ec.clear(); continue; }
        if (best.empty() || t > bestT) { best = f; bestT = t; }
    }
    if (best.empty()) return -1;
    std::ifstream in(best);
    json j = json::parse(in, nullptr, false);   // read while the wallet rewrites it: a torn read is just skipped
    if (!j.is_object() || !j.contains("last_synced_block") || !j["last_synced_block"].is_number_integer()) return -1;
    return j["last_synced_block"].get<int64_t>();
}

// The wallet module's home (the newest instance's), and whether it is still bound to an old testnet
// sequencer: a home made under an earlier "testnet" entry keeps that entry's address after the
// network table moves testnet elsewhere, and the wallet then never opens (logos-rln-lez-module#1).
std::filesystem::path walletHome(const std::string& ourPersistPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (ourPersistPath.empty()) return {};
    const fs::path dir = fs::path(ourPersistPath).parent_path().parent_path() / "liblogos_lez_rln_module";
    fs::path best;
    fs::file_time_type bestT{};
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path h = it->path() / "wallet-home";
        auto t = fs::last_write_time(h / "wallet_config.json", ec);
        if (ec) { ec.clear(); continue; }
        if (best.empty() || t > bestT) { best = h; bestT = t; }
    }
    return best;
}

constexpr const char* kTestnetSequencerHost = "testnet.lez.logos.co";

std::string staleSequencer(const std::filesystem::path& home) {
    if (home.empty()) return "";
    std::ifstream nin(home / "network.json"), cin(home / "wallet_config.json");
    const json net = json::parse(nin, nullptr, false), cfg = json::parse(cin, nullptr, false);
    // Only a home the module bound from its own network table (not one an operator configured).
    if (!net.is_object() || net.value("network", std::string()) != "testnet" || net.value("source", std::string()) != "table") return "";
    if (!cfg.is_object()) return "";
    std::string addr = cfg.value("sequencer_addr", std::string());
    if (addr.empty() && cfg.contains("sequencer") && cfg["sequencer"].is_object()) addr = cfg["sequencer"].value("sequencer_addr", std::string());
    if (addr.empty() || addr.find(kTestnetSequencerHost) != std::string::npos) return "";
    return addr;
}

std::string minutesText(int64_t sec) {
    if (sec < 30) return "a few seconds";
    if (sec < 90) return "about a minute";
    return "about " + std::to_string((sec + 30) / 60) + " min";
}

}  // namespace

// One reading: the wallet, its sync progress and (once it is ready) the membership.
static std::string rlnModuleName();

struct RlnReading {
    std::deque<std::pair<int64_t, int64_t>> samples;   // (ms, synced block): the sync rate
    int64_t head = -1, headAtMs = 0, memAtMs = 0;
    int64_t lastSynced = -1, movedAtMs = 0;   // when the synced block last changed
    json membership;
};

static json readRln(BubbleCorePrivate* d, RlnReading& r) {
    const GifterDefaults gd = gifterDefaults();
    logos::LpClient lez("liblogos_lez_rln_module", "bubble_core");
    logos::CallError err;
    json w = parseReply(lez.invoke("wallet_status", json::array(), &err, 10000));
    json wallet = {{"state", "unavailable"}, {"detail", err.message}};
    if (w.is_object() && w.contains("state")) wallet = {{"state", w.value("state", std::string())}, {"detail", w.value("detail", std::string())}};
    // The wallet starts syncing only once a consumer names its network (use_network). Delivery's RLN
    // module does that at start, but if the wallet wasn't ready to answer then, nothing asks again
    // and it waits for ever. Name it ourselves (the same network; repeating it is a no-op), so the
    // order Bubble and Chat start in doesn't matter.
    if (w.is_object() && w.value("state", std::string()) == "pending" && w.value("network", std::string()).empty()) {
        logos::CallError nerr;
        json n = parseReply(lez.invoke("use_network", json::array({gd.network}), &nerr, 15000));
        if (n.is_object()) wallet["network"] = n;
    }
    const int64_t now = nowMs();
    std::string persist;
    { std::lock_guard<std::mutex> l(d->rwMu); persist = d->persistPath; }
    int64_t synced = walletSyncedBlock(persist);
    // A torn read (the wallet rewrites the file after every block) keeps the last good number.
    if (synced < 0 && !r.samples.empty() && now - r.samples.back().first < 30000) synced = r.samples.back().second;
    if (wallet["state"] != "unavailable" && (r.head < 0 || now - r.headAtMs > 30000)) {
        logos::CallError ferr;
        json f = parseReply(lez.invoke("get_fee_state", json::array(), &ferr, 15000));
        if (f.is_object() && f.contains("height") && f["height"].is_number_integer()) { r.head = f["height"].get<int64_t>(); r.headAtMs = now; }
    }
    if (synced >= 0) {
        wallet["synced"] = synced;
        if (r.samples.empty() || r.samples.back().second != synced || now - r.samples.back().first > 5000) r.samples.push_back({now, synced});
        while (r.samples.size() > 2 && now - r.samples.front().first > 90000) r.samples.pop_front();
    }
    const int64_t head = std::max<int64_t>(r.head, synced);
    if (head > 0) wallet["head"] = head;
    const bool ready = wallet["state"] == "ready";
    if (!ready && synced >= 0 && head > 0 && r.samples.size() >= 2) {
        const auto& a = r.samples.front();
        const auto& b = r.samples.back();
        if (b.first > a.first && b.second > a.second) {
            const double perSec = double(b.second - a.second) * 1000.0 / double(b.first - a.first);
            wallet["blocksPerSecond"] = std::round(perSec * 10) / 10;
            wallet["etaSeconds"] = int64_t(double(head - synced) / perSec);
        }
    }
    // The membership, once the wallet can read the registry (get_membership_state reads the chain).
    if (ready && (r.memAtMs == 0 || now - r.memAtMs > 15000)) {
        r.memAtMs = now;
        const std::string configHex = b58ToHex32(gd.registry);
        if (!configHex.empty()) {
            logos::LpClient rln(rlnModuleName(), "bubble_core");
            logos::CallError merr;
            json st = parseReply(rln.invoke("get_membership_state",
                json::array({"logos:" + gd.network + ":" + configHex, std::string(kDeliveryRlnIdentifier)}), &merr, 30000));
            if (st.is_object() && st.contains("state")) {
                r.membership = {{"state", st.value("state", std::string())}};
                if (st.contains("leaf_index") && !st["leaf_index"].is_null()) r.membership["leaf"] = st["leaf_index"];
            } else if (st.is_object() && st.contains("error") && st["error"].is_object()) {
                r.membership = {{"state", "error"}, {"detail", st["error"].value("kind", std::string())}};
            }
        }
    }
    // In words, for the view.
    std::string text;
    const std::string state = wallet["state"];
    // Even a "ready" wallet is no use on an old server: its chain reads go there.
    const std::string stale = staleSequencer(walletHome(persist));
    if (!stale.empty()) {
        wallet["staleSequencer"] = stale;
        text = "Your RLN wallet is still set up for an old LEZ testnet server (" + stale + "), so it can't open. "
               "Reset it with the button below and restart Basecamp; your membership isn't affected.";
    } else if (state == "unavailable") text = "Basecamp's RLN wallet module isn't answering yet. If this stays, restart Basecamp.";
    else if (state == "failed") text = "Basecamp's RLN wallet failed to start: " + wallet.value("detail", std::string());
    else if (state == "pending" && synced >= 0 && head > 0) {
        const int pct = int(std::min<int64_t>(100, synced * 100 / head));
        if (synced != r.lastSynced) { r.lastSynced = synced; r.movedAtMs = now; }
        const int64_t stillFor = (now - r.movedAtMs) / 1000;
        // The file on disk can say "all blocks" while the wallet module still isn't ready (it keeps
        // retrying to open the wallet, e.g. when it can't reach the sequencer): then the module's own
        // reason is what the player is waiting on, not a progress bar.
        const std::string detail = wallet.value("detail", std::string());
        if ((pct >= 100 || stillFor >= 90) && !detail.empty()) {
            text = "Your RLN wallet has the LEZ testnet up to block " + std::to_string(synced) + ", but it isn't ready yet: "
                 + detail + "." + (stillFor >= 180 ? " If this doesn't change in a few minutes, restart Basecamp." : "") + " A claim waits for it.";
            wallet["stuckSeconds"] = stillFor;
        } else {
            text = "Your RLN wallet is syncing the LEZ testnet: block " + std::to_string(synced) + " of " + std::to_string(head)
                 + " (" + std::to_string(pct) + " %)" + (wallet.contains("etaSeconds") ? ", " + minutesText(wallet["etaSeconds"].get<int64_t>()) + " left." : ".")
                 + " A claim waits for it.";
        }
        wallet["percent"] = pct;
    }
    else if (state == "pending") text = "Your RLN wallet is starting (" + wallet.value("detail", std::string("opening the wallet")) + "). A first start syncs the LEZ testnet, about 15 minutes.";
    else if (r.membership.is_object() && r.membership.value("state", std::string()) == "active")
        text = "Your Basecamp has RLN membership #" + (r.membership.contains("leaf") ? r.membership["leaf"].dump() : std::string("?")) + ": Chat can send.";
    else if (r.membership.is_object() && r.membership.value("state", std::string()) == "pending")
        text = "Your RLN wallet is synced. Your membership is registered and waiting for the chain to confirm it.";
    else text = "Your RLN wallet is synced: a claim goes straight to the gifter.";
    return {{"ok", true}, {"wallet", wallet}, {"membership", r.membership.is_null() ? json::object() : r.membership}, {"text", text}};
}

static void ensureRlnWatch(BubbleCorePrivate* d) {
    std::lock_guard<std::mutex> l(d->rwMu);
    d->rwAskedMs = nowMs();
    if (d->rwThread.joinable()) return;
    d->rwThread = std::thread([d] {
        RlnReading r;
        while (!d->rwStop) {
            int64_t asked;
            { std::lock_guard<std::mutex> l(d->rwMu); asked = d->rwAskedMs; }
            if (nowMs() - asked > 60000) { std::this_thread::sleep_for(std::chrono::milliseconds(250)); continue; }
            json j = readRln(d, r);
            { std::lock_guard<std::mutex> l(d->rwMu); d->rwLast = j; }
            for (int i = 0; i < 12 && !d->rwStop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });
}

static json lastRln(BubbleCorePrivate* d) {
    std::lock_guard<std::mutex> l(d->rwMu);
    return d->rwLast;
}

// ── The leaderboard (see ScoreBoard)
namespace {

// Delivery hands bytes over as {"_bytes": base64} (padded or not), a byte array or a string.
bool payloadBytes(const json& p, std::vector<unsigned char>& out) {
    out.clear();
    if (p.is_object() && p.contains("_bytes") && p["_bytes"].is_string()) {
        const std::string& b = p["_bytes"].get_ref<const std::string&>();
        for (int variant : {sodium_base64_VARIANT_ORIGINAL, sodium_base64_VARIANT_ORIGINAL_NO_PADDING,
                            sodium_base64_VARIANT_URLSAFE, sodium_base64_VARIANT_URLSAFE_NO_PADDING}) {
            out.assign(b.size(), 0);
            size_t len = 0;
            if (sodium_base642bin(out.data(), out.size(), b.c_str(), b.size(), nullptr, &len, nullptr, variant) == 0) { out.resize(len); return true; }
        }
        return false;
    }
    if (p.is_array()) {
        for (const auto& v : p) { if (!v.is_number_integer()) return false; out.push_back((unsigned char)v.get<int>()); }
        return true;
    }
    if (p.is_string()) { const std::string& t = p.get_ref<const std::string&>(); out.assign(t.begin(), t.end()); return true; }
    return false;
}

// A Delivery reply: {"success", "value", "error"} (or a bare value). ok + the value or the error.
bool deliveryOk(const json& r, json& value, std::string& error) {
    json v = parseReply(r);
    if (v.is_object() && v.contains("success")) {
        value = v.value("value", json());
        error = v["error"].is_string() ? v["error"].get<std::string>() : (v.value("success", false) ? "" : v.dump());
        return v.value("success", false);
    }
    value = v;
    error = v.is_null() ? "no answer" : "";
    return !v.is_null();
}

void boardSave(ScoreBoard* b) {
    if (b->persist.empty()) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(b->persist, ec);
    json e = json::array(), m = json::array();
    for (const auto& [id, v] : b->entries) e.push_back(v);
    for (const auto& [id, v] : b->mine) m.push_back(v);
    for (auto& [name, data] : std::vector<std::pair<std::string, json>>{{"leaderboard.json", e}, {"my-scores.json", m}}) {
        const fs::path tmp = fs::path(b->persist) / (name + ".tmp");
        { std::ofstream o(tmp); o << data.dump(); }
        fs::rename(tmp, fs::path(b->persist) / name, ec);
    }
}

void boardLoad(ScoreBoard* b) {
    if (b->persist.empty()) return;
    namespace fs = std::filesystem;
    auto read = [&](const char* name) { std::ifstream in(fs::path(b->persist) / name); return json::parse(in, nullptr, false); };
    json e = read("leaderboard.json"), m = read("my-scores.json");
    if (e.is_array()) for (const auto& v : e) if (v.is_object() && v.contains("id")) b->entries[v["id"].get<std::string>()] = v;
    if (m.is_array()) for (const auto& v : m) if (v.is_object() && v.contains("id")) b->mine[v["id"].get<std::string>()] = v;
}

// Keeps the board to the best 200 games.
void boardTrim(ScoreBoard* b) {
    while (b->entries.size() > 200) {
        auto worst = b->entries.begin();
        for (auto it = b->entries.begin(); it != b->entries.end(); ++it)
            if (it->second.value("score", 0) < worst->second.value("score", 0)) worst = it;
        if (worst->second.value("mine", false)) break;
        b->entries.erase(worst);
    }
}

// Brings up this Basecamp's Delivery node the way Chat does (createNode, or join the node that
// exists; a second start on a running node is harmless), and subscribes to the scores.
bool boardUp(ScoreBoard* b) {
    if (!b->delivery) b->delivery = std::make_unique<logos::LpClient>("delivery_module", "bubble_core");
    logos::CallError err;
    json value;
    std::string error;
    const json cfg = {{"mode", "Core"}, {"preset", "logos.test"}, {"messagingOverrides", {{"logLevel", "ERROR"}}}};
    if (!deliveryOk(b->delivery->invoke("createNode", json::array({cfg.dump()}), &err, 30000), value, error)) {
        json v2; std::string e2;
        if (!deliveryOk(b->delivery->invoke("getNodeInfo", json::array({"Version"}), &err, 15000), v2, e2)) {
            std::lock_guard<std::mutex> l(b->m);
            b->status = "error: Logos Delivery isn't available (" + (error.empty() ? err.message : error) + ")";
            return false;
        }
    }
    deliveryOk(b->delivery->invoke("start", json::array(), &err, 30000), value, error);
    if (!b->sub.valid()) {
        std::weak_ptr<int> dummy;
        ScoreBoard* bp = b;
        b->sub = b->delivery->subscribe("messageReceived", [bp](json a) {
            // An event callback: only queue it (no module calls from here).
            if (!a.is_array() || a.size() < 3) return;
            if (!a[1].is_string() || a[1].get<std::string>() != kScoreTopic) return;
            std::vector<unsigned char> bytes;
            if (!payloadBytes(a[2], bytes)) return;
            const std::string hash = a[0].is_string() ? a[0].get<std::string>() : std::string();
            {
                std::lock_guard<std::mutex> l(bp->m);
                if (bp->inbox.size() < 500) bp->inbox.emplace_back(hash, std::move(bytes));
            }
            bp->cv.notify_all();
        });
    }
    if (!deliveryOk(b->delivery->invoke("subscribe", json::array({kScoreTopic}), &err, 30000), value, error)) {
        std::lock_guard<std::mutex> l(b->m);
        b->status = "joining";   // the node is still starting: try again shortly
        return false;
    }
    std::lock_guard<std::mutex> l(b->m);
    b->status = "online";
    b->up = true;
    return true;
}

bool boardSend(ScoreBoard* b, const json& msg, std::string& why) {
    const std::string body = msg.dump();
    logos::CallError err;
    json value;
    const std::vector<unsigned char> bytes(body.begin(), body.end());
    const bool ok = deliveryOk(b->delivery->invoke("send", json::array({kScoreTopic, json{{"_bytes", toBase64(bytes)}}}), &err, 30000), value, why);
    if (!ok && why.empty()) why = err.message;
    return ok;
}

void boardRun(ScoreBoard* b) {
    {
        std::lock_guard<std::mutex> l(b->m);
        boardLoad(b);
    }
    const std::string lezUrl = defaultLezUrl();
    BeaconLookup lookup = [lezUrl](uint64_t id, Beacon& out, std::string& why, bool& unreachable) {
        return beaconById(lezUrl, id, out, why, unreachable);
    };
    int64_t nextUpMs = 0;
    while (!b->stop) {
        bool up;
        { std::lock_guard<std::mutex> l(b->m); up = b->up; }
        if (!up && nowMs() >= nextUpMs) {
            if (!boardUp(b)) nextUpMs = nowMs() + 10000;
            else {
                // Online: send what waited, and re-post the best right away (it seeds the Store).
                std::lock_guard<std::mutex> l(b->m);
                b->lastRepostMs = 0;
            }
        }
        std::pair<std::string, std::vector<unsigned char>> item;
        bool have = false;
        {
            std::unique_lock<std::mutex> l(b->m);
            if (b->inbox.empty()) b->cv.wait_for(l, std::chrono::seconds(5));
            if (b->stop) break;
            while (!b->inbox.empty()) {
                item = std::move(b->inbox.front());
                b->inbox.pop_front();
                if (!item.first.empty() && !b->seen.insert(item.first).second) continue;   // a duplicate delivery
                have = true;
                break;
            }
        }
        if (have) {
            json msg = json::parse(item.second.begin(), item.second.end(), nullptr, false);
            json v;
            try { v = verifyScore(msg, lookup); } catch (const std::exception& e) { v = {{"ok", false}, {"reason", e.what()}}; }
            std::lock_guard<std::mutex> l(b->m);
            if (v.value("ok", false)) {
                const std::string id = v["id"];
                const bool isMine = b->mine.count(id) > 0;
                json e = {{"id", id}, {"score", v["score"]}, {"ticks", v["ticks"]}, {"ended", v["ended"]}, {"when", v["when"]}, {"mine", isMine}};
                if (!b->entries.count(id)) { b->entries[id] = e; b->dirty = true; boardTrim(b); }
                b->checked++;
            } else {
                b->refused++;
                if (v.value("retry", false) && !item.first.empty()) b->seen.erase(item.first);   // the block lookup failed: a later copy may pass
            }
            continue;
        }
        // Outbox, and the periodic re-post of this player's best (every 30 min).
        std::deque<json> send;
        {
            std::lock_guard<std::mutex> l(b->m);
            if (!b->up) continue;
            send.swap(b->outbox);
            if (!b->mine.empty() && nowMs() - b->lastRepostMs > 30 * 60 * 1000) {
                const json* best = nullptr;
                for (const auto& [id, mm] : b->mine) if (!best || mm.value("score", 0) > best->value("score", 0)) best = &mm;
                if (best) send.push_back((*best)["msg"]);
                b->lastRepostMs = nowMs();
            }
        }
        for (const auto& m : send) {
            std::string why;
            if (!boardSend(b, m, why)) {
                std::lock_guard<std::mutex> l(b->m);
                b->status = "error: sending a score failed (" + why + ")";
                b->outbox.push_back(m);
                b->up = false;   // bring Delivery up again before the next try
                nextUpMs = nowMs() + 15000;
                break;
            }
        }
        std::lock_guard<std::mutex> l(b->m);
        if (b->dirty) { boardSave(b); b->dirty = false; }
    }
    std::lock_guard<std::mutex> l(b->m);
    boardSave(b);
}

}  // namespace

static std::shared_ptr<ScoreBoard> ensureBoard(BubbleCorePrivate* d, const std::string& persistRoot) {
    if (d->board) return d->board;
    auto b = std::make_shared<ScoreBoard>();
    b->persist = persistRoot.empty() ? std::string() : (std::filesystem::path(persistRoot) / "scores").string();
    ScoreBoard* bp = b.get();
    b->worker = std::thread([bp] { boardRun(bp); });
    d->board = b;
    return b;
}

static std::string rlnModuleName() {
    const char* e = std::getenv("BUBBLE_RLN_MODULE");
    return e && *e ? e : "liblogos_rln_module";
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
    // liblogos_rln_module 0.12 (what Delivery 0.3.3 uses) asks its gifter through a module named
    // rln_gifter_module (Bubble ships a bridge by that name onto the membership allocation
    // protocol); it needs the gifter's peer and address, and the auth vector to produce.
    json opts = json::array();
    auto kv = [&](const std::string& k, const std::string& v) { opts.push_back({{"key", k}, {"value", v}}); };
    kv("delegated", "true");
    kv("rate_limit", std::to_string(o.value("rate", 100)));
    kv("gifter_peer_id", o.value("allocatorPeerId", gd.peer));
    kv("gifter_multiaddr", o.value("allocatorMultiaddr", gd.addr));
    kv("auth_type", kAuthType);
    kv("auth_provider", me);
    kv("auth_args", json{{"gameId", g->id}, {"audience", o.value("audience", std::string())},
                         {"config", o["config"]}}.dump());
    {
        std::lock_guard<std::mutex> l(g->m);
        g->claimResult = json{{"demoIdentity", false}, {"registryId", registryId}};
    }
    if (!d->rln) d->rln = std::make_unique<logos::LpClient>(rlnModuleName(), me);
    if (!d->libp2p) d->libp2p = std::make_unique<logos::LpClient>("libp2p_module", me);
    std::weak_ptr<Live> weak = g;
    logos::LpClient* rln = d->rln.get();
    logos::LpClient* p2p = d->libp2p.get();
    d->claimThreads.erase(std::remove_if(d->claimThreads.begin(), d->claimThreads.end(), [](std::thread& t) {
        return !t.joinable(); }), d->claimThreads.end());
    const json reqConfig = o["config"];
    const int64_t rate = o.value("rate", 100);
    const std::string meName = me, gameIdC = g->id, audienceC = o.value("audience", std::string());
    const std::string peerC = o.value("allocatorPeerId", gd.peer), addrC = o.value("allocatorMultiaddr", gd.addr);
    d->claimThreads.emplace_back([d, weak, rln, p2p, registryId, rlnId, opts, reqConfig, rate, meName, gameIdC, audienceC, peerC, addrC] {
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
        auto stage = [&](const std::string& st) {
            if (auto g = weak.lock()) { std::lock_guard<std::mutex> l(g->m); g->claimResult["stage"] = st; }
        };
        // The registry is read through liblogos_lez_rln_module's wallet: a fresh one syncs the
        // testnet first (~15 min on a first start), and until then every registry read stalls.
        // So the claim waits for it (rlnStatus says how far it is), for up to an hour.
        {
            logos::LpClient lez("liblogos_lez_rln_module", "bubble_core");
            int silent = 0;
            for (int i = 0;; i++) {
                if (weak.expired() || d->rwStop) return;
                logos::CallError werr;
                json w = parseReply(lez.invoke("wallet_status", json::array(), &werr, 20000));
                if (!w.is_object() && ++silent < 3) { std::this_thread::sleep_for(std::chrono::seconds(3)); continue; }
                if (!w.is_object()) {
                    finish("failed", json::object(), "Basecamp's RLN wallet isn't running. Install and open Chat (Logos Messaging) first, then claim again.");
                    return;
                }
                silent = 0;
                if (w.value("ready", false)) break;
                if (w.value("state", std::string()) == "failed") {
                    finish("failed", w, "Basecamp's RLN wallet failed to start: " + w.value("detail", std::string()));
                    return;
                }
                if (i >= 1200) {
                    finish("failed", w, "Your RLN wallet still hasn't synced after an hour. Try again later.");
                    return;
                }
                stage("wallet");
                ensureRlnWatch(d);
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }
        }
        stage("gifter");
        // A fresh keystore unlocks itself (rln_module's auto-unlock); a user-password one must be unlocked by its owner.
        json u = parseReply(rln->invoke("unlock_keystore_auto", json::array(), &err, 30000));

        // At most ONE record per registry in Delivery's keystore: liblogos_rln_module 0.12 counts a
        // failed record as a candidate for the scope, so a second record makes every
        // get_membership_state "ambiguous" and Delivery stops sending. So: a usable record is
        // done, a pending one is waited for, a failed one is re-registered with its own
        // commitment (its next state read then heals it to active), and only no record at all
        // makes a new identity.
        json mine = json::array();
        {
            json all = parseReply(rln->invoke("get_memberships", json::array({registryId}), &err, 30000));
            if (all.is_object() && all["memberships"].is_array()) mine = all["memberships"];
            else if (!all.is_object()) {
                // No answer at all: the keystore module isn't running (Bubble depends on it, so
                // Basecamp should have loaded it; a restart of Basecamp loads it again).
                finish("failed", json::object(), "Basecamp's RLN keystore (liblogos_rln_module) didn't answer"
                       + (err.message.empty() ? std::string() : " (" + err.message + ")") + ". Restart Basecamp and claim again.");
                return;
            }
        }
        auto stateOf = [](const json& m) { return m.value("state", std::string()); };
        bool wait = false;
        for (const auto& m : mine) {
            const std::string st = stateOf(m);
            if (st == "active" || st == "grace_period") {
                finish("granted", m, "Your Basecamp already has an RLN membership; this game wasn't needed for it.");
                return;
            }
            if (st == "pending") wait = true;
        }
        if (!wait && mine.size() > 1) {
            finish("failed", json{{"memberships", mine}}, "Your Basecamp's RLN keystore holds several failed memberships for this registry, which liblogos_rln_module 0.12 can't choose between. It needs a fresh keystore.");
            return;
        }
        if (!wait && mine.size() == 1) {
            // Re-register the one failed record's commitment directly through the gifter.
            const std::string idc = mine[0]["credential"].value("identity_commitment", std::string());
            json args = {{"config", reqConfig}, {"identityCommitment", idc}, {"rate", rate},
                         {"authType", kAuthType}, {"authProvider", meName},
                         {"authArgs", json{{"gameId", gameIdC}, {"audience", audienceC}}.dump()},
                         {"discover", false}, {"allocatorPeerId", peerC}, {"allocatorMultiaddr", addrC}};
            logos::LpClient gifter("rln_gifter_module", meName);
            json r = parseReply(gifter.invoke("request", json::array({args.dump()}), &err, 300000));
            const bool landed = r.is_object() && (r.contains("leaf_index") || r.value("pending", false));
            if (!landed) {
                const std::string why = r.is_object() ? r.value("message", r.value("error", r.dump())) : (err.message.empty() ? "no reply from rln_gifter_module" : err.message);
                const bool used = why.find("credential already used") != std::string::npos;
                finish(used ? "refused" : "failed", r.is_object() ? r : json::object(),
                       used ? "This game has already been claimed. Play another one to claim again." : why);
                return;
            }
        } else if (!wait) {
            json r = parseReply(rln->invoke("register_membership", json::array({registryId, rlnId, opts.dump()}), &err, 180000));
            // No answer in time is not a refusal: the registration may be under way; the poll below tells.
            const bool unanswered = !r.is_object() && err.code == "timeout";
            if (!unanswered && (!r.is_object() || r.contains("error"))) {
                std::string why = r.is_object() && r["error"].is_object() ? r["error"].value("message", r["error"].dump())
                                : r.is_object() ? r["error"].dump() : (err.message.empty() ? "no reply from rln_module" : err.message);
                if (u.is_object() && u.contains("error")) why += " (keystore: " + u["error"].dump() + ")";
                finish("failed", r.is_object() ? r : json::object(), "rln_module could not start the registration: " + why);
                return;
            }
        }
        stage("chain");
        // Poll the membership until the gifter's registration shows on chain (or fails).
        // Testnet blocks are slow: a registration can show up minutes after the gifter sent it, and
        // a record marked failed heals to active on the read after it lands, so only a refusal ends this.
        for (int i = 0; i < 300; i++) {
            if (weak.expired()) return;
            json st = parseReply(rln->invoke("get_membership_state", json::array({registryId, rlnId}), &err, 30000));
            const std::string state = st.is_object() ? st.value("state", std::string()) : "";
            if (state == "active") { finish("granted", st, ""); return; }
            if (st.is_object() && st.contains("error") && st["error"].is_object() && st["error"].value("kind", std::string()) == "ambiguous_selection") {
                finish("failed", st, "Your Basecamp's RLN keystore holds more than one membership for this registry, which liblogos_rln_module 0.12 can't choose between.");
                return;
            }
            if (state == "failed") {
                const std::string why = st.value("failed_reason", std::string("the registration failed"));
                const bool mayLand = why.find("may still land") != std::string::npos || why.find("confirmation_window") != std::string::npos
                                  || why.find("not on chain") != std::string::npos;
                if (mayLand && i < 299) { std::this_thread::sleep_for(std::chrono::seconds(3)); continue; }
                const bool used = why.find("credential already used") != std::string::npos || why.find("credential_used") != std::string::npos;
                finish(used ? "refused" : "failed", st, used ? "This game has already been claimed. Play another one to claim again." : why);
                return;
            }
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
        finish("failed", json::object(), "the membership did not become active within 15 minutes; it may still land (claim again to check)");
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

std::string BubbleCoreImpl::rlnStatus() {
    {
        std::lock_guard<std::mutex> l(d->rwMu);
        d->persistPath = instancePersistencePath();
    }
    ensureRlnWatch(d);
    json j = lastRln(d);
    if (!j.is_object()) return json{{"ok", true}, {"wallet", {{"state", "checking"}}}, {"membership", json::object()}, {"text", "Checking Basecamp's RLN wallet…"}}.dump();
    return j.dump();
}

std::string BubbleCoreImpl::rlnResetWallet() {
    // Only a wallet stuck on an old testnet sequencer: its home is moved aside (kept, renamed), and
    // the wallet module makes a fresh one for the current testnet on its next start.
    const auto home = walletHome(instancePersistencePath());
    const std::string stale = staleSequencer(home);
    if (stale.empty()) return json{{"ok", false}, {"error", "the RLN wallet isn't bound to an old testnet server; nothing to reset"}}.dump();
    std::error_code ec;
    const auto aside = home.parent_path() / ("wallet-home.old-" + std::to_string(wallMs()));
    std::filesystem::rename(home, aside, ec);
    if (ec) return json{{"ok", false}, {"error", "couldn't move " + home.string() + ": " + ec.message()}}.dump();
    return json{{"ok", true}, {"movedTo", aside.string()}, {"text", "Done. Restart Basecamp: the wallet then syncs the current LEZ testnet from scratch (about 15 minutes)."}}.dump();
}

std::string BubbleCoreImpl::finish(const std::string& gameId) {
    auto g = d->live;
    if (!g || g->id != gameId) return json{{"ok", false}, {"error", "no such game"}}.dump();
    std::lock_guard<std::mutex> l(g->m);
    if (!g->params.endless) return json{{"ok", false}, {"error", "only an endless game can be finished (give up a claim game instead)"}}.dump();
    if (!g->game->over) {
        // Ends here, as a replay of the record cut at this tick does: the score is the size now.
        g->game->over = true;
        g->game->why = "quit";
        g->rec.end = g->game->tick;
        g->jobs.clear();
    }
    return json{{"ok", true}}.dump();
}

std::string BubbleCoreImpl::publishScore(const std::string& gameId) {
    auto g = d->live;
    if (!g || g->id != gameId) return json{{"ok", false}, {"error", "no such game"}}.dump();
    json msg, res;
    {
        std::lock_guard<std::mutex> l(g->m);
        if (!g->params.endless) return json{{"ok", false}, {"error", "only endless games have scores"}}.dump();
        if (!g->claimable) return json{{"ok", false}, {"error", "a practice game (no LEZ block to seed it from) can't be published"}}.dump();
        if (g->checkState != "done" || !g->checkResult.value("ok", false))
            return json{{"ok", false}, {"error", "the game isn't over and checked yet"}}.dump();
        res = g->checkResult;
        msg = makeScore(g->beacon, g->pk, g->sk, g->rec, res.value("score", (int64_t)0), res.value("ticks", (int64_t)0),
                        res.value("ended", std::string()), wallMs());
    }
    if (msg.dump().size() > kScoreMaxBytes) return json{{"ok", false}, {"error", "this game is too long to post in one message"}}.dump();
    // The id a receiver will compute (score.h), so this score is known as ours on the board.
    BeaconLookup self = [g](uint64_t, Beacon& out, std::string&, bool&) { out = g->beacon; return true; };
    json v = verifyScore(msg, self, g->pow);
    if (!v.value("ok", false)) return json{{"ok", false}, {"error", "the score doesn't check: " + v.value("reason", std::string())}}.dump();
    auto b = ensureBoard(d, instancePersistencePath());
    const std::string id = v["id"];
    {
        std::lock_guard<std::mutex> l(b->m);
        b->mine[id] = {{"id", id}, {"score", v["score"]}, {"msg", msg}};
        b->entries[id] = {{"id", id}, {"score", v["score"]}, {"ticks", v["ticks"]}, {"ended", v["ended"]}, {"when", v["when"]}, {"mine", true}};
        b->outbox.push_back(msg);
        b->dirty = true;
    }
    b->cv.notify_all();
    return json{{"ok", true}, {"id", id}, {"score", v["score"]}}.dump();
}

std::string BubbleCoreImpl::leaderboard() {
    auto b = ensureBoard(d, instancePersistencePath());
    std::lock_guard<std::mutex> l(b->m);
    std::vector<json> all;
    for (const auto& [id, e] : b->entries) all.push_back(e);
    std::sort(all.begin(), all.end(), [](const json& a, const json& c) {
        if (a.value("score", 0) != c.value("score", 0)) return a.value("score", 0) > c.value("score", 0);
        return a.value("when", (int64_t)0) < c.value("when", (int64_t)0);
    });
    json top = json::array(), myBest;
    int rank = 0, myRank = 0;
    for (const auto& e : all) {
        rank++;
        if (rank <= 20) { json x = e; x["rank"] = rank; top.push_back(x); }
        if (e.value("mine", false) && !myRank) { myRank = rank; myBest = e; myBest["rank"] = rank; }
    }
    return json{{"ok", true}, {"status", b->status}, {"entries", top}, {"count", (int64_t)all.size()},
                {"myBest", myBest}, {"pending", (int64_t)(b->inbox.size() + b->outbox.size())},
                {"checked", b->checked}, {"refused", b->refused}}.dump();
}

std::string BubbleCoreImpl::claimStatus(const std::string& gameId) {
    auto g = d->live;
    if (!g || g->id != gameId) return json{{"ok", false}, {"error", "no such game"}}.dump();
    std::lock_guard<std::mutex> l(g->m);
    return json{{"ok", true}, {"state", g->claimState}, {"result", g->claimResult}}.dump();
}

