#pragma once

#include <string>
#include <cstdint>
#include "logos_module_context.h"

struct BubbleCorePrivate;

/**
 * BubbleCoreImpl - the engine behind the Bubble Basecamp view.
 *
 * The view plays the game (sim.js, the same file this module embeds). This module:
 *  - starts a game: its seed, the rules (params) and the digestion proof of work (pow);
 *  - digests: background threads find the proof-of-work nonce for every chunk of the game;
 *  - checks a finished game the way the membership allocator will (replay + every nonce),
 *    before anything is claimed.
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

    // The current game, the rules, the proof-of-work settings and this computer's thread count.
    std::string snapshot();

    // A new game (the previous one stops): {"ok", "gameId", "seed", "params", "pow", "chunks"}.
    // optionsJson: {} for now.
    std::string newGame(const std::string& optionsJson);

    // The game's digestion so far: {"ok", "found": [[chunk, nonce], ...] in chunk order, "hashes",
    // "hashRate", "check": {"state": "" | "running" | "done", "result": {...}}}.
    std::string progress(const std::string& gameId);

    // The finished game's record ({v, seed, inputs, digests, end}); checks it in the background,
    // the result arrives in progress().check.
    std::string finish(const std::string& gameId, const std::string& recordJson);

    // Stop the game and its digestion.
    std::string abandon(const std::string& gameId);

    // The game rules as sim.js, for a tool that wants to replay a record itself.
    std::string simSource();

private:
    BubbleCorePrivate* d;
};
