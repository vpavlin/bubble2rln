// Bubble: grow your bubble, digest with proof of work, claim an RLN membership.
//
// The game runs here, in sim.js (the same file bubble_core embeds to check a finished game).
// bubble_core gives each game its seed and finds the digestion proofs of work in the background;
// this view asks for them every half second and applies each one when the stomach holds a chunk.
// Every core call goes through callVia (logos.callModuleAsync; never a blocking call in a view).
// The 30 Hz loop runs only while a game is being played.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls
import "sim.js" as Sim

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
    property string screen: "start"     // start | starting | play | checking | won | lost
    property string message: ""
    property var info: null             // newGame's reply: gameId, seed, params, pow, chunks, threads
    property var game: null             // sim.js state
    property var rec: null              // sim.js recorder
    property var found: ({})            // chunk -> nonce, from the core
    property int foundCount: 0
    property real hashRate: 0
    property var checkResult: null
    property int dir: 64                // Sim.STOP
    property real mouseX: -1
    property real mouseY: -1
    property var keys: ({})
    property int frame: 0               // bumps every tick, for the HUD bindings
    property bool autopilot: false      // tests only (the harness sets it): head for the nearest dot
    readonly property bool playing: root.screen === "play"
    readonly property bool paused: root.playing && !root.activeNow
    readonly property bool activeNow: Qt.application.state === Qt.ApplicationActive && root.visible

    function startGame() {
        root.screen = "starting"; root.message = ""
        callVia("newGame", ["{}"], function (raw) {
            var r = root.parse(raw)
            if (!r || !r.ok) { root.screen = "start"; root.message = (r && r.error) || "bubble_core did not answer."; return }
            root.info = r
            root.game = Sim.newGame(r.seed, r.params)
            root.rec = Sim.newRecorder()
            root.found = ({}); root.foundCount = 0; root.hashRate = 0; root.checkResult = null
            root.dir = 64
            root.screen = "play"
            root.forceActiveFocus()
        })
    }

    function poll() {
        if (!root.info) return
        callVia("progress", [root.info.gameId], function (raw) {
            var r = root.parse(raw)
            if (!r || !r.ok) return
            var f = ({})
            for (var i = 0; i < r.found.length; i++) f[r.found[i][0]] = r.found[i][1]
            root.found = f; root.foundCount = r.found.length; root.hashRate = r.hashRate || 0
            if (root.screen === "checking" && r.check && r.check.state === "done") {
                root.checkResult = r.check.result
                root.screen = (r.check.result && r.check.result.ok) ? "won" : "lost"
                if (!(r.check.result && r.check.result.ok)) root.message = "The check refused the game: " + ((r.check.result && r.check.result.reason) || "no reason given")
            }
        })
    }

    // The heading from the mouse (relative to the player in the middle) or the arrow keys / WASD.
    function heading() {
        if (root.autopilot) {
            var me = root.game.me, best = null, bd = 1e18
            for (var i = 0; i < root.game.pellets.length; i++) {
                var p = root.game.pellets[i], dd = Math.abs(p.x - me.x) + Math.abs(p.y - me.y)
                if (dd < bd) { bd = dd; best = p }
            }
            return best ? Sim.dirOf(best.x - me.x, best.y - me.y) : 64
        }
        var kx = (root.keys.right ? 1 : 0) - (root.keys.left ? 1 : 0), ky = (root.keys.down ? 1 : 0) - (root.keys.up ? 1 : 0)
        if (kx !== 0 || ky !== 0) return Sim.dirOf(kx, ky)
        if (root.mouseX < 0) return root.dir
        var dx = root.mouseX - board.width / 2, dy = root.mouseY - board.height / 2
        var rPx = Sim.radius(Sim.mass(root.game.me)) / Sim.U * board.scale
        if (dx * dx + dy * dy < rPx * rPx * 0.25) return 64
        return Sim.dirOf(Math.round(dx * 1000), Math.round(dy * 1000))
    }

    function tick() {
        var g = root.game
        if (!g || g.over) return
        var d = heading()
        // Digest every chunk whose proof has arrived, while the stomach holds a chunk.
        var dig = [], need = Sim.chunksNeeded(g.p)
        for (var k = g.chunks; k < need && root.found[k] !== undefined && g.me.stomach >= g.p.chunk * (dig.length + 1); k++)
            dig.push([k, root.found[k]])
        Sim.record(root.rec, g.tick, d, dig)
        var err = Sim.step(g, d, dig.map(function (x) { return x[0] }))
        root.dir = d
        if (err) { root.message = "Game error: " + err; root.screen = "lost"; return }
        root.frame++
        board.requestPaint()
        if (g.over) finishGame()
    }

    function finishGame() {
        var g = root.game
        if (!g.won) {
            root.message = g.why === "time" ? "Out of time." : "You gave up."
            root.screen = "lost"
            callVia("abandon", [root.info.gameId], null)
            return
        }
        var r = { v: Sim.SIM_VERSION, seed: root.info.seed, inputs: root.rec.inputs, digests: root.rec.digests, end: g.tick }
        root.screen = "checking"
        callVia("finish", [root.info.gameId, JSON.stringify(r)], null)
    }

    Timer { interval: 33; repeat: true; running: root.playing && root.activeNow; onTriggered: root.tick() }
    Timer { interval: 500; repeat: true; running: root.playing || root.screen === "checking"; onTriggered: root.poll() }

    Keys.onPressed: function (e) { root.setKey(e.key, true) }
    Keys.onReleased: function (e) { if (!e.isAutoRepeat) root.setKey(e.key, false) }
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

    // ── the board
    Canvas {
        id: board
        anchors.fill: parent
        visible: root.game !== null && root.screen !== "start" && root.screen !== "starting"
        renderStrategy: Canvas.Cooperative
        // px per world px: zoom out as the bubble grows.
        property real scale: (root.game && root.frame >= 0) ? Math.min(1.4, height / (700 + 7 * Sim.radius(Sim.mass(root.game.me)) / Sim.U)) : 1
        onPaint: {
            var ctx = getContext("2d"), g = root.game
            ctx.fillStyle = root.cBg; ctx.fillRect(0, 0, width, height)
            if (!g) return
            var s = scale / Sim.U, cx = width / 2 - g.me.x * s, cy = height / 2 - g.me.y * s
            // Grid every 100 px and the world's edge.
            ctx.strokeStyle = root.cLine; ctx.lineWidth = 1; ctx.beginPath()
            var step = 100 * Sim.U, W = Sim.WORLD
            var x0 = Math.max(0, Math.floor((-cx / s) / step) * step), x1 = Math.min(W, (width - cx) / s)
            var y0 = Math.max(0, Math.floor((-cy / s) / step) * step), y1 = Math.min(W, (height - cy) / s)
            for (var x = x0; x <= x1; x += step) { ctx.moveTo(cx + x * s, cy + Math.max(0, y0) * s); ctx.lineTo(cx + x * s, cy + y1 * s) }
            for (var y = y0; y <= y1; y += step) { ctx.moveTo(cx + Math.max(0, x0) * s, cy + y * s); ctx.lineTo(cx + x1 * s, cy + y * s) }
            ctx.stroke()
            ctx.strokeStyle = root.cFaint; ctx.lineWidth = 3; ctx.strokeRect(cx, cy, W * s, W * s)
            // Pellets.
            for (var i = 0; i < g.pellets.length; i++) {
                var p = g.pellets[i], px = cx + p.x * s, py = cy + p.y * s
                if (px < -5 || py < -5 || px > width + 5 || py > height + 5) continue
                ctx.fillStyle = root.bubbleColours[i % root.bubbleColours.length]
                ctx.beginPath(); ctx.arc(px, py, Math.max(2, 3 * scale), 0, 2 * Math.PI); ctx.fill()
            }
            // Bubbles: the dangerous ones get a red ring.
            var mm = Sim.mass(g.me)
            for (var j = 0; j < g.bots.length; j++) {
                var b = g.bots[j], r = Sim.radius(b.mass) * s, bx = cx + b.x * s, by = cy + b.y * s
                if (bx < -r || by < -r || bx > width + r || by > height + r) continue
                ctx.globalAlpha = 0.85
                ctx.fillStyle = root.bubbleColours[j % root.bubbleColours.length]
                ctx.beginPath(); ctx.arc(bx, by, r, 0, 2 * Math.PI); ctx.fill()
                ctx.globalAlpha = 1
                if (b.mass * 10 >= mm * 11) { ctx.strokeStyle = root.cBad; ctx.lineWidth = 3; ctx.stroke() }
            }
            // The player: the body, and the stomach as a lighter core still to digest.
            var rm = Sim.radius(mm) * s
            ctx.fillStyle = root.cAccent
            ctx.beginPath(); ctx.arc(width / 2, height / 2, rm, 0, 2 * Math.PI); ctx.fill()
            if (g.me.shield > 0) { ctx.strokeStyle = "#ffffff"; ctx.globalAlpha = 0.6; ctx.lineWidth = 3
                ctx.beginPath(); ctx.arc(width / 2, height / 2, rm + 5, 0, 2 * Math.PI); ctx.stroke(); ctx.globalAlpha = 1 }
            if (g.me.stomach > 0) {
                ctx.fillStyle = "#ffd3b8"; ctx.globalAlpha = 0.75
                ctx.beginPath(); ctx.arc(width / 2, height / 2, rm * Math.sqrt(g.me.stomach / mm), 0, 2 * Math.PI); ctx.fill()
                ctx.globalAlpha = 1
            }
        }
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            enabled: root.playing
            onPositionChanged: function (m) { root.mouseX = m.x; root.mouseY = m.y }
            onExited: { root.mouseX = -1 }
        }
    }

    // ── HUD while playing
    Rectangle {
        visible: root.screen === "play" || root.screen === "checking"
        anchors { left: parent.left; top: parent.top; margins: 16 }
        width: 300; height: hud.implicitHeight + 24; radius: 10
        color: root.cSurface; opacity: 0.92; border.color: root.cLine
        ColumnLayout {
            id: hud
            anchors { fill: parent; margins: 12 }
            spacing: 6
            property int f: root.frame
            readonly property var me: (root.game && hud.f >= 0) ? root.game.me : null
            readonly property int need: root.game ? Sim.chunksNeeded(root.game.p) : 1
            Text { color: root.cText; font.pixelSize: 15; font.bold: true
                text: hud.me ? ("Size " + hud.me.body + " / " + root.game.p.goal) : "" }
            Rectangle { Layout.fillWidth: true; height: 8; radius: 4; color: root.cRaised
                Rectangle { height: parent.height; radius: 4; color: root.cAccent
                    width: hud.me ? parent.width * Math.min(1, (hud.me.body - root.game.p.start) / (root.game.p.goal - root.game.p.start)) : 0 } }
            Text { color: root.cSub; font.pixelSize: 13
                text: hud.me ? ("In the stomach: " + hud.me.stomach + (hud.me.stomach >= root.game.p.chunk && root.found[root.game.chunks] === undefined ? "  (digesting…)" : "")) : "" }
            Text { color: root.cSub; font.pixelSize: 13
                text: "Digestion proofs: " + root.foundCount + " / " + hud.need + "  ·  " + Math.round(root.hashRate) + " hashes/s" }
            Rectangle { Layout.fillWidth: true; height: 6; radius: 3; color: root.cRaised
                Rectangle { height: parent.height; radius: 3; color: root.cOk; width: parent.width * Math.min(1, root.foundCount / hud.need) } }
            Text { color: root.cFaint; font.pixelSize: 12
                text: root.game ? ((hud.me && hud.me.shield > 0 && root.game.deaths > 0 ? "Eaten! Your digested size is safe.  ·  " : "") + Math.floor(root.game.tick / 30 / 60) + ":" + ("0" + Math.floor(root.game.tick / 30) % 60).slice(-2) + (hud.f >= 0 && root.paused ? "  ·  paused" : "")) : "" }
        }
    }
    Text {
        visible: root.screen === "play"
        anchors { right: parent.right; top: parent.top; margins: 16 }
        color: root.cFaint; font.pixelSize: 12
        text: "Steer with the mouse or the arrow keys · Esc gives up"
    }
    Keys.onEscapePressed: { if (root.playing) { root.game.over = true; root.game.why = "gave up"; root.finishGame(); root.message = "You gave up." } }

    // ── cards: start, checking, won, lost
    Rectangle {
        visible: root.screen !== "play"
        anchors.fill: parent
        color: root.screen === "start" || root.screen === "starting" ? root.cBg : "#a0101014"
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(560, parent.width - 48); height: card.implicitHeight + 48; radius: 14
            color: root.cSurface; border.color: root.cLine
            ColumnLayout {
                id: card
                anchors { fill: parent; margins: 24 }
                spacing: 14
                Text { Layout.fillWidth: true; color: root.cText; font.pixelSize: 26; font.bold: true; wrapMode: Text.Wrap
                    text: root.screen === "won" ? "You made it!" : root.screen === "lost" ? "Game over" : root.screen === "checking" ? "Checking your game…" : "Bubble" }
                Text {
                    Layout.fillWidth: true; color: root.cSub; font.pixelSize: 14; wrapMode: Text.Wrap; textFormat: Text.StyledText
                    visible: root.screen === "start" || root.screen === "starting"
                    text: "Grow from a small bubble to size <b>" + (root.info ? root.info.params.goal : 300) + "</b>. Eat the dots and anything at least 10% smaller than you; "
                        + "keep away from bubbles with a <font color='" + root.cBad + "'>red ring</font>. If one eats you, you lose only what you have not digested yet.<br><br>"
                        + "What you eat goes to your stomach first. Your computer <b>digests</b> it by solving small proofs of work, so you grow only as fast as it can work. "
                        + "Reach the goal and the game, with its proofs, becomes your ticket for an <b>RLN membership</b>: your right to send messages on Logos Messaging."
                }
                Text {
                    Layout.fillWidth: true; color: root.cSub; font.pixelSize: 14; wrapMode: Text.Wrap
                    visible: root.screen === "checking"
                    text: "Replaying your " + ((root.game && root.frame >= 0) ? Math.round(root.game.tick / 30) : 0) + " s game and checking every proof of work, the way the membership gifter will."
                }
                Text {
                    Layout.fillWidth: true; color: root.cSub; font.pixelSize: 14; wrapMode: Text.Wrap
                    visible: root.screen === "won"
                    text: root.checkResult ? ("Size " + root.checkResult.body + " in " + Math.round(root.checkResult.ticks / 30) + " s, " + root.checkResult.chunks
                        + " proofs of work, all checked" + (root.checkResult.deaths ? (", eaten " + root.checkResult.deaths + "×") : "") + ". Claiming the membership arrives in the next version; your game is ready for it.") : ""
                }
                Text {
                    Layout.fillWidth: true; color: root.screen === "lost" ? root.cSub : root.cBad; font.pixelSize: 14; wrapMode: Text.Wrap
                    visible: root.message !== ""
                    text: root.message
                }
                RowLayout {
                    spacing: 10
                    LogosButton {
                        text: root.screen === "starting" ? "Starting…" : (root.screen === "start" ? "Play" : "Play again")
                        enabled: root.screen !== "starting" && root.screen !== "checking"
                        onClicked: root.startGame()
                    }
                    Text { color: root.cFaint; font.pixelSize: 12; visible: !!root.info && root.screen === "start"; text: root.info ? (root.info.threads + " threads digest") : "" }
                }
            }
        }
    }
}
