# rln_gifter_module (bridge)

Basecamp's Delivery (delivery_module 0.3.3) keeps its RLN identities in `liblogos_rln_module`
0.12. Its delegated registration (`register_membership` with `delegated: true`) asks a gifter
through a co-located module named `rln_gifter_module`, with `request({gifterPeerId,
gifterMultiaddr, identityCommitment, rate, authType, authProvider, authArgs})`, and reads back
`{leaf_index, tx_hash}` or `{error}`.

This module is `membership_allocation_client_module` (logos-co/logos-membership-allocation-client-module
d8286f4) under that name. It maps gifterPeerId/gifterMultiaddr to allocatorPeerId/allocatorMultiaddr
(discovery off), takes the registry from `authArgs.config`, and requests over the membership
allocation protocol (/logos/paymaster/rln-membership/1.0.0) that the Bubble gifter serves. A reply
of kind `unconfirmed` (sent, not on chain within the gifter's window) goes back as
`{pending: true, tx_hash}`: 0.12 marks a record failed on any `error`, and a failed record can't be
dropped. So a won game registers an identity that Delivery's own keystore made, and Delivery sends
with it.
