# The public gifter on the LEZ testnet (since 2026-11-23)

Same peer (16Uiu2HAkx4HRjCAN4HCWZyqXn7ZxnDGhxQhcpDuBhANBhx3VXP88) and address
(/ip4/128.140.55.128/tcp/9101) as the devnet gifter it replaced (bubble-gifter.service, now
disabled). It registers into the testnet registry Logos Delivery's logos.test preset trusts:
config J3NXPWUspX2zqJU3tUbR6MHmm83aJc5AzvidEaBBKnat (logos:testnet:fd322d32…).

- Session: /root/bubble-testnet (logosctl 0.3.1; HOME and LOGOSCTL_CONFIG_DIR inside it).
- Modules: libp2p_module 1.1.0 (catalog), bubble_core (the verifier), the catalog's
  liblogos_lez_rln_module 5.1.0 (the LEZ v0.3.1 zone testnet runs; logos-lez-rln main / v0.4
  don't speak to it), and membership_allocation_module built with MEMBERSHIP_REGISTRY_MODULE
  support (it hard-coded rln_lez_module).
- Payer: Public/6xhp9WpQJmNi3oC34XeUcbhJbuUqEVRbjTNpB35ZMw8x, funded by atlas/lbn. One
  membership costs ~74.3M (1M price + fees); the module wants ~182.8M free per submit.
- The beacon (a game's freshness) stays on the VPS devnet (bubble-chain + bubble-beacon :3041):
  testnet blocks are slow and may change for hours after they appear.
- Cap: MAX_PER_HOUR in env.sh (6).

Traps met on the way:
- The 5.1.0 wallet can't read a storage.json written by the v0.4 wallet tools: import the key
  with LEZ_RLN_PAYER_KEY into a fresh home instead.
- The v0.4 storage.json has two secrets per account; the account's key is `ssk` (its x-only
  public key hashes to the account id), not `sk`. Importing `sk` gives another account and
  register fails with "Fee payer is a non-signing account of this transaction".
- When the module can't send, register_member answers "" and the allocator keeps the claim's
  nullifier (that game can't be claimed again).
