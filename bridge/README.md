# logos-membership-allocation-client-module

Logos module `membership_allocation_client_module`: the client of the RLN membership
allocator. It asks an allocator, over libp2p, to register an RLN membership
on the client's behalf, so the client holds no funds and its RLN identity secret never leaves its
machine. The allocator side is
[`logos-membership-allocation-module`](https://github.com/logos-co/logos-membership-allocation-module).

## Methods

- `request(args_json)`: request a membership for an identity commitment. Allocators
  advertising the (registry, auth type) pair through libp2p service discovery are asked first,
  then the configured `allocatorPeerId`/`allocatorMultiaddr` pair if any. The auth payload is given raw
  (`authPayload`) or produced by an `authProvider` module that implements the `auth_vector`
  producer contract; an auth type with neither needs no payload. This module knows no auth
  vector by name. A failure carries a `kind` to branch on.
- `discover(args_json)`: the allocators currently advertising a (registry, auth type) pair.

The argument and reply shapes are documented on the trait in `rust-lib/src/lib.rs`.

## Dependencies

- `libp2p_module` (declared; loaded first by the host).
- An auth producer module, when `authProvider` names one. It is opened on first use.
- Shared crates, pinned by commit: `membership_allocation_action`, `paymaster_client`,
  `paymaster_lp` and `paymaster_wire` from `logos-membership-allocation-crates`.

## Build and test

```sh
nix build .#lgx-portable      # the release package
nix run .#generate            # stage rust-lib/generated/ and logos-rust-sdk-src/ for cargo
cd rust-lib && cargo test && cargo clippy --all-targets -- -D warnings
```

The tests enable the `test-fake` feature of `paymaster_lp` as a dev-dependency; the
plugin build links the real `lp_*` bindings (`cargo check --lib` compiles that path).
