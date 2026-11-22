#pragma once
// The claim: what a won game sends to the gifter (the RLN membership allocator), and the
// gifter's check of it. It is an auth vector of the allocator (auth_type "bubble-play"):
// verify_auth / produce_auth in the module API, per logos-membership-allocation-crates
// docs/auth-vectors.md.
//
// Nothing here proves a human played: a bot can send the same claim. What the gifter checks
// is the price, every proof of work of the game, plus that the game is fresh and the claim is
// bound to the membership it asks for. The allocator accepts at most 4096 bytes per request,
// so the claim carries the proofs, not the game record (the player's own module replays the
// whole game before it claims).
//
// The game seed:  seed = sha256("bubble/v2/seed" || u64le beacon block id || beacon block hash
//                               || game public key)
// The beacon is a recent LEZ block: no game can be played (or its proofs computed) before
// that block exists. The game key is an Ed25519 key the game makes at its start; it signs the
// claim, so a claim can't be re-aimed at another membership and a game's proofs can't be
// claimed by anyone who doesn't hold the key.
//
// The payload (binary, ~570 bytes for 38 chunks):
//   "BBL1" | u64le beacon id | pubkey(32) | u32le end tick | u8 n |
//   n x (u32le tick | u32le state | u32le nonce) | sig(64)
// sig = Ed25519 over the auth-vector binding digest (docs/auth-vectors.md 4.1):
//   sha256("logos/auth/" || domain || "/" || auth_type || action(32) || u8 len(aud) || aud
//          || extra), extra = sha256(the payload before sig)
// The nullifier is the seed: one claim per game.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "check.h"
#include "sim.h"

namespace bubble {

constexpr const char* kAuthType = "bubble-play";

struct Beacon {
    uint64_t id = 0;
    unsigned char hash[32] = {0};
    int64_t tsMs = 0;
};

struct ChunkProof { uint32_t tick, state, nonce; };

struct Claim {
    uint64_t beaconId = 0;
    unsigned char pub[32] = {0};
    uint32_t end = 0;
    std::vector<ChunkProof> chunks;
    unsigned char sig[64] = {0};
};

// The seed of a game played on this beacon by this key, as 64 hex digits.
std::string gameSeed(const Beacon& b, const unsigned char pub[32]);

std::vector<unsigned char> encodeClaim(const Claim& c, bool withSig);
bool decodeClaim(const std::vector<unsigned char>& bytes, Claim& out, std::string& why);

// The digest the game key signs.
void bindingDigest(const std::string& domain, const std::string& authType, const unsigned char action[32],
                   const std::string& audience, const std::vector<unsigned char>& unsignedPayload, unsigned char out[32]);

// The gifter's rules (the allocator's authVerifiers config for "bubble-play").
struct GifterRules {
    Params params;             // start/goal/chunk give the number of proofs; minTicks/maxTicks
    PowParams pow;
    int64_t maxBeaconAgeS = 3600;
    std::string audience;
    std::string lezUrl = "https://testnet.lez.logos.co";
    int maxClaimsPerHour = 0;  // 0 = no cap
    static bool fromJson(const nlohmann::json& j, GifterRules& out, std::string& why);
};

// The beacon lookup: the block with this id, or false (unknown or unreachable: `why` says).
using BeaconLookup = std::function<bool(uint64_t id, Beacon& out, std::string& why, bool& unreachable)>;

// verify_auth's verdict for one request ({"ok", "reason"?, "nullifier"?, "identity"?, "scheme"?}
// or {"error"} for operator problems). Cheapest checks first, so junk costs the gifter little.
nlohmann::json verifyClaim(const nlohmann::json& request, const BeaconLookup& lookup, int64_t nowMs);

}
