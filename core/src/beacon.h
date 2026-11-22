#pragma once
// The game's beacon: a recent LEZ block, read from a sequencer's JSON-RPC (getLastBlockId,
// getBlock). getBlock returns the block borsh-encoded in base64; its header starts with
// u64le block_id, prev_block_hash(32), hash(32), u64le timestamp (ms).
#include <string>
#include "claim.h"

namespace bubble {
// The newest block. false + why on failure (unreachable, bad reply).
bool latestBeacon(const std::string& lezUrl, Beacon& out, std::string& why);
// Block `id`. unreachable = the node failed (vs. the block is unknown). Cached per URL and id.
bool beaconById(const std::string& lezUrl, uint64_t id, Beacon& out, std::string& why, bool& unreachable);
}
