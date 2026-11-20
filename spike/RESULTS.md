# Spike results (2026-11-20, jimmy-crib, 4 cores amd64)

## One copy of the rules, same result everywhere
- `sim/sim.js` is integer-only. A bot game replayed in node and in Qt's QJSEngine (the core
  module's and the verifier's engine) ends in the same state hash (`26f270de`, 9407 ticks).
- Replay cost for a 5-minute game (9407 ticks): node 0.19 s, QJSEngine 3.2 s. It started at
  15 s; a pellet grid and memoised radius/speed fixed it without changing the results.
- Record size: ~20 KB JSON (2041 direction changes, 61 digestions). It can be packed to
  ~3 KB binary later.

## A script can win (as expected)
- The greedy bot (`sim/bot.js`) wins ~28% of games, median ~6.7 min of game time, but runs at
  ~0.2 s CPU per game.
- So gameplay alone costs a bot about nothing. The proof of work is the only real cost; the
  issuance cap and the low rate bound the damage.

## Digestion proof of work (argon2id, libsodium, 1 lane, ops 1)
| memory | per hash (1 thread) |
|---|---|
| 4 MiB | 2.7 ms |
| 8 MiB | 5.6 ms |
| 16 MiB | 11.5 ms |
| 32 MiB | 24 ms |

- At 8 MiB, D bits per chunk means 2^D hashes per chunk on average. 61 chunks at D=10 ≈ 62k hashes
  ≈ 6 CPU-min ≈ 1.5 min wall on 4 threads. Verifying = 61 hashes ≈ 0.35 s.
- What that costs an attacker: ~6 CPU-min ≈ well under a cent of cloud compute per membership.
  A speed bump, as the plan says; the issuance cap is the real bound.
- Starting point: D=10, 8 MiB. Tune after playtesting so digestion roughly keeps pace with a
  human eating.
