#!/bin/bash
echo "########## URMA runs ##########"
for i in 1 2 3; do
  pkill -f urma_bench_v2 2>/dev/null
  sleep 1
  URMA_DEV_NAME=udmac0d1e2 setsid /tmp/urma_bench_v2 -s > /tmp/u_srv.log 2>&1 < /dev/null &
  sleep 2
  timeout 120 env URMA_DEV_NAME=udmac0d1e2 /tmp/urma_bench_v2 > /tmp/u_cli.log 2>&1
  echo "=== urma run $i rc=$? ==="
  cat /tmp/u_cli.log
  echo "--- server ---"
  grep -E "RESULT|WRITE-verify" /tmp/u_srv.log
done
echo
echo "########## VERBS runs ##########"
for i in 1 2 3; do
  pkill -f verbs_bench_v2 2>/dev/null
  sleep 1
  IBV_DEV_NAME=rocep40s0f0 IBV_GID_IDX=2 setsid /tmp/verbs_bench_v2 -s > /tmp/v_srv.log 2>&1 < /dev/null &
  sleep 2
  timeout 120 env IBV_DEV_NAME=rocep40s0f0 IBV_GID_IDX=2 /tmp/verbs_bench_v2 > /tmp/v_cli.log 2>&1
  echo "=== verbs run $i rc=$? ==="
  cat /tmp/v_cli.log
  echo "--- server ---"
  grep -E "RESULT|WRITE-verify" /tmp/v_srv.log
done
echo ALL_DONE
