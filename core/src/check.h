#pragma once
// The full check of a played game, shared by the core (before claiming) and the verifier:
// replay the record with the rules (sim.h), then check every digestion nonce (pow.h).
#include <string>
#include <nlohmann/json.hpp>
#include "sim.h"

namespace bubble {

struct PowParams {
    int bits = 10;
    size_t memKiB = 8192;
    nlohmann::json toJson() const { return {{"bits", bits}, {"memKiB", memKiB}}; }
};

// {"ok": bool, "reason", "ticks", "body", "chunks", "deaths", "hash"}. Thread-safe; a long game
// takes a few hundred ms plus one argon2id per chunk.
nlohmann::json checkGame(const std::string& seedHex, const Params& params, const PowParams& pow, const nlohmann::json& record);
}
