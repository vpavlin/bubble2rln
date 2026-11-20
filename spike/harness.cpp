// Spike harness: (1) replay a record through QJSEngine, as the core module and the verifier will;
// (2) benchmark the digestion proof of work (argon2id via libsodium).
#include <QCoreApplication>
#include <QFile>
#include <QJSEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <sodium.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>
#include <vector>

static QString readAll(const char* p) { QFile f(p); f.open(QIODevice::ReadOnly); return QString::fromUtf8(f.readAll()); }

// h = argon2id("bubble/v1/digest" || seed || chunk || nonce), salt = sha256("bubble/v1/salt" || seed)[0..16]
static bool digestHash(const unsigned char seed[32], uint32_t chunk, uint32_t nonce, size_t memKiB, unsigned char out[32]) {
    unsigned char pw[16 + 32 + 8]; memcpy(pw, "bubble/v1/digest", 16); memcpy(pw + 16, seed, 32);
    for (int i = 0; i < 4; i++) { pw[48 + i] = chunk >> (8 * i); pw[52 + i] = nonce >> (8 * i); }
    unsigned char sb[14 + 32], salt[32]; memcpy(sb, "bubble/v1/salt", 14); memcpy(sb + 14, seed, 32);
    crypto_hash_sha256(salt, sb, sizeof sb);
    return crypto_pwhash(out, 32, (const char*)pw, sizeof pw, salt, 1, memKiB * 1024, crypto_pwhash_ALG_ARGON2ID13) == 0;
}
static int zeroBits(const unsigned char h[32]) { int n = 0; for (int i = 0; i < 32; i++) { if (!h[i]) { n += 8; continue; } n += __builtin_clz(h[i]) - 24; break; } return n; }

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (sodium_init() < 0) return 1;
    if (argc > 2 && !strcmp(argv[1], "replay")) {
        QJSEngine js;
        QJSValue r = js.evaluate(readAll(argv[2]), "sim.js");
        if (r.isError()) { printf("load: %s\n", qPrintable(r.toString())); return 1; }
        QJsonDocument d = QJsonDocument::fromJson(readAll(argv[3]).toUtf8());
        QJSValue o = js.toScriptValue(d.object().toVariantMap());
        QElapsedTimer t; t.start();
        QJSValue res = js.globalObject().property("replay").call({o.property("seed"), o.property("params"), o.property("rec")});
        printf("qjsengine replay: %s in %lld ms (node hash %s)\n", qPrintable(QString::fromUtf8(QJsonDocument::fromVariant(res.toVariant()).toJson(QJsonDocument::Compact))), (long long)t.elapsed(), qPrintable(o.property("hash").toString()));
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "pow")) {
        unsigned char seed[32]; randombytes_buf(seed, 32); unsigned char h[32];
        for (size_t mem : {4096, 8192, 16384, 32768}) {
            QElapsedTimer t; t.start(); int n = 20;
            for (int i = 0; i < n; i++) digestHash(seed, 0, i, mem, h);
            printf("argon2id %5zu KiB: %.1f ms/hash (1 thread)\n", mem, t.elapsed() / (double)n);
        }
        // Solve 8 chunks at D bits on all cores, as the client would.
        int D = argc > 2 ? atoi(argv[2]) : 8; size_t mem = argc > 3 ? atoi(argv[3]) : 8192;
        unsigned threads = std::thread::hardware_concurrency();
        QElapsedTimer t; t.start(); std::atomic<uint32_t> next{0}; std::atomic<long> tries{0};
        std::vector<std::thread> ws;
        for (unsigned w = 0; w < threads; w++) ws.emplace_back([&] {
            for (uint32_t c; (c = next++) < 8;) { unsigned char hh[32]; for (uint32_t n = 0;; n++) { tries++; digestHash(seed, c, n, mem, hh); if (zeroBits(hh) >= D) break; } }
        });
        for (auto& w : ws) w.join();
        double s = t.elapsed() / 1000.0;
        printf("D=%d mem=%zuKiB: 8 chunks in %.1f s on %u threads (%ld hashes, %.2f s/chunk wall)\n", D, mem, s, threads, (long)tries, s / 8);
        return 0;
    }
    printf("usage: harness replay sim.js record.json | harness pow [D] [memKiB]\n");
    return 2;
}
