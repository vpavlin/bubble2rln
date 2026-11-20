// bubble-sim v1: the one and only copy of the game rules.
//
// It runs in three places and must give the same result in all of them:
//  - the Basecamp view (QML imports this file) to play;
//  - the core module (Qt's QJSEngine) to check a game before claiming;
//  - the allocator's verifier, to decide a claim.
// So: integers only. No Math.random, no Math.sin/cos/sqrt/atan2 (engines may round those
// differently), no Date, no object-key ordering tricks. + - * on integers below 2^53 and
// Math.trunc(a / b) are exact in every IEEE-754 engine, and Math.imul / >>> are specified bit
// for bit. Positions are in 1/16 px.

var SIM_VERSION = 1;

var COS = [1024,1019,1004,980,946,903,851,792,724,650,569,483,392,297,200,100,0,-100,-200,-297,-392,-483,-569,-650,-724,-792,-851,-903,-946,-980,-1004,-1019,-1024,-1019,-1004,-980,-946,-903,-851,-792,-724,-650,-569,-483,-392,-297,-200,-100,0,100,200,297,392,483,569,650,724,792,851,903,946,980,1004,1019];
var SIN = [0,100,200,297,392,483,569,650,724,792,851,903,946,980,1004,1019,1024,1019,1004,980,946,903,851,792,724,650,569,483,392,297,200,100,0,-100,-200,-297,-392,-483,-569,-650,-724,-792,-851,-903,-946,-980,-1004,-1019,-1024,-1019,-1004,-980,-946,-903,-851,-792,-724,-650,-569,-483,-392,-297,-200,-100];
var DIRS = 64;
var STOP = 64;          // input value: stand still

var U = 16;             // units per px
var WORLD = 2400 * U;   // square world side
var PELLETS = 320;
var BOTS = 24;
var RETARGET = 12;      // ticks between a bot's decisions
var SIGHT = 450 * U;    // how far a bot looks
var SHIELD = 90;        // ticks a respawned player can't be eaten
var CELL = 128 * U;     // pellet grid cell (only a speed-up: results are as without it)
var CELLS = Math.trunc((WORLD + CELL - 1) / CELL) + 1;

// Defaults; the allocator's config decides the real ones and the claim must match them.
function defaultParams() {
    return { start: 10, goal: 200, chunk: 5, minTicks: 1800, maxTicks: 36000 };
}

// ---- integer helpers -------------------------------------------------------------------------

function isqrt(n) {                     // floor(sqrt(n)) for 0 <= n < 2^52, integer Newton
    if (n < 2) return n;
    var x = n, y = Math.trunc((x + 1) / 2);
    while (y < x) { x = y; y = Math.trunc((x + Math.trunc(n / x)) / 2); }
    return x;
}

// Both are pure functions of an integer mass, memoised (a speed-up only).
var RADIUS = [], SPEED = [];
function radius(mass) {                                          // 4 px * sqrt(mass), in units
    var r = RADIUS[mass];
    if (r === undefined) { r = isqrt(mass * 4096); if (mass < 100000) RADIUS[mass] = r; }
    return r;
}

function speed(mass) {                                           // units per tick
    var v = SPEED[mass];
    if (v === undefined) { v = Math.trunc(2400 / isqrt(mass * 16 + 64)); if (v < 6) v = 6; if (mass < 100000) SPEED[mass] = v; }
    return v;
}

// xoshiro128** seeded from 32 hex digits (or more; the first 32 are used).
function rngFromHex(hex) {
    var s = [0, 0, 0, 0];
    for (var i = 0; i < 4; i++) s[i] = parseInt(hex.substr(i * 8, 8), 16) >>> 0;
    if ((s[0] | s[1] | s[2] | s[3]) === 0) s[0] = 1;
    return { s: s };
}
function rotl(x, k) { return ((x << k) | (x >>> (32 - k))) >>> 0; }
function next32(r) {
    var s = r.s;
    var result = Math.imul(rotl(Math.imul(s[1], 5) >>> 0, 7), 9) >>> 0;
    var t = (s[1] << 9) >>> 0;
    s[2] = (s[2] ^ s[0]) >>> 0; s[3] = (s[3] ^ s[1]) >>> 0;
    s[1] = (s[1] ^ s[2]) >>> 0; s[0] = (s[0] ^ s[3]) >>> 0;
    s[2] = (s[2] ^ t) >>> 0;    s[3] = rotl(s[3], 11);
    return result;
}
function randInt(r, n) { return next32(r) % n; }                 // bias is irrelevant here

// The direction (0..63) closest to the vector (dx, dy).
function dirOf(dx, dy) {
    var best = 0, bestDot = -Infinity;
    for (var i = 0; i < DIRS; i++) {
        var d = COS[i] * dx + SIN[i] * dy;
        if (d > bestDot) { bestDot = d; best = i; }
    }
    return best;
}

// ---- the world -------------------------------------------------------------------------------

function newGame(seedHex, params) {
    var p = params || defaultParams();
    var g = {
        v: SIM_VERSION, p: p, rng: rngFromHex(seedHex), tick: 0,
        me: { x: WORLD / 2, y: WORLD / 2, body: p.start, stomach: 0, dir: STOP, shield: SHIELD }, deaths: 0,
        bots: [], pellets: [], chunks: 0, over: false, won: false, why: ""
    };
    for (var i = 0; i < PELLETS; i++) g.pellets.push(spawnPellet(g));
    g.grid = []; g.eatenBy = [];
    for (var c = 0; c < CELLS * CELLS; c++) g.grid.push([]);
    for (var k = 0; k < PELLETS; k++) { gridAdd(g, k); g.eatenBy.push(-1); }
    for (var j = 0; j < BOTS; j++) g.bots.push(spawnBot(g, j < 10 ? 4 + randInt(g.rng, 8) : (j < 20 ? 12 + randInt(g.rng, 48) : 60 + randInt(g.rng, 100))));
    return g;
}

function spawnPellet(g) {
    return { x: randInt(g.rng, WORLD), y: randInt(g.rng, WORLD) };
}

function spawnBot(g, mass) {
    // Not on top of the player.
    var x, y, tries = 0;
    do {
        x = randInt(g.rng, WORLD); y = randInt(g.rng, WORLD); tries++;
    } while (tries < 20 && Math.abs(x - g.me.x) + Math.abs(y - g.me.y) < 600 * U);
    return { x: x, y: y, mass: mass, dir: randInt(g.rng, DIRS), wait: randInt(g.rng, RETARGET) };
}

function cellOf(x, y) { return Math.trunc(y / CELL) * CELLS + Math.trunc(x / CELL); }
function gridAdd(g, j) { g.grid[cellOf(g.pellets[j].x, g.pellets[j].y)].push(j); }
function gridRemove(g, j) {
    var a = g.grid[cellOf(g.pellets[j].x, g.pellets[j].y)];
    for (var i = 0; i < a.length; i++) if (a[i] === j) { a[i] = a[a.length - 1]; a.pop(); return; }
}

// Mark the pellets inside the circle (x, y, r) as eaten by `who`, unless an earlier mover has.
function markPellets(g, x, y, r, who) {
    var c0 = Math.trunc(Math.max(0, x - r) / CELL), c1 = Math.trunc(Math.min(WORLD, x + r) / CELL);
    var r0 = Math.trunc(Math.max(0, y - r) / CELL), r1 = Math.trunc(Math.min(WORLD, y + r) / CELL);
    for (var row = r0; row <= r1; row++) for (var col = c0; col <= c1; col++) {
        var a = g.grid[row * CELLS + col];
        for (var i = 0; i < a.length; i++) {
            var j = a[i];
            if (g.eatenBy[j] !== -1) continue;
            var dx = x - g.pellets[j].x, dy = y - g.pellets[j].y;
            if (dx * dx + dy * dy < r * r) g.eatenBy[j] = who;
        }
    }
}

// How many chunks take the body from start to goal: the proofs of work a game needs.
function chunksNeeded(p) { return Math.trunc((p.goal - p.start + p.chunk - 1) / p.chunk); }

function mass(me) { return me.body + me.stomach; }

function clamp(v) { return v < 0 ? 0 : (v > WORLD ? WORLD : v); }

function move(o, dir, m, boost) {
    if (dir === STOP) return;
    var s = boost ? Math.trunc(speed(m) * 115 / 100) : speed(m);
    o.x = clamp(o.x + Math.trunc(COS[dir] * s / 1024));
    o.y = clamp(o.y + Math.trunc(SIN[dir] * s / 1024));
}

// a eats b when a is 10% bigger and b's centre is inside a.
function eats(ax, ay, am, ra, bx, by, bm) {
    if (am * 10 < bm * 11) return false;
    var dx = ax - bx, dy = ay - by;
    if (dx > ra || dx < -ra || dy > ra || dy < -ra) return false;
    return dx * dx + dy * dy < ra * ra;
}

function botThink(g, b, idx) {
    if (b.wait > 0) { b.wait--; return; }
    b.wait = RETARGET;
    var me = g.me, mm = mass(me);
    var fx = 0, fy = 0, threat = false;           // flee vector
    var tx = 0, ty = 0, best = -1;                // chase target
    var cands = [{ x: me.x, y: me.y, m: mm }];
    for (var i = 0; i < g.bots.length; i++) if (i !== idx) cands.push({ x: g.bots[i].x, y: g.bots[i].y, m: g.bots[i].mass });
    for (var k = 0; k < cands.length; k++) {
        var c = cands[k], dx = c.x - b.x, dy = c.y - b.y;
        if (Math.abs(dx) > SIGHT || Math.abs(dy) > SIGHT) continue;
        if (c.m * 10 >= b.mass * 11) { fx -= Math.sign(dx) * (SIGHT - Math.abs(dx)); fy -= Math.sign(dy) * (SIGHT - Math.abs(dy)); threat = true; }
        else if (b.mass * 10 >= c.m * 11) {
            var score = c.m * 1000 - Math.trunc((Math.abs(dx) + Math.abs(dy)) / U);
            if (score > best) { best = score; tx = dx; ty = dy; }
        }
    }
    if (threat) b.dir = dirOf(fx, fy);
    else if (best >= 0 && randInt(g.rng, 4) !== 0) b.dir = dirOf(tx, ty);
    else if (randInt(g.rng, 3) === 0) b.dir = randInt(g.rng, DIRS);
}

// One tick. input: 0..63 heading, 64 stop, or -1 keep the last one. digests: chunk numbers
// whose proof of work arrives at this tick, in order. Returns "" or why the step is invalid.
function step(g, input, digests) {
    if (g.over) return "the game is over";
    var me = g.me, p = g.p;
    if (input !== -1) {
        if (input !== Math.trunc(input) || input < 0 || input > STOP) return "bad input at tick " + g.tick;
        me.dir = input;
    }
    if (digests) for (var d = 0; d < digests.length; d++) {
        if (digests[d] !== g.chunks) return "chunk " + digests[d] + " out of order at tick " + g.tick;
        if (g.chunks >= chunksNeeded(p)) return "chunk " + digests[d] + " beyond the goal at tick " + g.tick;
        if (me.stomach < p.chunk) return "chunk " + digests[d] + " with nothing to digest at tick " + g.tick;
        me.stomach -= p.chunk; me.body += p.chunk; g.chunks++;
    }
    move(me, me.dir, mass(me), true);      // the player is 15% quicker than a bot its size
    for (var i = 0; i < g.bots.length; i++) { botThink(g, g.bots[i], i); move(g.bots[i], g.bots[i].dir, g.bots[i].mass, false); }

    // Pellets: each goes to the first mover (the player, then the bots in order) whose circle
    // holds it at the start of the pass; then they respawn in pellet order.
    markPellets(g, me.x, me.y, radius(mass(me)), 0);
    for (var q = 0; q < g.bots.length; q++) markPellets(g, g.bots[q].x, g.bots[q].y, radius(g.bots[q].mass), q + 1);
    for (var j = 0; j < g.pellets.length; j++) {
        var who = g.eatenBy[j];
        if (who === -1) continue;
        g.eatenBy[j] = -1;
        if (who === 0) me.stomach += 1; else g.bots[who - 1].mass += 1;
        gridRemove(g, j); g.pellets[j] = spawnPellet(g); gridAdd(g, j);
    }
    // Bubbles.
    for (var a = 0; a < g.bots.length; a++) {
        var ba = g.bots[a];
        var mm = mass(me);
        if (eats(me.x, me.y, mm, radius(mm), ba.x, ba.y, ba.mass)) { me.stomach += ba.mass; g.bots[a] = spawnBot(g, 4 + randInt(g.rng, 40)); continue; }
        var ra = radius(ba.mass);
        if (me.shield === 0 && eats(ba.x, ba.y, ba.mass, ra, me.x, me.y, mass(me))) {
            // Eaten: the bot gets what was still undigested; the digested body is safe. Respawn
            // somewhere else with a short shield.
            ba.mass += me.stomach; ra = radius(ba.mass);
            me.stomach = 0; g.deaths++; me.shield = SHIELD;
            me.x = randInt(g.rng, WORLD); me.y = randInt(g.rng, WORLD);
        }
        for (var c = 0; c < g.bots.length; c++) {
            if (c === a) continue;
            var bc = g.bots[c];
            if (eats(ba.x, ba.y, ba.mass, ra, bc.x, bc.y, bc.mass)) { ba.mass += bc.mass; ra = radius(ba.mass); g.bots[c] = spawnBot(g, 4 + randInt(g.rng, 40)); }
        }
        // Huge bots slowly shrink, so the world never locks up.
        if (ba.mass > 600 && (g.tick + a) % 30 === 0) ba.mass -= 1;
    }
    if (me.shield > 0) me.shield--;
    g.tick++;
    if (!g.over && me.body >= p.goal && g.tick >= p.minTicks) { g.over = true; g.won = true; g.why = "goal"; }
    if (!g.over && g.tick >= p.maxTicks) { g.over = true; g.why = "time"; }
    return "";
}

// A 32-bit fingerprint of the whole state, to compare two engines tick by tick.
function stateHash(g) {
    var h = 0x811c9dc5;
    function mix(v) { h = Math.imul((h ^ (v >>> 0)) >>> 0, 16777619) >>> 0; }
    mix(g.tick); mix(g.deaths); mix(g.me.shield); mix(g.me.x); mix(g.me.y); mix(g.me.body); mix(g.me.stomach); mix(g.chunks);
    for (var i = 0; i < 4; i++) mix(g.rng.s[i]);
    for (var j = 0; j < g.bots.length; j++) { mix(g.bots[j].x); mix(g.bots[j].y); mix(g.bots[j].mass); mix(g.bots[j].dir); }
    for (var k = 0; k < g.pellets.length; k++) { mix(g.pellets[k].x); mix(g.pellets[k].y); }
    return ("00000000" + h.toString(16)).slice(-8);
}

// ---- the record and its replay ---------------------------------------------------------------
//
// record = { v, seed, inputs: [[tick, dir], …] (changes only), digests: [[tick, chunk, nonce], …],
//            end: last tick }. The nonces are checked outside (argon2id lives in C++); this only
// replays the moves and the digestion order.

function newRecorder() { return { inputs: [], digests: [], lastDir: -2 }; }

function record(rec, tick, dir, digested) {         // digested: [[chunk, nonce], …] at this tick
    if (dir !== rec.lastDir) { rec.inputs.push([tick, dir]); rec.lastDir = dir; }
    if (digested) for (var i = 0; i < digested.length; i++) rec.digests.push([tick, digested[i][0], digested[i][1]]);
}

// Returns {ok, reason, ticks, body, chunks, hash}.
function replay(seedHex, params, rec) {
    if (!rec || rec.v !== SIM_VERSION) return { ok: false, reason: "unknown record version" };
    var g = newGame(seedHex, params), ii = 0, di = 0;
    var ins = rec.inputs || [], dg = rec.digests || [];
    var end = rec.end;
    if (end !== Math.trunc(end) || end < 1 || end > params.maxTicks) return { ok: false, reason: "bad end tick" };
    while (g.tick < end && !g.over) {
        var input = -1;
        if (ii < ins.length && ins[ii][0] === g.tick) { input = ins[ii][1]; ii++; }
        var ds = null;
        while (di < dg.length && dg[di][0] === g.tick) { (ds = ds || []).push(dg[di][1]); di++; }
        var err = step(g, input, ds);
        if (err) return { ok: false, reason: err };
    }
    if (ii !== ins.length || di !== dg.length) return { ok: false, reason: "entries after the end" };
    if (!g.won) return { ok: false, reason: g.over ? ("lost: " + g.why) : "goal not reached", ticks: g.tick, body: g.me.body };
    return { ok: true, reason: "", ticks: g.tick, body: g.me.body, chunks: g.chunks, deaths: g.deaths, hash: stateHash(g) };
}

// For a module that loads this file as a plain script.
if (typeof module !== "undefined" && module.exports)
    module.exports = { SIM_VERSION: SIM_VERSION, STOP: STOP, DIRS: DIRS, WORLD: WORLD, U: U, defaultParams: defaultParams,
        newGame: newGame, step: step, replay: replay, newRecorder: newRecorder, record: record, stateHash: stateHash,
        radius: radius, mass: mass, chunksNeeded: chunksNeeded, dirOf: dirOf, isqrt: isqrt };
