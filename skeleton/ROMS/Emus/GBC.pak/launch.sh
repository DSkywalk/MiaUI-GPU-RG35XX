#!/bin/sh

EMU_EXE=gambatte
#EMU_EXE=gearboy

# para que funcione el overlay este "export" es muy importante
export PAK_PATH=$(dirname "$0")

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG/states"
mkdir -p "$SAVES_PATH/$EMU_TAG/saves"

HOME="$USERDATA_PATH"
cd "$HOME"
minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM"

