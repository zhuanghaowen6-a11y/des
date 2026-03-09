#!/bin/bash
set -e
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"
TEST_DURATION="${1:-180}"
TOPOLOGY_MODE="${2:-fat-tree-k8-64}"
ITERATIONS="${ITERATIONS:-10}"
TOPOLOGY_MODE="$(echo "$TOPOLOGY_MODE" | tr '[:upper:]' '[:lower:]')"
case "$TOPOLOGY_MODE" in
  fat-tree-k6) NUM_ROUTERS=45 ;;
  fat-tree-k8-64) NUM_ROUTERS=64 ;;
  ring) NUM_ROUTERS="${NUM_ROUTERS:-10}" ;;
  *) NUM_ROUTERS="${NUM_ROUTERS:-5}" ;;
esac
BATCH_DIR="results/container_only/batch"
mkdir -p "$BATCH_DIR"
START_TS="$(date +%Y%m%d_%H%M%S)"
BATCH_FILE="$BATCH_DIR/${TOPOLOGY_MODE}_n${NUM_ROUTERS}_pinned_${TEST_DURATION}s_${START_TS}.csv"
if [ ! -f "$BATCH_FILE" ]; then
  echo "iteration,start_time,result_dir,T_session,T_route_rib,T_update_quiescence" > "$BATCH_FILE"
fi
for i in $(seq 1 "$ITERATIONS"); do
  RUN_START="$(date +%Y-%m-%d_%H:%M:%S)"
  RUN_LOG="$BATCH_DIR/run_${START_TS}_iter${i}.log"
  CPU_PINNING=1 "$SCRIPT_DIR/test_container_only.sh" "$TEST_DURATION" "$TOPOLOGY_MODE" > "$RUN_LOG" 2>&1 || true
  RESULT_DIR="$(grep -E '^结果目录: ' "$RUN_LOG" | tail -n 1 | awk -F': ' '{print $2}')"
  if [ -z "$RESULT_DIR" ]; then
    RESULT_DIR="$(ls -dt results/container_only/with_pinning/${TOPOLOGY_MODE}_n${NUM_ROUTERS}_* 2>/dev/null | head -n 1)"
  fi
  ANALYSIS_OUT="$RESULT_DIR/meta/analysis.txt"
  TIME_MODE=wallclock T0_FILE="$RESULT_DIR/meta/t0_epoch.txt" TOPOLOGY_MODE="$TOPOLOGY_MODE" python3 "$SCRIPT_DIR/analyze_bgp_logs.py" "$NUM_ROUTERS" "$RESULT_DIR/logs" > "$ANALYSIS_OUT" 2>&1 || true
  ANALYSIS_PLAIN="$RESULT_DIR/meta/analysis_plain.txt"
  sed -r 's/\x1B\[[0-9;]*[A-Za-z]//g' "$ANALYSIS_OUT" > "$ANALYSIS_PLAIN" || cp "$ANALYSIS_OUT" "$ANALYSIS_PLAIN"
  TS_VALUE="$(grep -E '^  T_session ' "$ANALYSIS_PLAIN" | grep -oE '[0-9]+\.[0-9]+')"
  TR_VALUE="$(grep -E '^  T_route_rib ' "$ANALYSIS_PLAIN" | grep -oE '[0-9]+\.[0-9]+')"
  TQ_VALUE="$(grep -E '^  T_update_quiescence ' "$ANALYSIS_PLAIN" | grep -oE '[0-9]+\.[0-9]+')"
  echo "${i},${RUN_START},${RESULT_DIR},${TS_VALUE},${TR_VALUE},${TQ_VALUE}" >> "$BATCH_FILE"
done
echo "Batch results saved: $BATCH_FILE"
