#!/bin/bash
# Inject a ToR-Agg single-link failure during DES mode experiment (test_n_bird_docker.sh).
#
# This script uses wall-clock warmup (because external VT query is not exposed to the host).
# The convergence curve script will later derive t_fail in VT from BIRD logs.
#
# Usage:
#   ./scripts/inject_fault_tor_agg_des.sh <RESULT_DIR> [TOR_ID] [KEEP_AGG_ID] [WARMUP_S] [STABILIZE_S]
# Example:
#   ./scripts/inject_fault_tor_agg_des.sh results/eye_catcher/des/fat-tree-k8-64_tor41_... 41 17 60 10
#
set -euo pipefail

RESULT_DIR=${1:-}
TOR_ID=${2:-41}
KEEP_AGG_ID=${3:-17}
WARMUP_S=${4:-60}
STABILIZE_S=${5:-10}
INJECT_TIMEOUT_S=${INJECT_TIMEOUT_S:-5}
WAIT_FOR_CONVERGENCE=${WAIT_FOR_CONVERGENCE:-0}
CONVERGENCE_TIMEOUT_S=${CONVERGENCE_TIMEOUT_S:-300}
CONVERGENCE_POLL_INTERVAL_S=${CONVERGENCE_POLL_INTERVAL_S:-2}
CONVERGENCE_STABLE_ROUNDS=${CONVERGENCE_STABLE_ROUNDS:-3}
CONVERGENCE_CHECK_ALL=${CONVERGENCE_CHECK_ALL:-0}
CONVERGENCE_CHECK_TIMEOUT_S=${CONVERGENCE_CHECK_TIMEOUT_S:-5}

if [ -z "$RESULT_DIR" ]; then
  echo "[ERROR] RESULT_DIR is required"
  exit 1
fi

MARKER_FILE="$RESULT_DIR/meta/bird_started_epoch.txt"

# For fat-tree-k8-64, ToR uplinks are 4 Aggs in its pod.
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
echo "[INFO] TOR_ID=$TOR_ID uplinks: ${AGGS[*]} (disable ${OTHER_AGGS[*]} first)"
echo "[INFO] warmup=${WARMUP_S}s stabilize=${STABILIZE_S}s timeout=${INJECT_TIMEOUT_S}s"

# Wait until bird start marker exists (written by test_n_bird_docker.sh)
while [ ! -f "$MARKER_FILE" ]; do
  sleep 0.2
  # Allow user to ctrl-c; no extra checks here.
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

ENV_FILE="$RESULT_DIR/meta/env.txt"
NUM_ROUTERS=${NUM_ROUTERS:-64}
if [ -f "$ENV_FILE" ]; then
  set +u
  # shellcheck disable=SC1090
  source "$ENV_FILE"
  set -u
fi

sleep_with_liveness_check() {
  local total_s=$1
  local step_s=1
  local elapsed=0
  while [ "$elapsed" -lt "$total_s" ]; do
    if ! sudo docker inspect "r$TOR_ID" >/dev/null 2>&1; then
      echo "[ERROR] container r$TOR_ID not found (environment may have been cleaned up)"
      exit 1
    fi
    if ! sudo docker inspect "r$KEEP_AGG_ID" >/dev/null 2>&1; then
      echo "[ERROR] container r$KEEP_AGG_ID not found (environment may have been cleaned up)"
      exit 1
    fi
    sleep "$step_s"
    elapsed=$((elapsed + step_s))
  done
}

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

  local check_routers=()
  if [ "$CONVERGENCE_CHECK_ALL" -eq 1 ]; then
    for rid in $(seq 1 "$NUM_ROUTERS"); do
      check_routers+=("$rid")
    done
  else
    check_routers+=("$TOR_ID")
    check_routers+=("$KEEP_AGG_ID")
    for a in "${AGGS[@]}"; do
      check_routers+=("$a")
    done
    check_routers+=("1")
  fi

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

if [ "$WAIT_FOR_CONVERGENCE" -eq 1 ] || [ "$WARMUP_S" -eq 0 ]; then
  echo "[STEP] waiting for convergence via birdc (timeout=${CONVERGENCE_TIMEOUT_S}s, stable_rounds=${CONVERGENCE_STABLE_ROUNDS})"
  wait_for_convergence
else
  echo "[STEP] warmup sleep ${WARMUP_S}s"
  sleep_with_liveness_check "$WARMUP_S"
fi

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

echo "[STEP] Disable other uplinks on both ends: r$TOR_ID <-> r{${OTHER_AGGS[*]}}"
for agg in "${OTHER_AGGS[@]}"; do
  if ! sudo docker inspect "r$agg" >/dev/null 2>&1; then
    echo "[ERROR] container r$agg not found (environment may have been cleaned up)"
    exit 1
  fi
  do_exec "r$TOR_ID" "r$agg"
  do_exec "r$agg" "r$TOR_ID"
  echo "  disabled r$TOR_ID<->r$agg"
done

sleep_with_liveness_check "$STABILIZE_S"

# Record wall-clock injection time for debugging
TFAIL_EPOCH=$(date +%s.%N)
echo "$TFAIL_EPOCH" > "$RESULT_DIR/meta/t_fail_epoch.txt"
echo "[INFO] t_fail_epoch=$TFAIL_EPOCH (written to meta/t_fail_epoch.txt)"

echo "[STEP] Inject single-link failure on both ends: r$TOR_ID <-> r$KEEP_AGG_ID"
do_exec "r$TOR_ID" "r$KEEP_AGG_ID"
do_exec "r$KEEP_AGG_ID" "r$TOR_ID"
echo "  disabled r$TOR_ID<->r$KEEP_AGG_ID"

echo "[DONE] Fault injected. Wait for test_n_bird_docker.sh to collect logs."
