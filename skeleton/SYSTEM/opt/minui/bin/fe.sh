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

# >>> launch fallback
# A .pak copied from Windows (SMB, SFTP...) lands with every file 0644.
# minui runs `sh launch.sh`, so the script itself still starts, but whatever
# it executes then fails with 126, the shell's "not executable" (EACCES).
# Only then, and only once: give the pak's scripts and binaries their +x back
# and launch again. A launch that exits normally never gets here.

# the .pak a queued "sh '<pak>/launch.sh' ['rom']" runs, or nothing
pak_of() {
	eval "set -- $1" 2>/dev/null || return 1
	[ "$1" = "sh" ] || return 1
	case "$2" in
		*.pak/launch.sh) dirname "$2" ;;
		*) return 1 ;;
	esac
}

# +x for the pak's launch.sh and every script (#!) or ELF in it that lacks it.
# Succeeds only if something other than launch.sh was fixed: launch.sh runs
# through sh either way, so fixing it alone can't change the outcome.
fix_exec() {
	chmod +x "$1/launch.sh" 2>/dev/null
	elf=$(printf '\177ELF')
	n=$(find "$1" -type f ! -perm -u+x ! -name launch.sh 2>/dev/null | while IFS= read -r f; do
		case "$(head -c 4 "$f" 2>/dev/null)" in
			"$elf"|"#!"*) chmod +x "$f" && echo "$f" ;;
		esac
	done | wc -l)
	[ "$n" -gt 0 ]
}

run_next() {
	eval $1
	status=$?
	[ $status -eq 126 ] || return $status
	pak=$(pak_of "$1") || return $status
	fix_exec "$pak" || return $status
	echo "$(date +'%F %T') exit 126 from $pak: +x restored, launching once more" >> /tmp/minui_launch.log
	eval $1 # the second and last try, whatever it returns
}
# <<< launch fallback

touch "$EXEC_PATH"
while [ -f "$EXEC_PATH" ]; do
	overclock.elf $CPU_SPEED_PERF
	minui.elf &> /tmp/minui.txt
	sync

	if [ -f $NEXT_PATH ]; then
		CMD=`cat $NEXT_PATH`
		run_next "$CMD"
		rm -f $NEXT_PATH
		overclock.elf $CPU_SPEED_PERF
		sync
	fi
done

