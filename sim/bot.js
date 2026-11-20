// A greedy bot: chase the best nearby food, run from anything bigger. Digestion is instant
// (as if the proof of work were free), so this measures the gameplay part alone.
// usage: node bot.js [games] [seedPrefix]
const S = require("../core/src/sim.js");
const crypto = require("crypto");

function decide(g) {
    const me = g.me, mm = S.mass(me), r = S.radius(mm);
    let fx = 0, fy = 0, threat = false;
    for (const b of g.bots) {
        const dx = b.x - me.x, dy = b.y - me.y, d = Math.abs(dx) + Math.abs(dy);
        if (b.mass * 10 >= mm * 11 && d < r * 6 + 300 * S.U) {
            const w = Math.max(1, 1e6 - d) * (1 + Math.floor(b.mass / mm)); fx -= Math.sign(dx) * w; fy -= Math.sign(dy) * w; threat = true;
        }
    }
    if (threat) {
        // Walls trap a runner: push away from any wall within 400 px.
        const m = 400 * S.U, W = S.WORLD;
        if (me.x < m) fx += (m - me.x) * 4000; if (W - me.x < m) fx -= (m - (W - me.x)) * 4000;
        if (me.y < m) fy += (m - me.y) * 4000; if (W - me.y < m) fy -= (m - (W - me.y)) * 4000;
        return S.dirOf(fx, fy);
    }
    let best = null, bestScore = -Infinity;
    for (const b of g.bots) {
        if (mm * 10 < b.mass * 11) continue;
        const d = Math.abs(b.x - me.x) + Math.abs(b.y - me.y), s = b.mass * 4000 - d;
        if (s > bestScore) { bestScore = s; best = b; }
    }
    for (const p of g.pellets) {
        const d = Math.abs(p.x - me.x) + Math.abs(p.y - me.y), s = 4000 - d;
        if (s > bestScore) { bestScore = s; best = p; }
    }
    return best ? S.dirOf(best.x - me.x, best.y - me.y) : S.STOP;
}

function play(seed, params) {
    const g = S.newGame(seed, params), rec = S.newRecorder();
    while (!g.over) {
        const dir = decide(g);
        const dig = [];
        while (g.me.stomach >= params.chunk * (dig.length + 1) && g.chunks + dig.length < S.chunksNeeded(params)) dig.push([g.chunks + dig.length, 0]);
        S.record(rec, g.tick, dir, dig);
        const err = S.step(g, dir, dig.map(d => d[0]));
        if (err) throw new Error(err);
    }
    rec.v = S.SIM_VERSION; rec.seed = seed; rec.end = g.tick;
    delete rec.lastDir;
    return { g, rec };
}

if (require.main === module) {
    const n = +(process.argv[2] || 20), params = S.defaultParams();
    let wins = 0, ticks = [], why = {};
    const t0 = Date.now();
    for (let i = 0; i < n; i++) {
        const seed = crypto.createHash("sha256").update((process.argv[3] || "s") + i).digest("hex");
        const { g, rec } = play(seed, params);
        why[g.why] = (why[g.why] || 0) + 1;
        if (g.won) {
            wins++; ticks.push(g.tick);
            const r = S.replay(seed, params, rec);
            if (!r.ok || r.hash !== S.stateHash(g)) throw new Error("replay differs: " + JSON.stringify(r));
        }
        if (i === 0) require("fs").writeFileSync(__dirname + "/sample-record.json", JSON.stringify({ seed, params, rec, hash: S.stateHash(g), won: g.won }));
    }
    ticks.sort((a, b) => a - b);
    const ms = Date.now() - t0;
    console.log(JSON.stringify({ games: n, wins, outcomes: why, medianTicks: ticks[ticks.length >> 1], medianSeconds: ticks.length ? (ticks[ticks.length >> 1] / 30).toFixed(0) : null, msPerGame: Math.round(ms / n) }));
}
module.exports = { play, decide };
