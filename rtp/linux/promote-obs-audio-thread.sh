#!/bin/sh
# Give OBS's PipeWire data thread realtime priority (SCHED_RR 20).
# Without it, on a loaded machine the audio capture timeline jumps by 20-60 ms.
# Needs sudo (chrt on another process's thread). Run after OBS has started.
set -eu
pid=$(pgrep -x obs | head -1)
[ -n "$pid" ] || { echo "OBS is not running" >&2; exit 1; }
for i in $(seq 1 20); do
    tids=$(ps -Lo tid=,comm= -p "$pid" | awk '/pw-data-loop/{print $1}')
    [ -n "$tids" ] && break
    sleep 1
done
[ -n "$tids" ] || { echo "no pw-data-loop thread in OBS (is the PipeWire audio plugin loaded?)" >&2; exit 1; }
for t in $tids; do sudo chrt -r -p 20 "$t"; done
ps -Lo tid,cls,rtprio,comm -p "$pid" | awk 'NR==1 || /pw-data-loop/'
