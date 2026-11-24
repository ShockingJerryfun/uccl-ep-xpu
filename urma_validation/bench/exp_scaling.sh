#!/bin/bash
# Experiment: WRITE throughput scaling analysis
# Exp1: message size sweep (8B..32KB) at fixed window=32
# Exp2: window depth sweep (32..256) at fixed size=8B
run_urma() {
  local size=$1 win=$2
  pkill -f urma_bench_v2 2>/dev/null; pkill -f verbs_bench_v2 2>/dev/null
  sleep 1
  URMA_DEV_NAME=udmac0d1e2 setsid /tmp/urma_bench_v2 -s > /tmp/e_srv.log 2>&1 < /dev/null &
  sleep 1
  timeout 60 env URMA_DEV_NAME=udmac0d1e2 URMA_NO_VERIFY=1 \
    URMA_SKIP_LAT=1 URMA_NO_FA=1 URMA_NO_BARRIER=1 URMA_NO_QUIET=1 \
    URMA_BW_SIZE=$size URMA_BW_WINDOW=$win \
    /tmp/urma_bench_v2 2>/dev/null | grep "WRITE-bw" | sed "s/^/URMA  /"
}
run_verbs() {
  local size=$1 win=$2
  pkill -f urma_bench_v2 2>/dev/null; pkill -f verbs_bench_v2 2>/dev/null
  sleep 1
  IBV_DEV_NAME=rocep40s0f0 IBV_GID_IDX=2 setsid /tmp/verbs_bench_v2 -s > /tmp/e_srv.log 2>&1 < /dev/null &
  sleep 1
  timeout 60 env IBV_DEV_NAME=rocep40s0f0 IBV_GID_IDX=2 IBV_NO_VERIFY=1 \
    IBV_SKIP_LAT=1 IBV_NO_FA=1 IBV_NO_BARRIER=1 IBV_NO_QUIET=1 \
    IBV_BW_SIZE=$size IBV_BW_WINDOW=$win \
    /tmp/verbs_bench_v2 2>/dev/null | grep "WRITE-bw" | sed "s/^/VERBS /"
}
echo "===== EXP1: size sweep (w=32) ====="
for sz in 8 512 4096 16384 32768; do
  echo "--- size $sz ---"
  run_urma $sz 32
  run_verbs $sz 32
done
echo "===== EXP2: window sweep (8B) ====="
for w in 32 128 256; do
  echo "--- window $w ---"
  run_urma 8 $w
  run_verbs 8 $w
done
echo EXP_DONE
