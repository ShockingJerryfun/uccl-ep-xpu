#!/bin/bash
echo "=== ConnectX-6 PCIe link (sysfs) ==="
for f in max_link_speed current_link_speed max_link_width current_link_width; do
  echo "$f: $(cat /sys/bus/pci/devices/0000:28:00.0/$f 2>/dev/null)"
done
echo "=== ubcore udmac0d1e2 ==="
ls /sys/class/ubcore/udmac0d1e2/ 2>/dev/null | head -20
echo "=== ubcore top-level ==="
ls /sys/class/ubcore/ 2>/dev/null
for f in /sys/class/ubcore/udmac0d1e2/*; do
  if [ -f "$f" ] && [ $(stat -c%s "$f") -lt 4096 ]; then
    v=$(cat "$f" 2>/dev/null | head -1)
    [ -n "$v" ] && echo "$(basename $f) = $v"
  fi
done