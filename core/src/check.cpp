#include "check.h"
#include "pow.h"
#include "sim_js.h"
#include <QJSEngine>
#include <QJsonDocument>
#include <QString>

using nlohmann::json;

namespace bubble {

const char* simSource() { return kSimJs; }

static QJSValue toJs(QJSEngine& js, const json& j) {
    return js.toScriptValue(QJsonDocument::fromJson(QByteArray::fromStdString(j.dump())).toVariant());
}

json checkGame(const std::string& seedHex, const json& params, const json& pow, const json& record) {
    unsigned char seed[32];
    if (!seedFromHex(seedHex, seed)) return {{"ok", false}, {"reason", "bad seed"}};
    if (!record.is_object() || !record.contains("digests") || !record["digests"].is_array())
        return {{"ok", false}, {"reason", "bad record"}};
    const int bits = pow.value("bits", 0);
    const size_t memKiB = pow.value("memKiB", 8192);
    if (bits < 0 || bits > 24 || memKiB < 64 || memKiB > 262144) return {{"ok", false}, {"reason", "bad pow params"}};

    // The moves first: cheaper to refuse a wrong game before hashing.
    QJSEngine js;
    QJSValue loaded = js.evaluate(QString::fromUtf8(kSimJs), "sim.js");
    if (loaded.isError()) return {{"ok", false}, {"reason", "sim.js: " + loaded.toString().toStdString()}};
    QJSValue res = js.globalObject().property("replay").call(
        {QJSValue(QString::fromStdString(seedHex)), toJs(js, params), toJs(js, record)});
    if (res.isError()) return {{"ok", false}, {"reason", "replay: " + res.toString().toStdString()}};
    json out = json::parse(QJsonDocument::fromVariant(res.toVariant()).toJson(QJsonDocument::Compact).toStdString(),
                           nullptr, false);
    if (!out.is_object() || !out.value("ok", false)) return out.is_object() ? out : json{{"ok", false}, {"reason", "replay failed"}};

    for (const auto& d : record["digests"]) {
        if (!d.is_array() || d.size() != 3 || !d[1].is_number_unsigned() || !d[2].is_number_unsigned())
            return {{"ok", false}, {"reason", "bad digest entry"}};
        const uint64_t chunk = d[1].get<uint64_t>(), nonce = d[2].get<uint64_t>();
        if (chunk > 0xffffffffull || nonce > 0xffffffffull) return {{"ok", false}, {"reason", "bad digest entry"}};
        if (!digestOk(seed, (uint32_t)chunk, (uint32_t)nonce, bits, memKiB))
            return {{"ok", false}, {"reason", "chunk " + std::to_string(chunk) + " was not digested (proof of work fails)"}};
    }
    return out;
}

}
