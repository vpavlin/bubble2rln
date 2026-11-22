// Replays one reference game (no proof of work): the work a ZK prover would have to prove.
#include "sim.h"
#include <fstream>
#include <sstream>
#include <cstdio>
using namespace bubble; using nlohmann::json;
int main(int argc, char** argv) {
    std::ifstream f(argv[1]); std::stringstream ss; ss << f.rdbuf();
    json v = json::parse(ss.str())[atoi(argv[2])];
    Params p; Params::fromJson(v["params"], p);
    json rj = v["rec"]; rj["v"] = kSimVersion; rj["digests"] = json::array();
    for (const auto& d : v["rec"]["digests"]) rj["digests"].push_back({d[0], d[1], 0});
    Record rec; std::string why; Record::fromJson(rj, rec, why);
    json r = replay(v["seed"], p, rec);
    printf("%s ticks %lld\n", r.value("ok", false) ? "ok" : "FAIL", (long long)r.value("ticks", 0));
}
