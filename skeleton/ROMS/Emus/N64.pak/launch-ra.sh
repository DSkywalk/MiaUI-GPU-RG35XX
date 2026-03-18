#!/bin/sh

EMU_EXE=mupen64plus-next
export PAK_PATH=$(dirname "$0")

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG/states"
mkdir -p "$SAVES_PATH/$EMU_TAG/saves"

echo "$CORES_PATH/${EMU_EXE}_libretro.so"

HOME="$USERDATA_PATH"
cd "$HOME"

retroarch -L "$CORES_PATH/${EMU_EXE}_libretro.so" -s "$SAVES_PATH/$EMU_TAG" -S "$SAVES_PATH/$EMU_TAG" --appendconfig "$PAK_PATH/override.cfg" "$ROM"

