#!/bin/sh

EMU_EXE=tic80

# para que funcione el overlay este "export" es muy importante
export PAK_PATH=$(dirname "$0")

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$USERDATA_PATH"
cd "$HOME"

export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$CORES_PATH/


minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM"
#"$CORES_PATH/tic80" "$ROM"
