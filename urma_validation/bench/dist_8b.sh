#!/bin/bash
# Distribution test: 8B w=32 WRITE-bw, 10 runs per platform, isolated flags.
N=${N:-10}
for i in $(seq 1 $N); do
  pkill -f urma_bench_v2 2>/dev/null; pkill -f verbs_bench_v2 2>/dev/null
  sleep 0.5
  URMA_DEV_NAME=udmac0d1e2 setsid /tmp/urma_bench_v2 -s > /tmp/e_srv.log 2>&1 < /dev/null &
  sleep 1
  timeout 30 env URMA_DEV_NAME=udmac0d1e2 URMA_NO_VERIFY=1 URMA_SKIP_LAT=1 \
    URMA_NO_FA=1 URMA_NO_BARRIER=1 URMA_NO_QUIET=1 \
    URMA_BW_SIZE=8 URMA_BW_WINDOW=32 \
    /tmp/urma_bench_v2 2>/dev/null | grep "WRITE-bw" | sed "s/^/URMA  r$i /"
done
for i in $(seq 1 $N); do
  pkill -f urma_bench_v2 2>/dev/null; pkill -f verbs_bench_v2 2>/dev/null
  sleep 0.5
  IBV_DEV_NAME=rocep40s0f0 IBV_GID_IDX=2 setsid /tmp/verbs_bench_v2 -s > /tmp/e_srv.log 2>&1 < /dev/null &
  sleep 1
  timeout 30 env IBV_DEV_NAME=rocep40s0f0 IBV_GID_IDX=2 IBV_NO_VERIFY=1 \
    IBV_SKIP_LAT=1 IBV_NO_FA=1 IBV_NO_BARRIER=1 IBV_NO_QUIET=1 \
    IBV_BW_SIZE=8 IBV_BW_WINDOW=32 \
    /tmp/verbs_bench_v2 2>/dev/null | grep "WRITE-bw" | sed "s/^/VERBS r$i /"
done
echo DIST_DONE
