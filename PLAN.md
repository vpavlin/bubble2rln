# Bubble → RLN membership: plan (draft, 2026-11-20)

Idea: an agar-style game (start as a small bubble, eat smaller ones, grow). Reaching the
goal lets you claim an RLN membership from a gifter (allocator), based on proof that you played.

## 1. What exists today (researched 2026-11-20)

- **Allocator = the gifter.** `logos-co/logos-membership-allocation-module`, first commit
  2026-11-20; it replaces `logos-rln-gifter`. A funded node runs `serve`, authenticates each
  request with an **auth vector**, and registers the client's identity commitment on chain
  through `rln_lez_module`. The client (`membership_allocation_client_module`) holds no funds,
  and its RLN secret stays local.
- **Auth vectors are pluggable.** The allocator's `authVerifiers` entry can name a module
  (`{"<type>": {"module": "...", "config": {...}}}`). That module implements
  `verify_auth({auth_type, payload_hex, domain, action_hex, config, peer_id})`
  → `{ok, reason?, nullifier?, identity?}`. The client side implements
  `produce_auth({auth_type, domain, action_hex, args})` → `{payload_hex}`.
  Normative contract: `logos-membership-allocation-crates/docs/auth-vectors.md`.
  - `action_hex` already binds the identity commitment and the rate, so a proof can be tied to
    one membership.
  - A returned `nullifier` gets replay protection, once per scope, for free.
- **Built-in vectors:** eth/lez/peer/keycard/logos × allowlist/any/attestation, plus
  `logos-pow-claim`.
  - `logos-pow-claim`: one mined logos-blockchain reward = one membership. It is the closest
    thing to "proof of work", but it needs a mining node.
  - `peer-any`: "open but once per libp2p key". It is trivially sybil-able.
- **Cost of a membership:** gas plus `rate × price` in the native LEZ token, about 7.5e7 units.
  There is no stake and no slashing on the messaging fleet. A membership is 20–600 msgs per
  10-min epoch.
- **Blocker (still open):** we have no funded key on the RLN chain. There is no faucet. Atlas
  can bridge only to the main LEZ testnet, not to where the registry lived in October.

## 2. The honest problem

Nothing that runs on the player's device can prove that *a human* played.

- A replay can prove that *a valid game was won*: fresh seed, legal inputs, the goal reached.
- A bot can produce the same thing, and faster than real time.
- So the game alone is a speed bump, not sybil resistance. It is still worth building as
  **onboarding UX** (people learn what a membership is, and the claim is the reward).

Sybil resistance has to come from a scarce resource. In this design:

- **Work**, woven into the game;
- **Issuance caps** on the allocator;
- **Low rate** for gifted members.

Stronger identity (keycard, mined PoW) stays the upgrade path.

## 3. Proposed design

### 3.1 Deterministic game + replay proof
- One Rust crate `bubble-sim`:
  - fixed-point math, fixed 30 Hz tick;
  - AI bubbles driven by a seeded PRNG.
- The same code runs everywhere:
  - in the game (Basecamp core module, wasm for a web demo);
  - in the verifier.
  - One implementation means the game and the verifier can never disagree.
- **Proof of play** = `seed_inputs` + input log (one heading byte per tick, delta-coded: a
  few KB for 3–5 min).
- The verifier re-simulates (milliseconds) and checks:
  - the goal mass was reached;
  - at least `min_ticks` elapsed;
  - all inputs were legal.
- **Freshness, no server state:**
  - `seed = H("bubble/v1" ‖ beacon_block_hash ‖ action)`.
  - The beacon is a recent LEZ block (the verifier checks its age ≤ N min through `node_url`,
    as pow-claim does).
  - This stops pre-computed wins.
  - It also stops moving a win to another commitment, because `action` contains the
    commitment.
- **Nullifier** = `H(seed)`, so one game claims at most once.

### 3.2 Work as the growth mechanic (the "proof of work" part)
- Growth is **digestion**: each pellet you eat has to be digested by memory-hard hashing
  (argon2id, so GPUs gain less), bound to the seed and the tick.
  - Your device's hash rate caps how fast you can grow.
  - The UI shows it.
- The verifier checks the digestion nonces cheaply (one argon2 call per pellet, or a sampled
  subset).
- Difficulty is operator config. Target: about 3–5 CPU-minutes on a mid-range phone. The
  operator raises it when claims spike.
- Be honest about what this buys: it puts a **price** on a membership (cents of compute), not
  a wall. A farm can still buy memberships at that price.

### 3.3 Bound the damage instead of pretending to stop it
- **Gifted rate is low** (e.g. 20 msgs/epoch, which is enough for chat or sync onboarding).
  The rate is in the action, so the verifier checks it.
- **Allocator issuance cap** per hour/day. That is a hard bound on spam however good the bots
  are.
- **Optional extra gate per claim:** one per peer id, or compose with `keycard-attestation`
  for a full-rate membership.

### 3.4 Modules
- `bubble_auth_module` (Rust Logos module):
  - `produce_auth`: the client turns the finished game record into the payload;
  - `verify_auth`: re-sim + PoW + beacon checks on the allocator.
- `bubble_ui` (Basecamp QML view) + `bubble_core`:
  - the core runs the sim, and the QML draws circles;
  - on a win it calls `liblogos_rln_module` register, delegated, with
    `authType "bubble-play"` and `authProvider "bubble_auth_module"`.
- Allocator config:
  `{"bubble-play": {"module": "bubble_auth_module", "config": {"node_url", "difficulty",
  "goal_mass", "min_ticks", "max_beacon_age_s", "max_rate": 20}}}`.

### 3.5 Stretch: no trusted gifter
- Prove the replay in RISC Zero, which LEZ already uses.
- A LEZ program could then verify the receipt and register directly from a pool, so no
  allocator operator is trusted.
- Defer until the trusted version works.

## 4. Phases

0. **Spike, 2–3 days:**
   - `bubble-sim` crate;
   - headless replay verify;
   - a simple bot, to measure how fast a bot wins;
   - argon2 calibration on laptop, Pi, phone and GPU.

   Output: the real cost per membership for a human vs a bot. Go/no-go on the PoW parameters.
1. **Playable game in Basecamp**, arm64 + darwin as always. A web wasm demo can reuse the
   sim later for events or the site.
2. **`bubble_auth_module` + local e2e:** local LEZ sequencer + RLN registry program (the
   `logos-rln-e2e` harness shows how), allocator + client, win → membership → node sends with
   RLN.
3. **Testnet:**
   - a funded allocator wallet (needs the Logos team, see blocker);
   - issuance cap;
   - a small dashboard of claims.
4. Later: Android once the phone delivery lib does RLN (v0.39); the zk/on-chain variant.

## 5. Questions
1. Audience: newcomers at events and onboarding (cheap and fun is fine), or public sybil
   resistance (then the PoW and caps matter much more)?
2. The rate for game-gifted memberships, 20/epoch?
3. Should we raise a "game/play" vector with the RLN/allocator authors? Their contract fits
   as-is, so a module needs no upstream change.
4. Do memberships expire on the registry? If not, a low-rate gifted membership is forever.
   Check before launch.
