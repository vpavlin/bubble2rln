// Bubble: grow your bubble, digest with proof of work, claim an RLN membership.
//
// A thin view. bubble_core runs the game (rules, loop, record, proof of work, check); this view
// only steers and draws. While a game is on it asks for a frame about 30 times a second
// (up to three calls in flight, so a slow round trip doesn't cap the rate; a reply older than the
// one already drawn is dropped): it sends where the pointer is and gets back the visible world
// in view pixels. When it stops asking (hidden, closed), the core pauses the game.
// Every core call goes through callVia (logos.callModuleAsync; never a blocking call in a view).

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

Item {
    id: root
    width: 1200
    height: 760
    focus: true

    // ── colours: design-system tokens with fallbacks
    function tok(v, fb) { return (v === undefined || v === null) ? fb : v }
    readonly property var pal: (typeof Theme !== "undefined" && Theme.palette) ? Theme.palette : ({})
    readonly property color cBg: tok(pal.background, "#141416")
    readonly property color cSurface: tok(pal.surface, "#1c1c20")
    readonly property color cRaised: tok(pal.surfaceRaised, "#26262c")
    readonly property color cText: tok(pal.text, "#ececf1")
    readonly property color cSub: tok(pal.textSecondary, "#a8a8b3")
    readonly property color cFaint: tok(pal.textTertiary, "#74747f")
    readonly property color cLine: tok(pal.borderHairline, "#2e2e36")
    readonly property color cAccent: tok(pal.primary, "#ff7a3d")
    readonly property color cOk: tok(pal.success, "#5fbf7f")
    readonly property color cBad: tok(pal.error, "#ef6f6c")
    readonly property var bubbleColours: ["#7fd1ff", "#b9f18c", "#ffa0e0", "#ffe14d", "#c9a0ff", "#8ff0d8"]

    // ── core bridge
    readonly property string coreName: "bubble_core"
    function _deliver(cb, raw) { if (cb) { try { cb(raw === undefined || raw === null ? "" : raw) } catch (e) { console.warn("bubble view: " + e) } } }
    function callVia(method, args, cb) {
        if (typeof logos === "undefined" || logos === null || typeof logos.callModuleAsync !== "function") { Qt.callLater(function () { root._deliver(cb, "") }); return }
        try { logos.callModuleAsync(root.coreName, method, args || [], function (res) { root._deliver(cb, res) }, 30000) }
        catch (e) { Qt.callLater(function () { root._deliver(cb, "") }) }
    }
    function parse(raw) {
        var v = raw
        for (var i = 0; i < 3 && typeof v === "string"; i++) {
            var s = v.trim(); if (s === "") return null
            try { v = JSON.parse(s) } catch (e) { return null }
        }
        return (v && typeof v === "object") ? v : null
    }

    // ── state
    property string screen: "start"     // start | starting | play | checking | won | over
    property string message: ""
    property var info: null             // newGame's reply: gameId, params, pow, chunks, threads
    property var fr: null               // the last frame
    property int inFlight: 0
    property real mouseX: -1
    property real mouseY: -1
    property var keys: ({})
    property bool autopilot: false      // tests only (the harness sets it): head for the nearest dot
    property var claim: ({ state: "", result: null })   // claimStatus's reply: "" | asking | granted | refused | failed
    property bool claimable: false      // the game is seeded from a LEZ block (newGame's reply)
    readonly property bool playing: root.screen === "play" || root.screen === "checking"
    // Endless mode (after a claim): no goal, the game ends when you are eaten (or Esc), and the score
    // can be posted to the leaderboard on Logos Delivery.
    property bool endless: false
    property bool paused: false     // P: the view stops asking for frames, so the core stops the clock and the digestion
    function togglePause() {
        if (root.screen !== "play") return
        root.paused = !root.paused
        if (!root.paused) { root._buf = []; root._clock = 0; root._arriveMs = 0 }   // pick the picture up afresh
    }
    property var scores: ({})                                // leaderboard()'s reply
    property string nickname: ""                             // the name scores go out under (kept by the core)
    property bool nameLoaded: false
    property var posted: ({ state: "", result: null })        // publishScore: "" | posting | done | failed
    readonly property bool canEndless: !!(root.rln.membership && root.rln.membership.state === "active")
    readonly property bool activeNow: Qt.application.state === Qt.ApplicationActive && root.visible

    function startGame(endless) {
        root.forceActiveFocus()   // keys to the game, even after typing a name
        root.screen = "starting"; root.message = ""; root.fr = null; root.claim = { state: "", result: null }
        root.endless = !!endless; root.posted = { state: "", result: null }; root.paused = false
        root._buf = []; root._clock = 0; root._arriveMs = 0; root.boostKey = false
        callVia("newGame", [root.endless ? "{\"mode\":\"endless\"}" : "{}"], function (raw) {
            var r = root.parse(raw)
            if (!r || !r.ok) { root.screen = "start"; root.message = (r && r.error) || "bubble_core did not answer."; return }
            root.info = r
            root.claimable = !!r.claimable
            root.screen = "play"
            root.forceActiveFocus()
        })
    }

    function claimMembership() {
        if (!root.info) return
        root.claim = { state: "asking", result: null }
        callVia("claim", [root.info.gameId, "{}"], function (raw) {
            const r = root.parse(raw)
            if (!r || !r.ok) root.claim = { state: "failed", result: { error: r && r.error ? r.error : "the claim could not start" } }
        })
    }
    function pollClaim() {
        if (!root.info) return
        callVia("claimStatus", [root.info.gameId], function (raw) {
            const r = root.parse(raw)
            if (r && r.ok && r.state !== "") root.claim = { state: r.state, result: r.result }
        })
    }
    Timer { interval: 2000; repeat: true; running: root.screen === "won" && root.claim.state === "asking" && root.activeNow; onTriggered: root.pollClaim() }

    // What the player's Basecamp waits on before a claim can land (the RLN wallet's sync, the membership).
    property var rln: ({})
    function pollRln() {
        callVia("rlnStatus", [], function (raw) {
            const r = root.parse(raw)
            if (r && r.ok) root.rln = r
        })
    }
    Timer { interval: 3000; repeat: true; triggeredOnStart: true; onTriggered: root.pollRln()
        running: (root.screen === "start" || root.screen === "won") && root.activeNow }

    function giveUp() {
        if (!root.info || root.screen !== "play") return
        if (root.endless) { callVia("finish", [root.info.gameId], null); return }   // ends here; the score counts
        callVia("abandon", [root.info.gameId], null)
        root.screen = "over"; root.message = "You gave up."
    }
    function publishScore() {
        if (!root.info) return
        root.posted = { state: "posting", result: null }
        root.forceActiveFocus()   // keys back to the game (P, Esc) after typing the name
        const gameId = root.info.gameId
        // The name first (the core keeps it for next time and signs it into the post).
        callVia("setNickname", [nameField.text], function (rawName) {
            const n = root.parse(rawName)
            if (n && n.ok) { root.nickname = n.name; nameField.text = n.name }
            root.postNow(gameId)
        })
    }
    function postNow(gameId) {
        callVia("publishScore", [gameId], function (raw) {
            const r = root.parse(raw)
            root.posted = r && r.ok ? { state: "done", result: r } : { state: "failed", result: { error: r && r.error ? r.error : "bubble_core did not answer" } }
            root.pollBoard()
        })
    }
    function pollBoard() {
        callVia("leaderboard", [], function (raw) {
            const r = root.parse(raw)
            if (r && r.ok) {
                root.scores = r
                if (r.nickname !== undefined && !root.nameLoaded) { root.nickname = r.nickname; nameField.text = r.nickname; root.nameLoaded = true }
            }
        })
    }
    // The leaderboard is always on screen: every 5 s between games, every 15 s while playing.
    Timer { interval: root.screen === "play" ? 15000 : 5000; repeat: true; triggeredOnStart: true; onTriggered: root.pollBoard()
        running: root.activeNow }
    // Where a score would rank on the board right now (the endless HUD shows it live).
    function rankFor(score) {
        var e = root.scores.entries || [], n = 1
        for (var i = 0; i < e.length; i++) if (e[i].score >= score && !e[i].mine) n++
        return n
    }
    readonly property bool boardBeside: root.width >= 760

    // Where to head, in px from the middle of the view.
    function steer() {
        if (root.autopilot && root.fr) {
            var p = root.fr.pellets, best = -1, bd = 1e12, cx = board.width / 2, cy = board.height / 2
            for (var i = 0; i < p.length; i += 3) {
                var dd = Math.abs(p[i] - cx) + Math.abs(p[i + 1] - cy)
                if (dd < bd) { bd = dd; best = i }
            }
            return best < 0 ? [0, 0] : [p[best] - cx, p[best + 1] - cy]
        }
        var kx = (root.keys.right ? 1 : 0) - (root.keys.left ? 1 : 0), ky = (root.keys.down ? 1 : 0) - (root.keys.up ? 1 : 0)
        if (kx !== 0 || ky !== 0) return [kx * 400, ky * 400]
        if (root.mouseX < 0) return root._lastSteer
        return [root.mouseX - board.width / 2, root.mouseY - board.height / 2]
    }
    property var _lastSteer: [0, 0]

    function pump() {
        if (root.inFlight >= 3 || !root.info) return
        var st = steer(); root._lastSteer = st
        root.inFlight++
        var sent = Date.now()
        callVia("frame", [root.info.gameId, Math.round(st[0]), Math.round(st[1]), JSON.stringify({ w: Math.round(board.width), h: Math.round(board.height), boost: root.boostKey })], function (raw) {
            root.inFlight = Math.max(0, root.inFlight - 1)
            root._rttMs = root._rttMs > 0 ? 0.8 * root._rttMs + 0.2 * (Date.now() - sent) : (Date.now() - sent)
            var f = root.parse(raw)
            if (!f || !f.ok) return
            if (root.fr && f.tick <= root.fr.tick && !f.over && f.check.state === root.fr.check.state) return   // overtaken by a newer reply
            // Into the drawing buffer, placed on the game's own clock (the tick), so replies that
            // arrive unevenly still draw evenly (drawing only: every position comes from the core).
            var now = Date.now()
            f._byId = root.botsById(f)
            var buf = root._buf.filter(function (x) { return x.tick < f.tick })
            buf.push(f); if (buf.length > 8) buf.shift()
            root._buf = buf
            var sample = now - f.tick * root.tickMs
            root._clock = (root._clock === 0 || sample < root._clock) ? sample : root._clock + 0.02 * (sample - root._clock)
            if (root._arriveMs > 0) root._gapMs = Math.max(16, Math.min(200, 0.8 * root._gapMs + 0.2 * (now - root._arriveMs)))
            root._arriveMs = now
            root._frames++
            if (now - root._fpsSince >= 1000) { root.fps = Math.round(root._frames * 1000 / (now - root._fpsSince)); root._frames = 0; root._fpsSince = now }
            root.fr = f
            board.requestPaint()
            if (f.over && root.screen === "play") {
                if (f.won || root.endless) root.screen = "checking"
                else { root.screen = "over"; root.message = f.why === "time" ? "Out of time." : ("The game stopped: " + f.why) }
            }
            if (root.screen === "checking" && f.check && f.check.state === "done") {
                root.screen = f.check.result && f.check.result.ok ? (root.endless ? "scored" : "won") : "over"
                if (!(f.check.result && f.check.result.ok)) root.message = "The check refused the game: " + ((f.check.result && f.check.result.reason) || "no reason given")
            }
        })
    }

    Timer { interval: 33; repeat: true; running: root.playing && root.activeNow && !root.paused; onTriggered: root.pump() }
    // Redraws between frames so the camera glide is smooth; only while a game is on.
    Timer { interval: 16; repeat: true; running: root.playing && root.activeNow && root.fr !== null && !root.paused; onTriggered: board.requestPaint() }
    readonly property real tickMs: 1000 / 30
    property var _buf: []           // recent frames, oldest first
    property real _clock: 0         // local time of tick 0, as the replies show it (the earliest)
    property bool boostKey: false   // space held
    property real _rttMs: 0         // a frame call's round trip, smoothed
    property real _paintMs: 0       // one board paint, smoothed
    function botsById(f) {
        var m = {}
        for (var j = 0; j < f.bots.length; j++) { var b = f.bots[j]; m[b[3]] = [b[0] - f.world[0], b[1] - f.world[1]] }
        return m
    }
    property real _arriveMs: 0
    property real _gapMs: 33
    property int _frames: 0
    property real _fpsSince: 0
    property int fps: 0             // frames from the core per second, as this view gets them

    Keys.onPressed: function (e) {
        if (e.key === Qt.Key_Escape) { if (root.paused) root.togglePause(); root.giveUp(); return }
        if (e.key === Qt.Key_P) { root.togglePause(); e.accepted = true; return }
        if (e.key === Qt.Key_Space) { root.boostKey = true; e.accepted = true; return }
        root.setKey(e.key, true)
    }
    Keys.onReleased: function (e) {
        if (e.isAutoRepeat) return
        if (e.key === Qt.Key_Space) { root.boostKey = false; return }
        root.setKey(e.key, false)
    }
    function setKey(k, down) {
        var n = null
        if (k === Qt.Key_Left || k === Qt.Key_A) n = "left"
        else if (k === Qt.Key_Right || k === Qt.Key_D) n = "right"
        else if (k === Qt.Key_Up || k === Qt.Key_W) n = "up"
        else if (k === Qt.Key_Down || k === Qt.Key_S) n = "down"
        if (!n) return
        var c = Object.assign({}, root.keys); c[n] = down; root.keys = c
    }

    Rectangle { anchors.fill: parent; color: root.cBg }

    // ── the board: draws the last frame as the core sent it
    Canvas {
        id: board
        anchors.fill: parent
        visible: root.fr !== null
        renderStrategy: Canvas.Cooperative
        onPaint: {
            var t0 = Date.now()
            var ctx = getContext("2d")
            ctx.fillStyle = root.cBg; ctx.fillRect(0, 0, width, height)
            var B = root._buf
            if (!root.fr || B.length === 0) return
            // Draw a little in the past (about two gaps between replies), between the two frames
            // around that moment: a = before, f = after, t = how far between.
            var at = (Date.now() - Math.max(50, Math.min(220, 2 * root._gapMs + 15)) - root._clock) / root.tickMs
            var k = B.length - 1
            while (k > 0 && B[k - 1].tick > at) k--
            var f = B[k], a = k > 0 ? B[k - 1] : null, t = 1
            if (a && at < f.tick) t = Math.max(0, (at - a.tick) / (f.tick - a.tick))
            if (a && Math.abs(f.world[0] - a.world[0]) + Math.abs(f.world[1] - a.world[1]) > width / 3) a = null   // a respawn: no glide
            ctx.save()
            if (a) ctx.translate((1 - t) * (a.world[0] - f.world[0]), (1 - t) * (a.world[1] - f.world[1]))
            // Grid and the world's edge: world = [x, y, size, grid step] in view px.
            var wx = f.world[0], wy = f.world[1], ws = f.world[2], step = f.world[3]
            ctx.strokeStyle = root.cLine; ctx.lineWidth = 1; ctx.beginPath()
            var gx0 = Math.max(wx, wx + Math.ceil(-wx / step) * step), gx1 = Math.min(width, wx + ws)
            var gy0 = Math.max(wy, wy + Math.ceil(-wy / step) * step), gy1 = Math.min(height, wy + ws)
            var top = Math.max(0, wy), bottom = Math.min(height, wy + ws), left = Math.max(0, wx), right = Math.min(width, wx + ws)
            for (var x = gx0; x <= gx1; x += step) { ctx.moveTo(x, top); ctx.lineTo(x, bottom) }
            for (var y = gy0; y <= gy1; y += step) { ctx.moveTo(left, y); ctx.lineTo(right, y) }
            ctx.stroke()
            ctx.strokeStyle = root.cFaint; ctx.lineWidth = 3; ctx.strokeRect(wx, wy, ws, ws)
            // Dots: flat [x, y, colour, …]; one path per colour.
            var pr = Math.max(2, 3 * f.scale), p = f.pellets
            for (var c = 0; c < 6; c++) {
                ctx.fillStyle = root.bubbleColours[c]; ctx.beginPath()
                for (var i = 0; i < p.length; i += 3) {
                    if (p[i + 2] !== c) continue
                    ctx.moveTo(p[i] + pr, p[i + 1]); ctx.arc(p[i], p[i + 1], pr, 0, 2 * Math.PI)
                }
                ctx.fill()
            }
            // Bubbles move on their own: glide each from where the last frame had it.
            var pb = a ? a._byId : null
            // Bubbles: [x, y, r, colour, danger]; 1 = could eat you (red ring), 2 = hunting you (two rings).
            for (var j = 0; j < f.bots.length; j++) {
                var b = f.bots[j]
                ctx.globalAlpha = 0.85
                ctx.fillStyle = root.bubbleColours[b[3] % root.bubbleColours.length]
                var bx = b[0], by = b[1], was = pb ? pb[b[3]] : undefined
                if (was) { bx = f.world[0] + was[0] + t * (b[0] - f.world[0] - was[0]); by = f.world[1] + was[1] + t * (b[1] - f.world[1] - was[1]) }
                ctx.beginPath(); ctx.arc(bx, by, b[2], 0, 2 * Math.PI); ctx.fill()
                ctx.globalAlpha = 1
                if (b[4]) { ctx.strokeStyle = root.cBad; ctx.lineWidth = 3; ctx.stroke() }
                if (b[4] === 2) { ctx.lineWidth = 2; ctx.beginPath(); ctx.arc(bx, by, b[2] + 8, 0, 2 * Math.PI); ctx.stroke() }
            }
            ctx.restore()
            // The player in the middle: the body, the stomach as a lighter core, the shield as a ring.
            var me = f.me, cx = width / 2, cy = height / 2
            ctx.fillStyle = root.cAccent
            ctx.beginPath(); ctx.arc(cx, cy, me.r, 0, 2 * Math.PI); ctx.fill()
            if (me.stomachR > 0) {
                ctx.fillStyle = "#ffd3b8"; ctx.globalAlpha = 0.75
                ctx.beginPath(); ctx.arc(cx, cy, me.stomachR, 0, 2 * Math.PI); ctx.fill()
                ctx.globalAlpha = 1
            }
            if (me.shield) {
                ctx.strokeStyle = "#ffffff"; ctx.globalAlpha = 0.6; ctx.lineWidth = 3
                ctx.beginPath(); ctx.arc(cx, cy, me.r + 5, 0, 2 * Math.PI); ctx.stroke(); ctx.globalAlpha = 1
            }
            var pm = Date.now() - t0
            root._paintMs = root._paintMs > 0 ? 0.9 * root._paintMs + 0.1 * pm : pm
        }
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            enabled: root.screen === "play"
            onPositionChanged: function (m) { root.mouseX = m.x; root.mouseY = m.y }
            onExited: { root.mouseX = -1 }
        }
    }

    // ── HUD
    Rectangle {
        visible: root.fr !== null && (root.screen === "play" || root.screen === "checking")
        anchors { left: parent.left; top: parent.top; margins: 16 }
        width: 300; height: hud.implicitHeight + 24; radius: 10
        color: root.cSurface; opacity: 0.92; border.color: root.cLine
        ColumnLayout {
            id: hud
            anchors { fill: parent; margins: 12 }
            spacing: 6
            readonly property var me: root.fr ? root.fr.me : null
            readonly property var proofs: root.fr ? root.fr.proofs : null
            Text { color: root.cText; font.pixelSize: 15; font.bold: true; text: hud.me ? (hud.me.endless ? ("Score " + hud.me.body + "  ·  endless") : ("Size " + hud.me.body + " / " + hud.me.goal)) : "" }
            Rectangle { Layout.fillWidth: true; height: 8; radius: 4; color: root.cRaised; visible: !(hud.me && hud.me.endless)
                Rectangle { height: parent.height; radius: 4; color: root.cAccent
                    width: hud.me ? parent.width * Math.min(1, (hud.me.body - hud.me.start) / (hud.me.goal - hud.me.start)) : 0 } }
            Text { color: root.cSub; font.pixelSize: 13
                text: hud.me ? ("In the stomach: " + hud.me.stomach + (hud.me.digesting ? "  (digesting…)" : "")) : "" }
            Text { color: hud.me && hud.me.dotFeeds === 0 ? root.cAccent : root.cSub; font.pixelSize: 13
                visible: hud.me && hud.me.dotFeeds !== undefined && hud.me.dotFeeds < 100
                text: hud.me ? (hud.me.dotFeeds === 0 ? "Dots don't feed you any more: eat bubbles" : "Dots feed you " + hud.me.dotFeeds + " %") : "" }
            Text { color: hud.me && hud.me.boost === "on" ? root.cAccent : root.cSub; font.pixelSize: 13
                visible: hud.me && hud.me.boost !== undefined
                text: hud.me ? ("Space: boost " + ({ ready: "ready", on: "now!", rest: "recharging", empty: "(needs food)" })[hud.me.boost]) : "" }
            Text { color: root.cSub; font.pixelSize: 13; visible: hud.me && hud.me.slowedBy > 0
                text: hud.me ? ("Full stomach: " + hud.me.slowedBy + " % slower") : "" }
            Text { color: root.cSub; font.pixelSize: 13
                text: hud.proofs ? ("Digestion proofs: " + hud.proofs.found + (root.endless ? "" : " / " + hud.proofs.need) + (root.endless || hud.proofs.found < hud.proofs.need ? "  ·  " + Math.round(hud.proofs.hashRate) + " hashes/s" : "")) : "" }
            Rectangle { Layout.fillWidth: true; height: 6; radius: 3; color: root.cRaised; visible: !root.endless
                Rectangle { height: parent.height; radius: 3; color: root.cOk; width: hud.proofs ? parent.width * Math.min(1, hud.proofs.found / hud.proofs.need) : 0 } }
            Text { color: root.cFaint; font.pixelSize: 12
                text: root.fr ? ((root.fr.me.shield && root.fr.me.deaths > 0 ? "Eaten! You lost your stomach and half of your size.  ·  " : "")
                    + Math.floor(root.fr.seconds / 60) + ":" + ("0" + Math.floor(root.fr.seconds) % 60).slice(-2)
                    + (root.fps > 0 ? "  ·  " + root.fps + " frames/s" : "")
                    + (root._rttMs > 0 ? "  ·  call " + Math.round(root._rttMs) + " ms  ·  draw " + Math.round(root._paintMs) + " ms" : "")) : "" }
        }
    }
    Text {
        visible: root.screen === "play"
        anchors { right: parent.right; top: parent.top; margins: 16 }
        color: root.cFaint; font.pixelSize: 12
        text: "Steer with the mouse (further = faster) or the arrow keys · Space boosts · Esc " + (root.endless ? "ends the game (your score counts)" : "gives up") + " · P pauses"
    }

    // Paused: the clock and the digestion stand still until P.
    Rectangle {
        visible: root.paused && root.screen === "play"
        anchors.fill: parent
        color: "#a0101014"
        Column {
            anchors.centerIn: parent
            spacing: 8
            Text { anchors.horizontalCenter: parent.horizontalCenter; color: root.cText; font.pixelSize: 30; font.bold: true; text: "Paused" }
            Text { anchors.horizontalCenter: parent.horizontalCenter; color: root.cSub; font.pixelSize: 14
                text: "The clock and your digestion wait too. Press P to go on." }
        }
        MouseArea { anchors.fill: parent; onClicked: root.togglePause() }
    }

    // ── cards: start, checking, won, over
    Rectangle {
        visible: root.screen !== "play"
        anchors.fill: parent
        color: root.screen === "start" || root.screen === "starting" ? root.cBg : "#a0101014"
        Rectangle {
            id: cardBox
            anchors.centerIn: parent
            anchors.horizontalCenterOffset: root.boardBeside ? -143 : 0
            width: Math.min(560, parent.width - 48 - (root.boardBeside ? 286 : 0)); height: card.implicitHeight + 48; radius: 14
            color: root.cSurface; border.color: root.cLine
            ColumnLayout {
                id: card
                anchors { fill: parent; margins: 24 }
                spacing: 14
                Text { Layout.fillWidth: true; color: root.cText; font.pixelSize: 26; font.bold: true; wrapMode: Text.Wrap
                    text: root.screen === "scored" ? ("Score " + (root.fr && root.fr.check && root.fr.check.result ? root.fr.check.result.score : ""))
                        : root.screen === "won" ? "You made it!" : root.screen === "over" ? "Game over" : root.screen === "checking" ? "Checking your game…" : "Bubble" }
                Text {
                    Layout.fillWidth: true; color: root.cSub; font.pixelSize: 14; wrapMode: Text.Wrap; textFormat: Text.StyledText
                    visible: root.screen === "start" || root.screen === "starting"
                    text: "Grow from a small bubble to the goal size. Eat the dots and anything at least 10% smaller than you; "
                        + "keep away from bubbles with a <font color='" + root.cBad + "'>red ring</font>. If one eats you, you lose what you haven't digested yet and <b>half of your size</b>.<br><br>"
                        + "Dots feed you less as you grow, and past size 200 not at all: the rest you hunt. Food you haven't swallowed whole slowly goes. "
                        + "Once you are past 120, two big <font color='" + root.cBad + "'>double-ringed</font> hunters come after you. "
                        + "A full stomach slows you down; <b>Space</b> boosts you away, but burns some of what you have eaten.<br><br>"
                        + "What you eat goes to your stomach first. Your computer <b>digests</b> it by solving small proofs of work, so you grow only as fast as it can work. "
                        + "Reach the goal and the game, with its proofs, becomes your ticket for an <b>RLN membership</b>: your right to send messages on Logos Messaging."
                }
                Text {
                    Layout.fillWidth: true; color: root.cSub; font.pixelSize: 14; wrapMode: Text.Wrap
                    visible: root.screen === "checking"
                    text: "Replaying your " + (root.fr ? Math.round(root.fr.seconds) : 0) + " s game and checking every proof of work, the way the membership gifter will."
                }
                Text {
                    Layout.fillWidth: true; color: root.cSub; font.pixelSize: 14; wrapMode: Text.Wrap
                    visible: root.screen === "won"
                    readonly property var r: root.fr && root.fr.check ? root.fr.check.result : null
                    text: r ? ("Size " + r.body + " in " + Math.round(r.ticks / 30) + " s, " + r.chunks + " proofs of work, all checked"
                        + (r.deaths ? (", eaten " + r.deaths + "×") : "") + ".") : ""
                }
                // Endless: the result, and posting it to the leaderboard.
                Text {
                    Layout.fillWidth: true; color: root.cSub; font.pixelSize: 14; wrapMode: Text.Wrap
                    visible: root.screen === "scored"
                    readonly property var r: root.fr && root.fr.check ? root.fr.check.result : null
                    text: r ? ("Size " + r.score + " after " + Math.round(r.ticks / 30) + " s: " + ({ eaten: "you were eaten", quit: "you ended the game", time: "the clock ran out" })[r.ended]
                        + ". " + r.chunks + " proofs of work, all checked.") : ""
                }
                Text {
                    Layout.fillWidth: true; font.pixelSize: 14; wrapMode: Text.Wrap; textFormat: Text.StyledText
                    visible: root.screen === "scored"
                    readonly property var p: root.posted
                    color: p.state === "done" ? root.cOk : p.state === "failed" ? root.cBad : root.cSub
                    text: p.state === "" ? "Post it to the <b>leaderboard</b>: the score goes out on Logos Messaging with your game's record, signed, and every Bubble that receives it replays the game before listing it."
                        : p.state === "posting" ? "Posting…"
                        : p.state === "done" ? ("Posted. It reaches the others' boards in a moment" + (root.scores.status && root.scores.status !== "online" ? " (" + root.scores.status + ")" : "") + ".")
                        : ("Not posted: " + ((p.result && p.result.error) || "unknown error"))
                }
                Text {
                    Layout.fillWidth: true; color: root.cSub; font.pixelSize: 13; wrapMode: Text.Wrap
                    visible: root.screen === "start" && root.canEndless
                    text: "Your Basecamp has an RLN membership, so the endless game is open: no goal, it ends when you're eaten (or Esc), and your size then is your score."
                }
                // The claim: the game's proofs go to the gifter, which checks them and registers an RLN membership.
                Text {
                    Layout.fillWidth: true; font.pixelSize: 14; wrapMode: Text.Wrap; textFormat: Text.StyledText
                    visible: root.screen === "won"
                    readonly property var c: root.claim
                    readonly property var res: c.result || ({})
                    color: c.state === "granted" ? root.cOk : (c.state === "refused" || c.state === "failed") ? root.cBad : root.cSub
                    text: !root.claimable ? "This was a practice game (no LEZ block to seed it from), so it can't be claimed."
                        : c.state === "" ? "Claim your <b>RLN membership</b>: the gifter checks your proofs of work and registers you, 100 messages every 10 minutes."
                        : c.state === "asking" && res.stage === "wallet" ? "Your claim is waiting for your RLN wallet to finish syncing (below). Then it goes to the gifter by itself."
                        : c.state === "asking" && res.stage === "chain" ? "The gifter has your registration. Waiting for the LEZ testnet to confirm it and your node to see it: testnet blocks are slow, this can take several minutes. Keep this screen open."
                        : c.state === "asking" ? "Asking the gifter… it checks your proofs, then registers your node's RLN identity on the LEZ testnet. Keep this screen open: a new game stops the claim."
                        : c.state === "granted" ? ("Membership <b>#" + res.leaf_index + "</b> registered" + (res.tx_hash ? " (tx " + String(res.tx_hash).slice(0, 12) + "…)" : "")
                            + (res.demoIdentity ? ".<br>Demo identity: it is real on the registry, but this build has no RLN key of yours to send with." : ".<br>It belongs to your Basecamp's messaging node, which sends with it from now on: open Chat and say hello."))
                        : (res.message || res.error || "The claim failed.")
                }
                // The RLN wallet: what a claim waits on (shown until the membership is in place).
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 6
                    readonly property var w: root.rln.wallet || ({})
                    visible: (root.screen === "start" || (root.screen === "won" && root.claimable && root.claim.state !== "granted")) && !!root.rln.text
                    Text { Layout.fillWidth: true; font.pixelSize: 13; wrapMode: Text.Wrap
                        color: parent.w.state === "ready" ? root.cOk : (parent.w.state === "failed" || parent.w.state === "unavailable") ? root.cBad : root.cSub
                        text: root.rln.text || "" }
                    LogosButton {
                        visible: !!parent.w.staleSequencer
                        text: "Reset RLN wallet"
                        onClicked: callVia("rlnResetWallet", [], function (raw) {
                            const r = root.parse(raw)
                            root.message = r && r.ok ? r.text : ("Couldn't reset the wallet: " + ((r && r.error) || "no answer"))
                        })
                    }
                    Rectangle { Layout.fillWidth: true; height: 6; radius: 3; color: root.cRaised; visible: parent.w.percent !== undefined && parent.w.state === "pending" && !parent.w.staleSequencer
                        Rectangle { height: parent.height; radius: 3; color: root.cAccent; width: parent.width * Math.min(1, (parent.parent.w.percent || 0) / 100) } }
                }
                RowLayout {
                    Layout.fillWidth: true; spacing: 10
                    visible: root.screen === "scored" && (root.posted.state === "" || root.posted.state === "failed")
                    Text { color: root.cSub; font.pixelSize: 14; text: "Your name" }
                    LogosTextField {
                        id: nameField
                        Layout.fillWidth: true
                        placeholderText: "anything you like, up to 24 characters"
                        textInput.maximumLength: 24
                        textInput.onAccepted: root.publishScore()
                    }
                }
                Text {
                    Layout.fillWidth: true; color: root.screen === "over" ? root.cSub : root.cBad; font.pixelSize: 14; wrapMode: Text.Wrap
                    visible: root.message !== ""
                    text: root.message
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 10
                    LogosButton {
                        visible: root.screen === "won" && root.claimable && root.claim.state !== "granted"
                        text: root.claim.state === "asking" ? "Claiming…" : root.claim.state === "" ? "Claim membership" : "Try again"
                        enabled: root.claim.state !== "asking"
                        onClicked: root.claimMembership()
                    }
                    LogosButton {
                        visible: root.screen === "scored" && (root.posted.state === "" || root.posted.state === "failed")
                        text: "Post score"
                        onClicked: root.publishScore()
                    }
                    LogosButton {
                        visible: root.canEndless && root.screen !== "checking"
                        text: root.screen === "scored" ? "Play endless again" : "Play endless"
                        enabled: root.screen !== "starting"
                        onClicked: root.startGame(true)
                    }
                    LogosButton {
                        text: root.screen === "starting" ? "Starting…" : (root.screen === "start" ? "Play" : (root.endless ? "Play for a membership" : "Play again"))
                        enabled: root.screen !== "starting" && root.screen !== "checking"
                        onClicked: root.startGame(false)
                    }
                }
            }
        }

    }

    // ── the leaderboard, on every screen: scores posted on Logos Messaging, each replayed and
    // checked by this Bubble. Beside the card between games; small in the corner while playing
    // (or when the window is too narrow for both).
    Rectangle {
        id: boardPanel
        readonly property bool compact: root.screen === "play" || !root.boardBeside
        readonly property int rows: compact ? 5 : 12
        x: compact ? parent.width - width - 16 : parent.width / 2 - 143 + cardBox.width / 2 + 16
        y: compact ? 40 : (parent.height - height) / 2
        width: compact ? 220 : 270; height: boardCol.implicitHeight + (compact ? 20 : 32); radius: compact ? 10 : 14
        color: root.cSurface; border.color: root.cLine; opacity: compact ? 0.92 : 1
        ColumnLayout {
            id: boardCol
            anchors { fill: parent; margins: boardPanel.compact ? 10 : 16 }
            spacing: boardPanel.compact ? 3 : 6
            Text { color: root.cText; font.pixelSize: boardPanel.compact ? 14 : 16; font.bold: true; text: "Leaderboard" }
            Text { Layout.fillWidth: true; color: root.cFaint; font.pixelSize: 11; wrapMode: Text.Wrap
                text: (root.scores.count || 0) + " checked score" + ((root.scores.count || 0) === 1 ? "" : "s")
                    + (root.scores.status && root.scores.status !== "online" ? "  ·  " + root.scores.status : "")
                    + (root.scores.pending ? "  ·  checking " + root.scores.pending : "") }
            Repeater {
                model: root.scores.entries || []
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true; spacing: 8
                    visible: modelData.rank <= boardPanel.rows
                    Text { Layout.preferredWidth: 28; color: root.cFaint; font.pixelSize: 13; text: "#" + modelData.rank }
                    Text { Layout.preferredWidth: 44; color: modelData.mine ? root.cAccent : root.cText; font.pixelSize: 14; font.bold: true; text: modelData.score }
                    // Names are whatever players typed: plain text only, cut to the row.
                    Text { Layout.fillWidth: true; color: modelData.mine ? root.cAccent : root.cSub; font.pixelSize: 12
                        textFormat: Text.PlainText; elide: Text.ElideRight; maximumLineCount: 1
                        text: (modelData.name ? modelData.name : "anonymous") + (boardPanel.compact ? "" : "  ·  " + Math.round(modelData.ticks / 30) + " s") }
                }
            }
            Text { visible: !(root.scores.entries && root.scores.entries.length); color: root.cSub; font.pixelSize: 13
                text: root.canEndless ? "No scores yet. Be the first." : "No scores yet." }
            Text { visible: !!root.scores.myBest && root.scores.myBest.rank > boardPanel.rows; color: root.cAccent; font.pixelSize: 13
                textFormat: Text.PlainText
                text: root.scores.myBest ? ("You: #" + root.scores.myBest.rank + " with " + root.scores.myBest.score) : "" }
            Text { visible: root.screen === "play" && root.endless && !!root.fr; color: root.cAccent; font.pixelSize: 13; font.bold: true
                text: root.fr ? ("Now: " + root.fr.me.body + " would be #" + root.rankFor(root.fr.me.body)) : "" }
            Text { visible: !boardPanel.compact && !root.canEndless; Layout.fillWidth: true; color: root.cFaint; font.pixelSize: 11; wrapMode: Text.Wrap
                text: "Win a game to claim a membership, then play endless and post your score here." }
        }
    }
}
