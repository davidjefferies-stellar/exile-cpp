# exile-cpp

[![tests](https://github.com/davidjefferies-stellar/exile-cpp/actions/workflows/tests.yml/badge.svg)](https://github.com/davidjefferies-stellar/exile-cpp/actions/workflows/tests.yml)

A C++ port of the BBC Micro masterpiece **Exile** (Peter Irvin & Jeremy Smith, 1988),
working from the original 6502 disassembly. The goal of the project is educational -
to re-author the 6502 in C++ for easier study and to preserve its groundbreaking its 
concepts and techniques for a future where people no longer know 6502.

The disassembly used as the spec is the superb effort by Level7 and lives at 
`exile-standard-disassembly.txt`in the project root.

## Build

Visual Studio solution. There's no CMake / headless build path right now.

**Command line (MSBuild):**

```
msbuild exile.vcxproj /p:Configuration=Release /p:Platform=x64
```

**Visual Studio 2022 IDE:**

1. Open `exile-cpp.sln` in VS2022 (the solution targets toolset v143 / C++20).
2. The solution contains two projects:
   - **`exile`** — the game (default startup project).
   - **`exile_tests`** — headless test runner (see [Tests](#tests)).
3. Pick the configuration in the toolbar — `Debug | x64` or `Release | x64`
   (only x64 is configured).
4. **Build → Build Solution** (`Ctrl+Shift+B`) or just **Start Debugging** (`F5`)
   to build and launch the game. The exe lands in the `OutDir` set on
   the project (the checked-in path is `D:\Projects\Exile\`; override in
   your local `exile.vcxproj.user` if you want it elsewhere — that file
   is gitignored).
5. The working directory at launch is the project root, so `exile.ini`,
   `exile.map`, `data/`, and `resources/` resolve as-is.

The build pulls in two single-header dependencies bundled under `deps/`:

- `fenster.h` — windowing + framebuffer
- `fenster_audio.h` — audio (Windows backend is bypassed by our own
  `waveOut` implementation in `src/audio/`; the upstream sets `nBlockAlign`
  incorrectly for PCM 16-bit mono)

## Run

```
exile.exe
```

Configuration lives in `exile.ini` — start position, cache sizes, debug
flags, key bindings, and the landscape-generator A/B toggle.

## Controls

| Key            | Action                                    |
|----------------|-------------------------------------------|
| arrows         | move (with jetpack: thrust)               |
| `Q` `W` `P` `L`| jetpack left / right / up / down (arrow-key aliases) |
| `Z`            | toggle jetpack                            |
| Space          | fire selected weapon                      |
| Tab            | turn around                               |
| Left Ctrl / Left Shift | lie down                          |
| Right Ctrl / `[`       | jetpack booster (held = 2× thrust) |
| `,` `M` `.`    | pick up / drop / throw held object        |
| `S` / `G`      | store in pocket / retrieve from pocket    |
| `R` / `T`      | remember position / teleport to it        |
| `I` `K` `O`    | aim centre / down / up                    |
| `1`–`5`        | select weapon slot                        |
| `Y` / `U`      | whistle one / two                         |
| Esc            | pause                                     |
| `;` / `'`      | save game state / load game state         |
| `\`            | save current landscape to `exile.map`     |
| mouse wheel    | zoom                                      |
| right-drag     | pan camera (map mode)                     |
| left-click     | tile info / toggle bottom-HUD checkbox    |

Map-mode toggle and the rest of the debug overlays (tile grid, object
labels, switch / transporter wires, collision shading, debug text) live
on the bottom-HUD checkboxes — click them to toggle.

## Save / load

Three different files, three different keys.

| Key | File | Notes |
|-----|------|-------|
| `;` | `exile.sav` | Writes a text-format snapshot of mutable game state: frame counter, RNG, player, events, 16 primaries, 32 secondaries, 235 tertiary bytes. Landscape is *not* in this file — it's deterministic from the seed (or comes from `exile.map`). |
| `'` | `exile.sav` | Loads the snapshot back. |
| `\` | `exile.map` | Writes the current 256×256 landscape grid (post-bake, plus any tertiary edits). Shows a "Saved exile.map" / "Save FAILED" banner. |

**When does the map load procedurally vs. from disk?**

`Game::init` always tries `landscape_.load_from_file("exile.map")` first.
If that file exists and parses cleanly, the game uses it. If it's
missing or invalid, the game falls back to `landscape_.bake()` — the
procedural generator that derives the entire grid from the seed in
`exile.ini`. Either way the rest of the game (tertiary spawning,
collision, rendering) doesn't care whether the grid came from disk or
algorithm.

So: delete (or rename) `exile.map` to get the procedural map; press `\`
once to capture the current grid and the game will load that one
forever after.

## Landscape generator: C++ vs pseudo-6502

There are two implementations of the procedural landscape generator,
selected by `exile.ini`'s `[landscape] use_cpp_impl`:

- `use_cpp_impl = false` (default) — `src/world/landscape.cpp`'s
  `bake_tile_pseudo_6502`. Tracks A and the carry flag through an `Alu`
  struct, matching the 6502 disassembly line-for-line. This is the
  reference implementation.
- `use_cpp_impl = true` — `src/world/landscape_cpp.cpp`'s `get_tile_cpp`.
  Native C++ rewrite of the same algorithm using `unsigned` accumulators
  with implicit `uint8_t` wraparound and explicit carry only where the
  algorithm consumes it.

The two paths are intended to produce byte-identical maps; the toggle
exists so the C++ rewrite can be A/B-tested against the reference.
Flipping the flag at runtime via the ini and relaunching is the easiest
way to compare — see `docs/landscape.md` for details.

## Tests

A small headless test suite lives under `tests/`, built by
`exile_tests.vcxproj`. It compiles the game source against a
`NullRenderer` (no window, no audio device assumed) and runs every
`TEST(...)` registered in `tests/*_test.cpp` — boot smoke, walking
gate, RNG, fixed-point arithmetic, weapons firing + impact.

Build and run from VS2022 (set `exile_tests` as the startup project,
press `F5`) or on the command line:

```
msbuild exile_tests.vcxproj /p:Configuration=Release /p:Platform=x64
exile_tests.exe
```

The exe returns the failure count, so it doubles as CI. GitHub Actions
runs the same build on every push to `master` and on PRs — see
`.github/workflows/tests.yml`.

## Source layout

```
src/
  core/         Types, fixed-point, RNG. No game-state dependencies.
  world/        Landscape generation, tertiary resolution, water, wind, tile data.
  objects/      Primary / secondary / tertiary storage, physics, collision.
  behaviours/   Per-type update routines (creature, robot, projectile,
                environment, collectable) + dispatch table + shared NPC helpers.
  particles/    Particle system (&207e update + per-type tables at &0206).
  rendering/    Renderer interface + fenster framebuffer backend, palette
                decode, sprite atlas, debug overlays.
  audio/        SN76489-style envelope synthesizer over fenster_audio.h.
  player/       Input, motion, action, sprite handling.
  game/         Top-level loop orchestration (game.cpp ≤ ~200 lines).
```

## Status

See `docs/porting_progress.md` for a system-by-system survey of what's
faithfully ported vs. partial vs. missing, with 6502 address ranges for
each entry. The disk-load supervisor, copy protection, and demo mode are
intentionally left out; everything else aims to match the disassembly.

`docs/tertiary.md` and `docs/tertiary_objects.md` (a complete
column-table view of all 254 tertiary entries),
`docs/object_spawning.md`, and
`docs/angles_from_velocities.md` document specific subsystems where the
6502's data layout / arithmetic semantics needed careful porting.
`docs/intro_triax.md` traces the non-scripted Triax / destinator opening
beat through the per-frame update loop. `docs/collision.md` covers the
tile and object collision system, including the 22.5°/45° slope
patterns. `docs/rendering.md` walks the tile-byte → atlas sprite →
palette → pixels pipeline, including the procedural palette codes for
stone strata, bushes, leaves and mushrooms.
