# bubble2rln

A small Basecamp game that ends with an RLN membership.

You start as a small bubble and grow by eating dots and smaller bubbles. What you eat goes to
your stomach first; your computer **digests** it by solving small memory-hard proofs of work
(argon2id), one per 5 mass, and only digested mass counts. If a bigger bubble eats you, you
lose only what you had not digested yet. Reach the goal and the recorded game, with its
proofs, becomes your ticket for an RLN membership from a gifting allocator
(planned: [logos-membership-allocation-module](https://github.com/logos-co/logos-membership-allocation-module)).

The game is deterministic (integers only, its own PRNG), so a verifier replays the record and
checks every proof. Each proof is bound to the game seed, the chunk, and the tick and game state
at which the chunk entered the stomach, so no proof can be computed in advance.

This does not stop bots: playing costs a bot nothing. The proofs put a price on a membership (a
few CPU-minutes); a low gifted rate and an issuance cap bound the damage. See [PLAN.md](PLAN.md).

## Layout

- `core/`: `bubble_core`, the whole game in C++: rules (`src/sim.cpp`), proof of work
  (`src/pow.cpp`), the check (`src/check.cpp`) and the module API (`src/bubble_core_impl.*`).
- `ui/`: `bubble`, a pure QML view that only draws the frames the core returns and steers.
- `core/test/run.sh`: rules tests (reference records, a bot, tampering).
- `scripts/e2e-core.js`: a headless game through `logosctl`.
- `spike/`: the first measurements (historical).

## Build

```sh
cd core && nix build .#lgx-portable -o result-portable
cd ../ui && nix build .#lgx-portable -o result-portable
```

Install both `.lgx` files in Basecamp 0.3.

## Status

Playable in Basecamp (linux-amd64). The claim (an auth-vector verifier for the allocator and
the seed bound to a recent block) is the next step.

## License

MIT or Apache-2.0, at your option.
