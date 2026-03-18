#!/bin/bash
# Orchestrate an "eye-catcher" experiment in DES mode (test_n_bird_docker.sh) and generate VT-based convergence curve.
#
# Usage:
#   ./scripts/run_eye_catcher_des.sh <TEST_DURATION> <TOPOLOGY_MODE> <TOR_ID> <KEEP_AGG_ID> <TARGET_PREFIX> [STABILIZE_S]
# Example:
#   GLOBAL_CPUSET=0-1 DESD_CPUSET=0-1 ./scripts/run_eye_catcher_des.sh 120 fat-tree-k8-64 41 17 192.168.41.0/24 5
#
set -euo pipefail

TEST_DURATION=${1:-120}
TOPOLOGY_MODE=${2:-fat-tree-k8-64}
TOR_ID=${3:-41}
KEEP_AGG_ID=${4:-17}
TARGET_PREFIX=${5:-192.168.41.0/24}
STABILIZE_S=${6:-5}

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}" )" && pwd)"
PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$PROJECT_ROOT/results/eye_catcher/des/${TOPOLOGY_MODE}_tor${TOR_ID}_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

echo "[INFO] RESULT_DIR=$RESULT_DIR"

# Run DES test in background.
# SKIP_ANALYZE=1 because fault injection will make session-health checks fail by design.
RESULT_DIR_OVERRIDE="$RESULT_DIR" \
SKIP_ANALYZE=1 \
TOPOLOGY_MODE="$TOPOLOGY_MODE" \
TOR_ID="$TOR_ID" \
KEEP_AGG_ID="$KEEP_AGG_ID" \
"$PROJECT_ROOT/scripts/test_n_bird_docker.sh" 64 "$TEST_DURATION" "$TOPOLOGY_MODE" &
RUN_PID=$!

echo "[INFO] test_n_bird_docker.sh pid=$RUN_PID"

# Wait until BIRD start marker exists (produced by test_n_bird_docker.sh after launching birds)
MARKER_FILE="$RESULT_DIR/meta/bird_started_epoch.txt"
WAIT_S=${WAIT_S:-600}
DEADLINE=$(( $(date +%s) + WAIT_S ))
while [ ! -f "$MARKER_FILE" ]; do
  sleep 0.2
  if ! kill -0 "$RUN_PID" 2>/dev/null; then
    echo "[ERROR] test_n_bird_docker.sh exited before bird start marker was produced"
    wait "$RUN_PID" || true
    exit 1
  fi
  if [ "$(date +%s)" -ge "$DEADLINE" ]; then
    echo "[ERROR] timeout waiting for $MARKER_FILE"
    echo "        Check $RESULT_DIR/logs/desd_n*.log and container logs."
    exit 1
  fi
done

# Inject fault
set +e
"$PROJECT_ROOT/scripts/inject_fault_tor_agg_des.sh" "$RESULT_DIR" "$TOR_ID" "$KEEP_AGG_ID" "$STABILIZE_S"
INJECT_RC=$?
set -e

if [ "$INJECT_RC" -ne 0 ]; then
  echo "[ERROR] fault injection failed (rc=$INJECT_RC). Terminating DES run..."
  kill "$RUN_PID" 2>/dev/null || true
  wait "$RUN_PID" || true
  exit "$INJECT_RC"
fi

# Wait for DES run to finish (it will collect logs into RESULT_DIR/logs)
wait "$RUN_PID" || true

echo "[INFO] Logs collected. Computing convergence curve (VT) for prefix: $TARGET_PREFIX"
"$PROJECT_ROOT/scripts/compute_convergence_curve_vt.py" "$RESULT_DIR" "$TARGET_PREFIX" "$TOPOLOGY_MODE" 64 "$TOR_ID" "$KEEP_AGG_ID"

echo "[DONE]"
echo "  Result dir: $RESULT_DIR"
echo "  - logs/"
echo "  - meta/t_fail_vt.txt"
echo "  - meta/convergence_curve.csv"
echo "  - meta/convergence_curve.png (if matplotlib is available)"
