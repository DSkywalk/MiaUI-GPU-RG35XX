#!/bin/sh

DATA=$(dirname "$0")
ROMPATH=$(dirname "$1")
MAPNAME=$(basename "$1")

HOME="$SAVES_PATH/ECWOLF"

#overclock.elf $CPU_SPEED_PERF
#overclock.elf $CPU_SPEED_GAME
overclock.elf $CPU_SPEED_LOW

PK3MAP=$1

ROMPATH="$ROMPATH/.files/$MAPNAME"
echo "$ROMPATH"
cd "$ROMPATH"

echo "$PK3MAP"

ecwolf --savedir "$SAVES_PATH/ECWOLF" --config "$SAVES_PATH/ECWOLF/ecwolf.cfg" --file "$PK3MAP"


