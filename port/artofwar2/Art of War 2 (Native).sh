#!/bin/bash

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

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR=/$directory/ports/artofwar2
BINARY=aow2.${DEVICE_ARCH}

cd $GAMEDIR

> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

if [ ! -f "$GAMEDIR/gamedata/ArtOfWar2__Nokia_5800.jar" ]; then
  pm_show_error "Copy ArtOfWar2__Nokia_5800.jar from your own copy of the Nokia 5800 edition (version 1.4.0) to ports/artofwar2/gamedata/ and launch the game again." --title "Game file missing" --type warning
  exit 1
fi

# Copies made over a network share can lose the executable bit.
chmod +x "$GAMEDIR/$BINARY"

export LD_LIBRARY_PATH="$GAMEDIR/libs.${DEVICE_ARCH}:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# Devices without an analog stick move the pointer with the D-pad.
POINTER=""
[ "${ANALOG_STICKS:-1}" = "0" ] && POINTER="--dpad-pointer"

$GPTOKEYB2 "$BINARY" > /dev/null 2>&1 &

pm_platform_helper "$GAMEDIR/$BINARY"

./$BINARY gamedata/ArtOfWar2__Nokia_5800.jar --fullscreen --saves "$GAMEDIR/saves" $POINTER

pm_finish
