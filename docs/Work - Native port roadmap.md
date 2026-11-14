# Work - Native port roadmap

## Project goal

Run the Nokia 5800 edition of Art of War 2: Global Confederation natively on the Anbernic RG40XXV under KNULLI, with all seven campaign missions, the offline modes and full handheld controls, and publish it through PortMaster.

The release goal is a PortMaster listing that appears in KNULLI's **Ports** menu. A **Ready to Run** listing that bundles the game data needs the rights holder's permission. Without it, the package is published as a port that reads the player's own `ArtOfWar2__Nokia_5800.jar`.

## Approach (changed 14 November 2026)

The port now runs the original game code instead of re-implementing it. `aow2` contains a small Java runtime written for this one game (CLDC 1.0: integers and longs, no floating point or reflection) and implements the MIDP phone features the game calls in C++ on SDL2. Rules, AI, mission scripts, menus and saves come from the player's JAR unchanged.

Decompiling showed the whole game is about 15,000 lines in 13 classes, calling roughly 200 platform methods. Re-deriving every rule by hand, as items 03–16 of the earlier roadmap planned, would have taken far longer and stayed approximate. The earlier prototype is kept only in local copies, not in the repository. See [Work - Running the original code natively](Work%20-%20Running%20the%20original%20code%20natively.md).

## How to use this roadmap

Work through one numbered item at a time. Add or update its own `docs/Work - {task name}.md` with the observations, implementation, verification commands and results, and any blockers. Update the status here and link the note from the [docs index](README.md). If an item is blocked, record what is needed and keep it open.

**Current status:** items 01–05 complete. Item 08 is partly done. Next: item 06, and items 09–10 for publishing.

## Work items

| # | Work item | Completion check |
| --- | --- | --- |
| 01 | Java runtime and phone API — Complete | Original game boots, reaches menus and the campaign, and runs a battle. All 8 checks pass. |
| 02 | Gameplay check on PC — Complete for the Mission 1 opening | Selection, build menu, footprint placement, construction, power, save, and load after a restart all work. |
| 03 | ARM64 build — Complete | Reproducible Docker cross-build. The binary needs only SDL2 and glibc 2.36 and boots to the main menu under ARM emulation. |
| 04 | Test on the RG40XXV — Complete | Runs from Ports on KNULLI scarab (glibc 2.40): menus, base building, unit training panel and music confirmed on the device. |
| 05 | Music — Complete | The MIDI track plays through a built-in synthesizer after "Sound on?" is accepted, and follows the game's volume. |
| 06 | Full playthrough | Play all seven missions and the offline Custom Game mode. Log runtime errors or wrong behavior. |
| 07 | System-font text | The game falls back to the phone's font in a few places (`drawString`). Render it. |
| 08 | Controls polish — Number keys done | L1, R1 and Select send the phone keys 1 (rally point), 3 (destroy) and 0 (info); R2 + L1/R1 send 7 and 9. Still to tune: pointer speed and edge scrolling on the device. |
| 09 | Game-data redistribution decision | Contact the rights holder. Without permission, the release stays "bring your own JAR". |
| 10 | PortMaster package and submission | Port folder is ready in `port/artofwar2`. Still needed: testing on other firmware, then the pull request. See [Work - Publishing on PortMaster](Work%20-%20Publishing%20on%20PortMaster.md). |

## PortMaster references

- [Ready to Run and game-data requirements](https://portmaster.games/faq/)
- [Port packaging guide](https://portmaster.games/contribute/porting/packaging/)
- [Submission repository](https://github.com/PortsMaster/PortMaster-New)
