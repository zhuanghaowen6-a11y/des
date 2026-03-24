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
INJECT_TIMEOUT_S=${INJECT_TIMEOUT_S:-5}
CONVERGENCE_TIMEOUT_S=${CONVERGENCE_TIMEOUT_S:-300}
CONVERGENCE_POLL_INTERVAL_S=${CONVERGENCE_POLL_INTERVAL_S:-2}
CONVERGENCE_STABLE_ROUNDS=${CONVERGENCE_STABLE_ROUNDS:-10}
CONVERGENCE_CHECK_TIMEOUT_S=${CONVERGENCE_CHECK_TIMEOUT_S:-5}

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

echo "[INFO] RESULT_DIR=$RESULT_DIR"
echo "[INFO] TOR_ID=$TOR_ID KEEP_AGG_ID=$KEEP_AGG_ID (Scheme C: only this link exists)"
echo "[INFO] inject_timeout=${INJECT_TIMEOUT_S}s"

if ! sudo docker inspect "r$TOR_ID" >/dev/null 2>&1; then
  echo "[ERROR] container r$TOR_ID not found (environment may have been cleaned up)"
  exit 1
fi
if ! sudo docker inspect "r$KEEP_AGG_ID" >/dev/null 2>&1; then
  echo "[ERROR] container r$KEEP_AGG_ID not found (environment may have been cleaned up)"
  exit 1
fi

birdc_show_protocols() {
  local rid=$1
  timeout "${CONVERGENCE_CHECK_TIMEOUT_S}s" sudo docker exec "r${rid}" birdc show protocols 2>/dev/null
}

router_bgp_established() {
  local rid=$1
  local out
  out=$(birdc_show_protocols "$rid" || true)
  if [ -z "$out" ]; then
    return 1
  fi
  local bgp_lines
  bgp_lines=$(echo "$out" | awk 'NF >= 2 && $2 == "BGP" {print $0}')
  if [ -z "$bgp_lines" ]; then
    return 1
  fi
  if echo "$bgp_lines" | grep -vq "Established"; then
    return 1
  fi
  return 0
}

wait_for_convergence() {
  local start_ts
  start_ts=$(date +%s)
  local ok_rounds=0
  local check_routers=("$TOR_ID" "$KEEP_AGG_ID")

  while true; do
    local now_ts
    now_ts=$(date +%s)
    if [ $((now_ts - start_ts)) -ge "$CONVERGENCE_TIMEOUT_S" ]; then
      echo "[ERROR] convergence wait timed out after ${CONVERGENCE_TIMEOUT_S}s"
      return 1
    fi

    local all_ok=1
    for rid in "${check_routers[@]}"; do
      if ! sudo docker inspect "r$rid" >/dev/null 2>&1; then
        echo "[ERROR] container r$rid not found (environment may have been cleaned up)"
        return 1
      fi
      if ! router_bgp_established "$rid"; then
        all_ok=0
        break
      fi
    done

    if [ "$all_ok" -eq 1 ]; then
      ok_rounds=$((ok_rounds + 1))
      echo "[INFO] convergence check ok (${ok_rounds}/${CONVERGENCE_STABLE_ROUNDS})"
      if [ "$ok_rounds" -ge "$CONVERGENCE_STABLE_ROUNDS" ]; then
        return 0
      fi
    else
      ok_rounds=0
    fi

    sleep "${CONVERGENCE_POLL_INTERVAL_S}"
  done
}

echo "[STEP] waiting for convergence via birdc (timeout=${CONVERGENCE_TIMEOUT_S}s, stable_rounds=${CONVERGENCE_STABLE_ROUNDS})"
wait_for_convergence

do_exec() {
  local cname=$1
  local peer=$2
  if ! command -v timeout >/dev/null 2>&1; then
    echo "[ERROR] host-side 'timeout' command not found; refusing to run potentially blocking birdc" 1>&2
    exit 127
  fi
  local ts
  ts=$(date +%s.%N)
  echo "[INJECT] ts=${ts} exec: ${cname} birdc disable ${peer}"
  timeout "${INJECT_TIMEOUT_S}s" sudo docker exec "$cname" birdc disable "$peer" >/dev/null
}

TFAIL_EPOCH=$(date +%s.%N)
echo "$TFAIL_EPOCH" > "$TFAIL_FILE"
echo "[INFO] t_fail_epoch=$TFAIL_EPOCH (written to $TFAIL_FILE)"

echo "[STEP] Inject single-link failure on both ends: r$TOR_ID <-> r$KEEP_AGG_ID"
do_exec "r$TOR_ID" "r$KEEP_AGG_ID"
do_exec "r$KEEP_AGG_ID" "r$TOR_ID"
echo "  disabled r$TOR_ID<->r$KEEP_AGG_ID"

echo "[DONE] Fault injected. Wait for test_container_only.sh to finish collecting logs."
