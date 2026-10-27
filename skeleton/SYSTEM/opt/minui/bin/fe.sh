#!/bin/sh


source /etc/minui.env

echo userspace > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

#######################################

# export SDL_VSYNC=1

#######################################

cd $HOME

#######################################

EXEC_PATH=/tmp/minui_exec    
NEXT_PATH="/tmp/next"   

touch "$EXEC_PATH"
while [ -f "$EXEC_PATH" ]; do
	overclock.elf $CPU_SPEED_PERF
	minui.elf &> /tmp/minui.txt
	sync
	
	if [ -f $NEXT_PATH ]; then
		CMD=`cat $NEXT_PATH`
		eval $CMD
		rm -f $NEXT_PATH
		overclock.elf $CPU_SPEED_PERF
		sync
	fi
done

