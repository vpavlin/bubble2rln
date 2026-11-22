#include "beacon.h"

#include <curl/curl.h>
#include <sodium.h>
#include <nlohmann/json.hpp>
#include <cstring>
#include <map>
#include <mutex>

using nlohmann::json;

namespace bubble {

namespace {

size_t collect(char* p, size_t s, size_t n, void* ud) {
    auto* out = static_cast<std::string*>(ud);
    if (out->size() + s * n > (8u << 20)) return 0;   // a block reply is far smaller
    out->append(p, s * n);
    return s * n;
}

// One JSON-RPC call; false + why when the node can't be reached or answers an error.
bool rpc(const std::string& url, const std::string& method, const json& params, json& result, std::string& why, bool& unreachable) {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* c = curl_easy_init();
    if (!c) { why = "curl init failed"; unreachable = true; return false; }
    const std::string body = json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}}.dump();
    std::string reply;
    curl_slist* h = curl_slist_append(nullptr, "content-type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &reply);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 8000L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 4000L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    const CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) { why = curl_easy_strerror(rc); unreachable = true; return false; }
    if (code != 200) { why = "HTTP " + std::to_string(code); unreachable = true; return false; }
    json j = json::parse(reply, nullptr, false);
    if (!j.is_object()) { why = "the reply is not JSON"; unreachable = true; return false; }
    if (j.contains("error")) { why = j["error"].dump(); return false; }
    if (!j.contains("result")) { why = "no result"; unreachable = true; return false; }
    result = j["result"];
    return true;
}

bool fetchBlock(const std::string& url, uint64_t id, Beacon& out, std::string& why, bool& unreachable) {
    json r;
    if (!rpc(url, "getBlock", json::array({id}), r, why, unreachable)) return false;
    if (!r.is_string()) { why = "block " + std::to_string(id) + " not found"; return false; }
    const std::string b64 = r;
    std::vector<unsigned char> raw(b64.size());
    size_t len = 0;
    if (sodium_base642bin(raw.data(), raw.size(), b64.data(), b64.size(), nullptr, &len, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0 || len < 80) {
        why = "block " + std::to_string(id) + " does not decode";
        unreachable = true;
        return false;
    }
    uint64_t bid = 0, ts = 0;
    for (int i = 0; i < 8; i++) { bid |= (uint64_t)raw[i] << (8 * i); ts |= (uint64_t)raw[72 + i] << (8 * i); }
    if (bid != id) { why = "the node returned block " + std::to_string(bid) + " for " + std::to_string(id); unreachable = true; return false; }
    out.id = id;
    memcpy(out.hash, &raw[40], 32);
    out.tsMs = (int64_t)ts;
    return true;
}

std::mutex cacheMu;
std::map<std::pair<std::string, uint64_t>, Beacon> cache;

}

bool latestBeacon(const std::string& url, Beacon& out, std::string& why) {
    json r;
    bool unreachable = false;
    if (!rpc(url, "getLastBlockId", json::array(), r, why, unreachable)) return false;
    if (!r.is_number_unsigned()) { why = "getLastBlockId: not a block id"; return false; }
    return beaconById(url, r.get<uint64_t>(), out, why, unreachable);
}

bool beaconById(const std::string& url, uint64_t id, Beacon& out, std::string& why, bool& unreachable) {
    {
        std::lock_guard<std::mutex> l(cacheMu);
        auto it = cache.find({url, id});
        if (it != cache.end()) { out = it->second; return true; }
    }
    if (!fetchBlock(url, id, out, why, unreachable)) return false;
    std::lock_guard<std::mutex> l(cacheMu);
    if (cache.size() > 4096) cache.clear();
    cache[{url, id}] = out;
    return true;
}

}
