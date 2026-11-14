## Notes

Thanks to [HeroCraft](https://www.herocraft.com) for creating Art of War 2: Global Confederation, a real-time strategy game that fit base building, research and a seven-mission campaign onto a 2010 phone.

The original Java ME game runs natively on a small runtime written for this port, so no Java or emulator is needed. Copy `ArtOfWar2__Nokia_5800.jar` from your own copy of the Nokia 5800 edition (version 1.4.0) to `ports/artofwar2/gamedata/`.

The game was made for a touch screen: the left stick moves a pointer that acts as your finger. On devices without a stick, the D-pad moves the pointer.

## Controls

| Button | Action |
|--|--|
| Left stick | Move the pointer |
| A | Touch at the pointer (hold to drag) |
| D-Pad | Scroll the map and move the cursor |
| Y | Fire or select at the cursor |
| X | Confirm, pause menu |
| Start | Confirm, pause menu |
| B | Back |
| L1 | Set rally point (phone key 1) |
| R1 | Destroy building (phone key 3) |
| Select | Information (phone key 0) |
| R2 + L1 | Phone key 7 in selection panels |
| R2 + R1 | Phone key 9 in selection panels |
| L2 | Slow pointer (hold) |

## Compile

Needs Docker on an x86-64 Linux machine or WSL2. The build runs in an Ubuntu 20.04 container with an AArch64 cross-compiler.

```bash
git clone https://github.com/PrashantBe/AOW2-Native.git
cd AOW2-Native
tools/arm64/build.sh
```

The binary is written to `build-arm64/aow2`.
