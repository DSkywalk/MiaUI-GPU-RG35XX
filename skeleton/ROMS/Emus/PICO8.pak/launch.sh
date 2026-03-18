#!/bin/sh

EMU_EXE=pico8
CORES_PATH=$(dirname "$0")

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$SAVES_PATH/$EMU_TAG"

# usando la config personalizada para pico8
export SDL_GAMECONTROLLERCONFIG=$(grep "RG35XX" "$SAVES_PATH/$EMU_TAG/sdl_controllers.txt")

# necesitamos el fichero .asoundrc para engañar a pico8 y suene bien a 44kHz
HOME="$CORES_PATH"

if [ ! -f "$SAVES_PATH/$EMU_TAG/config.txt.bck" ]; then
  cp -v $SAVES_PATH/$EMU_TAG/base_config.txt /tmp/pico8_config.txt
fi


export GAMEDIR="$CORES_PATH"
cd "$GAMEDIR"

overclock.elf low

case "$ROM" in
  *"[2x]"* | *"[2X]"*)
    EXTRAS="-displays_x 2 -displays_y 2"
    ;;
  *)
    EXTRAS=""
    ;;
esac

cp /usr/share/evmapy/pico8.keys /var/run/evmapy/event1.json
evmapy &

./pico8_dyn -run "$ROM" -home "$SAVES_PATH/$EMU_TAG" -desktop "$SAVES_PATH/$EMU_TAG" $EXTRAS

killall evmapy

