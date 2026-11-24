#!/bin/bash
# WRITE latency vs size sweep: 3 runs per platform
for i in 1 2 3; do
  pkill -f urma_bench_v2 2>/dev/null; pkill -f verbs_bench_v2 2>/dev/null
  sleep 0.5
  URMA_DEV_NAME=udmac0d1e2 setsid /tmp/urma_bench_v2 -s > /tmp/l_srv.log 2>&1 < /dev/null &
  sleep 1
  timeout 40 env URMA_DEV_NAME=udmac0d1e2 URMA_SKIP_LAT=0 URMA_NO_BW=1 \
    URMA_NO_FA=1 URMA_NO_BARRIER=1 URMA_NO_QUIET=1 URMA_NO_VERIFY=1 \
    /tmp/urma_bench_v2 2>/dev/null | grep "WRITE-latency" | sed "s/^/URMA  r$i /"
done
for i in 1 2 3; do
  pkill -f urma_bench_v2 2>/dev/null; pkill -f verbs_bench_v2 2>/dev/null
  sleep 0.5
  IBV_DEV_NAME=rocep40s0f0 IBV_GID_IDX=2 setsid /tmp/verbs_bench_v2 -s > /tmp/l_srv.log 2>&1 < /dev/null &
  sleep 1
  timeout 40 env IBV_DEV_NAME=rocep40s0f0 IBV_GID_IDX=2 IBV_SKIP_LAT=0 IBV_NO_BW=1 \
    IBV_NO_FA=1 IBV_NO_BARRIER=1 IBV_NO_QUIET=1 IBV_NO_VERIFY=1 \
    /tmp/verbs_bench_v2 2>/dev/null | grep "WRITE-latency" | sed "s/^/VERBS r$i /"
done
echo LAT_DONE
