# Work - Publishing on PortMaster

Sources: PortMaster's [What can be ported](https://portmaster.games/contribute/porting/what-can-be-ported/) and [Packaging](https://portmaster.games/contribute/porting/packaging/) pages, the [PortMaster-New](https://github.com/PortsMaster/PortMaster-New) README and `AGENTS.md`, and the community "Complete PortMaster Porting Reference" (revision 2026-11-11) shared on the PortMaster Discord. Where they differ, the reference is newer and was followed.

## Does this game qualify?

**Technically, yes.** PortMaster accepts bytecode games where the port supplies the virtual machine, which is what `aow2` does for this Java ME game.

**The content rules are the open question.** The crew's rules include:

- The port ships no game data; players copy in their own. This port does that.
- Instructions name exactly one legitimate source and never an unofficial one. The port says "from your own copy of the Nokia 5800 edition".
- **If a game is still sold today in a different form, a port that only works with an old, unobtainable version is not accepted.** A search on 14 November 2026 found the Google Play version removed in 2013, no Steam release, and an iOS App Store listing titled "Art Of War 2: Global Confederation" from GEAR GAMES GLOBAL FZE, marked free. If that listing is live and counts as the same game, the crew may reject the port or send it to the separate "multiverse" repository. Games that are off sale go to the main repository after a waiting period the crew sets. **The crew decides; ask on Discord before opening the pull request.**

## Package status

PortMaster's own `build_release.py --do-check` passes with no errors or warnings (`tools/portmaster-check.sh`).

| Requirement | Status |
| --- | --- |
| Folder `artofwar2`: lowercase, unique, matches the game folder | Done. No existing port uses the name. |
| Launch script: standard header, `BINARY`, `pm_show_error` for missing files (message first), gptokeyb2 for the Select + Start exit, `pm_platform_helper`, `pm_finish`, LF line endings, one-line comments | Done. Passes `shellcheck -S error`. |
| No `SDL_VIDEODRIVER`, `LD_PRELOAD`, `HOME` redirect or CFW-name branches | Done. Saves go to `artofwar2/saves` through the program's own `--saves` option. |
| Fullscreen requested by the program, system cursor hidden, software-render fallback if a GLES driver rejects the texture | Done |
| `port.json` schema version 4: `rtr: false`, `availability: "paid"`, `store: []` (not sold), `min_glibc: "2.29"`, `arch: ["aarch64"]`, plain-text `desc` and `inst` | Done |
| `gameinfo.xml` with `<gameList><game>`, `YYYYMMDDT000000` date, image `./artofwar2/cover.png` | Done |
| `README.md` starting at `## Notes` with a thank-you to HeroCraft, a Controls table and a Compile section | Done |
| `screenshot.png` and `cover.png`: exactly 640×480, under 100 KB, real capture | Done (74 KB and 71 KB) |
| `licenses/`: aow2 (MIT), libpng, zlib, libstdc++ (all linked into the binary) | Done |
| Nothing bundled that the device provides; no SDL2 | Done. Only SDL2 and glibc 2.29+ are needed from the device. |
| Testing thread post for Discord | [testing_thread.txt](testing_thread.txt) |

## Pull request: AI disclosure

PortMaster's pull request template asks the porter to confirm: "I wrote and understand this script/patch myself, or clearly marked which parts came from an AI assistant and reviewed them" and "I can explain every non-standard line in my launch script". Reviewers do ask. This port's runtime, launcher and packaging were written with an AI assistant, so tick the box only with a clear note saying so, and be ready to explain each line of the launch script.

## Submission steps

1. Ask on the PortMaster Discord (https://discord.gg/eqjK6yNQS4) whether a bring-your-own-JAR Java ME title is acceptable, mentioning the iOS listing above.
2. Run `tools/portmaster-check.sh` and post `dist/artofwar2.zip` with [testing_thread.txt](testing_thread.txt) in the testing channel. Collect results for the CFWs and resolutions listed there. See [Work - Testing on other firmware](Work%20-%20Testing%20on%20other%20firmware.md).
3. Fork `PortsMaster/PortMaster-New`, disable Actions on the fork and create a feature branch.
4. Copy `port/artofwar2/` to `ports/artofwar2/` and add `artofwar2/aow2.aarch64` from `tools/arm64/build.sh`.
5. Open the pull request with PortMaster's template, the test results and the AI note. Push fixes to the same pull request rather than opening a new one.

## Open items before submitting

- The crew's decision on whether the game is acceptable and in which repository.
- Testing on other firmware and a 1280×720 device.
- A full playthrough of all seven missions and Custom Game.
- The script name contains "(Native)" because the test handheld also has a FreeJ2ME launcher called `Art of War 2.sh`. Plain `Art of War 2.sh` is the cleaner name for the submission.
