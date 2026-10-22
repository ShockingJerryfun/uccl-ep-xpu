#!/bin/bash
# Isolate the FADD-throughput stall: run ONLY the FADD-thr phase, repeatedly.
# Sweep window size to see if stall depends on outstanding-op depth.
N=${N:-10}
SKIPU="URMA_SKIP_LAT=1 URMA_NO_BW=1 URMA_NO_BARRIER=1 URMA_NO_QUIET=1 URMA_NO_VERIFY=1"
SKIPV="IBV_SKIP_LAT=1 IBV_NO_BW=1 IBV_NO_BARRIER=1 IBV_NO_QUIET=1 IBV_NO_VERIFY=1"
run_urma() {
  local w=$1
  for i in $(seq 1 $N); do
    pkill -f urma_bench_v2 2>/dev/null
    sleep 0.5
    URMA_DEV_NAME=udmac0d1e2 setsid /tmp/urma_bench_v2 -s > /tmp/s_u_srv.log 2>&1 < /dev/null &
    sleep 2
    timeout 60 env URMA_DEV_NAME=udmac0d1e2 $SKIPU URMA_FA_WINDOW=$w \
      /tmp/urma_bench_v2 > /tmp/s_u_cli.log 2>&1
    rc=$?
    line=$(grep -E 'FADD-throughput|STALL|missing' /tmp/s_u_cli.log | tr '\n' '|' | tail -c 200)
    echo "URMA w=$w run=$i rc=$rc :: $line"
  done
}
run_verbs() {
  local w=$1
  for i in $(seq 1 $N); do
    pkill -f verbs_bench_v2 2>/dev/null
    sleep 0.5
    IBV_DEV_NAME=rocep40s0f0 IBV_GID_IDX=2 setsid /tmp/verbs_bench_v2 -s > /tmp/s_v_srv.log 2>&1 < /dev/null &
    sleep 2
    timeout 60 env IBV_DEV_NAME=rocep40s0f0 IBV_GID_IDX=2 $SKIPV IBV_FA_WINDOW=$w \
      /tmp/verbs_bench_v2 > /tmp/s_v_cli.log 2>&1
    rc=$?
    line=$(grep -E 'FADD-throughput|STALL|missing' /tmp/s_v_cli.log | tr '\n' '|' | tail -c 200)
    echo "VERBS w=$w run=$i rc=$rc :: $line"
  done
}
echo "===== URMA FADD-only, window=128, N=$N ====="
run_urma 128
echo "===== VERBS FADD-only, window=128, N=$N ====="
run_verbs 128
echo "===== URMA FADD-only, window=16, N=$N ====="
run_urma 16
echo "===== VERBS FADD-only, window=16, N=$N ====="
run_verbs 16
echo "STRESS_DONE"
