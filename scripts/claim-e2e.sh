#!/usr/bin/env bash
# The claim end to end, headless on one machine: a gifter session (libp2p + the allocator with
# bubble_core as its "bubble-play" verifier [+ rln_lez_module]) and a player session (libp2p +
# the allocator client + bubble_core). The player wins a short game, claims, and the script
# polls claimStatus until the gifter answers.
#
# usage: scripts/claim-e2e.sh [up|play|claim|down]   (default: all of up, play, claim)
# env:
#   ALLOC_LGX, CLIENT_LGX, CORE_LGX   the packages (defaults: the local builds)
#   REGISTRY      the registry config account (base58); default the testnet one
#   LEZ_URL       the sequencer for the game's beacon block (default the LEZ testnet)
#   RLN_LEZ_LGX   rln_lez_module to load on the gifter (unset = no registration: the claim is
#                 verified, then waits for a registry module that never comes)
#   GIFTER_ENV    extra env for the gifter daemon (e.g. LEZ_RLN_SEQUENCER=… LEZ_RLN_PAYER_KEY=…)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CTL=${CTL:-$HOME/port-0.3/ctl.sh}
ALLOC_LGX=${ALLOC_LGX:-$(ls $HOME/rln-explore/logos-membership-allocation-module/result-portable/*.lgx)}
CLIENT_LGX=${CLIENT_LGX:-$(ls $HOME/rln-explore/logos-membership-allocation-client-module/result-portable/*.lgx)}
CORE_LGX=${CORE_LGX:-$HERE/../core/result-portable/logos-bubble_core-module-lib.lgx}
REGISTRY=${REGISTRY:-J3NXPWUspX2zqJU3tUbR6MHmm83aJc5AzvidEaBBKnat}
LEZ_URL=${LEZ_URL:-https://testnet.lez.logos.co}
GOAL=${GOAL:-40}; MIN_TICKS=${MIN_TICKS:-300}; BITS=${BITS:-8}
STATE=${STATE:-$HOME/port-0.3/claim-e2e.state}

j() { python3 -c "import json,sys; print(json.dumps(json.loads(sys.argv[1])))" "$1"; }
call() { local s=$1; shift; "$CTL" "$s" -j call "$@" | tail -1; }
result() { python3 -c 'import json,sys; o=json.loads(sys.stdin.read()); r=o.get("result"); print(r if isinstance(r,str) else json.dumps(r))'; }

up() {
    env ${GIFTER_ENV:-} "$CTL" gift daemon start --detach | tail -1
    BUBBLE_LEZ_URL=$LEZ_URL BUBBLE_GOAL=$GOAL BUBBLE_MIN_TICKS=$MIN_TICKS BUBBLE_POW_BITS=$BITS BUBBLE_PAUSE_MS=5000 \
        "$CTL" play daemon start --detach | tail -1
    for s in gift play; do "$CTL" $s package install -y libp2p_module >/dev/null; "$CTL" $s install "$CORE_LGX" -y >/dev/null; done
    "$CTL" gift install "$ALLOC_LGX" -y >/dev/null
    "$CTL" play install "$CLIENT_LGX" -y >/dev/null
    if [ -n "${RLN_LEZ_LGX:-}" ]; then "$CTL" gift install "$RLN_LEZ_LGX" -y >/dev/null; fi
    local port=9101 peer
    for s in gift play; do
        "$CTL" $s module load libp2p_module >/dev/null
        call $s libp2p_module createNode "str:{\"addrs\":[\"/ip4/127.0.0.1/tcp/$port\"],\"transport\":\"tcp\",\"mountGossipsub\":false,\"mountKad\":false,\"mountServiceDiscovery\":false}" >/dev/null
        call $s libp2p_module start >/dev/null
        "$CTL" $s module load bubble_core >/dev/null
        port=$((port + 1))
    done
    peer=$(call gift libp2p_module getNodeInfo str:PeerId | python3 -c 'import json,sys; print(json.load(sys.stdin)["result"]["value"])')
    if [ -n "${RLN_LEZ_LGX:-}" ]; then "$CTL" gift module load rln_lez_module >/dev/null; fi
    "$CTL" gift module load membership_allocation_module >/dev/null
    "$CTL" play module load membership_allocation_client_module >/dev/null
    local serve
    serve=$(python3 -c "import json; print(json.dumps({'config': '$REGISTRY', 'advertise': False,
        'consumedNullifiersPath': '$HOME/port-0.3/sess-gift/nullifiers.json',
        'authVerifiers': {'bubble-play': {'module': 'bubble_core', 'config': {'goal': $GOAL, 'minTicks': $MIN_TICKS, 'bits': $BITS,
            'maxBeaconAgeS': 3600, 'maxClaimsPerHour': 20, 'lezUrl': '$LEZ_URL'}}}}))")
    echo "gifter serve: $(call gift membership_allocation_module serve "str:$serve" | result)"
    echo "peer=$peer" > "$STATE"
}

play() {
    local out
    out=$(KEEP=1 node "$HERE/e2e-core.js" play | tee /dev/stderr)
    echo "$out" | sed -n 's/^kept game \([0-9a-f]*\) .*/game=\1/p' >> "$STATE"
}

claim() {
    . "$STATE"
    local idc opts r
    idc=$(python3 -c "import os; print((os.urandom(31) + b'\x00').hex())")   # a valid BN254 element, little-endian
    opts=$(python3 -c "import json; print(json.dumps({'config': '$REGISTRY', 'identityCommitment': '$idc', 'rate': 100,
        'allocatorPeerId': '$peer', 'allocatorMultiaddr': '/ip4/127.0.0.1/tcp/9101'}))")
    echo "claim: $(call play bubble_core claim "str:$game" "str:$opts" | result)"
    for i in $(seq 1 60); do
        r=$(call play bubble_core claimStatus "str:$game" | result)
        case "$r" in *'"state":"asking"'*) sleep 5 ;; *) break ;; esac
    done
    echo "claim status: $r"
}

down() { for s in gift play; do "$CTL" $s daemon stop | tail -1; done; }

case "${1:-all}" in
    up) up ;; play) play ;; claim) claim ;; down) down ;;
    all) up; play; claim ;;
esac
