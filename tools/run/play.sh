#!/bin/bash
# Play with RG_NATIVE_CPU=1 by default.
#   tools/run/play.sh cpu0     the old checkpoint unwinds instead (flag off), to compare
#   tools/run/play.sh          your normal saves
#   tools/run/play.sh late     a fresh copy of the end-game save (mc0-late)
#   (the GS path is gone: build the tag gs-path-final to compare)
#   tools/run/play.sh rdoc     under RenderDoc, so F8 bug marks carry the frame:
#                              F8 with the mouse over the fault, then tools/bugmark/bugmark.py
#   tools/run/play.sh late RG_NATIVE_DOF=0   any VAR=value is passed through
cd "$(dirname "$0")/../.." || exit 1
export RG_SETTINGS_FILE=build/run/native-on.ini
export RG_LOG_FILE=build/run/play.log
export RG_NATIVE_CPU=1 DQ8_GFX_STATS_EVERY=600
for a in "$@"; do
  case $a in
    late) rm -rf build/run/mc0-play; cp -r build/run/mc0-late build/run/mc0-play
          export RG_MC0_DIR=build/run/mc0-play ;;
    cpu0) unset RG_NATIVE_CPU ;;
    rdoc) # RenderDoc's layer built with Wayland support (the Arch package has none, and
          # through X11 the picture flashed): ~/.local/share/renderdoc-wayland.
          export VK_ADD_IMPLICIT_LAYER_PATH=$HOME/.local/share/renderdoc-wayland ENABLE_VULKAN_RENDERDOC_CAPTURE=1 ;;
    *=*)  export "$a" ;;
  esac
done
echo "native-cpu $(git rev-parse --short HEAD); log in build/run/play.log"
build/game/src/roguegalaxy disc/SCUS_974.90 2>&1 | tee build/run/play.log
