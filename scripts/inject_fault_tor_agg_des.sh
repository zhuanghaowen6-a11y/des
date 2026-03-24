#!/bin/bash
# Inject a ToR-Agg single-link failure during DES mode experiment (test_n_bird_docker.sh).
#
# Scheme C: The 3 non-KEEP uplinks are already excluded at config generation time
# (PRE_DISABLE_OTHER_UPLINKS=1), so this script only waits for convergence and then
# disables the single KEEP link on both ends.
#
# Usage:
#   ./scripts/inject_fault_tor_agg_des.sh <RESULT_DIR> [TOR_ID] [KEEP_AGG_ID]
# Example:
#   ./scripts/inject_fault_tor_agg_des.sh results/eye_catcher/des/fat-tree-k8-64_tor41_... 41 17
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

MARKER_FILE="$RESULT_DIR/meta/bird_started_epoch.txt"

# Validate TOR_ID for fat-tree-k8-64
if [ "$TOR_ID" -lt 41 ] || [ "$TOR_ID" -gt 64 ]; then
  echo "[ERROR] TOR_ID=$TOR_ID is out of range for fat-tree-k8-64 (41..64)"
  exit 1
fi

# Validate KEEP_AGG_ID is in same pod as TOR_ID
pod=$(( (TOR_ID - 41) / 4 ))
agg0=$((17 + pod*4 + 0))
agg1=$((17 + pod*4 + 1))
agg2=$((17 + pod*4 + 2))
agg3=$((17 + pod*4 + 3))
AGGS=("$agg0" "$agg1" "$agg2" "$agg3")

is_valid_keep=0
for a in "${AGGS[@]}"; do
  if [ "$a" -eq "$KEEP_AGG_ID" ]; then
    is_valid_keep=1
    break
  fi
done
if [ "$is_valid_keep" -ne 1 ]; then
  echo "[ERROR] KEEP_AGG_ID=$KEEP_AGG_ID must be one of ToR $TOR_ID uplinks: ${AGGS[*]}"
  exit 1
fi

echo "[INFO] RESULT_DIR=$RESULT_DIR"
echo "[INFO] TOR_ID=$TOR_ID KEEP_AGG_ID=$KEEP_AGG_ID (Scheme C: only this link exists)"
echo "[INFO] inject_timeout=${INJECT_TIMEOUT_S}s"

# Wait until bird start marker exists (written by test_n_bird_docker.sh)
while [ ! -f "$MARKER_FILE" ]; do
  sleep 0.2
done

echo "[INFO] bird start marker present: $MARKER_FILE"

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

  # Scheme C: only check the two endpoints that have the single session
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

# Record t_fail_epoch before injection
TFAIL_EPOCH=$(date +%s.%N)
echo "$TFAIL_EPOCH" > "$RESULT_DIR/meta/t_fail_epoch.txt"
echo "[INFO] t_fail_epoch=$TFAIL_EPOCH (written to meta/t_fail_epoch.txt)"

echo "[STEP] Inject single-link failure on both ends: r$TOR_ID <-> r$KEEP_AGG_ID"
do_exec "r$TOR_ID" "r$KEEP_AGG_ID"
do_exec "r$KEEP_AGG_ID" "r$TOR_ID"
echo "  disabled r$TOR_ID<->r$KEEP_AGG_ID"

echo "[DONE] Fault injected. Wait for test_n_bird_docker.sh to collect logs."
