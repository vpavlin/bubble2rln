// Digestion hash benchmark: argon2id, 8 MiB, ops 1, 1 lane (bubble's PoW), 1..N threads.
#include <sodium.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static volatile int stop_;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void* work(void* arg) {
    long* n = arg; unsigned char pw[64] = {0}, salt[16] = {1}, h[32];
    while (!stop_) { memcpy(pw, n, sizeof *n); crypto_pwhash(h, 32, (char*)pw, 64, salt, 1, 8192 * 1024, crypto_pwhash_ALG_ARGON2ID13); (*n)++; }
    return 0;
}
int main(int argc, char** argv) {
    if (sodium_init() < 0) return 1;
    int maxT = argc > 1 ? atoi(argv[1]) : (int)sysconf(_SC_NPROCESSORS_ONLN);
    double secs = argc > 2 ? atof(argv[2]) : 15;
    printf("cpus %ld\n", sysconf(_SC_NPROCESSORS_ONLN));
    for (int t = 1; t <= maxT; t = t < maxT && t * 2 > maxT ? maxT : t * 2) {
        pthread_t th[64]; long n[64] = {0}; stop_ = 0;
        double t0 = now();
        for (int i = 0; i < t; i++) pthread_create(&th[i], 0, work, &n[i]);
        while (now() - t0 < secs) usleep(100000);
        stop_ = 1; long tot = 0;
        for (int i = 0; i < t; i++) { pthread_join(th[i], 0); tot += n[i]; }
        double rate = tot / (now() - t0);
        // a game = 38 chunks; a chunk needs 2^bits hashes on average
        printf("threads %2d: %7.1f hashes/s  | 38 chunks: bits 10 %5.0f s, bits 11 %5.0f s, bits 12 %5.0f s\n",
               t, rate, 38 * 1024 / rate, 38 * 2048 / rate, 38 * 4096 / rate);
        fflush(stdout);
        if (t == maxT) break;
    }
}
