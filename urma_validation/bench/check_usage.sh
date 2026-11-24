#!/bin/bash
echo "=== 1. Logged in users ==="
who 2>/dev/null | head -20
echo
echo "=== 2. Load average ==="
uptime
echo
echo "=== 3. High-CPU processes (excluding ours/kthreads) ==="
ps aux --sort=-%cpu 2>/dev/null | head -15
echo
echo "=== 4. Processes using URMA devices ==="
fuser /dev/uburma 2>&1 | head -5
ls /proc/*/fd 2>/dev/null | while read p; do
  :
done
for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
  if ls -la /proc/$pid/fd 2>/dev/null | grep -q "uburma\|infiniband"; then
    echo "PID $pid: $(cat /proc/$pid/comm 2>/dev/null)"
  fi
done
echo
echo "=== 5. Processes using ibverbs/RDMA ==="
lsof /dev/infiniband/* 2>/dev/null | head -10
echo
echo "=== 6. Our test port conflicts (18716/18715) ==="
ss -tlnp 2>/dev/null | grep -E "18716|18715" || echo "no conflicts"
echo
echo "=== 7. Recent dmesg (driver state) ==="
sudo dmesg 2>/dev/null | tail -5
