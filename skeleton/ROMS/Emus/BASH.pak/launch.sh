#!/bin/sh

EMU_EXE=bash
PORT_PATH=$(dirname "$0")

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
BIN="$1"
mkdir -p "$BIOS_PATH/../Data/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
HOME="$PORT_PATH"
cd "$HOME"
#minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM" DMG &> "$LOGS_PATH/$EMU_TAG.txt"
./$BIN
