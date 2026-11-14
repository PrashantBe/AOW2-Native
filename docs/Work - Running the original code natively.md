# Work - Running the original code natively

**Status:** roadmap items 01, 02, 03 and 05 complete. The original game runs on the PC and boots on ARM64 under emulation, with music. Not yet tried on a real RG40XXV.

## What changed

The project stopped re-implementing the game's rules by hand. `aow2` now loads the 13 original classes from the player's JAR and runs them on a small built-in Java runtime, with the phone features they call implemented in C++ on SDL2.

| File | Role |
| --- | --- |
| [jar.cpp](../runtime/jar.cpp) | Reads the JAR (ZIP) and its manifest with zlib. |
| [vm.cpp](../runtime/vm.cpp) | Class loader, bytecode interpreter, exceptions, monitors, threads, mark-and-sweep garbage collector. Covers the CLDC 1.0 instruction set the game uses (no floating point). One Java thread runs at a time behind a fair lock. |
| [natives_core.cpp](../runtime/natives_core.cpp) | `java.lang`, `java.io`, `java.util`: strings, buffers, streams, `Vector`, `Hashtable`, `Random`, threads, the game clock. |
| [natives_midp.cpp](../runtime/natives_midp.cpp) | MIDP 2.0: canvas, images (PNG), drawing with clipping and sprite transforms, display switching, saved records, sound players, MIDlet properties. Network calls fail as if offline. |
| [midi.cpp](../runtime/midi.cpp) | Built-in General MIDI synthesizer for the music track. |
| [main.cpp](../runtime/main.cpp) | Window, controller, mouse and keyboard input, audio device, and the headless test modes. |

Saved games are kept per record store in `saves/*.rms`, written to a temporary file and then renamed so a crash never leaves a half-written save.

## How the game sees the device

- Screen: 640×360, the Nokia 5800's landscape mode. On the handheld's 640×480 display it is centered with black bars.
- Touch: the pointer is the finger. Touching near a screen edge scrolls the map, as on the phone.
- Keys: the game treats −6 or `*` as ✓ and −7 or `#` as ✗. The handheld sends these from X/Start and B; Y is Fire; the D-pad sends arrow keys.
- Sound: `Manager.createPlayer` accepts MIDI. The game asks "Sound on?" at start and only plays music after ✓.

## Verification

All run in Ubuntu 24.04 under WSL.

- `ctest --test-dir build`: 8 of 8 pass, including `runtime-boot`, which boots the original game headless to the main menu and requires at least 80 painted frames.
- Live session driven by `--commands` at 4× game speed: Campaign → Easy → tutorial off → intro → Pucallpa briefing → battle. Selected the Command Center, opened its build panel, placed a Generator by dragging the footprint until all tiles were green, watched construction complete (power 4/6 → 4/12), saved from the Pause menu (12 KB record), quit, restarted, and chose Campaign → Continue. The Generator, power and funds came back.
- Performance: 4 minutes of battle used 6.8 s of CPU on the PC, peak memory 30 MB.
- Music: `--music-wav` renders 30 s at about −18 dBFS with no clipping. In the windowed game, with SDL's file audio driver, the output is silent until "Sound on?" is accepted and then plays continuously.
- ARM64: `tools/arm64/build.sh` cross-compiles in Debian 12. The stripped binary is 1.6 MB and needs only `libSDL2-2.0.so.0` and glibc. `tools/arm64/test.sh` boots it to the main menu under QEMU 10 in 20 s of game time (199 frames).

## Problems found and fixed

- Debian 11 packages have moved to the archive, so the build image uses Debian 12.
- QEMU 7.2 (Debian 12) hangs on any thread creation, including a five-line test program. The emulated test uses QEMU 10 from Debian 13.

## Known gaps

- Not yet run on the RG40XXV. Pointer speed, edge scrolling and audio latency need tuning there.
- Text drawn with the phone's system font (`Graphics.drawString`) is skipped. The game's own bitmap fonts cover the menus, dialogue and HUD seen so far.
- Only the start of Mission 1 has been played through. Later missions, Custom Game and the Rank screen are untested.
- The synthesizer is simple. Instruments are approximated by waveform family.
