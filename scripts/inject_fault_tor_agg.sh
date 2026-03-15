#!/bin/bash
# Inject a ToR-Agg single-link failure for container-only experiments.
# It assumes containers are named r<id> (e.g., r41), and birdc is available in the container.
#
# Usage:
#   ./scripts/inject_fault_tor_agg.sh <RESULT_DIR> [TOR_ID] [KEEP_AGG_ID] [WARMUP_S] [STABILIZE_S]
# Example:
#   ./scripts/inject_fault_tor_agg.sh results/container_only/no_pinning/fat-tree-k8-64_n64_20260315_132233 41 17 60 10
#
# Output:
#   Writes RESULT_DIR/meta/t_fail_epoch.txt
#
set -euo pipefail

RESULT_DIR=${1:-}
TOR_ID=${2:-41}
KEEP_AGG_ID=${3:-17}
WARMUP_S=${4:-60}
STABILIZE_S=${5:-10}

if [ -z "$RESULT_DIR" ]; then
  echo "[ERROR] RESULT_DIR is required"
  exit 1
fi

T0_FILE="$RESULT_DIR/meta/t0_epoch.txt"
TFAIL_FILE="$RESULT_DIR/meta/t_fail_epoch.txt"

if [ ! -f "$T0_FILE" ]; then
  echo "[ERROR] t0 file not found: $T0_FILE"
  echo "        Run test_container_only.sh first and point RESULT_DIR to its output directory."
  exit 1
fi

mkdir -p "$RESULT_DIR/meta"

# For fat-tree-k8-64, ToR uplinks are 4 Aggs in its pod.
# We compute them deterministically from IDs:
# ToR range: 41..64, pod = (tor-41)//4, aggs: 17 + pod*4 + {0..3}
if [ "$TOR_ID" -lt 41 ] || [ "$TOR_ID" -gt 64 ]; then
  echo "[ERROR] TOR_ID=$TOR_ID is out of range for fat-tree-k8-64 (41..64)"
  exit 1
fi
pod=$(( (TOR_ID - 41) / 4 ))
agg0=$((17 + pod*4 + 0))
agg1=$((17 + pod*4 + 1))
agg2=$((17 + pod*4 + 2))
agg3=$((17 + pod*4 + 3))
AGGS=("$agg0" "$agg1" "$agg2" "$agg3")

# Build list of OTHER_AGGS (all uplinks except KEEP_AGG_ID)
OTHER_AGGS=()
for a in "${AGGS[@]}"; do
  if [ "$a" -ne "$KEEP_AGG_ID" ]; then
    OTHER_AGGS+=("$a")
  fi
done

if [ "${#OTHER_AGGS[@]}" -ne 3 ]; then
  echo "[ERROR] KEEP_AGG_ID=$KEEP_AGG_ID must be one of uplinks: ${AGGS[*]}"
  exit 1
fi

T0_EPOCH=$(cat "$T0_FILE")

echo "[INFO] RESULT_DIR=$RESULT_DIR"
echo "[INFO] t0=$T0_EPOCH"
echo "[INFO] Will warm up ${WARMUP_S}s, then restrict uplinks to KEEP_AGG_ID=$KEEP_AGG_ID, then inject failure."
echo "[INFO] TOR_ID=$TOR_ID uplinks: ${AGGS[*]} (disable ${OTHER_AGGS[*]} first)"

# Wait until wallclock >= t0 + WARMUP_S
TARGET=$(python3 -c 'import sys; print(float(sys.argv[1]) + float(sys.argv[2]))' "$T0_EPOCH" "$WARMUP_S")

while true; do
  now=$(date +%s.%N)
  ok=$(python3 -c 'import sys; print(1 if float(sys.argv[1]) >= float(sys.argv[2]) else 0)' "$now" "$TARGET")
  if [ "$ok" -eq 1 ]; then
    break
  fi
  sleep 0.05
done

echo "[STEP] Disable other uplinks on both ends: TOR r$TOR_ID <-> Agg r{${OTHER_AGGS[*]}}"
for agg in "${OTHER_AGGS[@]}"; do
  sudo docker exec "r$TOR_ID" birdc disable "r$agg" >/dev/null
  sudo docker exec "r$agg" birdc disable "r$TOR_ID" >/dev/null
  echo "  disabled r$TOR_ID<->r$agg"
done

sleep "$STABILIZE_S"

# Record t_fail (epoch seconds)
TFAIL=$(date +%s.%N)
echo "$TFAIL" > "$TFAIL_FILE"
echo "[INFO] t_fail=$TFAIL (written to $TFAIL_FILE)"

echo "[STEP] Inject single-link failure on both ends: r$TOR_ID <-> r$KEEP_AGG_ID"
sudo docker exec "r$TOR_ID" birdc disable "r$KEEP_AGG_ID" >/dev/null
sudo docker exec "r$KEEP_AGG_ID" birdc disable "r$TOR_ID" >/dev/null
echo "  disabled r$TOR_ID<->r$KEEP_AGG_ID"

echo "[DONE] Fault injected. Wait for test_container_only.sh to finish collecting logs."
