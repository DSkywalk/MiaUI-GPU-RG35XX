#!/bin/sh


if [ ! -d /userdata/Profile/configs/ppsspp ];then
	mkdir -p /userdata/Profile/configs/ppsspp
	cp -r /usr/share/ppsspp/* /userdata/Profile/configs/ppsspp/
fi

cp /usr/share/evmapy/psp.ppsspp.keys /var/run/evmapy/event1.json
evmapy &

overclock.elf 1488000

cd /userdata/Profile/configs/ppsspp

export SDL_GAMECONTROLLERCONFIG=$(grep "RG35XX" "${HOME}/.config/gamecontrollerdb.txt")
ROM="$1"

HOME="$USERDATA_PATH"
echo $HOME

PPSSPP "$ROM"

killall evmapy
