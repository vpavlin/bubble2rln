#pragma once
// Endless-mode scores, published on Logos Delivery and checked by every Bubble that receives one.
//
// A score message carries everything a stranger needs to check it without trusting the sender:
// the LEZ block the game was seeded from, the game's own Ed25519 key, the whole game record
// (compact binary, base64) and a signature over all of it. A receiver rebuilds the seed from the
// block and the key, replays the record under the endless rules (sim.h) with every proof of work
// (check.h), and lists the score only if the replay ends at exactly that size. The block is looked
// up on the chain, so a seed can't be made up either.
//
// What it can't prove: that a person played (a good bot makes valid records too). Sending needs
// an RLN proof (Delivery does that), so posting is rate-limited per membership but anonymous.
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "claim.h"
#include "sim.h"

namespace bubble {

constexpr const char* kScoreTopic = "/bubble/1/scores/json";
constexpr int kScoreMsgVersion = 2;   // 2 adds the player's name (signed); 1 is still accepted
constexpr size_t kNameMaxChars = 24;
constexpr size_t kScoreMaxBytes = 120 * 1024;   // a message, base64 included (Delivery caps ~150 KiB)

// The rules every score is checked against: the endless defaults and the default proof of work,
// independent of any environment overrides (so every receiver agrees).
Params scoreParams();
PowParams scorePow();

// The record in a compact binary form (varints), and back.
std::vector<unsigned char> encodeRecordBin(const Record& r);
bool decodeRecordBin(const std::vector<unsigned char>& b, Record& out, std::string& why);

std::string toBase64(const std::vector<unsigned char>& b);
bool fromBase64(const std::string& s, std::vector<unsigned char>& out);

// A player's name as it may be posted: valid UTF-8, no control characters, trimmed, at most
// kNameMaxChars characters (longer is cut). Anything goes otherwise; it's a nickname, not an identity.
std::string cleanName(const std::string& raw);

// A signed score message for a finished, checked endless game. The name is signed with the rest,
// so nobody passing the message on can change it.
nlohmann::json makeScore(const Beacon& beacon, const unsigned char pk[32], const unsigned char sk[64],
                         const Record& rec, int64_t score, int64_t ticks, const std::string& ended, int64_t whenMs,
                         const std::string& name = "");

// Checks a received score message: {"ok", "reason", "score", "ticks", "ended", "id", "when", "name"}.
// id identifies the game (stable across republishing), for deduplication.
// pow: the proof-of-work rules (scorePow() everywhere but tests).
nlohmann::json verifyScore(const nlohmann::json& msg, const BeaconLookup& lookup, const PowParams& pow = scorePow());

}
