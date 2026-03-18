#!/bin/sh

cd "$(dirname "$0")"

killall mpg123 2> /dev/null
killall vlc 2> /dev/null

# add --vlc to add support for ogg, flac, wav, ...
mplayer.elf

