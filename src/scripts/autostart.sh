#!/bin/bash

while true; do
	CMUS="$(cmus.sh)"
	NET="up: 100b/s | down: 999kb/s"
	VOL=$(pavolume.sh)
	TIME="{$(date +"%H:%M:%S, %A, %Y-%m-%d")} "

	xsetroot -name " $CMUS    $NET    Vol: $VOL     $TIME"
	sleep 1s
done &
