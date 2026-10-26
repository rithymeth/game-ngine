#!/bin/sh
# Samples emulator health every 10 s during the Android emulator CI job. When adb loses the device, it records the
# host-side emulator process state once (threads, wchan, CPU) so a hang/exit without a minidump explains itself.
out="${1:-emulator-diagnostics}"
mkdir -p "$out"
lost=0
while true; do
  echo "=== $(date -u +%T)"
  if timeout 8 adb shell 'echo ok' >/dev/null 2>&1; then
    lost=0
    timeout 8 adb shell 'cat /proc/loadavg; ps -A -o PID,RSS,S,WCHAN,NAME | grep -E "surfaceflinger|system_server|aether|composer|allocator|binder" ; cat /sys/kernel/debug/binder/stats 2>/dev/null | head -5' 2>&1
  else
    echo "adb shell unresponsive; get-state: $(timeout 5 adb get-state 2>&1)"
    if [ "$lost" -eq 0 ]; then
      lost=1
      {
        timeout 5 adb devices -l
        for pid in $(pgrep -f qemu-system-x86_64); do
          echo "--- emulator pid $pid"
          cat "/proc/$pid/status" 2>/dev/null | grep -E 'State|VmRSS|Threads'
          ps -L -o tid=,stat=,pcpu=,wchan:32=,comm= -p "$pid" 2>/dev/null | sort -k3 -rn | head -40
          sudo cat "/proc/$pid/stack" 2>/dev/null
        done
        pgrep -a -f 'qemu-system|emulator' || echo "no emulator process (it exited)"
        timeout 15 adb logcat -d -t 300 2>&1 | tail -n 300
      } > "$out/device-lost-$(date -u +%H%M%S).txt" 2>&1
    fi
  fi
  sleep 10
done
