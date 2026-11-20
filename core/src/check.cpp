#include "check.h"
#include "pow.h"

using nlohmann::json;

namespace bubble {

json checkGame(const std::string& seedHex, const Params& params, const PowParams& pow, const json& record) {
    unsigned char seed[32];
    if (!seedFromHex(seedHex, seed)) return {{"ok", false}, {"reason", "bad seed"}};
    if (pow.bits < 0 || pow.bits > 24 || pow.memKiB < 64 || pow.memKiB > 262144) return {{"ok", false}, {"reason", "bad pow params"}};
    Record rec;
    std::string why;
    if (!Record::fromJson(record, rec, why)) return {{"ok", false}, {"reason", why}};
    if (record.contains("seed") && record["seed"] != seedHex) return {{"ok", false}, {"reason", "the record is for another seed"}};
    // The moves first: cheaper to refuse a wrong game before hashing.
    json out = replay(seedHex, params, rec);
    if (!out.value("ok", false)) return out;
    for (const auto& [t, chunk, nonce] : rec.digests) {
        (void)t;
        if (chunk < 0 || chunk > 0xffffffffll || !digestOk(seed, (uint32_t)chunk, nonce, pow.bits, pow.memKiB))
            return {{"ok", false}, {"reason", "chunk " + std::to_string(chunk) + " was not digested (proof of work fails)"}};
    }
    return out;
}

}
