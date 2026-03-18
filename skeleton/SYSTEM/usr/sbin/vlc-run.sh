#!/bin/sh

cd "$(dirname "$0")"
VIDEO="$1"

cp -v /usr/share/evmapy/vlc.keys /var/run/evmapy/event1.json
evmapy &

sleep 1

#cvlc --vout drm "$VIDEO" < /dev/tty0
#cvlc --vout gles2 --window wl "$VIDEO" < /dev/tty0
#cvlc --vout gles2 --window fbdev "$VIDEO"

ifconfig lo 127.0.0.1 up

vlc -I oldrc --rc-host 127.0.0.1:8080 --vout fb --fbdev /dev/fb0 --width 640 --height 480 --swscale-mode 0 --video-filter=canvas --canvas-width=640 --canvas-height=480 "$VIDEO" < /dev/tty0


# LIMPIEZA TOTAL AL SALIR
killall evmapy

