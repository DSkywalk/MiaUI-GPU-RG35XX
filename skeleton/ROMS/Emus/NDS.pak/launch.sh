#!/bin/sh


if [ ! -d /userdata/Profile/configs/drastic ];then
	mkdir -p /userdata/Profile/configs/drastic
	cp -r /usr/share/drastic/* /userdata/Profile/configs/drastic/
fi

cp /usr/share/evmapy/drastic.keys /var/run/evmapy/event1.json
evmapy &

overclock.elf 1104000

cd /userdata/Profile/configs/drastic

ROM="$1"

HOME="$USERDATA_PATH"
echo $HOME

drastic "$ROM"

killall evmapy
