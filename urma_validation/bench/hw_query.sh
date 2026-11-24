#!/bin/bash
echo "=== ConnectX-6 PCIe link ==="
PCI=$(basename "$(readlink /sys/class/infiniband/rocep40s0f0/device)")
echo "rocep40s0f0 at $PCI"
lspci -s "$PCI"
lspci -s "$PCI" -vvv 2>/dev/null | grep -E "LnkCap|LnkSta:" | head -4
echo "=== ubus/udma dmesg ==="
dmesg 2>/dev/null | grep -iE "ubus|udma" | head -10
echo "=== udmac0d1e2 sysfs ==="
find /sys -maxdepth 4 -name "*udmac*" 2>/dev/null | head -5
