#!/usr/bin/env bash
# A local RLN registry for the Bubble gifter: a standalone LEZ sequencer (logos-lez-rln's dev.sh,
# Bedrock mocked) with a payer funded at genesis, and the RLN registry deployed on it.
# Run on the chain host (it needs logos-lez-rln built: the host bins and the guest ELFs).
#
# usage: scripts/local-registry.sh [start|deploy|stop|status]   (default: start + deploy)
# env: LEZ_RLN_REPO (default ~/rln-explore/logos-lez-rln), DEVNET (default ~/rln-explore/devnet),
#      PORT (default 3040)
#
# The wallet config carries gas_limit 10M: rln_lez_module adopts this wallet as it is, and a
# registration needs ~9.1M cycles (the wallet's default limit is far lower: "would_revert").
#
# Writes $DEVNET/registry.env: SEQUENCER, PAYER, CONFIG_ACCOUNT, WALLET (the wallet home a
# gifter's rln_lez_module adopts with LEE_WALLET_HOME_DIR; it holds the payer key).
set -euo pipefail
REPO=${LEZ_RLN_REPO:-$HOME/rln-explore/logos-lez-rln}
DEVNET=${DEVNET:-$HOME/rln-explore/devnet}
PORT=${PORT:-3040}
WALLET=$DEVNET/wallet
export LEE_WALLET_HOME_DIR=$WALLET LEE_WALLET_PASSWORD="" RUST_LOG=${RUST_LOG:-warn}
BIN=$REPO/lez-rln/target/debug

up() { python3 - "$PORT" <<'PY'
import json, sys, urllib.request
try:
    req = urllib.request.Request(f"http://127.0.0.1:{sys.argv[1]}/", data=json.dumps({"jsonrpc": "2.0", "id": 1, "method": "getLastBlockId", "params": []}).encode(), headers={"content-type": "application/json"})
    print(json.load(urllib.request.urlopen(req, timeout=3))["result"])
except Exception:
    sys.exit(1)
PY
}

start() {
    if up >/dev/null; then echo "sequencer already up on $PORT"; return; fi
    mkdir -p "$WALLET"
    [ -f "$WALLET/wallet_config.json" ] || cat > "$WALLET/wallet_config.json" <<JSON
{ "sequencer": { "sequencer_addr": "http://127.0.0.1:$PORT/" }, "seq_poll_timeout": "30s",
  "seq_tx_poll_max_blocks": 15, "seq_poll_max_retries": 10, "seq_block_poll_max_amount": 100,
  "gas_limit": 10000000 }
JSON
    local lines
    lines=$("$BIN/mint_payer")
    PAYER=$(echo "$lines" | sed -n 1p); PAYER_PK=$(echo "$lines" | sed -n 2p)
    echo "payer $PAYER (public key $PAYER_PK)"
    # Optimized, with 2 s blocks: the debug build at dev.sh's 1 s blocks answers RPC in ~15 s,
    # and the gifter's wallet times out. (A restart wipes the chain: deploy again.)
    LEZ_RLN_PORT=$PORT LEZ_RLN_SEQUENCER_HOME=$DEVNET/sequencer LEZ_RLN_GENESIS_FUND=$PAYER_PK \
        LEZ_RLN_BLOCK_TIME=${BLOCK_TIME:-2s} CARGO_PROFILE_DEV_OPT_LEVEL=${OPT_LEVEL:-3} \
        nohup "$REPO/dev.sh" > "$DEVNET/sequencer.log" 2>&1 < /dev/null &
    echo "sequencer starting (log $DEVNET/sequencer.log)"
    for i in $(seq 1 600); do up >/dev/null && break; sleep 3; done
    echo "sequencer up at block $(up)"
}

deploy() {
    local out
    PAYER=$(LEE_WALLET_HOME_DIR=$WALLET "$BIN/mint_payer" | sed -n 1p)
    # The registry's tree id: one per devnet, kept so a re-run reuses the deployment.
    [ -f "$DEVNET/tree_id" ] || python3 -c "import os; print(os.urandom(32).hex())" > "$DEVNET/tree_id"
    out=$(cd "$REPO/lez-rln" && LEZ_RLN_PAYER=$PAYER LEZ_RLN_TREE_ID_HEX=$(cat "$DEVNET/tree_id") "$BIN/run_setup")
    echo "$out"
    CONFIG=$(echo "$out" | sed -n 's/^Config account: *//p')
    cat > "$DEVNET/registry.env" <<EOF
SEQUENCER=http://127.0.0.1:$PORT/
PAYER=$PAYER
CONFIG_ACCOUNT=$CONFIG
WALLET=$WALLET
TREE_ID=$(cat "$DEVNET/tree_id")
EOF
    echo "wrote $DEVNET/registry.env"; cat "$DEVNET/registry.env"
}

stop() { pid=$(ss -ltnp "sport = :$PORT" 2>/dev/null | sed -n 's/.*pid=\([0-9]*\).*/\1/p' | head -1); [ -n "$pid" ] && kill "$pid" && echo "stopped $pid"; }

case "${1:-all}" in
    start) start ;; deploy) deploy ;; stop) stop ;; status) up && cat "$DEVNET/registry.env" ;;
    all) start; deploy ;;
esac
