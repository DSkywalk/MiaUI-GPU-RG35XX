#!/bin/sh

if [ ! -d /userdata/Profile/configs/mupen64 ];then
	mkdir -p /userdata/Profile/configs/mupen64
	cp -r /usr/share/mupen64/* /userdata/Profile/configs/mupen64/
fi

overclock.elf 1488000

cd /userdata/Profile/configs/mupen64

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
EMU_SAVES="$SAVES_PATH/$EMU_TAG"

mkdir -p "$EMU_SAVES"

HOME="$EMU_SAVES"
echo $HOME
export HOME

# Comprobamos todas las variantes: [RISE], [rise], (RISE), (rise)
case "$ROM" in
  *"[RISE]"* | *"[rise]"* | *"(RISE)"* | *"(rise)"*)
    # más básico fallan sombras
    mupen64plus --corelib /usr/lib/libmupen64plus.so.2.0.0 --gfx /usr/lib/mupen64plus/mupen64plus-video-glide64mk2.so --configdir /userdata/Profile/configs/mupen64/ --datadir /userdata/Profile/configs/mupen64/ "$ROM"
    ;;
  *)    
    mupen64plus --corelib /usr/lib/libmupen64plus.so.2.0.0 --gfx /usr/lib/mupen64plus/mupen64plus-video-rice.so --configdir /userdata/Profile/configs/mupen64/ --datadir /userdata/Profile/configs/mupen64/ "$ROM"
    ;;
esac

