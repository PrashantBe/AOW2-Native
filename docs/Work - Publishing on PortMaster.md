# Work - Publishing on PortMaster

Sources: [What can be ported](https://portmaster.games/contribute/porting/what-can-be-ported/), [Packaging](https://portmaster.games/contribute/porting/packaging/), [PortMaster-New](https://github.com/PortsMaster/PortMaster-New) README and `AGENTS.md`, read 14 November 2026.

## Does this game qualify?

Yes. PortMaster accepts games whose code "can be made to run on ARM Linux", including the **bytecode VM** route, where the port supplies the VM and the original game runs on it. That is what `aow2` does for this Java game. It needs no box64 or emulator.

The game itself is commercial and is not shipped. The port is "bring your own data" (`"rtr": false` in `port.json`): players copy their own `ArtOfWar2__Nokia_5800.jar` into `ports/artofwar2/gamedata/`. A Ready to Run listing would need HeroCraft's written permission to redistribute the JAR.

## Required files and where they are

PortMaster wants one folder per port under `ports/`. Ours is [port/artofwar2](../port/artofwar2):

| Requirement | Status |
| --- | --- |
| Folder name: lowercase letters, digits, `.` or `_`; unique | `artofwar2` |
| Launch script ending in `.sh`, LF line endings, based on the template, ends with `pm_finish` | `Art of War 2 (Native).sh` |
| Port directory with the same name as the folder | `artofwar2/` |
| Executable named per architecture | `aow2.aarch64`, added by `port/make-package.sh` |
| `port.json` (version 2, name, items, attr: title, desc, inst, genres, porter, rtr, runtime, reqs, arch) | Done |
| `README.md` thanking the creator, with controls | Done |
| `gameinfo.xml` for EmulationStation | Done |
| `screenshot.png`, exactly 640×480, showing gameplay | Done |
| `cover.png` | Optional, not made |
| `licenses/LICENSE.<component>.txt` for every bundled component | Done: aow2 (MIT), libpng, zlib, libstdc++ |
| No bundled SDL2, libc or libstdc++ `.so` files | Only SDL2 and libc are needed from the device; nothing bundled |
| Binary's executable bit set at run time, not on the `.sh` | The launcher runs `chmod +x` on the binary |

## Submission steps

1. Join the PortMaster Discord (https://discord.gg/eqjK6yNQS4), introduce the port and ask the crew about a "bring your own JAR" Java ME title. Reviewers decide what is accepted.
2. Test on as many systems as possible. PortMaster lists AmberELEC, dArkOS, MuOS, ROCKNIX and Knulli, at 640×480 and at a higher resolution such as 1280×720. Ports tested on one device only are rejected. So far: Knulli on RG40XXV (H700), 640×480.
3. Fork `PortsMaster/PortMaster-New` and disable Actions on the fork. Create a feature branch.
4. Run `tools/prepare_repo.sh`, copy `port/artofwar2/` to `ports/artofwar2/` and add `artofwar2/aow2.aarch64` from `tools/arm64/build.sh`.
5. Run `python3 tools/build_release.py --do-check` and fix any warnings. The zip it builds in `releases/` is the test build to share on Discord.
6. Open a pull request. Push fixes to the same PR rather than opening a new one.

## Open items before submitting

- Testing on other firmware and a 1280×720 device.
- A full playthrough of all seven missions and Custom Game.
- The script name contains parentheses because this handheld already has an `Art of War 2.sh` from the FreeJ2ME experiment. For the submission, `Art of War 2.sh` is the cleaner name.
