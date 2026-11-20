// Headless test of bubble_core through logosctl: new game -> the bot plays that seed -> wait for
// every digestion nonce -> finish -> the check passes. Then a tampered record must fail.
// usage: node e2e-core.js SESSION   (the session's daemon has bubble_core loaded)
const { execFileSync } = require("child_process");
const S = require("../core/src/sim.js"), B = require("../sim/bot.js");
const sess = process.argv[2] || "bubble";
function call(method, ...args) {
    const out = execFileSync(process.env.HOME + "/port-0.3/ctl.sh", [sess, "-j", "call", "bubble_core", method, ...args.map(a => "str:" + a)], { encoding: "utf8", maxBuffer: 1 << 26 });
    const o = JSON.parse(out.trim().split("\n").pop());
    if (o.status !== "ok") throw new Error(out);
    return JSON.parse(o.result);
}
const sleep = ms => new Promise(r => setTimeout(r, ms));
(async () => {
    const g0 = call("newGame", "{}");
    console.log("game", g0.gameId, "seed", g0.seed.slice(0, 16), "chunks", g0.chunks, "pow", JSON.stringify(g0.pow));
    let won = null;
    for (let i = 0; i < 40 && !won; i++) {     // the bot needs a winning game on THIS seed
        const { g, rec } = B.play(g0.seed, g0.params);
        if (g.won) won = rec; else break;
    }
    if (!won) { console.log("the bot lost on this seed; run again"); process.exit(3); }
    let p;
    for (;;) { p = call("progress", g0.gameId); if (p.found.length >= g0.chunks) break; await sleep(500); }
    console.log("digested", p.found.length, "chunks with", p.hashes, "hashes at", Math.round(p.hashRate), "h/s");
    const nonce = new Map(p.found.map(([c, n]) => [c, n]));
    won.digests = won.digests.map(([t, c]) => [t, c, nonce.get(c)]);
    const t0 = Date.now();
    call("finish", g0.gameId, JSON.stringify(won));
    for (;;) { p = call("progress", g0.gameId); if (p.check.state === "done") break; await sleep(300); }
    console.log("check:", JSON.stringify(p.check.result), "in", Date.now() - t0, "ms");
    const bad = JSON.parse(JSON.stringify(won)); bad.digests[3][2] = (bad.digests[3][2] + 1) >>> 0;
    call("finish", g0.gameId, JSON.stringify(bad));
    for (;;) { p = call("progress", g0.gameId); if (p.check.state === "done") break; await sleep(300); }
    console.log("tampered nonce:", JSON.stringify(p.check.result));
    const bad2 = JSON.parse(JSON.stringify(won)); bad2.inputs[5][1] = (bad2.inputs[5][1] + 32) % 64;
    call("finish", g0.gameId, JSON.stringify(bad2));
    for (;;) { p = call("progress", g0.gameId); if (p.check.state === "done") break; await sleep(300); }
    console.log("tampered move:", JSON.stringify(p.check.result));
    call("abandon", g0.gameId);
})().catch(e => { console.error(e.message); process.exit(1); });
