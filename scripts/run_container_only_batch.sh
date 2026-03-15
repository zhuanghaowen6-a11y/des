#!/bin/bash
set -Eeuo pipefail
trap 'echo "[ERROR] $(basename "$0") failed at line $LINENO: $BASH_COMMAND" >&2' ERR
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"
TEST_DURATION="${1:-180}"
TOPOLOGY_MODE="${2:-fat-tree-k8-64}"
ITERATIONS="${ITERATIONS:-10}"
CPU_PINNING="${CPU_PINNING:-1}"
PINNING_ARG="${3:-}"
if [ -n "$PINNING_ARG" ]; then
  case "$PINNING_ARG" in
    0|no|nopin|false) CPU_PINNING=0 ;;
    *) CPU_PINNING=1 ;;
  esac
fi
TOPOLOGY_MODE="$(echo "$TOPOLOGY_MODE" | tr '[:upper:]' '[:lower:]')"
case "$TOPOLOGY_MODE" in
  fat-tree-k6) NUM_ROUTERS=45 ;;
  fat-tree-k8-64) NUM_ROUTERS=64 ;;
  fat-tree-k4) NUM_ROUTERS=20 ;;
  ring) NUM_ROUTERS="${NUM_ROUTERS:-10}" ;;
  *) NUM_ROUTERS="${NUM_ROUTERS:-5}" ;;
esac
PINNING_LABEL=$([ "$CPU_PINNING" -eq 1 ] && echo "with_pinning" || echo "no_pinning")
BATCH_DIR="results/container_only/batch"
mkdir -p "$BATCH_DIR"
START_TS="$(date +%Y%m%d_%H%M%S)"
BATCH_FILE="$BATCH_DIR/${TOPOLOGY_MODE}_n${NUM_ROUTERS}_${PINNING_LABEL}_${TEST_DURATION}s_${START_TS}.csv"
if [ ! -f "$BATCH_FILE" ]; then
  echo "iteration,start_time,result_dir,T_session,T_route_rib,T_update_quiescence" > "$BATCH_FILE"
fi
SINGLE_RUN="$SCRIPT_DIR/test_container_only.sh"
if [ ! -f "$SINGLE_RUN" ]; then
  echo "[ERROR] missing single-run script: $SINGLE_RUN" >&2
  exit 1
fi
for i in $(seq 1 "$ITERATIONS"); do
  RUN_START="$(date +%Y-%m-%d_%H:%M:%S)"
  RUN_LOG="$BATCH_DIR/run_${START_TS}_iter${i}.log"
  if ! CPU_PINNING="$CPU_PINNING" bash "$SINGLE_RUN" "$TEST_DURATION" "$TOPOLOGY_MODE" > "$RUN_LOG" 2>&1; then
    echo "[WARN] single-run failed for iter ${i}, see $RUN_LOG" >&2
  fi
  RESULT_DIR="$(awk -F': ' '/^(结果目录|Result Dir): /{print $2}' "$RUN_LOG" | tail -n 1)"
  if [ -z "$RESULT_DIR" ]; then
    R_TOPO="$(awk -F': ' '/^Topology: /{print $2}' "$RUN_LOG" | tail -n 1 | tr '[:upper:]' '[:lower:]')"
    R_NUM="$(awk -F': ' '/^Routers: /{print $2}' "$RUN_LOG" | tail -n 1)"
    [ -z "$R_TOPO" ] && R_TOPO="$TOPOLOGY_MODE"
    [ -z "$R_NUM" ] && R_NUM="$NUM_ROUTERS"
    RESULT_DIR="$(ls -dt results/container_only/${PINNING_LABEL}/${R_TOPO}_n${R_NUM}_* 2>/dev/null | head -n 1)"
  fi
  if [ -z "$RESULT_DIR" ] || [ ! -d "$RESULT_DIR" ]; then
    echo "${i},${RUN_START},,," >> "$BATCH_FILE"
    continue
  fi
  mkdir -p "$RESULT_DIR/meta"
  ANALYSIS_OUT="$RESULT_DIR/meta/analysis.txt"
  if [ -f "$SCRIPT_DIR/analyze_bgp_logs.py" ]; then
    TIME_MODE=wallclock T0_FILE="$RESULT_DIR/meta/t0_epoch.txt" TOPOLOGY_MODE="$TOPOLOGY_MODE" python3 "$SCRIPT_DIR/analyze_bgp_logs.py" "$NUM_ROUTERS" "$RESULT_DIR/logs" > "$ANALYSIS_OUT" 2>&1 || true
  else
    echo "[WARN] analyze_bgp_logs.py not found; skipping analysis" > "$ANALYSIS_OUT"
  fi
  ANALYSIS_PLAIN="$RESULT_DIR/meta/analysis_plain.txt"
  if [ -s "$ANALYSIS_OUT" ]; then
    sed -r 's/\x1B\[[0-9;]*[A-Za-z]//g' "$ANALYSIS_OUT" > "$ANALYSIS_PLAIN" || cp "$ANALYSIS_OUT" "$ANALYSIS_PLAIN" || true
    TS_VALUE="$(grep -E '^  T_session ' "$ANALYSIS_PLAIN" | grep -oE '[0-9]+\.[0-9]+' || true)"
    TR_VALUE="$(grep -E '^  T_route_rib ' "$ANALYSIS_PLAIN" | grep -oE '[0-9]+\.[0-9]+' || true)"
    TQ_VALUE="$(grep -E '^  T_update_quiescence ' "$ANALYSIS_PLAIN" | grep -oE '[0-9]+\.[0-9]+' || true)"
  else
    TS_VALUE=""
    TR_VALUE=""
    TQ_VALUE=""
  fi
  echo "${i},${RUN_START},${RESULT_DIR},${TS_VALUE},${TR_VALUE},${TQ_VALUE}" >> "$BATCH_FILE"
done
echo "Batch results saved: $BATCH_FILE"
