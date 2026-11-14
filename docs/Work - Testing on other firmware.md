# Work - Testing on other firmware

PortMaster reviewers expect a port to work on several firmwares (AmberELEC, dArkOS, muOS, ROCKNIX, Knulli) and screen sizes, and reject ports tested on one device only. This note covers what was done on the PC to reduce that risk, and how to test on real firmware.

## Done on the PC

| Risk | What was done | Result |
| --- | --- | --- |
| Older firmware has an older C library, so a new binary will not start | Build moved to Ubuntu 20.04 (glibc 2.31), the oldest base PortMaster recommends | Binary needs glibc 2.29. Runs on every current firmware, including ArkOS (2.30). |
| Older SDL2 versions on the device | Boot test now runs in an Ubuntu 20.04 ARM64 userland with SDL 2.0.10 (`tools/arm64/test.sh`) | Boots to the main menu, 199 frames in 20 s |
| Screens other than 640×480 | Captured the game at 640×480, 1280×720, 720×720 and 480×320 (`--window`, `--window-shot`) | Letterboxed with the shape kept. Whole-number scales stay sharp; others are smoothed so text stays readable. |
| Devices without an analog stick | `--dpad-pointer`: the D-pad moves the pointer. The launcher switches it on when PortMaster reports `ANALOG_STICKS=0` | Pointer usable with D-pad + A |
| Missing libraries | Only `libSDL2-2.0.so.0` and the C library are needed; libpng, zlib and the C++ runtime are built in | Nothing to bundle |
| Windows line endings in the launcher | `.gitattributes` keeps `.sh` files LF | `bash -n` passes |
| Rejection by PortMaster's automatic check | Ran their `build_release.py --do-check` (`tools/portmaster-check.sh`) | Passes, no warnings |

## Testing on real firmware

### With the RG40XXV (H700) you have

The RG40XXV runs three of the firmwares PortMaster lists. Use a spare microSD card for each, so the Knulli card stays untouched.

| Firmware | Supports RG40XXV | PortMaster |
| --- | --- | --- |
| Knulli | Yes (tested) | Built in |
| muOS | Yes | Built in |
| ROCKNIX | Yes (H700 builds) | Built in |
| AmberELEC, dArkOS | No (Rockchip devices only) | Ask testers, see below |

Check each project's device list before flashing, as support changes between releases.

For each firmware:

1. Flash the firmware's RG40XXV image to a spare card (balenaEtcher or Raspberry Pi Imager), boot it once, and let it finish setup.
2. Copy the contents of `dist/` into the ports folder (`roms/ports/` on Knulli and muOS, `roms/ports/` or `/storage/roms/ports/` on ROCKNIX) over the network share or by putting the card in the PC.
3. Copy `ArtOfWar2__Nokia_5800.jar` into `ports/artofwar2/gamedata/`.
4. Refresh the game list and launch **Art of War 2 (Native)** from Ports.
5. Work through the checklist below, then copy `ports/artofwar2/log.txt` back to the PC.

### Checklist for each firmware

- [ ] Starts to the "Sound on?" screen within a few seconds; music plays after ✓
- [ ] Picture fills the screen with black bars only, correct shape, no stretching
- [ ] Stick moves the pointer; A touches; holding A drags the building footprint
- [ ] D-pad scrolls; X/Start opens the pause menu; B goes back
- [ ] L1, R1 and Select do rally point, destroy and info in a building panel
- [ ] Save from the pause menu, quit with Select + Start, relaunch, Campaign → Continue restores the game
- [ ] Ten minutes of play without slowdown or crash
- [ ] `log.txt` has no errors

Record the firmware name, version, device and result in the table below.

### Other devices and resolutions

Reviewers also want a higher resolution such as 1280×720, and Rockchip devices for AmberELEC and dArkOS. The usual way is the PortMaster Discord (https://discord.gg/eqjK6yNQS4):

1. Build the release zip with `tools/portmaster-check.sh`, which runs PortMaster's own check and writes `dist/artofwar2.zip`.
2. Post it in the porting or testing channel with the checklist above and a note that players supply their own JAR.
3. Collect results and fix problems before opening the pull request.

## Results

| Firmware | Version | Device | Resolution | Result | Notes |
| --- | --- | --- | --- | --- | --- |
| Knulli | scarab (2026-05-11) | RG40XXV | 640×480 | Plays | Menus, base building, training, music confirmed |
| Ubuntu 20.04 userland (emulated) | glibc 2.31, SDL 2.0.10 | QEMU | headless | Boots to menu | Compatibility check, not a device |
