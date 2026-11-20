#pragma once
// The full check of a played game, shared by the core (before claiming) and the verifier:
// replay the record with sim.js in a QJSEngine, then check every digestion nonce.
#include <string>
#include <nlohmann/json.hpp>

namespace bubble {
// params: the sim params {start, goal, chunk, minTicks, maxTicks}; pow: {bits, memKiB}.
// Returns {"ok": bool, "reason": "...", "ticks", "body", "chunks"}. Call from any one thread at a
// time per call (it makes its own engine); it takes seconds for a long game.
nlohmann::json checkGame(const std::string& seedHex, const nlohmann::json& params, const nlohmann::json& pow,
                         const nlohmann::json& record);
const char* simSource();
}
