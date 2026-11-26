#pragma once

#include <string>
#include <cstdint>
#include "logos_module_context.h"

struct BubbleCorePrivate;

/**
 * BubbleCoreImpl - the whole game behind the Bubble Basecamp view.
 *
 * The view only draws and steers. This module:
 *  - runs the game (sim.h) on its own 30 Hz loop, while the view keeps asking for frames;
 *  - records every heading change and digestion (the record the claim carries);
 *  - digests: background threads find the proof-of-work nonce for every chunk of the game;
 *  - checks a finished game the way the membership allocator will (replay + every nonce).
 *
 * Rules the module glue imposes (see the logos-basecamp-module skill):
 *  - every public method returns a JSON string
 *  - no default arguments, at most 4 arguments, no trailing comments on declarations
 */
class BubbleCoreImpl : public LogosModuleContext
{
public:
    BubbleCoreImpl();
    ~BubbleCoreImpl() override;

    // The current game id, the rules, the proof-of-work settings and the digesting thread count.
    std::string snapshot();

    // A new game (the previous one stops): {"ok", "gameId", "params", "pow", "chunks", "threads"}.
    // It starts paused and runs while frame() keeps being called. optionsJson: {} for now.
    std::string newGame(const std::string& optionsJson);

    // Steer and get the picture. dx, dy: where the pointer is, in px from the middle of the view
    // (0, 0 = stand still). viewJson: {"w": px, "h": px}. The reply is the visible world in view
    // px: {"ok", "tick", "over", "won", "why", "me", "scale", "world", "bots", "pellets",
    // "proofs", "check"}. Without a frame for 400 ms the game pauses.
    std::string frame(const std::string& gameId, int64_t dx, int64_t dy, const std::string& viewJson);

    // Stop the game and its digestion.
    std::string abandon(const std::string& gameId);

    // The game's record so far: {"ok", "seed", "params", "pow", "record"}.
    std::string record(const std::string& gameId);

    // Check any record against this module's rules: {"ok", "reason", ...} (for tests and tools).
    std::string checkRecord(const std::string& seedHex, const std::string& recordJson);

    // Claim an RLN membership with the won game, in the background (poll claimStatus or frame).
    // optionsJson: {"config"?: registry, "identityCommitment"?: 64 hex, "rate"?, "allocatorPeerId"?,
    // "allocatorMultiaddr"?, "audience"?} -> {"ok", "state"}; the gifter defaults to this build's
    // (snapshot's "gifter"), the identity to a demo one. Asks membership_allocation_client_module.
    std::string claim(const std::string& gameId, const std::string& optionsJson);

    // The claim's state: {"ok", "state": "" | "asking" | "granted" | "refused" | "failed", "result"}.
    std::string claimStatus(const std::string& gameId);

    // Endless mode (newGame {"mode":"endless"}, once this Basecamp has an RLN membership): ends the
    // game now; its score is the size reached. -> {"ok"}
    std::string finish(const std::string& gameId);

    // Posts a finished, checked endless game's score on Logos Delivery (signed, with its record, so
    // every receiver can replay it). -> {"ok", "id", "score"} or {"ok": false, "error"}
    std::string publishScore(const std::string& gameId);

    // The leaderboard: every received score that checked out. -> {"ok", "status", "entries": [{"rank",
    // "score", "ticks", "ended", "when", "mine"}] (top 20), "count", "myBest", "pending", "checked", "refused", "nickname", "loading"}; entries carry "name"
    std::string leaderboard();

    // The name this player posts scores under (kept across restarts, in this instance's data).
    // setNickname cleans it (score.h cleanName: up to 24 characters, no control characters) and
    // returns what was kept. -> {"ok", "name"}
    std::string nickname();
    std::string setNickname(const std::string& name);

    // What the player's Basecamp waits on before a claim can land, read in the background (cheap to poll):
    // {"ok", "wallet": {"state": checking|unavailable|pending|ready|failed, "detail", "synced"?, "head"?,
    // "percent"?, "blocksPerSecond"?, "etaSeconds"?}, "membership": {"state"?, "leaf"?}, "text"}.
    std::string rlnStatus();

    // When rlnStatus reports wallet.staleSequencer (a wallet made for an old testnet server, which
    // never opens): moves that wallet's home aside so the wallet module makes a fresh one on the next
    // Basecamp start. The RLN membership (in the keystore) is untouched. -> {"ok", "movedTo", "text"}
    std::string rlnResetWallet();

    // The allocator's auth vector "bubble-play" (claim.h), the gifter's side: check a claim.
    // requestJson: {"auth_type", "payload_hex", "domain", "action_hex", "config"?, "peer_id"?}
    // -> {"ok", "reason"?, "nullifier"?, "identity"?, "scheme"?} or {"error"}.
    std::string verify_auth(const std::string& requestJson);

    // The player's side: the claim payload of the current won game, signed for this action.
    // requestJson: {"auth_type", "domain", "action_hex", "args"?: {"gameId"?, "audience"?}} -> {"payload_hex"} or {"error"}.
    std::string produce_auth(const std::string& requestJson);

private:
    BubbleCorePrivate* d;
};
