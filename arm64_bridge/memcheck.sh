#!/bin/bash
# Sample the bridge's memory on a connected device.
#
# The number that matters is the guest arena: the anonymous reservation the size of
# the guest address space. Its growth IS the leak, without the noise of
# dalvik/scudo/GL in the process total.
#
# The stack guard pages (mprotect PROT_NONE) and guest file mappings (MAP_FIXED at the
# guest address) split the reservation into many VMAs, so the arena is found as the run
# of address-contiguous VMAs that starts with an unnamed anonymous VMA and spans exactly
# the 4 GiB reservation; all of them are summed.
#
# Rss alone lies: zram swaps out cold leaked pages, so the arena's Rss can *fall*
# in the middle of a growing leak. Rss+Swap is monotonic; that is what is summed.
#
# No root needed as long as the app is debuggable -- run-as gives full /proc access.
#
# usage: DEV=<serial> PKG=<package> ./memcheck.sh [samples] [interval_sec] [outfile]
D=${DEV:?set DEV to the adb serial}
PKG=${PKG:?set PKG to the application id}
N=${1:-24}
IV=${2:-5}
OUT=${3:-mem_log.csv}

PID=$(adb -s "$D" shell pidof "$PKG" | tr -d '\r')
[ -z "$PID" ] && { echo "not running"; exit 1; }
echo "pid=$PID  interval=${IV}s  samples=$N"
echo "t_s,rss_kb,arena_kb,jit_kb,vmsize_kb,threads" > "$OUT"

for i in $(seq 0 $((N-1))); do
  T=$((i*IV))
  S=$(adb -s "$D" shell "run-as $PKG sh -c 'grep -E \"^VmRSS|^VmSize|^Threads\" /proc/$PID/status; echo ===; cat /proc/$PID/smaps'" 2>/dev/null | tr -d '\r')
  [ -z "$S" ] && { echo "$T,DEAD"; break; }
  echo "$S" | awk -v t="$T" '
    /^VmRSS:/{rss=$2} /^VmSize:/{vsz=$2} /^Threads:/{thr=$2}
    /^[0-9a-f]+-[0-9a-f]+ /{
      n++; split($1,r,"-"); lo[n]=strtonum("0x" r[1]); hi[n]=strtonum("0x" r[2]); named[n]=(NF>=6)
      jit=($2=="rwxp" && (hi[n]-lo[n])/1024>=65536)
      next
    }
    /^Rss:/ { mem[n]+=$2; if(jit) j+=$2 }
    /^Swap:/{ mem[n]+=$2 }
    END{
      # arena: address-contiguous VMAs (guard splits, guest file maps) that start with an
      # unnamed anonymous VMA and span exactly the 4 GiB reservation
      for(i=1;i<=n && a=="";i++){
        if(named[i]) continue
        s=0; m=0
        for(k=i;k<=n;k++){
          if(k>i && lo[k]!=hi[k-1]) break
          s+=(hi[k]-lo[k])/1024; m+=mem[k]
          if(s==4194304){ a=m; break }
          if(s>4194304) break
        }
      }
      printf "%s,%s,%s,%s,%s,%s\n", t, rss, a, j, vsz, thr }'
  sleep "$IV"
done | tee -a "$OUT"

# Reading the result:
#   flat arena          -> no leak
#   rising and settling -> free lists / asset caches warming up; measure a second window
#   rising linearly     -> real leak. Send the app to the background (input keyevent
#                          KEYCODE_HOME) and re-measure: zero means it is render-driven.
#                          Then attribute it by guest LR from inside the malloc thunks.
