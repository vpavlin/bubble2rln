// Headless test of bubble_core through logosctl: new game -> steer to the nearest dot through
// frame() until the goal -> the core's own check passes -> tampered copies of the record fail.
// Start the session's daemon with a short game and a slow-driver pause, e.g.
//   BUBBLE_GOAL=60 BUBBLE_MIN_TICKS=300 BUBBLE_POW_BITS=10 BUBBLE_PAUSE_MS=5000
// usage: node e2e-core.js SESSION
const { execFileSync } = require("child_process");
const sess = process.argv[2] || "bubble";
function call(method, ...args) {
    const out = execFileSync(process.env.HOME + "/port-0.3/ctl.sh", [sess, "-j", "call", "bubble_core", method, ...args], { encoding: "utf8", maxBuffer: 1 << 26 });
    const o = JSON.parse(out.trim().split("\n").pop());
    if (o.status !== "ok") throw new Error(out);
    return JSON.parse(o.result);
}
const view = "str:" + JSON.stringify({ w: 1000, h: 700 });
const g0 = call("newGame", "str:{}");
console.log("game", g0.gameId, "params", JSON.stringify(g0.params), "pow", JSON.stringify(g0.pow));
let f = call("frame", "str:" + g0.gameId, "0", "0", view), calls = 1;
while (!(f.over && f.check.state === "done")) {
    let dx = 0, dy = 0, bd = 1e12;
    for (let i = 0; i < f.pellets.length; i += 3) {
        const x = f.pellets[i] - 500, y = f.pellets[i + 1] - 350, d = Math.abs(x) + Math.abs(y);
        if (d < bd) { bd = d; dx = x; dy = y; }
    }
    f = call("frame", "str:" + g0.gameId, String(Math.round(dx)), String(Math.round(dy)), view); calls++;
    if (calls % 20 === 0) console.log(`  t=${f.seconds.toFixed(1)}s size ${f.me.body}/${f.me.goal} stomach ${f.me.stomach} proofs ${f.proofs.found}/${f.proofs.need} queued ${f.proofs.queued}${f.me.digesting ? " (digesting)" : ""} ${Math.round(f.proofs.hashRate)} h/s deaths ${f.me.deaths}`);
    if (f.over && !f.won) throw new Error("lost: " + f.why);
}
console.log("core's check:", JSON.stringify(f.check.result), `(${calls} frames)`);
if (!f.check.result.ok) process.exit(1);
const r = call("record", "str:" + g0.gameId);
const again = call("checkRecord", "str:" + r.seed, "str:" + JSON.stringify(r.record));
console.log("checkRecord of the record:", again.ok, again.hash);
const bad = JSON.parse(JSON.stringify(r.record)); bad.digests[1][2] = (bad.digests[1][2] + 1) >>> 0;
console.log("tampered nonce:", JSON.stringify(call("checkRecord", "str:" + r.seed, "str:" + JSON.stringify(bad))));
const bad2 = JSON.parse(JSON.stringify(r.record)); bad2.inputs[2][1] = (bad2.inputs[2][1] + 32) % 64;
console.log("tampered move:", JSON.stringify(call("checkRecord", "str:" + r.seed, "str:" + JSON.stringify(bad2))));
console.log("another seed:", JSON.stringify(call("checkRecord", "str:" + "ab".repeat(32), "str:" + JSON.stringify(r.record))));
if (!process.env.KEEP) call("abandon", "str:" + g0.gameId);
else console.log("kept game", g0.gameId, "claimable", g0.claimable, JSON.stringify(g0.beacon));
