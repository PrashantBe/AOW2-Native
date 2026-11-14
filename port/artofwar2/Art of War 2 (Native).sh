#!/bin/bash
# PortMaster launcher for Art of War 2: Global Confederation.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/artofwar2"
cd "$GAMEDIR" || exit 1
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

JAR="$GAMEDIR/gamedata/ArtOfWar2__Nokia_5800.jar"
if [ ! -f "$JAR" ]; then
  echo "Missing game file. Copy ArtOfWar2__Nokia_5800.jar into ports/artofwar2/gamedata/"
  pm_message "Copy ArtOfWar2__Nokia_5800.jar into ports/artofwar2/gamedata/" 2>/dev/null
  sleep 5
  pm_finish
  exit 1
fi

# Copies made over the network can lose the executable flag.
chmod +x "$GAMEDIR/aow2.${DEVICE_ARCH}" 2>/dev/null

export LD_LIBRARY_PATH="$GAMEDIR/libs.${DEVICE_ARCH}:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# gptokeyb only provides the Select+Start exit hotkey here. The game reads the controller itself.
$GPTOKEYB "aow2.${DEVICE_ARCH}" &
pm_platform_helper "$GAMEDIR/aow2.${DEVICE_ARCH}"
# Devices without an analog stick steer the pointer with the D-pad.
ARGS=""
[ "${ANALOG_STICKS:-1}" = "0" ] && ARGS="--dpad-pointer"
"./aow2.${DEVICE_ARCH}" "$JAR" --fullscreen --saves "$GAMEDIR/saves" $ARGS

pm_finish
