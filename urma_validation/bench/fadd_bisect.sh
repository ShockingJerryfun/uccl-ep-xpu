#!/bin/bash
# Bisect: which preceding phase triggers the FADD-thr stall?
# B = lat + FADD-thr, C = WRITE-bw + FADD-thr
N=${N:-5}
run_urma() {
  local tag=$1; shift
  for i in $(seq 1 $N); do
    pkill -f urma_bench_v2 2>/dev/null
    sleep 0.5
    URMA_DEV_NAME=udmac0d1e2 setsid /tmp/urma_bench_v2 -s > /tmp/s_u_srv.log 2>&1 < /dev/null &
    sleep 2
    timeout 60 env URMA_DEV_NAME=udmac0d1e2 "$@" /tmp/urma_bench_v2 > /tmp/s_u_cli.log 2>&1
    rc=$?
    line=$(grep -E 'FADD-throughput|STALL|SPIN' /tmp/s_u_cli.log | tr '\n' '|' | tail -c 250)
    echo "URMA $tag run=$i rc=$rc :: $line"
  done
}
run_verbs() {
  local tag=$1; shift
  for i in $(seq 1 $N); do
    pkill -f verbs_bench_v2 2>/dev/null
    sleep 0.5
    IBV_DEV_NAME=rocep40s0f0 IBV_GID_IDX=2 setsid /tmp/verbs_bench_v2 -s > /tmp/s_v_srv.log 2>&1 < /dev/null &
    sleep 2
    timeout 60 env IBV_DEV_NAME=rocep40s0f0 IBV_GID_IDX=2 "$@" /tmp/verbs_bench_v2 > /tmp/s_v_cli.log 2>&1
    rc=$?
    line=$(grep -E 'FADD-throughput|STALL|SPIN' /tmp/s_v_cli.log | tr '\n' '|' | tail -c 250)
    echo "VERBS $tag run=$i rc=$rc :: $line"
  done
}
echo "===== B: lat + FADD-thr (URMA) ====="
run_urma "lat+fa" URMA_SKIP_LAT=0 URMA_NO_BW=1 URMA_NO_BARRIER=1 URMA_NO_QUIET=1 URMA_NO_VERIFY=1
echo "===== B: lat + FADD-thr (VERBS) ====="
run_verbs "lat+fa" IBV_SKIP_LAT=0 IBV_NO_BW=1 IBV_NO_BARRIER=1 IBV_NO_QUIET=1 IBV_NO_VERIFY=1
echo "===== C: bw + FADD-thr (URMA) ====="
run_urma "bw+fa" URMA_SKIP_LAT=1 URMA_NO_BW=0 URMA_NO_BARRIER=1 URMA_NO_QUIET=1 URMA_NO_VERIFY=1
echo "===== C: bw + FADD-thr (VERBS) ====="
run_verbs "bw+fa" IBV_SKIP_LAT=1 IBV_NO_BW=0 IBV_NO_BARRIER=1 IBV_NO_QUIET=1 IBV_NO_VERIFY=1
echo "BISECT_DONE"
