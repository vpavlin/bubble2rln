// Bubble: grow your bubble, digest with proof of work, claim an RLN membership.
//
// A thin view. bubble_core runs the game (rules, loop, record, proof of work, check); this view
// only steers and draws. While a game is on it asks for a frame about 30 times a second
// (one call in flight at a time): it sends where the pointer is and gets back the visible world
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
    property bool inFlight: false
    property real mouseX: -1
    property real mouseY: -1
    property var keys: ({})
    property bool autopilot: false      // tests only (the harness sets it): head for the nearest dot
    property var claim: ({ state: "", result: null })   // claimStatus's reply: "" | asking | granted | refused | failed
    property bool claimable: false      // the game is seeded from a LEZ block (newGame's reply)
    readonly property bool playing: root.screen === "play" || root.screen === "checking"
    readonly property bool activeNow: Qt.application.state === Qt.ApplicationActive && root.visible

    function startGame() {
        root.screen = "starting"; root.message = ""; root.fr = null; root.claim = { state: "", result: null }
        callVia("newGame", ["{}"], function (raw) {
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

    function giveUp() {
        if (!root.info || root.screen !== "play") return
        callVia("abandon", [root.info.gameId], null)
        root.screen = "over"; root.message = "You gave up."
    }

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
        if (root.inFlight || !root.info) return
        var st = steer(); root._lastSteer = st
        root.inFlight = true
        callVia("frame", [root.info.gameId, Math.round(st[0]), Math.round(st[1]), JSON.stringify({ w: Math.round(board.width), h: Math.round(board.height) })], function (raw) {
            root.inFlight = false
            var f = root.parse(raw)
            if (!f || !f.ok) return
            // The camera glides from the last frame's to this one's over the measured gap
            // between frames (drawing only: every position still comes from the core).
            var now = Date.now()
            if (root.fr && root.fr.world && f.world && Math.abs(f.world[0] - root.fr.world[0]) + Math.abs(f.world[1] - root.fr.world[1]) < board.width / 3)
                root._prevWorld = [root.fr.world[0], root.fr.world[1]]
            else root._prevWorld = null
            if (root._arriveMs > 0) root._gapMs = Math.max(16, Math.min(200, 0.8 * root._gapMs + 0.2 * (now - root._arriveMs)))
            root._arriveMs = now
            root._frames++
            if (now - root._fpsSince >= 1000) { root.fps = Math.round(root._frames * 1000 / (now - root._fpsSince)); root._frames = 0; root._fpsSince = now }
            root.fr = f
            board.requestPaint()
            if (f.over && root.screen === "play") {
                if (f.won) root.screen = "checking"
                else { root.screen = "over"; root.message = f.why === "time" ? "Out of time." : ("The game stopped: " + f.why) }
            }
            if (root.screen === "checking" && f.check && f.check.state === "done") {
                root.screen = f.check.result && f.check.result.ok ? "won" : "over"
                if (!(f.check.result && f.check.result.ok)) root.message = "The check refused the game: " + ((f.check.result && f.check.result.reason) || "no reason given")
            }
        })
    }

    Timer { interval: 33; repeat: true; running: root.playing && root.activeNow; onTriggered: root.pump() }
    // Redraws between frames so the camera glide is smooth; only while a game is on.
    Timer { interval: 16; repeat: true; running: root.playing && root.activeNow && root.fr !== null; onTriggered: board.requestPaint() }
    property var _prevWorld: null
    property real _arriveMs: 0
    property real _gapMs: 33
    property int _frames: 0
    property real _fpsSince: 0
    property int fps: 0             // frames from the core per second, as this view gets them

    Keys.onPressed: function (e) {
        if (e.key === Qt.Key_Escape) { root.giveUp(); return }
        root.setKey(e.key, true)
    }
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

    // ── the board: draws the last frame as the core sent it
    Canvas {
        id: board
        anchors.fill: parent
        visible: root.fr !== null
        renderStrategy: Canvas.Cooperative
        onPaint: {
            var ctx = getContext("2d"), f = root.fr
            ctx.fillStyle = root.cBg; ctx.fillRect(0, 0, width, height)
            if (!f) return
            ctx.save()
            if (root._prevWorld) {
                var t = Math.min(1, (Date.now() - root._arriveMs) / root._gapMs)
                ctx.translate((1 - t) * (root._prevWorld[0] - f.world[0]), (1 - t) * (root._prevWorld[1] - f.world[1]))
            }
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
            // Dots: flat [x, y, colour, …].
            var pr = Math.max(2, 3 * f.scale), p = f.pellets
            for (var i = 0; i < p.length; i += 3) {
                ctx.fillStyle = root.bubbleColours[p[i + 2]]
                ctx.beginPath(); ctx.arc(p[i], p[i + 1], pr, 0, 2 * Math.PI); ctx.fill()
            }
            // Bubbles: [x, y, r, colour, dangerous]; the dangerous ones get a red ring.
            for (var j = 0; j < f.bots.length; j++) {
                var b = f.bots[j]
                ctx.globalAlpha = 0.85
                ctx.fillStyle = root.bubbleColours[b[3] % root.bubbleColours.length]
                ctx.beginPath(); ctx.arc(b[0], b[1], b[2], 0, 2 * Math.PI); ctx.fill()
                ctx.globalAlpha = 1
                if (b[4]) { ctx.strokeStyle = root.cBad; ctx.lineWidth = 3; ctx.stroke() }
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
            Text { color: root.cText; font.pixelSize: 15; font.bold: true; text: hud.me ? ("Size " + hud.me.body + " / " + hud.me.goal) : "" }
            Rectangle { Layout.fillWidth: true; height: 8; radius: 4; color: root.cRaised
                Rectangle { height: parent.height; radius: 4; color: root.cAccent
                    width: hud.me ? parent.width * Math.min(1, (hud.me.body - hud.me.start) / (hud.me.goal - hud.me.start)) : 0 } }
            Text { color: root.cSub; font.pixelSize: 13
                text: hud.me ? ("In the stomach: " + hud.me.stomach + (hud.me.digesting ? "  (digesting…)" : "")) : "" }
            Text { color: root.cSub; font.pixelSize: 13
                text: hud.proofs ? ("Digestion proofs: " + hud.proofs.found + " / " + hud.proofs.need + (hud.proofs.found < hud.proofs.need ? "  ·  " + Math.round(hud.proofs.hashRate) + " hashes/s" : "")) : "" }
            Rectangle { Layout.fillWidth: true; height: 6; radius: 3; color: root.cRaised
                Rectangle { height: parent.height; radius: 3; color: root.cOk; width: hud.proofs ? parent.width * Math.min(1, hud.proofs.found / hud.proofs.need) : 0 } }
            Text { color: root.cFaint; font.pixelSize: 12
                text: root.fr ? ((root.fr.me.shield && root.fr.me.deaths > 0 ? "Eaten! Your digested size is safe.  ·  " : "")
                    + Math.floor(root.fr.seconds / 60) + ":" + ("0" + Math.floor(root.fr.seconds) % 60).slice(-2)
                    + (root.fps > 0 ? "  ·  " + root.fps + " frames/s" : "")) : "" }
        }
    }
    Text {
        visible: root.screen === "play"
        anchors { right: parent.right; top: parent.top; margins: 16 }
        color: root.cFaint; font.pixelSize: 12
        text: "Steer with the mouse or the arrow keys · Esc gives up"
    }

    // ── cards: start, checking, won, over
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
                    text: root.screen === "won" ? "You made it!" : root.screen === "over" ? "Game over" : root.screen === "checking" ? "Checking your game…" : "Bubble" }
                Text {
                    Layout.fillWidth: true; color: root.cSub; font.pixelSize: 14; wrapMode: Text.Wrap; textFormat: Text.StyledText
                    visible: root.screen === "start" || root.screen === "starting"
                    text: "Grow from a small bubble to the goal size. Eat the dots and anything at least 10% smaller than you; "
                        + "keep away from bubbles with a <font color='" + root.cBad + "'>red ring</font>. If one eats you, you lose only what you have not digested yet.<br><br>"
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
                // The claim: the game's proofs go to the gifter, which checks them and registers an RLN membership.
                Text {
                    Layout.fillWidth: true; font.pixelSize: 14; wrapMode: Text.Wrap; textFormat: Text.StyledText
                    visible: root.screen === "won"
                    readonly property var c: root.claim
                    readonly property var res: c.result || ({})
                    color: c.state === "granted" ? root.cOk : (c.state === "refused" || c.state === "failed") ? root.cBad : root.cSub
                    text: !root.claimable ? "This was a practice game (no LEZ block to seed it from), so it can't be claimed."
                        : c.state === "" ? "Claim your <b>RLN membership</b>: the gifter checks your proofs of work and registers you, 100 messages every 10 minutes."
                        : c.state === "asking" ? "Asking the gifter… it checks your proofs, then registers the membership on chain (up to a few minutes)."
                        : c.state === "granted" ? ("Membership <b>#" + res.leaf_index + "</b> registered" + (res.tx_hash ? " (tx " + String(res.tx_hash).slice(0, 12) + "…)" : "")
                            + (res.demoIdentity ? ".<br>Demo identity: it is real on the registry, but this build has no RLN key of yours to send with." : "."))
                        : (res.message || res.error || "The claim failed.")
                }
                Text {
                    Layout.fillWidth: true; color: root.screen === "over" ? root.cSub : root.cBad; font.pixelSize: 14; wrapMode: Text.Wrap
                    visible: root.message !== ""
                    text: root.message
                }
                RowLayout {
                    spacing: 10
                    LogosButton {
                        visible: root.screen === "won" && root.claimable && root.claim.state !== "granted"
                        text: root.claim.state === "asking" ? "Claiming…" : root.claim.state === "" ? "Claim membership" : "Try again"
                        enabled: root.claim.state !== "asking"
                        onClicked: root.claimMembership()
                    }
                    LogosButton {
                        text: root.screen === "starting" ? "Starting…" : (root.screen === "start" ? "Play" : "Play again")
                        enabled: root.screen !== "starting" && root.screen !== "checking"
                        onClicked: root.startGame()
                    }
                }
            }
        }
    }
}
