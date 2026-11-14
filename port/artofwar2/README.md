## Notes

Thanks to [HeroCraft](https://www.herocraft.com) for creating Art of War 2: Global Confederation.

This port runs the original Nokia 5800 game code natively through a small built-in Java runtime written for this game. No Java or emulator is needed. Game files are not included. Copy your own `ArtOfWar2__Nokia_5800.jar` (version 1.4.0) into `ports/artofwar2/gamedata/`.

The game was designed for a touch screen. The left stick moves a pointer that acts as your finger.

Source: https://github.com/PrashantBe/AOW2-Native

## Controls

| Button | Action |
|--|--|
| Left stick | Move the pointer (hold L2 for fine movement) |
| A | Touch at the pointer (hold to drag) |
| D-pad | Scroll the map / move the cursor |
| Y | Fire / select at the cursor |
| X or Start | ✓ button, pause menu |
| B | ✗ button, back |
| L1 | Key 1: set rally point |
| R1 | Key 3: destroy building |
| Select | Key 0: information |
| R2 + L1 / R2 + R1 | Keys 7 / 9 in selection panels |
| Select + Start | Quit |

## Compile

```shell
tools/arm64/build.sh
```
