#!/usr/bin/env bash
# core/src/sim.js is the one copy of the rules; the view ships it too. Copy it, or check (--check).
set -e
cd "$(dirname "$0")/.."
if [ "$1" = "--check" ]; then cmp -s core/src/sim.js ui/qml/sim.js || { echo "ui/qml/sim.js differs from core/src/sim.js: run scripts/sync-sim.sh"; exit 1; }; exit 0; fi
cp core/src/sim.js ui/qml/sim.js
