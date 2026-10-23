# Skee HeadPart Lookup Fix

An SKSE plugin that removes a slow head-part lookup in RaceMenu (`skee64.dll`). In load orders with
many head parts, this lookup causes stutter whenever the face of a sculpted character is rebuilt:
after loading a save, when the character's 3D is reloaded, and when mods ask for a face or body
update.

## The problem

When RaceMenu applies **sculpt** data (the face sculpt brush) to a character, it looks up each face
mesh's head part by name. The lookup walks **every head part in the game**, and on each step it
creates and releases an engine string (`BSFixedString`). Each create/release locks the engine's
global string pool.

- Cost per face rebuild ≈ (number of sculpted face meshes) × (number of head parts in the load order).
- In the author's load order (~36,800 head parts, a sculpted UBE character with 15 face meshes), one
  rebuild meant roughly 440,000 string-pool create/release pairs. The player's face is rebuilt
  several times right after a save loads, which showed up as a cluster of 80–120 ms hitches in the
  first seconds.
- With a typical head part count (a few thousand) the cost is smaller, but it is still paid on every
  rebuild.

## What it does

At startup it builds a case-insensitive `name → head part` table once. The lookup is then answered
from the table, with the same result as the original: the first match in load order. RaceMenu's own
code still applies the sculpt, and nothing in your save or presets changes.

When the table cannot give an exact answer, the plugin falls back to the original full scan:
- the table is not built yet,
- the head part list has changed,
- the name contains non-ASCII characters.

In the author's game the log showed, for one save load: 75 lookups from the table, 0 fallbacks, and
about 2.2 million string-pool create/release pairs avoided.

## Supported RaceMenu builds

| RaceMenu (`skee64.dll`) | Game | Patched |
|---|---|---|
| 0.4.20.0 (Steam) | AE 1.6.1170 | sculpt lookup + `GetHeadPartByName` |
| 0.4.20.0 (GOG) | AE (GOG) | sculpt lookup + `GetHeadPartByName` |
| 0.4.19.16 | AE 1.6.1170 | sculpt lookup + `GetHeadPartByName` |
| 0.4.16 | SE 1.5.97 | `GetHeadPartByName` (the sculpt code calls it on this build, so all 5 callers get faster) |

**Safety check.** The plugin recognises the exact `skee64.dll` build from its PE header. It then
checks every instruction it relies on: 18 code locations on AE, 9 on SE. If anything differs, it
**does not patch** and writes the reason to its log. That covers an unknown build, an edited or
recompiled `skee64.dll`, or another plugin having changed the same code. Without a patch, RaceMenu
just runs as it always did.

**Not supported:**
- RaceMenu VR
- Edited or recompiled `skee64.dll` builds
- Any RaceMenu build not in the table, including future builds for newer game versions

## Requirements

- SKSE for your game version
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- [RaceMenu](https://www.nexusmods.com/skyrimspecialedition/mods/19080), one of the builds above

## Installation

Install with a mod manager, or copy `SKSE/Plugins/SkeeHeadPartLookupFix.dll` into `Data`.
Load order does not matter. It can be added or removed at any time; it changes nothing in saves.

## Compatibility

- The patches were tested in game on RaceMenu 0.4.20.0 (Steam, AE 1.6.1170) with version 2.0.0, alongside
  *RaceMenu OverlayFix and Various Mod Fixes* and *Racemenu(SKEE) cosave Load Crash Fix*. 2.1.0 changes
  no patch code; it removes the diagnostic options and shortens the log.
- GOG 0.4.20.0, 0.4.19.16 and SE 0.4.16 support was verified against the original DLLs (build
  identification, every checked instruction, and no branches into the patched bytes). **It has not
  yet been tested in game.** Reports with the log file are welcome.
- If another plugin patches the same code first, this plugin steps aside: the byte check fails and
  it logs which address differed.

## Log

`Documents\My Games\Skyrim Special Edition\SKSE\SkeeHeadPartLookupFix.log` (the folder can differ
depending on your INI location).

```
RaceMenu 0.4.20.0 (AE, Steam): patched skee64+BDDF4 (sculpt head part lookup) -> ...
RaceMenu 0.4.20.0 (AE, Steam): patched skee64+582C0 (GetHeadPartByName) -> ...
Lookup cache built: 36775 head parts, 36775 named, 33086 unique names (...)
[this load, first 30 s] head part lookups: 75 from the cache, 0 not found, 0 used the original scan; ~2179590 string-pool create/release pairs avoided
```

The figures are for one save load (they restart when a save starts loading). A line saying
`not patching` gives the reason. Please include the whole log when reporting a problem.

## Limitations

- It only helps when a character with RaceMenu sculpt data has its face rebuilt. It does not raise
  average FPS; it removes the hitches those rebuilds cause.
- On AE builds, three more copies of the same lookup are inlined into RaceMenu code paths that did
  not show up in profiling (they look like preset handling). Those keep the original scan.

## Credits

- RaceMenu by **Expired** (expired6978). This plugin only speeds up one lookup; all features are
  RaceMenu's.
- Built with CommonLibSSE-NG.

## License

MIT, see [LICENSE](LICENSE). Source: https://github.com/servaltullius/SkeeHeadPartLookupFix

## Building from source

Requirements: Visual Studio 2022 and vcpkg with `commonlibsse-ng`. Run `build.cmd`. The output is
`G:\skyrim-build\SkeeHeadPartLookupFix\package` (mod layout) and
`SkeeHeadPartLookupFix-<version>.zip`. Paths are set in `CMakeLists.txt` / `CMakePresets.json`.
