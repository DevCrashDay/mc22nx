#!/usr/bin/env bash
# Runs an NRO in Ryujinx until the game logs "[ee] stopped" (set --frames in sdcard/mc2/args.txt) or a crash, then closes Ryujinx.
# Frame dumps land in sdcard/mc2/frames; tools/ryujinx_compare.py <out.png> compares them with baseline/pc-gl.
# usage: ryu.sh <nro> [timeout_s]  -- runs the NRO in Ryujinx until the game logs its end or timeout, then closes Ryujinx
SD="$APPDATA/Ryujinx/sdcard/mc2"; NRO="$1"; T="${2:-900}"
rm -rf "$SD/frames" "$SD/switch_log.txt" "$SD/crash.txt"; mkdir -p "$SD/frames"
$HOME/Downloads/ryujinx-1.3.3-win_x64/publish/Ryujinx.exe "$(cygpath -w "$NRO")" > /dev/null 2>&1 &
pid=$!; start=$(date +%s)
while :; do
  sleep 5
  if grep -qE "^\[ee\] stopped|^\[crash\]" "$SD/switch_log.txt" 2>/dev/null || [ -f "$SD/crash.txt" ]; then sleep 3; break; fi
  [ $(( $(date +%s) - start )) -ge "$T" ] && { echo TIMEOUT; break; }
  kill -0 $pid 2>/dev/null || { echo "RYUJINX EXITED"; break; }
done
taskkill //F //IM Ryujinx.exe > /dev/null 2>&1
echo "elapsed $(( $(date +%s) - start ))s"
