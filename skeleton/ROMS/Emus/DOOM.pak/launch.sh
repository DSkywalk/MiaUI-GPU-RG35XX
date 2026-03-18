#!/bin/sh

DATA=$(dirname "$0")
ROMPATH=$(dirname "$1")
ROMNAME=$(basename "$1")

cd "$DATA"

HOME="$SAVES_PATH/DOOM"
# HOME=/userdata/Saves/DOOM

#overclock.elf $CPU_SPEED_PERF
#overclock.elf $CPU_SPEED_GAME
overclock.elf $CPU_SPEED_LOW

BIN_NAME=`echo "$ROMNAME" | awk -F'[][]' '{print $2}'`
if [ ! -z "$BIN_NAME" ]; then
  DOOM_BIN="crispy-$BIN_NAME"
  
  gptokeyb &
else
  DOOM_BIN="crispy-doom"
fi
echo "BIN: '$DOOM_BIN'"


DOOM_PARAM=`echo "$ROMNAME" | awk -F'[()]' '{print $2}'`
if [ ! -z "$DOOM_PATH" ]; then
  ./$DOOM_BIN -iwad "$ROMPATH/${DOOM_PATH}.WAD" -file "$1"
else
  ./$DOOM_BIN -iwad "$1"
fi

sync &
killall -9 gptokeyb

