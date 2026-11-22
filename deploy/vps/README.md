# The public Bubble gifter (VPS)

Two systemd units on the VPS, under `/root/bubble-gifter`:

- `bubble-chain.service`: a standalone LEZ sequencer (logos-lez-rln's v0.4.0-rc3, built
  optimized on Atlas, stripped) with the RLN registry deployed on it. RPC on 127.0.0.1:3040
  only. Needs `r0vm` 3.0.5 on its PATH (`chain/bin`). State in `chain/home`; the payer wallet
  (`chain/wallet`, gas_limit 10M) was minted and the registry deployed from Atlas with
  `scripts/local-registry.sh deploy` through an ssh tunnel (`chain/registry.env`).
- `bubble-gifter.service`: logosctl 0.3.1 running libp2p_module (fixed key `node.key`, public
  tcp/9101), membership_allocation_module serving the registry with bubble_core as the
  `bubble-play` verifier, and rln_lez_module registering on the chain above.
  `after-start` loads and configures everything once the daemon is up.

- `bubble-beacon.service`: `beacon-proxy.py`, public tcp/3041: only getLastBlockId and getBlock
  of the chain, for the games' beacon blocks (bubble_core's default `BUBBLE_LEZ_URL`).

Players reach the gifter (libp2p) and the beacon proxy. The rest of the chain stays private: with the demo identity the
player needs nothing from it; the player's own RLN identity needs the testnet registry.
