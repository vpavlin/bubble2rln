# Bubble → RLN membership: the claim (proposal, 2026-11-22)

## What we can and can't prove

Nothing a player's machine sends proves that a human played: a bot can send exactly the same
thing. A full replay of the game proves only that *a valid game was won*, which costs a bot
nothing. So the design does not try to tell humans from bots. It sets a **price** per
membership that a person pays happily (a few minutes of fun) and that makes farming at scale not
worth it, and it caps what the gifter gives away.

The allocator takes requests of at most 4096 bytes and calls our verifier inline for each one.
The gifter is not RLN-protected itself, so the claim must be small and cheap to check, and junk
must be cheap to reject. Everything else (the replay, the game itself) stays on the player's
side.

## The claim

- **Game seed** = sha256("bubble/v2/seed" ‖ LEZ block id ‖ LEZ block hash ‖ game public key).
  - The block is the newest LEZ block when the game starts (the "beacon"): no game, and no proof
    of work, can be prepared before that block exists.
  - The game makes its own Ed25519 key at the start. It signs the claim, so a claim can't be
    re-aimed at another membership, and nobody without the key can claim the game.
- **Digestion proofs**: 38 argon2id proofs (8 MiB, 1 pass), one per 5 mass digested, each bound
  to the seed, the chunk, and the tick and game state at which the chunk was swallowed.
- **Payload** (binary, 569 bytes): beacon id, game key, game length, 38 × (tick, state, nonce),
  and the signature over the allocator's binding digest (domain, auth type, action = the
  identity commitment and rate, audience, and the payload itself).
- **Nullifier** = the seed: one membership per game, enforced by the allocator's nullifier store.

## The gifter's check (auth type `bubble-play`, `bubble_core.verify_auth`)

Cheapest first:

1. shape: the right number of proofs, game length within the rules, proofs in game order;
2. the game key's signature over this membership request (microseconds);
3. the beacon: a real LEZ block (read from the operator's sequencer, cached), at most
   `maxBeaconAgeS` old, and at least as old as the game claims to have lasted;
4. every proof of work, in random order, so a partly computed claim fails early;
5. the hourly cap (`maxClaimsPerHour`): the hard bound on what bots can take.

Measured on crib: an honest claim checks in **~213 ms** (38 argon2id); a claim with no work in
it is refused in **~6 ms**.

## The price

Hashes per second, argon2id 8 MiB (measured 2026-11-22):

| Machine | 1 thread | 2 threads | 4 threads |
|---|---|---|---|
| crib (Intel i5, 4 cores) | 177 | 300 | 274 |
| Atlas (Intel i5-7600) | 210 | 272 | 287 |
| pi5 (Cortex-A76) | 87 | 151 | 192 |

The hash is memory-bound: more than 2 threads gains little on these machines.

At difficulty *bits*, a game costs 38 × 2^bits hashes on average:

| bits | hashes | crib (2 thr) | pi5 (2 thr) | a bot, per membership |
|---|---|---|---|---|
| 10 | 38,912 | ~2.2 min | ~4.3 min | ~3.7 CPU-min |
| 11 | 77,824 | ~4.3 min | ~8.6 min | ~7.3 CPU-min |
| 12 | 155,648 | ~8.6 min | ~17 min | ~15 CPU-min |

The proofs are computed while the player plays (digestion starts when food is swallowed), so at
bits 10 most of the work hides in a 3–5 minute game on a laptop.

A CPU-minute costs a bot a fraction of a cent, so the PoW alone does not stop a determined
farm. What bounds the damage:

- **the hourly cap** on the gifter (e.g. 20 memberships/hour): bots can at most take a share of
  it, at a CPU price per slot;
- **the rate**: 100 messages per 10-minute epoch (the registry's minimum);
- **expiry**: registry memberships have an active duration and a grace period, so gifts end
  (the testnet registry's durations still to check).

## Measured end to end (crib, two logosctl sessions, libp2p)

Player: libp2p_module 1.1.0 + membership_allocation_client_module 0.1.0 + bubble_core.
Gifter: libp2p_module 1.1.0 + membership_allocation_module 0.1.0 + bubble_core as the
`bubble-play` verifier.

- A game seeded from a real LEZ testnet block, won, checked by the player's own replay.
- The claim went over libp2p to the allocator, which called `bubble_core.verify_auth`; the
  claim was accepted and its nullifier (the game seed) recorded.
- The same claim aimed at another membership is refused (signature).
- The same flow from inside the module (`bubble_core.claim` → client → libp2p → gifter →
  `verify_auth` accepted → `register_member`) failed only at the registry module, which was not
  loaded; the allocator reported `not_submitted` and released the nullifier.
- Junk and replays: a claim aimed at another membership is refused (signature); a claim with no
  work is refused in ~6 ms.

Registration itself needs `rln_lez_module` and a funded payer.

**Local registry (Atlas):** logos-lez-rln's `tools/e2e-local.sh` passes: a standalone LEZ
v0.4.0-rc3 sequencer with a payer funded at genesis, deploy, three registrations, an RLN proof
against the on-chain root, slash, extend, erase (6 min). `rln_lez_module` 6.1.1 pins the same LEZ
tag and takes `LEZ_RLN_SEQUENCER` + `LEZ_RLN_PAYER_KEY`, so a gifter can register on it.

**Testnet:** the registry the delivery module's `logos.test` preset uses (`J3NXPW…`,
deployment testnet-v03-z3) is on the main LEZ testnet (channel 0101…01). In October Atlas's
L1 node could bridge only to that zone, while the registry lived elsewhere; now they are the
same. Atlas's mined testnet balance (~1.26e9 units in October) covers ~15 registrations at
~74M each. Not done: bridging moves real (testnet) funds and is your call.

**Local chain end to end (2026-11-22, `scripts/local-registry.sh` on Atlas + `scripts/claim-e2e.sh`
on crib, through an ssh tunnel to the sequencer):** a game seeded from a LEZ testnet block, won,
claimed from inside bubble_core → client → libp2p → allocator → `verify_auth` accepted →
`rln_lez_module.register_member` on the local registry → **granted, leaf 0**, membership
`active`, rate 100, active 30 days + 7 days grace (the registry's durations, the same shape as
the testnet registry). The same game claimed again for another identity: refused in 0.3 s,
`credential already used`. Gotcha: the wallet the gifter's `rln_lez_module` adopts must carry
`gas_limit: 10000000` (a registration needs ~9.1M cycles; without it: `would_revert`).

## For Wednesday: two ways to demo

**A. Local chain (no funds, fully ours).** The LEZ sequencer + RLN registry run on Atlas; the
gifter (allocator + rln_lez_module + bubble_core) on crib or Atlas; the player in Basecamp.
Win → Claim → the gifter checks the proofs and registers the membership on the local chain →
the game shows "membership #N registered". The membership is real on that chain but not usable
on logos.test, and the player's Basecamp needs to reach the local sequencer only for the
beacon (or the gifter serves it).

**B. Testnet (needs the bridge).** Bridge part of Atlas's mined testnet tokens to the LEZ
testnet and fund the gifter's payer. The player's own `rln_module` makes the RLN identity and
claims with the game (`register_membership` with `delegated`, `auth_type` bubble-play,
`auth_provider` bubble_core), so the membership lands in the keystore their delivery node uses:
win → claim → send messages on logos.test with RLN. The real thing, but it depends on the
bridge working (≈1 h to finality) and on testnet stability on the day.

Recommendation: build A as the safe demo (Monday), try B's bridge in parallel; if B works by
Tuesday, demo B and keep A as the fallback.

Settings for the demo: 38 proofs at bits 10 (a 3–5 minute game on a laptop), beacon age ≤ 1 h,
20 memberships/hour cap, rate 100/epoch.
