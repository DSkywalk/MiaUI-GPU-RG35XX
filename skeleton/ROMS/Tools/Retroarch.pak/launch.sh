#!/bin/sh

cd $(dirname "$0")
export PAK_PATH=$(dirname "$0")

HOME="$USERDATA_PATH"
cd "$HOME"

retroarch --appendconfig "$PAK_PATH/override.cfg"

