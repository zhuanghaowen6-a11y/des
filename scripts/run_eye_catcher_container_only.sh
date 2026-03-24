#!/bin/bash
# Orchestrate an "eye-catcher" container-only fault-injection experiment and generate convergence curve.
#
# This script:
#  1) runs test_container_only.sh (in background) to start containers and later collect logs
#  2) waits for RESULT_DIR/meta/t0_epoch.txt to appear
#  3) injects a single ToR-Agg link failure (after warmup)
#  4) waits for test_container_only.sh to finish
#  5) computes convergence CDF curve for a target prefix
#
# Usage:
#   ./scripts/run_eye_catcher_container_only.sh <TEST_DURATION> <TOPOLOGY_MODE> <TOR_ID> <TARGET_PREFIX> [WARMUP_S] [STABILIZE_S]
# Example:
#   ./scripts/run_eye_catcher_container_only.sh 180 fat-tree-k8-64 41 192.168.41.0/24 60 10
#
set -euo pipefail

TEST_DURATION=${1:-180}
TOPOLOGY_MODE=${2:-fat-tree-k8-64}
TOR_ID=${3:-41}
KEEP_AGG_ID=${4:-17}
TARGET_PREFIX=${5:-192.168.41.0/24}

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"

# Create a deterministic RESULT_DIR so we can reference it before the run prints it.
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$PROJECT_ROOT/results/eye_catcher/container_only/${TOPOLOGY_MODE}_tor${TOR_ID}_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

echo "[INFO] RESULT_DIR=$RESULT_DIR"

# Start the container-only run in background.
# SKIP_ANALYZE=1 because we will intentionally break sessions; analyze_bgp_logs.py's session-health check would fail.
RESULT_DIR_OVERRIDE="$RESULT_DIR" \
SKIP_ANALYZE=1 \
TOPOLOGY_MODE="$TOPOLOGY_MODE" \
TOR_ID="$TOR_ID" \
KEEP_AGG_ID="$KEEP_AGG_ID" \
PRE_DISABLE_OTHER_UPLINKS=1 \
"$PROJECT_ROOT/scripts/test_container_only.sh" "$TEST_DURATION" "$TOPOLOGY_MODE" &
RUN_PID=$!

echo "[INFO] test_container_only.sh pid=$RUN_PID"

# Wait until t0 exists
T0_FILE="$RESULT_DIR/meta/t0_epoch.txt"
while [ ! -f "$T0_FILE" ]; do
  sleep 0.2
  # If the run died early, fail fast
  if ! kill -0 "$RUN_PID" 2>/dev/null; then
    echo "[ERROR] test_container_only.sh exited before t0 was produced"
    wait "$RUN_PID" || true
    exit 1
  fi


# Inject fault
set +e
"$PROJECT_ROOT/scripts/inject_fault_tor_agg.sh" "$RESULT_DIR" "$TOR_ID" "$KEEP_AGG_ID"
INJECT_RC=$?
set -e

if [ "$INJECT_RC" -ne 0 ]; then
  echo "[ERROR] fault injection failed (rc=$INJECT_RC). Terminating container-only run..."
  kill "$RUN_PID" 2>/dev/null || true
  wait "$RUN_PID" || true
  exit "$INJECT_RC"
fi

# Wait for main run to finish (collect logs)
wait "$RUN_PID"

echo "[INFO] Logs collected. Computing convergence curve for prefix: $TARGET_PREFIX"
"$PROJECT_ROOT/scripts/compute_convergence_curve.py" "$RESULT_DIR" "$TARGET_PREFIX" "$TOPOLOGY_MODE" 64

echo "[DONE]"
echo "  Result dir: $RESULT_DIR"
echo "  - meta/per_router_convergence.csv"
echo "  - meta/convergence_curve.csv"
echo "  - meta/convergence_curve.png (if matplotlib is available)"
